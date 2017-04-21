/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   Copyright (c) 2017, IBM Corporation.
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

/*
 * NVMe over PCIe transport
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "nvme_internal.h"
#include "nvme_uevent.h"

/*
 * Number of completion queue entries to process before ringing the
 *  completion queue doorbell.
 */
#define NVME_MIN_COMPLETIONS	(1)
#define NVME_MAX_COMPLETIONS	(128)

#define NVME_ADMIN_ENTRIES	(128)

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(253)

#define NVME_MAX_PRP_LIST_ENTRIES	(506)

struct nvme_pcie_enum_ctx {
	spdk_nvme_probe_cb probe_cb;
	void *cb_ctx;
	struct spdk_pci_addr pci_addr;
	bool has_pci_addr;
};

/* PCIe transport extensions for spdk_nvme_ctrlr */
struct nvme_pcie_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers *regs;

	/** NVMe MMIO register size */
	uint64_t regs_size;

	/* BAR mapping address which contains controller memory buffer */
	void *cmb_bar_virt_addr;

	/* BAR physical address which contains controller memory buffer */
	uint64_t cmb_bar_phys_addr;

	/* Controller memory buffer size in Bytes */
	uint64_t cmb_size;

	/* Current offset of controller memory buffer */
	uint64_t cmb_current_offset;

	void *cmb_mem_register_addr;
	size_t cmb_mem_register_size;

	bool cmb_io_data_supported;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;

	/* Opaque handle to associated PCI device. */
	struct spdk_pci_device *devhandle;

	/* File descriptor returned from spdk_pci_device_claim().  Closed when ctrlr is detached. */
	int claim_fd;

	/* Flag to indicate the MMIO register has been remapped */
	bool is_remapped;
};

struct nvme_tracker {
	TAILQ_ENTRY(nvme_tracker)       tq_list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint16_t			rsvd1: 14;
	uint16_t			timed_out: 1;
	uint16_t			active: 1;

	uint32_t			rsvd2;

	/* The value of spdk_get_ticks() when the tracker was submitted to the hardware. */
	uint64_t			submit_tick;

	uint64_t			prp_sgl_bus_addr;

	union {
		uint64_t			prp[NVME_MAX_PRP_LIST_ENTRIES];
		struct spdk_nvme_sgl_descriptor	sgl[NVME_MAX_SGL_DESCRIPTORS];
	} u;
};
/*
 * struct nvme_tracker must be exactly 4K so that the prp[] array does not cross a page boundary
 * and so that there is no padding required to meet alignment requirements.
 */
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) == 4096, "nvme_tracker is not 4K");
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, u.sgl) & 7) == 0, "SGL must be Qword aligned");

/* PCIe transport extensions for spdk_nvme_qpair */
struct nvme_pcie_qpair {
	/* Submission queue tail doorbell */
	volatile uint32_t *sq_tdbl;

	/* Completion queue head doorbell */
	volatile uint32_t *cq_hdbl;

	/* Submission queue shadow tail doorbell */
	volatile uint32_t *sq_shadow_tdbl;

	/* Completion queue shadow head doorbell */
	volatile uint32_t *cq_shadow_hdbl;

	/* Submission queue event index */
	volatile uint32_t *sq_eventidx;

	/* Completion queue event index */
	volatile uint32_t *cq_eventidx;

	/* Submission queue */
	struct spdk_nvme_cmd *cmd;

	/* Completion queue */
	struct spdk_nvme_cpl *cpl;

	TAILQ_HEAD(, nvme_tracker) free_tr;
	TAILQ_HEAD(nvme_outstanding_tr_head, nvme_tracker) outstanding_tr;

	/* Array of trackers indexed by command ID. */
	struct nvme_tracker *tr;

	uint16_t num_entries;

	uint16_t max_completions_cap;

	uint16_t sq_tail;
	uint16_t cq_head;
	uint16_t sq_head;

	uint8_t phase;

	bool is_enabled;

	/*
	 * Base qpair structure.
	 * This is located after the hot data in this structure so that the important parts of
	 * nvme_pcie_qpair are in the same cache line.
	 */
	struct spdk_nvme_qpair qpair;

	/*
	 * Fields below this point should not be touched on the normal I/O path.
	 */

	bool sq_in_cmb;

	uint64_t cmd_bus_addr;
	uint64_t cpl_bus_addr;
};

static int nvme_pcie_ctrlr_attach(spdk_nvme_probe_cb probe_cb, void *cb_ctx,
				  struct spdk_pci_addr *pci_addr);
static int nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair);
static int nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair);

__thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr = NULL;
static volatile uint16_t g_signal_lock;
static bool g_sigset = false;
static int hotplug_fd = -1;

static void
nvme_sigbus_fault_sighandler(int signum, siginfo_t *info, void *ctx)
{
	void *map_address;

	if (!__sync_bool_compare_and_swap(&g_signal_lock, 0, 1)) {
		return;
	}

	assert(g_thread_mmio_ctrlr != NULL);

	if (!g_thread_mmio_ctrlr->is_remapped) {
		map_address = mmap((void *)g_thread_mmio_ctrlr->regs, g_thread_mmio_ctrlr->regs_size,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (map_address == MAP_FAILED) {
			SPDK_ERRLOG("mmap failed\n");
			g_signal_lock = 0;
			return;
		}
		memset(map_address, 0xFF, sizeof(struct spdk_nvme_registers));
		g_thread_mmio_ctrlr->regs = (volatile struct spdk_nvme_registers *)map_address;
		g_thread_mmio_ctrlr->is_remapped = true;
	}
	g_signal_lock = 0;
	return;
}

static void
nvme_pcie_ctrlr_setup_signal(void)
{
	struct sigaction sa;

	sa.sa_sigaction = nvme_sigbus_fault_sighandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, NULL);
}

