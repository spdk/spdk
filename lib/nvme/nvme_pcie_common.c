/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 * NVMe over PCIe common library
 */

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include "spdk/trace.h"

#include "spdk_internal/trace_defs.h"

__thread struct nvme_pcie_ctrlr *g_thread_mmio_ctrlr = NULL;

static struct spdk_nvme_pcie_stat g_dummy_stat = {};

static void
nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr);

static inline uint64_t
nvme_pcie_vtophys(struct spdk_nvme_ctrlr *ctrlr, const void *buf, uint64_t *size)
{
	if (spdk_likely(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE)) {
		return spdk_vtophys(buf, size);
	} else {
		/* vfio-user address translation with IOVA=VA mode */
		return (uint64_t)(uintptr_t)buf;
	}
}

int
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

static void
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid, uint64_t phys_addr)
{
	tr->prp_sgl_bus_addr = phys_addr + offsetof(struct nvme_tracker, u.prp);
	tr->cid = cid;
	tr->req = NULL;
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

int
nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair,
			  const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	uint16_t		i;
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
			pqpair->cmd_bus_addr = nvme_pcie_vtophys(ctrlr, pqpair->cmd, NULL);
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
		pqpair->cpl_bus_addr =  nvme_pcie_vtophys(ctrlr, pqpair->cpl, NULL);
		if (pqpair->cpl_bus_addr == SPDK_VTOPHYS_ERROR) {
			SPDK_ERRLOG("spdk_vtophys(pqpair->cpl) failed\n");
			return -EFAULT;
		}
	}

	pqpair->sq_tdbl = pctrlr->doorbell_base + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
	pqpair->cq_hdbl = pctrlr->doorbell_base + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;

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
		nvme_qpair_construct_tracker(tr, i, nvme_pcie_vtophys(ctrlr, tr, NULL));
		TAILQ_INSERT_HEAD(&pqpair->free_tr, tr, tq_list);
	}

	nvme_pcie_qpair_reset(qpair);

	return 0;
}

int
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
	pqpair->pcie_state = NVME_PCIE_QPAIR_READY;

	ctrlr->adminq = &pqpair->qpair;

	rc = nvme_qpair_init(ctrlr->adminq,
			     0, /* qpair ID */
			     ctrlr,
			     SPDK_NVME_QPRIO_URGENT,
			     num_entries,
			     false);
	if (rc != 0) {
		return rc;
	}

	pqpair->stat = spdk_zmalloc(sizeof(*pqpair->stat), 64, NULL, SPDK_ENV_SOCKET_ID_ANY,
				    SPDK_MALLOC_SHARE);
	if (!pqpair->stat) {
		SPDK_ERRLOG("Failed to allocate admin qpair statistics\n");
		return -ENOMEM;
	}

	return nvme_pcie_qpair_construct(ctrlr->adminq, NULL);
}

/**
 * Note: the ctrlr_lock must be held when calling this function.
 */
void
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
void
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

int
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

int
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

int
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

int
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

static void
nvme_completion_sq_error_delete_cq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("delete_io_cq failed!\n");
	}

	pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
}

static void
nvme_completion_create_sq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	int rc;

	if (pqpair->flags.defer_destruction) {
		/* This qpair was deleted by the application while the
		 * connection was still in progress.  We had to wait
		 * to free the qpair resources until this outstanding
		 * command was completed.  Now that we have the completion
		 * free it now.
		 */
		nvme_pcie_qpair_destroy(qpair);
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("nvme_create_io_sq failed, deleting cq!\n");
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_sq_error_delete_cq_cb,
						      qpair);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to send request to delete_io_cq with rc=%d\n", rc);
			pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		}
		return;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_READY;
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

}

static void
nvme_completion_create_cq_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_qpair *qpair = arg;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	int rc;

	if (pqpair->flags.defer_destruction) {
		/* This qpair was deleted by the application while the
		 * connection was still in progress.  We had to wait
		 * to free the qpair resources until this outstanding
		 * command was completed.  Now that we have the completion
		 * free it now.
		 */
		nvme_pcie_qpair_destroy(qpair);
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		SPDK_ERRLOG("nvme_create_io_cq failed!\n");
		return;
	}

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_create_sq_cb, qpair);

	if (rc != 0) {
		SPDK_ERRLOG("Failed to send request to create_io_sq, deleting cq!\n");
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_sq_error_delete_cq_cb,
						      qpair);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to send request to delete_io_cq with rc=%d\n", rc);
			pqpair->pcie_state = NVME_PCIE_QPAIR_FAILED;
		}
		return;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_WAIT_FOR_SQ;
}

static int
_nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 uint16_t qid)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	int	rc;

	/* Statistics may already be allocated in the case of controller reset */
	if (!pqpair->stat) {
		if (qpair->poll_group) {
			struct nvme_pcie_poll_group *group = SPDK_CONTAINEROF(qpair->poll_group,
							     struct nvme_pcie_poll_group, group);

			pqpair->stat = &group->stats;
			pqpair->shared_stats = true;
		} else {
			pqpair->stat = calloc(1, sizeof(*pqpair->stat));
			if (!pqpair->stat) {
				SPDK_ERRLOG("Failed to allocate qpair statistics\n");
				nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
				return -ENOMEM;
			}
		}
	}


	rc = nvme_pcie_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_create_cq_cb, qpair);

	if (rc != 0) {
		SPDK_ERRLOG("Failed to send request to create_io_cq\n");
		nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
		return rc;
	}
	pqpair->pcie_state = NVME_PCIE_QPAIR_WAIT_FOR_CQ;
	return 0;
}

