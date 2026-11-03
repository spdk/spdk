/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "nvme/nvme_pcie_common.c"
#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;
DEFINE_STUB(nvme_ctrlr_get_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr, pid_t pid), NULL);

static bool g_capture_cq_commands;
static struct spdk_nvme_cmd g_cq_command;
static int g_cq_submit_rc;
static int g_cq_wait_rc;
static bool g_cq_wait_timeout;
static struct nvme_completion_poll_status *g_cq_timed_out_status;

int
nvme_ctrlr_submit_admin_request(struct spdk_nvme_ctrlr *ctrlr, struct nvme_request *req)
{
	if (g_capture_cq_commands) {
		g_cq_command = req->cmd;
		_nvme_free_request(req, ctrlr->adminq);
		return g_cq_submit_rc;
	}
	return 0;
}

DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));
int
nvme_wait_for_adminq_completion(struct spdk_nvme_ctrlr *ctrlr,
				struct nvme_completion_poll_status *status, bool release)
{
	if (g_capture_cq_commands) {
		CU_ASSERT(!release);
		status->timed_out = g_cq_wait_timeout;
		if (status->timed_out) {
			g_cq_timed_out_status = status;
		}
		return g_cq_wait_rc;
	}
	return 0;
}

DEFINE_STUB(nvme_completion_is_retry, bool, (const struct spdk_nvme_cpl *cpl), false);

DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));

DEFINE_STUB_V(spdk_nvme_qpair_print_completion_ext, (const struct spdk_nvme_qpair *qpair,
		const struct spdk_nvme_cpl *cpl, uint8_t opc));

DEFINE_STUB_V(nvme_qpair_deinit, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB_V(nvme_qpair_abort_all_queued_reqs, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB(nvme_qpair_check_enabled, bool, (struct spdk_nvme_qpair *qpair), true);

static struct spdk_nvme_ctrlr_process *g_current_process;

struct spdk_nvme_ctrlr_process *
nvme_ctrlr_get_current_process(struct spdk_nvme_ctrlr *ctrlr)
{
	return g_current_process;
}

DEFINE_STUB(spdk_nvme_qpair_process_completions, int32_t,
	    (struct spdk_nvme_qpair *qpair, uint32_t max_completions), 0);

DEFINE_STUB(nvme_request_check_timeout, int, (struct nvme_request *req, uint16_t cid,
		struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick), 0);
DEFINE_STUB(spdk_strerror, const char *, (int errnum), NULL);

static int g_nvme_ctrlr_disable_calls;
static struct spdk_nvme_ctrlr *g_nvme_ctrlr_disable_ctrlr;

void
nvme_ctrlr_disable(struct spdk_nvme_ctrlr *ctrlr)
{
	g_nvme_ctrlr_disable_calls++;
	g_nvme_ctrlr_disable_ctrlr = ctrlr;
}

static int g_nvme_ctrlr_disable_poll_calls;
static int g_nvme_ctrlr_disable_poll_rc;

int
nvme_ctrlr_disable_poll(struct spdk_nvme_ctrlr *ctrlr)
{
	g_nvme_ctrlr_disable_poll_calls++;
	return g_nvme_ctrlr_disable_poll_rc;
}

static int g_nvme_transport_ctrlr_disconnect_qpair_done_calls;
static struct spdk_nvme_qpair *g_nvme_transport_ctrlr_disconnect_qpair_done_qpair;

void
nvme_transport_ctrlr_disconnect_qpair_done(struct spdk_nvme_qpair *qpair)
{
	g_nvme_transport_ctrlr_disconnect_qpair_done_calls++;
	g_nvme_transport_ctrlr_disconnect_qpair_done_qpair = qpair;
	nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
}
DEFINE_STUB(spdk_nvme_ctrlr_get_numa_id, int32_t, (struct spdk_nvme_ctrlr *ctrlr),
	    SPDK_ENV_NUMA_ID_ANY);
DEFINE_STUB(nvme_qpair_state_string, const char *, (enum nvme_qpair_state state), NULL);
DEFINE_STUB(nvme_shared_cq_enabled, bool, (void), false);
static uint32_t g_cq_resubmits[4], g_cq_sw_maintenance[4];

void
nvme_qpair_resubmit_requests(struct spdk_nvme_qpair *qpair, uint32_t num_requests)
{
	CU_ASSERT(qpair->id < SPDK_COUNTOF(g_cq_resubmits));
	g_cq_resubmits[qpair->id] += num_requests;
}

void
nvme_qpair_process_sw_completions(struct spdk_nvme_qpair *qpair)
{
	CU_ASSERT(qpair->id < SPDK_COUNTOF(g_cq_sw_maintenance));
	g_cq_sw_maintenance[qpair->id]++;
}

int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		struct spdk_nvme_ctrlr *ctrlr,
		enum spdk_nvme_qprio qprio,
		uint32_t num_requests, bool async)
{
	qpair->id = id;
	qpair->qprio = qprio;
	qpair->ctrlr = ctrlr;
	qpair->async = async;

	return 0;
}

static void
test_nvme_pcie_standalone_cq_lifecycle(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req = {};
	struct spdk_nvme_cpl ring[8];
	struct spdk_nvme_io_cq_opts opts = {
		.io_queue_size = SPDK_COUNTOF(ring),
		.cq = {.vaddr = ring, .paddr = 0x12345000, .buffer_size = sizeof(ring)},
	};
	uint32_t doorbells[32] = {};
	struct spdk_nvme_cq *cq;
	struct nvme_pcie_cq *pcie_cq;
	uint32_t i;

	pctrlr.ctrlr.adminq = &adminq;
	pctrlr.ctrlr.opts.num_io_queues = 8;
	pctrlr.ctrlr.opts.io_queue_size = 8;
	pctrlr.ctrlr.opts.enable_interrupts = true;
	pctrlr.doorbell_base = doorbells;
	pctrlr.doorbell_stride_u32 = 2;
	STAILQ_INIT(&adminq.free_req);
	STAILQ_INSERT_HEAD(&adminq.free_req, &req, stailq);
	req.qpair = &adminq;
	g_capture_cq_commands = true;
	g_cq_submit_rc = g_cq_wait_rc = 0;
	g_cq_wait_timeout = false;
	memset(ring, 0xff, sizeof(ring));
	cq = nvme_pcie_ctrlr_create_io_cq(&pctrlr.ctrlr, 3, &opts);
	SPDK_CU_ASSERT_FATAL(cq != NULL);
	pcie_cq = &nvme_pcie_io_cq(cq)->pcie_cq;
	CU_ASSERT(g_cq_command.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(g_cq_command.cdw10_bits.create_io_q.qid == 3);
	CU_ASSERT(g_cq_command.cdw10_bits.create_io_q.qsize == 7);
	CU_ASSERT(g_cq_command.cdw11_bits.create_io_cq.ien == 1);
	CU_ASSERT(g_cq_command.cdw11_bits.create_io_cq.iv == 3);
	CU_ASSERT(g_cq_command.dptr.prp.prp1 == opts.cq.paddr);
	CU_ASSERT(pcie_cq->cq_hdbl == &doorbells[14]);
	CU_ASSERT(pcie_cq->sq_map_size == 9);
	CU_ASSERT(TAILQ_EMPTY(&pcie_cq->sq_members));
	for (i = 0; i < SPDK_COUNTOF(ring); i++) {
		CU_ASSERT(ring[i].status.p == 0);
		CU_ASSERT(pcie_cq->sq_map[i] == NULL);
		ring[i].status.p = 1;
	}
	pcie_cq->cq_head = 5;
	pcie_cq->phase = 0;
	CU_ASSERT(nvme_pcie_ctrlr_connect_io_cq(&pctrlr.ctrlr, cq) == 0);
	CU_ASSERT(pcie_cq->cq_head == 0);
	CU_ASSERT(pcie_cq->phase == 1);
	for (i = 0; i < SPDK_COUNTOF(ring); i++) {
		CU_ASSERT(ring[i].status.p == 0);
	}
	g_cq_wait_rc = -EIO;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_cq(&pctrlr.ctrlr, cq) == -EIO);
	CU_ASSERT(g_cq_command.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	CU_ASSERT(g_cq_command.cdw10_bits.delete_io_q.qid == 3);
	CU_ASSERT(pcie_cq->cpl == ring);
	g_cq_wait_rc = 0;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_cq(&pctrlr.ctrlr, cq) == 0);
	/* Caller memory must remain writable after deletion. */
	ring[0].status.p = 1;

	cq = nvme_pcie_ctrlr_create_io_cq(&pctrlr.ctrlr, 1, NULL);
	SPDK_CU_ASSERT_FATAL(cq != NULL);
	CU_ASSERT(nvme_pcie_io_cq(cq)->pcie_cq.cq_vaddr == NULL);
	pctrlr.ctrlr.is_removed = true;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_cq(&pctrlr.ctrlr, cq) == 0);
	pctrlr.ctrlr.is_removed = false;

	g_cq_wait_rc = -ETIMEDOUT;
	g_cq_wait_timeout = true;
	cq = nvme_pcie_ctrlr_create_io_cq(&pctrlr.ctrlr, 2, &opts);
	SPDK_CU_ASSERT_FATAL(cq != NULL);
	CU_ASSERT(pctrlr.ctrlr.io_cq_create_timed_out);
	CU_ASSERT(nvme_pcie_io_cq(cq)->pcie_cq.cpl == ring);
	free(g_cq_timed_out_status);
	g_cq_timed_out_status = NULL;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_cq(&pctrlr.ctrlr, cq) == -ETIMEDOUT);
	CU_ASSERT(pctrlr.ctrlr.io_cq_delete_timed_out);
	CU_ASSERT(nvme_pcie_io_cq(cq)->pcie_cq.cpl == ring);
	free(g_cq_timed_out_status);
	g_cq_timed_out_status = NULL;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_cq_force(&pctrlr.ctrlr, cq) == 0);
	g_cq_wait_rc = 0;
	g_cq_wait_timeout = false;
	pctrlr.ctrlr.io_cq_create_timed_out = false;
	g_cq_submit_rc = -ENOMEM;
	CU_ASSERT(nvme_pcie_ctrlr_create_io_cq(&pctrlr.ctrlr, 2, &opts) == NULL);
	g_cq_submit_rc = 0;
	MOCK_SET(spdk_vtophys, SPDK_VTOPHYS_ERROR);
	CU_ASSERT(nvme_pcie_ctrlr_create_io_cq(&pctrlr.ctrlr, 2, NULL) == NULL);
	MOCK_CLEAR(spdk_vtophys);
	opts.io_queue_size = UINT16_MAX + 1U;
	CU_ASSERT(nvme_pcie_ctrlr_create_io_cq(&pctrlr.ctrlr, 2, &opts) == NULL);
	g_capture_cq_commands = false;
}

