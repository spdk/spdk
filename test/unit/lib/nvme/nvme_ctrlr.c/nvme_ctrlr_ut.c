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

#include "common/lib/test_env.c"

struct spdk_trace_flag SPDK_LOG_NVME = {
	.name = "nvme",
	.enabled = false,
};

#include "nvme/nvme_ctrlr.c"
#include "nvme/nvme_quirks.c"

pid_t g_spdk_nvme_pid;

struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

struct nvme_driver *g_spdk_nvme_driver = &_g_nvme_driver;

struct spdk_nvme_registers g_ut_nvme_regs = {};

__thread int    nvme_thread_ioq_index = -1;

uint32_t set_size = 1;

int set_status_cpl = -1;

DEFINE_STUB(nvme_ctrlr_cmd_set_host_id, int,
	    (struct spdk_nvme_ctrlr *ctrlr, void *host_id, uint32_t host_id_size,
	     spdk_nvme_cmd_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(nvme_ctrlr_identify_ns, int, (struct spdk_nvme_ns *ns), 0);
DEFINE_STUB(nvme_ctrlr_identify_id_desc, int, (struct spdk_nvme_ns *ns), 0);
DEFINE_STUB_V(nvme_ns_set_identify_data, (struct spdk_nvme_ns *ns));

struct spdk_nvme_ctrlr *nvme_transport_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	return NULL;
}

int
nvme_transport_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	nvme_ctrlr_destruct_finish(ctrlr);

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

uint16_t
nvme_transport_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return 1;
}

void *
nvme_transport_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	return NULL;
}

int
nvme_transport_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	return 0;
}

struct spdk_nvme_qpair *
nvme_transport_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				     const struct spdk_nvme_io_qpair_opts *opts)
{
	struct spdk_nvme_qpair *qpair;

	qpair = calloc(1, sizeof(*qpair));
	SPDK_CU_ASSERT_FATAL(qpair != NULL);

	qpair->ctrlr = ctrlr;
	qpair->id = qid;
	qpair->qprio = opts->qprio;

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

int
nvme_driver_init(void)
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
	CU_ASSERT(0);
	return -1;
}

int
spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	CU_ASSERT(0);
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
	 * For the purposes of this unit test, we don't need to bother emulating request submission.
	 */

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
spdk_nvme_wait_for_completion_robust_lock(
	struct spdk_nvme_qpair *qpair,
	struct nvme_completion_poll_status *status,
	pthread_mutex_t *robust_mutex)
{
	status->done = true;
	memset(&status->cpl, 0, sizeof(status->cpl));
	status->cpl.status.sc = 0;
	if (set_status_cpl == 1) {
		status->cpl.status.sc = 1;
	}
	return spdk_nvme_cpl_is_error(&status->cpl) ? -EIO : 0;
}

int
spdk_nvme_wait_for_completion(struct spdk_nvme_qpair *qpair,
			      struct nvme_completion_poll_status *status)
{
	return spdk_nvme_wait_for_completion_robust_lock(qpair, status, NULL);
}


int
nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_feat_async_event_configuration config, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid, uint32_t nsid,
			void *payload, size_t payload_size,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	if (cns == SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST) {
		uint32_t count = 0;
		uint32_t i = 0;
		struct spdk_nvme_ns_list *ns_list = (struct spdk_nvme_ns_list *)payload;

		for (i = 1; i <= ctrlr->num_ns; i++) {
			if (i <= nsid) {
				continue;
			}

			ns_list->ns_list[count++] = i;
			if (count == SPDK_COUNTOF(ns_list->ns_list)) {
				break;
			}
		}

	}
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
nvme_ctrlr_cmd_get_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
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
	CU_ASSERT(fw_commit->ca == SPDK_NVME_FW_COMMIT_REPLACE_IMG);
	if (fw_commit->fs == 0) {
		return -1;
	}
	set_status_cpl = 1;
	if (ctrlr->is_resetting == true) {
		set_status_cpl = 0;
	}
	return 0;
}