static int
_nvme_pcie_hotplug_monitor(void *cb_ctx, spdk_nvme_probe_cb probe_cb,
			   spdk_nvme_remove_cb remove_cb)
{
	struct spdk_nvme_ctrlr *ctrlr, *tmp;
	struct spdk_uevent event;
	struct spdk_pci_addr pci_addr;
	union spdk_nvme_csts_register csts;

	while (spdk_get_uevent(hotplug_fd, &event) > 0) {
		if (event.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_UIO ||
		    event.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_VFIO) {
			if (event.action == SPDK_NVME_UEVENT_ADD) {
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "add nvme address: %s\n",
					      event.traddr);
				if (spdk_process_is_primary()) {
					if (!spdk_pci_addr_parse(&pci_addr, event.traddr)) {
						nvme_pcie_ctrlr_attach(probe_cb, cb_ctx, &pci_addr);
					}
				}
			} else if (event.action == SPDK_NVME_UEVENT_REMOVE) {
				struct spdk_nvme_transport_id trid;

				memset(&trid, 0, sizeof(trid));
				trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
				snprintf(trid.traddr, sizeof(trid.traddr), "%s", event.traddr);

				ctrlr = spdk_nvme_get_ctrlr_by_trid_unsafe(&trid);
				if (ctrlr == NULL) {
					return 0;
				}
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "remove nvme address: %s\n",
					      event.traddr);

				nvme_ctrlr_fail(ctrlr, true);

				/* get the user app to clean up and stop I/O */
				if (remove_cb) {
					nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
					remove_cb(cb_ctx, ctrlr);
					nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
				}
			}
		}
	}

	/* This is a work around for vfio-attached device hot remove detection. */
	TAILQ_FOREACH_SAFE(ctrlr, &g_spdk_nvme_driver->shared_attached_ctrlrs, tailq, tmp) {
		csts = spdk_nvme_ctrlr_get_regs_csts(ctrlr);
		if (csts.raw == 0xffffffffU) {
			nvme_ctrlr_fail(ctrlr, true);
			nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
			remove_cb(cb_ctx, ctrlr);
			nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
		}
	}
	return 0;
}

static inline struct nvme_pcie_ctrlr *
nvme_pcie_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE);
	return (struct nvme_pcie_ctrlr *)((uintptr_t)ctrlr - offsetof(struct nvme_pcie_ctrlr, ctrlr));
}

static inline struct nvme_pcie_qpair *
nvme_pcie_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_PCIE);
	return (struct nvme_pcie_qpair *)((uintptr_t)qpair - offsetof(struct nvme_pcie_qpair, qpair));
}

static volatile void *
nvme_pcie_reg_addr(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return (volatile void *)((uintptr_t)pctrlr->regs + offset);
}

int
nvme_pcie_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	g_thread_mmio_ctrlr = pctrlr;
	spdk_mmio_write_4(nvme_pcie_reg_addr(ctrlr, offset), value);
	g_thread_mmio_ctrlr = NULL;
	return 0;
}

int
nvme_pcie_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	g_thread_mmio_ctrlr = pctrlr;
	spdk_mmio_write_8(nvme_pcie_reg_addr(ctrlr, offset), value);
	g_thread_mmio_ctrlr = NULL;
	return 0;
}

int
nvme_pcie_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	assert(value != NULL);
	g_thread_mmio_ctrlr = pctrlr;
	*value = spdk_mmio_read_4(nvme_pcie_reg_addr(ctrlr, offset));
	g_thread_mmio_ctrlr = NULL;
	if (~(*value) == 0) {
		return -1;
	}

	return 0;
}

int
nvme_pcie_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	assert(value != NULL);
	g_thread_mmio_ctrlr = pctrlr;
	*value = spdk_mmio_read_8(nvme_pcie_reg_addr(ctrlr, offset));
	g_thread_mmio_ctrlr = NULL;
	if (~(*value) == 0) {
		return -1;
	}

	return 0;
}

static int
nvme_pcie_ctrlr_set_asq(struct nvme_pcie_ctrlr *pctrlr, uint64_t value)
{
	return nvme_pcie_ctrlr_set_reg_8(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, asq),
					 value);
}

static int
nvme_pcie_ctrlr_set_acq(struct nvme_pcie_ctrlr *pctrlr, uint64_t value)
{
	return nvme_pcie_ctrlr_set_reg_8(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, acq),
					 value);
}

static int
nvme_pcie_ctrlr_set_aqa(struct nvme_pcie_ctrlr *pctrlr, const union spdk_nvme_aqa_register *aqa)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw),
					 aqa->raw);
}

static int
nvme_pcie_ctrlr_get_cmbloc(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbloc_register *cmbloc)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbloc.raw),
					 &cmbloc->raw);
}

static int
nvme_pcie_ctrlr_get_cmbsz(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbsz_register *cmbsz)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
					 &cmbsz->raw);
}

uint32_t
nvme_pcie_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/*
	 * For commands requiring more than 2 PRP entries, one PRP will be
	 *  embedded in the command (prp1), and the rest of the PRP entries
	 *  will be in a list pointed to by the command (prp2).  This means
	 *  that real max number of PRP entries we support is 506+1, which
	 *  results in a max xfer size of 506*ctrlr->page_size.
	 */
	return NVME_MAX_PRP_LIST_ENTRIES * ctrlr->page_size;
}

uint16_t
nvme_pcie_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_SGL_DESCRIPTORS;
}

static void
nvme_pcie_ctrlr_map_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr;
	uint32_t bir;
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
	uint64_t size, unit_size, offset, bar_size, bar_phys_addr;
	uint64_t mem_register_start, mem_register_end;

	if (nvme_pcie_ctrlr_get_cmbsz(pctrlr, &cmbsz) ||
	    nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
		SPDK_ERRLOG("get registers failed\n");
		goto exit;
	}

	if (!cmbsz.bits.sz) {
		goto exit;
	}

	bir = cmbloc.bits.bir;
	/* Values 0 2 3 4 5 are valid for BAR */
	if (bir > 5 || bir == 1) {
		goto exit;
	}

	/* unit size for 4KB/64KB/1MB/16MB/256MB/4GB/64GB */
	unit_size = (uint64_t)1 << (12 + 4 * cmbsz.bits.szu);
	/* controller memory buffer size in Bytes */
	size = unit_size * cmbsz.bits.sz;
	/* controller memory buffer offset from BAR in Bytes */
	offset = unit_size * cmbloc.bits.ofst;

	rc = spdk_pci_device_map_bar(pctrlr->devhandle, bir, &addr,
				     &bar_phys_addr, &bar_size);
	if ((rc != 0) || addr == NULL) {
		goto exit;
	}

	if (offset > bar_size) {
		goto exit;
	}

	if (size > bar_size - offset) {
		goto exit;
	}

	pctrlr->cmb_bar_virt_addr = addr;
	pctrlr->cmb_bar_phys_addr = bar_phys_addr;
	pctrlr->cmb_size = size;
	pctrlr->cmb_current_offset = offset;

	if (!cmbsz.bits.sqs) {
		pctrlr->ctrlr.opts.use_cmb_sqs = false;
	}

	/* If only SQS is supported use legacy mapping */
	if (cmbsz.bits.sqs && !(cmbsz.bits.wds || cmbsz.bits.rds)) {
		return;
	}

	/* If CMB is less than 4MiB in size then abort CMB mapping */
	if (pctrlr->cmb_size < (1ULL << 22)) {
		goto exit;
	}

	mem_register_start = (((uintptr_t)pctrlr->cmb_bar_virt_addr + offset + 0x200000) & ~(0x200000 - 1));
	mem_register_end = ((uintptr_t)pctrlr->cmb_bar_virt_addr + offset + pctrlr->cmb_size);
	mem_register_end &= ~(uint64_t)(0x200000 - 1);
	pctrlr->cmb_mem_register_addr = (void *)mem_register_start;
	pctrlr->cmb_mem_register_size = mem_register_end - mem_register_start;

	rc = spdk_mem_register(pctrlr->cmb_mem_register_addr, pctrlr->cmb_mem_register_size);
	if (rc) {
		SPDK_ERRLOG("spdk_mem_register() failed\n");
		goto exit;
	}
	pctrlr->cmb_current_offset = mem_register_start - ((uint64_t)pctrlr->cmb_bar_virt_addr + offset);
	pctrlr->cmb_io_data_supported = true;

	return;