static void
test_nvme_pcie_ctrlr_alloc_cmb(void)
{

	struct nvme_pcie_ctrlr pctrlr = {};
	void *vaddr = NULL;
	uint64_t alignment;
	uint64_t phys_addr_var;
	uint64_t size;

	size = 64;
	alignment = 4096;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	pctrlr.cmb.size = 1 << 16;

	/* Allocate CMB */
	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == (void *)0xF9001000);
	CU_ASSERT(phys_addr_var == 0xF8001000);
	CU_ASSERT(pctrlr.cmb.current_offset == 4160);

	/* CMB size overload */
	size = 0x1000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	SPDK_CU_ASSERT_FATAL(vaddr == NULL);

	/* BAR is mapped for data */
	pctrlr.cmb.mem_register_addr = (void *)0xF0000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == NULL);
}

static void
test_nvme_pcie_qpair_construct_destroy(void)
{
	struct spdk_nvme_io_qpair_opts opts = {};
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_nvme_cpl cpl[2] = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	size_t page_align = sysconf(_SC_PAGESIZE);
	uint64_t cmb_offset;
	int rc;

	opts.sq.paddr = 0xDEADBEEF;
	opts.cq.paddr = 0xDBADBEEF;
	opts.sq.vaddr = (void *)0xDCADBEEF;
	opts.cq.vaddr = cpl;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.transport_retry_count = 1;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	cmb_offset = pctrlr.cmb.current_offset;
	/* Make sure that CMB size is big enough and includes page alignment */
	pctrlr.cmb.size = (1 << 16) + page_align;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;

	/* Allocate memory for destroying. */
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cq_local.cpl = cpl;
	pqpair->qpair.trtype = SPDK_NVME_TRANSPORT_PCIE;

	/* Enable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = true;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->cq == &pqpair->cq_local);
	CU_ASSERT(pqpair->cq->num_entries == pqpair->num_entries);
	CU_ASSERT(pqpair->cq->cqid == pqpair->qpair.id);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == true);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == (((pctrlr.cmb.bar_pa + cmb_offset) + page_align - 1) & ~
					   (page_align - 1)));
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->cq->phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	CU_ASSERT(pctrlr.cmb.current_offset == (uintptr_t)pqpair->cmd + (pqpair->num_entries * sizeof(
				struct spdk_nvme_cmd)) - (uintptr_t)pctrlr.cmb.bar_va);
	cmb_offset = pctrlr.cmb.current_offset;
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cq_local.cpl = cpl;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDEADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->cq->phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer, sq_vaddr and cq_vaddr invalid. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cq_local.cpl = cpl;
	pqpair->qpair.trtype = SPDK_NVME_TRANSPORT_PCIE;
	MOCK_SET(spdk_vtophys, 0xDAADBEEF);

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDAADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq->cq_hdbl == (void *)0xF700000c);
	CU_ASSERT(pqpair->cq->phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);
	MOCK_CLEAR(spdk_vtophys);
}

static void
private_cq_test_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	uint32_t *completed = arg;

	CU_ASSERT(!spdk_nvme_cpl_is_error(cpl));
	(*completed)++;
}

static void
test_nvme_pcie_private_cq_wrap_reset(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair *pqpair;
	struct nvme_request req = {};
	struct nvme_tracker *tr;
	struct spdk_nvme_pcie_stat stat = {};
	uint32_t doorbell = 0, shadow = 0, eventidx = 0;
	uint32_t completed = 0;
	uint16_t tail = 0;
	uint8_t phase = 1;
	unsigned int i;
	int rc;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->qpair.id = 1;
	pqpair->num_entries = 4;
	rc = nvme_pcie_qpair_construct(&pqpair->qpair, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(pqpair->cq == &pqpair->cq_local);
	CU_ASSERT(pqpair->cq->cqid == 1);
	CU_ASSERT(pqpair->cq->num_entries == 4);
	pqpair->qpair.state = NVME_QPAIR_ENABLED;
	STAILQ_INIT(&pqpair->qpair.free_req);
	pqpair->stat = &stat;
	pqpair->shared_stats = true;
	pqpair->cq->cq_hdbl = &doorbell;
	tr = &pqpair->tr[0];
	req.qpair = &pqpair->qpair;
	req.cmd.cid = tr->cid;

	for (i = 0; i < 12; i++) {
		pqpair->qpair.num_outstanding_reqs = 1;
		pqpair->qpair.queue_depth = 1;
		tr->req = &req;
		tr->cb_fn = private_cq_test_cb;
		tr->cb_arg = &completed;
		TAILQ_REMOVE(&pqpair->free_tr, tr, tq_list);
		TAILQ_INSERT_TAIL(&pqpair->outstanding_tr, tr, tq_list);
		pqpair->cq->cpl[tail].cid = tr->cid;
		pqpair->cq->cpl[tail].status.p = phase;
		rc = nvme_pcie_qpair_process_completions(&pqpair->qpair, 0);
		if (++tail == 4) {
			tail = 0;
			phase = !phase;
		}
		CU_ASSERT(rc == 1);
		CU_ASSERT(completed == i + 1);
		CU_ASSERT(pqpair->cq->cq_head == tail);
		CU_ASSERT(pqpair->cq->phase == phase);
		CU_ASSERT(doorbell == tail);
		CU_ASSERT(tr->req == NULL);
		CU_ASSERT(TAILQ_FIRST(&pqpair->free_tr) == tr);
		CU_ASSERT(TAILQ_EMPTY(&pqpair->outstanding_tr));
		STAILQ_INIT(&pqpair->qpair.free_req);
	}
	CU_ASSERT(stat.cq_mmio_doorbell_updates == 12);
	pqpair->cq->shadow_cq_hdbl = &shadow;
	pqpair->cq->shadow_cq_eventidx = &eventidx;
	pqpair->flags.has_shadow_doorbell = 1;
	pqpair->cq->cq_head = 2;
	nvme_pcie_qpair_ring_cq_doorbell(&pqpair->qpair);
	CU_ASSERT(shadow == 2);
	CU_ASSERT(stat.cq_shadow_doorbell_updates == 1);

	pqpair->sq_tail = 2;
	pqpair->sq_head = 1;
	pqpair->last_sq_tail = 2;
	CU_ASSERT(nvme_pcie_qpair_reset(&pqpair->qpair) == 0);
	CU_ASSERT(pqpair->sq_tail == 0);
	CU_ASSERT(pqpair->sq_head == 0);
	CU_ASSERT(pqpair->last_sq_tail == 0);
	CU_ASSERT(pqpair->cq->cq_head == 0);
	CU_ASSERT(pqpair->cq->phase == 1);
	for (i = 0; i < 4; i++) {
		CU_ASSERT(pqpair->cq->cpl[i].status.p == 0);
	}
	CU_ASSERT(pqpair->cq->cqid == 1);
	nvme_pcie_qpair_destroy(&pqpair->qpair);
}

static void
test_nvme_pcie_shared_sq_lifecycle(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_nvme_qpair adminq = {}, *sq1, *sq2;
	struct nvme_request req = {};
	struct spdk_nvme_io_qpair_opts opts = {
		.io_queue_size = 16,
		.io_queue_requests = 16,
		.create_only = true,
	};
	struct spdk_nvme_io_cq_opts cq_opts = {.io_queue_size = 8};
	struct spdk_nvme_cpl cpl = {};
	struct spdk_nvme_cq *cq;
	struct nvme_pcie_cq *pcie_cq;
	struct spdk_nvme_transport_poll_group *tgroup;
	uint32_t doorbells[16] = {};

	pctrlr.ctrlr.adminq = &adminq;
	pctrlr.ctrlr.page_size = 4096;
	pctrlr.ctrlr.opts.num_io_queues = 4;
	pctrlr.doorbell_base = doorbells;
	pctrlr.doorbell_stride_u32 = 1;
	STAILQ_INIT(&adminq.free_req);
	STAILQ_INSERT_HEAD(&adminq.free_req, &req, stailq);
	req.qpair = &adminq;
	g_capture_cq_commands = true;
	g_cq_wait_rc = g_cq_submit_rc = 0;
	g_cq_wait_timeout = false;
	cq = nvme_pcie_ctrlr_create_io_cq(&pctrlr.ctrlr, 3, &cq_opts);
	SPDK_CU_ASSERT_FATAL(cq != NULL);
	cq->ctrlr = &pctrlr.ctrlr;
	pcie_cq = &nvme_pcie_io_cq(cq)->pcie_cq;
	pcie_cq->cq_head = 5;
	pcie_cq->phase = 0;
	sq1 = nvme_pcie_ctrlr_create_io_sq(&pctrlr.ctrlr, cq, 1, &opts);
	SPDK_CU_ASSERT_FATAL(sq1 != NULL);
	sq2 = nvme_pcie_ctrlr_create_io_sq(&pctrlr.ctrlr, cq, 2, &opts);
	SPDK_CU_ASSERT_FATAL(sq2 != NULL);
	CU_ASSERT(pcie_cq->cq_head == 5);
	CU_ASSERT(pcie_cq->phase == 0);
	CU_ASSERT(cq->refcount == 2);
	CU_ASSERT(pcie_cq->refcount == 2);
	CU_ASSERT(pcie_cq->sq_map[1] == nvme_pcie_qpair(sq1));
	CU_ASSERT(pcie_cq->sq_map[2] == nvme_pcie_qpair(sq2));
	CU_ASSERT(nvme_pcie_qpair(sq1)->cq == nvme_pcie_qpair(sq2)->cq);
	CU_ASSERT(nvme_pcie_qpair(sq1)->cq_local.cpl == NULL);
	CU_ASSERT(nvme_pcie_ctrlr_create_io_sq(&pctrlr.ctrlr, cq, 1, &opts) == NULL);
	tgroup = nvme_pcie_poll_group_create();
	SPDK_CU_ASSERT_FATAL(tgroup != NULL);
	STAILQ_INIT(&tgroup->connected_qpairs);
	STAILQ_INIT(&tgroup->disconnected_qpairs);
	sq1->poll_group = sq2->poll_group = tgroup;
	CU_ASSERT(nvme_pcie_poll_group_add(tgroup, sq1) == 0);
	CU_ASSERT(nvme_pcie_poll_group_add(tgroup, sq2) == 0);
	CU_ASSERT(nvme_pcie_poll_group_connect_qpair(sq1) == 0);
	CU_ASSERT(nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, sq1) == 0);
	CU_ASSERT(g_cq_command.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(g_cq_command.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(g_cq_command.cdw10_bits.create_io_q.qsize == 15);
	CU_ASSERT(g_cq_command.cdw11_bits.create_io_sq.cqid == 3);
	nvme_completion_create_sq_cb(sq1, &cpl);
	CU_ASSERT(nvme_pcie_qpair(sq1)->flags.sq_created);
	CU_ASSERT(pcie_cq->cq_head == 5);
	CU_ASSERT(pcie_cq->phase == 0);
	g_cq_wait_rc = -EIO;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_qpair(&pctrlr.ctrlr, sq1) == -EIO);
	CU_ASSERT(g_cq_command.opc == SPDK_NVME_OPC_DELETE_IO_SQ);
	CU_ASSERT(cq->refcount == 2);
	CU_ASSERT(pcie_cq->sq_map[1] == nvme_pcie_qpair(sq1));
	CU_ASSERT(pcie_cq->poll_group_qpair_count == 2);
	g_cq_wait_rc = 0;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_qpair(&pctrlr.ctrlr, sq1) == 0);
	CU_ASSERT(cq->refcount == 1);
	CU_ASSERT(pcie_cq->sq_map[1] == NULL);
	CU_ASSERT(pcie_cq->poll_group_qpair_count == 1);
	CU_ASSERT(pcie_cq->poll_group_connected_count == 0);
	CU_ASSERT(pcie_cq->owner_tgroup == tgroup);
	/* A never-connected SQ has no device queue to delete. */
	g_cq_command.opc = 0;
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_qpair(&pctrlr.ctrlr, sq2) == 0);
	CU_ASSERT(g_cq_command.opc == 0);
	CU_ASSERT(cq->refcount == 0);
	CU_ASSERT(TAILQ_EMPTY(&pcie_cq->sq_members));
	CU_ASSERT(pcie_cq->poll_group == NULL && pcie_cq->owner_tgroup == NULL);
	CU_ASSERT(nvme_pcie_poll_group_destroy(tgroup) == 0);
	CU_ASSERT(nvme_pcie_ctrlr_delete_io_cq(&pctrlr.ctrlr, cq) == 0);
	g_capture_cq_commands = false;
}

