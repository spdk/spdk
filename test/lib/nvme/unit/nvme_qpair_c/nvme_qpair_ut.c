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


#include <sys/uio.h>

#include "spdk_cunit.h"

#include "nvme/nvme_qpair.c"

struct nvme_driver g_nvme_driver = {
	.lock = NVME_MUTEX_INITIALIZER,
};

int32_t spdk_nvme_retry_count = 1;

char outbuf[OUTBUF_SIZE];

struct nvme_request *g_request = NULL;

bool fail_vtophys = false;

bool fail_next_sge = false;

uint64_t nvme_vtophys(void *buf)
{
	if (fail_vtophys) {
		return (uint64_t) - 1;
	} else {
		return (uintptr_t)buf;
	}
}

struct io_request {
	uint64_t address_offset;
	bool	invalid_addr;
	bool	invalid_second_addr;
};

static void nvme_request_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	struct io_request *req = (struct io_request *)cb_arg;

	req->address_offset = 0;
	req->invalid_addr = false;
	req->invalid_second_addr = false;
	switch (sgl_offset) {
	case 0:
		req->invalid_addr = false;
		break;
	case 1:
		req->invalid_addr = true;
		break;
	case 2:
		req->invalid_addr = false;
		req->invalid_second_addr = true;
		break;
	default:
		break;
	}
	return;
}

static int nvme_request_next_sge(void *cb_arg, uint64_t *address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;

	if (req->address_offset == 0) {
		if (req->invalid_addr) {
			*address = 7;
		} else {
			*address = 4096 * req->address_offset;
		}
	} else if (req->address_offset == 1) {
		if (req->invalid_second_addr) {
			*address = 7;
		} else {
			*address = 4096 * req->address_offset;
		}
	} else {
		*address = 4096 * req->address_offset;
	}

	req->address_offset += 1;
	*length = 4096;

	if (fail_next_sge) {
		return - 1;
	} else {
		return 0;
	}

}

struct nvme_request *
nvme_allocate_request(const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_request *req = NULL;

	nvme_alloc_request(&req);

	if (req == NULL) {
		return req;
	}

	/*
	 * Only memset up to (but not including) the children
	 *  TAILQ_ENTRY.  children, and following members, are
	 *  only used as part of I/O splitting so we avoid
	 *  memsetting them until it is actually needed.
	 *  They will be initialized in nvme_request_add_child()
	 *  if the request is split.
	 */
	memset(req, 0, offsetof(struct nvme_request, children));
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->payload = *payload;
	req->payload_size = payload_size;

	return req;
}

