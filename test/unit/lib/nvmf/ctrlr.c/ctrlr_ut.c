/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk_internal/thread.h"

#include "common/lib/ut_multithread.c"
#include "nvmf/ctrlr.c"

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)

struct spdk_bdev {
	int ut_mock;
	uint64_t blockcnt;
};

const char subsystem_default_sn[SPDK_NVME_CTRLR_SN_LEN + 1] = "subsys_default_sn";
const char subsystem_default_mn[SPDK_NVME_CTRLR_MN_LEN + 1] = "subsys_default_mn";

DEFINE_STUB(spdk_nvmf_tgt_find_subsystem,
	    struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt, const char *subnqn),
	    NULL);

DEFINE_STUB(spdk_nvmf_poll_group_create,
	    struct spdk_nvmf_poll_group *,
	    (struct spdk_nvmf_tgt *tgt),
	    NULL);

DEFINE_STUB(spdk_nvmf_subsystem_get_sn,
	    const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    subsystem_default_sn);

DEFINE_STUB(spdk_nvmf_subsystem_get_mn,
	    const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    subsystem_default_mn);

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
	      (struct spdk_nvmf_tgt *tgt, const char *hostnqn, struct iovec *iov,
	       uint32_t iovcnt, uint64_t offset, uint32_t length));

DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid,
	    int,
	    (struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid),
	    0);

DEFINE_STUB(spdk_nvmf_subsystem_listener_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvme_transport_id *trid),
	    true);

DEFINE_STUB(spdk_nvmf_transport_qpair_set_sqsize,
	    int,
	    (struct spdk_nvmf_qpair *qpair),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_read_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_write_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_write_zeroes_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_flush_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_dsm_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_nvme_passthru_io,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_transport_req_complete,
	    int,
	    (struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB_V(spdk_nvmf_ns_reservation_request, (void *ctx));

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_get_dif_ctx, bool,
	    (struct spdk_bdev *bdev, struct spdk_nvme_cmd *cmd,
	     struct spdk_dif_ctx *dif_ctx),
	    true);

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair, nvmf_qpair_disconnect_cb cb_fn, void *ctx)
{
	return 0;
}