static void
test_nvme_pcie_shared_cq_routing(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_io_cq pcq = {};
	struct nvme_pcie_cq *cq = &pcq.pcie_cq;
	struct nvme_pcie_qpair *members[2], *map[4] = {};
	struct spdk_nvme_cpl ring[8] = {};
	struct nvme_request reqs[2] = {};
	struct spdk_nvme_pcie_stat stats[2] = {};
	struct spdk_nvme_ctrlr_process foreign = {};
	struct spdk_nvme_transport_poll_group tgroup = {};
	uint32_t doorbells[16] = {}, completed[2] = {};
	uint32_t i, n, before;
	struct nvme_tracker *tr;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.page_size = 4096;
	pctrlr.doorbell_base = doorbells;
	pctrlr.doorbell_stride_u32 = 1;
	pcq.cq.ctrlr = &pctrlr.ctrlr;
	cq->num_entries = SPDK_COUNTOF(ring);
	cq->phase = 1;
	cq->cqid = 3;
	cq->cpl = ring;
	cq->cq_hdbl = &doorbells[7];
	cq->sq_map = map;
	cq->sq_map_size = SPDK_COUNTOF(map);
	TAILQ_INIT(&cq->sq_members);
	memset(g_cq_resubmits, 0, sizeof(g_cq_resubmits));
	memset(g_cq_sw_maintenance, 0, sizeof(g_cq_sw_maintenance));
	MOCK_SET(nvme_shared_cq_enabled, true);
	for (n = 0; n < 2; n++) {
		members[n] = spdk_zmalloc(sizeof(*members[n]), 64, NULL,
					  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
		SPDK_CU_ASSERT_FATAL(members[n] != NULL);
		members[n]->qpair.ctrlr = &pctrlr.ctrlr;
		members[n]->qpair.id = n + 1;
		members[n]->num_entries = 4;
		CU_ASSERT(nvme_pcie_qpair_construct(&members[n]->qpair, NULL) == 0);
		members[n]->cq = cq;
		members[n]->stat = &stats[n];
		members[n]->shared_stats = true;
		members[n]->qpair.state = NVME_QPAIR_ENABLED;
		STAILQ_INIT(&members[n]->qpair.free_req);
		map[n + 1] = members[n];
		TAILQ_INSERT_TAIL(&cq->sq_members, members[n], cq_member_link);
		reqs[n].qpair = &members[n]->qpair;
	}
	for (i = 0; i < 24; i++) {
		n = i % 2;
		tr = &members[n]->tr[0];
		tr->req = &reqs[n];
		tr->cb_fn = private_cq_test_cb;
		tr->cb_arg = &completed[n];
		members[n]->qpair.num_outstanding_reqs = 1;
		members[n]->qpair.queue_depth = 1;
		TAILQ_REMOVE(&members[n]->free_tr, tr, tq_list);
		TAILQ_INSERT_TAIL(&members[n]->outstanding_tr, tr, tq_list);
		ring[cq->cq_head].sqid = n + 1;
		ring[cq->cq_head].cid = 0;
		ring[cq->cq_head].sqhd = (i / 2) % 4;
		ring[cq->cq_head].status.p = cq->phase;
		CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == 1);
		CU_ASSERT(completed[n] == i / 2 + 1);
		CU_ASSERT(g_cq_resubmits[n + 1] == completed[n]);
		CU_ASSERT(members[n]->sq_head == (i / 2) % 4);
		CU_ASSERT(tr->req == NULL);
		CU_ASSERT(!members[n]->qpair.in_completion_context);
		CU_ASSERT(doorbells[7] == cq->cq_head);
		STAILQ_INIT(&members[n]->qpair.free_req);
	}
	CU_ASSERT(cq->cq_head == 0);
	CU_ASSERT(cq->phase == 0);
	CU_ASSERT(cq->completions == 24);
	CU_ASSERT(g_cq_sw_maintenance[1] == 24);
	CU_ASSERT(g_cq_sw_maintenance[2] == 24);
	tr = &members[0]->tr[0];
	tr->req = &reqs[0];
	members[0]->qpair.num_outstanding_reqs = 1;
	members[0]->qpair.queue_depth = 1;
	members[0]->retry_count = 1;
	members[0]->flags.delay_cmd_submit = 1;
	TAILQ_REMOVE(&members[0]->free_tr, tr, tq_list);
	TAILQ_INSERT_TAIL(&members[0]->outstanding_tr, tr, tq_list);
	ring[cq->cq_head].sqid = 1;
	ring[cq->cq_head].cid = 0;
	ring[cq->cq_head].status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	ring[cq->cq_head].status.p = cq->phase;
	before = g_cq_resubmits[1];
	MOCK_SET(nvme_completion_is_retry, true);
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == 1);
	CU_ASSERT(tr->req == &reqs[0]);
	CU_ASSERT(reqs[0].retries == 1);
	CU_ASSERT(g_cq_resubmits[1] == before);
	CU_ASSERT(members[0]->last_sq_tail == members[0]->sq_tail);
	MOCK_SET(nvme_completion_is_retry, false);
	ring[cq->cq_head].sqid = 1;
	ring[cq->cq_head].cid = 0;
	ring[cq->cq_head].status.sc = 0;
	ring[cq->cq_head].status.p = cq->phase;
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == 1);
	CU_ASSERT(tr->req == NULL);
	CU_ASSERT(g_cq_resubmits[1] == before + 1);
	CU_ASSERT(completed[0] == 13);
	/* Malformed SQIDs/CIDs must be consumed without indexing past either table. */
	for (i = 0; i < SPDK_COUNTOF(ring); i++) {
		ring[i].sqid = i == 0 ? 1 : UINT16_MAX;
		ring[i].cid = UINT16_MAX;
		ring[i].status.p = i >= cq->cq_head ? cq->phase : !cq->phase;
	}
	before = g_cq_resubmits[1];
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == 7);
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == 1);
	CU_ASSERT(g_cq_resubmits[1] == before);
	CU_ASSERT(cq->phase == 1);
	cq->owner_tgroup = &tgroup;
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == -EBUSY);
	cq->owner_tgroup = NULL;
	cq->owner_proc = &foreign;
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == -EPERM);
	cq->owner_proc = NULL;
	cq->in_completion_context = true;
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == -EBUSY);
	cq->in_completion_context = false;
	pctrlr.ctrlr.is_failed = true;
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == -ENXIO);
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_removed = true;
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == -ENXIO);
	pctrlr.ctrlr.is_removed = false;
	for (n = 0; n < 2; n++) {
		members[n]->qpair.state = NVME_QPAIR_DISCONNECTED;
	}
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == 0);
	for (n = 0; n < 2; n++) {
		members[n]->cq = &members[n]->cq_local;
		nvme_pcie_qpair_destroy(&members[n]->qpair);
	}
	MOCK_SET(nvme_shared_cq_enabled, false);
}