struct nvme_request *
nvme_allocate_request_contig(void *buffer, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
			     void *cb_arg)
{
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	return nvme_allocate_request(&payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_null(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(NULL, 0, cb_fn, cb_arg);
}

void
nvme_free_request(struct nvme_request *req)
{
	nvme_dealloc_request(req);
}

void
nvme_request_remove_child(struct nvme_request *parent,
			  struct nvme_request *child)
{
	parent->num_children--;
	TAILQ_REMOVE(&parent->children, child, child_tailq);
}

int
nvme_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t length, uint64_t aligned,
		     uint64_t *offset)
{
	return -1;
}

static void
test1(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_cmd cmd = {};

	outbuf[0] = '\0';

	/*
	 * qpair.id == 0 means it is an admin queue.  Ensure
	 *  that the opc is decoded as an admin opc and not an
	 *  I/o opc.
	 */
	qpair.id = 0;
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;

	nvme_qpair_print_command(&qpair, &cmd);

	CU_ASSERT(strstr(outbuf, "IDENTIFY") != NULL);
}

static void
test2(void)
{
	struct spdk_nvme_qpair qpair = {};
	struct spdk_nvme_cmd cmd = {};

	outbuf[0] = '\0';

	/*
	 * qpair.id != 0 means it is an I/O queue.  Ensure
	 *  that the opc is decoded as an I/O opc and not an
	 *  admin opc.
	 */
	qpair.id = 1;
	cmd.opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;

	nvme_qpair_print_command(&qpair, &cmd);

	CU_ASSERT(strstr(outbuf, "DATASET MANAGEMENT") != NULL);
}

static void
prepare_submit_request_test(struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_ctrlr *ctrlr,
			    struct spdk_nvme_registers *regs)
{
	memset(ctrlr, 0, sizeof(*ctrlr));
	ctrlr->regs = regs;
	TAILQ_INIT(&ctrlr->free_io_qpairs);
	TAILQ_INIT(&ctrlr->active_io_qpairs);
	nvme_qpair_construct(qpair, 1, 128, 32, ctrlr);

	CU_ASSERT(qpair->sq_tail == 0);
	CU_ASSERT(qpair->cq_head == 0);

	fail_vtophys = false;
}

static void
cleanup_submit_request_test(struct spdk_nvme_qpair *qpair)
{
	nvme_qpair_destroy(qpair);
}

static void
ut_insert_cq_entry(struct spdk_nvme_qpair *qpair, uint32_t slot)
{
	struct nvme_request	*req;
	struct nvme_tracker 	*tr;
	struct spdk_nvme_cpl	*cpl;

	nvme_alloc_request(&req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	memset(req, 0, sizeof(*req));

	tr = LIST_FIRST(&qpair->free_tr);
	LIST_REMOVE(tr, list); /* remove tr from free_tr */
	LIST_INSERT_HEAD(&qpair->outstanding_tr, tr, list);
	req->cmd.cid = tr->cid;
	tr->req = req;
	qpair->tr[tr->cid].active = true;

	cpl = &qpair->cpl[slot];
	cpl->status.p = qpair->phase;
	cpl->cid = tr->cid;
}

static void
expected_success_callback(void *arg, const struct spdk_nvme_cpl *cpl)
{
	CU_ASSERT(!spdk_nvme_cpl_is_error(cpl));
}

static void
expected_failure_callback(void *arg, const struct spdk_nvme_cpl *cpl)
{
	CU_ASSERT(spdk_nvme_cpl_is_error(cpl));
}

static void
test3(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req;
	struct nvme_tracker		*tr;
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct spdk_nvme_registers	regs = {};
	uint16_t			cid;

	prepare_submit_request_test(&qpair, &ctrlr, &regs);

	req = nvme_allocate_request_null(expected_success_callback, NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	CU_ASSERT(qpair.sq_tail == 0);

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) == 0);

	CU_ASSERT(qpair.sq_tail == 1);

	/*
	 * Since sq_tail was 0 when the command was submitted, it is in cmd[0].
	 * Extract its command ID to retrieve its tracker.
	 */
	cid = qpair.cmd[0].cid;
	tr = &qpair.tr[cid];
	SPDK_CU_ASSERT_FATAL(tr != NULL);

	/*
	 * Complete the tracker so that it is returned to the free list.
	 * This also frees the request.
	 */
	nvme_qpair_manual_complete_tracker(&qpair, tr, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0,
					   false);

	cleanup_submit_request_test(&qpair);
}

static void
test4(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req;
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct spdk_nvme_registers	regs = {};
	char				payload[4096];

	prepare_submit_request_test(&qpair, &ctrlr, &regs);

	req = nvme_allocate_request_contig(payload, sizeof(payload), expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* Force vtophys to return a failure.  This should
	 *  result in the nvme_qpair manually failing
	 *  the request with error status to signify
	 *  a bad payload buffer.
	 */
	fail_vtophys = true;
	outbuf[0] = '\0';

	CU_ASSERT(qpair.sq_tail == 0);

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);

	CU_ASSERT(qpair.sq_tail == 0);
	/* Assert that command/completion data was printed to log. */
	CU_ASSERT(strlen(outbuf) > 0);

	cleanup_submit_request_test(&qpair);
}


static void
test_sgl_req(void)
{
	struct spdk_nvme_qpair	qpair = {};
	struct nvme_request	*req;
	struct spdk_nvme_ctrlr	ctrlr = {};
	struct spdk_nvme_registers	regs = {};
	struct nvme_payload	payload = {};
	struct nvme_tracker 	*sgl_tr = NULL;
	uint64_t 		i;
	struct io_request	io_req = {};

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.u.sgl.reset_sgl_fn = nvme_request_reset_sgl;
	payload.u.sgl.next_sge_fn = nvme_request_next_sge;
	payload.u.sgl.cb_arg = &io_req;

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	req = nvme_allocate_request(&payload, PAGE_SIZE, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 7 | 0;
	req->payload_offset = 1;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) == 0);
	CU_ASSERT(req->cmd.psdt == SPDK_NVME_PSDT_PRP);
	CU_ASSERT(req->cmd.dptr.prp.prp1 == 7);
	CU_ASSERT(req->cmd.dptr.prp.prp2 == 4096);

	sgl_tr = LIST_FIRST(&qpair.outstanding_tr);
	LIST_REMOVE(sgl_tr, list);
	cleanup_submit_request_test(&qpair);
	nvme_free_request(req);

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	req = nvme_allocate_request(&payload, PAGE_SIZE, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 7 | 0;
	spdk_nvme_retry_count = 1;
	fail_next_sge = true;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);
	CU_ASSERT(qpair.sq_tail == 0);
	cleanup_submit_request_test(&qpair);

	fail_next_sge = false;

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	req = nvme_allocate_request(&payload, 2 * PAGE_SIZE, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 15 | 0;
	req->payload_offset = 2;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);
	CU_ASSERT(qpair.sq_tail == 0);
	cleanup_submit_request_test(&qpair);

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	req = nvme_allocate_request(&payload, (NVME_MAX_PRP_LIST_ENTRIES + 1) * PAGE_SIZE, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 4095 | 0;

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) == 0);

	CU_ASSERT(req->cmd.dptr.prp.prp1 == 0);
	CU_ASSERT(qpair.sq_tail == 1);
	sgl_tr = LIST_FIRST(&qpair.outstanding_tr);
	if (sgl_tr != NULL) {
		for (i = 0; i < NVME_MAX_PRP_LIST_ENTRIES; i++) {
			CU_ASSERT(sgl_tr->u.prp[i] == (PAGE_SIZE * (i + 1)));
		}

		LIST_REMOVE(sgl_tr, list);
	}
	cleanup_submit_request_test(&qpair);
	nvme_free_request(req);
}