void
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
				 bool dif_insert_or_strip)
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
	cmd.nvme_cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_ERROR;
	cmd.nvme_cmd.cdw10_bits.get_log_page.numdl = (req.length / 4 - 1);
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
	cmd.nvme_cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_ERROR;
	cmd.nvme_cmd.cdw10_bits.get_log_page.numdl = (req.length / 4 - 1);
	cmd.nvme_cmd.cdw12 = 2;
	CU_ASSERT(spdk_nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Get Log Page without data buffer */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	req.data = NULL;
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_ERROR;
	cmd.nvme_cmd.cdw10_bits.get_log_page.numdl = (req.length / 4 - 1);
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
	struct spdk_nvmf_poll_group group;
	struct spdk_nvmf_subsystem_poll_group *sgroups;
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
	group.thread = spdk_get_thread();

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
	transport.opts.max_aq_depth = 32;
	transport.opts.max_queue_depth = 64;
	transport.opts.max_qpairs_per_ctrlr = 3;
	transport.tgt = &tgt;

	memset(&qpair, 0, sizeof(qpair));
	qpair.transport = &transport;
	qpair.group = &group;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	TAILQ_INIT(&qpair.outstanding);

	memset(&connect_data, 0, sizeof(connect_data));
	memcpy(connect_data.hostid, hostid, sizeof(hostid));
	connect_data.cntlid = 0xFFFF;
	snprintf(connect_data.subnqn, sizeof(connect_data.subnqn), "%s", subnqn);
	snprintf(connect_data.hostnqn, sizeof(connect_data.hostnqn), "%s", hostnqn);

	memset(&subsystem, 0, sizeof(subsystem));
	subsystem.thread = spdk_get_thread();
	subsystem.id = 1;
	TAILQ_INIT(&subsystem.ctrlrs);
	subsystem.tgt = &tgt;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "%s", subnqn);

	sgroups = calloc(subsystem.id + 1, sizeof(struct spdk_nvmf_subsystem_poll_group));
	sgroups[subsystem.id].io_outstanding = 5;
	group.sgroups = sgroups;

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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
	spdk_nvmf_ctrlr_stop_keep_alive_timer(qpair.ctrlr);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;

	/* Valid admin connect command with kato = 0 */
	cmd.connect_cmd.kato = 0;
	memset(&rsp, 0, sizeof(rsp));
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL && qpair.ctrlr->keep_alive_poller == NULL);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;
	cmd.connect_cmd.kato = 120000;

	/* Invalid data length */
	memset(&rsp, 0, sizeof(rsp));
	req.length = sizeof(connect_data) - 1;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);
	CU_ASSERT(qpair.ctrlr == NULL);
	req.length = sizeof(connect_data);

	/* Invalid recfmt */
	memset(&rsp, 0, sizeof(rsp));
	cmd.connect_cmd.recfmt = 1234;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.recfmt = 0;

	/* Unterminated subnqn */
	memset(&rsp, 0, sizeof(rsp));
	memset(connect_data.subnqn, 'a', sizeof(connect_data.subnqn));
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_HOST);
	CU_ASSERT(qpair.ctrlr == NULL);
	MOCK_SET(spdk_nvmf_subsystem_host_allowed, true);

	/* Invalid sqsize == 0 */
	memset(&rsp, 0, sizeof(rsp));
	cmd.connect_cmd.sqsize = 0;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 44);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.sqsize = 31;

	/* Invalid admin sqsize > max_aq_depth */
	memset(&rsp, 0, sizeof(rsp));
	cmd.connect_cmd.sqsize = 32;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 44);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.sqsize = 31;

	/* Invalid I/O sqsize > max_queue_depth */
	memset(&rsp, 0, sizeof(rsp));
	cmd.connect_cmd.qid = 1;
	cmd.connect_cmd.sqsize = 64;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 44);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.qid = 0;
	cmd.connect_cmd.sqsize = 31;

	/* Invalid cntlid for admin queue */
	memset(&rsp, 0, sizeof(rsp));
	connect_data.cntlid = 0x1234;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	cmd.connect_cmd.sqsize = 63;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr == &ctrlr);
	qpair.ctrlr = NULL;
	cmd.connect_cmd.sqsize = 31;

	/* Non-existent controller */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_subsystem_get_ctrlr, NULL);
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);

	/* I/O connect to discovery controller with keep-alive-timeout != 0 */
	cmd.connect_cmd.qid = 0;
	cmd.connect_cmd.kato = 120000;
	memset(&rsp, 0, sizeof(rsp));
	subsystem.subtype = SPDK_NVMF_SUBTYPE_DISCOVERY;
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
	CU_ASSERT(qpair.ctrlr->keep_alive_poller != NULL);
	spdk_nvmf_ctrlr_stop_keep_alive_timer(qpair.ctrlr);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;

	/* I/O connect to discovery controller with keep-alive-timeout == 0.
	 *  Then, a fixed timeout value is set to keep-alive-timeout.
	 */
	cmd.connect_cmd.kato = 0;
	memset(&rsp, 0, sizeof(rsp));
	subsystem.subtype = SPDK_NVMF_SUBTYPE_DISCOVERY;
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
	CU_ASSERT(qpair.ctrlr->keep_alive_poller != NULL);
	spdk_nvmf_ctrlr_stop_keep_alive_timer(qpair.ctrlr);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;
	cmd.connect_cmd.qid = 1;
	cmd.connect_cmd.kato = 120000;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	/* I/O connect to disabled controller */
	memset(&rsp, 0, sizeof(rsp));
	ctrlr.vcprop.cc.bits.en = 0;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
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
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = spdk_nvmf_ctrlr_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);
	CU_ASSERT(qpair.ctrlr == NULL);

	/* Clean up globals */
	MOCK_CLEAR(spdk_nvmf_tgt_find_subsystem);
	MOCK_CLEAR(spdk_nvmf_poll_group_create);

	spdk_bit_array_free(&ctrlr.qpair_mask);
	free(sgroups);
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
	cmd.nvme_cmd.cdw10_bits.identify.cns = SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST;

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

static void
test_set_get_features(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_qpair admin_qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsystem, .admin_qpair = &admin_qpair };
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_ns ns[3];
	struct spdk_nvmf_ns *ns_arr[3] = {&ns[0], NULL, &ns[2]};;
	struct spdk_nvmf_request req;
	int rc;

	subsystem.ns = ns_arr;
	subsystem.max_nsid = SPDK_COUNTOF(ns_arr);
	admin_qpair.ctrlr = &ctrlr;
	req.qpair = &admin_qpair;
	cmd.nvme_cmd.nsid = 1;
	req.cmd = &cmd;
	req.rsp = &rsp;

	/* Set SPDK_NVME_FEAT_HOST_RESERVE_PERSIST feature */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw11_bits.feat_rsv_persistence.bits.ptpl = 1;
	ns[0].ptpl_file = "testcfg";
	rc = spdk_nvmf_ctrlr_set_features_reservation_persistence(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE);
	CU_ASSERT(ns[0].ptpl_activated == true);

	/* Get SPDK_NVME_FEAT_HOST_RESERVE_PERSIST feature */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.nvme_cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_HOST_RESERVE_PERSIST;
	rc = spdk_nvmf_ctrlr_get_features_reservation_persistence(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(rsp.nvme_cpl.cdw0 == 1);
}