int
nvme_pcie_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;

	if (!nvme_qpair_is_admin_queue(qpair)) {
		rc = _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qpair->id);
	} else {
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
	}

	return rc;
}

void
nvme_pcie_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
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

void
nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;

	req = tr->req;
	assert(req != NULL);

	spdk_trace_record(TRACE_NVME_PCIE_SUBMIT, qpair->id, 0, (uintptr_t)req,
			  req->cmd.cid, req->cmd.opc, req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12);

	if (req->cmd.fuse) {
		/*
		 * Keep track of the fuse operation sequence so that we ring the doorbell only
		 * after the second fuse is submitted.
		 */
		qpair->last_fuse = req->cmd.fuse;
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

void
nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_pcie_qpair		*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_request		*req;
	bool				retry, error;
	bool				req_from_current_proc = true;
	bool				print_error;

	req = tr->req;

	spdk_trace_record(TRACE_NVME_PCIE_COMPLETE, qpair->id, 0, (uintptr_t)req, req->cmd.cid);

	assert(req != NULL);

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < pqpair->retry_count;
	print_error = error && print_on_error && !qpair->ctrlr->opts.disable_error_logging;

	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
	}

	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
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

void
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

void
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

void
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

void
nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

void
nvme_pcie_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	nvme_pcie_qpair_abort_trackers(qpair, dnr);
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

int32_t
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
	int			 rc;

	if (spdk_unlikely(pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED)) {
		return -ENXIO;
	}

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		if (pqpair->pcie_state == NVME_PCIE_QPAIR_READY) {
			/* It is possible that another thread set the pcie_state to
			 * QPAIR_READY, if it polled the adminq and processed the SQ
			 * completion for this qpair.  So check for that condition
			 * here and then update the qpair's state to CONNECTED, since
			 * we can only set the qpair state from the qpair's thread.
			 * (Note: this fixed issue #2157.)
			 */
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
			nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
			return -ENXIO;
		} else {
			rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
			if (rc < 0) {
				return rc;
			} else if (pqpair->pcie_state == NVME_PCIE_QPAIR_FAILED) {
				nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
				return -ENXIO;
			}
		}
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

	pqpair->stat->polls++;

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
		pqpair->stat->completions += num_completions;
		nvme_pcie_qpair_ring_cq_doorbell(qpair);
	} else {
		pqpair->stat->idle_polls++;
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

	if (spdk_unlikely(pqpair->flags.has_pending_vtophys_failures)) {
		struct nvme_tracker *tr, *tmp;

		TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, tmp) {
			if (tr->bad_vtophys) {
				tr->bad_vtophys = 0;
				nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			}
		}
		pqpair->flags.has_pending_vtophys_failures = 0;
	}

	return num_completions;
}

int
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

	if (!pqpair->shared_stats) {
		if (qpair->id) {
			free(pqpair->stat);
		} else {
			/* statistics of admin qpair are allocates from huge pages because
			 * admin qpair is shared for multi-process */
			spdk_free(pqpair->stat);
		}

	}

	spdk_free(pqpair);

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

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (pqpair == NULL) {
		return NULL;
	}

	pqpair->num_entries = opts->io_queue_size;
	pqpair->flags.delay_cmd_submit = opts->delay_cmd_submit;

	qpair = &pqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, opts->qprio, opts->io_queue_requests, opts->async_mode);
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

int
nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_completion_poll_status *status;
	int rc;

	assert(ctrlr != NULL);

	if (ctrlr->is_removed) {
		goto free;
	}

	if (ctrlr->prepare_for_reset) {
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING) {
			pqpair->flags.defer_destruction = true;
		}
		goto clear_shadow_doorbells;
	}

	/* If attempting to delete a qpair that's still being connected, we have to wait until it's
	 * finished, so that we don't free it while it's waiting for the create cq/sq callbacks.
	 */
	while (pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ ||
	       pqpair->pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ) {
		rc = spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		if (rc < 0) {
			break;
		}
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		goto free;
	}

	/* Delete the I/O submission queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send request to delete_io_sq with rc=%d\n", rc);
		free(status);
		goto free;
	}
	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		goto free;
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
		goto free;
	}
	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		goto free;
	}
	free(status);

clear_shadow_doorbells:
	if (pqpair->flags.has_shadow_doorbell) {
		*pqpair->shadow_doorbell.sq_tdbl = 0;
		*pqpair->shadow_doorbell.cq_hdbl = 0;
		*pqpair->shadow_doorbell.sq_eventidx = 0;
		*pqpair->shadow_doorbell.cq_eventidx = 0;
	}
free:
	if (qpair->no_deletion_notification_needed == 0) {
		/* Abort the rest of the I/O */
		nvme_pcie_qpair_abort_trackers(qpair, 1);
	}

	if (!pqpair->flags.defer_destruction) {
		nvme_pcie_qpair_destroy(qpair);
	}
	return 0;
}