int
nvme_ctrlr_cmd_fw_image_download(struct spdk_nvme_ctrlr *ctrlr,
				 uint32_t size, uint32_t offset, void *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	if ((size != 0 && payload == NULL) || (size == 0 && payload != NULL)) {
		return -1;
	}
	CU_ASSERT(offset == 0);
	return 0;
}

void
nvme_ns_destruct(struct spdk_nvme_ns *ns)
{
}

int
nvme_ns_construct(struct spdk_nvme_ns *ns, uint32_t id,
		  struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

#define DECLARE_AND_CONSTRUCT_CTRLR()	\
	struct spdk_nvme_ctrlr	ctrlr = {};	\
	struct spdk_nvme_qpair	adminq = {};	\
	struct nvme_request	req;		\
						\
	STAILQ_INIT(&adminq.free_req);		\
	STAILQ_INSERT_HEAD(&adminq.free_req, &req, stailq);	\
	ctrlr.adminq = &adminq;

static void
test_nvme_ctrlr_init_en_1_rdy_0(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 1, CSTS.RDY = 0
	 */
	g_ut_nvme_regs.cc.bits.en = 1;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
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
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_1_rdy_1(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 1, CSTS.RDY = 1
	 * init() should set CC.EN = 0.
	 */
	g_ut_nvme_regs.cc.bits.en = 1;
	g_ut_nvme_regs.csts.bits.rdy = 1;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
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
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_rr(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_wrr(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}
static void
test_nvme_ctrlr_init_en_0_rdy_0_ams_vs(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	ctrlr.page_size = 0x1000;
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
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_0(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 0
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
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
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
test_nvme_ctrlr_init_en_0_rdy_1(void)
{
	DECLARE_AND_CONSTRUCT_CTRLR();

	memset(&g_ut_nvme_regs, 0, sizeof(g_ut_nvme_regs));

	/*
	 * Initial state: CC.EN = 0, CSTS.RDY = 1
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 1;

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.page_size = 0x1000;
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
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_ADMIN_QUEUE);

	/*
	 * Transition to READY.
	 */
	while (ctrlr.state != NVME_CTRLR_STATE_READY) {
		nvme_ctrlr_process_init(&ctrlr);
	}

	g_ut_nvme_regs.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
	nvme_ctrlr_destruct(&ctrlr);
}

static void
setup_qpairs(struct spdk_nvme_ctrlr *ctrlr, uint32_t num_io_queues)
{
	uint32_t i;

	CU_ASSERT(pthread_mutex_init(&ctrlr->ctrlr_lock, NULL) == 0);

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(ctrlr) == 0);

	ctrlr->page_size = 0x1000;
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
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0;

	setup_qpairs(&ctrlr, 1);

	/*
	 * Fake to simulate the controller with default round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_RR;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));

	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, NULL, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	/* Only 1 I/O qpair was allocated, so this should fail */
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, NULL, 0) == NULL);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Now that the qpair has been returned to the free list,
	 * we should be able to allocate it again.
	 */
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, NULL, 0);
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/* Only 0 qprio is acceptable for default round robin arbitration mechanism */
	opts.qprio = 1;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	opts.qprio = 2;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	opts.qprio = 3;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 == NULL);

	/* Only 0 ~ 3 qprio is acceptable */
	opts.qprio = 4;
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts)) == NULL);

	cleanup_qpairs(&ctrlr);
}

static void
test_alloc_io_qpair_wrr_1(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0, *q1;

	setup_qpairs(&ctrlr, 2);

	/*
	 * Fake to simulate the controller with weighted round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_WRR;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));

	/*
	 * Allocate 2 qpairs and free them
	 */
	opts.qprio = 0;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);

	opts.qprio = 1;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);

	/*
	 * Allocate 2 qpairs and free them in the reverse order
	 */
	opts.qprio = 2;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 2);

	opts.qprio = 3;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 3);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q0) == 0);
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_free_io_qpair(q1) == 0);

	/* Only 0 ~ 3 qprio is acceptable */
	opts.qprio = 4;
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts)) == NULL);

	cleanup_qpairs(&ctrlr);
}

