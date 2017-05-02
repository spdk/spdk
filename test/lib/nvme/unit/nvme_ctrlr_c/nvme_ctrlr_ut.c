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

#include "spdk_internal/log.h"

#include "lib/test_env.c"

struct spdk_trace_flag SPDK_TRACE_NVME = {
	.name = "nvme",
	.enabled = false,
};

#include "nvme/nvme_ctrlr.c"

struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

struct spdk_nvme_registers g_ut_nvme_regs = {};

__thread int    nvme_thread_ioq_index = -1;

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	return NULL;
}

int
nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_transport_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

int
nvme_transport_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 4);
	*(uint32_t *)((uintptr_t)&g_ut_nvme_regs + offset) = value;
	return 0;
}

int
nvme_transport_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 8);
	*(uint64_t *)((uintptr_t)&g_ut_nvme_regs + offset) = value;
	return 0;
}

int
nvme_transport_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 4);
	*value = *(uint32_t *)((uintptr_t)&g_ut_nvme_regs + offset);
	return 0;
}

int
nvme_transport_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	SPDK_CU_ASSERT_FATAL(offset <= sizeof(struct spdk_nvme_registers) - 8);
	*value = *(uint64_t *)((uintptr_t)&g_ut_nvme_regs + offset);
	return 0;
}

uint32_t
nvme_transport_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return UINT32_MAX;
}

uint32_t
nvme_transport_ctrlr_get_max_io_queue_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return SPDK_NVME_IO_QUEUE_MAX_ENTRIES;
}

struct spdk_nvme_qpair *
nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				     enum spdk_nvme_qprio qprio)
{
	struct spdk_nvme_qpair *qpair;

	qpair = calloc(1, sizeof(*qpair));
	SPDK_CU_ASSERT_FATAL(qpair != NULL);

	qpair->ctrlr = ctrlr;
	qpair->id = qid;
	qpair->qprio = qprio;

	return qpair;
}

int
nvme_transport_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	free(qpair);
	return 0;
}

int
nvme_transport_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_transport_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		    struct spdk_nvme_ctrlr *ctrlr,
		    enum spdk_nvme_qprio qprio,
		    uint32_t num_requests)
{
	qpair->id = id;
	qpair->qprio = qprio;
	qpair->ctrlr = ctrlr;

	return 0;
}

static void
fake_cpl_success(spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_cpl cpl = {};

	cpl.status.sc = SPDK_NVME_SC_SUCCESS;
	cb_fn(cb_arg, &cpl);
}

int
spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, uint32_t cdw12, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	CU_ASSERT_FATAL(0);
	return -1;
}

int
spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	CU_ASSERT_FATAL(0);
	return -1;
}

int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				 uint32_t nsid, void *payload, uint32_t payload_size,
				 uint64_t offset, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST);

	/*
	 * Free the request here so it does not leak.
	 * For the purposes of this unit test, we don't need to bother emulating request submission.
	 */
	free(req);

	return 0;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return 0;
}

void
nvme_qpair_disable(struct spdk_nvme_qpair *qpair)
{
}

void
nvme_qpair_enable(struct spdk_nvme_qpair *qpair)
{
}

void
nvme_completion_poll_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_completion_poll_status	*status = arg;

	status->cpl = *cpl;
	status->done = true;
}

