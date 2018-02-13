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

#include "nvmf/ctrlr.c"

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)


DEFINE_STUB(spdk_nvmf_tgt_find_subsystem,
	    struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt, const char *subnqn),
	    NULL)

DEFINE_STUB(spdk_nvmf_poll_group_create,
	    struct spdk_nvmf_poll_group *,
	    (struct spdk_nvmf_tgt *tgt),
	    NULL)

DEFINE_STUB_V(spdk_nvmf_poll_group_destroy,
	      (struct spdk_nvmf_poll_group *group))

DEFINE_STUB_V(spdk_nvmf_transport_qpair_fini,
	      (struct spdk_nvmf_qpair *qpair))

DEFINE_STUB(spdk_nvmf_poll_group_add,
	    int,
	    (struct spdk_nvmf_poll_group *group, struct spdk_nvmf_qpair *qpair),
	    0)

DEFINE_STUB(spdk_nvmf_poll_group_remove,
	    int,
	    (struct spdk_nvmf_poll_group *group, struct spdk_nvmf_qpair *qpair),
	    0)

DEFINE_STUB(spdk_nvmf_subsystem_get_sn,
	    const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    NULL)

DEFINE_STUB(spdk_nvmf_subsystem_get_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, uint32_t nsid),
	    NULL)

DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem),
	    NULL)

DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns,
	    struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns),
	    NULL)

DEFINE_STUB(spdk_nvmf_subsystem_host_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, const char *hostnqn),
	    true)

DEFINE_STUB(spdk_nvmf_subsystem_add_ctrlr,
	    int,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr),
	    0)

DEFINE_STUB_V(spdk_nvmf_subsystem_remove_ctrlr,
	      (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr))

DEFINE_STUB(spdk_nvmf_subsystem_get_ctrlr,
	    struct spdk_nvmf_ctrlr *,
	    (struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid),
	    NULL)

DEFINE_STUB(spdk_nvmf_ctrlr_dsm_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false)

DEFINE_STUB(spdk_nvmf_ctrlr_write_zeroes_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false)

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_identify_ns,
	    int,
	    (struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata),
	    -1)

DEFINE_STUB_V(spdk_nvmf_get_discovery_log_page,
	      (struct spdk_nvmf_tgt *tgt, void *buffer, uint64_t offset, uint32_t length))

DEFINE_STUB(spdk_nvmf_request_complete,
	    int,
	    (struct spdk_nvmf_request *req),
	    -1)

DEFINE_STUB(spdk_nvmf_request_abort,
	    int,
	    (struct spdk_nvmf_request *req),
	    -1)

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

void
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx)
{
	fn(ctx);
}

static void
test_connect(void)
{
	struct spdk_nvmf_fabric_connect_data connect_data;
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

	memset(&group, 0, sizeof(group));

	memset(&ctrlr, 0, sizeof(ctrlr));
	TAILQ_INIT(&ctrlr.qpairs);
	ctrlr.subsys = &subsystem;
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.vcprop.cc.bits.iosqes = 6;
	ctrlr.vcprop.cc.bits.iocqes = 4;
	ctrlr.max_qpairs_allowed = 3;

	memset(&admin_qpair, 0, sizeof(admin_qpair));
	TAILQ_INSERT_TAIL(&ctrlr.qpairs, &admin_qpair, link);
	admin_qpair.group = &group;

	memset(&tgt, 0, sizeof(tgt));
	tgt.opts.max_queue_depth = 64;
	tgt.opts.max_qpairs_per_ctrlr = 3;

	memset(&transport, 0, sizeof(transport));
	transport.tgt = &tgt;

	memset(&qpair, 0, sizeof(qpair));
	qpair.transport = &transport;
	qpair.group = &group;

	memset(&connect_data, 0, sizeof(connect_data));
	memcpy(connect_data.hostid, hostid, sizeof(hostid));
	connect_data.cntlid = 0xFFFF;
	strncpy(connect_data.subnqn, subnqn, sizeof(connect_data.subnqn));
	strncpy(connect_data.hostnqn, hostnqn, sizeof(connect_data.hostnqn));

	memset(&subsystem, 0, sizeof(subsystem));
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

	MOCK_SET(spdk_nvmf_tgt_find_subsystem, struct spdk_nvmf_subsystem *, &subsystem);
	MOCK_SET(spdk_nvmf_poll_group_create, struct spdk_nvmf_poll_group *, &group);

	/* Valid admin connect command */
	memset(&rsp, 0, sizeof(rsp));
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
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
	strncpy(connect_data.subnqn, subnqn, sizeof(connect_data.subnqn));

	/* Subsystem not found */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_tgt_find_subsystem, struct spdk_nvmf_subsystem *, NULL);
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 256);
	CU_ASSERT(qpair.ctrlr == NULL);
	MOCK_SET(spdk_nvmf_tgt_find_subsystem, struct spdk_nvmf_subsystem *, &subsystem);

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
	strncpy(connect_data.hostnqn, hostnqn, sizeof(connect_data.hostnqn));

	/* Host not allowed */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_subsystem_host_allowed, bool, false);
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_HOST);
	CU_ASSERT(qpair.ctrlr == NULL);
	MOCK_SET(spdk_nvmf_subsystem_host_allowed, bool, true);

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
	MOCK_SET(spdk_nvmf_subsystem_get_ctrlr, struct spdk_nvmf_ctrlr *, &ctrlr);
	cmd.connect_cmd.qid = 1;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr == &ctrlr);
	qpair.ctrlr = NULL;
	ctrlr.num_qpairs = 0;
	TAILQ_INIT(&ctrlr.qpairs);

	/* Non-existent controller */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_subsystem_get_ctrlr, struct spdk_nvmf_ctrlr *, NULL);
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 16);
	CU_ASSERT(qpair.ctrlr == NULL);
	MOCK_SET(spdk_nvmf_subsystem_get_ctrlr, struct spdk_nvmf_ctrlr *, &ctrlr);

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
	ctrlr.num_qpairs = 3;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY);
	CU_ASSERT(qpair.ctrlr == NULL);
	ctrlr.num_qpairs = 0;

	/* I/O connect with duplicate queue ID */
	memset(&rsp, 0, sizeof(rsp));
	memset(&qpair2, 0, sizeof(qpair2));
	qpair2.group = &group;
	qpair2.qid = 1;
	TAILQ_INSERT_TAIL(&ctrlr.qpairs, &qpair, link);
	cmd.connect_cmd.qid = 1;
	rc = spdk_nvmf_ctrlr_connect(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR);
	CU_ASSERT(qpair.ctrlr == NULL);
	TAILQ_INIT(&ctrlr.qpairs);

	/* Clean up globals */
	MOCK_SET(spdk_nvmf_tgt_find_subsystem, struct spdk_nvmf_subsystem *, NULL);
	MOCK_SET(spdk_nvmf_poll_group_create, struct spdk_nvmf_poll_group *, NULL);
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
		CU_add_test(suite, "connect", test_connect) == NULL
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