static void
test_alloc_io_qpair_wrr_2(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_qpair *q0, *q1, *q2, *q3;

	setup_qpairs(&ctrlr, 4);

	/*
	 * Fake to simulate the controller with weighted round robin
	 * arbitration mechanism.
	 */
	g_ut_nvme_regs.cc.bits.ams = SPDK_NVME_CC_AMS_WRR;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));

	opts.qprio = 0;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 0);

	opts.qprio = 1;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);

	opts.qprio = 2;
	q2 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q2 != NULL);
	SPDK_CU_ASSERT_FATAL(q2->qprio == 2);

	opts.qprio = 3;
	q3 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q3 != NULL);
	SPDK_CU_ASSERT_FATAL(q3->qprio == 3);

	/* Only 4 I/O qpairs was allocated, so this should fail */
	opts.qprio = 0;
	SPDK_CU_ASSERT_FATAL(spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts)) == NULL);
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
	opts.qprio = 1;
	q0 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q0 != NULL);
	SPDK_CU_ASSERT_FATAL(q0->qprio == 1);

	opts.qprio = 1;
	q1 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q1 != NULL);
	SPDK_CU_ASSERT_FATAL(q1->qprio == 1);

	opts.qprio = 3;
	q2 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
	SPDK_CU_ASSERT_FATAL(q2 != NULL);
	SPDK_CU_ASSERT_FATAL(q2->qprio == 3);

	opts.qprio = 3;
	q3 = spdk_nvme_ctrlr_alloc_io_qpair(&ctrlr, &opts, sizeof(opts));
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
	struct spdk_pci_id				pci_id = {};

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
test_ctrlr_get_default_ctrlr_opts(void)
{
	struct spdk_nvme_ctrlr_opts opts = {};

	CU_ASSERT(spdk_uuid_parse(&g_spdk_nvme_driver->default_extended_host_id,
				  "e53e9258-c93b-48b5-be1a-f025af6d232a") == 0);

	memset(&opts, 0, sizeof(opts));

	/* set a smaller opts_size */
	CU_ASSERT(sizeof(opts) > 8);
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, 8);
	CU_ASSERT_EQUAL(opts.num_io_queues, DEFAULT_MAX_IO_QUEUES);
	CU_ASSERT_TRUE(opts.use_cmb_sqs);
	/* check below fields are not initialized by default value */
	CU_ASSERT_EQUAL(opts.arb_mechanism, 0);
	CU_ASSERT_EQUAL(opts.keep_alive_timeout_ms, 0);
	CU_ASSERT_EQUAL(opts.io_queue_size, 0);
	CU_ASSERT_EQUAL(opts.io_queue_requests, 0);
	for (int i = 0; i < 8; i++) {
		CU_ASSERT(opts.host_id[i] == 0);
	}
	for (int i = 0; i < 16; i++) {
		CU_ASSERT(opts.extended_host_id[i] == 0);
	}
	CU_ASSERT(strlen(opts.hostnqn) == 0);
	CU_ASSERT(strlen(opts.src_addr) == 0);
	CU_ASSERT(strlen(opts.src_svcid) == 0);

	/* set a consistent opts_size */
	spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
	CU_ASSERT_EQUAL(opts.num_io_queues, DEFAULT_MAX_IO_QUEUES);
	CU_ASSERT_TRUE(opts.use_cmb_sqs);
	CU_ASSERT_EQUAL(opts.arb_mechanism, SPDK_NVME_CC_AMS_RR);
	CU_ASSERT_EQUAL(opts.keep_alive_timeout_ms, 10 * 1000);
	CU_ASSERT_EQUAL(opts.io_queue_size, DEFAULT_IO_QUEUE_SIZE);
	CU_ASSERT_EQUAL(opts.io_queue_requests, DEFAULT_IO_QUEUE_REQUESTS);
	for (int i = 0; i < 8; i++) {
		CU_ASSERT(opts.host_id[i] == 0);
	}
	CU_ASSERT_STRING_EQUAL(opts.hostnqn,
			       "2014-08.org.nvmexpress:uuid:e53e9258-c93b-48b5-be1a-f025af6d232a");
	CU_ASSERT(memcmp(opts.extended_host_id, &g_spdk_nvme_driver->default_extended_host_id,
			 sizeof(opts.extended_host_id)) == 0);
	CU_ASSERT(strlen(opts.src_addr) == 0);
	CU_ASSERT(strlen(opts.src_svcid) == 0);
}

