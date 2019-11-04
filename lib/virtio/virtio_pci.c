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

#include "spdk/mmio.h"
#include "spdk/string.h"
#include "spdk/env.h"

#include "spdk_internal/virtio.h"
#include "spdk_internal/memory.h"

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

	struct spdk_pci_ioport io_port;
	int modern;

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

	if (hw->modern) {
		for (i = 0; i < 6; ++i) {
			if (hw->pci_bar[i].vaddr == NULL) {
				continue;
			}

			spdk_pci_device_unmap_bar(hw->pci_dev, i, hw->pci_bar[i].vaddr);
		}
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

#ifdef PCI_LEGACY_SUPPORT

/*
 * VirtIO Header, located in BAR 0.
 */
#define VIRTIO_PCI_HOST_FEATURES  0  /* host's supported features (32bit, RO) */
#define VIRTIO_PCI_GUEST_FEATURES 4  /* guest's supported features (32, RW) */
#define VIRTIO_PCI_QUEUE_PFN      8  /* physical address of VQ (32, RW) */
#define VIRTIO_PCI_QUEUE_NUM      12 /* number of ring entries (16, RO) */
#define VIRTIO_PCI_QUEUE_SEL      14 /* current VQ selection (16, RW) */
#define VIRTIO_PCI_QUEUE_NOTIFY   16 /* notify host regarding VQ (16, RW) */
#define VIRTIO_PCI_STATUS         18 /* device status register (8, RW) */
#define VIRTIO_PCI_ISR		  19 /* interrupt status register, reading
				      * also clears the register (8, RO) */
/* Only if MSIX is enabled: */
#define VIRTIO_MSI_CONFIG_VECTOR  20 /* configuration change vector (16, RW) */
#define VIRTIO_MSI_QUEUE_VECTOR	  22 /* vector for selected VQ notifications
				      (16, RW) */

enum virtio_msix_status {
	VIRTIO_MSIX_NONE = 0,
	VIRTIO_MSIX_DISABLED = 1,
	VIRTIO_MSIX_ENABLED = 2
};

/* from the spec "When MSI-X capability is present and enabled in the device
 * 4 bytes at byte of offset 20 are used to map configuration change and queue
 * interrupts to MSI-X vectors"
 * When the MSI-X is enabled the ISR Status field is unused.
 */
#define VIRTIO_PCI_CONFIG_LEGACY(hw) \
		(((hw)->use_msix == VIRTIO_MSIX_ENABLED) ? 24 : 20)

/*
 * Since we are in legacy mode:
 * http://ozlabs.org/~rusty/virtio-spec/virtio-0.9.5.pdf
 *
 * "Note that this is possible because while the virtio header is PCI (i.
 * little) endian, the device-specific region is encoded in the native endian of
 * the guest (where such distinction is applicable)."
 *
 * For powerpc which supports both, qemu supposes that cpu is big endian and
 * enforces this for the virtio-net stuff.
 */
static int
legacy_read_dev_config(struct virtio_dev *dev, size_t offset,
		       void *dst, int length)
{
	struct virtio_hw *hw = dev->ctx;

	spdk_pci_ioport_read(&hw->io_port, dst, length,
			     VIRTIO_PCI_CONFIG_LEGACY(hw) + offset);

	return 0;
}

static int
legacy_write_dev_config(struct virtio_dev *dev, size_t offset,
			const void *src, int length)
{
	struct virtio_hw *hw = dev->ctx;

	spdk_pci_ioport_write(&hw->io_port, src, length,
			      VIRTIO_PCI_CONFIG_LEGACY(hw) + offset);

	return 0;
}

static uint64_t
legacy_get_features(struct virtio_dev *dev)
{
	struct virtio_hw *hw = dev->ctx;
	uint32_t dst;

	spdk_pci_ioport_read(&hw->io_port, &dst, 4, VIRTIO_PCI_HOST_FEATURES);

	return dst;
}

static int
legacy_set_features(struct virtio_dev *dev, uint64_t features)
{
	struct virtio_hw *hw = dev->ctx;

	if ((features >> 32) != 0) {
		SPDK_ERRLOG("only 32 bit features are allowed for legacy virtio!\n");
		return -1;
	}

	spdk_pci_ioport_write(&hw->io_port, &features, 4,
			      VIRTIO_PCI_GUEST_FEATURES);

	dev->negotiated_features = features;

	return 0;
}

static void
legacy_destruct_dev(struct virtio_dev *vdev)
{
	struct virtio_hw *hw = vdev->ctx;
	struct spdk_pci_device *pci_dev = hw->pci_dev;

	free_virtio_hw(hw);
	spdk_pci_device_detach(pci_dev);
}

static uint8_t
legacy_get_status(struct virtio_dev *dev)
{
	struct virtio_hw *hw = dev->ctx;
	uint8_t dst;

	spdk_pci_ioport_read(&hw->io_port, &dst, 1, VIRTIO_PCI_STATUS);

	return dst;
}

static void
legacy_set_status(struct virtio_dev *dev, uint8_t status)
{
	struct virtio_hw *hw = dev->ctx;

	spdk_pci_ioport_write(&hw->io_port, &status, 1, VIRTIO_PCI_STATUS);
}

static uint16_t
legacy_get_queue_size(struct virtio_dev *dev, uint16_t queue_id)
{
	struct virtio_hw *hw = dev->ctx;
	uint16_t dst;

	spdk_pci_ioport_write(&hw->io_port, &queue_id, 2, VIRTIO_PCI_QUEUE_SEL);
	spdk_pci_ioport_read(&hw->io_port, &dst, 2, VIRTIO_PCI_QUEUE_NUM);

	return dst;
}

static int
legacy_setup_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = dev->ctx;
	void *queue_mem;
	uint64_t queue_mem_phys_addr;
	uint32_t src;

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
		return -1;
	}

	spdk_pci_ioport_write(&hw->io_port, &vq->vq_queue_index, 2,
			      VIRTIO_PCI_QUEUE_SEL);
	src = vq->vq_ring_mem >> VIRTIO_PCI_QUEUE_ADDR_SHIFT;
	spdk_pci_ioport_write(&hw->io_port, &src, 4, VIRTIO_PCI_QUEUE_PFN);

	return 0;
}

