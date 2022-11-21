/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2017, IBM Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
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
#include "nvme_pcie_internal.h"

struct nvme_pcie_enum_ctx {
	struct spdk_nvme_probe_ctx *probe_ctx;
	struct spdk_pci_addr pci_addr;
	bool has_pci_addr;
};

static uint16_t g_signal_lock;
static bool g_sigset = false;
static spdk_nvme_pcie_hotplug_filter_cb g_hotplug_filter_cb;

static void
nvme_sigbus_fault_sighandler(const void *failure_addr, void *ctx)
{
	void *map_address;
	uint16_t flag = 0;

	if (!__atomic_compare_exchange_n(&g_signal_lock, &flag, 1, false, __ATOMIC_ACQUIRE,
					 __ATOMIC_RELAXED)) {
		SPDK_DEBUGLOG(nvme, "request g_signal_lock failed\n");
		return;
	}

	if (g_thread_mmio_ctrlr == NULL) {
		return;
	}

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
_nvme_pcie_event_process(struct spdk_pci_event *event, void *cb_ctx)
{
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr *ctrlr;

	if (event->action == SPDK_UEVENT_ADD) {
		if (spdk_process_is_primary()) {
			if (g_hotplug_filter_cb == NULL || g_hotplug_filter_cb(&event->traddr)) {
				/* The enumerate interface implement the add operation */
				spdk_pci_device_allow(&event->traddr);
			}
		}
	} else if (event->action == SPDK_UEVENT_REMOVE) {
		memset(&trid, 0, sizeof(trid));
		spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);

		if (spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &event->traddr) < 0) {
			SPDK_ERRLOG("Failed to format pci address\n");
			return;
		}

		ctrlr = nvme_get_ctrlr_by_trid_unsafe(&trid);
		if (ctrlr == NULL) {
			return;
		}
		SPDK_DEBUGLOG(nvme, "remove nvme address: %s\n", trid.traddr);

		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		nvme_ctrlr_fail(ctrlr, true);
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

		/* get the user app to clean up and stop I/O */
		if (ctrlr->remove_cb) {
			nvme_robust_mutex_unlock(&g_spdk_nvme_driver->lock);
			ctrlr->remove_cb(ctrlr->cb_ctx, ctrlr);
			nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
		}
	}
}

static int
_nvme_pcie_hotplug_monitor(struct spdk_nvme_probe_ctx *probe_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr, *tmp;
	struct spdk_pci_event event;

	if (g_spdk_nvme_driver->hotplug_fd >= 0) {
		while (spdk_pci_get_event(g_spdk_nvme_driver->hotplug_fd, &event) > 0) {
			_nvme_pcie_event_process(&event, probe_ctx->cb_ctx);
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
				ctrlr->remove_cb(ctrlr->cb_ctx, ctrlr);
				nvme_robust_mutex_lock(&g_spdk_nvme_driver->lock);
			}
		}
	}
	return 0;
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

static int
nvme_pcie_ctrlr_get_pmrcap(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrcap_register *pmrcap)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrcap.raw),
					 &pmrcap->raw);
}

static int
nvme_pcie_ctrlr_set_pmrctl(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrctl_register *pmrctl)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrctl.raw),
					 pmrctl->raw);
}

static int
nvme_pcie_ctrlr_get_pmrctl(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrctl_register *pmrctl)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrctl.raw),
					 &pmrctl->raw);
}

static int
nvme_pcie_ctrlr_get_pmrsts(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_pmrsts_register *pmrsts)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrsts.raw),
					 &pmrsts->raw);
}

static int
nvme_pcie_ctrlr_set_pmrmscl(struct nvme_pcie_ctrlr *pctrlr, uint32_t value)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrmscl.raw),
					 value);
}

static int
nvme_pcie_ctrlr_set_pmrmscu(struct nvme_pcie_ctrlr *pctrlr, uint32_t value)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, pmrmscu),
					 value);
}