exit:
	pctrlr->cmb_bar_virt_addr = NULL;
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
	return;
}

static int
nvme_pcie_ctrlr_unmap_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	union spdk_nvme_cmbloc_register cmbloc;
	void *addr = pctrlr->cmb_bar_virt_addr;

	if (addr) {
		if (pctrlr->cmb_mem_register_addr) {
			spdk_mem_unregister(pctrlr->cmb_mem_register_addr, pctrlr->cmb_mem_register_size);
		}

		if (nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
			SPDK_ERRLOG("get_cmbloc() failed\n");
			return -EIO;
		}
		rc = spdk_pci_device_unmap_bar(pctrlr->devhandle, cmbloc.bits.bir, addr);
	}
	return rc;
}

static int
nvme_pcie_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t length, uint64_t aligned,
			  uint64_t *offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	uint64_t round_offset;

	round_offset = pctrlr->cmb_current_offset;
	round_offset = (round_offset + (aligned - 1)) & ~(aligned - 1);

	if (round_offset + length > pctrlr->cmb_size) {
		return -1;
	}

	*offset = round_offset;
	pctrlr->cmb_current_offset = round_offset + length;

	return 0;
}

void *
nvme_pcie_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	uint64_t offset;

	if (pctrlr->cmb_bar_virt_addr == NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "CMB not available\n");
		return NULL;
	}

	if (!pctrlr->cmb_io_data_supported) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "CMB doesn't support I/O data\n");
		return NULL;
	}

	if (nvme_pcie_ctrlr_alloc_cmb(ctrlr, size, 4, &offset) != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "%zu-byte CMB allocation failed\n", size);
		return NULL;
	}

	return pctrlr->cmb_bar_virt_addr + offset;
}

int
nvme_pcie_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	/*
	 * Do nothing for now.
	 * TODO: Track free space so buffers may be reused.
	 */
	return 0;
}

static int
nvme_pcie_ctrlr_allocate_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;

	rc = spdk_pci_device_map_bar(pctrlr->devhandle, 0, &addr,
				     &phys_addr, &size);
	pctrlr->regs = (volatile struct spdk_nvme_registers *)addr;
	if ((pctrlr->regs == NULL) || (rc != 0)) {
		SPDK_ERRLOG("nvme_pcicfg_map_bar failed with rc %d or bar %p\n",
			    rc, pctrlr->regs);
		return -1;
	}

	pctrlr->regs_size = size;
	nvme_pcie_ctrlr_map_cmb(pctrlr);

	return 0;
}

static int
nvme_pcie_ctrlr_free_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	void *addr = (void *)pctrlr->regs;

	if (pctrlr->ctrlr.is_removed) {
		return rc;
	}

	rc = nvme_pcie_ctrlr_unmap_cmb(pctrlr);
	if (rc != 0) {
		SPDK_ERRLOG("nvme_ctrlr_unmap_cmb failed with error code %d\n", rc);
		return -1;
	}

	if (addr) {
		/* NOTE: addr may have been remapped here. We're relying on DPDK to call
		 * munmap internally.
		 */
		rc = spdk_pci_device_unmap_bar(pctrlr->devhandle, 0, addr);
	}
	return rc;
}

static int
nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_qpair *pqpair;
	int rc;

	pqpair = spdk_dma_zmalloc(sizeof(*pqpair), 64, NULL);
	if (pqpair == NULL) {
		return -ENOMEM;
	}

	pqpair->num_entries = NVME_ADMIN_ENTRIES;

	ctrlr->adminq = &pqpair->qpair;

	rc = nvme_qpair_init(ctrlr->adminq,
			     0, /* qpair ID */
			     ctrlr,
			     SPDK_NVME_QPRIO_URGENT,
			     NVME_ADMIN_ENTRIES);
	if (rc != 0) {
		return rc;
	}

	return nvme_pcie_qpair_construct(ctrlr->adminq);
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
static int
pcie_nvme_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct spdk_nvme_transport_id trid = {};
	struct nvme_pcie_enum_ctx *enum_ctx = ctx;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_pci_addr pci_addr;

	pci_addr = spdk_pci_device_get_addr(pci_dev);

	trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &pci_addr);

	/* Verify that this controller is not already attached */
	ctrlr = spdk_nvme_get_ctrlr_by_trid_unsafe(&trid);
	if (ctrlr) {
		if (spdk_process_is_primary()) {
			/* Already attached */
			return 0;
		} else {
			return nvme_ctrlr_add_process(ctrlr, pci_dev);
		}
	}

	/* check whether user passes the pci_addr */
	if (enum_ctx->has_pci_addr &&
	    (spdk_pci_addr_compare(&pci_addr, &enum_ctx->pci_addr) != 0)) {
		return 1;
	}

	return nvme_ctrlr_probe(&trid, pci_dev,
				enum_ctx->probe_cb, enum_ctx->cb_ctx);
}