static void
test_hw_sgl_req(void)
{
	struct spdk_nvme_qpair	qpair = {};
	struct nvme_request	*req;
	struct spdk_nvme_ctrlr	ctrlr = {};
	struct spdk_nvme_registers	regs = {};
	struct nvme_payload	payload = {};
	struct nvme_tracker 	*sgl_tr = NULL;
	uint64_t 		i;
	struct io_request	io_req = {};

	payload.type = NVME_PAYLOAD_TYPE_SGL;
	payload.u.sgl.reset_sgl_fn = nvme_request_reset_sgl;
	payload.u.sgl.next_sge_fn = nvme_request_next_sge;
	payload.u.sgl.cb_arg = &io_req;

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	req = nvme_allocate_request(&payload, PAGE_SIZE, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 7 | 0;
	req->payload_offset = 0;
	ctrlr.flags |= SPDK_NVME_CTRLR_SGL_SUPPORTED;

	nvme_qpair_submit_request(&qpair, req);

	sgl_tr = LIST_FIRST(&qpair.outstanding_tr);
	CU_ASSERT(sgl_tr != NULL);
	CU_ASSERT(sgl_tr->u.sgl[0].type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	CU_ASSERT(sgl_tr->u.sgl[0].type_specific == 0);
	CU_ASSERT(sgl_tr->u.sgl[0].length == 4096);
	CU_ASSERT(sgl_tr->u.sgl[0].address == 0);
	CU_ASSERT(req->cmd.dptr.sgl1.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
	LIST_REMOVE(sgl_tr, list);
	cleanup_submit_request_test(&qpair);
	nvme_free_request(req);

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	req = nvme_allocate_request(&payload, NVME_MAX_SGL_DESCRIPTORS * PAGE_SIZE, NULL, &io_req);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	req->cmd.opc = SPDK_NVME_OPC_WRITE;
	req->cmd.cdw10 = 10000;
	req->cmd.cdw12 = 2023 | 0;
	req->payload_offset = 0;
	ctrlr.flags |= SPDK_NVME_CTRLR_SGL_SUPPORTED;

	nvme_qpair_submit_request(&qpair, req);

	sgl_tr = LIST_FIRST(&qpair.outstanding_tr);
	CU_ASSERT(sgl_tr != NULL);
	for (i = 0; i < NVME_MAX_SGL_DESCRIPTORS; i++) {
		CU_ASSERT(sgl_tr->u.sgl[i].type == SPDK_NVME_SGL_TYPE_DATA_BLOCK);
		CU_ASSERT(sgl_tr->u.sgl[i].type_specific == 0);
		CU_ASSERT(sgl_tr->u.sgl[i].length == 4096);
		CU_ASSERT(sgl_tr->u.sgl[i].address == i * 4096);
	}
	CU_ASSERT(req->cmd.dptr.sgl1.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	LIST_REMOVE(sgl_tr, list);
	cleanup_submit_request_test(&qpair);
	nvme_free_request(req);
}


static void
test_ctrlr_failed(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req;
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct spdk_nvme_registers	regs = {};
	char				payload[4096];

	prepare_submit_request_test(&qpair, &ctrlr, &regs);

	req = nvme_allocate_request_contig(payload, sizeof(payload), expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* Disable the queue and set the controller to failed.
	 * Set the controller to resetting so that the qpair won't get re-enabled.
	 */
	qpair.is_enabled = false;
	ctrlr.is_failed = true;
	ctrlr.is_resetting = true;

	outbuf[0] = '\0';

	CU_ASSERT(qpair.sq_tail == 0);

	CU_ASSERT(nvme_qpair_submit_request(&qpair, req) != 0);

	CU_ASSERT(qpair.sq_tail == 0);

	cleanup_submit_request_test(&qpair);
}

static void struct_packing(void)
{
	/* ctrlr is the first field in nvme_qpair after the fields
	 * that are used in the I/O path. Make sure the I/O path fields
	 * all fit into two cache lines.
	 */
	CU_ASSERT(offsetof(struct spdk_nvme_qpair, ctrlr) <= 128);
}

static void test_nvme_qpair_fail(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct nvme_request		*req = NULL;
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct spdk_nvme_registers	regs = {};
	struct nvme_tracker		*tr_temp;

	prepare_submit_request_test(&qpair, &ctrlr, &regs);

	tr_temp = LIST_FIRST(&qpair.free_tr);
	SPDK_CU_ASSERT_FATAL(tr_temp != NULL);
	LIST_REMOVE(tr_temp, list);
	tr_temp->req = nvme_allocate_request_null(expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(tr_temp->req != NULL);
	tr_temp->req->cmd.cid = tr_temp->cid;

	LIST_INSERT_HEAD(&qpair.outstanding_tr, tr_temp, list);
	nvme_qpair_fail(&qpair);
	CU_ASSERT_TRUE(LIST_EMPTY(&qpair.outstanding_tr));

	req = nvme_allocate_request_null(expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(req != NULL);

	STAILQ_INSERT_HEAD(&qpair.queued_req, req, stailq);
	nvme_qpair_fail(&qpair);
	CU_ASSERT_TRUE(STAILQ_EMPTY(&qpair.queued_req));

	cleanup_submit_request_test(&qpair);
}

static void test_nvme_qpair_process_completions(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct spdk_nvme_registers	regs = {};

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	qpair.is_enabled = false;
	qpair.ctrlr->is_resetting = true;

	spdk_nvme_qpair_process_completions(&qpair, 0);
	cleanup_submit_request_test(&qpair);
}

static void
test_nvme_qpair_process_completions_limit(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct spdk_nvme_registers	regs = {};

	prepare_submit_request_test(&qpair, &ctrlr, &regs);
	qpair.is_enabled = true;

	/* Insert 4 entries into the completion queue */
	CU_ASSERT(qpair.cq_head == 0);
	ut_insert_cq_entry(&qpair, 0);
	ut_insert_cq_entry(&qpair, 1);
	ut_insert_cq_entry(&qpair, 2);
	ut_insert_cq_entry(&qpair, 3);

	/* This should only process 2 completions, and 2 should be left in the queue */
	spdk_nvme_qpair_process_completions(&qpair, 2);
	CU_ASSERT(qpair.cq_head == 2);

	/* This should only process 1 completion, and 1 should be left in the queue */
	spdk_nvme_qpair_process_completions(&qpair, 1);
	CU_ASSERT(qpair.cq_head == 3);

	/* This should process the remaining completion */
	spdk_nvme_qpair_process_completions(&qpair, 5);
	CU_ASSERT(qpair.cq_head == 4);

	cleanup_submit_request_test(&qpair);
}

static void test_nvme_qpair_destroy(void)
{
	struct spdk_nvme_qpair		qpair = {};
	struct spdk_nvme_ctrlr		ctrlr = {};
	struct spdk_nvme_registers	regs = {};
	struct nvme_tracker		*tr_temp;

	memset(&ctrlr, 0, sizeof(ctrlr));
	ctrlr.regs = &regs;
	TAILQ_INIT(&ctrlr.free_io_qpairs);
	TAILQ_INIT(&ctrlr.active_io_qpairs);

	nvme_qpair_construct(&qpair, 1, 128, 32, &ctrlr);
	nvme_qpair_destroy(&qpair);


	nvme_qpair_construct(&qpair, 0, 128, 32, &ctrlr);
	tr_temp = LIST_FIRST(&qpair.free_tr);
	SPDK_CU_ASSERT_FATAL(tr_temp != NULL);
	LIST_REMOVE(tr_temp, list);
	tr_temp->req = nvme_allocate_request_null(expected_failure_callback, NULL);
	SPDK_CU_ASSERT_FATAL(tr_temp->req != NULL);

	tr_temp->req->cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
	tr_temp->req->cmd.cid = tr_temp->cid;
	LIST_INSERT_HEAD(&qpair.outstanding_tr, tr_temp, list);

	nvme_qpair_destroy(&qpair);
	CU_ASSERT(LIST_EMPTY(&qpair.outstanding_tr));
}

static void test_nvme_completion_is_retry(void)
{
	struct spdk_nvme_cpl	cpl = {};

	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	cpl.status.dnr = 0;
	CU_ASSERT_TRUE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_COMMAND_ID_CONFLICT;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_DATA_TRANSFER_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ABORTED_POWER_LOSS;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = SPDK_NVME_SC_CAPACITY_EXCEEDED;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sc = 0x70;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = SPDK_NVME_SCT_VENDOR_SPECIFIC;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));

	cpl.status.sct = 0x4;
	CU_ASSERT_FALSE(nvme_completion_is_retry(&cpl));
}

static void
test_get_status_string(void)
{
	const char	*status_string;

	status_string = get_status_string(SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(strcmp(status_string, "SUCCESS") == 0);

	status_string = get_status_string(SPDK_NVME_SCT_COMMAND_SPECIFIC,
					  SPDK_NVME_SC_COMPLETION_QUEUE_INVALID);
	CU_ASSERT(strcmp(status_string, "INVALID COMPLETION QUEUE") == 0);

	status_string = get_status_string(SPDK_NVME_SCT_MEDIA_ERROR, SPDK_NVME_SC_UNRECOVERED_READ_ERROR);
	CU_ASSERT(strcmp(status_string, "UNRECOVERED READ ERROR") == 0);

	status_string = get_status_string(SPDK_NVME_SCT_VENDOR_SPECIFIC, 0);
	CU_ASSERT(strcmp(status_string, "VENDOR SPECIFIC") == 0);

	status_string = get_status_string(100, 0);
	CU_ASSERT(strcmp(status_string, "RESERVED") == 0);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_qpair", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test1", test1) == NULL
		|| CU_add_test(suite, "test2", test2) == NULL
		|| CU_add_test(suite, "test3", test3) == NULL
		|| CU_add_test(suite, "test4", test4) == NULL
		|| CU_add_test(suite, "ctrlr_failed", test_ctrlr_failed) == NULL
		|| CU_add_test(suite, "struct_packing", struct_packing) == NULL
		|| CU_add_test(suite, "nvme_qpair_fail", test_nvme_qpair_fail) == NULL
		|| CU_add_test(suite, "spdk_nvme_qpair_process_completions",
			       test_nvme_qpair_process_completions) == NULL
		|| CU_add_test(suite, "spdk_nvme_qpair_process_completions_limit",
			       test_nvme_qpair_process_completions_limit) == NULL
		|| CU_add_test(suite, "nvme_qpair_destroy", test_nvme_qpair_destroy) == NULL
		|| CU_add_test(suite, "nvme_completion_is_retry", test_nvme_completion_is_retry) == NULL
		|| CU_add_test(suite, "get_status_string", test_get_status_string) == NULL
		|| CU_add_test(suite, "sgl_request", test_sgl_req) == NULL
		|| CU_add_test(suite, "hw_sgl_request", test_hw_sgl_req) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