/*
 * Reservation Unit Test Configuration
 *       --------             --------    --------
 *      | Host A |           | Host B |  | Host C |
 *       --------             --------    --------
 *      /        \               |           |
 *  --------   --------       -------     -------
 * |Ctrlr1_A| |Ctrlr2_A|     |Ctrlr_B|   |Ctrlr_C|
 *  --------   --------       -------     -------
 *    \           \              /           /
 *     \           \            /           /
 *      \           \          /           /
 *      --------------------------------------
 *     |            NAMESPACE 1               |
 *      --------------------------------------
 */

static struct spdk_nvmf_ctrlr g_ctrlr1_A, g_ctrlr2_A, g_ctrlr_B, g_ctrlr_C;
struct spdk_nvmf_subsystem_pg_ns_info g_ns_info;

static void
ut_reservation_init(enum spdk_nvme_reservation_type rtype)
{
	/* Host A has two controllers */
	spdk_uuid_generate(&g_ctrlr1_A.hostid);
	spdk_uuid_copy(&g_ctrlr2_A.hostid, &g_ctrlr1_A.hostid);

	/* Host B has 1 controller */
	spdk_uuid_generate(&g_ctrlr_B.hostid);

	/* Host C has 1 controller */
	spdk_uuid_generate(&g_ctrlr_C.hostid);

	memset(&g_ns_info, 0, sizeof(g_ns_info));
	g_ns_info.rtype = rtype;
	g_ns_info.reg_hostid[0] = g_ctrlr1_A.hostid;
	g_ns_info.reg_hostid[1] = g_ctrlr_B.hostid;
	g_ns_info.reg_hostid[2] = g_ctrlr_C.hostid;
}

static void
test_reservation_write_exclusive(void)
{
	struct spdk_nvmf_request req = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	int rc;

	req.cmd = &cmd;
	req.rsp = &rsp;

	/* Host A holds reservation with type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE */
	ut_reservation_init(SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);
	g_ns_info.holder_id = g_ctrlr1_A.hostid;

	/* Test Case: Issue a Read command from Host A and Host B */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_READ;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr1_A, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Test Case: Issue a DSM Write command from Host A and Host B */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr1_A, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	/* Test Case: Issue a Write command from Host C */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_WRITE;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_C, &req);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	/* Test Case: Issue a Read command from Host B */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_READ;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Unregister Host C */
	memset(&g_ns_info.reg_hostid[2], 0, sizeof(struct spdk_uuid));

	/* Test Case: Read and Write commands from non-registrant Host C */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_WRITE;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_C, &req);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_READ;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_C, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
}

static void
test_reservation_exclusive_access(void)
{
	struct spdk_nvmf_request req = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	int rc;

	req.cmd = &cmd;
	req.rsp = &rsp;

	/* Host A holds reservation with type SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS */
	ut_reservation_init(SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS);
	g_ns_info.holder_id = g_ctrlr1_A.hostid;

	/* Test Case: Issue a Read command from Host B */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_READ;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	/* Test Case: Issue a Reservation Release command from a valid Registrant */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_RESERVATION_RELEASE;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
}

static void
_test_reservation_write_exclusive_regs_only_and_all_regs(enum spdk_nvme_reservation_type rtype)
{
	struct spdk_nvmf_request req = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	int rc;

	req.cmd = &cmd;
	req.rsp = &rsp;

	/* SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY and SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS */
	ut_reservation_init(rtype);
	g_ns_info.holder_id = g_ctrlr1_A.hostid;

	/* Test Case: Issue a Read command from Host A and Host C */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_READ;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr1_A, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_C, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Test Case: Issue a DSM Write command from Host A and Host C */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr1_A, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_C, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Unregister Host C */
	memset(&g_ns_info.reg_hostid[2], 0, sizeof(struct spdk_uuid));

	/* Test Case: Read and Write commands from non-registrant Host C */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_READ;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_C, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_WRITE;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_C, &req);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);
}

