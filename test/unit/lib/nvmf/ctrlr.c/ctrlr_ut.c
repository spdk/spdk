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
#include "spdk_internal/mock.h"

#include "common/lib/test_env.c"
#include "nvmf/ctrlr.c"

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)

struct spdk_bdev {
	int ut_mock;
	uint64_t blockcnt;
};

DEFINE_STUB(spdk_nvmf_tgt_find_subsystem,
	    struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt, const char *subnqn),
	    NULL);

DEFINE_STUB(spdk_nvmf_poll_group_create,
	    struct spdk_nvmf_poll_group *,
	    (struct spdk_nvmf_tgt *tgt),
	    NULL);

DEFINE_STUB_V(spdk_nvmf_poll_group_destroy,
	      (struct spdk_nvmf_poll_group *group));

DEFINE_STUB_V(spdk_nvmf_transport_qpair_fini,
	      (struct spdk_nvmf_qpair *qpair));

DEFINE_STUB(spdk_nvmf_poll_group_add,
	    int,
	    (struct spdk_nvmf_poll_group *group, struct spdk_nvmf_qpair *qpair),
	    0);

DEFINE_STUB(spdk_nvmf_subsystem_get_sn,
	    const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_get_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, uint32_t nsid),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_host_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, const char *hostnqn),
	    true);

DEFINE_STUB(spdk_nvmf_subsystem_add_ctrlr,
	    int,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr),
	    0);

DEFINE_STUB_V(spdk_nvmf_subsystem_remove_ctrlr,
	      (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr));

DEFINE_STUB(spdk_nvmf_subsystem_get_ctrlr,
	    struct spdk_nvmf_ctrlr *,
	    (struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid),
	    NULL);

DEFINE_STUB(spdk_nvmf_ctrlr_dsm_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB(spdk_nvmf_ctrlr_write_zeroes_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB_V(spdk_nvmf_get_discovery_log_page,
	      (struct spdk_nvmf_tgt *tgt, void *buffer, uint64_t offset, uint32_t length));

DEFINE_STUB(spdk_nvmf_request_complete,
	    int,
	    (struct spdk_nvmf_request *req),
	    -1);

DEFINE_STUB(spdk_nvmf_request_free,
	    int,
	    (struct spdk_nvmf_request *req),
	    -1);

DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid,
	    int,
	    (struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid),
	    0);

DEFINE_STUB(spdk_nvmf_subsystem_listener_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvme_transport_id *trid),
	    true);

static void
ctrlr_ut_pass_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

void
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata)
{
	uint64_t num_blocks;

	SPDK_CU_ASSERT_FATAL(ns->bdev != NULL);
	num_blocks = ns->bdev->blockcnt;
	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->lbaf[0].lbads = spdk_u32log2(512);
}

static void
test_get_log_page(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	char data[4096];

	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	ctrlr.subsys = &subsystem;

	qpair.ctrlr = &ctrlr;

	req.qpair = &qpair;
	req.cmd = &cmd;
	req.rsp = &rsp;
	req.data = &data;
	req.length = sizeof(data);

	/* Get Log Page - all valid */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = SPDK_NVME_LOG_ERROR | (req.length / 4 - 1) << 16;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Get Log Page with invalid log ID */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = 0;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Get Log Page with invalid offset (not dword aligned) */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = SPDK_NVME_LOG_ERROR | (req.length / 4 - 1) << 16;
	cmd.nvme_cmd.cdw12 = 2;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Get Log Page without data buffer */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	req.data = NULL;
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = SPDK_NVME_LOG_ERROR | (req.length / 4 - 1) << 16;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);
	req.data = data;
}