static void
nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	if (!qpair->in_completion_context) {
		struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

		tr->bad_vtophys = 1;
		pqpair->flags.has_pending_vtophys_failures = 1;
		return;
	}

	/*
	 * Bad vtophys translation, so abort this request and return
	 *  immediately.
	 */
	SPDK_ERRLOG("vtophys or other payload buffer related error\n");
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
nvme_pcie_prp_list_append(struct spdk_nvme_ctrlr *ctrlr, struct nvme_tracker *tr,
			  uint32_t *prp_index, void *virt_addr, size_t len,
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

		phys_addr = nvme_pcie_vtophys(ctrlr, virt_addr, NULL);
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

	rc = nvme_pcie_prp_list_append(qpair->ctrlr, tr, &prp_index,
				       req->payload.contig_or_cb_arg + req->payload_offset,
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

		mapping_length = length;
		phys_addr = nvme_pcie_vtophys(qpair->ctrlr, virt_addr, &mapping_length);
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
			phys_addr = nvme_pcie_vtophys(qpair->ctrlr, virt_addr, &mapping_length);
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

		rc = nvme_pcie_prp_list_append(qpair->ctrlr, tr, &prp_index, virt_addr, length, page_size);
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
			tr->meta_sgl.address = nvme_pcie_vtophys(qpair->ctrlr, md_payload, NULL);
			if (tr->meta_sgl.address == SPDK_VTOPHYS_ERROR) {
				goto exit;
			}
			tr->meta_sgl.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			tr->meta_sgl.unkeyed.length = req->md_size;
			tr->meta_sgl.unkeyed.subtype = 0;
			req->cmd.mptr = tr->prp_sgl_bus_addr - sizeof(struct spdk_nvme_sgl_descriptor);
		} else {
			req->cmd.mptr = nvme_pcie_vtophys(qpair->ctrlr, md_payload, NULL);
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

int
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
		pqpair->stat->queued_requests++;
		/* Inform the upper layer to try again later. */
		rc = -EAGAIN;
		goto exit;
	}

	pqpair->stat->submitted_requests++;
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

		if (sgl_supported) {
			/* Don't use SGL for DSM command */
			if (spdk_unlikely((ctrlr->quirks & NVME_QUIRK_NO_SGL_FOR_DSM) &&
					  (req->cmd.opc == SPDK_NVME_OPC_DATASET_MANAGEMENT))) {
				sgl_supported = false;
			}
		}

		if (sgl_supported && !(ctrlr->flags & SPDK_NVME_CTRLR_SGL_REQUIRES_DWORD_ALIGNMENT)) {
			dword_aligned = false;
		}

		/* If we fail to build the request or the metadata, do not return the -EFAULT back up
		 * the stack.  This ensures that we always fail these types of requests via a
		 * completion callback, and never in the context of the submission.
		 */
		rc = g_nvme_pcie_build_req_table[payload_type][sgl_supported](qpair, req, tr, dword_aligned);
		if (rc < 0) {
			assert(rc == -EFAULT);
			rc = 0;
			goto exit;
		}

		rc = nvme_pcie_qpair_build_metadata(qpair, tr, sgl_supported, dword_aligned);
		if (rc < 0) {
			assert(rc == -EFAULT);
			rc = 0;
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

struct spdk_nvme_transport_poll_group *
nvme_pcie_poll_group_create(void)
{
	struct nvme_pcie_poll_group *group = calloc(1, sizeof(*group));

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	return &group->group;
}

int
nvme_pcie_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_pcie_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_pcie_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			 struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_pcie_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			    struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->stat = &g_dummy_stat;
	return 0;
}

int64_t
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

int
nvme_pcie_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	free(tgroup);

	return 0;
}

SPDK_TRACE_REGISTER_FN(nvme_pcie, "nvme_pcie", TRACE_GROUP_NVME_PCIE)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"NVME_PCIE_SUBMIT", TRACE_NVME_PCIE_SUBMIT,
			OWNER_NVME_PCIE_QP, OBJECT_NVME_PCIE_TR, 1,
			{	{ "cid", SPDK_TRACE_ARG_TYPE_INT, 8 },
				{ "opc", SPDK_TRACE_ARG_TYPE_INT, 8 },
				{ "dw10", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "dw11", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "dw12", SPDK_TRACE_ARG_TYPE_PTR, 8 }
			}
		},
		{
			"NVME_PCIE_COMPLETE", TRACE_NVME_PCIE_COMPLETE,
			OWNER_NVME_PCIE_QP, OBJECT_NVME_PCIE_TR, 0,
			{{ "cid", SPDK_TRACE_ARG_TYPE_INT, 8 }}
		},
	};

	spdk_trace_register_object(OBJECT_NVME_PCIE_TR, 'p');
	spdk_trace_register_owner(OWNER_NVME_PCIE_QP, 'q');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