static void
legacy_del_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = dev->ctx;
	uint32_t src = 0;


	spdk_pci_ioport_write(&hw->io_port, &vq->vq_queue_index, 2,
			      VIRTIO_PCI_QUEUE_SEL);
	spdk_pci_ioport_write(&hw->io_port, &src, 4, VIRTIO_PCI_QUEUE_PFN);

	spdk_free(vq->vq_ring_virt_mem);
}

static void
legacy_notify_queue(struct virtio_dev *dev, struct virtqueue *vq)
{
	struct virtio_hw *hw = dev->ctx;

	spdk_pci_ioport_write(&hw->io_port, &vq->vq_queue_index, 2,
			      VIRTIO_PCI_QUEUE_NOTIFY);
}

static const struct virtio_dev_ops legacy_ops = {
	.read_dev_cfg	= legacy_read_dev_config,
	.write_dev_cfg	= legacy_write_dev_config,
	.get_status	= legacy_get_status,
	.set_status	= legacy_set_status,
	.get_features	= legacy_get_features,
	.set_features	= legacy_set_features,
	.destruct_dev	= legacy_destruct_dev,
	.get_queue_size	= legacy_get_queue_size,
	.setup_queue	= legacy_setup_queue,
	.del_queue	= legacy_del_queue,
	.notify_queue	= legacy_notify_queue,
	.dump_json_info = pci_dump_json_info,
	.write_json_config = pci_write_json_config,
};
#endif

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
	struct spdk_pci_device *pci_dev = hw->pci_dev;

	free_virtio_hw(hw);
	spdk_pci_device_detach(pci_dev);
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

	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "queue %"PRIu16" addresses:\n", vq->vq_queue_index);
	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "\t desc_addr: %" PRIx64 "\n", desc_addr);
	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "\t aval_addr: %" PRIx64 "\n", avail_addr);
	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "\t used_addr: %" PRIx64 "\n", used_addr);
	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "\t notify addr: %p (notify offset: %"PRIu16")\n",
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

#define PCI_MSIX_ENABLE	0x8000

