/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include <linux/virtio_scsi.h>

#include "spdk/mmio.h"
#include "spdk/string.h"

#include "virtio_pci.h"

struct virtio_driver g_virtio_driver = {
	.init_ctrlrs = TAILQ_HEAD_INITIALIZER(g_virtio_driver.init_ctrlrs),
	.attached_ctrlrs = TAILQ_HEAD_INITIALIZER(g_virtio_driver.attached_ctrlrs),
};

/*
 * Following macros are derived from linux/pci_regs.h, however,
 * we can't simply include that header here, as there is no such
 * file for non-Linux platform.
 */
#define PCI_CAPABILITY_LIST	0x34
#define PCI_CAP_ID_VNDR		0x09
#define PCI_CAP_ID_MSIX		0x11

#define virtio_dev_get_hw(hw) \
	((struct virtio_hw *)((uintptr_t)(hw) - offsetof(struct virtio_hw, vdev)))

static inline int
check_vq_phys_addr_ok(struct virtqueue *vq)
{
	/* Virtio PCI device VIRTIO_PCI_QUEUE_PF register is 32bit,
	 * and only accepts 32 bit page frame number.
	 * Check if the allocated physical memory exceeds 16TB.
	 */
	if ((vq->vq_ring_mem + vq->vq_ring_size - 1) >>
	    (VIRTIO_PCI_QUEUE_ADDR_SHIFT + 32)) {
		SPDK_ERRLOG("vring address shouldn't be above 16TB!\n");
		return 0;
	}

	return 1;
}

static void
free_virtio_hw(struct virtio_dev *dev)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
	unsigned i;

	for (i = 0; i < 6; ++i) {
		if (hw->pci_bar[i].vaddr == NULL) {
			continue;
		}

		spdk_pci_device_unmap_bar(hw->pci_dev, i, hw->pci_bar[i].vaddr);
	}

	free(dev->name);
	free(hw);
}

static void
pci_dump_json_config(struct virtio_dev *dev, struct spdk_json_write_ctx *w)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr((struct spdk_pci_device *)hw->pci_dev);
	char addr[32];

	spdk_json_write_name(w, "type");
	if (dev->modern) {
		spdk_json_write_string(w, "pci-modern");
	} else {
		spdk_json_write_string(w, "pci-legacy");
	}

	spdk_json_write_name(w, "pci_address");
	spdk_pci_addr_fmt(addr, sizeof(addr), &pci_addr);
	spdk_json_write_string(w, addr);
}

static inline void
io_write64_twopart(uint64_t val, uint32_t *lo, uint32_t *hi)
{
	spdk_mmio_write_4(lo, val & ((1ULL << 32) - 1));
	spdk_mmio_write_4(hi, val >> 32);
}

static void
modern_read_dev_config(struct virtio_dev *dev, size_t offset,
		       void *dst, int length)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
	int i;
	uint8_t *p;
	uint8_t old_gen, new_gen;

	do {
		old_gen = spdk_mmio_read_1(&hw->common_cfg->config_generation);

		p = dst;
		for (i = 0;  i < length; i++)
			*p++ = spdk_mmio_read_1((uint8_t *)hw->dev_cfg + offset + i);

		new_gen = spdk_mmio_read_1(&hw->common_cfg->config_generation);
	} while (old_gen != new_gen);
}

static void
modern_write_dev_config(struct virtio_dev *dev, size_t offset,
			const void *src, int length)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
	int i;
	const uint8_t *p = src;

	for (i = 0;  i < length; i++)
		spdk_mmio_write_1(((uint8_t *)hw->dev_cfg) + offset + i, *p++);
}

static uint64_t
modern_get_features(struct virtio_dev *dev)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
	uint32_t features_lo, features_hi;

	spdk_mmio_write_4(&hw->common_cfg->device_feature_select, 0);
	features_lo = spdk_mmio_read_4(&hw->common_cfg->device_feature);

	spdk_mmio_write_4(&hw->common_cfg->device_feature_select, 1);
	features_hi = spdk_mmio_read_4(&hw->common_cfg->device_feature);

	return ((uint64_t)features_hi << 32) | features_lo;
}