int
nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_critical_warning_state state, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_identify_controller(struct spdk_nvme_ctrlr *ctrlr, void *payload,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_cmd_cb cb_fn,
			 void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, struct spdk_nvme_format *format,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_fw_commit(struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_fw_commit *fw_commit,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
nvme_ctrlr_cmd_fw_image_download(struct spdk_nvme_ctrlr *ctrlr,
				 uint32_t size, uint32_t offset, void *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return 0;
}

void
nvme_ns_destruct(struct spdk_nvme_ns *ns)
{
}

int
nvme_ns_construct(struct spdk_nvme_ns *ns, uint16_t id,
		  struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

struct nvme_request *
nvme_allocate_request(struct spdk_nvme_qpair *qpair,
		      const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_request *req = NULL;
	req = calloc(1, sizeof(*req));

	if (req != NULL) {
		memset(req, 0, offsetof(struct nvme_request, children));

		req->payload = *payload;
		req->payload_size = payload_size;

		req->cb_fn = cb_fn;
		req->cb_arg = cb_arg;
		req->qpair = qpair;
		req->pid = getpid();
	}

	return req;
}

struct nvme_request *
nvme_allocate_request_contig(struct spdk_nvme_qpair *qpair, void *buffer, uint32_t payload_size,
			     spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_payload payload;

	payload.type = NVME_PAYLOAD_TYPE_CONTIG;
	payload.u.contig = buffer;

	return nvme_allocate_request(qpair, &payload, payload_size, cb_fn, cb_arg);
}

struct nvme_request *
nvme_allocate_request_null(struct spdk_nvme_qpair *qpair, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return nvme_allocate_request_contig(qpair, NULL, 0, cb_fn, cb_arg);
}

void
nvme_free_request(struct nvme_request *req)
{
	free(req);
}

static void
test_nvme_ctrlr_init_en_1_rdy_0(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 1, CSTS.RDY = 0
	 */
	g_ut_nvme_regs.cc.bits.en = 1;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1);

	/*
	 * Transition to CSTS.RDY = 1.
	 * init() should set CC.EN = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Transition to CSTS.RDY = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	/*
	 * Transition to CC.EN = 1
	 */
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_1_rdy_1(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 1, CSTS.RDY = 1
	 * init() should set CC.EN = 0.
	 */
	g_ut_nvme_regs.cc.bits.en = 1;
	g_ut_nvme_regs.csts.bits.rdy = 1;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Transition to CSTS.RDY = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	/*
	 * Transition to CC.EN = 1
	 */
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_rr(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Default round robin enabled
	 */
	g_ut_nvme_regs.cap.bits.ams = 0x0;
	ctrlr.cap = g_ut_nvme_regs.cap;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to default round robin arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_wrr(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Weighted round robin enabled
	 */
	g_ut_nvme_regs.cap.bits.ams = SPDK_NVME_CAP_AMS_WRR;
	ctrlr.cap = g_ut_nvme_regs.cap;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_WRR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_WRR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to weighted round robin arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_WRR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_WRR);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}
static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_vs(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Default round robin enabled
	 */
	g_ut_nvme_regs.cap.bits.ams = SPDK_NVME_CAP_AMS_VS;
	ctrlr.cap = g_ut_nvme_regs.cap;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_VS);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_VS);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Complete and destroy the controller
	 */
	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to vendor specific arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_VS);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_VS);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);

	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_1(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 1
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 1;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);

	/*
	 * Transition to CSTS.RDY = 0.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE);

	/*
	 * Transition to CC.EN = 1
	 */
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);

	/*
	 * Transition to CSTS.RDY = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 1;
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_READY);

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
setup_qpairs(struct spdk_nvme_ctrlr *ctrlr, uint32_t num_io_queues)
{
	uint32_t i;

	CU_ASSERT_FATAL(pthread_mutex_init(&ctrlr->ctrlr_lock, NULL) == 0);

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(ctrlr) == 0);

	ctrlr->opts.num_io_queues = num_io_queues;
	ctrlr->free_io_qids = spdk_bit_array_create(num_io_queues + 1);
	SPDK_CU_ASSERT_FATAL(ctrlr->free_io_qids != NULL);

	spdk_bit_array_clear(ctrlr->free_io_qids, 0);
	for (i = 1; i <= num_io_queues; i++) {
		spdk_bit_array_set(ctrlr->free_io_qids, i);
	}
}

static void
cleanup_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_destruct(ctrlr);
}

static void
test_alloc_io_qpair_rr_1(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0;

	setup_qpairs(&ctrlr, 1);

	/*
	 * Fake to simulate the controller with default round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_RR;

	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	/* Only 1 I/O qpair was allocated, so this should fail */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 0) == NULL);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Now that the qpair has been returned to the free list,
	 * we should be able to allocate it again.
	 */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/* Only 0 qprio is acceptable for default round robin arbitration mechanism */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(q0 == NULL);
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 2);
	SPDK_CU_ASSERT_FATAL(q0 == NULL);
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 3);
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	/* Only 0 ~ 3 qprio is acceptable */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 4) == NULL);

	cleanup_qpairs(&ctrlr);
}

