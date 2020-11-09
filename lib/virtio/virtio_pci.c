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

#include "spdk/memory.h"
#include "spdk/mmio.h"
#include "spdk/string.h"
#include "spdk/env.h"

#include "spdk_internal/virtio.h"

struct virtio_hw {
	uint8_t	    use_msix;
	uint32_t    notify_off_multiplier;
	uint8_t     *isr;
	uint16_t    *notify_base;

	struct {
		/** Mem-mapped resources from given PCI BAR */
		void        *vaddr;

		/** Length of the address space */
		uint32_t    len;
	} pci_bar[6];

	struct virtio_pci_common_cfg *common_cfg;
	struct spdk_pci_device *pci_dev;

	/** Device-specific PCI config space */
	void *dev_cfg;
};

struct virtio_pci_probe_ctx {
	virtio_pci_create_cb enum_cb;
	void *enum_ctx;
	uint16_t device_id;
};

/*
 * Following macros are derived from linux/pci_regs.h, however,
 * we can't simply include that header here, as there is no such
 * file for non-Linux platform.
 */
#define PCI_CAPABILITY_LIST	0x34
#define PCI_CAP_ID_VNDR		0x09
#define PCI_CAP_ID_MSIX		0x11

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
free_virtio_hw(struct virtio_hw *hw)
{
	unsigned i;

	for (i = 0; i < 6; ++i) {
		if (hw->pci_bar[i].vaddr == NULL) {
			continue;
		}

		spdk_pci_device_unmap_bar(hw->pci_dev, i, hw->pci_bar[i].vaddr);
	}

	free(hw);
}

static void
pci_dump_json_info(struct virtio_dev *dev, struct spdk_json_write_ctx *w)
{
	struct virtio_hw *hw = dev->ctx;
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr((struct spdk_pci_device *)hw->pci_dev);
	char addr[32];

	spdk_json_write_name(w, "type");
	if (dev->modern) {
		spdk_json_write_string(w, "pci-modern");
	} else {
		spdk_json_write_string(w, "pci-legacy");
	}

	spdk_pci_addr_fmt(addr, sizeof(addr), &pci_addr);
	spdk_json_write_named_string(w, "pci_address", addr);
}

static void
pci_write_json_config(struct virtio_dev *dev, struct spdk_json_write_ctx *w)
{
	struct virtio_hw *hw = dev->ctx;
	struct spdk_pci_addr pci_addr = spdk_pci_device_get_addr(hw->pci_dev);
	char addr[32];

	spdk_pci_addr_fmt(addr, sizeof(addr), &pci_addr);

	spdk_json_write_named_string(w, "trtype", "pci");
	spdk_json_write_named_string(w, "traddr", addr);
}

static inline void
io_write64_twopart(uint64_t val, uint32_t *lo, uint32_t *hi)
{
	spdk_mmio_write_4(lo, val & ((1ULL << 32) - 1));
	spdk_mmio_write_4(hi, val >> 32);
}

static int
modern_read_dev_config(struct virtio_dev *dev, size_t offset,
		       void *dst, int length)
{
	struct virtio_hw *hw = dev->ctx;
	int i;
	uint8_t *p;
	uint8_t old_gen, new_gen;

	do {
		old_gen = spdk_mmio_read_1(&hw->common_cfg->config_generation);

		p = dst;
		for (i = 0;  i < length; i++) {
			*p++ = spdk_mmio_read_1((uint8_t *)hw->dev_cfg + offset + i);
		}

		new_gen = spdk_mmio_read_1(&hw->common_cfg->config_generation);
	} while (old_gen != new_gen);

	return 0;
}

static int
modern_write_dev_config(struct virtio_dev *dev, size_t offset,
			const void *src, int length)
{
	struct virtio_hw *hw = dev->ctx;
	int i;
	const uint8_t *p = src;

	for (i = 0;  i < length; i++) {
		spdk_mmio_write_1(((uint8_t *)hw->dev_cfg) + offset + i, *p++);
	}

	return 0;
}

