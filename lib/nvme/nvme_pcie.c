/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2017, IBM Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/string.h"
#include "nvme_internal.h"
#include "nvme_uevent.h"

/*
 * Number of completion queue entries to process before ringing the
 *  completion queue doorbell.
 */
#define NVME_MIN_COMPLETIONS	(1)
#define NVME_MAX_COMPLETIONS	(128)

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(250)

#define NVME_MAX_PRP_LIST_ENTRIES	(503)

struct nvme_pcie_enum_ctx {
	struct spdk_nvme_probe_ctx *probe_ctx;
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

	struct {
		/* BAR mapping address which contains controller memory buffer */
		void *bar_va;

		/* BAR physical address which contains controller memory buffer */
		uint64_t bar_pa;

		/* Controller memory buffer size in Bytes */
		uint64_t size;

		/* Current offset of controller memory buffer, relative to start of BAR virt addr */
		uint64_t current_offset;

		void *mem_register_addr;
		size_t mem_register_size;
	} cmb;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;

	/* Opaque handle to associated PCI device. */
	struct spdk_pci_device *devhandle;

	/* Flag to indicate the MMIO register has been remapped */
	bool is_remapped;
};

struct nvme_tracker {
	TAILQ_ENTRY(nvme_tracker)       tq_list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint16_t			rsvd0;
	uint32_t			rsvd1;

	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;

	uint64_t			prp_sgl_bus_addr;

	/* Don't move, metadata SGL is always contiguous with Data Block SGL */
	struct spdk_nvme_sgl_descriptor		meta_sgl;
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
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, meta_sgl) & 7) == 0, "SGL must be Qword aligned");

struct nvme_pcie_poll_group {
	struct spdk_nvme_transport_poll_group group;
};

/* PCIe transport extensions for spdk_nvme_qpair */
struct nvme_pcie_qpair {
	/* Submission queue tail doorbell */
	volatile uint32_t *sq_tdbl;

	/* Completion queue head doorbell */
	volatile uint32_t *cq_hdbl;

	/* Submission queue */
	struct spdk_nvme_cmd *cmd;

	/* Completion queue */
	struct spdk_nvme_cpl *cpl;

	TAILQ_HEAD(, nvme_tracker) free_tr;
	TAILQ_HEAD(nvme_outstanding_tr_head, nvme_tracker) outstanding_tr;

	/* Array of trackers indexed by command ID. */
	struct nvme_tracker *tr;

	uint16_t num_entries;

	uint8_t retry_count;

	uint16_t max_completions_cap;

	uint16_t last_sq_tail;
	uint16_t sq_tail;
	uint16_t cq_head;
	uint16_t sq_head;

	struct {
		uint8_t phase			: 1;
		uint8_t delay_cmd_submit	: 1;
		uint8_t has_shadow_doorbell	: 1;
	} flags;

	/*
	 * Base qpair structure.
	 * This is located after the hot data in this structure so that the important parts of
	 * nvme_pcie_qpair are in the same cache line.
	 */
	struct spdk_nvme_qpair qpair;

	struct {
		/* Submission queue shadow tail doorbell */
		volatile uint32_t *sq_tdbl;

		/* Completion queue shadow head doorbell */
		volatile uint32_t *cq_hdbl;

		/* Submission queue event index */
		volatile uint32_t *sq_eventidx;

		/* Completion queue event index */
		volatile uint32_t *cq_eventidx;
	} shadow_doorbell;

	/*
	 * Fields below this point should not be touched on the normal I/O path.
	 */

	bool sq_in_cmb;

	uint64_t cmd_bus_addr;
	uint64_t cpl_bus_addr;

	struct spdk_nvme_cmd *sq_vaddr;
	struct spdk_nvme_cpl *cq_vaddr;
};

static int nvme_pcie_ctrlr_attach(struct spdk_nvme_probe_ctx *probe_ctx,
				  struct spdk_pci_addr *pci_addr);
static int nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
				     const struct spdk_nvme_io_qpair_opts *opts);
static int nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair);

__thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr = NULL;
static uint16_t g_signal_lock;
static bool g_sigset = false;

static void
nvme_sigbus_fault_sighandler(int signum, siginfo_t *info, void *ctx)
{
	void *map_address;
	uint16_t flag = 0;

	if (!__atomic_compare_exchange_n(&g_signal_lock, &flag, 1, false, __ATOMIC_ACQUIRE,
					 __ATOMIC_RELAXED)) {
		SPDK_DEBUGLOG(nvme, "request g_signal_lock failed\n");
		return;
	}

	assert(g_thread_mmio_ctrlr != NULL);

	if (!g_thread_mmio_ctrlr->is_remapped) {
		map_address = mmap((void *)g_thread_mmio_ctrlr->regs, g_thread_mmio_ctrlr->regs_size,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (map_address == MAP_FAILED) {
			SPDK_ERRLOG("mmap failed\n");
			__atomic_store_n(&g_signal_lock, 0, __ATOMIC_RELEASE);
			return;
		}
		memset(map_address, 0xFF, sizeof(struct spdk_nvme_registers));
		g_thread_mmio_ctrlr->regs = (volatile struct spdk_nvme_registers *)map_address;
		g_thread_mmio_ctrlr->is_remapped = true;
	}
	__atomic_store_n(&g_signal_lock, 0, __ATOMIC_RELEASE);
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

static inline struct nvme_pcie_ctrlr *
nvme_pcie_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_pcie_ctrlr, ctrlr);
}

static int
_nvme_pcie_hotplug_monitor(struct spdk_nvme_probe_ctx *probe_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr, *tmp;
	struct spdk_uevent event;
	struct spdk_pci_addr pci_addr;

	if (g_spdk_nvme_driver->hotplug_fd < 0) {
		return 0;
	}

	while (nvme_get_uevent(g_spdk_nvme_driver->hotplug_fd, &event) > 0) {
		if (event.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_UIO ||
		    event.subsystem == SPDK_NVME_UEVENT_SUBSYSTEM_VFIO) {
			if (event.action == SPDK_NVME_UEVENT_ADD) {
				SPDK_DEBUGLOG(nvme, "add nvme address: %s\n",
					      event.traddr);
				if (spdk_process_is_primary()) {
					if (!spdk_pci_addr_parse(&pci_addr, event.traddr)) {
						nvme_pcie_ctrlr_attach(probe_ctx, &pci_addr);
					}
				}
			} else if (event.action == SPDK_NVME_UEVENT_REMOVE) {
				struct spdk_nvme_transport_id trid;

				memset(&trid, 0, sizeof(trid));
				spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
				snprintf(trid.traddr, sizeof(trid.traddr), "%s", event.traddr);

				ctrlr = nvme_get_ctrlr_by_trid_unsafe(&trid);
				if (ctrlr == NULL) {
					return 0;
				}
				SPDK_DEBUGLOG(nvme, "remove nvme address: %s\n",
					      event.traddr);

				nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
				nvme_ctrlr_fail(ctrlr, true);
				nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

				/* get the user app to clean up and stop I/O */
				if (ctrlr->remove_cb) {
					nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
					ctrlr->remove_cb(probe_ctx->cb_ctx, ctrlr);
					nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
				}
			}
		}
	}

	/* Initiate removal of physically hotremoved PCI controllers. Even after
	 * they're hotremoved from the system, SPDK might still report them via RPC.
	 */
	TAILQ_FOREACH_SAFE(ctrlr, &g_spdk_nvme_driver->shared_attached_ctrlrs, tailq, tmp) {
		bool do_remove = false;
		struct nvme_pcie_ctrlr *pctrlr;

		if (ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
			continue;
		}

		pctrlr = nvme_pcie_ctrlr(ctrlr);
		if (spdk_pci_device_is_removed(pctrlr->devhandle)) {
			do_remove = true;
		}

		if (do_remove) {
			nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
			nvme_ctrlr_fail(ctrlr, true);
			nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
			if (ctrlr->remove_cb) {
				nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
				ctrlr->remove_cb(probe_ctx->cb_ctx, ctrlr);
				nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
			}
		}
	}
	return 0;
}