int
nvme_pcie_ctrlr_scan(const struct spdk_nvme_transport_id *trid,
		     void *cb_ctx,
		     spdk_nvme_probe_cb probe_cb,
		     spdk_nvme_remove_cb remove_cb,
		     bool direct_connect)
{
	struct nvme_pcie_enum_ctx enum_ctx = {};

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.cb_ctx = cb_ctx;

	if (strlen(trid->traddr) != 0) {
		if (spdk_pci_addr_parse(&enum_ctx.pci_addr, trid->traddr)) {
			return -1;
		}
		enum_ctx.has_pci_addr = true;
	}

	if (hotplug_fd < 0) {
		hotplug_fd = spdk_uevent_connect();
		if (hotplug_fd < 0) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "Failed to open uevent netlink socket\n");
		}
	} else {
		_nvme_pcie_hotplug_monitor(cb_ctx, probe_cb, remove_cb);
	}

	if (enum_ctx.has_pci_addr == false) {
		return spdk_pci_nvme_enumerate(pcie_nvme_enum_cb, &enum_ctx);
	} else {
		return spdk_pci_nvme_device_attach(pcie_nvme_enum_cb, &enum_ctx, &enum_ctx.pci_addr);
	}
}

static int
nvme_pcie_ctrlr_attach(spdk_nvme_probe_cb probe_cb, void *cb_ctx, struct spdk_pci_addr *pci_addr)
{
	struct nvme_pcie_enum_ctx enum_ctx;

	enum_ctx.probe_cb = probe_cb;
	enum_ctx.cb_ctx = cb_ctx;

	return spdk_pci_nvme_device_attach(pcie_nvme_enum_cb, &enum_ctx, pci_addr);
}

struct spdk_nvme_ctrlr *nvme_pcie_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct spdk_pci_device *pci_dev = devhandle;
	struct nvme_pcie_ctrlr *pctrlr;
	union spdk_nvme_cap_register cap;
	uint32_t cmd_reg;
	int rc, claim_fd;
	struct spdk_pci_id pci_id;
	struct spdk_pci_addr pci_addr;

	if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
		SPDK_ERRLOG("could not parse pci address\n");
		return NULL;
	}

	claim_fd = spdk_pci_device_claim(&pci_addr);
	if (claim_fd < 0) {
		SPDK_ERRLOG("could not claim device %s\n", trid->traddr);
		return NULL;
	}

	pctrlr = spdk_dma_zmalloc(sizeof(struct nvme_pcie_ctrlr), 64, NULL);
	if (pctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	pctrlr->is_remapped = false;
	pctrlr->ctrlr.is_removed = false;
	pctrlr->ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr->devhandle = devhandle;
	pctrlr->ctrlr.opts = *opts;
	pctrlr->claim_fd = claim_fd;
	memcpy(&pctrlr->ctrlr.trid, trid, sizeof(pctrlr->ctrlr.trid));

	rc = nvme_pcie_ctrlr_allocate_bars(pctrlr);
	if (rc != 0) {
		spdk_dma_free(pctrlr);
		return NULL;
	}

	/* Enable PCI busmaster and disable INTx */
	spdk_pci_device_cfg_read32(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x404;
	spdk_pci_device_cfg_write32(pci_dev, cmd_reg, 4);

	if (nvme_ctrlr_get_cap(&pctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		spdk_dma_free(pctrlr);
		return NULL;
	}

	nvme_ctrlr_init_cap(&pctrlr->ctrlr, &cap);

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	rc = nvme_ctrlr_construct(&pctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		return NULL;
	}

	pci_id = spdk_pci_device_get_id(pci_dev);
	pctrlr->ctrlr.quirks = nvme_get_quirks(&pci_id);

	rc = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		return NULL;
	}

	/* Construct the primary process properties */
	rc = nvme_ctrlr_add_process(&pctrlr->ctrlr, pci_dev);
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		return NULL;
	}

	if (g_sigset != true) {
		nvme_pcie_ctrlr_setup_signal();
		g_sigset = true;
	}

	return &pctrlr->ctrlr;
}

int
nvme_pcie_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair *padminq = nvme_pcie_qpair(ctrlr->adminq);
	union spdk_nvme_aqa_register aqa;

	if (nvme_pcie_ctrlr_set_asq(pctrlr, padminq->cmd_bus_addr)) {
		SPDK_ERRLOG("set_asq() failed\n");
		return -EIO;
	}

	if (nvme_pcie_ctrlr_set_acq(pctrlr, padminq->cpl_bus_addr)) {
		SPDK_ERRLOG("set_acq() failed\n");
		return -EIO;
	}

	aqa.raw = 0;
	/* acqs and asqs are 0-based. */
	aqa.bits.acqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;
	aqa.bits.asqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;

	if (nvme_pcie_ctrlr_set_aqa(pctrlr, &aqa)) {
		SPDK_ERRLOG("set_aqa() failed\n");
		return -EIO;
	}

	return 0;
}

int
nvme_pcie_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct spdk_pci_device *devhandle = nvme_ctrlr_proc_get_devhandle(ctrlr);

	close(pctrlr->claim_fd);

	if (ctrlr->adminq) {
		nvme_pcie_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_free_processes(ctrlr);

	nvme_pcie_ctrlr_free_bars(pctrlr);

	if (devhandle) {
		spdk_pci_device_detach(devhandle);
	}

	spdk_dma_free(pctrlr);

	return 0;
}

static void
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid, uint64_t phys_addr)
{
	tr->prp_sgl_bus_addr = phys_addr + offsetof(struct nvme_tracker, u.prp);
	tr->cid = cid;
	tr->active = false;
}

int
nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->sq_tail = pqpair->cq_head = 0;

	/*
	 * First time through the completion queue, HW will set phase
	 *  bit on completions to 1.  So set this to 1 here, indicating
	 *  we're looking for a 1 to know which entries have completed.
	 *  we'll toggle the bit each time when the completion queue
	 *  rolls over.
	 */
	pqpair->phase = 1;

	memset(pqpair->cmd, 0,
	       pqpair->num_entries * sizeof(struct spdk_nvme_cmd));
	memset(pqpair->cpl, 0,
	       pqpair->num_entries * sizeof(struct spdk_nvme_cpl));

	return 0;
}

