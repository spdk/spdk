/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#include "ioat_internal.h"
#include "ioat_pci.h"

/** List of channels that have been attached but are not yet assigned to a thread.
 *
 * Must hold g_ioat_driver.lock while manipulating this list.
 */
static SLIST_HEAD(, ioat_channel) ioat_free_channels;

/** IOAT channel assigned to this thread (or NULL if not assigned yet). */
static __thread struct ioat_channel *ioat_thread_channel;

struct ioat_driver {
	ioat_mutex_t	lock;
};

static struct ioat_driver g_ioat_driver = {
	.lock = IOAT_MUTEX_INITIALIZER,
};

struct pci_device_id {
	uint16_t vendor;
	uint16_t device;
};

static const struct pci_device_id ioat_pci_table[] = {
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB0},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB1},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB2},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB3},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB4},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB5},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB6},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_SNB7},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB0},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB1},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB2},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB3},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB4},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB5},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB6},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_IVB7},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW0},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW1},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW2},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW3},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW4},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW5},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW6},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_HSW7},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX0},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX1},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX2},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX3},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX4},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX5},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX6},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX7},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX8},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDX9},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BWD0},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BWD1},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BWD2},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BWD3},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDXDE0},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDXDE1},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDXDE2},
	{PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IOAT_BDXDE3},
};

bool
ioat_pci_device_match_id(uint16_t vendor_id, uint16_t device_id)
{
	size_t i;
	const struct pci_device_id *ids;

	for (i = 0; i < sizeof(ioat_pci_table) / sizeof(struct pci_device_id); i++) {
		ids = &ioat_pci_table[i];
		if (ids->device == device_id && ids->vendor == vendor_id) {
			return true;
		}
	}
	return false;
}

static uint64_t
ioat_get_chansts(struct ioat_channel *ioat)
{
	return spdk_mmio_read_8(&ioat->regs->chansts);
}

static void
ioat_write_chancmp(struct ioat_channel *ioat, uint64_t addr)
{
	spdk_mmio_write_8(&ioat->regs->chancmp, addr);
}

static void
ioat_write_chainaddr(struct ioat_channel *ioat, uint64_t addr)
{
	spdk_mmio_write_8(&ioat->regs->chainaddr, addr);
}

static inline void
ioat_suspend(struct ioat_channel *ioat)
{
	ioat->regs->chancmd = IOAT_CHANCMD_SUSPEND;
}

static inline void
ioat_reset(struct ioat_channel *ioat)
{
	ioat->regs->chancmd = IOAT_CHANCMD_RESET;
}

static inline uint32_t
ioat_reset_pending(struct ioat_channel *ioat)
{
	uint8_t cmd;

	cmd = ioat->regs->chancmd;
	return (cmd & IOAT_CHANCMD_RESET) == IOAT_CHANCMD_RESET;
}

static int
ioat_map_pci_bar(struct ioat_channel *ioat)
{
	int regs_bar, rc;
	void *addr;

	regs_bar = 0;
	rc = ioat_pcicfg_map_bar(ioat->device, regs_bar, 0, &addr);
	if (rc != 0 || addr == NULL) {
		ioat_printf(ioat, "%s: pci_device_map_range failed with error code %d\n",
			    __func__, rc);
		return -1;
	}

	ioat->regs = (volatile struct ioat_registers *)addr;

	return 0;
}

static int
ioat_unmap_pci_bar(struct ioat_channel *ioat)
{
	int rc = 0;
	void *addr = (void *)ioat->regs;

	if (addr) {
		rc = ioat_pcicfg_unmap_bar(ioat->device, 0, addr);
	}
	return rc;
}


static inline uint32_t
ioat_get_active(struct ioat_channel *ioat)
{
	return (ioat->head - ioat->tail) & ((1 << ioat->ring_size_order) - 1);
}

static inline uint32_t
ioat_get_ring_space(struct ioat_channel *ioat)
{
	return (1 << ioat->ring_size_order) - ioat_get_active(ioat) - 1;
}

static uint32_t
ioat_get_ring_index(struct ioat_channel *ioat, uint32_t index)
{
	return index & ((1 << ioat->ring_size_order) - 1);
}

static void
ioat_get_ring_entry(struct ioat_channel *ioat, uint32_t index,
		    struct ioat_descriptor **desc,
		    struct ioat_dma_hw_descriptor **hw_desc)
{
	uint32_t i = ioat_get_ring_index(ioat, index);