static void
test_alloc_io_qpair_wrr_1(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0, *q1;

	setup_qpairs(&ctrlr, 2);

	/*
	 * Fake to simulate the controller with weighted round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_WRR;

	/*
	 * Allocate 2 qpairs and free them
	 */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Allocate 2 qpairs and free them in the reverse order
	 */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 2);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 2);
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 3);
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 3);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);

	/* Only 0 ~ 3 qprio is acceptable */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 4) == NULL);

	cleanup_qpairs(&ctrlr);
}

static void
test_alloc_io_qpair_wrr_2(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0, *q1, *q2, *q3;

	setup_qpairs(&ctrlr, 4);

	/*
	 * Fake to simulate the controller with weighted round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_WRR;

	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);
	q2 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 2);
	SPDK_CU_ASSERT_FATAL(q2 != NULL);
	SPDK_CU_ASSERT_FATAL(q2->qprio == 2);
	q3 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 3);
	SPDK_CU_ASSERT_FATAL(q3 != NULL);
	SPDK_CU_ASSERT_FATAL(q3->qprio == 3);
	/* Only 4 I/O qpairs was allocated, so this should fail */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 0) == NULL);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q3) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q2) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Now that the qpair has been returned to the free list,
	 * we should be able to allocate it again.
	 *
	 * Allocate 4 I/O qpairs and half of them with same qprio.
	 */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 1);
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 1);
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);
	q2 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 3);
	SPDK_CU_ASSERT_FATAL(q2 != NULL);
	SPDK_CU_ASSERT_FATAL(q2->qprio == 3);
	q3 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, 3);
	SPDK_CU_ASSERT_FATAL(q3 != NULL);
	SPDK_CU_ASSERT_FATAL(q3->qprio == 3);

	/*
	 * Free all I/O qpairs in reverse order
	 */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q2) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q3) == 0);

	cleanup_qpairs(&ctrlr);
}

static void
test_nvme_ctrlr_fail(void)
{
	struct spdk_nvme_ctrlr	ctrlr = {};

	ctrlr.opts.num_io_queues = 0;
	nvme_ctrlr_fail(&ctrlr, false);

	CU_ASSERT(ctrlr.is_failed == true);
}

static void
test_nvme_ctrlr_construct_intel_support_log_page_list(void)
{
	bool	res;
	struct spdk_nvme_ctrlr				ctrlr = {};
	struct spdk_nvme_intel_log_page_directory	payload = {};
	struct spdk_pci_id 				pci_id = {};

	/* Get quirks for a device with all 0 vendor/device id */
	ctrlr.quirks = nvme_get_quirks(&pci_id);
	CU_ASSERT(ctrlr.quirks == 0);

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == false);

	/* Set the vendor to Intel, but provide no device id */
	ctrlr.cdata.vid = pci_id.vendor_id = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 1;
	ctrlr.quirks = nvme_get_quirks(&pci_id);
	memset(ctrlr.log_page_supported, 0, sizeof(ctrlr.log_page_supported));

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
	CU_ASSERT(res == false);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_SMART);
	CU_ASSERT(res == false);

	/* set valid vendor id, device id and sub device id */
	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 0;
	pci_id.vendor_id = SPDK_PCI_VID_INTEL;
	pci_id.device_id = 0x0953;
	pci_id.subvendor_id = SPDK_PCI_VID_INTEL;
	pci_id.subdevice_id = 0x3702;
	ctrlr.quirks = nvme_get_quirks(&pci_id);
	memset(ctrlr.log_page_supported, 0, sizeof(ctrlr.log_page_supported));

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_PAGE_DIRECTORY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == false);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_SMART);
	CU_ASSERT(res == false);
}

static void
test_nvme_ctrlr_set_supported_features(void)
{
	bool	res;
	struct spdk_nvme_ctrlr			ctrlr = {};

	/* set a invalid vendor id */
	ctrlr.cdata.vid = 0xFFFF;
	nvme_ctrlr_set_supported_features(&ctrlr);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_FEAT_ARBITRATION);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_INTEL_FEAT_MAX_LBA);
	CU_ASSERT(res == false);

	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	nvme_ctrlr_set_supported_features(&ctrlr);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_FEAT_ARBITRATION);
	CU_ASSERT(res == true);
	res = spdk_nvme_ctrlr_is_feature_supported(&ctrlr, SPDK_NVME_INTEL_FEAT_MAX_LBA);
	CU_ASSERT(res == true);
}

