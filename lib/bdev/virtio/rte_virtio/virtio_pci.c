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
#include <stdint.h>

#ifdef RTE_EXEC_ENV_LINUXAPP
 #include <dirent.h>
 #include <fcntl.h>
#endif

#include <rte_io.h>

#include "virtio_pci.h"
#include "virtio_logs.h"
#include "virtio_queue.h"

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

/*
 * The remaining space is defined by each driver as the per-driver
 * configuration space.
 */
#define VIRTIO_PCI_CONFIG(hw) (((hw)->use_msix) ? 24 : 20)

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
		PMD_INIT_LOG(ERR, "vring address shouldn't be above 16TB!");
		return 0;
	}

	return 1;
}

/*
 * Since we are in legacy mode:
 * http://ozlabs.org/~rusty/virtio-spec/virtio-0.9.5.pdf
 *
 * "Note that this is possible because while the virtio header is PCI (i.e.
 * little) endian, the device-specific region is encoded in the native endian of
 * the guest (where such distinction is applicable)."
 *
 * For powerpc which supports both, qemu supposes that cpu is big endian and
 * enforces this for the virtio-net stuff.
 */
static void
legacy_read_dev_config(struct virtio_dev *dev, size_t offset,
		       void *dst, int length)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
#ifdef RTE_ARCH_PPC_64
	int size;

	while (length > 0) {
		if (length >= 4) {
			size = 4;
			rte_pci_ioport_read(VTPCI_IO(dev), dst, size,
				VIRTIO_PCI_CONFIG(dev) + offset);
			*(uint32_t *)dst = rte_be_to_cpu_32(*(uint32_t *)dst);
		} else if (length >= 2) {
			size = 2;
			rte_pci_ioport_read(VTPCI_IO(dev), dst, size,
				VIRTIO_PCI_CONFIG(dev) + offset);
			*(uint16_t *)dst = rte_be_to_cpu_16(*(uint16_t *)dst);
		} else {
			size = 1;
			rte_pci_ioport_read(VTPCI_IO(dev), dst, size,
				VIRTIO_PCI_CONFIG(dev) + offset);
		}

		dst = (char *)dst + size;
		offset += size;
		length -= size;
	}
#else
	rte_pci_ioport_read(VTPCI_IO(dev), dst, length,
		VIRTIO_PCI_CONFIG(hw) + offset);
#endif
}

static void
legacy_write_dev_config(struct virtio_dev *dev, size_t offset,
			const void *src, int length)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
#ifdef RTE_ARCH_PPC_64
	union {
		uint32_t u32;
		uint16_t u16;
	} tmp;
	int size;

	while (length > 0) {
		if (length >= 4) {
			size = 4;
			tmp.u32 = rte_cpu_to_be_32(*(const uint32_t *)src);
			rte_pci_ioport_write(VTPCI_IO(hw), &tmp.u32, size,
				VIRTIO_PCI_CONFIG(hw) + offset);
		} else if (length >= 2) {
			size = 2;
			tmp.u16 = rte_cpu_to_be_16(*(const uint16_t *)src);
			rte_pci_ioport_write(VTPCI_IO(hw), &tmp.u16, size,
				VIRTIO_PCI_CONFIG(hw) + offset);
		} else {
			size = 1;
			rte_pci_ioport_write(VTPCI_IO(hw), src, size,
				VIRTIO_PCI_CONFIG(hw) + offset);
		}

		src = (const char *)src + size;
		offset += size;
		length -= size;
	}
#else
	rte_pci_ioport_write(VTPCI_IO(dev), src, length,
		VIRTIO_PCI_CONFIG(hw) + offset);
#endif
}

static uint64_t
legacy_get_features(struct virtio_dev *dev)
{
	uint32_t dst;

	rte_pci_ioport_read(VTPCI_IO(dev), &dst, 4, VIRTIO_PCI_HOST_FEATURES);
	return dst;
}

static void
legacy_set_features(struct virtio_dev *dev, uint64_t features)
{
	if ((features >> 32) != 0) {
		PMD_DRV_LOG(ERR,
			"only 32 bit features are allowed for legacy virtio!");
		return;
	}
	rte_pci_ioport_write(VTPCI_IO(dev), &features, 4,
		VIRTIO_PCI_GUEST_FEATURES);
}

static uint8_t
legacy_get_status(struct virtio_dev *dev)
{
	uint8_t dst;

	rte_pci_ioport_read(VTPCI_IO(dev), &dst, 1, VIRTIO_PCI_STATUS);
	return dst;
}

