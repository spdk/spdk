/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
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
static SLIST_HEAD(, spdk_ioat_chan) ioat_free_channels;

/** IOAT channel assigned to this thread (or NULL if not assigned yet). */
static __thread struct spdk_ioat_chan *ioat_thread_channel;

struct ioat_driver {
	ioat_mutex_t	lock;
	TAILQ_HEAD(, spdk_ioat_chan)	attached_chans;
};

static struct ioat_driver g_ioat_driver = {
	.lock = IOAT_MUTEX_INITIALIZER,
	.attached_chans = TAILQ_HEAD_INITIALIZER(g_ioat_driver.attached_chans),
};

static uint64_t
ioat_get_chansts(struct spdk_ioat_chan *ioat)
{
	return spdk_mmio_read_8(&ioat->regs->chansts);
}

static void
ioat_write_chancmp(struct spdk_ioat_chan *ioat, uint64_t addr)
{
	spdk_mmio_write_8(&ioat->regs->chancmp, addr);
}

static void
ioat_write_chainaddr(struct spdk_ioat_chan *ioat, uint64_t addr)
{
	spdk_mmio_write_8(&ioat->regs->chainaddr, addr);
}

static inline void
ioat_suspend(struct spdk_ioat_chan *ioat)
{
	ioat->regs->chancmd = SPDK_IOAT_CHANCMD_SUSPEND;
}

static inline void
ioat_reset(struct spdk_ioat_chan *ioat)
{
	ioat->regs->chancmd = SPDK_IOAT_CHANCMD_RESET;
}

static inline uint32_t
ioat_reset_pending(struct spdk_ioat_chan *ioat)
{
	uint8_t cmd;

	cmd = ioat->regs->chancmd;
	return (cmd & SPDK_IOAT_CHANCMD_RESET) == SPDK_IOAT_CHANCMD_RESET;
}

static int
ioat_map_pci_bar(struct spdk_ioat_chan *ioat)
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

	ioat->regs = (volatile struct spdk_ioat_registers *)addr;

	return 0;
}

static int
ioat_unmap_pci_bar(struct spdk_ioat_chan *ioat)
{
	int rc = 0;
	void *addr = (void *)ioat->regs;

	if (addr) {
		rc = ioat_pcicfg_unmap_bar(ioat->device, 0, addr);
	}
	return rc;
}


static inline uint32_t
ioat_get_active(struct spdk_ioat_chan *ioat)
{
	return (ioat->head - ioat->tail) & ((1 << ioat->ring_size_order) - 1);
}

static inline uint32_t
ioat_get_ring_space(struct spdk_ioat_chan *ioat)
{
	return (1 << ioat->ring_size_order) - ioat_get_active(ioat) - 1;
}

static uint32_t
ioat_get_ring_index(struct spdk_ioat_chan *ioat, uint32_t index)
{
	return index & ((1 << ioat->ring_size_order) - 1);
}

static void
ioat_get_ring_entry(struct spdk_ioat_chan *ioat, uint32_t index,
		    struct ioat_descriptor **desc,
		    union spdk_ioat_hw_desc **hw_desc)
{
	uint32_t i = ioat_get_ring_index(ioat, index);

	*desc = &ioat->ring[i];
	*hw_desc = &ioat->hw_ring[i];
}

static uint64_t
ioat_get_desc_phys_addr(struct spdk_ioat_chan *ioat, uint32_t index)
{
	return ioat->hw_ring_phys_addr +
	       ioat_get_ring_index(ioat, index) * sizeof(union spdk_ioat_hw_desc);
}

static void
ioat_submit_single(struct spdk_ioat_chan *ioat)
{
	ioat->head++;
}

static void
ioat_flush(struct spdk_ioat_chan *ioat)
{
	ioat->regs->dmacount = (uint16_t)ioat->head;
}

static struct ioat_descriptor *
ioat_prep_null(struct spdk_ioat_chan *ioat)
{
	struct ioat_descriptor *desc;
	union spdk_ioat_hw_desc *hw_desc;

	if (ioat_get_ring_space(ioat) < 1) {
		return NULL;
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->dma.u.control_raw = 0;
	hw_desc->dma.u.control.op = SPDK_IOAT_OP_COPY;
	hw_desc->dma.u.control.null = 1;
	hw_desc->dma.u.control.completion_update = 1;

	hw_desc->dma.size = 8;
	hw_desc->dma.src_addr = 0;
	hw_desc->dma.dest_addr = 0;

	desc->callback_fn = NULL;
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);

	return desc;
}

