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

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "nvme/nvme_pcie_common.c"
#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;
DEFINE_STUB(nvme_ctrlr_get_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr, pid_t pid), NULL);

DEFINE_STUB(nvme_ctrlr_submit_admin_request, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_request *req), 0);

DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));

DEFINE_STUB(nvme_wait_for_completion, int,
	    (struct spdk_nvme_qpair *qpair,
	     struct nvme_completion_poll_status *status), 0);

DEFINE_STUB(nvme_completion_is_retry, bool, (const struct spdk_nvme_cpl *cpl), false);

DEFINE_STUB_V(nvme_ctrlr_process_async_event, (struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_cpl *cpl));

DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));

DEFINE_STUB_V(spdk_nvme_qpair_print_completion, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cpl *cpl));

DEFINE_STUB_V(nvme_qpair_deinit, (struct spdk_nvme_qpair *qpair));

DEFINE_STUB(nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB(spdk_nvme_qpair_process_completions, int32_t,
	    (struct spdk_nvme_qpair *qpair, uint32_t max_completions), 0);

DEFINE_STUB(nvme_request_check_timeout, int, (struct nvme_request *req, uint16_t cid,
		struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick), 0);
DEFINE_STUB(spdk_strerror, const char *, (int errnum), NULL);

int nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
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
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	/* Enable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = true;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == true);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == (((pctrlr.cmb.bar_pa + cmb_offset) + page_align - 1) & ~
					   (page_align - 1)));
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase = 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	CU_ASSERT(pctrlr.cmb.current_offset == (uintptr_t)pqpair->cmd + (pqpair->num_entries * sizeof(
				struct spdk_nvme_cmd)) - (uintptr_t)pctrlr.cmb.bar_va);
	cmb_offset = pctrlr.cmb.current_offset;
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDEADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase = 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer, sq_vaddr and cq_vaddr invalid. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;
	MOCK_SET(spdk_vtophys, 0xDAADBEEF);

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDAADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000c);
	CU_ASSERT(pqpair->flags.phase = 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);
	MOCK_CLEAR(spdk_vtophys);
}

static void
test_nvme_pcie_ctrlr_cmd_create_delete_io_queue(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_pcie_qpair pqpair = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req = {};
	int rc;

	ctrlr.adminq = &adminq;
	STAILQ_INIT(&ctrlr.adminq->free_req);
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
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
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.cqid = 1);
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
	struct nvme_pcie_qpair	pqpair = {};
	struct spdk_nvme_transport_poll_group poll_group = {};
	struct spdk_nvme_cpl cpl = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req[3] = {};
	int rc;

	pqpair.cpl = &cpl;
	pqpair.num_entries = 1;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
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
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid = 1);
	CU_ASSERT(req[1].cmd.dptr.prp.prp1 == 0xDDADBEEF);

	pqpair.qpair.state = NVME_QPAIR_CONNECTING;
	/* Complete the second request */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_READY);
	/* State is still CONNECTING until the thread is polled again. */
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_CONNECTING);

	/* doorbell stride and qid are 1 */
	CU_ASSERT(pqpair.shadow_doorbell.sq_tdbl == pctrlr.ctrlr.shadow_doorbell + 2);
	CU_ASSERT(pqpair.shadow_doorbell.cq_hdbl == pctrlr.ctrlr.shadow_doorbell + 3);
	CU_ASSERT(pqpair.shadow_doorbell.sq_eventidx == pctrlr.ctrlr.eventidx + 2);
	CU_ASSERT(pqpair.shadow_doorbell.cq_eventidx == pctrlr.ctrlr.eventidx + 3);
	CU_ASSERT(pqpair.flags.has_shadow_doorbell == 1);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	spdk_free(pctrlr.ctrlr.shadow_doorbell);
	spdk_free(pctrlr.ctrlr.eventidx);
	pctrlr.ctrlr.shadow_doorbell = NULL;
	pctrlr.ctrlr.eventidx = NULL;

	/* Shadow doorbell 0 */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
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
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid = 1);
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
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	/* Modify cpl such that CQ fails */
	pqpair.cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
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
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
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
	pqpair.cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;

	/* Request to complete sq callback in async operation */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(req[2].cmd.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	/* Modify cpl back to success */
	pqpair.cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	req[2].cb_fn(req[2].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_FAILED);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);
	/* No need to remove unused requests here */
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));


	/* No available request used */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_nvme_pcie_ctrlr_construct_admin_qpair(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	int rc = 0;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.admin_queue_size = 32;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;
	pctrlr.ctrlr.flags = 0;
	pctrlr.ctrlr.free_io_qids = NULL;
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

	rc = nvme_pcie_qpair_destroy(pctrlr.ctrlr.adminq);
	CU_ASSERT(rc == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvme_pcie_common", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_alloc_cmb);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_construct_destroy);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_cmd_create_delete_io_queue);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_connect_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_construct_admin_qpair);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