static inline struct nvme_pcie_qpair *
nvme_pcie_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_PCIE);
	return SPDK_CONTAINEROF(qpair, struct nvme_pcie_qpair, qpair);
}

static volatile void *
nvme_pcie_reg_addr(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return (volatile void *)((uintptr_t)pctrlr->regs + offset);
}

static int
nvme_pcie_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	g_thread_mmio_ctrlr = pctrlr;
	spdk_mmio_write_4(nvme_pcie_reg_addr(ctrlr, offset), value);
	g_thread_mmio_ctrlr = NULL;
	return 0;
}

static int
nvme_pcie_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	g_thread_mmio_ctrlr = pctrlr;
	spdk_mmio_write_8(nvme_pcie_reg_addr(ctrlr, offset), value);
	g_thread_mmio_ctrlr = NULL;
	return 0;
}

static int
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

static int
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

static  uint32_t
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

static uint16_t
nvme_pcie_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_SGL_DESCRIPTORS;
}

static void
nvme_pcie_ctrlr_map_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr = NULL;
	uint32_t bir;
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
	uint64_t size, unit_size, offset, bar_size = 0, bar_phys_addr = 0;

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

	pctrlr->cmb.bar_va = addr;
	pctrlr->cmb.bar_pa = bar_phys_addr;
	pctrlr->cmb.size = size;
	pctrlr->cmb.current_offset = offset;

	if (!cmbsz.bits.sqs) {
		pctrlr->ctrlr.opts.use_cmb_sqs = false;
	}

	return;
exit:
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
	return;
}

static int
nvme_pcie_ctrlr_unmap_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	union spdk_nvme_cmbloc_register cmbloc;
	void *addr = pctrlr->cmb.bar_va;

	if (addr) {
		if (pctrlr->cmb.mem_register_addr) {
			spdk_mem_unregister(pctrlr->cmb.mem_register_addr, pctrlr->cmb.mem_register_size);
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
nvme_pcie_ctrlr_reserve_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	if (pctrlr->cmb.bar_va == NULL) {
		SPDK_DEBUGLOG(nvme, "CMB not available\n");
		return -ENOTSUP;
	}

	if (ctrlr->opts.use_cmb_sqs) {
		SPDK_ERRLOG("CMB is already in use for submission queues.\n");
		return -ENOTSUP;
	}

	return 0;
}

static void *
nvme_pcie_ctrlr_map_io_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
	uint64_t mem_register_start, mem_register_end;
	int rc;

	if (pctrlr->cmb.mem_register_addr != NULL) {
		*size = pctrlr->cmb.mem_register_size;
		return pctrlr->cmb.mem_register_addr;
	}

	*size = 0;

	if (pctrlr->cmb.bar_va == NULL) {
		SPDK_DEBUGLOG(nvme, "CMB not available\n");
		return NULL;
	}

	if (ctrlr->opts.use_cmb_sqs) {
		SPDK_ERRLOG("CMB is already in use for submission queues.\n");
		return NULL;
	}

	if (nvme_pcie_ctrlr_get_cmbsz(pctrlr, &cmbsz) ||
	    nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
		SPDK_ERRLOG("get registers failed\n");
		return NULL;
	}

	/* If only SQS is supported */
	if (!(cmbsz.bits.wds || cmbsz.bits.rds)) {
		return NULL;
	}

	/* If CMB is less than 4MiB in size then abort CMB mapping */
	if (pctrlr->cmb.size < (1ULL << 22)) {
		return NULL;
	}

	mem_register_start = _2MB_PAGE((uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.current_offset +
				       VALUE_2MB - 1);
	mem_register_end = _2MB_PAGE((uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.current_offset +
				     pctrlr->cmb.size);

	rc = spdk_mem_register((void *)mem_register_start, mem_register_end - mem_register_start);
	if (rc) {
		SPDK_ERRLOG("spdk_mem_register() failed\n");
		return NULL;
	}

	pctrlr->cmb.mem_register_addr = (void *)mem_register_start;
	pctrlr->cmb.mem_register_size = mem_register_end - mem_register_start;

	*size = pctrlr->cmb.mem_register_size;
	return pctrlr->cmb.mem_register_addr;
}

static int
nvme_pcie_ctrlr_unmap_io_cmb(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	int rc;

	if (pctrlr->cmb.mem_register_addr == NULL) {
		return 0;
	}

	rc = spdk_mem_unregister(pctrlr->cmb.mem_register_addr, pctrlr->cmb.mem_register_size);

	if (rc == 0) {
		pctrlr->cmb.mem_register_addr = NULL;
		pctrlr->cmb.mem_register_size = 0;
	}

	return rc;
}

static int
nvme_pcie_ctrlr_allocate_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr = NULL;
	uint64_t phys_addr = 0, size = 0;

	rc = spdk_pci_device_map_bar(pctrlr->devhandle, 0, &addr,
				     &phys_addr, &size);

	if ((addr == NULL) || (rc != 0)) {
		SPDK_ERRLOG("nvme_pcicfg_map_bar failed with rc %d or bar %p\n",
			    rc, addr);
		return -1;
	}

	pctrlr->regs = (volatile struct spdk_nvme_registers *)addr;
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
nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t num_entries)
{
	struct nvme_pcie_qpair *pqpair;
	int rc;

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (pqpair == NULL) {
		return -ENOMEM;
	}

	pqpair->num_entries = num_entries;
	pqpair->flags.delay_cmd_submit = 0;

	ctrlr->adminq = &pqpair->qpair;

	rc = nvme_qpair_init(ctrlr->adminq,
			     0, /* qpair ID */
			     ctrlr,
			     SPDK_NVME_QPRIO_URGENT,
			     num_entries);
	if (rc != 0) {
		return rc;
	}

	return nvme_pcie_qpair_construct(ctrlr->adminq, NULL);
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

	spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
	spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &pci_addr);

	ctrlr = nvme_get_ctrlr_by_trid_unsafe(&trid);
	if (!spdk_process_is_primary()) {
		if (!ctrlr) {
			SPDK_ERRLOG("Controller must be constructed in the primary process first.\n");
			return -1;
		}

		return nvme_ctrlr_add_process(ctrlr, pci_dev);
	}

	/* check whether user passes the pci_addr */
	if (enum_ctx->has_pci_addr &&
	    (spdk_pci_addr_compare(&pci_addr, &enum_ctx->pci_addr) != 0)) {
		return 1;
	}

	return nvme_ctrlr_probe(&trid, enum_ctx->probe_ctx, pci_dev);
}