static int
modern_set_features(struct virtio_dev *dev, uint64_t features)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	if ((features & (1ULL << VIRTIO_F_VERSION_1)) == 0) {
		SPDK_ERRLOG("VIRTIO_F_VERSION_1 feature is not enabled.\n");
		return -1;
	}

	spdk_mmio_write_4(&hw->common_cfg->guest_feature_select, 0);
	spdk_mmio_write_4(&hw->common_cfg->guest_feature, features & ((1ULL << 32) - 1));

	spdk_mmio_write_4(&hw->common_cfg->guest_feature_select, 1);
	spdk_mmio_write_4(&hw->common_cfg->guest_feature, features >> 32);

	dev->negotiated_features = features;

	return 0;
}

static uint8_t
modern_get_status(struct virtio_dev *dev)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	return spdk_mmio_read_1(&hw->common_cfg->device_status);
}

static void
modern_set_status(struct virtio_dev *dev, uint8_t status)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	spdk_mmio_write_1(&hw->common_cfg->device_status, status);
}

static uint16_t
modern_get_queue_num(struct virtio_dev *dev, uint16_t queue_id)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	spdk_mmio_write_2(&hw->common_cfg->queue_select, queue_id);
	return spdk_mmio_read_2(&hw->common_cfg->queue_size);
}

static int
modern_setup_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
	uint64_t desc_addr, avail_addr, used_addr;
	uint16_t notify_off;

	if (!check_vq_phys_addr_ok(vq))
		return -1;

	desc_addr = vq->vq_ring_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = RTE_ALIGN_CEIL(avail_addr + offsetof(struct vring_avail,
				   ring[vq->vq_nentries]),
				   VIRTIO_PCI_VRING_ALIGN);

	spdk_mmio_write_2(&hw->common_cfg->queue_select, vq->vq_queue_index);

	io_write64_twopart(desc_addr, &hw->common_cfg->queue_desc_lo,
			   &hw->common_cfg->queue_desc_hi);
	io_write64_twopart(avail_addr, &hw->common_cfg->queue_avail_lo,
			   &hw->common_cfg->queue_avail_hi);
	io_write64_twopart(used_addr, &hw->common_cfg->queue_used_lo,
			   &hw->common_cfg->queue_used_hi);

	notify_off = spdk_mmio_read_2(&hw->common_cfg->queue_notify_off);
	vq->notify_addr = (void *)((uint8_t *)hw->notify_base +
				   notify_off * hw->notify_off_multiplier);

	spdk_mmio_write_2(&hw->common_cfg->queue_enable, 1);

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "queue %"PRIu16" addresses:\n", vq->vq_queue_index);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "\t desc_addr: %" PRIx64 "\n", desc_addr);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "\t aval_addr: %" PRIx64 "\n", avail_addr);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "\t used_addr: %" PRIx64 "\n", used_addr);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "\t notify addr: %p (notify offset: %"PRIu16")\n",
		      vq->notify_addr, notify_off);

	return 0;
}

static void
modern_del_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	spdk_mmio_write_2(&hw->common_cfg->queue_select, vq->vq_queue_index);

	io_write64_twopart(0, &hw->common_cfg->queue_desc_lo,
			   &hw->common_cfg->queue_desc_hi);
	io_write64_twopart(0, &hw->common_cfg->queue_avail_lo,
			   &hw->common_cfg->queue_avail_hi);
	io_write64_twopart(0, &hw->common_cfg->queue_used_lo,
			   &hw->common_cfg->queue_used_hi);

	spdk_mmio_write_2(&hw->common_cfg->queue_enable, 0);
}

static void
modern_notify_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	spdk_mmio_write_2(vq->notify_addr, vq->vq_queue_index);
}

const struct virtio_pci_ops modern_ops = {
	.read_dev_cfg	= modern_read_dev_config,
	.write_dev_cfg	= modern_write_dev_config,
	.get_status	= modern_get_status,
	.set_status	= modern_set_status,
	.get_features	= modern_get_features,
	.set_features	= modern_set_features,
	.free_vdev	= free_virtio_hw,
	.get_queue_num	= modern_get_queue_num,
	.setup_queue	= modern_setup_queue,
	.del_queue	= modern_del_queue,
	.notify_queue	= modern_notify_queue,
	.dump_json_config = pci_dump_json_config,
};


void
vtpci_read_dev_config(struct virtio_dev *dev, size_t offset,
		      void *dst, int length)
{
	vtpci_ops(dev)->read_dev_cfg(dev, offset, dst, length);
}