static  uint32_t
nvme_pcie_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/*
	 * For commands requiring more than 2 PRP entries, one PRP will be
	 *  embedded in the command (prp1), and the rest of the PRP entries
	 *  will be in a list pointed to by the command (prp2).  The number
	 *  of PRP entries in the list is defined by
	 *  NVME_MAX_PRP_LIST_ENTRIES.
	 *
	 *  Note that the max xfer size is not (MAX_ENTRIES + 1) * page_size
	 *  because the first PRP entry may not be aligned on a 4KiB
	 *  boundary.
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

static void
nvme_pcie_ctrlr_map_pmr(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr = NULL;
	uint32_t bir;
	union spdk_nvme_pmrcap_register pmrcap;
	uint64_t bar_size = 0, bar_phys_addr = 0;

	if (!pctrlr->regs->cap.bits.pmrs) {
		return;
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		SPDK_ERRLOG("get registers failed\n");
		return;
	}

	bir = pmrcap.bits.bir;
	/* Values 2 3 4 5 are valid for BAR */
	if (bir > 5 || bir < 2) {
		SPDK_ERRLOG("invalid base indicator register value\n");
		return;
	}

	rc = spdk_pci_device_map_bar(pctrlr->devhandle, bir, &addr, &bar_phys_addr, &bar_size);
	if ((rc != 0) || addr == NULL) {
		SPDK_ERRLOG("could not map the bar %d\n", bir);
		return;
	}

	if (pmrcap.bits.cmss) {
		uint32_t pmrmscl, pmrmscu, cmse = 1;
		union spdk_nvme_pmrsts_register pmrsts;

		/* Enable Controller Memory Space */
		pmrmscl = (uint32_t)((bar_phys_addr & 0xFFFFF000ULL) | (cmse << 1));
		pmrmscu = (uint32_t)((bar_phys_addr >> 32ULL) & 0xFFFFFFFFULL);

		if (nvme_pcie_ctrlr_set_pmrmscu(pctrlr, pmrmscu)) {
			SPDK_ERRLOG("set_pmrmscu() failed\n");
			spdk_pci_device_unmap_bar(pctrlr->devhandle, bir, addr);
			return;
		}

		if (nvme_pcie_ctrlr_set_pmrmscl(pctrlr, pmrmscl)) {
			SPDK_ERRLOG("set_pmrmscl() failed\n");
			spdk_pci_device_unmap_bar(pctrlr->devhandle, bir, addr);
			return;
		}

		if (nvme_pcie_ctrlr_get_pmrsts(pctrlr, &pmrsts)) {
			SPDK_ERRLOG("get pmrsts failed\n");
			spdk_pci_device_unmap_bar(pctrlr->devhandle, bir, addr);
			return;
		}

		if (pmrsts.bits.cbai) {
			SPDK_ERRLOG("Controller Memory Space Enable Failure\n");
			SPDK_ERRLOG("CBA Invalid - Host Addresses cannot reference PMR\n");
		} else {
			SPDK_DEBUGLOG(nvme, "Controller Memory Space Enable Success\n");
			SPDK_DEBUGLOG(nvme, "Host Addresses can reference PMR\n");
		}
	}

	pctrlr->pmr.bar_va = addr;
	pctrlr->pmr.bar_pa = bar_phys_addr;
	pctrlr->pmr.size = pctrlr->ctrlr.pmr_size = bar_size;
}

static int
nvme_pcie_ctrlr_unmap_pmr(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	union spdk_nvme_pmrcap_register pmrcap;
	void *addr = pctrlr->pmr.bar_va;

	if (addr == NULL) {
		return rc;
	}

	if (pctrlr->pmr.mem_register_addr) {
		spdk_mem_unregister(pctrlr->pmr.mem_register_addr, pctrlr->pmr.mem_register_size);
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		SPDK_ERRLOG("get_pmrcap() failed\n");
		return -EIO;
	}

	if (pmrcap.bits.cmss) {
		if (nvme_pcie_ctrlr_set_pmrmscu(pctrlr, 0)) {
			SPDK_ERRLOG("set_pmrmscu() failed\n");
		}

		if (nvme_pcie_ctrlr_set_pmrmscl(pctrlr, 0)) {
			SPDK_ERRLOG("set_pmrmscl() failed\n");
		}
	}

	rc = spdk_pci_device_unmap_bar(pctrlr->devhandle, pmrcap.bits.bir, addr);

	return rc;
}