static void
legacy_set_status(struct virtio_dev *dev, uint8_t status)
{
	rte_pci_ioport_write(VTPCI_IO(dev), &status, 1, VIRTIO_PCI_STATUS);
}

static void
legacy_reset(struct virtio_dev *dev)
{
	legacy_set_status(dev, VIRTIO_CONFIG_STATUS_RESET);
}

static uint8_t
legacy_get_isr(struct virtio_dev *dev)
{
	uint8_t dst;

	rte_pci_ioport_read(VTPCI_IO(dev), &dst, 1, VIRTIO_PCI_ISR);
	return dst;
}

/* Enable one vector (0) for Link State Intrerrupt */
static uint16_t
legacy_set_config_irq(struct virtio_dev *dev, uint16_t vec)
{
	uint16_t dst;

	rte_pci_ioport_write(VTPCI_IO(dev), &vec, 2, VIRTIO_MSI_CONFIG_VECTOR);
	rte_pci_ioport_read(VTPCI_IO(dev), &dst, 2, VIRTIO_MSI_CONFIG_VECTOR);
	return dst;
}

static uint16_t
legacy_set_queue_irq(struct virtio_dev *dev, struct virtqueue *vq, uint16_t vec)
{
	uint16_t dst;

	rte_pci_ioport_write(VTPCI_IO(dev), &vq->vq_queue_index, 2,
		VIRTIO_PCI_QUEUE_SEL);
	rte_pci_ioport_write(VTPCI_IO(dev), &vec, 2, VIRTIO_MSI_QUEUE_VECTOR);
	rte_pci_ioport_read(VTPCI_IO(dev), &dst, 2, VIRTIO_MSI_QUEUE_VECTOR);
	return dst;
}

static uint16_t
legacy_get_queue_num(struct virtio_dev *dev, uint16_t queue_id)
{
	uint16_t dst;

	rte_pci_ioport_write(VTPCI_IO(dev), &queue_id, 2, VIRTIO_PCI_QUEUE_SEL);
	rte_pci_ioport_read(VTPCI_IO(dev), &dst, 2, VIRTIO_PCI_QUEUE_NUM);
	return dst;
}

static int
legacy_setup_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	uint32_t src;

	if (!check_vq_phys_addr_ok(vq))
		return -1;

	rte_pci_ioport_write(VTPCI_IO(dev), &vq->vq_queue_index, 2,
		VIRTIO_PCI_QUEUE_SEL);
	src = vq->vq_ring_mem >> VIRTIO_PCI_QUEUE_ADDR_SHIFT;
	rte_pci_ioport_write(VTPCI_IO(dev), &src, 4, VIRTIO_PCI_QUEUE_PFN);

	return 0;
}

static void
legacy_del_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	uint32_t src = 0;

	rte_pci_ioport_write(VTPCI_IO(dev), &vq->vq_queue_index, 2,
		VIRTIO_PCI_QUEUE_SEL);
	rte_pci_ioport_write(VTPCI_IO(dev), &src, 4, VIRTIO_PCI_QUEUE_PFN);
}

static void
legacy_notify_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	rte_pci_ioport_write(VTPCI_IO(dev), &vq->vq_queue_index, 2,
		VIRTIO_PCI_QUEUE_NOTIFY);
}

const struct virtio_pci_ops legacy_ops = {
	.read_dev_cfg	= legacy_read_dev_config,
	.write_dev_cfg	= legacy_write_dev_config,
	.reset		= legacy_reset,
	.get_status	= legacy_get_status,
	.set_status	= legacy_set_status,
	.get_features	= legacy_get_features,
	.set_features	= legacy_set_features,
	.get_isr	= legacy_get_isr,
	.set_config_irq	= legacy_set_config_irq,
	.set_queue_irq  = legacy_set_queue_irq,
	.get_queue_num	= legacy_get_queue_num,
	.setup_queue	= legacy_setup_queue,
	.del_queue	= legacy_del_queue,
	.notify_queue	= legacy_notify_queue,
};

static inline void
io_write64_twopart(uint64_t val, uint32_t *lo, uint32_t *hi)
{
	rte_write32(val & ((1ULL << 32) - 1), lo);
	rte_write32(val >> 32,		     hi);
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
		old_gen = rte_read8(&hw->common_cfg->config_generation);

		p = dst;
		for (i = 0;  i < length; i++)
			*p++ = rte_read8((uint8_t *)hw->dev_cfg + offset + i);

		new_gen = rte_read8(&hw->common_cfg->config_generation);
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
		rte_write8((*p++), (((uint8_t *)hw->dev_cfg) + offset + i));
}