void
vtpci_write_dev_config(struct virtio_dev *dev, size_t offset,
		       const void *src, int length)
{
	vtpci_ops(dev)->write_dev_cfg(dev, offset, src, length);
}

void
vtpci_reset(struct virtio_dev *dev)
{
	vtpci_ops(dev)->set_status(dev, VIRTIO_CONFIG_S_RESET);
	/* flush status write */
	vtpci_ops(dev)->get_status(dev);
}

void
vtpci_set_status(struct virtio_dev *dev, uint8_t status)
{
	if (status != VIRTIO_CONFIG_S_RESET)
		status |= vtpci_ops(dev)->get_status(dev);

	vtpci_ops(dev)->set_status(dev, status);
}

uint8_t
vtpci_get_status(struct virtio_dev *dev)
{
	return vtpci_ops(dev)->get_status(dev);
}

static void *
get_cfg_addr(struct virtio_hw *hw, struct virtio_pci_cap *cap)
{
	uint8_t  bar    = cap->bar;
	uint32_t length = cap->length;
	uint32_t offset = cap->offset;

	if (bar > 5) {
		SPDK_ERRLOG("invalid bar: %"PRIu8"\n", bar);
		return NULL;
	}

	if (offset + length < offset) {
		SPDK_ERRLOG("offset(%"PRIu32") + length(%"PRIu32") overflows\n",
			    offset, length);
		return NULL;
	}

	if (offset + length > hw->pci_bar[bar].len) {
		SPDK_ERRLOG("invalid cap: overflows bar space: %"PRIu32" > %"PRIu32"\n",
			    offset + length, hw->pci_bar[bar].len);
		return NULL;
	}

	if (hw->pci_bar[bar].vaddr == NULL) {
		SPDK_ERRLOG("bar %"PRIu8" base addr is NULL\n", bar);
		return NULL;
	}

	return hw->pci_bar[bar].vaddr + offset;
}

static int
virtio_read_caps(struct virtio_hw *hw)
{
	uint8_t pos;
	struct virtio_pci_cap cap;
	int ret;

	ret = spdk_pci_device_cfg_read(hw->pci_dev, &pos, 1, PCI_CAPABILITY_LIST);
	if (ret < 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "failed to read pci capability list\n");
		return -1;
	}

	while (pos) {
		ret = spdk_pci_device_cfg_read(hw->pci_dev, &cap, sizeof(cap), pos);
		if (ret < 0) {
			SPDK_ERRLOG("failed to read pci cap at pos: %"PRIx8"\n", pos);
			break;
		}

		if (cap.cap_vndr == PCI_CAP_ID_MSIX)
			hw->use_msix = 1;

		if (cap.cap_vndr != PCI_CAP_ID_VNDR) {
			SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI,
				      "[%2"PRIx8"] skipping non VNDR cap id: %02"PRIx8"\n",
				      pos, cap.cap_vndr);
			goto next;
		}

		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI,
			      "[%2"PRIx8"] cfg type: %"PRIu8", bar: %"PRIu8", offset: %04"PRIx32", len: %"PRIu32"\n",
			      pos, cap.cfg_type, cap.bar, cap.offset, cap.length);

		switch (cap.cfg_type) {
		case VIRTIO_PCI_CAP_COMMON_CFG:
			hw->common_cfg = get_cfg_addr(hw, &cap);
			break;
		case VIRTIO_PCI_CAP_NOTIFY_CFG:
			spdk_pci_device_cfg_read(hw->pci_dev, &hw->notify_off_multiplier,
						 4, pos + sizeof(cap));
			hw->notify_base = get_cfg_addr(hw, &cap);
			break;
		case VIRTIO_PCI_CAP_DEVICE_CFG:
			hw->dev_cfg = get_cfg_addr(hw, &cap);
			break;
		case VIRTIO_PCI_CAP_ISR_CFG:
			hw->isr = get_cfg_addr(hw, &cap);
			break;
		}

next:
		pos = cap.cap_next;
	}

	if (hw->common_cfg == NULL || hw->notify_base == NULL ||
	    hw->dev_cfg == NULL    || hw->isr == NULL) {
		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "no modern virtio pci device found.\n");
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "found modern virtio pci device.\n");

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "common cfg mapped at: %p\n", hw->common_cfg);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "device cfg mapped at: %p\n", hw->dev_cfg);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "isr cfg mapped at: %p\n", hw->isr);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO_PCI, "notify base: %p, notify off multiplier: %u\n",
		      hw->notify_base, hw->notify_off_multiplier);

	return 0;
}