static int
nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	uint16_t		i;
	volatile uint32_t	*doorbell_base;
	uint64_t		phys_addr = 0;
	uint64_t		offset;
	uint16_t		num_trackers;
	size_t 			page_size = sysconf(_SC_PAGESIZE);

	/*
	 * Limit the maximum number of completions to return per call to prevent wraparound,
	 * and calculate how many trackers can be submitted at once without overflowing the
	 * completion queue.
	 */
	pqpair->max_completions_cap = pqpair->num_entries / 4;
	pqpair->max_completions_cap = spdk_max(pqpair->max_completions_cap, NVME_MIN_COMPLETIONS);
	pqpair->max_completions_cap = spdk_min(pqpair->max_completions_cap, NVME_MAX_COMPLETIONS);
	num_trackers = pqpair->num_entries - pqpair->max_completions_cap;

	SPDK_INFOLOG(SPDK_LOG_NVME, "max_completions_cap = %" PRIu16 " num_trackers = %" PRIu16 "\n",
		     pqpair->max_completions_cap, num_trackers);

	assert(num_trackers != 0);

	pqpair->sq_in_cmb = false;

	/* cmd and cpl rings must be aligned on page size boundaries. */
	if (ctrlr->opts.use_cmb_sqs) {
		if (nvme_pcie_ctrlr_alloc_cmb(ctrlr, pqpair->num_entries * sizeof(struct spdk_nvme_cmd),
					      page_size, &offset) == 0) {
			pqpair->cmd = pctrlr->cmb_bar_virt_addr + offset;
			pqpair->cmd_bus_addr = pctrlr->cmb_bar_phys_addr + offset;
			pqpair->sq_in_cmb = true;
		}
	}
	if (pqpair->sq_in_cmb == false) {
		pqpair->cmd = spdk_dma_zmalloc(pqpair->num_entries * sizeof(struct spdk_nvme_cmd),
					       page_size,
					       &pqpair->cmd_bus_addr);
		if (pqpair->cmd == NULL) {
			SPDK_ERRLOG("alloc qpair_cmd failed\n");
			return -ENOMEM;
		}
	}

	pqpair->cpl = spdk_dma_zmalloc(pqpair->num_entries * sizeof(struct spdk_nvme_cpl),
				       page_size,
				       &pqpair->cpl_bus_addr);
	if (pqpair->cpl == NULL) {
		SPDK_ERRLOG("alloc qpair_cpl failed\n");
		return -ENOMEM;
	}

	doorbell_base = &pctrlr->regs->doorbell[0].sq_tdbl;
	pqpair->sq_tdbl = doorbell_base + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
	pqpair->cq_hdbl = doorbell_base + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;

	/*
	 * Reserve space for all of the trackers in a single allocation.
	 *   struct nvme_tracker must be padded so that its size is already a power of 2.
	 *   This ensures the PRP list embedded in the nvme_tracker object will not span a
	 *   4KB boundary, while allowing access to trackers in tr[] via normal array indexing.
	 */
	pqpair->tr = spdk_dma_zmalloc(num_trackers * sizeof(*tr), sizeof(*tr), &phys_addr);
	if (pqpair->tr == NULL) {
		SPDK_ERRLOG("nvme_tr failed\n");
		return -ENOMEM;
	}

	TAILQ_INIT(&pqpair->free_tr);
	TAILQ_INIT(&pqpair->outstanding_tr);

	for (i = 0; i < num_trackers; i++) {
		tr = &pqpair->tr[i];
		nvme_qpair_construct_tracker(tr, i, phys_addr);
		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
		phys_addr += sizeof(struct nvme_tracker);
	}

	nvme_pcie_qpair_reset(qpair);

	return 0;
}

static inline void
nvme_pcie_copy_command(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	/* dst and src are known to be non-overlapping and 64-byte aligned. */
#if defined(__AVX__)
	__m256i *d256 = (__m256i *)dst;
	const __m256i *s256 = (const __m256i *)src;

	_mm256_store_si256(&d256[0], _mm256_load_si256(&s256[0]));
	_mm256_store_si256(&d256[1], _mm256_load_si256(&s256[1]));
#elif defined(__SSE2__)
	__m128i *d128 = (__m128i *)dst;
	const __m128i *s128 = (const __m128i *)src;

	_mm_store_si128(&d128[0], _mm_load_si128(&s128[0]));
	_mm_store_si128(&d128[1], _mm_load_si128(&s128[1]));
	_mm_store_si128(&d128[2], _mm_load_si128(&s128[2]));
	_mm_store_si128(&d128[3], _mm_load_si128(&s128[3]));
#else
	*dst = *src;
#endif
}

/**
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_pcie_qpair_insert_pending_admin_request(struct spdk_nvme_qpair *qpair,
		struct nvme_request *req, struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	struct nvme_request		*active_req = req;
	struct spdk_nvme_ctrlr_process	*active_proc;
	bool				pending_on_proc = false;

	/*
	 * The admin request is from another process. Move to the per
	 *  process list for that process to handle it later.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));
	assert(active_req->pid != getpid());

	TAILQ_FOREACH(active_proc, &ctrlr->active_procs, tailq) {
		if (active_proc->pid == active_req->pid) {
			/* Saved the original completion information */
			memcpy(&active_req->cpl, cpl, sizeof(*cpl));
			STAILQ_INSERT_TAIL(&active_proc->active_reqs, active_req, stailq);
			pending_on_proc = true;

			break;
		}
	}

	if (pending_on_proc == false) {
		SPDK_ERRLOG("The owning process (pid %d) is not found. Drop the request.\n",
			    active_req->pid);

		nvme_free_request(active_req);
	}
}

/**
 * Note: the ctrlr_lock must be held when calling this function.
 */
static void
nvme_pcie_qpair_complete_pending_admin_request(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	struct nvme_request		*req, *tmp_req;
	bool				proc_found = false;
	pid_t				pid = getpid();
	struct spdk_nvme_ctrlr_process	*proc;

	/*
	 * Check whether there is any pending admin request from
	 * other active processes.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));

	TAILQ_FOREACH(proc, &ctrlr->active_procs, tailq) {
		if (proc->pid == pid) {
			proc_found = true;

			break;
		}
	}

	if (proc_found == false) {
		SPDK_ERRLOG("the active process (pid %d) is not found for this controller.\n", pid);
		assert(proc_found);
	}

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == pid);

		if (req->cb_fn) {
			req->cb_fn(req->cb_arg, &req->cpl);
		}

		nvme_free_request(req);
	}
}

static inline int
nvme_pcie_qpair_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old)
{
	return (uint16_t)(new_idx - event_idx) <= (uint16_t)(new_idx - old);
}

static bool
nvme_pcie_qpair_update_mmio_required(struct spdk_nvme_qpair *qpair, uint16_t value,
				     volatile uint32_t *shadow_db,
				     volatile uint32_t *eventidx)
{
	uint16_t old;

	if (!shadow_db) {
		return true;
	}

	old = *shadow_db;
	*shadow_db = value;

	if (!nvme_pcie_qpair_need_event(*eventidx, value, old)) {
		return false;
	}

	return true;
}

static void
nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);

	tr->timed_out = 0;
	if (spdk_unlikely(qpair->ctrlr->timeout_cb_fn != NULL)) {
		tr->submit_tick = spdk_get_ticks();
	}

	req = tr->req;
	pqpair->tr[tr->cid].active = true;

	/* Copy the command from the tracker to the submission queue. */
	nvme_pcie_copy_command(&pqpair->cmd[pqpair->sq_tail], &req->cmd);

	if (++pqpair->sq_tail == pqpair->num_entries) {
		pqpair->sq_tail = 0;
	}

	if (pqpair->sq_tail == pqpair->sq_head) {
		SPDK_ERRLOG("sq_tail is passing sq_head!\n");
	}

	spdk_wmb();
	g_thread_mmio_ctrlr = pctrlr;
	if (spdk_likely(nvme_pcie_qpair_update_mmio_required(qpair,
			pqpair->sq_tail,
			pqpair->sq_shadow_tdbl,
			pqpair->sq_eventidx))) {
		spdk_mmio_write_4(pqpair->sq_tdbl, pqpair->sq_tail);
	}
	g_thread_mmio_ctrlr = NULL;
}