static uint64_t
modern_get_features(struct virtio_dev *dev)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);
	uint32_t features_lo, features_hi;

	rte_write32(0, &hw->common_cfg->device_feature_select);
	features_lo = rte_read32(&hw->common_cfg->device_feature);

	rte_write32(1, &hw->common_cfg->device_feature_select);
	features_hi = rte_read32(&hw->common_cfg->device_feature);

	return ((uint64_t)features_hi << 32) | features_lo;
}

static void
modern_set_features(struct virtio_dev *dev, uint64_t features)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	rte_write32(0, &hw->common_cfg->guest_feature_select);
	rte_write32(features & ((1ULL << 32) - 1),
		    &hw->common_cfg->guest_feature);

	rte_write32(1, &hw->common_cfg->guest_feature_select);
	rte_write32(features >> 32,
		    &hw->common_cfg->guest_feature);
}

static uint8_t
modern_get_status(struct virtio_dev *dev)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	return rte_read8(&hw->common_cfg->device_status);
}

static void
modern_set_status(struct virtio_dev *dev, uint8_t status)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	rte_write8(status, &hw->common_cfg->device_status);
}

static void
modern_reset(struct virtio_dev *dev)
{
	modern_set_status(dev, VIRTIO_CONFIG_STATUS_RESET);
	modern_get_status(dev);
}

static uint8_t
modern_get_isr(struct virtio_dev *dev)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	return rte_read8(hw->isr);
}

static uint16_t
modern_set_config_irq(struct virtio_dev *dev, uint16_t vec)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	rte_write16(vec, &hw->common_cfg->msix_config);
	return rte_read16(&hw->common_cfg->msix_config);
}

static uint16_t
modern_set_queue_irq(struct virtio_dev *dev, struct virtqueue *vq, uint16_t vec)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	rte_write16(vq->vq_queue_index, &hw->common_cfg->queue_select);
	rte_write16(vec, &hw->common_cfg->queue_msix_vector);
	return rte_read16(&hw->common_cfg->queue_msix_vector);
}

static uint16_t
modern_get_queue_num(struct virtio_dev *dev, uint16_t queue_id)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	rte_write16(queue_id, &hw->common_cfg->queue_select);
	return rte_read16(&hw->common_cfg->queue_size);
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

	rte_write16(vq->vq_queue_index, &hw->common_cfg->queue_select);

	io_write64_twopart(desc_addr, &hw->common_cfg->queue_desc_lo,
				      &hw->common_cfg->queue_desc_hi);
	io_write64_twopart(avail_addr, &hw->common_cfg->queue_avail_lo,
				       &hw->common_cfg->queue_avail_hi);
	io_write64_twopart(used_addr, &hw->common_cfg->queue_used_lo,
				      &hw->common_cfg->queue_used_hi);

	notify_off = rte_read16(&hw->common_cfg->queue_notify_off);
	vq->notify_addr = (void *)((uint8_t *)hw->notify_base +
				notify_off * hw->notify_off_multiplier);

	rte_write16(1, &hw->common_cfg->queue_enable);

	PMD_INIT_LOG(DEBUG, "queue %u addresses:", vq->vq_queue_index);
	PMD_INIT_LOG(DEBUG, "\t desc_addr: %" PRIx64, desc_addr);
	PMD_INIT_LOG(DEBUG, "\t aval_addr: %" PRIx64, avail_addr);
	PMD_INIT_LOG(DEBUG, "\t used_addr: %" PRIx64, used_addr);
	PMD_INIT_LOG(DEBUG, "\t notify addr: %p (notify offset: %u)",
		vq->notify_addr, notify_off);

	return 0;
}

static void
modern_del_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = virtio_dev_get_hw(dev);

	rte_write16(vq->vq_queue_index, &hw->common_cfg->queue_select);

	io_write64_twopart(0, &hw->common_cfg->queue_desc_lo,
				  &hw->common_cfg->queue_desc_hi);
	io_write64_twopart(0, &hw->common_cfg->queue_avail_lo,
				  &hw->common_cfg->queue_avail_hi);
	io_write64_twopart(0, &hw->common_cfg->queue_used_lo,
				  &hw->common_cfg->queue_used_hi);

	rte_write16(0, &hw->common_cfg->queue_enable);
}