static int
nvme_pcie_ctrlr_config_pmr(struct spdk_nvme_ctrlr *ctrlr, bool enable)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	union spdk_nvme_pmrcap_register pmrcap;
	union spdk_nvme_pmrctl_register pmrctl;
	union spdk_nvme_pmrsts_register pmrsts;
	uint8_t pmrto, pmrtu;
	uint64_t timeout_in_ms, ticks_per_ms, timeout_in_ticks, now_ticks;

	if (!pctrlr->regs->cap.bits.pmrs) {
		SPDK_ERRLOG("PMR is not supported by the controller\n");
		return -ENOTSUP;
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		SPDK_ERRLOG("get registers failed\n");
		return -EIO;
	}

	pmrto = pmrcap.bits.pmrto;
	pmrtu = pmrcap.bits.pmrtu;

	if (pmrtu > 1) {
		SPDK_ERRLOG("PMR Time Units Invalid\n");
		return -EINVAL;
	}

	ticks_per_ms = spdk_get_ticks_hz() / 1000;
	timeout_in_ms = pmrto * (pmrtu ? (60 * 1000) : 500);
	timeout_in_ticks = timeout_in_ms * ticks_per_ms;

	if (nvme_pcie_ctrlr_get_pmrctl(pctrlr, &pmrctl)) {
		SPDK_ERRLOG("get pmrctl failed\n");
		return -EIO;
	}

	if (enable && pmrctl.bits.en != 0) {
		SPDK_ERRLOG("PMR is already enabled\n");
		return -EINVAL;
	} else if (!enable && pmrctl.bits.en != 1) {
		SPDK_ERRLOG("PMR is already disabled\n");
		return -EINVAL;
	}

	pmrctl.bits.en = enable;

	if (nvme_pcie_ctrlr_set_pmrctl(pctrlr, &pmrctl)) {
		SPDK_ERRLOG("set pmrctl failed\n");
		return -EIO;
	}

	now_ticks =  spdk_get_ticks();

	do {
		if (nvme_pcie_ctrlr_get_pmrsts(pctrlr, &pmrsts)) {
			SPDK_ERRLOG("get pmrsts failed\n");
			return -EIO;
		}

		if (pmrsts.bits.nrdy == enable &&
		    spdk_get_ticks() > now_ticks + timeout_in_ticks) {
			SPDK_ERRLOG("PMR Enable - Timed Out\n");
			return -ETIMEDOUT;
		}
	} while (pmrsts.bits.nrdy == enable);

	SPDK_DEBUGLOG(nvme, "PMR %s\n", enable ? "Enabled" : "Disabled");

	return 0;
}

static int
nvme_pcie_ctrlr_enable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_pcie_ctrlr_config_pmr(ctrlr, true);
}

static int
nvme_pcie_ctrlr_disable_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	return nvme_pcie_ctrlr_config_pmr(ctrlr, false);
}

static void *
nvme_pcie_ctrlr_map_io_pmr(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	union spdk_nvme_pmrcap_register pmrcap;
	uint64_t mem_register_start, mem_register_end;
	int rc;

	if (!pctrlr->regs->cap.bits.pmrs) {
		SPDK_ERRLOG("PMR is not supported by the controller\n");
		return NULL;
	}

	if (pctrlr->pmr.mem_register_addr != NULL) {
		*size = pctrlr->pmr.mem_register_size;
		return pctrlr->pmr.mem_register_addr;
	}

	*size = 0;

	if (pctrlr->pmr.bar_va == NULL) {
		SPDK_DEBUGLOG(nvme, "PMR not available\n");
		return NULL;
	}

	if (nvme_pcie_ctrlr_get_pmrcap(pctrlr, &pmrcap)) {
		SPDK_ERRLOG("get registers failed\n");
		return NULL;
	}

	/* Check if WDS / RDS is supported */
	if (!(pmrcap.bits.wds || pmrcap.bits.rds)) {
		return NULL;
	}

	/* If PMR is less than 4MiB in size then abort PMR mapping */
	if (pctrlr->pmr.size < (1ULL << 22)) {
		return NULL;
	}

	mem_register_start = _2MB_PAGE((uintptr_t)pctrlr->pmr.bar_va + VALUE_2MB - 1);
	mem_register_end = _2MB_PAGE((uintptr_t)pctrlr->pmr.bar_va + pctrlr->pmr.size);

	rc = spdk_mem_register((void *)mem_register_start, mem_register_end - mem_register_start);
	if (rc) {
		SPDK_ERRLOG("spdk_mem_register() failed\n");
		return NULL;
	}

	pctrlr->pmr.mem_register_addr = (void *)mem_register_start;
	pctrlr->pmr.mem_register_size = mem_register_end - mem_register_start;

	*size = pctrlr->pmr.mem_register_size;
	return pctrlr->pmr.mem_register_addr;
}