static void
test_process_fabrics_cmd(void)
{
	struct	spdk_nvmf_request req = {};
	int	ret;
	struct	spdk_nvmf_qpair req_qpair = {};
	union	nvmf_h2c_msg  req_cmd = {};
	union	nvmf_c2h_msg   req_rsp = {};

	req.qpair = &req_qpair;
	req.cmd  = &req_cmd;
	req.rsp  = &req_rsp;
	req.qpair->ctrlr = NULL;

	/* No ctrlr and invalid command check */
	req.cmd->nvmf_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	ret = spdk_nvmf_ctrlr_process_fabrics_cmd(&req);
	CU_ASSERT_EQUAL(req.rsp->nvme_cpl.status.sc, SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR);
	CU_ASSERT_EQUAL(ret, SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
}

static bool
nvme_status_success(const struct spdk_nvme_status *status)
{
	return status->sct == SPDK_NVME_SCT_GENERIC && status->sc == SPDK_NVME_SC_SUCCESS;
}

static void
test_connect(void)
{
	struct spdk_nvmf_fabric_connect_data connect_data;
	struct spdk_thread *thread;
	struct spdk_nvmf_poll_group group;
	struct spdk_nvmf_transport transport;
	struct spdk_nvmf_subsystem subsystem;
	struct spdk_nvmf_request req;
	struct spdk_nvmf_qpair admin_qpair;
	struct spdk_nvmf_qpair qpair;
	struct spdk_nvmf_qpair qpair2;
	struct spdk_nvmf_ctrlr ctrlr;
	struct spdk_nvmf_tgt tgt;
	union nvmf_h2c_msg cmd;
	union nvmf_c2h_msg rsp;
	const uint8_t hostid[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
	};
	const char subnqn[] = "nqn.2016-06.io.spdk:subsystem1";
	const char hostnqn[] = "nqn.2016-06.io.spdk:host1";
	int rc;

	thread = spdk_allocate_thread(ctrlr_ut_pass_msg, NULL, NULL, NULL, "ctrlr_ut");
	SPDK_CU_ASSERT_FATAL(thread != NULL);

	memset(&group, 0, sizeof(group));
	group.thread = thread;

	memset(&ctrlr, 0, sizeof(ctrlr));
	ctrlr.subsys = &subsystem;
	ctrlr.qpair_mask = spdk_bit_array_create(3);
	SPDK_CU_ASSERT_FATAL(ctrlr.qpair_mask != NULL);
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.vcprop.cc.bits.iosqes = 6;
	ctrlr.vcprop.cc.bits.iocqes = 4;

	memset(&admin_qpair, 0, sizeof(admin_qpair));
	admin_qpair.group = &group;

	memset(&tgt, 0, sizeof(tgt));
	memset(&transport, 0, sizeof(transport));
	transport.opts.max_queue_depth = 64;
	transport.opts.max_qpairs_per_ctrlr = 3;
	transport.tgt = &tgt;

	memset(&qpair, 0, sizeof(qpair));
	qpair.transport = &transport;
	qpair.group = &group;

	memset(&connect_data, 0, sizeof(connect_data));
	memcpy(connect_data.hostid, hostid, sizeof(hostid));
	connect_data.cntlid = 0xFFFF;
	snprintf(connect_data.subnqn, sizeof(connect_data.subnqn), "%s", subnqn);
	snprintf(connect_data.hostnqn, sizeof(connect_data.hostnqn), "%s", hostnqn);

	memset(&subsystem, 0, sizeof(subsystem));
	subsystem.thread = thread;
	subsystem.id = 1;
	TAILQ_INIT(&subsystem.ctrlrs);
	subsystem.tgt = &tgt;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;
	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "%s", subnqn);

	memset(&cmd, 0, sizeof(cmd));
	cmd.connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.connect_cmd.cid = 1;
	cmd.connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	cmd.connect_cmd.recfmt = 0;
	cmd.connect_cmd.qid = 0;
	cmd.connect_cmd.sqsize = 31;
	cmd.connect_cmd.cattr = 0;
	cmd.connect_cmd.kato = 120000;

	memset(&req, 0, sizeof(req));
	req.qpair = &qpair;
	req.length = sizeof(connect_data);
	req.xfer = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
	req.data = &connect_data;
	req.cmd = &cmd;
	req.rsp = &rsp;

	MOCK_SET(spdk_nvmf_tgt_find_subsystem, &subsystem);
	MOCK_SET(spdk_nvmf_poll_group_create, &group);

	/* Valid admin connect command */
	memset(&rsp, 0, sizeof(rsp));
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;

	/* Invalid data length */
	memset(&rsp, 0, sizeof(rsp));
	req.length = sizeof(connect_data) - 1;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT(qpair.ctrlr == NULL);
	req.length = sizeof(connect_data);

	/* Invalid recfmt */
	memset(&rsp, 0, sizeof(rsp));
	cmd.connect_cmd.recfmt = 1234;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.recfmt = 0;

	/* Unterminated subnqn */
	memset(&rsp, 0, sizeof(rsp));
	memset(connect_data.subnqn, 'a', sizeof(connect_data.subnqn));
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 256);
	CU_ASSERT(qpair.ctrlr == NULL);
	snprintf(connect_data.subnqn, sizeof(connect_data.subnqn), "%s", subnqn);

	/* Subsystem not found */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_tgt_find_subsystem, NULL);
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 256);
	CU_ASSERT(qpair.ctrlr == NULL);
	MOCK_SET(spdk_nvmf_tgt_find_subsystem, &subsystem);

	/* Unterminated hostnqn */
	memset(&rsp, 0, sizeof(rsp));
	memset(connect_data.hostnqn, 'b', sizeof(connect_data.hostnqn));
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 512);
	CU_ASSERT(qpair.ctrlr == NULL);
	snprintf(connect_data.hostnqn, sizeof(connect_data.hostnqn), "%s", hostnqn);

	/* Host not allowed */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_subsystem_host_allowed, false);
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_HOST);
	CU_ASSERT(qpair.ctrlr == NULL);
	MOCK_SET(spdk_nvmf_subsystem_host_allowed, true);

	/* Invalid sqsize == 0 */
	memset(&rsp, 0, sizeof(rsp));
	cmd.connect_cmd.sqsize = 0;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 44);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.sqsize = 31;

	/* Invalid sqsize > max_queue_depth */
	memset(&rsp, 0, sizeof(rsp));
	cmd.connect_cmd.sqsize = 64;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 44);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.sqsize = 31;

	/* Invalid cntlid for admin queue */
	memset(&rsp, 0, sizeof(rsp));
	connect_data.cntlid = 0x1234;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 16);
	CU_ASSERT(qpair.ctrlr == NULL);
	connect_data.cntlid = 0xFFFF;

	ctrlr.admin_qpair = &admin_qpair;
	ctrlr.subsys = &subsystem;

	/* Valid I/O queue connect command */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_subsystem_get_ctrlr, &ctrlr);
	cmd.connect_cmd.qid = 1;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr == &ctrlr);
	qpair.ctrlr = NULL;

	/* Non-existent controller */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_subsystem_get_ctrlr, NULL);
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 16);
	CU_ASSERT(qpair.ctrlr == NULL);
	MOCK_SET(spdk_nvmf_subsystem_get_ctrlr, &ctrlr);

	/* I/O connect to discovery controller */
	memset(&rsp, 0, sizeof(rsp));
	subsystem.subtype = SPDK_NVMF_SUBTYPE_DISCOVERY;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	/* I/O connect to disabled controller */
	memset(&rsp, 0, sizeof(rsp));
	ctrlr.vcprop.cc.bits.en = 0;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	ctrlr.vcprop.cc.bits.en = 1;

	/* I/O connect with invalid IOSQES */
	memset(&rsp, 0, sizeof(rsp));
	ctrlr.vcprop.cc.bits.iosqes = 3;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	ctrlr.vcprop.cc.bits.iosqes = 6;

	/* I/O connect with invalid IOCQES */
	memset(&rsp, 0, sizeof(rsp));
	ctrlr.vcprop.cc.bits.iocqes = 3;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	ctrlr.vcprop.cc.bits.iocqes = 4;

	/* I/O connect with too many existing qpairs */
	memset(&rsp, 0, sizeof(rsp));
	spdk_bit_array_set(ctrlr.qpair_mask, 0);
	spdk_bit_array_set(ctrlr.qpair_mask, 1);
	spdk_bit_array_set(ctrlr.qpair_mask, 2);
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);
	CU_ASSERT(qpair.ctrlr == NULL);
	spdk_bit_array_clear(ctrlr.qpair_mask, 0);
	spdk_bit_array_clear(ctrlr.qpair_mask, 1);
	spdk_bit_array_clear(ctrlr.qpair_mask, 2);

	/* I/O connect with duplicate queue ID */
	memset(&rsp, 0, sizeof(rsp));
	memset(&qpair2, 0, sizeof(qpair2));
	qpair2.group = &group;
	qpair2.qid = 1;
	spdk_bit_array_set(ctrlr.qpair_mask, 1);
	cmd.connect_cmd.qid = 1;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);
	CU_ASSERT(qpair.ctrlr == NULL);

	/* Clean up globals */
	MOCK_CLEAR(spdk_nvmf_tgt_find_subsystem);
	MOCK_CLEAR(spdk_nvmf_poll_group_create);

	spdk_bit_array_free(&ctrlr.qpair_mask);
	spdk_free_thread();
}