static void
test_nvme_pcie_ctrlr_cmd_create_delete_io_queue(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_pcie_qpair pqpair = { .cq = &pqpair.cq_local };
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req = {};
	int rc;

	ctrlr.adminq = &adminq;
	STAILQ_INIT(&ctrlr.adminq->free_req);
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.cq->cqid = 1;
	pqpair.cq->cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;

	rc = nvme_pcie_ctrlr_cmd_create_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0xDEADBEEF);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0xDDADBEEF);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	/* No free request available */
	rc = nvme_pcie_ctrlr_cmd_create_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	/* Delete cq or sq */
	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	CU_ASSERT(req.cmd.cdw10_bits.delete_io_q.qid == 1);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_DELETE_IO_SQ);
	CU_ASSERT(req.cmd.cdw10_bits.delete_io_q.qid == 1);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	/* No free request available */
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_nvme_pcie_ctrlr_connect_qpair(void)
{
	struct nvme_pcie_ctrlr	pctrlr = {};
	struct nvme_pcie_qpair	pqpair = { .cq = &pqpair.cq_local };
	struct spdk_nvme_transport_poll_group poll_group = {};
	struct spdk_nvme_cpl cpl = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req[3] = {};
	int rc;

	pqpair.cq->cpl = &cpl;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.cq->cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	pctrlr.ctrlr.page_size = 4096;

	/* Shadow doorbell available */
	pctrlr.doorbell_stride_u32 = 1;
	pctrlr.ctrlr.shadow_doorbell = spdk_zmalloc(pctrlr.ctrlr.page_size, pctrlr.ctrlr.page_size,
				       NULL, SPDK_ENV_LCORE_ID_ANY,
				       SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	pctrlr.ctrlr.eventidx = spdk_zmalloc(pctrlr.ctrlr.page_size, pctrlr.ctrlr.page_size,
					     NULL, SPDK_ENV_LCORE_ID_ANY,
					     SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	pctrlr.ctrlr.adminq = &adminq;
	STAILQ_INIT(&pctrlr.ctrlr.adminq->free_req);
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[0].cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req[0].cmd.dptr.prp.prp1 == 0xDEADBEEF);

	/* Complete the first request, which triggers the second. */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req[1].cmd.dptr.prp.prp1 == 0xDDADBEEF);

	pqpair.qpair.state = NVME_QPAIR_CONNECTING;
	/* Complete the second request */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_READY);
	/* State is still CONNECTING until the thread is polled again. */
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_CONNECTING);

	/* doorbell stride and qid are 1 */
	CU_ASSERT(pqpair.shadow_doorbell.sq_tdbl == pctrlr.ctrlr.shadow_doorbell + 2);
	CU_ASSERT(pqpair.cq->shadow_cq_hdbl == pctrlr.ctrlr.shadow_doorbell + 3);
	CU_ASSERT(pqpair.shadow_doorbell.sq_eventidx == pctrlr.ctrlr.eventidx + 2);
	CU_ASSERT(pqpair.cq->shadow_cq_eventidx == pctrlr.ctrlr.eventidx + 3);
	CU_ASSERT(pqpair.flags.has_shadow_doorbell == 1);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	spdk_free(pctrlr.ctrlr.shadow_doorbell);
	spdk_free(pctrlr.ctrlr.eventidx);
	pctrlr.ctrlr.shadow_doorbell = NULL;
	pctrlr.ctrlr.eventidx = NULL;

	/* Shadow doorbell 0 */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cq = &pqpair.cq_local;
	pqpair.cq->cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.cq->cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[0].cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req[0].cmd.dptr.prp.prp1 == 0xDEADBEEF);

	/* Complete the first request, which triggers the second. */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req[1].cmd.dptr.prp.prp1 == 0xDDADBEEF);

	pqpair.qpair.state = NVME_QPAIR_CONNECTING;
	/* Complete the second request */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_READY);
	/* State is still CONNECTING until the thread is polled again. */
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_CONNECTING);

	CU_ASSERT(pqpair.shadow_doorbell.sq_tdbl == NULL);
	CU_ASSERT(pqpair.shadow_doorbell.sq_eventidx == NULL);
	CU_ASSERT(pqpair.flags.has_shadow_doorbell == 0);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	/* Completion error for CQ */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cq = &pqpair.cq_local;
	pqpair.cq->cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.cq->cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	/* Modify cpl such that CQ fails */
	pqpair.cq->cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cq->cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);

	/* Request to complete callback in async operation */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_FAILED);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);

	/* Remove unused request */
	STAILQ_REMOVE_HEAD(&pctrlr.ctrlr.adminq->free_req, stailq);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	/* Completion error for SQ */
	memset(req, 0, sizeof(struct nvme_request) * 3);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cq = &pqpair.cq_local;
	pqpair.cq->cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.cq->cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.cq->cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cq->cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	pqpair.qpair.poll_group = &poll_group;
	for (int i = 0; i < 3; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ);

	/* Request to complete cq callback in async operation */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ);
	/* Modify cpl such that SQ fails */
	pqpair.cq->cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cq->cpl->status.sct = SPDK_NVME_SCT_GENERIC;

	/* Request to complete sq callback in async operation */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(req[2].cmd.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	/* Modify cpl back to success */
	pqpair.cq->cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cq->cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	req[2].cb_fn(req[2].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_FAILED);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);
	/* No need to remove unused requests here */
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));


	/* No available request used */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cq = &pqpair.cq_local;
	pqpair.cq->cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.cq->cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == -ENOMEM);
}