static void
nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_pcie_qpair		*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_request		*req;
	bool				retry, error, was_active;
	bool				req_from_current_proc = true;

	req = tr->req;

	assert(req != NULL);

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < spdk_nvme_retry_count;

	if (error && print_on_error) {
		nvme_qpair_print_command(qpair, &req->cmd);
		nvme_qpair_print_completion(qpair, cpl);
	}

	was_active = pqpair->tr[cpl->cid].active;
	pqpair->tr[cpl->cid].active = false;

	assert(cpl->cid == req->cmd.cid);

	if (retry) {
		req->retries++;
		nvme_pcie_qpair_submit_tracker(qpair, tr);
	} else {
		if (was_active) {
			/* Only check admin requests from different processes. */
			if (nvme_qpair_is_admin_queue(qpair) && req->pid != getpid()) {
				req_from_current_proc = false;
				nvme_pcie_qpair_insert_pending_admin_request(qpair, req, cpl);
			} else {
				if (req->cb_fn) {
					req->cb_fn(req->cb_arg, cpl);
				}
			}
		}

		if (req_from_current_proc == true) {
			nvme_free_request(req);
		}

		tr->req = NULL;

		TAILQ_REMOVE(&pqpair->outstanding_tr, tr, tq_list);
		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);

		/*
		 * If the controller is in the middle of resetting, don't
		 *  try to submit queued requests here - let the reset logic
		 *  handle that instead.
		 */
		if (!STAILQ_EMPTY(&qpair->queued_req) &&
		    !qpair->ctrlr->is_resetting) {
			req = STAILQ_FIRST(&qpair->queued_req);
			STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
			nvme_qpair_submit_request(qpair, req);
		}
	}
}

static void
nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
					struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
					bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.cid = tr->cid;
	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.status.dnr = dnr;
	nvme_pcie_qpair_complete_tracker(qpair, tr, &cpl, print_on_error);
}

static void
nvme_pcie_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *temp;

	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, temp) {
		SPDK_ERRLOG("aborting outstanding command\n");
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, dnr, true);
	}
}

static void
nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;

	tr = TAILQ_FIRST(&pqpair->outstanding_tr);
	while (tr != NULL) {
		assert(tr->req != NULL);
		if (tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			nvme_pcie_qpair_manual_complete_tracker(qpair, tr,
								SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, 0,
								false);
			tr = TAILQ_FIRST(&pqpair->outstanding_tr);
		} else {
			tr = TAILQ_NEXT(tr, tq_list);
		}
	}
}

static void
nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

static int
nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_pcie_admin_qpair_destroy(qpair);
	}
	if (pqpair->cmd && !pqpair->sq_in_cmb) {
		spdk_dma_free(pqpair->cmd);
	}
	if (pqpair->cpl) {
		spdk_dma_free(pqpair->cpl);
	}
	if (pqpair->tr) {
		spdk_dma_free(pqpair->tr);
	}

	spdk_dma_free(pqpair);

	return 0;
}

static void
nvme_pcie_admin_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	/*
	 * Manually abort each outstanding admin command.  Do not retry
	 *  admin commands found here, since they will be left over from
	 *  a controller reset and its likely the context in which the
	 *  command was issued no longer applies.
	 */
	nvme_pcie_qpair_abort_trackers(qpair, 1 /* do not retry */);
}

static void
nvme_pcie_io_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	/* Manually abort each outstanding I/O. */
	nvme_pcie_qpair_abort_trackers(qpair, 0);
}

int
nvme_pcie_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->is_enabled = true;
	if (nvme_qpair_is_io_queue(qpair)) {
		nvme_pcie_io_qpair_enable(qpair);
	} else {
		nvme_pcie_admin_qpair_enable(qpair);
	}

	return 0;
}

static void
nvme_pcie_admin_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

static void
nvme_pcie_io_qpair_disable(struct spdk_nvme_qpair *qpair)
{
}

int
nvme_pcie_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->is_enabled = false;
	if (nvme_qpair_is_io_queue(qpair)) {
		nvme_pcie_io_qpair_disable(qpair);
	} else {
		nvme_pcie_admin_qpair_disable(qpair);
	}

	return 0;
}


int
nvme_pcie_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_qpair_abort_trackers(qpair, 1 /* do not retry */);

	return 0;
}

static int
nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_CQ;

	/*
	 * TODO: create a create io completion queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((pqpair->num_entries - 1) << 16) | io_que->id;
	/*
	 * 0x2 = interrupts enabled
	 * 0x1 = physically contiguous
	 */
	cmd->cdw11 = 0x1;
	cmd->dptr.prp.prp1 = pqpair->cpl_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_SQ;

	/*
	 * TODO: create a create io submission queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((pqpair->num_entries - 1) << 16) | io_que->id;
	/* 0x1 = physically contiguous */
	cmd->cdw11 = (io_que->id << 16) | (io_que->qprio << 1) | 0x1;
	cmd->dptr.prp.prp1 = pqpair->cmd_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd->cdw10 = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd->cdw10 = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
_nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 uint16_t qid)
{
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_completion_poll_status	status;
	int					rc;

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_create_io_cq failed!\n");
		return -1;
	}

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_create_io_sq failed!\n");
		/* Attempt to delete the completion queue */
		status.done = false;
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
		if (rc != 0) {
			return -1;
		}
		while (status.done == false) {
			spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		}
		return -1;
	}

	if (ctrlr->shadow_doorbell) {
		pqpair->sq_shadow_tdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
		pqpair->cq_shadow_hdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;
		pqpair->sq_eventidx = ctrlr->eventidx + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
		pqpair->cq_eventidx = ctrlr->eventidx + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;
	}
	nvme_pcie_qpair_reset(qpair);

	return 0;
}

struct spdk_nvme_qpair *
nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	struct nvme_pcie_qpair *pqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	assert(ctrlr != NULL);

	pqpair = spdk_dma_zmalloc(sizeof(*pqpair), 64, NULL);
	if (pqpair == NULL) {
		return NULL;
	}

	pqpair->num_entries = opts->io_queue_size;

	qpair = &pqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, opts->qprio, opts->io_queue_requests);
	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	rc = nvme_pcie_qpair_construct(qpair);
	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	rc = _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qid);

	if (rc != 0) {
		SPDK_ERRLOG("I/O queue creation failed\n");
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

int
nvme_pcie_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qpair->id);
}

int
nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status status;
	int rc;

	assert(ctrlr != NULL);

	if (ctrlr->is_removed) {
		goto free;
	}

	/* Delete the I/O submission queue */
	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		return -1;
	}

	if (qpair->no_deletion_notification_needed == 0) {
		/* Complete any I/O in the completion queue */
		nvme_pcie_qpair_process_completions(qpair, 0);

		/* Abort the rest of the I/O */
		nvme_pcie_qpair_abort_trackers(qpair, 1);
	}

	/* Delete the completion queue */
	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		return -1;
	}

free:
	nvme_pcie_qpair_destroy(qpair);
	return 0;
}

static void
nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	/*
	 * Bad vtophys translation, so abort this request and return
	 *  immediately.
	 */
	nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						SPDK_NVME_SC_INVALID_FIELD,
						1 /* do not retry */, true);
}

/*
 * Append PRP list entries to describe a virtually contiguous buffer starting at virt_addr of len bytes.
 *
 * *prp_index will be updated to account for the number of PRP entries used.
 */
static int
nvme_pcie_prp_list_append(struct nvme_tracker *tr, uint32_t *prp_index, void *virt_addr, size_t len,
			  uint32_t page_size)
{
	struct spdk_nvme_cmd *cmd = &tr->req->cmd;
	uintptr_t page_mask = page_size - 1;
	uint64_t phys_addr;
	uint32_t i;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "prp_index:%u virt_addr:%p len:%u\n",
		      *prp_index, virt_addr, (uint32_t)len);

	if (spdk_unlikely(((uintptr_t)virt_addr & 3) != 0)) {
		SPDK_ERRLOG("virt_addr %p not dword aligned\n", virt_addr);
		return -EINVAL;
	}

	i = *prp_index;
	while (len) {
		uint32_t seg_len;

		/*
		 * prp_index 0 is stored in prp1, and the rest are stored in the prp[] array,
		 * so prp_index == count is valid.
		 */
		if (spdk_unlikely(i > SPDK_COUNTOF(tr->u.prp))) {
			SPDK_ERRLOG("out of PRP entries\n");
			return -EINVAL;
		}

		phys_addr = spdk_vtophys(virt_addr);
		if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR)) {
			SPDK_ERRLOG("vtophys(%p) failed\n", virt_addr);
			return -EINVAL;
		}

		if (i == 0) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "prp1 = %p\n", (void *)phys_addr);
			cmd->dptr.prp.prp1 = phys_addr;
			seg_len = page_size - ((uintptr_t)virt_addr & page_mask);
		} else {
			if ((phys_addr & page_mask) != 0) {
				SPDK_ERRLOG("PRP %u not page aligned (%p)\n", i, virt_addr);
				return -EINVAL;
			}

			SPDK_DEBUGLOG(SPDK_LOG_NVME, "prp[%u] = %p\n", i - 1, (void *)phys_addr);
			tr->u.prp[i - 1] = phys_addr;
			seg_len = page_size;
		}

		seg_len = spdk_min(seg_len, len);
		virt_addr += seg_len;
		len -= seg_len;
		i++;
	}

	cmd->psdt = SPDK_NVME_PSDT_PRP;
	if (i <= 1) {
		cmd->dptr.prp.prp2 = 0;
	} else if (i == 2) {
		cmd->dptr.prp.prp2 = tr->u.prp[0];
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "prp2 = %p\n", (void *)cmd->dptr.prp.prp2);
	} else {
		cmd->dptr.prp.prp2 = tr->prp_sgl_bus_addr;
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "prp2 = %p (PRP list)\n", (void *)cmd->dptr.prp.prp2);
	}

	*prp_index = i;
	return 0;
}

/**
 * Build PRP list describing physically contiguous payload buffer.
 */
static int
nvme_pcie_qpair_build_contig_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr)
{
	uint32_t prp_index = 0;
	int rc;

	rc = nvme_pcie_prp_list_append(tr, &prp_index, req->payload.u.contig + req->payload_offset,
				       req->payload_size, qpair->ctrlr->page_size);
	if (rc) {
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
		return rc;
	}

	return 0;
}

/**
 * Build SGL list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr)
{
	int rc;
	void *virt_addr;
	uint64_t phys_addr;
	uint32_t remaining_transfer_len, length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload_size != 0);
	assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.u.sgl.reset_sgl_fn != NULL);
	assert(req->payload.u.sgl.next_sge_fn != NULL);
	req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);

	sgl = tr->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	remaining_transfer_len = req->payload_size;

	while (remaining_transfer_len > 0) {
		if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg, &virt_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		phys_addr = spdk_vtophys(virt_addr);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		length = spdk_min(remaining_transfer_len, length);
		remaining_transfer_len -= length;

		sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl->unkeyed.length = length;
		sgl->address = phys_addr;
		sgl->unkeyed.subtype = 0;

		sgl++;
		nseg++;
	}

	if (nseg == 1) {
		/*
		 * The whole transfer can be described by a single SGL descriptor.
		 *  Use the special case described by the spec where SGL1's type is Data Block.
		 *  This means the SGL in the tracker is not used at all, so copy the first (and only)
		 *  SGL element into SGL1.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.address = tr->u.sgl[0].address;
		req->cmd.dptr.sgl1.unkeyed.length = tr->u.sgl[0].unkeyed.length;
	} else {
		/* For now we can only support 1 SGL segment in NVMe controller */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	return 0;
}