static int
nvme_pcie_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		     bool direct_connect)
{
	struct nvme_pcie_enum_ctx enum_ctx = {};

	enum_ctx.probe_ctx = probe_ctx;

	if (strlen(probe_ctx->trid.traddr) != 0) {
		if (spdk_pci_addr_parse(&enum_ctx.pci_addr, probe_ctx->trid.traddr)) {
			return -1;
		}
		enum_ctx.has_pci_addr = true;
	}

	/* Only the primary process can monitor hotplug. */
	if (spdk_process_is_primary()) {
		_nvme_pcie_hotplug_monitor(probe_ctx);
	}

	if (enum_ctx.has_pci_addr == false) {
		return spdk_pci_enumerate(spdk_pci_nvme_get_driver(),
					  pcie_nvme_enum_cb, &enum_ctx);
	} else {
		return spdk_pci_device_attach(spdk_pci_nvme_get_driver(),
					      pcie_nvme_enum_cb, &enum_ctx, &enum_ctx.pci_addr);
	}
}

static int
nvme_pcie_ctrlr_attach(struct spdk_nvme_probe_ctx *probe_ctx, struct spdk_pci_addr *pci_addr)
{
	struct nvme_pcie_enum_ctx enum_ctx;

	enum_ctx.probe_ctx = probe_ctx;
	enum_ctx.has_pci_addr = true;
	enum_ctx.pci_addr = *pci_addr;

	return spdk_pci_enumerate(spdk_pci_nvme_get_driver(), pcie_nvme_enum_cb, &enum_ctx);
}