static void
test_get_ns_id_desc_list(void)
{
	struct spdk_nvmf_subsystem subsystem;
	struct spdk_nvmf_qpair qpair;
	struct spdk_nvmf_ctrlr ctrlr;
	struct spdk_nvmf_request req;
	struct spdk_nvmf_ns *ns_ptrs[1];
	struct spdk_nvmf_ns ns;
	union nvmf_h2c_msg cmd;
	union nvmf_c2h_msg rsp;
	struct spdk_bdev bdev;
	uint8_t buf[4096];

	memset(&subsystem, 0, sizeof(subsystem));
	ns_ptrs[0] = &ns;
	subsystem.ns = ns_ptrs;
	subsystem.max_nsid = 1;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	memset(&ns, 0, sizeof(ns));
	ns.opts.nsid = 1;
	ns.bdev = &bdev;

	memset(&qpair, 0, sizeof(qpair));
	qpair.ctrlr = &ctrlr;

	memset(&ctrlr, 0, sizeof(ctrlr));
	ctrlr.subsys = &subsystem;
	ctrlr.vcprop.cc.bits.en = 1;

	memset(&req, 0, sizeof(req));
	req.qpair = &qpair;
	req.cmd = &cmd;
	req.rsp = &rsp;
	req.xfer = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
	req.data = buf;
	req.length = sizeof(buf);

	memset(&cmd, 0, sizeof(cmd));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.nvme_cmd.cdw10 = SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST;

	/* Invalid NSID */
	cmd.nvme_cmd.nsid = 0;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);

	/* Valid NSID, but ns has no IDs defined */
	cmd.nvme_cmd.nsid = 1;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(spdk_mem_all_zero(buf, sizeof(buf)));

	/* Valid NSID, only EUI64 defined */
	ns.opts.eui64[0] = 0x11;
	ns.opts.eui64[7] = 0xFF;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(buf[0] == SPDK_NVME_NIDT_EUI64);
	CU_ASSERT(buf[1] == 8);
	CU_ASSERT(buf[4] == 0x11);
	CU_ASSERT(buf[11] == 0xFF);
	CU_ASSERT(buf[13] == 0);

	/* Valid NSID, only NGUID defined */
	memset(ns.opts.eui64, 0, sizeof(ns.opts.eui64));
	ns.opts.nguid[0] = 0x22;
	ns.opts.nguid[15] = 0xEE;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(buf[0] == SPDK_NVME_NIDT_NGUID);
	CU_ASSERT(buf[1] == 16);
	CU_ASSERT(buf[4] == 0x22);
	CU_ASSERT(buf[19] == 0xEE);
	CU_ASSERT(buf[21] == 0);

	/* Valid NSID, both EUI64 and NGUID defined */
	ns.opts.eui64[0] = 0x11;
	ns.opts.eui64[7] = 0xFF;
	ns.opts.nguid[0] = 0x22;
	ns.opts.nguid[15] = 0xEE;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(buf[0] == SPDK_NVME_NIDT_EUI64);
	CU_ASSERT(buf[1] == 8);
	CU_ASSERT(buf[4] == 0x11);
	CU_ASSERT(buf[11] == 0xFF);
	CU_ASSERT(buf[12] == SPDK_NVME_NIDT_NGUID);
	CU_ASSERT(buf[13] == 16);
	CU_ASSERT(buf[16] == 0x22);
	CU_ASSERT(buf[31] == 0xEE);
	CU_ASSERT(buf[33] == 0);

	/* Valid NSID, EUI64, NGUID, and UUID defined */
	ns.opts.eui64[0] = 0x11;
	ns.opts.eui64[7] = 0xFF;
	ns.opts.nguid[0] = 0x22;
	ns.opts.nguid[15] = 0xEE;
	ns.opts.uuid.u.raw[0] = 0x33;
	ns.opts.uuid.u.raw[15] = 0xDD;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(buf[0] == SPDK_NVME_NIDT_EUI64);
	CU_ASSERT(buf[1] == 8);
	CU_ASSERT(buf[4] == 0x11);
	CU_ASSERT(buf[11] == 0xFF);
	CU_ASSERT(buf[12] == SPDK_NVME_NIDT_NGUID);
	CU_ASSERT(buf[13] == 16);
	CU_ASSERT(buf[16] == 0x22);
	CU_ASSERT(buf[31] == 0xEE);
	CU_ASSERT(buf[32] == SPDK_NVME_NIDT_UUID);
	CU_ASSERT(buf[33] == 16);
	CU_ASSERT(buf[36] == 0x33);
	CU_ASSERT(buf[51] == 0xDD);
	CU_ASSERT(buf[53] == 0);
}