/**
 * Build PRP list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_prps_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				       struct nvme_tracker *tr)
{
	int rc;
	void *virt_addr;
	uint32_t remaining_transfer_len, length;
	uint32_t prp_index = 0;
	uint32_t page_size = qpair->ctrlr->page_size;

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.u.sgl.reset_sgl_fn != NULL);
	req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);

	remaining_transfer_len = req->payload_size;
	while (remaining_transfer_len > 0) {
		assert(req->payload.u.sgl.next_sge_fn != NULL);
		rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg, &virt_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		length = spdk_min(remaining_transfer_len, length);

		/*
		 * Any incompatible sges should have been handled up in the splitting routine,
		 *  but assert here as an additional check.
		 *
		 * All SGEs except last must end on a page boundary.
		 */
		assert((length == remaining_transfer_len) ||
		       _is_page_aligned((uintptr_t)virt_addr + length, page_size));

		rc = nvme_pcie_prp_list_append(tr, &prp_index, virt_addr, length, page_size);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return rc;
		}

		remaining_transfer_len -= length;
	}

	return 0;
}

static inline bool
nvme_pcie_qpair_check_enabled(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (!pqpair->is_enabled &&
	    !qpair->ctrlr->is_resetting) {
		nvme_qpair_enable(qpair);
	}
	return pqpair->is_enabled;
}

int
nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_tracker	*tr;
	int			rc = 0;
	void			*md_payload;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);

	nvme_pcie_qpair_check_enabled(qpair);

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	}

	tr = TAILQ_FIRST(&pqpair->free_tr);

	if (tr == NULL || !pqpair->is_enabled) {
		/*
		 * No tracker is available, or the qpair is disabled due to
		 *  an in-progress controller-level reset.
		 *
		 * Put the request on the qpair's request queue to be
		 *  processed when a tracker frees up via a command
		 *  completion or when the controller reset is
		 *  completed.
		 */
		STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
		goto exit;
	}

	TAILQ_REMOVE(&pqpair->free_tr, tr, tq_list); /* remove tr from free_tr */
	TAILQ_INSERT_TAIL(&pqpair->outstanding_tr, tr, tq_list);
	tr->req = req;
	req->cmd.cid = tr->cid;

	if (req->payload_size && req->payload.md) {
		md_payload = req->payload.md + req->md_offset;
		tr->req->cmd.mptr = spdk_vtophys(md_payload);
		if (tr->req->cmd.mptr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			rc = -EINVAL;
			goto exit;
		}
	}

	if (req->payload_size == 0) {
		/* Null payload - leave PRP fields zeroed */
		rc = 0;
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_pcie_qpair_build_contig_request(qpair, req, tr);
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_SGL) {
		if (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
			rc = nvme_pcie_qpair_build_hw_sgl_request(qpair, req, tr);
		} else {
			rc = nvme_pcie_qpair_build_prps_sgl_request(qpair, req, tr);
		}
	} else {
		assert(0);
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
		rc = -EINVAL;
	}

	if (rc < 0) {
		goto exit;
	}

	nvme_pcie_qpair_submit_tracker(qpair, tr);

exit:
	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}

	return rc;
}

static void
nvme_pcie_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct nvme_tracker *tr, *tmp;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	/* We don't want to expose the admin queue to the user,
	 * so when we're timing out admin commands set the
	 * qpair to NULL.
	 */
	if (qpair == ctrlr->adminq) {
		qpair = NULL;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
		if (tr->timed_out) {
			continue;
		}

		if (qpair == NULL &&
		    tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;
		}

		if (tr->submit_tick + ctrlr->timeout_ticks > t02) {
			/* The trackers are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}

		tr->timed_out = 1;
		ctrlr->timeout_cb_fn(ctrlr->timeout_cb_arg, ctrlr, qpair, tr->cid);
	}
}

int32_t
nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	struct nvme_tracker	*tr;
	struct spdk_nvme_cpl	*cpl;
	uint32_t		 num_completions = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;

	if (spdk_unlikely(!nvme_pcie_qpair_check_enabled(qpair))) {
		/*
		 * qpair is not enabled, likely because a controller reset is
		 *  is in progress.  Ignore the interrupt - any I/O that was
		 *  associated with this interrupt will get retried when the
		 *  reset is complete.
		 */
		return 0;
	}

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	}

	if (max_completions == 0 || max_completions > pqpair->max_completions_cap) {
		/*
		 * max_completions == 0 means unlimited, but complete at most
		 * max_completions_cap batch of I/O at a time so that the completion
		 * queue doorbells don't wrap around.
		 */
		max_completions = pqpair->max_completions_cap;
	}

	while (1) {
		cpl = &pqpair->cpl[pqpair->cq_head];

		if (cpl->status.p != pqpair->phase) {
			break;
		}
#ifdef __PPC64__
		/*
		 * This memory barrier prevents reordering of:
		 * - load after store from/to tr
		 * - load after load cpl phase and cpl cid
		 */
		spdk_mb();
#endif

		tr = &pqpair->tr[cpl->cid];
		pqpair->sq_head = cpl->sqhd;

		if (tr->active) {
			nvme_pcie_qpair_complete_tracker(qpair, tr, cpl, true);
		} else {
			SPDK_ERRLOG("cpl does not map to outstanding cmd\n");
			nvme_qpair_print_completion(qpair, cpl);
			assert(0);
		}

		if (spdk_unlikely(++pqpair->cq_head == pqpair->num_entries)) {
			pqpair->cq_head = 0;
			pqpair->phase = !pqpair->phase;
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		g_thread_mmio_ctrlr = pctrlr;
		if (spdk_likely(nvme_pcie_qpair_update_mmio_required(qpair, pqpair->cq_head,
				pqpair->cq_shadow_hdbl,
				pqpair->cq_eventidx))) {
			spdk_mmio_write_4(pqpair->cq_hdbl, pqpair->cq_head);
		}
		g_thread_mmio_ctrlr = NULL;
	}

	if (spdk_unlikely(qpair->ctrlr->timeout_cb_fn != NULL) &&
	    qpair->ctrlr->state == NVME_CTRLR_STATE_READY) {
		/*
		 * User registered for timeout callback
		 */
		nvme_pcie_qpair_check_timeout(qpair);
	}

	/* Before returning, complete any pending admin request. */
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_pcie_qpair_complete_pending_admin_request(qpair);

		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}

	return num_completions;
}