static struct spdk_nvme_ctrlr *nvme_pcie_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct spdk_pci_device *pci_dev = devhandle;
	struct nvme_pcie_ctrlr *pctrlr;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_vs_register vs;
	uint16_t cmd_reg;
	int rc;
	struct spdk_pci_id pci_id;

	rc = spdk_pci_device_claim(pci_dev);
	if (rc < 0) {
		SPDK_ERRLOG("could not claim device %s (%s)\n",
			    trid->traddr, spdk_strerror(-rc));
		return NULL;
	}

	pctrlr = spdk_zmalloc(sizeof(struct nvme_pcie_ctrlr), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (pctrlr == NULL) {
		spdk_pci_device_unclaim(pci_dev);
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	pctrlr->is_remapped = false;
	pctrlr->ctrlr.is_removed = false;
	pctrlr->devhandle = devhandle;
	pctrlr->ctrlr.opts = *opts;
	pctrlr->ctrlr.trid = *trid;

	rc = nvme_ctrlr_construct(&pctrlr->ctrlr);
	if (rc != 0) {
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(pctrlr);
		return NULL;
	}

	rc = nvme_pcie_ctrlr_allocate_bars(pctrlr);
	if (rc != 0) {
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(pctrlr);
		return NULL;
	}

	/* Enable PCI busmaster and disable INTx */
	spdk_pci_device_cfg_read16(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x404;
	spdk_pci_device_cfg_write16(pci_dev, cmd_reg, 4);

	if (nvme_ctrlr_get_cap(&pctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(pctrlr);
		return NULL;
	}

	if (nvme_ctrlr_get_vs(&pctrlr->ctrlr, &vs)) {
		SPDK_ERRLOG("get_vs() failed\n");
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(pctrlr);
		return NULL;
	}

	nvme_ctrlr_init_cap(&pctrlr->ctrlr, &cap, &vs);

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	pci_id = spdk_pci_device_get_id(pci_dev);
	pctrlr->ctrlr.quirks = nvme_get_quirks(&pci_id);

	rc = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr->ctrlr, pctrlr->ctrlr.opts.admin_queue_size);
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

static int
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

static int
nvme_pcie_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct spdk_pci_device *devhandle = nvme_ctrlr_proc_get_devhandle(ctrlr);

	if (ctrlr->adminq) {
		nvme_pcie_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	nvme_ctrlr_free_processes(ctrlr);

	nvme_pcie_ctrlr_free_bars(pctrlr);

	if (devhandle) {
		spdk_pci_device_unclaim(devhandle);
		spdk_pci_device_detach(devhandle);
	}

	spdk_free(pctrlr);

	return 0;
}

static void
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid, uint64_t phys_addr)
{
	tr->prp_sgl_bus_addr = phys_addr + offsetof(struct nvme_tracker, u.prp);
	tr->cid = cid;
	tr->req = NULL;
}

static int
nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	uint32_t i;

	/* all head/tail vals are set to 0 */
	pqpair->last_sq_tail = pqpair->sq_tail = pqpair->sq_head = pqpair->cq_head = 0;

	/*
	 * First time through the completion queue, HW will set phase
	 *  bit on completions to 1.  So set this to 1 here, indicating
	 *  we're looking for a 1 to know which entries have completed.
	 *  we'll toggle the bit each time when the completion queue
	 *  rolls over.
	 */
	pqpair->flags.phase = 1;
	for (i = 0; i < pqpair->num_entries; i++) {
		pqpair->cpl[i].status.p = 0;
	}

	return 0;
}

static void *
nvme_pcie_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t size, uint64_t alignment,
			  uint64_t *phys_addr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	uintptr_t addr;

	if (pctrlr->cmb.mem_register_addr != NULL) {
		/* BAR is mapped for data */
		return NULL;
	}

	addr = (uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.current_offset;
	addr = (addr + (alignment - 1)) & ~(alignment - 1);

	/* CMB may only consume part of the BAR, calculate accordingly */
	if (addr + size > ((uintptr_t)pctrlr->cmb.bar_va + pctrlr->cmb.size)) {
		SPDK_ERRLOG("Tried to allocate past valid CMB range!\n");
		return NULL;
	}
	*phys_addr = pctrlr->cmb.bar_pa + addr - (uintptr_t)pctrlr->cmb.bar_va;

	pctrlr->cmb.current_offset = (addr + size) - (uintptr_t)pctrlr->cmb.bar_va;

	return (void *)addr;
}

static int
nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
			  const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	uint16_t		i;
	volatile uint32_t	*doorbell_base;
	uint16_t		num_trackers;
	size_t			page_align = sysconf(_SC_PAGESIZE);
	size_t			queue_align, queue_len;
	uint32_t                flags = SPDK_MALLOC_DMA;
	uint64_t		sq_paddr = 0;
	uint64_t		cq_paddr = 0;

	if (opts) {
		pqpair->sq_vaddr = opts->sq.vaddr;
		pqpair->cq_vaddr = opts->cq.vaddr;
		sq_paddr = opts->sq.paddr;
		cq_paddr = opts->cq.paddr;
	}

	pqpair->retry_count = ctrlr->opts.transport_retry_count;

	/*
	 * Limit the maximum number of completions to return per call to prevent wraparound,
	 * and calculate how many trackers can be submitted at once without overflowing the
	 * completion queue.
	 */
	pqpair->max_completions_cap = pqpair->num_entries / 4;
	pqpair->max_completions_cap = spdk_max(pqpair->max_completions_cap, NVME_MIN_COMPLETIONS);
	pqpair->max_completions_cap = spdk_min(pqpair->max_completions_cap, NVME_MAX_COMPLETIONS);
	num_trackers = pqpair->num_entries - pqpair->max_completions_cap;

	SPDK_INFOLOG(nvme, "max_completions_cap = %" PRIu16 " num_trackers = %" PRIu16 "\n",
		     pqpair->max_completions_cap, num_trackers);

	assert(num_trackers != 0);

	pqpair->sq_in_cmb = false;

	if (nvme_qpair_is_admin_queue(&pqpair->qpair)) {
		flags |= SPDK_MALLOC_SHARE;
	}

	/* cmd and cpl rings must be aligned on page size boundaries. */
	if (ctrlr->opts.use_cmb_sqs) {
		pqpair->cmd = nvme_pcie_ctrlr_alloc_cmb(ctrlr, pqpair->num_entries * sizeof(struct spdk_nvme_cmd),
							page_align, &pqpair->cmd_bus_addr);
		if (pqpair->cmd != NULL) {
			pqpair->sq_in_cmb = true;
		}
	}

	if (pqpair->sq_in_cmb == false) {
		if (pqpair->sq_vaddr) {
			pqpair->cmd = pqpair->sq_vaddr;
		} else {
			/* To ensure physical address contiguity we make each ring occupy
			 * a single hugepage only. See MAX_IO_QUEUE_ENTRIES.
			 */
			queue_len = pqpair->num_entries * sizeof(struct spdk_nvme_cmd);
			queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
			pqpair->cmd = spdk_zmalloc(queue_len, queue_align, NULL, SPDK_ENV_SOCKET_ID_ANY, flags);
			if (pqpair->cmd == NULL) {
				SPDK_ERRLOG("alloc qpair_cmd failed\n");
				return -ENOMEM;
			}
		}
		if (sq_paddr) {
			assert(pqpair->sq_vaddr != NULL);
			pqpair->cmd_bus_addr = sq_paddr;
		} else {
			pqpair->cmd_bus_addr = spdk_vtophys(pqpair->cmd, NULL);
			if (pqpair->cmd_bus_addr == SPDK_VTOPHYS_ERROR) {
				SPDK_ERRLOG("spdk_vtophys(pqpair->cmd) failed\n");
				return -EFAULT;
			}
		}
	}

	if (pqpair->cq_vaddr) {
		pqpair->cpl = pqpair->cq_vaddr;
	} else {
		queue_len = pqpair->num_entries * sizeof(struct spdk_nvme_cpl);
		queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
		pqpair->cpl = spdk_zmalloc(queue_len, queue_align, NULL, SPDK_ENV_SOCKET_ID_ANY, flags);
		if (pqpair->cpl == NULL) {
			SPDK_ERRLOG("alloc qpair_cpl failed\n");
			return -ENOMEM;
		}
	}
	if (cq_paddr) {
		assert(pqpair->cq_vaddr != NULL);
		pqpair->cpl_bus_addr = cq_paddr;
	} else {
		pqpair->cpl_bus_addr = spdk_vtophys(pqpair->cpl, NULL);
		if (pqpair->cpl_bus_addr == SPDK_VTOPHYS_ERROR) {
			SPDK_ERRLOG("spdk_vtophys(pqpair->cpl) failed\n");
			return -EFAULT;
		}
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
	pqpair->tr = spdk_zmalloc(num_trackers * sizeof(*tr), sizeof(*tr), NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (pqpair->tr == NULL) {
		SPDK_ERRLOG("nvme_tr failed\n");
		return -ENOMEM;
	}

	TAILQ_INIT(&pqpair->free_tr);
	TAILQ_INIT(&pqpair->outstanding_tr);

	for (i = 0; i < num_trackers; i++) {
		tr = &pqpair->tr[i];
		nvme_qpair_construct_tracker(tr, i, spdk_vtophys(tr, NULL));
		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
	}

	nvme_pcie_qpair_reset(qpair);

	return 0;
}

/* Used when dst points to MMIO (i.e. CMB) in a virtual machine - in these cases we must
 * not use wide instructions because QEMU will not emulate such instructions to MMIO space.
 * So this function ensures we only copy 8 bytes at a time.
 */
static inline void
nvme_pcie_copy_command_mmio(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	uint64_t *dst64 = (uint64_t *)dst;
	const uint64_t *src64 = (const uint64_t *)src;
	uint32_t i;

	for (i = 0; i < sizeof(*dst) / 8; i++) {
		dst64[i] = src64[i];
	}
}

static inline void
nvme_pcie_copy_command(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	/* dst and src are known to be non-overlapping and 64-byte aligned. */
#if defined(__SSE2__)
	__m128i *d128 = (__m128i *)dst;
	const __m128i *s128 = (const __m128i *)src;

	_mm_stream_si128(&d128[0], _mm_load_si128(&s128[0]));
	_mm_stream_si128(&d128[1], _mm_load_si128(&s128[1]));
	_mm_stream_si128(&d128[2], _mm_load_si128(&s128[2]));
	_mm_stream_si128(&d128[3], _mm_load_si128(&s128[3]));
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

	/*
	 * The admin request is from another process. Move to the per
	 *  process list for that process to handle it later.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));
	assert(active_req->pid != getpid());

	active_proc = nvme_ctrlr_get_process(ctrlr, active_req->pid);
	if (active_proc) {
		/* Save the original completion information */
		memcpy(&active_req->cpl, cpl, sizeof(*cpl));
		STAILQ_INSERT_TAIL(&active_proc->active_reqs, active_req, stailq);
	} else {
		SPDK_ERRLOG("The owning process (pid %d) is not found. Dropping the request.\n",
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
	pid_t				pid = getpid();
	struct spdk_nvme_ctrlr_process	*proc;

	/*
	 * Check whether there is any pending admin request from
	 * other active processes.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));

	proc = nvme_ctrlr_get_current_process(ctrlr);
	if (!proc) {
		SPDK_ERRLOG("the active process (pid %d) is not found for this controller.\n", pid);
		assert(proc);
		return;
	}

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == pid);

		nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, &req->cpl);
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

	/*
	 * Ensure that the doorbell is updated before reading the EventIdx from
	 * memory
	 */
	spdk_mb();

	if (!nvme_pcie_qpair_need_event(*eventidx, value, old)) {
		return false;
	}

	return true;
}

static inline void
nvme_pcie_qpair_ring_sq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	bool need_mmio = true;

	if (qpair->first_fused_submitted) {
		/* This is first cmd of two fused commands - don't ring doorbell */
		qpair->first_fused_submitted = 0;
		return;
	}

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		need_mmio = nvme_pcie_qpair_update_mmio_required(qpair,
				pqpair->sq_tail,
				pqpair->shadow_doorbell.sq_tdbl,
				pqpair->shadow_doorbell.sq_eventidx);
	}

	if (spdk_likely(need_mmio)) {
		spdk_wmb();
		g_thread_mmio_ctrlr = pctrlr;
		spdk_mmio_write_4(pqpair->sq_tdbl, pqpair->sq_tail);
		g_thread_mmio_ctrlr = NULL;
	}
}

static inline void
nvme_pcie_qpair_ring_cq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(qpair->ctrlr);
	bool need_mmio = true;

	if (spdk_unlikely(pqpair->flags.has_shadow_doorbell)) {
		need_mmio = nvme_pcie_qpair_update_mmio_required(qpair,
				pqpair->cq_head,
				pqpair->shadow_doorbell.cq_hdbl,
				pqpair->shadow_doorbell.cq_eventidx);
	}

	if (spdk_likely(need_mmio)) {
		g_thread_mmio_ctrlr = pctrlr;
		spdk_mmio_write_4(pqpair->cq_hdbl, pqpair->cq_head);
		g_thread_mmio_ctrlr = NULL;
	}
}