static void
modern_notify_queue(struct virtio_dev *dev __rte_unused, struct virtqueue *vq)
{
	rte_write16(vq->vq_queue_index, vq->notify_addr);
}

const struct virtio_pci_ops modern_ops = {
	.read_dev_cfg	= modern_read_dev_config,
	.write_dev_cfg	= modern_write_dev_config,
	.reset		= modern_reset,
	.get_status	= modern_get_status,
	.set_status	= modern_set_status,
	.get_features	= modern_get_features,
	.set_features	= modern_set_features,
	.get_isr	= modern_get_isr,
	.set_config_irq	= modern_set_config_irq,
	.set_queue_irq  = modern_set_queue_irq,
	.get_queue_num	= modern_get_queue_num,
	.setup_queue	= modern_setup_queue,
	.del_queue	= modern_del_queue,
	.notify_queue	= modern_notify_queue,
};


void
vtpci_read_dev_config(struct virtio_dev *dev, size_t offset,
		      void *dst, int length)
{
	VTPCI_OPS(dev)->read_dev_cfg(dev, offset, dst, length);
}

void
vtpci_write_dev_config(struct virtio_dev *dev, size_t offset,
		       const void *src, int length)
{
	VTPCI_OPS(dev)->write_dev_cfg(dev, offset, src, length);
}

uint64_t
vtpci_negotiate_features(struct virtio_dev *dev, uint64_t host_features)
{
	uint64_t features;

	/*
	 * Limit negotiated features to what the driver, virtqueue, and
	 * host all support.
	 */
	features = host_features & dev->req_guest_features;
	VTPCI_OPS(dev)->set_features(dev, features);

	return features;
}

void
vtpci_reset(struct virtio_dev *dev)
{
	VTPCI_OPS(dev)->set_status(dev, VIRTIO_CONFIG_STATUS_RESET);
	/* flush status write */
	VTPCI_OPS(dev)->get_status(dev);
}

void
vtpci_reinit_complete(struct virtio_dev *dev)
{
	vtpci_set_status(dev, VIRTIO_CONFIG_STATUS_DRIVER_OK);
}

void
vtpci_set_status(struct virtio_dev *dev, uint8_t status)
{
	if (status != VIRTIO_CONFIG_STATUS_RESET)
		status |= VTPCI_OPS(dev)->get_status(dev);

	VTPCI_OPS(dev)->set_status(dev, status);
}

uint8_t
vtpci_get_status(struct virtio_dev *dev)
{
	return VTPCI_OPS(dev)->get_status(dev);
}

uint8_t
vtpci_isr(struct virtio_dev *dev)
{
	return VTPCI_OPS(dev)->get_isr(dev);
}

static void *
get_cfg_addr(struct spdk_pci_device *dev, struct virtio_pci_cap *cap)
{
	uint8_t  bar    = cap->bar;
	uint32_t length = cap->length;
	uint32_t offset = cap->offset;
	uint8_t *bar_vaddr;
	uint64_t bar_paddr, bar_len;

	if (bar > 5) {
		PMD_INIT_LOG(ERR, "invalid bar: %u", bar);
		return NULL;
	}

	if (offset + length < offset) {
		PMD_INIT_LOG(ERR, "offset(%u) + length(%u) overflows",
			offset, length);
		return NULL;
	}

	spdk_pci_device_map_bar(dev, bar, (void *) &bar_vaddr, &bar_paddr, &bar_len);
	/* FIXME the bar should be also unmapped.
	 * However, current spdk_pci_device_map_bar
	 * implementation doesn't alloc anything,
	 * so there's no leak whatsoever.
	 */

	if (offset + length > bar_len) {
		PMD_INIT_LOG(ERR,
			"invalid cap: overflows bar space: %u > %" PRIu64,
			offset + length, dev->mem_resource[bar].len);
		return NULL;
	}

	if (bar_vaddr == NULL) {
		PMD_INIT_LOG(ERR, "bar %u base addr is NULL", bar);
		return NULL;
	}

	return bar_vaddr + offset;
}