	*desc = &ioat->ring[i];
	*hw_desc = &ioat->hw_ring[i];
}

static uint64_t
ioat_get_desc_phys_addr(struct ioat_channel *ioat, uint32_t index)
{
	return ioat->hw_ring_phys_addr +
	       ioat_get_ring_index(ioat, index) * sizeof(struct ioat_dma_hw_descriptor);
}

static void
ioat_submit_single(struct ioat_channel *ioat)
{
	ioat->head++;
}

static void
ioat_flush(struct ioat_channel *ioat)
{
	ioat->regs->dmacount = (uint16_t)ioat->head;
}

static struct ioat_descriptor *
ioat_prep_null(struct ioat_channel *ioat)
{
	struct ioat_descriptor *desc;
	struct ioat_dma_hw_descriptor *hw_desc;

	if (ioat_get_ring_space(ioat) < 1) {
		return NULL;
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->u.control_raw = 0;
	hw_desc->u.control.op = IOAT_OP_COPY;
	hw_desc->u.control.null = 1;
	hw_desc->u.control.completion_update = 1;

	hw_desc->size = 8;
	hw_desc->src_addr = 0;
	hw_desc->dest_addr = 0;

	desc->callback_fn = NULL;
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);

	return desc;
}

static struct ioat_descriptor *
ioat_prep_copy(struct ioat_channel *ioat, uint64_t dst,
	       uint64_t src, uint32_t len)
{
	struct ioat_descriptor *desc;
	struct ioat_dma_hw_descriptor *hw_desc;

	ioat_assert(len <= ioat->max_xfer_size);

	if (ioat_get_ring_space(ioat) < 1) {
		return NULL;
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->u.control_raw = 0;
	hw_desc->u.control.op = IOAT_OP_COPY;
	hw_desc->u.control.completion_update = 1;

	hw_desc->size = len;
	hw_desc->src_addr = src;
	hw_desc->dest_addr = dst;

	desc->callback_fn = NULL;
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);

	return desc;
}

static int ioat_reset_hw(struct ioat_channel *ioat)
{
	int timeout;
	uint64_t status;
	uint32_t chanerr;

	status = ioat_get_chansts(ioat);
	if (is_ioat_active(status) || is_ioat_idle(status)) {
		ioat_suspend(ioat);
	}

	timeout = 20; /* in milliseconds */
	while (is_ioat_active(status) || is_ioat_idle(status)) {
		ioat_delay_us(1000);
		timeout--;
		if (timeout == 0) {
			ioat_printf(ioat, "%s: timed out waiting for suspend\n", __func__);
			return -1;
		}
		status = ioat_get_chansts(ioat);
	}

	/*
	 * Clear any outstanding errors.
	 * CHANERR is write-1-to-clear, so write the current CHANERR bits back to reset everything.
	 */
	chanerr = ioat->regs->chanerr;
	ioat->regs->chanerr = chanerr;

	ioat_reset(ioat);

	timeout = 20;
	while (ioat_reset_pending(ioat)) {
		ioat_delay_us(1000);
		timeout--;
		if (timeout == 0) {
			ioat_printf(ioat, "%s: timed out waiting for reset\n", __func__);
			return -1;
		}
	}

	return 0;
}

static void
ioat_process_channel_events(struct ioat_channel *ioat)
{
	struct ioat_descriptor *desc;
	uint64_t status, completed_descriptor, hw_desc_phys_addr;
	uint32_t tail;

	if (ioat->head == ioat->tail) {
		return;
	}

	status = *ioat->comp_update;
	completed_descriptor = status & IOAT_CHANSTS_COMPLETED_DESCRIPTOR_MASK;

	if (is_ioat_halted(status)) {
		ioat_printf(ioat, "%s: Channel halted (%x)\n", __func__, ioat->regs->chanerr);
		/* TODO: report error */
		return;
	}

	if (completed_descriptor == ioat->last_seen) {
		return;
	}

	do {
		tail = ioat_get_ring_index(ioat, ioat->tail);
		desc = &ioat->ring[tail];

		if (desc->callback_fn) {
			desc->callback_fn(desc->callback_arg);
		}

		hw_desc_phys_addr = ioat_get_desc_phys_addr(ioat, ioat->tail);
		ioat->tail++;
	} while (hw_desc_phys_addr != completed_descriptor);

	ioat->last_seen = hw_desc_phys_addr;
}