static int
virtio_dev_pci_init(struct virtio_dev *vdev)
{
	vdev->name = spdk_sprintf_alloc("VirtioScsi%"PRIu32, vdev->id);
	if (!vdev->name) {
		return -1;
	}

	vtpci_read_dev_config(vdev, offsetof(struct virtio_scsi_config, num_queues),
			      &vdev->max_queues, sizeof(vdev->max_queues));
	vdev->max_queues += SPDK_VIRTIO_SCSI_QUEUE_NUM_FIXED;
	TAILQ_INSERT_TAIL(&g_virtio_driver.init_ctrlrs, vdev, tailq);
	return 0;
}

static int
pci_enum_virtio_probe_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct virtio_hw *hw;
	struct virtio_dev *vdev;
	uint8_t *bar_vaddr;
	uint64_t bar_paddr, bar_len;
	int rc;
	unsigned i;

	hw = calloc(1, sizeof(*hw));
	if (hw == NULL) {
		SPDK_ERRLOG("calloc failed\n");
		return -1;
	}

	vdev = &hw->vdev;
	vdev->is_hw = 1;
	hw->pci_dev = pci_dev;

	for (i = 0; i < 6; ++i) {
		rc = spdk_pci_device_map_bar(pci_dev, i, (void *) &bar_vaddr, &bar_paddr,
					     &bar_len);
		if (rc != 0) {
			SPDK_ERRLOG("failed to memmap PCI BAR %u\n", i);
			goto err;
		}

		hw->pci_bar[i].vaddr = bar_vaddr;
		hw->pci_bar[i].len = bar_len;
	}

	/* Virtio PCI caps exist only on modern PCI devices.
	 * Legacy devices are not supported.
	 */
	if (virtio_read_caps(hw) != 0) {
		SPDK_NOTICELOG("Ignoring legacy PCI device.\n");
		goto err;
	}

	rc = vtpci_init(vdev, &modern_ops);
	if (rc != 0) {
		goto err;
	}
	vdev->modern = 1;

	rc = virtio_dev_pci_init(vdev);
	if (rc != 0) {
		vtpci_deinit(vdev->id);
		goto err;
	}

	return 0;

err:
	free_virtio_hw(vdev);
	return -1;
}

int
vtpci_init(struct virtio_dev *vdev, const struct virtio_pci_ops *ops)
{
	unsigned vdev_num;

	for (vdev_num = 0; vdev_num < VIRTIO_MAX_DEVICES; vdev_num++) {
		if (g_virtio_driver.internal[vdev_num].vtpci_ops == NULL) {
			break;
		}
	}

	if (vdev_num == VIRTIO_MAX_DEVICES) {
		SPDK_ERRLOG("Max vhost device limit reached (%u).\n", VIRTIO_MAX_DEVICES);
		return -ENOSPC;
	}

	vdev->id = vdev_num;
	pthread_mutex_init(&vdev->mutex, NULL);
	g_virtio_driver.internal[vdev_num].vtpci_ops = ops;

	return 0;
}

int
vtpci_enumerate_pci(void)
{
	if (!spdk_process_is_primary()) {
		SPDK_WARNLOG("virtio_pci secondary process support is not implemented yet.\n");
		return 0;
	}

	return spdk_pci_virtio_enumerate(pci_enum_virtio_probe_cb, NULL);
}

const struct virtio_pci_ops *
vtpci_ops(struct virtio_dev *dev)
{
	return g_virtio_driver.internal[dev->id].vtpci_ops;
}

void
vtpci_deinit(uint32_t id)
{
	g_virtio_driver.internal[id].vtpci_ops = NULL;
}

void
vtpci_dump_json_config(struct virtio_dev *hw, struct spdk_json_write_ctx *w)
{
	spdk_json_write_name(w, "virtio");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "vq_count");
	spdk_json_write_uint32(w, hw->max_queues);

	spdk_json_write_name(w, "vq_size");
	spdk_json_write_uint32(w, vtpci_ops(hw)->get_queue_num(hw, 0));

	vtpci_ops(hw)->dump_json_config(hw, w);

	spdk_json_write_object_end(w);
}

SPDK_LOG_REGISTER_TRACE_FLAG("virtio_pci", SPDK_TRACE_VIRTIO_PCI)