static int
nvme_pcie_ctrlr_unmap_io_pmr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	int rc;

	if (pctrlr->pmr.mem_register_addr == NULL) {
		return -ENXIO;
	}

	rc = spdk_mem_unregister(pctrlr->pmr.mem_register_addr, pctrlr->pmr.mem_register_size);

	if (rc == 0) {
		pctrlr->pmr.mem_register_addr = NULL;
		pctrlr->pmr.mem_register_size = 0;
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
	pctrlr->doorbell_base = (volatile uint32_t *)&pctrlr->regs->doorbell[0].sq_tdbl;
	nvme_pcie_ctrlr_map_cmb(pctrlr);
	nvme_pcie_ctrlr_map_pmr(pctrlr);

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

	rc = nvme_pcie_ctrlr_unmap_pmr(pctrlr);
	if (rc != 0) {
		SPDK_ERRLOG("nvme_ctrlr_unmap_pmr failed with error code %d\n", rc);
		return -1;
	}

	rc = nvme_pcie_ctrlr_unmap_cmb(pctrlr);
	if (rc != 0) {
		SPDK_ERRLOG("nvme_ctrlr_unmap_cmb failed with error code %d\n", rc);
		return -1;
	}

	if (addr && spdk_process_is_primary()) {
		/* NOTE: addr may have been remapped here. We're relying on DPDK to call
		 * munmap internally.
		 */
		rc = spdk_pci_device_unmap_bar(pctrlr->devhandle, 0, addr);
	}
	return rc;
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

static struct spdk_nvme_ctrlr *nvme_pcie_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct spdk_pci_device *pci_dev = devhandle;
	struct nvme_pcie_ctrlr *pctrlr;
	union spdk_nvme_cap_register cap;
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
	pctrlr->ctrlr.opts.admin_queue_size = spdk_max(pctrlr->ctrlr.opts.admin_queue_size,
					      NVME_PCIE_MIN_ADMIN_QUEUE_SIZE);

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
		spdk_pci_register_error_handler(nvme_sigbus_fault_sighandler,
						NULL);
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

void
spdk_nvme_pcie_set_hotplug_filter(spdk_nvme_pcie_hotplug_filter_cb filter_cb)
{
	g_hotplug_filter_cb = filter_cb;
}

static int
nvme_pcie_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_pcie_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	group = SPDK_CONTAINEROF(tgroup, struct nvme_pcie_poll_group, group);
	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for RDMA stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_PCIE;
	memcpy(&stats->pcie, &group->stats, sizeof(group->stats));

	*_stats = stats;

	return 0;
}

static void
nvme_pcie_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_transport_poll_group_stat *stats)
{
	free(stats);
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

	.ctrlr_enable_pmr = nvme_pcie_ctrlr_enable_pmr,
	.ctrlr_disable_pmr = nvme_pcie_ctrlr_disable_pmr,
	.ctrlr_map_pmr = nvme_pcie_ctrlr_map_io_pmr,
	.ctrlr_unmap_pmr = nvme_pcie_ctrlr_unmap_io_pmr,

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
	.poll_group_get_stats = nvme_pcie_poll_group_get_stats,
	.poll_group_free_stats = nvme_pcie_poll_group_free_stats
};

SPDK_NVME_TRANSPORT_REGISTER(pcie, &pcie_ops);