struct adminq_disconnect_ctx {
	bool called;
	uint16_t sct;
	uint16_t sc;
	uint16_t dnr;
};

static void
adminq_disconnect_req_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct adminq_disconnect_ctx *ctx = arg;

	ctx->called = true;
	ctx->sct = cpl->status.sct;
	ctx->sc = cpl->status.sc;
	ctx->dnr = cpl->status.dnr;
}

static void
reset_adminq_disconnect_mocks(void)
{
	g_current_process = NULL;
	g_nvme_ctrlr_disable_calls = 0;
	g_nvme_ctrlr_disable_ctrlr = NULL;
	g_nvme_ctrlr_disable_poll_calls = 0;
	g_nvme_ctrlr_disable_poll_rc = 0;
	g_nvme_transport_ctrlr_disconnect_qpair_done_calls = 0;
	g_nvme_transport_ctrlr_disconnect_qpair_done_qpair = NULL;
}

static void
test_nvme_pcie_ctrlr_disconnect_admin_qpair(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair pqpair = { .cq = &pqpair.cq_local };
	struct spdk_nvme_ctrlr_process proc = {};
	struct spdk_nvme_pcie_stat stat = {};
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tracker tr = {};
	struct nvme_request req = {};
	struct adminq_disconnect_ctx cb_ctx = {};
	int32_t rc;

	reset_adminq_disconnect_mocks();
	pthread_mutex_init(&pctrlr.ctrlr.ctrlr_lock, NULL);

	STAILQ_INIT(&proc.active_reqs);
	g_current_process = &proc;

	/* Model an enabled admin qpair with one command submitted to hardware. */
	pctrlr.ctrlr.adminq = &pqpair.qpair;
	pctrlr.ctrlr.is_disconnecting = true;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 0;
	pqpair.qpair.abort_dnr = 1;
	pqpair.qpair.num_outstanding_reqs = 1;
	pqpair.qpair.queue_depth = 1;
	pqpair.qpair.state = NVME_QPAIR_ENABLED;
	STAILQ_INIT(&pqpair.qpair.free_req);
	STAILQ_INIT(&pqpair.qpair.queued_req);
	TAILQ_INIT(&pqpair.qpair.err_cmd_head);

	pqpair.cq->cpl = &cpl;
	pqpair.num_entries = 1;
	pqpair.cq->num_entries = 1;
	pqpair.max_completions_cap = 1;
	pqpair.cq->phase = 1;
	pqpair.stat = &stat;
	TAILQ_INIT(&pqpair.free_tr);
	TAILQ_INIT(&pqpair.outstanding_tr);

	/* The tracker represents a stuck admin command that never posts a CQE. */
	tr.cid = 1;
	tr.req = &req;
	tr.cb_fn = adminq_disconnect_req_cb;
	tr.cb_arg = &cb_ctx;
	req.qpair = &pqpair.qpair;
	req.cmd.cid = tr.cid;
	/* Same-process admin completions invoke the request callback directly. */
	req.pid = getpid();
	TAILQ_INSERT_TAIL(&pqpair.outstanding_tr, &tr, tq_list);

	/* The common transport layer sets DISCONNECTING before calling into PCIe. */
	nvme_qpair_set_state(&pqpair.qpair, NVME_QPAIR_DISCONNECTING);
	g_nvme_ctrlr_disable_poll_rc = -EAGAIN;
	nvme_pcie_ctrlr_disconnect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(g_nvme_ctrlr_disable_calls == 1);
	CU_ASSERT(g_nvme_ctrlr_disable_ctrlr == &pctrlr.ctrlr);
	CU_ASSERT(g_nvme_transport_ctrlr_disconnect_qpair_done_calls == 0);

	/* While controller disable is still in progress, the tracker stays outstanding. */
	rc = nvme_pcie_qpair_process_completions(&pqpair.qpair, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_nvme_ctrlr_disable_poll_calls == 1);
	CU_ASSERT(g_nvme_transport_ctrlr_disconnect_qpair_done_calls == 0);
	CU_ASSERT(cb_ctx.called == false);
	CU_ASSERT(!TAILQ_EMPTY(&pqpair.outstanding_tr));

	/* Once disable completes, PCIe aborts outstanding admin trackers before disconnect done. */
	g_nvme_ctrlr_disable_poll_rc = 0;
	rc = nvme_pcie_qpair_process_completions(&pqpair.qpair, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_nvme_ctrlr_disable_poll_calls == 2);
	CU_ASSERT(g_nvme_transport_ctrlr_disconnect_qpair_done_calls == 1);
	CU_ASSERT(g_nvme_transport_ctrlr_disconnect_qpair_done_qpair == &pqpair.qpair);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);
	CU_ASSERT(cb_ctx.called == true);
	CU_ASSERT(cb_ctx.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(cb_ctx.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION);
	CU_ASSERT(cb_ctx.dnr == 1);
	CU_ASSERT(TAILQ_EMPTY(&pqpair.outstanding_tr));
	CU_ASSERT(TAILQ_FIRST(&pqpair.free_tr) == &tr);
	CU_ASSERT(STAILQ_FIRST(&pqpair.qpair.free_req) == &req);
	CU_ASSERT(pqpair.qpair.num_outstanding_reqs == 0);
	CU_ASSERT(pqpair.qpair.queue_depth == 0);

	pthread_mutex_destroy(&pctrlr.ctrlr.ctrlr_lock);
	reset_adminq_disconnect_mocks();
}