static struct ioat_descriptor *
ioat_prep_copy(struct spdk_ioat_chan *ioat, uint64_t dst,
	       uint64_t src, uint32_t len)
{
	struct ioat_descriptor *desc;
	union spdk_ioat_hw_desc *hw_desc;

	ioat_assert(len <= ioat->max_xfer_size);

	if (ioat_get_ring_space(ioat) < 1) {
		return NULL;
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->dma.u.control_raw = 0;
	hw_desc->dma.u.control.op = SPDK_IOAT_OP_COPY;
	hw_desc->dma.u.control.completion_update = 1;

	hw_desc->dma.size = len;
	hw_desc->dma.src_addr = src;
	hw_desc->dma.dest_addr = dst;

	desc->callback_fn = NULL;
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);

	return desc;
}

static struct ioat_descriptor *
ioat_prep_fill(struct spdk_ioat_chan *ioat, uint64_t dst,
	       uint64_t fill_pattern, uint32_t len)
{
	struct ioat_descriptor *desc;
	union spdk_ioat_hw_desc *hw_desc;

	ioat_assert(len <= ioat->max_xfer_size);

	if (ioat_get_ring_space(ioat) < 1) {
		return NULL;
	}

	ioat_get_ring_entry(ioat, ioat->head, &desc, &hw_desc);

	hw_desc->fill.u.control_raw = 0;
	hw_desc->fill.u.control.op = SPDK_IOAT_OP_FILL;
	hw_desc->fill.u.control.completion_update = 1;

	hw_desc->fill.size = len;
	hw_desc->fill.src_data = fill_pattern;
	hw_desc->fill.dest_addr = dst;

	desc->callback_fn = NULL;
	desc->callback_arg = NULL;

	ioat_submit_single(ioat);

	return desc;
}

static int ioat_reset_hw(struct spdk_ioat_chan *ioat)
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

