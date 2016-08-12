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

#include "spdk_cunit.h"

#include <stdbool.h>

#include "lib/nvme/unit/test_env.c"

bool trace_flag = false;
#define SPDK_TRACE_NVME trace_flag

#include "nvme/nvme_ctrlr.c"

struct nvme_driver _g_nvme_driver = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.request_mempool = NULL,
};

static uint16_t g_pci_vendor_id;
static uint16_t g_pci_device_id;
static uint16_t g_pci_subvendor_id;
static uint16_t g_pci_subdevice_id;

uint64_t g_ut_tsc = 0;
struct spdk_nvme_registers g_ut_nvme_regs = {};

__thread int    nvme_thread_ioq_index = -1;

int
spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	*mapped_addr = &g_ut_nvme_regs;
	*phys_addr = (uintptr_t)&g_ut_nvme_regs;
	*size = sizeof(g_ut_nvme_regs);
	return 0;
}

int
spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr)
{
	return 0;
}

int
spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value,
			   uint32_t offset)
{
	*value = 0xFFFFFFFFu;
	return 0;
}

int
spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value,
			    uint32_t offset)
{
	return 0;
}

uint16_t
spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev)
{
	return g_pci_vendor_id;
}

uint16_t
spdk_pci_device_get_device_id(struct spdk_pci_device *dev)
{
	return g_pci_device_id;
}

uint16_t
spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev)
{
	return g_pci_subvendor_id;
}

uint16_t
spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev)
{
	return g_pci_subdevice_id;
}

int nvme_qpair_construct(struct spdk_nvme_qpair *qpair, uint16_t id,
			 uint16_t num_entries, uint16_t num_trackers,
			 struct spdk_nvme_ctrlr *ctrlr)
{
	qpair->id = id;
	qpair->num_entries = num_entries;
	qpair->qprio = 0;
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
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				 uint32_t nsid, void *payload, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

void
nvme_qpair_fail(struct spdk_nvme_qpair *qpair)
{
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	CU_ASSERT(req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST);

	/*
	 * Free the request here so it does not leak.
	 * For the purposes of this unit test, we don't need to bother emulating request submission.
	 */
	spdk_mempool_put(_g_nvme_driver.request_mempool, req);

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
nvme_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
}

void
nvme_qpair_enable(struct spdk_nvme_qpair *qpair)
{
}

void
nvme_qpair_reset(struct spdk_nvme_qpair *qpair)
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
nvme_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
			    struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
			    void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
			    struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
			    void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	fake_cpl_success(cb_fn, cb_arg);
	return 0;
}

int
nvme_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
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
nvme_allocate_request(const struct nvme_payload *payload, uint32_t payload_size,
		      spdk_nvme_cmd_cb cb_fn,
		      void *cb_arg)
{
	struct nvme_request *req = NULL;
	req = spdk_mempool_get(_g_nvme_driver.request_mempool);

	if (req != NULL) {
		memset(req, 0, offsetof(struct nvme_request, children));

		req->payload = *payload;
		req->payload_size = payload_size;

		req->cb_fn = cb_fn;
		req->cb_arg = cb_arg;
	}

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

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
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
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
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

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Transition to CSTS.RDY = 0.
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
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

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to default round robin arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
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

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_WRR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_WRR);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to weighted round robin arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
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

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	/*
	 * Case 1: default round robin arbitration mechanism selected
	 */
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_RR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_RR);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_RR);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 2: weighted round robin arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_WRR;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 3: vendor specific arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.ams == SPDK_NVME_CC_AMS_VS);
	CU_ASSERT(ctrlr.opts.arb_mechanism == SPDK_NVME_CC_AMS_VS);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 4: invalid arbitration mechanism selected
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS + 1;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) != 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1);
	CU_ASSERT(g_ut_nvme_regs.cc.bits.en == 0);

	/*
	 * Reset to initial state
	 */
	g_ut_nvme_regs.cc.bits.en = 0;
	g_ut_nvme_regs.csts.bits.rdy = 0;

	/*
	 * Case 5: reset to vendor specific arbitration mechanism
	 */
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	ctrlr.opts.arb_mechanism = SPDK_NVME_CC_AMS_VS;

	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
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

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
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

	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(&ctrlr, NULL) == 0);
	ctrlr.cdata.nn = 1;
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_INIT);
	CU_ASSERT(nvme_ctrlr_process_init(&ctrlr) == 0);
	CU_ASSERT(ctrlr.state == NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0);

	/*
	 * Transition to CSTS.RDY = 0.
	 * init() should set CC.EN = 1.
	 */
	g_ut_nvme_regs.csts.bits.rdy = 0;
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
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct(ctrlr, NULL) == 0);

	/* Fake out the parts of ctrlr needed for I/O qpair allocation */
	ctrlr->opts.num_io_queues = num_io_queues;
	SPDK_CU_ASSERT_FATAL(nvme_ctrlr_construct_io_qpairs(ctrlr) == 0);
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
	nvme_ctrlr_fail(&ctrlr);

	CU_ASSERT(ctrlr.is_failed == true);
}

static void
test_nvme_ctrlr_construct_intel_support_log_page_list(void)
{
	bool	res;
	struct spdk_nvme_ctrlr				ctrlr = {};
	struct spdk_nvme_intel_log_page_directory	payload = {};

	/* set a invalid vendor id */
	ctrlr.cdata.vid = 0xFFFF;

	nvme_ctrlr_construct_intel_support_log_page_list(&ctrlr, &payload);
	res = spdk_nvme_ctrlr_is_log_page_supported(&ctrlr, SPDK_NVME_INTEL_LOG_TEMPERATURE);
	CU_ASSERT(res == false);

	/* set valid vendor id and log page directory*/
	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 1;
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

	/* set valid vendor id, device id and sub device id*/
	ctrlr.cdata.vid = SPDK_PCI_VID_INTEL;
	payload.temperature_statistics_log_len = 0;
	g_pci_vendor_id = SPDK_PCI_VID_INTEL;
	g_pci_device_id = 0x0953;
	g_pci_subvendor_id = SPDK_PCI_VID_INTEL;
	g_pci_subdevice_id = 0x3702;
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
		|| CU_add_test(suite, "alloc_io_qpair_wrr 1", test_alloc_io_qpair_wrr_1) == NULL
		|| CU_add_test(suite, "alloc_io_qpair_wrr 2", test_alloc_io_qpair_wrr_2) == NULL
		|| CU_add_test(suite, "test nvme_ctrlr function nvme_ctrlr_fail", test_nvme_ctrlr_fail) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_construct_intel_support_log_page_list",
			       test_nvme_ctrlr_construct_intel_support_log_page_list) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_set_supported_features",
			       test_nvme_ctrlr_set_supported_features) == NULL
		|| CU_add_test(suite, "test nvme ctrlr function nvme_ctrlr_alloc_cmb",
			       test_nvme_ctrlr_alloc_cmb) == NULL
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