static void
test_nvme_pcie_ctrlr_construct_admin_qpair(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	struct nvme_tracker *tr;
	struct nvme_request req = {};
	struct adminq_disconnect_ctx cb_ctx = {};
	struct spdk_nvme_ctrlr_process proc = {};
	int rc = 0;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.admin_queue_size = 32;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;
	pctrlr.ctrlr.flags = 0;
	pctrlr.ctrlr.free_io_sqids = NULL;
	pctrlr.ctrlr.free_io_cqids = NULL;
	pctrlr.ctrlr.is_resetting = false;
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_destructed = false;
	pctrlr.ctrlr.outstanding_aborts = 0;
	pctrlr.ctrlr.ana_log_page = NULL;
	pctrlr.ctrlr.ana_log_page_size = 0;

	TAILQ_INIT(&pctrlr.ctrlr.active_io_qpairs);
	STAILQ_INIT(&pctrlr.ctrlr.queued_aborts);
	TAILQ_INIT(&pctrlr.ctrlr.active_procs);

	rc = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr.ctrlr, 32);
	CU_ASSERT(rc == 0);
	pqpair = nvme_pcie_qpair(pctrlr.ctrlr.adminq);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	CU_ASSERT(pqpair->num_entries == 32);
	CU_ASSERT(pqpair->flags.delay_cmd_submit == 0);
	CU_ASSERT(pqpair->qpair.id == 0);
	CU_ASSERT(pqpair->qpair.qprio == SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT(pqpair->qpair.ctrlr == &pctrlr.ctrlr);
	CU_ASSERT(pqpair->stat != NULL);

	/* Inject an outstanding admin tracker before destroy to verify
	 * that nvme_pcie_qpair_destroy aborts it with a proper status. */
	STAILQ_INIT(&proc.active_reqs);
	g_current_process = &proc;
	pctrlr.ctrlr.adminq->abort_dnr = 1;

	tr = TAILQ_FIRST(&pqpair->free_tr);
	SPDK_CU_ASSERT_FATAL(tr != NULL);
	TAILQ_REMOVE(&pqpair->free_tr, tr, tq_list);

	tr->req = &req;
	tr->cb_fn = adminq_disconnect_req_cb;
	tr->cb_arg = &cb_ctx;
	req.qpair = &pqpair->qpair;
	req.cmd.cid = tr->cid;
	req.cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	req.pid = getpid();
	TAILQ_INSERT_TAIL(&pqpair->outstanding_tr, tr, tq_list);
	pqpair->qpair.num_outstanding_reqs = 1;

	rc = nvme_pcie_qpair_destroy(pctrlr.ctrlr.adminq);
	CU_ASSERT(rc == 0);


	CU_ASSERT(cb_ctx.called == true);
	CU_ASSERT(cb_ctx.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(cb_ctx.sc == SPDK_NVME_SC_ABORTED_SQ_DELETION);
	CU_ASSERT(cb_ctx.dnr == 1);
}

static void
ut_shared_cq_disconnected(struct spdk_nvme_qpair *qpair, void *ctx)
{
	unsigned *count = ctx;

	(*count)++;
}

static void
ut_shared_cq_fill(struct nvme_pcie_cq *cq, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		unsigned index = cq->cq_head + i;
		struct spdk_nvme_cpl *cpl = &cq->cpl[index % cq->num_entries];

		memset(cpl, 0, sizeof(*cpl));
		cpl->sqid = UINT16_MAX;
		cpl->status.p = cq->phase ^ (index >= cq->num_entries);
	}
}