static void
test_identify_ns(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_transport transport = {};
	struct spdk_nvmf_qpair admin_qpair = { .transport = &transport};
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsystem, .admin_qpair = &admin_qpair };
	struct spdk_nvme_cmd cmd = {};
	struct spdk_nvme_cpl rsp = {};
	struct spdk_nvme_ns_data nsdata = {};
	struct spdk_bdev bdev[3] = {{.blockcnt = 1234}, {.blockcnt = 0}, {.blockcnt = 5678}};
	struct spdk_nvmf_ns ns[3] = {{.bdev = &bdev[0]}, {.bdev = NULL}, {.bdev = &bdev[2]}};
	struct spdk_nvmf_ns *ns_arr[3] = {&ns[0], NULL, &ns[2]};

	subsystem.ns = ns_arr;
	subsystem.max_nsid = SPDK_COUNTOF(ns_arr);

	/* Invalid NSID 0 */
	cmd.nsid = 0;
	memset(&nsdata, 0, sizeof(nsdata));
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ns(&ctrlr, &cmd, &rsp,
					      &nsdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	CU_ASSERT(spdk_mem_all_zero(&nsdata, sizeof(nsdata)));

	/* Valid NSID 1 */
	cmd.nsid = 1;
	memset(&nsdata, 0, sizeof(nsdata));
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ns(&ctrlr, &cmd, &rsp,
					      &nsdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(nsdata.nsze == 1234);

	/* Valid but inactive NSID 2 */
	cmd.nsid = 2;
	memset(&nsdata, 0, sizeof(nsdata));
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ns(&ctrlr, &cmd, &rsp,
					      &nsdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(spdk_mem_all_zero(&nsdata, sizeof(nsdata)));

	/* Valid NSID 3 */
	cmd.nsid = 3;
	memset(&nsdata, 0, sizeof(nsdata));
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ns(&ctrlr, &cmd, &rsp,
					      &nsdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(nsdata.nsze == 5678);

	/* Invalid NSID 4 */
	cmd.nsid = 4;
	memset(&nsdata, 0, sizeof(nsdata));
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ns(&ctrlr, &cmd, &rsp,
					      &nsdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	CU_ASSERT(spdk_mem_all_zero(&nsdata, sizeof(nsdata)));

	/* Invalid NSID 0xFFFFFFFF (NS management not supported) */
	cmd.nsid = 0xFFFFFFFF;
	memset(&nsdata, 0, sizeof(nsdata));
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ns(&ctrlr, &cmd, &rsp,
					      &nsdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	CU_ASSERT(spdk_mem_all_zero(&nsdata, sizeof(nsdata)));
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("nvmf", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "get_log_page", test_get_log_page) == NULL ||
		CU_add_test(suite, "process_fabrics_cmd", test_process_fabrics_cmd) == NULL ||
		CU_add_test(suite, "connect", test_connect) == NULL ||
		CU_add_test(suite, "get_ns_id_desc_list", test_get_ns_id_desc_list) == NULL ||
		CU_add_test(suite, "identify_ns", test_identify_ns) == NULL
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