static int
ioat_channel_destruct(struct ioat_channel *ioat)
{
	ioat_unmap_pci_bar(ioat);

	if (ioat->ring) {
		free(ioat->ring);
	}

	if (ioat->hw_ring) {
		ioat_free(ioat->hw_ring);
	}

	if (ioat->comp_update) {
		ioat_free((void *)ioat->comp_update);
		ioat->comp_update = NULL;
	}

	return 0;
}

static int
ioat_channel_start(struct ioat_channel *ioat)
{
	uint8_t xfercap, version;
	uint64_t status;
	int i, num_descriptors;
	uint64_t comp_update_bus_addr;

	if (ioat_map_pci_bar(ioat) != 0) {
		ioat_printf(ioat, "%s: ioat_map_pci_bar() failed\n", __func__);
		return -1;
	}

	version = ioat->regs->cbver;
	if (version < IOAT_VER_3_0) {
		ioat_printf(ioat, "%s: unsupported IOAT version %u.%u\n",
			    __func__, version >> 4, version & 0xF);
		return -1;
	}

	xfercap = ioat->regs->xfercap;

	/* Only bits [4:0] are valid. */
	xfercap &= 0x1f;
	if (xfercap == 0) {
		/* 0 means 4 GB max transfer size. */
		ioat->max_xfer_size = 1ULL << 32;
	} else if (xfercap < 12) {
		/* XFCERCAP must be at least 12 (4 KB) according to the spec. */
		ioat_printf(ioat, "%s: invalid XFERCAP value %u\n", __func__, xfercap);
		return -1;
	} else {
		ioat->max_xfer_size = 1U << xfercap;
	}

	ioat->comp_update = ioat_zmalloc(NULL, sizeof(*ioat->comp_update), IOAT_CHANCMP_ALIGN,
					 &comp_update_bus_addr);
	if (ioat->comp_update == NULL) {
		return -1;
	}

	ioat->ring_size_order = IOAT_DEFAULT_ORDER;

	num_descriptors = 1 << ioat->ring_size_order;

	ioat->ring = calloc(num_descriptors, sizeof(struct ioat_descriptor));
	if (!ioat->ring) {
		return -1;
	}

	ioat->hw_ring = ioat_zmalloc(NULL, num_descriptors * sizeof(struct ioat_dma_hw_descriptor), 64,
				     &ioat->hw_ring_phys_addr);
	if (!ioat->hw_ring) {
		return -1;
	}

	for (i = 0; i < num_descriptors; i++) {
		ioat->hw_ring[i].next = ioat_get_desc_phys_addr(ioat, i + 1);
	}

	ioat->head = 0;
	ioat->tail = 0;
	ioat->last_seen = 0;

	ioat_reset_hw(ioat);

	ioat->regs->chanctrl = IOAT_CHANCTRL_ANY_ERR_ABORT_EN;
	ioat_write_chancmp(ioat, comp_update_bus_addr);
	ioat_write_chainaddr(ioat, ioat->hw_ring_phys_addr);

	ioat_prep_null(ioat);
	ioat_flush(ioat);

	i = 100;
	while (i-- > 0) {
		ioat_delay_us(100);
		status = ioat_get_chansts(ioat);
		if (is_ioat_idle(status))
			break;
	}

	if (is_ioat_idle(status)) {
		ioat_process_channel_events(ioat);
	} else {
		ioat_printf(ioat, "%s: could not start channel: status = %p\n error = %#x\n",
			    __func__, (void *)status, ioat->regs->chanerr);
		return -1;
	}

	return 0;
}

struct ioat_channel *
ioat_attach(void *device)
{
	struct ioat_driver	*driver = &g_ioat_driver;
	struct ioat_channel 	*ioat;
	uint32_t cmd_reg;

	ioat = malloc(sizeof(struct ioat_channel));
	if (ioat == NULL) {
		return NULL;
	}

	/* Enable PCI busmaster. */
	ioat_pcicfg_read32(device, &cmd_reg, 4);
	cmd_reg |= 0x4;
	ioat_pcicfg_write32(device, cmd_reg, 4);

	ioat->device = device;

	if (ioat_channel_start(ioat) != 0) {
		ioat_channel_destruct(ioat);
		free(ioat);
		return NULL;
	}

	ioat_mutex_lock(&driver->lock);
	SLIST_INSERT_HEAD(&ioat_free_channels, ioat, next);
	ioat_mutex_unlock(&driver->lock);

	return ioat;
}