static void
test_nvme_pcie_shared_cq_poll_group(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_io_cq pcq = {};
	struct nvme_pcie_cq *cq = &pcq.pcie_cq;
	struct nvme_pcie_qpair members[2] = {};
	struct nvme_pcie_qpair *map[3] = {NULL, &members[0], &members[1]};
	struct spdk_nvme_cpl ring[8] = {};
	struct spdk_nvme_ctrlr_process foreign = {};
	unsigned disconnected = 0, i;
	struct spdk_nvme_poll_group parent = {.ctx = &disconnected};
	struct spdk_nvme_transport_poll_group *tgroup, *other;
	struct nvme_pcie_poll_group *group;
	uint32_t cq_doorbell = 0, sq_doorbell[2] = {};
	uint64_t polls;

	tgroup = nvme_pcie_poll_group_create();
	other = nvme_pcie_poll_group_create();
	SPDK_CU_ASSERT_FATAL(tgroup != NULL && other != NULL);
	STAILQ_INIT(&tgroup->connected_qpairs);
	STAILQ_INIT(&tgroup->disconnected_qpairs);
	STAILQ_INIT(&other->connected_qpairs);
	STAILQ_INIT(&other->disconnected_qpairs);
	tgroup->group = &parent;
	other->group = &parent;
	group = SPDK_CONTAINEROF(tgroup, struct nvme_pcie_poll_group, group);
	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pcq.cq.ctrlr = &pctrlr.ctrlr;
	cq->cpl = ring;
	cq->num_entries = SPDK_COUNTOF(ring);
	cq->phase = 1;
	cq->cqid = 3;
	cq->cq_hdbl = &cq_doorbell;
	cq->sq_map = map;
	cq->sq_map_size = SPDK_COUNTOF(map);
	TAILQ_INIT(&cq->sq_members);
	for (i = 0; i < SPDK_COUNTOF(members); i++) {
		members[i].cq = cq;
		members[i].flags.shared_cq = true;
		members[i].qpair.ctrlr = &pctrlr.ctrlr;
		members[i].qpair.id = i + 1;
		members[i].qpair.poll_group = tgroup;
		members[i].sq_tdbl = &sq_doorbell[i];
		TAILQ_INIT(&members[i].outstanding_tr);
		TAILQ_INSERT_TAIL(&cq->sq_members, &members[i], cq_member_link);
		CU_ASSERT(nvme_pcie_poll_group_add(tgroup, &members[i].qpair) == 0);
		CU_ASSERT(nvme_pcie_poll_group_connect_qpair(&members[i].qpair) == 0);
		nvme_qpair_set_state(&members[i].qpair, NVME_QPAIR_ENABLED);
		STAILQ_INSERT_TAIL(&tgroup->connected_qpairs, &members[i].qpair, poll_group_stailq);
	}
	CU_ASSERT(cq->poll_group_qpair_count == 2);
	CU_ASSERT(cq->poll_group_connected_count == 2);
	CU_ASSERT(nvme_pcie_poll_group_add(other, &members[1].qpair) == -EINVAL);
	MOCK_SET(nvme_shared_cq_enabled, true);
	CU_ASSERT(spdk_nvme_pcie_cq_process_completions(&pcq.cq, 0) == -EBUSY);
	CU_ASSERT(nvme_pcie_poll_group_destroy(tgroup) == -EBUSY);

	ut_shared_cq_fill(cq, 3);
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 1, ut_shared_cq_disconnected) == 2);
	CU_ASSERT(cq->stat.polls == 1 && group->stats.polls == 1);
	CU_ASSERT(group->stats.completions == 2 && cq_doorbell == 2);
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 1, ut_shared_cq_disconnected) == 1);
	CU_ASSERT(cq->stat.polls == 2 && group->stats.polls == 2);

	ut_shared_cq_fill(cq, 7);
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, UINT32_MAX,
			ut_shared_cq_disconnected) == 7);
	CU_ASSERT(cq->stat.polls == 3 && group->stats.completions == 10);
	members[0].flags.delay_cmd_submit = true;
	members[0].sq_tail = 2;
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 0, ut_shared_cq_disconnected) == 0);
	CU_ASSERT(sq_doorbell[0] == 2 && members[0].last_sq_tail == 2);
	CU_ASSERT(group->stats.idle_polls == 1 && group->stats.sq_mmio_doorbell_updates == 1);
	ut_shared_cq_fill(cq, 1);
	polls = cq->stat.polls;
	MOCK_SET(nvme_qpair_check_enabled, false);
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 1, ut_shared_cq_disconnected) == 0);
	CU_ASSERT(cq->stat.polls == polls);
	MOCK_SET(nvme_qpair_check_enabled, true);
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 1, ut_shared_cq_disconnected) == 1);

	/* A connecting sibling must not drain independently or block the active SQ. */
	nvme_qpair_set_state(&members[1].qpair, NVME_QPAIR_CONNECTING);
	ut_shared_cq_fill(cq, 2);
	polls = cq->stat.polls;
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 1, ut_shared_cq_disconnected) == 1);
	CU_ASSERT(cq->stat.polls == polls + 1);
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 1, ut_shared_cq_disconnected) == 1);
	nvme_qpair_set_state(&members[0].qpair, NVME_QPAIR_DISCONNECTED);
	polls = cq->stat.polls;
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 0, ut_shared_cq_disconnected) == 0);
	CU_ASSERT(cq->stat.polls == polls);
	pctrlr.ctrlr.is_failed = true;
	CU_ASSERT(nvme_pcie_poll_group_process_completions(tgroup, 0,
			ut_shared_cq_disconnected) == -ENXIO);
	CU_ASSERT(disconnected == 2);
	pctrlr.ctrlr.is_failed = false;

	for (i = 0; i < SPDK_COUNTOF(members); i++) {
		CU_ASSERT(nvme_pcie_poll_group_disconnect_qpair(&members[i].qpair) == 0);
		STAILQ_REMOVE(&tgroup->connected_qpairs, &members[i].qpair, spdk_nvme_qpair,
			      poll_group_stailq);
		CU_ASSERT(nvme_pcie_poll_group_remove(tgroup, &members[i].qpair) == 0);
		members[i].qpair.poll_group = NULL;
		if (i == 0) {
			CU_ASSERT(cq->owner_tgroup == tgroup);
			CU_ASSERT(nvme_pcie_poll_group_add(other, &members[i].qpair) == -EINVAL);
		}
	}
	CU_ASSERT(cq->poll_group == NULL && cq->owner_tgroup == NULL);
	CU_ASSERT(cq->poll_group_qpair_count == 0 && cq->poll_group_connected_count == 0);
	CU_ASSERT(nvme_pcie_poll_group_destroy(tgroup) == 0);
	members[0].qpair.poll_group = other;
	CU_ASSERT(nvme_pcie_poll_group_add(other, &members[0].qpair) == 0);
	CU_ASSERT(cq->owner_tgroup == other);
	CU_ASSERT(nvme_pcie_poll_group_remove(other, &members[0].qpair) == 0);
	CU_ASSERT(nvme_pcie_poll_group_destroy(other) == 0);
	/* A dead process's poll-group pointer is not valid in this process. */
	cq->owner_proc = &foreign;
	cq->poll_group = (struct nvme_pcie_poll_group *)(uintptr_t)1;
	cq->poll_group_qpair_count = cq->poll_group_connected_count = 1;
	members[0].flags.shared_cq_tracked = members[0].flags.shared_cq_connected = true;
	nvme_pcie_poll_group_untrack_shared_cq(&members[0].qpair);
	CU_ASSERT(cq->poll_group == NULL);
	CU_ASSERT(cq->poll_group_qpair_count == 0 && cq->poll_group_connected_count == 0);
	CU_ASSERT(!members[0].flags.shared_cq_tracked && !members[0].flags.shared_cq_connected);
	MOCK_SET(nvme_shared_cq_enabled, false);
}