static int
virtio_read_caps(struct virtio_hw *hw)
{
	uint8_t pos;
	struct virtio_pci_cap cap;
	int ret;

	ret = spdk_pci_device_cfg_read(hw->pci_dev, &pos, 1, PCI_CAPABILITY_LIST);
	if (ret < 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "failed to read pci capability list\n");
		return ret;
	}

	while (pos) {
		ret = spdk_pci_device_cfg_read(hw->pci_dev, &cap, sizeof(cap), pos);
		if (ret < 0) {
			SPDK_ERRLOG("failed to read pci cap at pos: %"PRIx8"\n", pos);
			break;
		}

		if (cap.cap_vndr == PCI_CAP_ID_MSIX) {
			uint16_t flags;

			ret = spdk_pci_device_cfg_read(hw->pci_dev, &flags, sizeof(flags), pos + 2);
			if (ret < 0) {
				SPDK_ERRLOG("failed to read pci cap at pos: %x\n", pos + 2);
				break;
			}

			if (flags & PCI_MSIX_ENABLE) {
				hw->use_msix = VIRTIO_MSIX_ENABLED;
			} else {
				hw->use_msix = VIRTIO_MSIX_DISABLED;
			}
		}

		if (cap.cap_vndr != PCI_CAP_ID_VNDR) {
			SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI,
				      "[%2"PRIx8"] skipping non VNDR cap id: %02"PRIx8"\n",
				      pos, cap.cap_vndr);
			goto next;
		}

		SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI,
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
		SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "no modern virtio pci device found.\n");
		if (ret < 0) {
			return ret;
		} else {
			/* Legacy mode */
			return 1;
		}
	}

	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "found modern virtio pci device.\n");

	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "common cfg mapped at: %p\n", hw->common_cfg);
	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "device cfg mapped at: %p\n", hw->dev_cfg);
	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "isr cfg mapped at: %p\n", hw->isr);
	SPDK_DEBUGLOG(SPDK_LOG_VIRTIO_PCI, "notify base: %p, notify off multiplier: %u\n",
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

	/* Virtio PCI caps exist only on modern PCI devices.
	 * Legacy devices are not supported.
	 */
	rc = virtio_read_caps(hw);
	if (rc == 1) {
		SPDK_NOTICELOG("legacy PCI device at %s\n", bdf);

		if (spdk_init_iopl() != 0) {
			SPDK_ERRLOG("failed to init iopl\n");
			free(hw);
			return -1;
		}

		if (spdk_pci_ioport_map(pci_dev, 0, &hw->io_port) < 0) {
			free(hw);
			return -1;
		}

		hw->modern = 0;
	} else if (rc == 0) {
		SPDK_NOTICELOG("modern PCI device at %s\n", bdf);
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

		hw->modern = 1;
	} else {
		free(hw);
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

	if (pci_device_id != ctx->device_id) {
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
		      uint16_t pci_device_id, struct spdk_pci_addr *pci_address)
{
	struct virtio_pci_probe_ctx ctx;

	if (!spdk_process_is_primary()) {
		SPDK_WARNLOG("virtio_pci secondary process support is not implemented yet.\n");
		return 0;
	}

	ctx.enum_cb = enum_cb;
	ctx.enum_ctx = enum_ctx;
	ctx.device_id = pci_device_id;

	return spdk_pci_device_attach(spdk_pci_virtio_get_driver(),
				      virtio_pci_dev_probe_cb, &ctx, pci_address);
}

int
virtio_pci_dev_init(struct virtio_dev *vdev, const char *name,
		    struct virtio_pci_ctx *pci_ctx)
{
	struct virtio_hw *hw = (struct virtio_hw *)pci_ctx;
	int rc;

	if (hw->modern) {
		rc = virtio_dev_construct(vdev, name, &modern_ops, pci_ctx);
		if (rc != 0) {
			return rc;
		}

		vdev->modern = 1;
	} else {
		rc = virtio_dev_construct(vdev, name, &legacy_ops, pci_ctx);
		if (rc != 0) {
			return rc;
		}

		vdev->modern = 0;
	}

	vdev->is_hw = 1;

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("virtio_pci", SPDK_LOG_VIRTIO_PCI)