int
ioat_detach(struct ioat_channel *ioat)
{
	struct ioat_driver	*driver = &g_ioat_driver;

	/* ioat should be in the free list (not registered to a thread)
	 * when calling ioat_detach().
	 */
	ioat_mutex_lock(&driver->lock);
	SLIST_REMOVE(&ioat_free_channels, ioat, ioat_channel, next);
	ioat_mutex_unlock(&driver->lock);

	ioat_channel_destruct(ioat);
	free(ioat);

	return 0;
}

int
ioat_register_thread(void)
{
	struct ioat_driver	*driver = &g_ioat_driver;

	if (ioat_thread_channel) {
		ioat_printf(NULL, "%s: thread already registered\n", __func__);
		return -1;
	}

	ioat_mutex_lock(&driver->lock);

	ioat_thread_channel = SLIST_FIRST(&ioat_free_channels);
	if (ioat_thread_channel) {
		SLIST_REMOVE_HEAD(&ioat_free_channels, next);
	}

	ioat_mutex_unlock(&driver->lock);

	return ioat_thread_channel ? 0 : -1;
}

void
ioat_unregister_thread(void)
{
	struct ioat_driver	*driver = &g_ioat_driver;

	if (!ioat_thread_channel) {
		return;
	}

	ioat_mutex_lock(&driver->lock);

	SLIST_INSERT_HEAD(&ioat_free_channels, ioat_thread_channel, next);
	ioat_thread_channel = NULL;

	ioat_mutex_unlock(&driver->lock);
}

#define min(a, b) (((a)<(b))?(a):(b))

#define _2MB_PAGE(ptr)		((ptr) & ~(0x200000 - 1))
#define _2MB_OFFSET(ptr)	((ptr) &  (0x200000 - 1))

int64_t
ioat_submit_copy(void *cb_arg, ioat_callback_t cb_fn,
		 void *dst, const void *src, uint64_t nbytes)
{
	struct ioat_channel	*ioat;
	struct ioat_descriptor	*last_desc;
	uint64_t	remaining, op_size;
	uint64_t	vdst, vsrc;
	uint64_t	vdst_page, vsrc_page;
	uint64_t	pdst_page, psrc_page;
	uint32_t	orig_head;

	ioat = ioat_thread_channel;
	if (!ioat) {
		return -1;
	}

	orig_head = ioat->head;

	vdst = (uint64_t)dst;
	vsrc = (uint64_t)src;
	vsrc_page = _2MB_PAGE(vsrc);
	vdst_page = _2MB_PAGE(vdst);
	psrc_page = ioat_vtophys((void *)vsrc_page);
	pdst_page = ioat_vtophys((void *)vdst_page);

	remaining = nbytes;

	while (remaining) {
		op_size = remaining;
		op_size = min(op_size, (0x200000 - _2MB_OFFSET(vsrc)));
		op_size = min(op_size, (0x200000 - _2MB_OFFSET(vdst)));
		op_size = min(op_size, ioat->max_xfer_size);
		remaining -= op_size;

		last_desc = ioat_prep_copy(ioat,
					   pdst_page + _2MB_OFFSET(vdst),
					   psrc_page + _2MB_OFFSET(vsrc),
					   op_size);

		if (remaining == 0 || last_desc == NULL) {
			break;
		}

		vsrc += op_size;
		vdst += op_size;

		if (_2MB_PAGE(vsrc) != vsrc_page) {
			vsrc_page = _2MB_PAGE(vsrc);
			psrc_page = ioat_vtophys((void *)vsrc_page);
		}

		if (_2MB_PAGE(vdst) != vdst_page) {
			vdst_page = _2MB_PAGE(vdst);
			pdst_page = ioat_vtophys((void *)vdst_page);
		}
	}
	/* Issue null descriptor for null transfer */
	if (nbytes == 0) {
		last_desc = ioat_prep_null(ioat);
	}

	if (last_desc) {
		last_desc->callback_fn = cb_fn;
		last_desc->callback_arg = cb_arg;
	} else {
		/*
		 * Ran out of descriptors in the ring - reset head to leave things as they were
		 * in case we managed to fill out any descriptors.
		 */
		ioat->head = orig_head;
		return -1;
	}

	ioat_flush(ioat);
	return nbytes;
}

void ioat_process_events(void)
{
	if (!ioat_thread_channel) {
		return;
	}

	ioat_process_channel_events(ioat_thread_channel);
}