static void
test_nvme_pcie_poll_group_get_stats(void)
{
	int rc = 0;
	struct nvme_pcie_poll_group *pgroup = NULL;
	struct spdk_nvme_transport_poll_group *tgroup = NULL;
	struct spdk_nvme_transport_poll_group_stat *tgroup_stat = NULL;

	tgroup = nvme_pcie_poll_group_create();
	CU_ASSERT(tgroup != NULL);
	pgroup = SPDK_CONTAINEROF(tgroup, struct nvme_pcie_poll_group, group);
	CU_ASSERT(pgroup != NULL);

	/* Invalid group pointer, expect fail and return -EINVAL */
	rc = nvme_pcie_poll_group_get_stats(NULL, &tgroup_stat);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(tgroup_stat == NULL);

	/* Invalid stats, expect fail and return -EINVAL */
	rc = nvme_pcie_poll_group_get_stats(tgroup, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Get state success */
	rc = nvme_pcie_poll_group_get_stats(tgroup, &tgroup_stat);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(tgroup_stat != NULL);
	CU_ASSERT(tgroup_stat->trtype == SPDK_NVME_TRANSPORT_PCIE);
	CU_ASSERT(memcmp(&tgroup_stat->pcie, &pgroup->stats, sizeof(struct spdk_nvme_pcie_stat)) == 0);

	nvme_pcie_poll_group_free_stats(tgroup, tgroup_stat);
	rc = nvme_pcie_poll_group_destroy(tgroup);
	CU_ASSERT(rc == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvme_pcie_common", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_alloc_cmb);
	CU_ADD_TEST(suite, test_nvme_pcie_standalone_cq_lifecycle);
	CU_ADD_TEST(suite, test_nvme_pcie_shared_cq_routing);
	CU_ADD_TEST(suite, test_nvme_pcie_shared_sq_lifecycle);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_construct_destroy);
	CU_ADD_TEST(suite, test_nvme_pcie_private_cq_wrap_reset);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_cmd_create_delete_io_queue);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_connect_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_disconnect_admin_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_construct_admin_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_poll_group_get_stats);
	CU_ADD_TEST(suite, test_nvme_pcie_shared_cq_poll_group);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