static void
nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;

	req = tr->req;
	assert(req != NULL);

	if (req->cmd.fuse == SPDK_NVME_IO_FLAGS_FUSE_FIRST) {
		/* This is first cmd of two fused commands - don't ring doorbell */
		qpair->first_fused_submitted = 1;
	}

	/* Don't use wide instructions to copy NVMe command, this is limited by QEMU
	 * virtual NVMe controller, the maximum access width is 8 Bytes for one time.
	 */
	if (spdk_unlikely((ctrlr->quirks & NVME_QUIRK_MAXIMUM_PCI_ACCESS_WIDTH) && pqpair->sq_in_cmb)) {
		nvme_pcie_copy_command_mmio(&pqpair->cmd[pqpair->sq_tail], &req->cmd);
	} else {
		/* Copy the command from the tracker to the submission queue. */
		nvme_pcie_copy_command(&pqpair->cmd[pqpair->sq_tail], &req->cmd);
	}

	if (spdk_unlikely(++pqpair->sq_tail == pqpair->num_entries)) {
		pqpair->sq_tail = 0;
	}

	if (spdk_unlikely(pqpair->sq_tail == pqpair->sq_head)) {
		SPDK_ERRLOG("sq_tail is passing sq_head!\n");
	}

	if (!pqpair->flags.delay_cmd_submit) {
		nvme_pcie_qpair_ring_sq_doorbell(qpair);
	}
}

static void
nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_pcie_qpair		*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_request		*req;
	bool				retry, error;
	bool				req_from_current_proc = true;

	req = tr->req;

	assert(req != NULL);

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < pqpair->retry_count;

	if (error && print_on_error && !qpair->ctrlr->opts.disable_error_logging) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
		spdk_nvme_qpair_print_completion(qpair, cpl);
	}

	assert(cpl->cid == req->cmd.cid);

	if (retry) {
		req->retries++;
		nvme_pcie_qpair_submit_tracker(qpair, tr);
	} else {
		TAILQ_REMOVE(&pqpair->outstanding_tr, tr, tq_list);

		/* Only check admin requests from different processes. */
		if (nvme_qpair_is_admin_queue(qpair) && req->pid != getpid()) {
			req_from_current_proc = false;
			nvme_pcie_qpair_insert_pending_admin_request(qpair, req, cpl);
		} else {
			nvme_complete_request(tr->cb_fn, tr->cb_arg, qpair, req, cpl);
		}

		if (req_from_current_proc == true) {
			nvme_qpair_free_request(qpair, req);
		}

		tr->req = NULL;

		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
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
	struct nvme_tracker *tr, *temp, *last;

	last = TAILQ_LAST(&pqpair->outstanding_tr, nvme_outstanding_tr_head);

	/* Abort previously submitted (outstanding) trs */
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, temp) {
		if (!qpair->ctrlr->opts.disable_error_logging) {
			SPDK_ERRLOG("aborting outstanding command\n");
		}
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, dnr, true);

		if (tr == last) {
			break;
		}
	}
}

static int
nvme_pcie_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				 int (*iter_fn)(struct nvme_request *req, void *arg),
				 void *arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *tmp;
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
		assert(tr->req != NULL);

		rc = iter_fn(tr->req, arg);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
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
	/*
	 * We check sq_vaddr and cq_vaddr to see if the user specified the memory
	 * buffers when creating the I/O queue.
	 * If the user specified them, we cannot free that memory.
	 * Nor do we free it if it's in the CMB.
	 */
	if (!pqpair->sq_vaddr && pqpair->cmd && !pqpair->sq_in_cmb) {
		spdk_free(pqpair->cmd);
	}
	if (!pqpair->cq_vaddr && pqpair->cpl) {
		spdk_free(pqpair->cpl);
	}
	if (pqpair->tr) {
		spdk_free(pqpair->tr);
	}

	nvme_qpair_deinit(qpair);

	spdk_free(pqpair);

	return 0;
}

static void
nvme_pcie_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	nvme_pcie_qpair_abort_trackers(qpair, dnr);
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

	cmd->cdw10_bits.create_io_q.qid = io_que->id;
	cmd->cdw10_bits.create_io_q.qsize = pqpair->num_entries - 1;

	cmd->cdw11_bits.create_io_cq.pc = 1;
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

	cmd->cdw10_bits.create_io_q.qid = io_que->id;
	cmd->cdw10_bits.create_io_q.qsize = pqpair->num_entries - 1;
	cmd->cdw11_bits.create_io_sq.pc = 1;
	cmd->cdw11_bits.create_io_sq.qprio = io_que->qprio;
	cmd->cdw11_bits.create_io_sq.cqid = io_que->id;
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
	cmd->cdw10_bits.delete_io_q.qid = qpair->id;

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
	cmd->cdw10_bits.delete_io_q.qid = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
_nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 uint16_t qid)
{
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_completion_poll_status	*status;
	int					rc;

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	rc = nvme_pcie_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		free(status);
		return rc;
	}

	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		SPDK_ERRLOG("nvme_create_io_cq failed!\n");
		if (!status->timed_out) {
			free(status);
		}
		return -1;
	}

	memset(status, 0, sizeof(*status));
	rc = nvme_pcie_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		free(status);
		return rc;
	}

	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		SPDK_ERRLOG("nvme_create_io_sq failed!\n");
		if (status->timed_out) {
			/* Request is still queued, the memory will be freed in a completion callback.
			   allocate a new request */
			status = calloc(1, sizeof(*status));
			if (!status) {
				SPDK_ERRLOG("Failed to allocate status tracker\n");
				return -ENOMEM;
			}
		}

		memset(status, 0, sizeof(*status));
		/* Attempt to delete the completion queue */
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_poll_cb, status);
		if (rc != 0) {
			/* The originall or newly allocated status structure can be freed since
			 * the corresponding request has been completed of failed to submit */
			free(status);
			return -1;
		}
		nvme_wait_for_completion(ctrlr->adminq, status);
		if (!status->timed_out) {
			/* status can be freed regardless of nvme_wait_for_completion return value */
			free(status);
		}
		return -1;
	}

	if (ctrlr->shadow_doorbell) {
		pqpair->shadow_doorbell.sq_tdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 0) *
						  pctrlr->doorbell_stride_u32;
		pqpair->shadow_doorbell.cq_hdbl = ctrlr->shadow_doorbell + (2 * qpair->id + 1) *
						  pctrlr->doorbell_stride_u32;
		pqpair->shadow_doorbell.sq_eventidx = ctrlr->eventidx + (2 * qpair->id + 0) *
						      pctrlr->doorbell_stride_u32;
		pqpair->shadow_doorbell.cq_eventidx = ctrlr->eventidx + (2 * qpair->id + 1) *
						      pctrlr->doorbell_stride_u32;
		pqpair->flags.has_shadow_doorbell = 1;
	} else {
		pqpair->flags.has_shadow_doorbell = 0;
	}
	nvme_pcie_qpair_reset(qpair);
	free(status);

	return 0;
}

static struct spdk_nvme_qpair *
nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	struct nvme_pcie_qpair *pqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	assert(ctrlr != NULL);

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (pqpair == NULL) {
		return NULL;
	}

	pqpair->num_entries = opts->io_queue_size;
	pqpair->flags.delay_cmd_submit = opts->delay_cmd_submit;

	qpair = &pqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, opts->qprio, opts->io_queue_requests);
	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	rc = nvme_pcie_qpair_construct(qpair, opts);

	if (rc != 0) {
		nvme_pcie_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

static int
nvme_pcie_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	if (nvme_qpair_is_admin_queue(qpair)) {
		return 0;
	} else {
		return _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qpair->id);
	}
}

static void
nvme_pcie_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
}

static int32_t nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions);

static int
nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status *status;
	int rc;

	assert(ctrlr != NULL);

	if (ctrlr->is_removed) {
		goto free;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* Delete the I/O submission queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send request to delete_io_sq with rc=%d\n", rc);
		free(status);
		return rc;
	}
	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		return -1;
	}

	/* Now that the submission queue is deleted, the device is supposed to have
	 * completed any outstanding I/O. Try to complete them. If they don't complete,
	 * they'll be marked as aborted and completed below. */
	nvme_pcie_qpair_process_completions(qpair, 0);

	memset(status, 0, sizeof(*status));
	/* Delete the completion queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send request to delete_io_cq with rc=%d\n", rc);
		free(status);
		return rc;
	}
	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		return -1;
	}
	free(status);

free:
	if (qpair->no_deletion_notification_needed == 0) {
		/* Abort the rest of the I/O */
		nvme_pcie_qpair_abort_trackers(qpair, 1);
	}

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
static inline int
nvme_pcie_prp_list_append(struct nvme_tracker *tr, uint32_t *prp_index, void *virt_addr, size_t len,
			  uint32_t page_size)
{
	struct spdk_nvme_cmd *cmd = &tr->req->cmd;
	uintptr_t page_mask = page_size - 1;
	uint64_t phys_addr;
	uint32_t i;

	SPDK_DEBUGLOG(nvme, "prp_index:%u virt_addr:%p len:%u\n",
		      *prp_index, virt_addr, (uint32_t)len);

	if (spdk_unlikely(((uintptr_t)virt_addr & 3) != 0)) {
		SPDK_ERRLOG("virt_addr %p not dword aligned\n", virt_addr);
		return -EFAULT;
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
			return -EFAULT;
		}

		phys_addr = spdk_vtophys(virt_addr, NULL);
		if (spdk_unlikely(phys_addr == SPDK_VTOPHYS_ERROR)) {
			SPDK_ERRLOG("vtophys(%p) failed\n", virt_addr);
			return -EFAULT;
		}

		if (i == 0) {
			SPDK_DEBUGLOG(nvme, "prp1 = %p\n", (void *)phys_addr);
			cmd->dptr.prp.prp1 = phys_addr;
			seg_len = page_size - ((uintptr_t)virt_addr & page_mask);
		} else {
			if ((phys_addr & page_mask) != 0) {
				SPDK_ERRLOG("PRP %u not page aligned (%p)\n", i, virt_addr);
				return -EFAULT;
			}

			SPDK_DEBUGLOG(nvme, "prp[%u] = %p\n", i - 1, (void *)phys_addr);
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
		SPDK_DEBUGLOG(nvme, "prp2 = %p\n", (void *)cmd->dptr.prp.prp2);
	} else {
		cmd->dptr.prp.prp2 = tr->prp_sgl_bus_addr;
		SPDK_DEBUGLOG(nvme, "prp2 = %p (PRP list)\n", (void *)cmd->dptr.prp.prp2);
	}

	*prp_index = i;
	return 0;
}

static int
nvme_pcie_qpair_build_request_invalid(struct spdk_nvme_qpair *qpair,
				      struct nvme_request *req, struct nvme_tracker *tr, bool dword_aligned)
{
	assert(0);
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EINVAL;
}

/**
 * Build PRP list describing physically contiguous payload buffer.
 */
static int
nvme_pcie_qpair_build_contig_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr, bool dword_aligned)
{
	uint32_t prp_index = 0;
	int rc;

	rc = nvme_pcie_prp_list_append(tr, &prp_index, req->payload.contig_or_cb_arg + req->payload_offset,
				       req->payload_size, qpair->ctrlr->page_size);
	if (rc) {
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	}

	return rc;
}

/**
 * Build an SGL describing a physically contiguous payload buffer.
 *
 * This is more efficient than using PRP because large buffers can be
 * described this way.
 */
static int
nvme_pcie_qpair_build_contig_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
		struct nvme_tracker *tr, bool dword_aligned)
{
	void *virt_addr;
	uint64_t phys_addr, mapping_length;
	uint32_t length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	sgl = tr->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	length = req->payload_size;
	virt_addr = req->payload.contig_or_cb_arg + req->payload_offset;
	mapping_length = length;