static void
test_ctrlr_get_default_io_qpair_opts(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct spdk_nvme_io_qpair_opts opts = {};

	memset(&opts, 0, sizeof(opts));

	/* set a smaller opts_size */
	ctrlr.opts.io_queue_size = DEFAULT_IO_QUEUE_SIZE;
	CU_ASSERT(sizeof(opts) > 8);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, 8);
	CU_ASSERT_EQUAL(opts.qprio, SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT_EQUAL(opts.io_queue_size, DEFAULT_IO_QUEUE_SIZE);
	/* check below field is not initialized by default value */
	CU_ASSERT_EQUAL(opts.io_queue_requests, 0);

	/* set a consistent opts_size */
	ctrlr.opts.io_queue_size = DEFAULT_IO_QUEUE_SIZE;
	ctrlr.opts.io_queue_requests = DEFAULT_IO_QUEUE_REQUESTS;
	spdk_nvme_ctrlr_get_default_io_qpair_opts(&ctrlr, &opts, sizeof(opts));
	CU_ASSERT_EQUAL(opts.qprio, SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT_EQUAL(opts.io_queue_size, DEFAULT_IO_QUEUE_SIZE);
	CU_ASSERT_EQUAL(opts.io_queue_requests, DEFAULT_IO_QUEUE_REQUESTS);
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

static void
test_spdk_nvme_ctrlr_update_firmware(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	void *payload = NULL;
	int point_payload = 1;
	int slot = 0;
	int ret = 0;
	struct spdk_nvme_status status;
	enum spdk_nvme_fw_commit_action commit_action = SPDK_NVME_FW_COMMIT_REPLACE_IMG;

	/* Set invalid size check function return value */
	set_size = 5;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* When payload is NULL but set_size < min_page_size */
	set_size = 4;
	ctrlr.min_page_size = 5;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* When payload not NULL but min_page_size is 0 */
	set_size = 4;
	ctrlr.min_page_size = 0;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* Check firmware image download when payload not NULL and min_page_size not 0 , status.cpl value is 1 */
	set_status_cpl = 1;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -ENXIO);

	/* Check firmware image download and set status.cpl value is 0 */
	set_status_cpl = 0;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -1);

	/* Check firmware commit */
	ctrlr.is_resetting = false;
	set_status_cpl = 0;
	slot = 1;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == -ENXIO);

	/* Set size check firmware download and firmware commit */
	ctrlr.is_resetting = true;
	set_status_cpl = 0;
	slot = 1;
	set_size = 4;
	ctrlr.min_page_size = 5;
	payload = &point_payload;
	ret = spdk_nvme_ctrlr_update_firmware(&ctrlr, payload, set_size, slot, commit_action, &status);
	CU_ASSERT(ret == 0);

	set_status_cpl = 0;
}

int
nvme_ctrlr_cmd_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr, uint64_t prp1, uint64_t prp2,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

static void
test_spdk_nvme_ctrlr_doorbell_buffer_config(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	int ret = -1;

	ctrlr.cdata.oacs.doorbell_buffer_config = 1;
	ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	ctrlr.page_size = 0x1000;
	MOCK_CLEAR(spdk_malloc)
	MOCK_CLEAR(spdk_zmalloc)
	MOCK_CLEAR(spdk_dma_malloc)
	MOCK_CLEAR(spdk_dma_zmalloc)
	ret = nvme_ctrlr_set_doorbell_buffer_config(&ctrlr);
	CU_ASSERT(ret == 0);
	nvme_ctrlr_free_doorbell_buffer(&ctrlr);
}