static void
test_reservation_write_exclusive_regs_only_and_all_regs(void)
{
	_test_reservation_write_exclusive_regs_only_and_all_regs(
		SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);
	_test_reservation_write_exclusive_regs_only_and_all_regs(
		SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
}

static void
_test_reservation_exclusive_access_regs_only_and_all_regs(enum spdk_nvme_reservation_type rtype)
{
	struct spdk_nvmf_request req = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	int rc;

	req.cmd = &cmd;
	req.rsp = &rsp;

	/* SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY and SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS */
	ut_reservation_init(rtype);
	g_ns_info.holder_id = g_ctrlr1_A.hostid;

	/* Test Case: Issue a Write command from Host B */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_WRITE;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Unregister Host B */
	memset(&g_ns_info.reg_hostid[1], 0, sizeof(struct spdk_uuid));

	/* Test Case: Issue a Read command from Host B */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_READ;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_WRITE;
	rc = nvmf_ns_reservation_request_check(&g_ns_info, &g_ctrlr_B, &req);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);
}

static void
test_reservation_exclusive_access_regs_only_and_all_regs(void)
{
	_test_reservation_exclusive_access_regs_only_and_all_regs(
		SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY);
	_test_reservation_exclusive_access_regs_only_and_all_regs(
		SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS);
}

static void
test_reservation_notification_log_page(void)
{
	struct spdk_nvmf_ctrlr ctrlr;
	struct spdk_nvmf_qpair qpair;
	struct spdk_nvmf_ns ns;
	struct spdk_nvmf_request req;
	union nvmf_h2c_msg cmd;
	union nvmf_c2h_msg rsp = {{0}};
	union spdk_nvme_async_event_completion event = {0};
	struct spdk_nvme_reservation_notification_log logs[3];

	memset(&ctrlr, 0, sizeof(ctrlr));
	ctrlr.thread = spdk_get_thread();
	TAILQ_INIT(&ctrlr.log_head);
	ns.nsid = 1;

	/* Test Case: Mask all the reservation notifications */
	ns.mask = SPDK_NVME_REGISTRATION_PREEMPTED_MASK |
		  SPDK_NVME_RESERVATION_RELEASED_MASK |
		  SPDK_NVME_RESERVATION_PREEMPTED_MASK;
	spdk_nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					       SPDK_NVME_REGISTRATION_PREEMPTED);
	spdk_nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					       SPDK_NVME_RESERVATION_RELEASED);
	spdk_nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					       SPDK_NVME_RESERVATION_PREEMPTED);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&ctrlr.log_head));

	/* Test Case: Unmask all the reservation notifications,
	 * 3 log pages are generated, and AER was triggered.
	 */
	ns.mask = 0;
	ctrlr.num_avail_log_pages = 0;
	req.cmd = &cmd;
	req.rsp = &rsp;
	ctrlr.aer_req = &req;
	req.qpair = &qpair;
	TAILQ_INIT(&qpair.outstanding);
	qpair.ctrlr = NULL;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);

	spdk_nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					       SPDK_NVME_REGISTRATION_PREEMPTED);
	spdk_nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					       SPDK_NVME_RESERVATION_RELEASED);
	spdk_nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					       SPDK_NVME_RESERVATION_PREEMPTED);
	poll_threads();
	event.raw = rsp.nvme_cpl.cdw0;
	SPDK_CU_ASSERT_FATAL(event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_IO);
	SPDK_CU_ASSERT_FATAL(event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL);
	SPDK_CU_ASSERT_FATAL(event.bits.log_page_identifier == SPDK_NVME_LOG_RESERVATION_NOTIFICATION);
	SPDK_CU_ASSERT_FATAL(ctrlr.num_avail_log_pages == 3);

	/* Test Case: Get Log Page to clear the log pages */
	spdk_nvmf_get_reservation_notification_log_page(&ctrlr, (void *)logs, 0, sizeof(logs));
	SPDK_CU_ASSERT_FATAL(ctrlr.num_avail_log_pages == 0);
}