	while (length > 0) {
		if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
			SPDK_ERRLOG("virt_addr %p not dword aligned\n", virt_addr);
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		phys_addr = spdk_vtophys(virt_addr, &mapping_length);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		mapping_length = spdk_min(length, mapping_length);

		length -= mapping_length;
		virt_addr += mapping_length;

		sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl->unkeyed.length = mapping_length;
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
		/* SPDK NVMe driver supports only 1 SGL segment for now, it is enough because
		 *  NVME_MAX_SGL_DESCRIPTORS * 16 is less than one page.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	return 0;
}

/**
 * Build SGL list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr, bool dword_aligned)
{
	int rc;
	void *virt_addr;
	uint64_t phys_addr, mapping_length;
	uint32_t remaining_transfer_len, remaining_user_sge_len, length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	sgl = tr->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	remaining_transfer_len = req->payload_size;

	while (remaining_transfer_len > 0) {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg,
					      &virt_addr, &remaining_user_sge_len);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
		}

		/* Bit Bucket SGL descriptor */
		if ((uint64_t)virt_addr == UINT64_MAX) {
			/* TODO: enable WRITE and COMPARE when necessary */
			if (req->cmd.opc != SPDK_NVME_OPC_READ) {
				SPDK_ERRLOG("Only READ command can be supported\n");
				goto exit;
			}
			if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
				SPDK_ERRLOG("Too many SGL entries\n");
				goto exit;
			}

			sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_BIT_BUCKET;
			/* If the SGL describes a destination data buffer, the length of data
			 * buffer shall be discarded by controller, and the length is included
			 * in Number of Logical Blocks (NLB) parameter. Otherwise, the length
			 * is not included in the NLB parameter.
			 */
			remaining_user_sge_len = spdk_min(remaining_user_sge_len, remaining_transfer_len);
			remaining_transfer_len -= remaining_user_sge_len;

			sgl->unkeyed.length = remaining_user_sge_len;
			sgl->address = 0;
			sgl->unkeyed.subtype = 0;

			sgl++;
			nseg++;

			continue;
		}

		remaining_user_sge_len = spdk_min(remaining_user_sge_len, remaining_transfer_len);
		remaining_transfer_len -= remaining_user_sge_len;
		while (remaining_user_sge_len > 0) {
			if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
				SPDK_ERRLOG("Too many SGL entries\n");
				goto exit;
			}

			if (dword_aligned && ((uintptr_t)virt_addr & 3)) {
				SPDK_ERRLOG("virt_addr %p not dword aligned\n", virt_addr);
				goto exit;
			}

			mapping_length = remaining_user_sge_len;
			phys_addr = spdk_vtophys(virt_addr, &mapping_length);
			if (phys_addr == SPDK_VTOPHYS_ERROR) {
				goto exit;
			}

			length = spdk_min(remaining_user_sge_len, mapping_length);
			remaining_user_sge_len -= length;
			virt_addr += length;

			if (nseg > 0 && phys_addr ==
			    (*(sgl - 1)).address + (*(sgl - 1)).unkeyed.length) {
				/* extend previous entry */
				(*(sgl - 1)).unkeyed.length += length;
				continue;
			}

			sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			sgl->unkeyed.length = length;
			sgl->address = phys_addr;
			sgl->unkeyed.subtype = 0;

			sgl++;
			nseg++;
		}
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
		/* SPDK NVMe driver supports only 1 SGL segment for now, it is enough because
		 *  NVME_MAX_SGL_DESCRIPTORS * 16 is less than one page.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	return 0;

exit:
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EFAULT;
}

/**
 * Build PRP list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_prps_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				       struct nvme_tracker *tr, bool dword_aligned)
{
	int rc;
	void *virt_addr;
	uint32_t remaining_transfer_len, length;
	uint32_t prp_index = 0;
	uint32_t page_size = qpair->ctrlr->page_size;

	/*
	 * Build scattered payloads.
	 */
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	remaining_transfer_len = req->payload_size;
	while (remaining_transfer_len > 0) {
		assert(req->payload.next_sge_fn != NULL);
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -EFAULT;
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

typedef int(*build_req_fn)(struct spdk_nvme_qpair *, struct nvme_request *, struct nvme_tracker *,
			   bool);

static build_req_fn const g_nvme_pcie_build_req_table[][2] = {
	[NVME_PAYLOAD_TYPE_INVALID] = {
		nvme_pcie_qpair_build_request_invalid,			/* PRP */
		nvme_pcie_qpair_build_request_invalid			/* SGL */
	},
	[NVME_PAYLOAD_TYPE_CONTIG] = {
		nvme_pcie_qpair_build_contig_request,			/* PRP */
		nvme_pcie_qpair_build_contig_hw_sgl_request		/* SGL */
	},
	[NVME_PAYLOAD_TYPE_SGL] = {
		nvme_pcie_qpair_build_prps_sgl_request,			/* PRP */
		nvme_pcie_qpair_build_hw_sgl_request			/* SGL */
	}
};

static int
nvme_pcie_qpair_build_metadata(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
			       bool sgl_supported, bool dword_aligned)
{
	void *md_payload;
	struct nvme_request *req = tr->req;

	if (req->payload.md) {
		md_payload = req->payload.md + req->md_offset;
		if (dword_aligned && ((uintptr_t)md_payload & 3)) {
			SPDK_ERRLOG("virt_addr %p not dword aligned\n", md_payload);
			goto exit;
		}

		if (sgl_supported && dword_aligned) {
			assert(req->cmd.psdt == SPDK_NVME_PSDT_SGL_MPTR_CONTIG);
			req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
			tr->meta_sgl.address = spdk_vtophys(md_payload, NULL);
			if (tr->meta_sgl.address == SPDK_VTOPHYS_ERROR) {
				goto exit;
			}
			tr->meta_sgl.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			tr->meta_sgl.unkeyed.length = req->md_size;
			tr->meta_sgl.unkeyed.subtype = 0;
			req->cmd.mptr = tr->prp_sgl_bus_addr - sizeof(struct spdk_nvme_sgl_descriptor);
		} else {
			req->cmd.mptr = spdk_vtophys(md_payload, NULL);
			if (req->cmd.mptr == SPDK_VTOPHYS_ERROR) {
				goto exit;
			}
		}
	}

	return 0;

exit:
	nvme_pcie_fail_request_bad_vtophys(qpair, tr);
	return -EINVAL;
}

static int
nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_tracker	*tr;
	int			rc = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	enum nvme_payload_type	payload_type;
	bool			sgl_supported;
	bool			dword_aligned = true;

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	}

	tr = TAILQ_FIRST(&pqpair->free_tr);

	if (tr == NULL) {
		/* Inform the upper layer to try again later. */
		rc = -EAGAIN;
		goto exit;
	}

	TAILQ_REMOVE(&pqpair->free_tr, tr, tq_list); /* remove tr from free_tr */
	TAILQ_INSERT_TAIL(&pqpair->outstanding_tr, tr, tq_list);
	tr->req = req;
	tr->cb_fn = req->cb_fn;
	tr->cb_arg = req->cb_arg;
	req->cmd.cid = tr->cid;

	if (req->payload_size != 0) {
		payload_type = nvme_payload_type(&req->payload);
		/* According to the specification, PRPs shall be used for all
		 *  Admin commands for NVMe over PCIe implementations.
		 */
		sgl_supported = (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) != 0 &&
				!nvme_qpair_is_admin_queue(qpair);

		if (sgl_supported && !(ctrlr->flags & SPDK_NVME_CTRLR_SGL_REQUIRES_DWORD_ALIGNMENT)) {
			dword_aligned = false;
		}
		rc = g_nvme_pcie_build_req_table[payload_type][sgl_supported](qpair, req, tr, dword_aligned);
		if (rc < 0) {
			goto exit;
		}

		rc = nvme_pcie_qpair_build_metadata(qpair, tr, sgl_supported, dword_aligned);
		if (rc < 0) {
			goto exit;
		}
	}

	nvme_pcie_qpair_submit_tracker(qpair, tr);