static void
test_nvme_ctrlr_test_active_ns(void)
{
	uint32_t		nsid, minor;
	size_t			ns_id_count;
	struct spdk_nvme_ctrlr	ctrlr = {};

	ctrlr.page_size = 0x1000;

	for (minor = 0; minor <= 2; minor++) {
		ctrlr.cdata.ver.bits.mjr = 1;
		ctrlr.cdata.ver.bits.mnr = minor;
		ctrlr.cdata.ver.bits.ter = 0;
		ctrlr.num_ns = 1531;
		nvme_ctrlr_identify_active_ns(&ctrlr);

		for (nsid = 1; nsid <= ctrlr.num_ns; nsid++) {
			CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, nsid) == true);
		}
		ctrlr.num_ns = 1559;
		for (; nsid <= ctrlr.num_ns; nsid++) {
			CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, nsid) == false);
		}
		ctrlr.num_ns = 1531;
		for (nsid = 0; nsid < ctrlr.num_ns; nsid++) {
			ctrlr.active_ns_list[nsid] = 0;
		}
		CU_ASSERT(spdk_nvme_ctrlr_get_first_active_ns(&ctrlr) == 0);

		ctrlr.active_ns_list[0] = 1;
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1) == true);
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 2) == false);
		nsid = spdk_nvme_ctrlr_get_first_active_ns(&ctrlr);
		CU_ASSERT(nsid == 1);

		ctrlr.active_ns_list[1] = 3;
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 1) == true);
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 2) == false);
		CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, 3) == true);
		nsid = spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, nsid);
		CU_ASSERT(nsid == 3);
		nsid = spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, nsid);
		CU_ASSERT(nsid == 0);

		memset(ctrlr.active_ns_list, 0, ctrlr.num_ns);
		for (nsid = 0; nsid < ctrlr.num_ns; nsid++) {
			ctrlr.active_ns_list[nsid] = nsid + 1;
		}

		ns_id_count = 0;
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(&ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(&ctrlr, nsid)) {
			CU_ASSERT(spdk_nvme_ctrlr_is_active_ns(&ctrlr, nsid) == true);
			ns_id_count++;
		}
		CU_ASSERT(ns_id_count == ctrlr.num_ns);

		nvme_ctrlr_destruct(&ctrlr);
	}
}

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
		|| CU_add_test(suite, "get_default_ctrlr_opts", test_ctrlr_get_default_ctrlr_opts) == NULL
		|| CU_add_test(suite, "get_default_io_qpair_opts", test_ctrlr_get_default_io_qpair_opts) == NULL
		|| CU_add_test(suite, "alloc_io_qpair_wrr 1", test_alloc_io_qpair_wrr_1) == NULL
		|| CU_add_test(suite, "alloc_io_qpair_wrr 2", test_alloc_io_qpair_wrr_2) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function update_firmware",
			       test_spdk_nvme_ctrlr_update_firmware) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr function nvme_ctrlr_fail", test_nvme_ctrlr_fail) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_construct_intel_support_log_page_list",
			       test_nvme_ctrlr_construct_intel_support_log_page_list) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_set_supported_features",
			       test_nvme_ctrlr_set_supported_features) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_set_doorbell_buffer_config",
			       test_spdk_nvme_ctrlr_doorbell_buffer_config) == NULL
#if 0 /* TODO: move to PCIe-specific unit test */
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_alloc_cmb",
			       test_nvme_ctrlr_alloc_cmb) == NULL
#endif
		|| CU_add_test(suite, "test nvme ctrlr function test_nvme_ctrlr_test_active_ns",
			       test_nvme_ctrlr_test_active_ns) == NULL
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