static int
virtio_read_caps(struct spdk_pci_device *dev, struct virtio_hw *hw)
{
	uint8_t pos;
	struct virtio_pci_cap cap;
	int ret;

	ret = spdk_pci_device_cfg_read(dev, &pos, 1, PCI_CAPABILITY_LIST);
	if (ret < 0) {
		PMD_INIT_LOG(DEBUG, "failed to read pci capability list");
		return -1;
	}

	while (pos) {
		ret = spdk_pci_device_cfg_read(dev, &cap, sizeof(cap), pos);
		if (ret < 0) {
			PMD_INIT_LOG(ERR,
				"failed to read pci cap at pos: %x", pos);
			break;
		}

		if (cap.cap_vndr == PCI_CAP_ID_MSIX)
			hw->use_msix = 1;

		if (cap.cap_vndr != PCI_CAP_ID_VNDR) {
			PMD_INIT_LOG(DEBUG,
				"[%2x] skipping non VNDR cap id: %02x",
				pos, cap.cap_vndr);
			goto next;
		}

		PMD_INIT_LOG(DEBUG,
			"[%2x] cfg type: %u, bar: %u, offset: %04x, len: %u",
			pos, cap.cfg_type, cap.bar, cap.offset, cap.length);

		switch (cap.cfg_type) {
		case VIRTIO_PCI_CAP_COMMON_CFG:
			hw->common_cfg = get_cfg_addr(dev, &cap);
			break;
		case VIRTIO_PCI_CAP_NOTIFY_CFG:
			spdk_pci_device_cfg_read(dev, &hw->notify_off_multiplier,
					4, pos + sizeof(cap));
			hw->notify_base = get_cfg_addr(dev, &cap);
			break;
		case VIRTIO_PCI_CAP_DEVICE_CFG:
			hw->dev_cfg = get_cfg_addr(dev, &cap);
			break;
		case VIRTIO_PCI_CAP_ISR_CFG:
			hw->isr = get_cfg_addr(dev, &cap);
			break;
		}

next:
		pos = cap.cap_next;
	}

	if (hw->common_cfg == NULL || hw->notify_base == NULL ||
	    hw->dev_cfg == NULL    || hw->isr == NULL) {
		PMD_INIT_LOG(INFO, "no modern virtio pci device found.");
		return -1;
	}

	PMD_INIT_LOG(INFO, "found modern virtio pci device.");

	PMD_INIT_LOG(DEBUG, "common cfg mapped at: %p", hw->common_cfg);
	PMD_INIT_LOG(DEBUG, "device cfg mapped at: %p", hw->dev_cfg);
	PMD_INIT_LOG(DEBUG, "isr cfg mapped at: %p", hw->isr);
	PMD_INIT_LOG(DEBUG, "notify base: %p, notify off multiplier: %u",
		hw->notify_base, hw->notify_off_multiplier);

	return 0;
}

static int
pci_enum_virtio_probe_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct virtio_hw *hw;
	struct virtio_dev *vdev;

	hw = calloc(1, sizeof(*hw));
	vdev = &hw->vdev;
	vdev->is_hw = 1;
	hw->pci_dev = pci_dev;

	/*
	 * Try if we can succeed reading virtio pci caps, which exists
	 * only on modern pci device. If failed, we fallback to legacy
	 * virtio handling.
	 */
	if (virtio_read_caps(pci_dev, hw) == 0) {
		PMD_INIT_LOG(INFO, "modern virtio pci detected.");
		VTPCI_OPS(vdev) = &modern_ops;
		vdev->modern = 1;
		TAILQ_INSERT_TAIL(&g_virtio_driver.init_ctrlrs, vdev, tailq);
		return 0;
	}

#if 0
	PMD_INIT_LOG(INFO, "trying with legacy virtio pci.");
	if (rte_pci_ioport_map(dev, 0, VTPCI_IO(hw)) < 0) {
		if (dev->kdrv == RTE_KDRV_UNKNOWN &&
		    (!dev->device.devargs ||
		     dev->device.devargs->type !=
			RTE_DEVTYPE_WHITELISTED_PCI)) {
			PMD_INIT_LOG(INFO,
				"skip kernel managed virtio device.");
			return 1;
		}
		return -1;
	}
#endif

	VTPCI_OPS(vdev) = &legacy_ops;
	vdev->modern   = 0;

	TAILQ_INSERT_TAIL(&g_virtio_driver.init_ctrlrs, vdev, tailq);
	return 0;
}

int
vtpci_init(void)
{
	if (!spdk_process_is_primary()) {
		PMD_INIT_LOG(INFO, "virtio_pci secondary process support is not implemented yet.");
		return 0;
	}

	return spdk_pci_virtio_enumerate(pci_enum_virtio_probe_cb, NULL);
}