static uint64_t
modern_get_features(struct virtio_dev *dev)
{
	struct virtio_hw *hw = dev->ctx;
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
	struct virtio_hw *hw = dev->ctx;

	if ((features & (1ULL << VIRTIO_F_VERSION_1)) == 0) {
		SPDK_ERRLOG("VIRTIO_F_VERSION_1 feature is not enabled.\n");
		return -EINVAL;
	}

	spdk_mmio_write_4(&hw->common_cfg->guest_feature_select, 0);
	spdk_mmio_write_4(&hw->common_cfg->guest_feature, features & ((1ULL << 32) - 1));

	spdk_mmio_write_4(&hw->common_cfg->guest_feature_select, 1);
	spdk_mmio_write_4(&hw->common_cfg->guest_feature, features >> 32);

	dev->negotiated_features = features;

	return 0;
}

static void
modern_destruct_dev(struct virtio_dev *vdev)
{
	struct virtio_hw *hw = vdev->ctx;
	struct spdk_pci_device *pci_dev;

	if (hw != NULL) {
		pci_dev = hw->pci_dev;
		free_virtio_hw(hw);
		if (pci_dev) {
			spdk_pci_device_detach(pci_dev);
		}
	}
}

static uint8_t
modern_get_status(struct virtio_dev *dev)
{
	struct virtio_hw *hw = dev->ctx;

	return spdk_mmio_read_1(&hw->common_cfg->device_status);
}

static void
modern_set_status(struct virtio_dev *dev, uint8_t status)
{
	struct virtio_hw *hw = dev->ctx;

	spdk_mmio_write_1(&hw->common_cfg->device_status, status);
}

static uint16_t
modern_get_queue_size(struct virtio_dev *dev, uint16_t queue_id)
{
	struct virtio_hw *hw = dev->ctx;

	spdk_mmio_write_2(&hw->common_cfg->queue_select, queue_id);
	return spdk_mmio_read_2(&hw->common_cfg->queue_size);
}

static int
modern_setup_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = dev->ctx;
	uint64_t desc_addr, avail_addr, used_addr;
	uint16_t notify_off;
	void *queue_mem;
	uint64_t queue_mem_phys_addr;

	/* To ensure physical address contiguity we make the queue occupy
	 * only a single hugepage (2MB). As of Virtio 1.0, the queue size
	 * always falls within this limit.
	 */
	if (vq->vq_ring_size > VALUE_2MB) {
		return -ENOMEM;
	}

	queue_mem = spdk_zmalloc(vq->vq_ring_size, VALUE_2MB, NULL,
				 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (queue_mem == NULL) {
		return -ENOMEM;
	}

	queue_mem_phys_addr = spdk_vtophys(queue_mem, NULL);
	if (queue_mem_phys_addr == SPDK_VTOPHYS_ERROR) {
		spdk_free(queue_mem);
		return -EFAULT;
	}

	vq->vq_ring_mem = queue_mem_phys_addr;
	vq->vq_ring_virt_mem = queue_mem;

	if (!check_vq_phys_addr_ok(vq)) {
		spdk_free(queue_mem);
		return -ENOMEM;
	}

	desc_addr = vq->vq_ring_mem;
	avail_addr = desc_addr + vq->vq_nentries * sizeof(struct vring_desc);
	used_addr = (avail_addr + offsetof(struct vring_avail, ring[vq->vq_nentries])
		     + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);

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

	SPDK_DEBUGLOG(virtio_pci, "queue %"PRIu16" addresses:\n", vq->vq_queue_index);
	SPDK_DEBUGLOG(virtio_pci, "\t desc_addr: %" PRIx64 "\n", desc_addr);
	SPDK_DEBUGLOG(virtio_pci, "\t aval_addr: %" PRIx64 "\n", avail_addr);
	SPDK_DEBUGLOG(virtio_pci, "\t used_addr: %" PRIx64 "\n", used_addr);
	SPDK_DEBUGLOG(virtio_pci, "\t notify addr: %p (notify offset: %"PRIu16")\n",
		      vq->notify_addr, notify_off);

	return 0;
}