static void
test_get_dif_ctx(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ns *_ns = NULL;
	struct spdk_bdev bdev = {};
	union nvmf_h2c_msg cmd = {};
	struct spdk_dif_ctx dif_ctx = {};
	bool ret;

	ctrlr.subsys = &subsystem;

	qpair.ctrlr = &ctrlr;

	req.qpair = &qpair;
	req.cmd = &cmd;

	ns.bdev = &bdev;

	ctrlr.dif_insert_or_strip = false;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == false);

	ctrlr.dif_insert_or_strip = true;
	qpair.state = SPDK_NVMF_QPAIR_UNINITIALIZED;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == false);

	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	cmd.nvmf_cmd.opcode = SPDK_NVME_OPC_FABRIC;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == false);

	cmd.nvmf_cmd.opcode = SPDK_NVME_OPC_FLUSH;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == false);

	qpair.qid = 1;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == false);

	cmd.nvme_cmd.nsid = 1;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == false);

	subsystem.max_nsid = 1;
	subsystem.ns = &_ns;
	subsystem.ns[0] = &ns;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == false);

	cmd.nvmf_cmd.opcode = SPDK_NVME_OPC_WRITE;

	ret = spdk_nvmf_request_get_dif_ctx(&req, &dif_ctx);
	CU_ASSERT(ret == true);
}

static void
test_identify_ctrlr(void)
{
	struct spdk_nvmf_subsystem subsystem = {
		.subtype = SPDK_NVMF_SUBTYPE_NVME
	};
	struct spdk_nvmf_transport_ops tops = {};
	struct spdk_nvmf_transport transport = {
		.ops = &tops,
		.opts = {
			.in_capsule_data_size = 4096,
		},
	};
	struct spdk_nvmf_qpair admin_qpair = { .transport = &transport};
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsystem, .admin_qpair = &admin_qpair };
	struct spdk_nvme_ctrlr_data cdata = {};
	uint32_t expected_ioccsz;

	/* Check ioccsz, TCP transport */
	tops.type = SPDK_NVME_TRANSPORT_TCP;
	expected_ioccsz = sizeof(struct spdk_nvme_cmd) / 16 + transport.opts.in_capsule_data_size / 16;
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ctrlr(&ctrlr, &cdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(cdata.nvmf_specific.ioccsz == expected_ioccsz);

	/* Check ioccsz, RDMA transport */
	tops.type = SPDK_NVME_TRANSPORT_RDMA;
	expected_ioccsz = sizeof(struct spdk_nvme_cmd) / 16 + transport.opts.in_capsule_data_size / 16;
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ctrlr(&ctrlr, &cdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(cdata.nvmf_specific.ioccsz == expected_ioccsz);

	/* Check ioccsz, TCP transport with dif_insert_or_strip */
	tops.type = SPDK_NVME_TRANSPORT_TCP;
	ctrlr.dif_insert_or_strip = true;
	expected_ioccsz = sizeof(struct spdk_nvme_cmd) / 16 + transport.opts.in_capsule_data_size / 16;
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ctrlr(&ctrlr, &cdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(cdata.nvmf_specific.ioccsz == expected_ioccsz);

	/* Check ioccsz, RDMA transport with dif_insert_or_strip */
	tops.type = SPDK_NVME_TRANSPORT_RDMA;
	ctrlr.dif_insert_or_strip = true;
	expected_ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	CU_ASSERT(spdk_nvmf_ctrlr_identify_ctrlr(&ctrlr, &cdata) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(cdata.nvmf_specific.ioccsz == expected_ioccsz);
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

	if (CU_add_test(suite, "get_log_page", test_get_log_page) == NULL ||
	    CU_add_test(suite, "process_fabrics_cmd", test_process_fabrics_cmd) == NULL ||
	    CU_add_test(suite, "connect", test_connect) == NULL ||
	    CU_add_test(suite, "get_ns_id_desc_list", test_get_ns_id_desc_list) == NULL ||
	    CU_add_test(suite, "identify_ns", test_identify_ns) == NULL ||
	    CU_add_test(suite, "reservation_write_exclusive", test_reservation_write_exclusive) == NULL ||
	    CU_add_test(suite, "reservation_exclusive_access", test_reservation_exclusive_access) == NULL ||
	    CU_add_test(suite, "reservation_write_exclusive_regs_only_and_all_regs",
			test_reservation_write_exclusive_regs_only_and_all_regs) == NULL ||
	    CU_add_test(suite, "reservation_exclusive_access_regs_only_and_all_regs",
			test_reservation_exclusive_access_regs_only_and_all_regs) == NULL ||
	    CU_add_test(suite, "reservation_notification_log_page",
			test_reservation_notification_log_page) == NULL ||
	    CU_add_test(suite, "get_dif_ctx", test_get_dif_ctx) == NULL ||
	    CU_add_test(suite, "set_get_features", test_set_get_features) == NULL ||
	    CU_add_test(suite, "identify_ctrlr", test_identify_ctrlr) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