exit:
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
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
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
		assert(tr->req != NULL);

		if (nvme_request_check_timeout(tr->req, tr->cid, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

static int32_t
nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	struct spdk_nvme_cpl	*cpl, *next_cpl;
	uint32_t		 num_completions = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	uint16_t		 next_cq_head;
	uint8_t			 next_phase;
	bool			 next_is_valid = false;

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

		if (!next_is_valid && cpl->status.p != pqpair->flags.phase) {
			break;
		}

		if (spdk_likely(pqpair->cq_head + 1 != pqpair->num_entries)) {
			next_cq_head = pqpair->cq_head + 1;
			next_phase = pqpair->flags.phase;
		} else {
			next_cq_head = 0;
			next_phase = !pqpair->flags.phase;
		}
		next_cpl = &pqpair->cpl[next_cq_head];
		next_is_valid = (next_cpl->status.p == next_phase);
		if (next_is_valid) {
			__builtin_prefetch(&pqpair->tr[next_cpl->cid]);
		}

#ifdef __PPC64__
		/*
		 * This memory barrier prevents reordering of:
		 * - load after store from/to tr
		 * - load after load cpl phase and cpl cid
		 */
		spdk_mb();
#elif defined(__aarch64__)
		__asm volatile("dmb oshld" ::: "memory");
#endif

		if (spdk_unlikely(++pqpair->cq_head == pqpair->num_entries)) {
			pqpair->cq_head = 0;
			pqpair->flags.phase = !pqpair->flags.phase;
		}

		tr = &pqpair->tr[cpl->cid];
		/* Prefetch the req's STAILQ_ENTRY since we'll need to access it
		 * as part of putting the req back on the qpair's free list.
		 */
		__builtin_prefetch(&tr->req->stailq);
		pqpair->sq_head = cpl->sqhd;

		if (tr->req) {
			nvme_pcie_qpair_complete_tracker(qpair, tr, cpl, true);
		} else {
			SPDK_ERRLOG("cpl does not map to outstanding cmd\n");
			spdk_nvme_qpair_print_completion(qpair, cpl);
			assert(0);
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		nvme_pcie_qpair_ring_cq_doorbell(qpair);
	}

	if (pqpair->flags.delay_cmd_submit) {
		if (pqpair->last_sq_tail != pqpair->sq_tail) {
			nvme_pcie_qpair_ring_sq_doorbell(qpair);
			pqpair->last_sq_tail = pqpair->sq_tail;
		}
	}

	if (spdk_unlikely(ctrlr->timeout_enabled)) {
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

static struct spdk_nvme_transport_poll_group *
nvme_pcie_poll_group_create(void)
{
	struct nvme_pcie_poll_group *group = calloc(1, sizeof(*group));

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	return &group->group;
}

static int
nvme_pcie_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int
nvme_pcie_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int
nvme_pcie_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			 struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int
nvme_pcie_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			    struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int64_t
nvme_pcie_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	int32_t local_completions = 0;
	int64_t total_completions = 0;

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		disconnected_qpair_cb(qpair, tgroup->group->ctx);
	}

	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
		local_completions = spdk_nvme_qpair_process_completions(qpair, completions_per_qpair);
		if (local_completions < 0) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
			local_completions = 0;
		}
		total_completions += local_completions;
	}

	return total_completions;
}

static int
nvme_pcie_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	free(tgroup);

	return 0;
}

static struct spdk_pci_id nvme_pci_driver_id[] = {
	{
		.class_id = SPDK_PCI_CLASS_NVME,
		.vendor_id = SPDK_PCI_ANY_ID,
		.device_id = SPDK_PCI_ANY_ID,
		.subvendor_id = SPDK_PCI_ANY_ID,
		.subdevice_id = SPDK_PCI_ANY_ID,
	},
	{ .vendor_id = 0, /* sentinel */ },
};

SPDK_PCI_DRIVER_REGISTER(nvme, nvme_pci_driver_id,
			 SPDK_PCI_DRIVER_NEED_MAPPING | SPDK_PCI_DRIVER_WC_ACTIVATE);

const struct spdk_nvme_transport_ops pcie_ops = {
	.name = "PCIE",
	.type = SPDK_NVME_TRANSPORT_PCIE,
	.ctrlr_construct = nvme_pcie_ctrlr_construct,
	.ctrlr_scan = nvme_pcie_ctrlr_scan,
	.ctrlr_destruct = nvme_pcie_ctrlr_destruct,
	.ctrlr_enable = nvme_pcie_ctrlr_enable,

	.ctrlr_set_reg_4 = nvme_pcie_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_pcie_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_pcie_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_pcie_ctrlr_get_reg_8,

	.ctrlr_get_max_xfer_size = nvme_pcie_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_pcie_ctrlr_get_max_sges,

	.ctrlr_reserve_cmb = nvme_pcie_ctrlr_reserve_cmb,
	.ctrlr_map_cmb = nvme_pcie_ctrlr_map_io_cmb,
	.ctrlr_unmap_cmb = nvme_pcie_ctrlr_unmap_io_cmb,

	.ctrlr_create_io_qpair = nvme_pcie_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_pcie_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_pcie_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_pcie_ctrlr_disconnect_qpair,

	.qpair_abort_reqs = nvme_pcie_qpair_abort_reqs,
	.qpair_reset = nvme_pcie_qpair_reset,
	.qpair_submit_request = nvme_pcie_qpair_submit_request,
	.qpair_process_completions = nvme_pcie_qpair_process_completions,
	.qpair_iterate_requests = nvme_pcie_qpair_iterate_requests,
	.admin_qpair_abort_aers = nvme_pcie_admin_qpair_abort_aers,

	.poll_group_create = nvme_pcie_poll_group_create,
	.poll_group_connect_qpair = nvme_pcie_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_pcie_poll_group_disconnect_qpair,
	.poll_group_add = nvme_pcie_poll_group_add,
	.poll_group_remove = nvme_pcie_poll_group_remove,
	.poll_group_process_completions = nvme_pcie_poll_group_process_completions,
	.poll_group_destroy = nvme_pcie_poll_group_destroy,
};

SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops);