static void
test_ctrlr_opts_set_defaults(void)
{
	struct spdk_nvme_ctrlr_opts opts = {};

	spdk_nvme_ctrlr_opts_set_defaults(&opts);
	CU_ASSERT_EQUAL(opts.num_io_queues, DEFAULT_MAX_IO_QUEUES);
	CU_ASSERT_FALSE(opts.use_cmb_sqs);
	CU_ASSERT_EQUAL(opts.arb_mechanism, SPDK_NVME_CC_AMS_RR);
	CU_ASSERT_EQUAL(opts.keep_alive_timeout_ms, 10 * 1000);
	CU_ASSERT_EQUAL(opts.io_queue_size, DEFAULT_IO_QUEUE_SIZE);
	CU_ASSERT_STRING_EQUAL(opts.hostnqn, DEFAULT_HOSTNQN);
}

#if 0 /* TODO: move to PCIe-specific unit test */
static void
test_nvme_ctrlr_alloc_cmb(void)
{
	int			rc;
	uint64_t		offset;
	struct spdk_nvme_ctrlr	ctrlr = {};

	ctrlr.cmb_size = 0x1000000;
	ctrlr.cmb_current_offset = 0x100;
	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x200, 0x1000, &offset);
	CU_ASSERT(rc == 0);
	CU_ASSERT(offset == 0x1000);
	CU_ASSERT(ctrlr.cmb_current_offset == 0x1200);

	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x800, 0x1000, &offset);
	CU_ASSERT(rc == 0);
	CU_ASSERT(offset == 0x2000);
	CU_ASSERT(ctrlr.cmb_current_offset == 0x2800);

	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x800000, 0x100000, &offset);
	CU_ASSERT(rc == 0);
	CU_ASSERT(offset == 0x100000);
	CU_ASSERT(ctrlr.cmb_current_offset == 0x900000);

	rc = nvme_ctrlr_alloc_cmb(&ctrlr, 0x8000000, 0x1000, &offset);
	CU_ASSERT(rc == -1);
}
#endif

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvme_ctrlr", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test nvme_ctrlr init CC.EN = 1 CSTS.RDY = 0",
			    test_nvme_ctrlr_init_en_1_rdy_0) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr init CC.EN = 1 CSTS.RDY = 1",
			       test_nvme_ctrlr_init_en_1_rdy_1) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr init CC.EN = 0 CSTS.RDY = 0",
			       test_nvme_ctrlr_init_en_0_rdy_0) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr init CC.EN = 0 CSTS.RDY = 1",
			       test_nvme_ctrlr_init_en_0_rdy_1) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr init CC.EN = 0 CSTS.RDY = 0 AMS = RR",
			       test_nvme_ctrlr_init_en_0_rdy_0_ams_rr) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr init CC.EN = 0 CSTS.RDY = 0 AMS = WRR",
			       test_nvme_ctrlr_init_en_0_rdy_0_ams_wrr) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr init CC.EN = 0 CSTS.RDY = 0 AMS = VS",
			       test_nvme_ctrlr_init_en_0_rdy_0_ams_vs) == NULL
		|| CU_add_test(suite, "alloc_io_qpair_rr 1", test_alloc_io_qpair_rr_1) == NULL
		|| CU_add_test(suite, "set_defaults", test_ctrlr_opts_set_defaults) == NULL
		|| CU_add_test(suite, "alloc_io_qpair_wrr 1", test_alloc_io_qpair_wrr_1) == NULL
		|| CU_add_test(suite, "alloc_io_qpair_wrr 2", test_alloc_io_qpair_wrr_2) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr function nvme_ctrlr_fail", test_nvme_ctrlr_fail) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_construct_intel_support_log_page_list",
			       test_nvme_ctrlr_construct_intel_support_log_page_list) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_set_supported_features",
			       test_nvme_ctrlr_set_supported_features) == NULL
#if 0 /* TODO: move to PCIe-specific unit test */
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_alloc_cmb",
			       test_nvme_ctrlr_alloc_cmb) == NULL
#endif
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