static int
ioat_process_channel_events(struct spdk_ioat_chan *ioat)
{
	struct ioat_descriptor *desc;
	uint64_t status, completed_descriptor, hw_desc_phys_addr;
	uint32_t tail;

	if (ioat->head == ioat->tail) {
		return 0;
	}

	status = *ioat->comp_update;
	completed_descriptor = status & SPDK_IOAT_CHANSTS_COMPLETED_DESCRIPTOR_MASK;

	if (is_ioat_halted(status)) {
		ioat_printf(ioat, "%s: Channel halted (%x)\n", __func__, ioat->regs->chanerr);
		return -1;
	}

	if (completed_descriptor == ioat->last_seen) {
		return 0;
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
	return 0;
}

static int
ioat_channel_destruct(struct spdk_ioat_chan *ioat)
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
ioat_channel_start(struct spdk_ioat_chan *ioat)
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
	if (version < SPDK_IOAT_VER_3_0) {
		ioat_printf(ioat, "%s: unsupported IOAT version %u.%u\n",
			    __func__, version >> 4, version & 0xF);
		return -1;
	}

	/* Always support DMA copy */
	ioat->dma_capabilities = SPDK_IOAT_ENGINE_COPY_SUPPORTED;
	if (ioat->regs->dmacapability & SPDK_IOAT_DMACAP_BFILL)
		ioat->dma_capabilities |= SPDK_IOAT_ENGINE_FILL_SUPPORTED;
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

	ioat->comp_update = ioat_zmalloc(NULL, sizeof(*ioat->comp_update), SPDK_IOAT_CHANCMP_ALIGN,
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

	ioat->hw_ring = ioat_zmalloc(NULL, num_descriptors * sizeof(union spdk_ioat_hw_desc), 64,
				     &ioat->hw_ring_phys_addr);
	if (!ioat->hw_ring) {
		return -1;
	}

	for (i = 0; i < num_descriptors; i++) {
		ioat->hw_ring[i].generic.next = ioat_get_desc_phys_addr(ioat, i + 1);
	}

	ioat->head = 0;
	ioat->tail = 0;
	ioat->last_seen = 0;

	ioat_reset_hw(ioat);

	ioat->regs->chanctrl = SPDK_IOAT_CHANCTRL_ANY_ERR_ABORT_EN;
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

/* Caller must hold g_ioat_driver.lock */
static struct spdk_ioat_chan *
ioat_attach(void *device)
{
	struct spdk_ioat_chan 	*ioat;
	uint32_t cmd_reg;

	ioat = calloc(1, sizeof(struct spdk_ioat_chan));
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

	SLIST_INSERT_HEAD(&ioat_free_channels, ioat, next);

	return ioat;
}

struct ioat_enum_ctx {
	spdk_ioat_probe_cb probe_cb;
	spdk_ioat_attach_cb attach_cb;
	void *cb_ctx;
};

/* This function must only be called while holding g_ioat_driver.lock */
static int
ioat_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct ioat_enum_ctx *enum_ctx = ctx;
	struct spdk_ioat_chan *ioat;

	/* Verify that this device is not already attached */
	TAILQ_FOREACH(ioat, &g_ioat_driver.attached_chans, tailq) {
		/*
		 * NOTE: This assumes that the PCI abstraction layer will use the same device handle
		 *  across enumerations; we could compare by BDF instead if this is not true.
		 */
		if (pci_dev == ioat->device) {
			return 0;
		}
	}

	if (enum_ctx->probe_cb(enum_ctx->cb_ctx, pci_dev)) {
		/*
		 * Since I/OAT init is relatively quick, just perform the full init during probing.
		 *  If this turns out to be a bottleneck later, this can be changed to work like
		 *  NVMe with a list of devices to initialize in parallel.
		 */
		ioat = ioat_attach(pci_dev);
		if (ioat == NULL) {
			ioat_printf(NULL, "ioat_attach() failed\n");
			return -1;
		}

		TAILQ_INSERT_TAIL(&g_ioat_driver.attached_chans, ioat, tailq);

		enum_ctx->attach_cb(enum_ctx->cb_ctx, pci_dev, ioat);
	}

	return 0;
}

int
spdk_ioat_probe(void *cb_ctx, spdk_ioat_probe_cb probe_cb, spdk_ioat_attach_cb attach_cb)
{
	int rc;
	struct ioat_enum_ctx enum_ctx;

	ioat_mutex_lock(&g_ioat_driver.lock);

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.attach_cb = attach_cb;
	enum_ctx.cb_ctx = cb_ctx;

	rc = ioat_pci_enumerate(ioat_enum_cb, &enum_ctx);

	ioat_mutex_unlock(&g_ioat_driver.lock);

	return rc;
}

int
spdk_ioat_detach(struct spdk_ioat_chan *ioat)
{
	struct ioat_driver	*driver = &g_ioat_driver;

	/* ioat should be in the free list (not registered to a thread)
	 * when calling ioat_detach().
	 */
	ioat_mutex_lock(&driver->lock);
	SLIST_REMOVE(&ioat_free_channels, ioat, spdk_ioat_chan, next);
	TAILQ_REMOVE(&driver->attached_chans, ioat, tailq);
	ioat_mutex_unlock(&driver->lock);

	ioat_channel_destruct(ioat);
	free(ioat);

	return 0;
}

int
spdk_ioat_register_thread(void)
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
spdk_ioat_unregister_thread(void)
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
spdk_ioat_submit_copy(void *cb_arg, spdk_ioat_req_cb cb_fn,
		      void *dst, const void *src, uint64_t nbytes)
{
	struct spdk_ioat_chan	*ioat;
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

int64_t
spdk_ioat_submit_fill(void *cb_arg, spdk_ioat_req_cb cb_fn,
		      void *dst, uint64_t fill_pattern, uint64_t nbytes)
{
	struct spdk_ioat_chan	*ioat;
	struct ioat_descriptor	*last_desc = NULL;
	uint64_t	remaining, op_size;
	uint64_t	vdst;
	uint32_t	orig_head;

	ioat = ioat_thread_channel;
	if (!ioat) {
		return -1;
	}

	if (!(ioat->dma_capabilities & SPDK_IOAT_ENGINE_FILL_SUPPORTED)) {
		ioat_printf(ioat, "Channel does not support memory fill\n");
		return -1;
	}

	orig_head = ioat->head;

	vdst = (uint64_t)dst;
	remaining = nbytes;

	while (remaining) {
		op_size = remaining;
		op_size = min(op_size, ioat->max_xfer_size);
		remaining -= op_size;

		last_desc = ioat_prep_fill(ioat,
					   ioat_vtophys((void *)vdst),
					   fill_pattern,
					   op_size);

		if (remaining == 0 || last_desc == NULL) {
			break;
		}

		vdst += op_size;
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

uint32_t
spdk_ioat_get_dma_capabilities(void)
{
	struct spdk_ioat_chan	*ioat;

	ioat = ioat_thread_channel;
	if (!ioat) {
		return 0;
	}
	return ioat->dma_capabilities;
}

int
spdk_ioat_process_events(void)
{
	if (!ioat_thread_channel) {
		return -1;
	}

	return ioat_process_channel_events(ioat_thread_channel);
}