static void
modern_del_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = dev->ctx;

	spdk_mmio_write_2(&hw->common_cfg->queue_select, vq->vq_queue_index);

	io_write64_twopart(0, &hw->common_cfg->queue_desc_lo,
			   &hw->common_cfg->queue_desc_hi);
	io_write64_twopart(0, &hw->common_cfg->queue_avail_lo,
			   &hw->common_cfg->queue_avail_hi);
	io_write64_twopart(0, &hw->common_cfg->queue_used_lo,
			   &hw->common_cfg->queue_used_hi);

	spdk_mmio_write_2(&hw->common_cfg->queue_enable, 0);

	spdk_free(vq->vq_ring_virt_mem);
}

static void
modern_notify_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	spdk_mmio_write_2(vq->notify_addr, vq->vq_queue_index);
}

static const struct virtio_dev_ops modern_ops = {
	.read_dev_cfg	= modern_read_dev_config,
	.write_dev_cfg	= modern_write_dev_config,
	.get_status	= modern_get_status,
	.set_status	= modern_set_status,
	.get_features	= modern_get_features,
	.set_features	= modern_set_features,
	.destruct_dev	= modern_destruct_dev,
	.get_queue_size	= modern_get_queue_size,
	.setup_queue	= modern_setup_queue,
	.del_queue	= modern_del_queue,
	.notify_queue	= modern_notify_queue,
	.dump_json_info = pci_dump_json_info,
	.write_json_config = pci_write_json_config,
};

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
		SPDK_DEBUGLOG(virtio_pci, "failed to read pci capability list\n");
		return ret;
	}

	while (pos) {
		ret = spdk_pci_device_cfg_read(hw->pci_dev, &cap, sizeof(cap), pos);
		if (ret < 0) {
			SPDK_ERRLOG("failed to read pci cap at pos: %"PRIx8"\n", pos);
			break;
		}

		if (cap.cap_vndr == PCI_CAP_ID_MSIX) {
			hw->use_msix = 1;
		}

		if (cap.cap_vndr != PCI_CAP_ID_VNDR) {
			SPDK_DEBUGLOG(virtio_pci,
				      "[%2"PRIx8"] skipping non VNDR cap id: %02"PRIx8"\n",
				      pos, cap.cap_vndr);
			goto next;
		}

		SPDK_DEBUGLOG(virtio_pci,
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
		SPDK_DEBUGLOG(virtio_pci, "no modern virtio pci device found.\n");
		if (ret < 0) {
			return ret;
		} else {
			return -EINVAL;
		}
	}

	SPDK_DEBUGLOG(virtio_pci, "found modern virtio pci device.\n");

	SPDK_DEBUGLOG(virtio_pci, "common cfg mapped at: %p\n", hw->common_cfg);
	SPDK_DEBUGLOG(virtio_pci, "device cfg mapped at: %p\n", hw->dev_cfg);
	SPDK_DEBUGLOG(virtio_pci, "isr cfg mapped at: %p\n", hw->isr);
	SPDK_DEBUGLOG(virtio_pci, "notify base: %p, notify off multiplier: %u\n",
		      hw->notify_base, hw->notify_off_multiplier);

	return 0;
}

static int
virtio_pci_dev_probe(struct spdk_pci_device *pci_dev, struct virtio_pci_probe_ctx *ctx)
{
	struct virtio_hw *hw;
	uint8_t *bar_vaddr;
	uint64_t bar_paddr, bar_len;
	int rc;
	unsigned i;
	char bdf[32];
	struct spdk_pci_addr addr;

	addr = spdk_pci_device_get_addr(pci_dev);
	rc = spdk_pci_addr_fmt(bdf, sizeof(bdf), &addr);
	if (rc != 0) {
		SPDK_ERRLOG("Ignoring a device with non-parseable PCI address\n");
		return -1;
	}

	hw = calloc(1, sizeof(*hw));
	if (hw == NULL) {
		SPDK_ERRLOG("%s: calloc failed\n", bdf);
		return -1;
	}

	hw->pci_dev = pci_dev;

	for (i = 0; i < 6; ++i) {
		rc = spdk_pci_device_map_bar(pci_dev, i, (void *) &bar_vaddr, &bar_paddr,
					     &bar_len);
		if (rc != 0) {
			SPDK_ERRLOG("%s: failed to memmap PCI BAR %u\n", bdf, i);
			free_virtio_hw(hw);
			return -1;
		}

		hw->pci_bar[i].vaddr = bar_vaddr;
		hw->pci_bar[i].len = bar_len;
	}

	/* Virtio PCI caps exist only on modern PCI devices.
	 * Legacy devices are not supported.
	 */
	if (virtio_read_caps(hw) != 0) {
		SPDK_NOTICELOG("Ignoring legacy PCI device at %s\n", bdf);
		free_virtio_hw(hw);
		return -1;
	}

	rc = ctx->enum_cb((struct virtio_pci_ctx *)hw, ctx->enum_ctx);
	if (rc != 0) {
		free_virtio_hw(hw);
	}

	return rc;
}

static int
virtio_pci_dev_probe_cb(void *probe_ctx, struct spdk_pci_device *pci_dev)
{
	struct virtio_pci_probe_ctx *ctx = probe_ctx;
	uint16_t pci_device_id = spdk_pci_device_get_device_id(pci_dev);
	uint16_t device_id;

	if (pci_device_id < 0x1000 || pci_device_id > 0x107f) {
		SPDK_ERRLOG("Probe device is not a virtio device\n");
		return 1;
	}

	if (pci_device_id < 0x1040) {
		/* Transitional devices: use the PCI subsystem device id as
		 * virtio device id, same as legacy driver always did.
		 */
		device_id = spdk_pci_device_get_subdevice_id(pci_dev);
	} else {
		/* Modern devices: simply use PCI device id, but start from 0x1040. */
		device_id = pci_device_id - 0x1040;
	}

	if (device_id != ctx->device_id) {
		return 1;
	}

	return virtio_pci_dev_probe(pci_dev, ctx);
}

int
virtio_pci_dev_enumerate(virtio_pci_create_cb enum_cb, void *enum_ctx,
			 uint16_t pci_device_id)
{
	struct virtio_pci_probe_ctx ctx;

	if (!spdk_process_is_primary()) {
		SPDK_WARNLOG("virtio_pci secondary process support is not implemented yet.\n");
		return 0;
	}

	ctx.enum_cb = enum_cb;
	ctx.enum_ctx = enum_ctx;
	ctx.device_id = pci_device_id;

	return spdk_pci_enumerate(spdk_pci_virtio_get_driver(),
				  virtio_pci_dev_probe_cb, &ctx);
}

int
virtio_pci_dev_attach(virtio_pci_create_cb enum_cb, void *enum_ctx,
		      uint16_t device_id, struct spdk_pci_addr *pci_address)
{
	struct virtio_pci_probe_ctx ctx;

	if (!spdk_process_is_primary()) {
		SPDK_WARNLOG("virtio_pci secondary process support is not implemented yet.\n");
		return 0;
	}

	ctx.enum_cb = enum_cb;
	ctx.enum_ctx = enum_ctx;
	ctx.device_id = device_id;

	return spdk_pci_device_attach(spdk_pci_virtio_get_driver(),
				      virtio_pci_dev_probe_cb, &ctx, pci_address);
}

int
virtio_pci_dev_init(struct virtio_dev *vdev, const char *name,
		    struct virtio_pci_ctx *pci_ctx)
{
	int rc;

	rc = virtio_dev_construct(vdev, name, &modern_ops, pci_ctx);
	if (rc != 0) {
		return rc;
	}

	vdev->is_hw = 1;
	vdev->modern = 1;

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(virtio_pci)
