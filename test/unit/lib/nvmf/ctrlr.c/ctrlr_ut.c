/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#include "thread/thread_internal.h"

#include "common/lib/ut_multithread.c"
#include "nvmf/ctrlr.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

struct spdk_bdev {
	int ut_mock;
	uint64_t blockcnt;
	uint32_t blocklen;
};

const char subsystem_default_sn[SPDK_NVME_CTRLR_SN_LEN + 1] = "subsys_default_sn";
const char subsystem_default_mn[SPDK_NVME_CTRLR_MN_LEN + 1] = "subsys_default_mn";

static struct spdk_bdev_io *zcopy_start_bdev_io_read = (struct spdk_bdev_io *) 0x1122334455667788UL;
static struct spdk_bdev_io *zcopy_start_bdev_io_write = (struct spdk_bdev_io *)
		0x8877665544332211UL;
static struct spdk_bdev_io *zcopy_start_bdev_io_fail = (struct spdk_bdev_io *) 0xFFFFFFFFFFFFFFFFUL;

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

DEFINE_STUB(spdk_nvmf_subsystem_host_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, const char *hostnqn),
	    true);

DEFINE_STUB(nvmf_subsystem_add_ctrlr,
	    int,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr),
	    0);

DEFINE_STUB(nvmf_subsystem_get_ctrlr,
	    struct spdk_nvmf_ctrlr *,
	    (struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid),
	    NULL);

DEFINE_STUB(nvmf_ctrlr_dsm_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB(nvmf_ctrlr_write_zeroes_supported,
	    bool,
	    (struct spdk_nvmf_ctrlr *ctrlr),
	    false);

DEFINE_STUB_V(nvmf_get_discovery_log_page,
	      (struct spdk_nvmf_tgt *tgt, const char *hostnqn, struct iovec *iov,
	       uint32_t iovcnt, uint64_t offset, uint32_t length, struct spdk_nvme_transport_id *cmd_src_trid));

DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid,
	    int,
	    (struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid),
	    0);

DEFINE_STUB(spdk_nvmf_subsystem_listener_allowed,
	    bool,
	    (struct spdk_nvmf_subsystem *subsystem, const struct spdk_nvme_transport_id *trid),
	    true);

DEFINE_STUB(nvmf_subsystem_find_listener,
	    struct spdk_nvmf_subsystem_listener *,
	    (struct spdk_nvmf_subsystem *subsystem,
	     const struct spdk_nvme_transport_id *trid),
	    (void *)0x1);

DEFINE_STUB(nvmf_bdev_ctrlr_read_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_write_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_compare_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_compare_and_write_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_write_zeroes_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_flush_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_dsm_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_bdev_ctrlr_nvme_passthru_io,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(nvmf_transport_req_complete,
	    int,
	    (struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB_V(nvmf_ns_reservation_request, (void *ctx));

DEFINE_STUB(nvmf_bdev_ctrlr_get_dif_ctx, bool,
	    (struct spdk_bdev *bdev, struct spdk_nvme_cmd *cmd,
	     struct spdk_dif_ctx *dif_ctx),
	    true);

DEFINE_STUB_V(nvmf_transport_qpair_abort_request,
	      (struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_request *req));

DEFINE_STUB_V(spdk_nvme_print_command, (uint16_t qid, struct spdk_nvme_cmd *cmd));
DEFINE_STUB_V(spdk_nvme_print_completion, (uint16_t qid, struct spdk_nvme_cpl *cpl));

DEFINE_STUB_V(nvmf_subsystem_remove_ctrlr, (struct spdk_nvmf_subsystem *subsystem,
		struct spdk_nvmf_ctrlr *ctrlr));

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_abort_cmd,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req, struct spdk_nvmf_request *req_to_abort),
	    0);

DEFINE_STUB(nvmf_transport_req_free,
	    int,
	    (struct spdk_nvmf_request *req),
	    0);

DEFINE_STUB(spdk_nvmf_bdev_ctrlr_nvme_passthru_admin,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     struct spdk_nvmf_request *req, spdk_nvmf_nvme_passthru_cmd_cb cb_fn),
	    0);
DEFINE_STUB(spdk_bdev_reset, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				   spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair, nvmf_qpair_disconnect_cb cb_fn, void *ctx)
{
	return 0;
}

void
nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
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

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem)
{
	SPDK_CU_ASSERT_FATAL(subsystem->ns != NULL);
	return subsystem->ns[0];
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_ns *prev_ns)
{
	uint32_t nsid;

	SPDK_CU_ASSERT_FATAL(subsystem->ns != NULL);
	nsid = prev_ns->nsid;

	if (nsid >= subsystem->max_nsid) {
		return NULL;
	}
	for (nsid = nsid + 1; nsid <= subsystem->max_nsid; nsid++) {
		if (subsystem->ns[nsid - 1]) {
			return subsystem->ns[nsid - 1];
		}
	}
	return NULL;
}

bool
nvmf_bdev_zcopy_enabled(struct spdk_bdev *bdev)
{
	return true;
}

int
nvmf_bdev_ctrlr_zcopy_start(struct spdk_bdev *bdev,
			    struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;

	start_lba = from_le64(&req->cmd->nvme_cmd.cdw10);
	num_blocks = (from_le32(&req->cmd->nvme_cmd.cdw12) & 0xFFFFu) + 1;

	if ((start_lba + num_blocks) > bdev->blockcnt) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_WRITE) {
		req->zcopy_bdev_io = zcopy_start_bdev_io_write;
	} else if (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_READ) {
		req->zcopy_bdev_io = zcopy_start_bdev_io_read;
	} else {
		req->zcopy_bdev_io = zcopy_start_bdev_io_fail;
	}


	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

void
nvmf_bdev_ctrlr_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	req->zcopy_bdev_io = NULL;
	spdk_nvmf_request_complete(req);
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
	cmd.nvme_cmd.cdw10_bits.get_log_page.numdl = spdk_nvme_bytes_to_numd(req.length);
	CU_ASSERT(nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* Get Log Page with invalid log ID */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10 = 0;
	CU_ASSERT(nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Get Log Page with invalid offset (not dword aligned) */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_ERROR;
	cmd.nvme_cmd.cdw10_bits.get_log_page.numdl = spdk_nvme_bytes_to_numd(req.length);
	cmd.nvme_cmd.cdw12 = 2;
	CU_ASSERT(nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Get Log Page without data buffer */
	memset(&cmd, 0, sizeof(cmd));
	memset(&rsp, 0, sizeof(rsp));
	req.data = NULL;
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nvme_cmd.cdw10_bits.get_log_page.lid = SPDK_NVME_LOG_ERROR;
	cmd.nvme_cmd.cdw10_bits.get_log_page.numdl = spdk_nvme_bytes_to_numd(req.length);
	CU_ASSERT(nvmf_ctrlr_get_log_page(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
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
	ret = nvmf_ctrlr_process_fabrics_cmd(&req);
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
	struct spdk_nvmf_transport_ops tops = {};
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
	admin_qpair.state = SPDK_NVMF_QPAIR_ACTIVE;

	memset(&tgt, 0, sizeof(tgt));
	memset(&transport, 0, sizeof(transport));
	transport.ops = &tops;
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
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	nvmf_ctrlr_stop_keep_alive_timer(qpair.ctrlr);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;

	/* Valid admin connect command with kato = 0 */
	cmd.connect_cmd.kato = 0;
	memset(&rsp, 0, sizeof(rsp));
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL && qpair.ctrlr->keep_alive_poller == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;
	cmd.connect_cmd.kato = 120000;

	/* Invalid data length */
	memset(&rsp, 0, sizeof(rsp));
	req.length = sizeof(connect_data) - 1;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT);
	CU_ASSERT(qpair.ctrlr == NULL);
	cmd.connect_cmd.recfmt = 0;

	/* Subsystem not found */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(spdk_nvmf_tgt_find_subsystem, NULL);
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	rc = nvmf_ctrlr_cmd_connect(&req);
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
	MOCK_SET(nvmf_subsystem_get_ctrlr, &ctrlr);
	cmd.connect_cmd.qid = 1;
	cmd.connect_cmd.sqsize = 63;
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr == &ctrlr);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	qpair.ctrlr = NULL;
	cmd.connect_cmd.sqsize = 31;

	/* Non-existent controller */
	memset(&rsp, 0, sizeof(rsp));
	MOCK_SET(nvmf_subsystem_get_ctrlr, NULL);
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 1);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 16);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	MOCK_SET(nvmf_subsystem_get_ctrlr, &ctrlr);

	/* I/O connect to discovery controller */
	memset(&rsp, 0, sizeof(rsp));
	subsystem.subtype = SPDK_NVMF_SUBTYPE_DISCOVERY;
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);

	/* I/O connect to discovery controller with keep-alive-timeout != 0 */
	cmd.connect_cmd.qid = 0;
	cmd.connect_cmd.kato = 120000;
	memset(&rsp, 0, sizeof(rsp));
	subsystem.subtype = SPDK_NVMF_SUBTYPE_DISCOVERY;
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
	CU_ASSERT(qpair.ctrlr->keep_alive_poller != NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	nvmf_ctrlr_stop_keep_alive_timer(qpair.ctrlr);
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
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.ctrlr != NULL);
	CU_ASSERT(qpair.ctrlr->keep_alive_poller != NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	nvmf_ctrlr_stop_keep_alive_timer(qpair.ctrlr);
	spdk_bit_array_free(&qpair.ctrlr->qpair_mask);
	free(qpair.ctrlr);
	qpair.ctrlr = NULL;
	cmd.connect_cmd.qid = 1;
	cmd.connect_cmd.kato = 120000;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	/* I/O connect to disabled controller */
	memset(&rsp, 0, sizeof(rsp));
	ctrlr.vcprop.cc.bits.en = 0;
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	ctrlr.vcprop.cc.bits.en = 1;

	/* I/O connect with invalid IOSQES */
	memset(&rsp, 0, sizeof(rsp));
	ctrlr.vcprop.cc.bits.iosqes = 3;
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	ctrlr.vcprop.cc.bits.iosqes = 6;

	/* I/O connect with invalid IOCQES */
	memset(&rsp, 0, sizeof(rsp));
	ctrlr.vcprop.cc.bits.iocqes = 3;
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.iattr == 0);
	CU_ASSERT(rsp.connect_rsp.status_code_specific.invalid.ipo == 42);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	ctrlr.vcprop.cc.bits.iocqes = 4;

	/* I/O connect with too many existing qpairs */
	memset(&rsp, 0, sizeof(rsp));
	spdk_bit_array_set(ctrlr.qpair_mask, 0);
	spdk_bit_array_set(ctrlr.qpair_mask, 1);
	spdk_bit_array_set(ctrlr.qpair_mask, 2);
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
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
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);

	/* I/O connect when admin qpair is being destroyed */
	admin_qpair.group = NULL;
	admin_qpair.state = SPDK_NVMF_QPAIR_DEACTIVATING;
	memset(&rsp, 0, sizeof(rsp));
	sgroups[subsystem.id].mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	rc = nvmf_ctrlr_cmd_connect(&req);
	poll_threads();
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);
	CU_ASSERT(qpair.ctrlr == NULL);
	CU_ASSERT(sgroups[subsystem.id].mgmt_io_outstanding == 0);
	admin_qpair.group = &group;
	admin_qpair.state = SPDK_NVMF_QPAIR_ACTIVE;

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
	req.iovcnt = 1;
	req.iov[0].iov_base = req.data;
	req.iov[0].iov_len = req.length;

	memset(&cmd, 0, sizeof(cmd));
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.nvme_cmd.cdw10_bits.identify.cns = SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST;

	/* Invalid NSID */
	cmd.nvme_cmd.nsid = 0;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);

	/* Valid NSID, but ns has no IDs defined */
	cmd.nvme_cmd.nsid = 1;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(spdk_mem_all_zero(buf, sizeof(buf)));

	/* Valid NSID, only EUI64 defined */
	ns.opts.eui64[0] = 0x11;
	ns.opts.eui64[7] = 0xFF;
	memset(&rsp, 0, sizeof(rsp));
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
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
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
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
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
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
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
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
	enum spdk_nvme_ana_state ana_state[3];
	struct spdk_nvmf_subsystem_listener listener = { .ana_state = ana_state };
	struct spdk_nvmf_ctrlr ctrlr = {
		.subsys = &subsystem, .admin_qpair = &admin_qpair, .listener = &listener
	};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_ns ns[3];
	struct spdk_nvmf_ns *ns_arr[3] = {&ns[0], NULL, &ns[2]};
	struct spdk_nvmf_request req;
	int rc;

	ns[0].anagrpid = 1;
	ns[2].anagrpid = 3;
	subsystem.ns = ns_arr;
	subsystem.max_nsid = SPDK_COUNTOF(ns_arr);
	listener.ana_state[0] = SPDK_NVME_ANA_OPTIMIZED_STATE;
	listener.ana_state[2] = SPDK_NVME_ANA_OPTIMIZED_STATE;
	admin_qpair.ctrlr = &ctrlr;
	req.qpair = &admin_qpair;
	cmd.nvme_cmd.nsid = 1;
	req.cmd = &cmd;
	req.rsp = &rsp;

	/* Set SPDK_NVME_FEAT_HOST_RESERVE_PERSIST feature */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw11_bits.feat_rsv_persistence.bits.ptpl = 1;
	ns[0].ptpl_file = "testcfg";
	rc = nvmf_ctrlr_set_features_reservation_persistence(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE);
	CU_ASSERT(ns[0].ptpl_activated == true);

	/* Get SPDK_NVME_FEAT_HOST_RESERVE_PERSIST feature */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.nvme_cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_HOST_RESERVE_PERSIST;
	rc = nvmf_ctrlr_get_features_reservation_persistence(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	CU_ASSERT(rsp.nvme_cpl.cdw0 == 1);


	/* Get SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD - valid TMPSEL */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.nvme_cmd.cdw11 = 0x42;
	cmd.nvme_cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	rc = nvmf_ctrlr_get_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);

	/* Get SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD - invalid TMPSEL */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.nvme_cmd.cdw11 = 0x42 | 1 << 16 | 1 << 19; /* Set reserved value */
	cmd.nvme_cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	rc = nvmf_ctrlr_get_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Set SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD - valid TMPSEL */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw11 = 0x42;
	cmd.nvme_cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	rc = nvmf_ctrlr_set_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);

	/* Set SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD - invalid TMPSEL */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw11 = 0x42 | 1 << 16 | 1 << 19; /* Set reserved value */
	cmd.nvme_cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	rc = nvmf_ctrlr_set_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Set SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD - invalid THSEL */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw11 = 0x42;
	cmd.nvme_cmd.cdw11_bits.feat_temp_threshold.bits.thsel = 0x3; /* Set reserved value */
	cmd.nvme_cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD;

	rc = nvmf_ctrlr_set_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);


	/* get SPDK_NVME_FEAT_ERROR_RECOVERY - generic */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw10_bits.get_features.fid = SPDK_NVME_FEAT_ERROR_RECOVERY;

	rc = nvmf_ctrlr_get_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);

	/* Set SPDK_NVME_FEAT_ERROR_RECOVERY - DULBE set */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw11 = 0x42;
	cmd.nvme_cmd.cdw11_bits.feat_error_recovery.bits.dulbe = 0x1;
	cmd.nvme_cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_ERROR_RECOVERY;

	rc = nvmf_ctrlr_set_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_FIELD);

	/* Set SPDK_NVME_FEAT_ERROR_RECOVERY - DULBE cleared */
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.nvme_cmd.cdw11 = 0x42;
	cmd.nvme_cmd.cdw11_bits.feat_error_recovery.bits.dulbe = 0x0;
	cmd.nvme_cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_ERROR_RECOVERY;

	rc = nvmf_ctrlr_set_features(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
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
init_pending_async_events(struct spdk_nvmf_ctrlr *ctrlr)
{
	STAILQ_INIT(&ctrlr->async_events);
}

static void
cleanup_pending_async_events(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_async_event_completion *event, *event_tmp;

	STAILQ_FOREACH_SAFE(event, &ctrlr->async_events, link, event_tmp) {
		STAILQ_REMOVE(&ctrlr->async_events, event, spdk_nvmf_async_event_completion, link);
		free(event);
	}
}

static int
num_pending_async_events(struct spdk_nvmf_ctrlr *ctrlr)
{
	int num = 0;
	struct spdk_nvmf_async_event_completion *event;

	STAILQ_FOREACH(event, &ctrlr->async_events, link) {
		num++;
	}
	return num;
}

static void
test_reservation_notification_log_page(void)
{
	struct spdk_nvmf_ctrlr ctrlr;
	struct spdk_nvmf_qpair qpair;
	struct spdk_nvmf_ns ns;
	struct spdk_nvmf_request req = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	union spdk_nvme_async_event_completion event = {};
	struct spdk_nvme_reservation_notification_log logs[3];
	struct iovec iov;

	memset(&ctrlr, 0, sizeof(ctrlr));
	ctrlr.thread = spdk_get_thread();
	TAILQ_INIT(&ctrlr.log_head);
	init_pending_async_events(&ctrlr);
	ns.nsid = 1;

	/* Test Case: Mask all the reservation notifications */
	ns.mask = SPDK_NVME_REGISTRATION_PREEMPTED_MASK |
		  SPDK_NVME_RESERVATION_RELEASED_MASK |
		  SPDK_NVME_RESERVATION_PREEMPTED_MASK;
	nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					  SPDK_NVME_REGISTRATION_PREEMPTED);
	nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					  SPDK_NVME_RESERVATION_RELEASED);
	nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
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
	ctrlr.aer_req[0] = &req;
	ctrlr.nr_aer_reqs = 1;
	req.qpair = &qpair;
	TAILQ_INIT(&qpair.outstanding);
	qpair.ctrlr = NULL;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);

	nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					  SPDK_NVME_REGISTRATION_PREEMPTED);
	nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					  SPDK_NVME_RESERVATION_RELEASED);
	nvmf_ctrlr_reservation_notice_log(&ctrlr, &ns,
					  SPDK_NVME_RESERVATION_PREEMPTED);
	poll_threads();
	event.raw = rsp.nvme_cpl.cdw0;
	SPDK_CU_ASSERT_FATAL(event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_IO);
	SPDK_CU_ASSERT_FATAL(event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL);
	SPDK_CU_ASSERT_FATAL(event.bits.log_page_identifier == SPDK_NVME_LOG_RESERVATION_NOTIFICATION);
	SPDK_CU_ASSERT_FATAL(ctrlr.num_avail_log_pages == 3);

	/* Test Case: Get Log Page to clear the log pages */
	iov.iov_base = &logs[0];
	iov.iov_len = sizeof(logs);
	nvmf_get_reservation_notification_log_page(&ctrlr, &iov, 1, 0, sizeof(logs), 0);
	SPDK_CU_ASSERT_FATAL(ctrlr.num_avail_log_pages == 0);

	cleanup_pending_async_events(&ctrlr);
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
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {
		.subtype = SPDK_NVMF_SUBTYPE_NVME,
		.tgt = &tgt,
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

	nvmf_ctrlr_cdata_init(&transport, &subsystem, &ctrlr.cdata);

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
}

static int
custom_admin_cmd_hdlr(struct spdk_nvmf_request *req)
{
	req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SUCCESS;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
};

static void
test_custom_admin_cmd(void)
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
	int rc;

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
	cmd.nvme_cmd.opc = 0xc1;
	cmd.nvme_cmd.nsid = 0;
	memset(&rsp, 0, sizeof(rsp));

	spdk_nvmf_set_custom_admin_cmd_hdlr(cmd.nvme_cmd.opc, custom_admin_cmd_hdlr);

	/* Ensure that our hdlr is being called */
	rc = nvmf_ctrlr_process_admin_cmd(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
}

static void
test_fused_compare_and_write(void)
{
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvme_cmd cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ns *subsys_ns[1] = {};
	enum spdk_nvme_ana_state ana_state[1];
	struct spdk_nvmf_subsystem_listener listener = { .ana_state = ana_state };
	struct spdk_bdev bdev = {};

	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	struct spdk_nvmf_subsystem_pg_ns_info ns_info = {};
	struct spdk_io_channel io_ch = {};

	ns.bdev = &bdev;
	ns.anagrpid = 1;

	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsys_ns[0] = &ns;
	subsystem.ns = (struct spdk_nvmf_ns **)&subsys_ns;

	listener.ana_state[0] = SPDK_NVME_ANA_OPTIMIZED_STATE;

	/* Enable controller */
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.subsys = (struct spdk_nvmf_subsystem *)&subsystem;
	ctrlr.listener = &listener;

	group.num_sgroups = 1;
	sgroups.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	sgroups.num_ns = 1;
	ns_info.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info.channel = &io_ch;
	sgroups.ns_info = &ns_info;
	TAILQ_INIT(&sgroups.queued);
	group.sgroups = &sgroups;
	TAILQ_INIT(&qpair.outstanding);

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	qpair.qid = 1;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;

	cmd.nsid = 1;

	req.qpair = &qpair;
	req.cmd = (union nvmf_h2c_msg *)&cmd;
	req.rsp = &rsp;

	/* SUCCESS/SUCCESS */
	cmd.fuse = SPDK_NVME_CMD_FUSE_FIRST;
	cmd.opc = SPDK_NVME_OPC_COMPARE;

	spdk_nvmf_request_exec(&req);
	CU_ASSERT(qpair.first_fused_req != NULL);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));

	cmd.fuse = SPDK_NVME_CMD_FUSE_SECOND;
	cmd.opc = SPDK_NVME_OPC_WRITE;

	spdk_nvmf_request_exec(&req);
	CU_ASSERT(qpair.first_fused_req == NULL);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));

	/* Wrong sequence */
	cmd.fuse = SPDK_NVME_CMD_FUSE_SECOND;
	cmd.opc = SPDK_NVME_OPC_WRITE;

	spdk_nvmf_request_exec(&req);
	CU_ASSERT(!nvme_status_success(&rsp.nvme_cpl.status));
	CU_ASSERT(qpair.first_fused_req == NULL);

	/* Write as FUSE_FIRST (Wrong op code) */
	cmd.fuse = SPDK_NVME_CMD_FUSE_FIRST;
	cmd.opc = SPDK_NVME_OPC_WRITE;

	spdk_nvmf_request_exec(&req);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_OPCODE);
	CU_ASSERT(qpair.first_fused_req == NULL);

	/* Compare as FUSE_SECOND (Wrong op code) */
	cmd.fuse = SPDK_NVME_CMD_FUSE_FIRST;
	cmd.opc = SPDK_NVME_OPC_COMPARE;

	spdk_nvmf_request_exec(&req);
	CU_ASSERT(qpair.first_fused_req != NULL);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));

	cmd.fuse = SPDK_NVME_CMD_FUSE_SECOND;
	cmd.opc = SPDK_NVME_OPC_COMPARE;

	spdk_nvmf_request_exec(&req);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVME_SC_INVALID_OPCODE);
	CU_ASSERT(qpair.first_fused_req == NULL);
}

static void
test_multi_async_event_reqs(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_request req[5] = {};
	struct spdk_nvmf_ns *ns_ptrs[1] = {};
	struct spdk_nvmf_ns ns = {};
	union nvmf_h2c_msg cmd[5] = {};
	union nvmf_c2h_msg rsp[5] = {};

	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};

	int i;

	ns_ptrs[0] = &ns;
	subsystem.ns = ns_ptrs;
	subsystem.max_nsid = 1;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	ns.opts.nsid = 1;
	group.sgroups = &sgroups;

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	TAILQ_INIT(&qpair.outstanding);

	ctrlr.subsys = &subsystem;
	ctrlr.vcprop.cc.bits.en = 1;

	for (i = 0; i < 5; i++) {
		cmd[i].nvme_cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
		cmd[i].nvme_cmd.nsid = 1;
		cmd[i].nvme_cmd.cid = i;

		req[i].qpair = &qpair;
		req[i].cmd = &cmd[i];
		req[i].rsp = &rsp[i];
		TAILQ_INSERT_TAIL(&qpair.outstanding, &req[i], link);
	}

	/* Target can store NVMF_MAX_ASYNC_EVENTS reqs */
	sgroups.mgmt_io_outstanding = NVMF_MAX_ASYNC_EVENTS;
	for (i = 0; i < NVMF_MAX_ASYNC_EVENTS; i++) {
		CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req[i]) == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
		CU_ASSERT(ctrlr.nr_aer_reqs == i + 1);
	}
	CU_ASSERT(sgroups.mgmt_io_outstanding == 0);

	/* Exceeding the NVMF_MAX_ASYNC_EVENTS reports error */
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req[4]) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(ctrlr.nr_aer_reqs == NVMF_MAX_ASYNC_EVENTS);
	CU_ASSERT(rsp[4].nvme_cpl.status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp[4].nvme_cpl.status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED);

	/* Test if the aer_reqs keep continuous when abort a req in the middle */
	CU_ASSERT(nvmf_qpair_abort_aer(&qpair, 2) == true);
	CU_ASSERT(ctrlr.aer_req[0] == &req[0]);
	CU_ASSERT(ctrlr.aer_req[1] == &req[1]);
	CU_ASSERT(ctrlr.aer_req[2] == &req[3]);

	CU_ASSERT(nvmf_qpair_abort_aer(&qpair, 3) == true);
	CU_ASSERT(ctrlr.aer_req[0] == &req[0]);
	CU_ASSERT(ctrlr.aer_req[1] == &req[1]);
	CU_ASSERT(ctrlr.aer_req[2] == NULL);
	CU_ASSERT(ctrlr.nr_aer_reqs == 2);

	TAILQ_REMOVE(&qpair.outstanding, &req[0], link);
	TAILQ_REMOVE(&qpair.outstanding, &req[1], link);
}

static void
test_get_ana_log_page_one_ns_per_anagrp(void)
{
#define UT_ANA_DESC_SIZE (sizeof(struct spdk_nvme_ana_group_descriptor) + sizeof(uint32_t))
#define UT_ANA_LOG_PAGE_SIZE (sizeof(struct spdk_nvme_ana_page) + 3 * UT_ANA_DESC_SIZE)
	uint32_t ana_group[3];
	struct spdk_nvmf_subsystem subsystem = { .ana_group = ana_group };
	struct spdk_nvmf_ctrlr ctrlr = {};
	enum spdk_nvme_ana_state ana_state[3];
	struct spdk_nvmf_subsystem_listener listener = { .ana_state = ana_state };
	struct spdk_nvmf_ns ns[3];
	struct spdk_nvmf_ns *ns_arr[3] = {&ns[0], &ns[1], &ns[2]};
	uint64_t offset;
	uint32_t length;
	int i;
	char expected_page[UT_ANA_LOG_PAGE_SIZE] = {0};
	char actual_page[UT_ANA_LOG_PAGE_SIZE] = {0};
	struct iovec iov, iovs[2];
	struct spdk_nvme_ana_page *ana_hdr;
	char _ana_desc[UT_ANA_DESC_SIZE];
	struct spdk_nvme_ana_group_descriptor *ana_desc;

	subsystem.ns = ns_arr;
	subsystem.max_nsid = 3;
	for (i = 0; i < 3; i++) {
		subsystem.ana_group[i] = 1;
	}
	ctrlr.subsys = &subsystem;
	ctrlr.listener = &listener;

	for (i = 0; i < 3; i++) {
		listener.ana_state[i] = SPDK_NVME_ANA_OPTIMIZED_STATE;
	}

	for (i = 0; i < 3; i++) {
		ns_arr[i]->nsid = i + 1;
		ns_arr[i]->anagrpid = i + 1;
	}

	/* create expected page */
	ana_hdr = (void *)&expected_page[0];
	ana_hdr->num_ana_group_desc = 3;
	ana_hdr->change_count = 0;

	/* descriptor may be unaligned. So create data and then copy it to the location. */
	ana_desc = (void *)_ana_desc;
	offset = sizeof(struct spdk_nvme_ana_page);

	for (i = 0; i < 3; i++) {
		memset(ana_desc, 0, UT_ANA_DESC_SIZE);
		ana_desc->ana_group_id = ns_arr[i]->nsid;
		ana_desc->num_of_nsid = 1;
		ana_desc->change_count = 0;
		ana_desc->ana_state = ctrlr.listener->ana_state[i];
		ana_desc->nsid[0] = ns_arr[i]->nsid;
		memcpy(&expected_page[offset], ana_desc, UT_ANA_DESC_SIZE);
		offset += UT_ANA_DESC_SIZE;
	}

	/* read entire actual log page */
	offset = 0;
	while (offset < UT_ANA_LOG_PAGE_SIZE) {
		length = spdk_min(16, UT_ANA_LOG_PAGE_SIZE - offset);
		iov.iov_base = &actual_page[offset];
		iov.iov_len = length;
		nvmf_get_ana_log_page(&ctrlr, &iov, 1, offset, length, 0);
		offset += length;
	}

	/* compare expected page and actual page */
	CU_ASSERT(memcmp(expected_page, actual_page, UT_ANA_LOG_PAGE_SIZE) == 0);

	memset(&actual_page[0], 0, UT_ANA_LOG_PAGE_SIZE);
	offset = 0;
	iovs[0].iov_base = &actual_page[offset];
	iovs[0].iov_len = UT_ANA_LOG_PAGE_SIZE - UT_ANA_DESC_SIZE + 4;
	offset += UT_ANA_LOG_PAGE_SIZE - UT_ANA_DESC_SIZE + 4;
	iovs[1].iov_base = &actual_page[offset];
	iovs[1].iov_len = UT_ANA_LOG_PAGE_SIZE - offset;
	nvmf_get_ana_log_page(&ctrlr, &iovs[0], 2, 0, UT_ANA_LOG_PAGE_SIZE, 0);

	CU_ASSERT(memcmp(expected_page, actual_page, UT_ANA_LOG_PAGE_SIZE) == 0);

#undef UT_ANA_DESC_SIZE
#undef UT_ANA_LOG_PAGE_SIZE
}

static void
test_get_ana_log_page_multi_ns_per_anagrp(void)
{
#define UT_ANA_LOG_PAGE_SIZE	(sizeof(struct spdk_nvme_ana_page) +	\
				 sizeof(struct spdk_nvme_ana_group_descriptor) * 2 +	\
				 sizeof(uint32_t) * 5)
	struct spdk_nvmf_ns ns[5];
	struct spdk_nvmf_ns *ns_arr[5] = {&ns[0], &ns[1], &ns[2], &ns[3], &ns[4]};
	uint32_t ana_group[5] = {0};
	struct spdk_nvmf_subsystem subsystem = { .ns = ns_arr, .ana_group = ana_group, };
	enum spdk_nvme_ana_state ana_state[5];
	struct spdk_nvmf_subsystem_listener listener = { .ana_state = ana_state, };
	struct spdk_nvmf_ctrlr ctrlr = { .subsys = &subsystem, .listener = &listener, };
	char expected_page[UT_ANA_LOG_PAGE_SIZE] = {0};
	char actual_page[UT_ANA_LOG_PAGE_SIZE] = {0};
	struct iovec iov, iovs[2];
	struct spdk_nvme_ana_page *ana_hdr;
	char _ana_desc[UT_ANA_LOG_PAGE_SIZE];
	struct spdk_nvme_ana_group_descriptor *ana_desc;
	uint64_t offset;
	uint32_t length;
	int i;

	subsystem.max_nsid = 5;
	subsystem.ana_group[1] = 3;
	subsystem.ana_group[2] = 2;
	for (i = 0; i < 5; i++) {
		listener.ana_state[i] = SPDK_NVME_ANA_OPTIMIZED_STATE;
	}

	for (i = 0; i < 5; i++) {
		ns_arr[i]->nsid = i + 1;
	}
	ns_arr[0]->anagrpid = 2;
	ns_arr[1]->anagrpid = 3;
	ns_arr[2]->anagrpid = 2;
	ns_arr[3]->anagrpid = 3;
	ns_arr[4]->anagrpid = 2;

	/* create expected page */
	ana_hdr = (void *)&expected_page[0];
	ana_hdr->num_ana_group_desc = 2;
	ana_hdr->change_count = 0;

	/* descriptor may be unaligned. So create data and then copy it to the location. */
	ana_desc = (void *)_ana_desc;
	offset = sizeof(struct spdk_nvme_ana_page);

	memset(_ana_desc, 0, sizeof(_ana_desc));
	ana_desc->ana_group_id = 2;
	ana_desc->num_of_nsid = 3;
	ana_desc->change_count = 0;
	ana_desc->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	ana_desc->nsid[0] = 1;
	ana_desc->nsid[1] = 3;
	ana_desc->nsid[2] = 5;
	memcpy(&expected_page[offset], ana_desc, sizeof(struct spdk_nvme_ana_group_descriptor) +
	       sizeof(uint32_t) * 3);
	offset += sizeof(struct spdk_nvme_ana_group_descriptor) + sizeof(uint32_t) * 3;

	memset(_ana_desc, 0, sizeof(_ana_desc));
	ana_desc->ana_group_id = 3;
	ana_desc->num_of_nsid = 2;
	ana_desc->change_count = 0;
	ana_desc->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	ana_desc->nsid[0] = 2;
	ana_desc->nsid[1] = 4;
	memcpy(&expected_page[offset], ana_desc, sizeof(struct spdk_nvme_ana_group_descriptor) +
	       sizeof(uint32_t) * 2);

	/* read entire actual log page, and compare expected page and actual page. */
	offset = 0;
	while (offset < UT_ANA_LOG_PAGE_SIZE) {
		length = spdk_min(16, UT_ANA_LOG_PAGE_SIZE - offset);
		iov.iov_base = &actual_page[offset];
		iov.iov_len = length;
		nvmf_get_ana_log_page(&ctrlr, &iov, 1, offset, length, 0);
		offset += length;
	}

	CU_ASSERT(memcmp(expected_page, actual_page, UT_ANA_LOG_PAGE_SIZE) == 0);

	memset(&actual_page[0], 0, UT_ANA_LOG_PAGE_SIZE);
	offset = 0;
	iovs[0].iov_base = &actual_page[offset];
	iovs[0].iov_len = UT_ANA_LOG_PAGE_SIZE - sizeof(uint32_t) * 5;
	offset += UT_ANA_LOG_PAGE_SIZE - sizeof(uint32_t) * 5;
	iovs[1].iov_base = &actual_page[offset];
	iovs[1].iov_len = sizeof(uint32_t) * 5;
	nvmf_get_ana_log_page(&ctrlr, &iovs[0], 2, 0, UT_ANA_LOG_PAGE_SIZE, 0);

	CU_ASSERT(memcmp(expected_page, actual_page, UT_ANA_LOG_PAGE_SIZE) == 0);

#undef UT_ANA_LOG_PAGE_SIZE
}
static void
test_multi_async_events(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_request req[4] = {};
	struct spdk_nvmf_ns *ns_ptrs[1] = {};
	struct spdk_nvmf_ns ns = {};
	union nvmf_h2c_msg cmd[4] = {};
	union nvmf_c2h_msg rsp[4] = {};
	union spdk_nvme_async_event_completion event = {};
	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	int i;

	ns_ptrs[0] = &ns;
	subsystem.ns = ns_ptrs;
	subsystem.max_nsid = 1;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	ns.opts.nsid = 1;
	group.sgroups = &sgroups;

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	TAILQ_INIT(&qpair.outstanding);

	ctrlr.subsys = &subsystem;
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.feat.async_event_configuration.bits.ns_attr_notice = 1;
	ctrlr.feat.async_event_configuration.bits.ana_change_notice = 1;
	ctrlr.feat.async_event_configuration.bits.discovery_log_change_notice = 1;
	init_pending_async_events(&ctrlr);

	/* Target queue pending events when there is no outstanding AER request */
	nvmf_ctrlr_async_event_ns_notice(&ctrlr);
	nvmf_ctrlr_async_event_ana_change_notice(&ctrlr);
	nvmf_ctrlr_async_event_discovery_log_change_notice(&ctrlr);

	for (i = 0; i < 4; i++) {
		cmd[i].nvme_cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
		cmd[i].nvme_cmd.nsid = 1;
		cmd[i].nvme_cmd.cid = i;

		req[i].qpair = &qpair;
		req[i].cmd = &cmd[i];
		req[i].rsp = &rsp[i];

		TAILQ_INSERT_TAIL(&qpair.outstanding, &req[i], link);

		sgroups.mgmt_io_outstanding = 1;
		if (i < 3) {
			CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req[i]) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
			CU_ASSERT(sgroups.mgmt_io_outstanding == 0);
			CU_ASSERT(ctrlr.nr_aer_reqs == 0);
		} else {
			CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req[i]) == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS);
			CU_ASSERT(sgroups.mgmt_io_outstanding == 0);
			CU_ASSERT(ctrlr.nr_aer_reqs == 1);
		}
	}

	event.raw = rsp[0].nvme_cpl.cdw0;
	CU_ASSERT(event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED);
	event.raw = rsp[1].nvme_cpl.cdw0;
	CU_ASSERT(event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_ANA_CHANGE);
	event.raw = rsp[2].nvme_cpl.cdw0;
	CU_ASSERT(event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE);

	cleanup_pending_async_events(&ctrlr);
}

static void
test_rae(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_request req[3] = {};
	struct spdk_nvmf_ns *ns_ptrs[1] = {};
	struct spdk_nvmf_ns ns = {};
	union nvmf_h2c_msg cmd[3] = {};
	union nvmf_c2h_msg rsp[3] = {};
	union spdk_nvme_async_event_completion event = {};
	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	int i;
	char data[4096];

	ns_ptrs[0] = &ns;
	subsystem.ns = ns_ptrs;
	subsystem.max_nsid = 1;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;

	ns.opts.nsid = 1;
	group.sgroups = &sgroups;

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	TAILQ_INIT(&qpair.outstanding);

	ctrlr.subsys = &subsystem;
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.feat.async_event_configuration.bits.ns_attr_notice = 1;
	init_pending_async_events(&ctrlr);

	/* Target queue pending events when there is no outstanding AER request */
	nvmf_ctrlr_async_event_ns_notice(&ctrlr);
	nvmf_ctrlr_async_event_ns_notice(&ctrlr);
	nvmf_ctrlr_async_event_ns_notice(&ctrlr);
	/* only one event will be queued before RAE is clear */
	CU_ASSERT(num_pending_async_events(&ctrlr) == 1);

	req[0].qpair = &qpair;
	req[0].cmd = &cmd[0];
	req[0].rsp = &rsp[0];
	cmd[0].nvme_cmd.opc = SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;
	cmd[0].nvme_cmd.nsid = 1;
	cmd[0].nvme_cmd.cid = 0;

	for (i = 1; i < 3; i++) {
		req[i].qpair = &qpair;
		req[i].cmd = &cmd[i];
		req[i].rsp = &rsp[i];
		req[i].data = &data;
		req[i].length = sizeof(data);

		cmd[i].nvme_cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
		cmd[i].nvme_cmd.cdw10_bits.get_log_page.lid =
			SPDK_NVME_LOG_CHANGED_NS_LIST;
		cmd[i].nvme_cmd.cdw10_bits.get_log_page.numdl =
			spdk_nvme_bytes_to_numd(req[i].length);
		cmd[i].nvme_cmd.cid = i;
	}
	cmd[1].nvme_cmd.cdw10_bits.get_log_page.rae = 1;
	cmd[2].nvme_cmd.cdw10_bits.get_log_page.rae = 0;

	/* consume the pending event */
	TAILQ_INSERT_TAIL(&qpair.outstanding, &req[0], link);
	CU_ASSERT(nvmf_ctrlr_process_admin_cmd(&req[0]) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	event.raw = rsp[0].nvme_cpl.cdw0;
	CU_ASSERT(event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED);
	CU_ASSERT(num_pending_async_events(&ctrlr) == 0);

	/* get log with RAE set */
	CU_ASSERT(nvmf_ctrlr_get_log_page(&req[1]) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp[1].nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp[1].nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	/* will not generate new event until RAE is clear */
	nvmf_ctrlr_async_event_ns_notice(&ctrlr);
	CU_ASSERT(num_pending_async_events(&ctrlr) == 0);

	/* get log with RAE clear */
	CU_ASSERT(nvmf_ctrlr_get_log_page(&req[2]) == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp[2].nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(rsp[2].nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);

	nvmf_ctrlr_async_event_ns_notice(&ctrlr);
	CU_ASSERT(num_pending_async_events(&ctrlr) == 1);

	cleanup_pending_async_events(&ctrlr);
}

static void
test_nvmf_ctrlr_create_destruct(void)
{
	struct spdk_nvmf_fabric_connect_data connect_data = {};
	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups[2] = {};
	struct spdk_nvmf_transport transport = {};
	struct spdk_nvmf_transport_ops tops = {};
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr *ctrlr = NULL;
	struct spdk_nvmf_tgt tgt = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	const uint8_t hostid[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
	};
	const char subnqn[] = "nqn.2016-06.io.spdk:subsystem1";
	const char hostnqn[] = "nqn.2016-06.io.spdk:host1";

	group.thread = spdk_get_thread();
	transport.ops = &tops;
	transport.opts.max_aq_depth = 32;
	transport.opts.max_queue_depth = 64;
	transport.opts.max_qpairs_per_ctrlr = 3;
	transport.opts.dif_insert_or_strip = true;
	transport.tgt = &tgt;
	qpair.transport = &transport;
	qpair.group = &group;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	TAILQ_INIT(&qpair.outstanding);

	memcpy(connect_data.hostid, hostid, sizeof(hostid));
	connect_data.cntlid = 0xFFFF;
	snprintf(connect_data.subnqn, sizeof(connect_data.subnqn), "%s", subnqn);
	snprintf(connect_data.hostnqn, sizeof(connect_data.hostnqn), "%s", hostnqn);

	subsystem.thread = spdk_get_thread();
	subsystem.id = 1;
	TAILQ_INIT(&subsystem.ctrlrs);
	subsystem.tgt = &tgt;
	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "%s", subnqn);

	group.sgroups = sgroups;

	cmd.connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.connect_cmd.cid = 1;
	cmd.connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	cmd.connect_cmd.recfmt = 0;
	cmd.connect_cmd.qid = 0;
	cmd.connect_cmd.sqsize = 31;
	cmd.connect_cmd.cattr = 0;
	cmd.connect_cmd.kato = 120000;

	req.qpair = &qpair;
	req.length = sizeof(connect_data);
	req.xfer = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
	req.data = &connect_data;
	req.cmd = &cmd;
	req.rsp = &rsp;

	TAILQ_INSERT_TAIL(&qpair.outstanding, &req, link);
	sgroups[subsystem.id].mgmt_io_outstanding++;

	ctrlr = nvmf_ctrlr_create(&subsystem, &req, &req.cmd->connect_cmd, req.data);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(ctrlr != NULL);
	CU_ASSERT(req.qpair->ctrlr == ctrlr);
	CU_ASSERT(ctrlr->subsys == &subsystem);
	CU_ASSERT(ctrlr->thread == req.qpair->group->thread);
	CU_ASSERT(ctrlr->disconnect_in_progress == false);
	CU_ASSERT(ctrlr->qpair_mask != NULL);
	CU_ASSERT(ctrlr->feat.keep_alive_timer.bits.kato == 120000);
	CU_ASSERT(ctrlr->feat.async_event_configuration.bits.ns_attr_notice == 1);
	CU_ASSERT(ctrlr->feat.volatile_write_cache.bits.wce == 1);
	CU_ASSERT(ctrlr->feat.number_of_queues.bits.ncqr == 1);
	CU_ASSERT(ctrlr->feat.number_of_queues.bits.nsqr == 1);
	CU_ASSERT(!strncmp((void *)&ctrlr->hostid, hostid, 16));
	CU_ASSERT(ctrlr->vcprop.cap.bits.cqr == 1);
	CU_ASSERT(ctrlr->vcprop.cap.bits.mqes == 63);
	CU_ASSERT(ctrlr->vcprop.cap.bits.ams == 0);
	CU_ASSERT(ctrlr->vcprop.cap.bits.to == NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS / 500);
	CU_ASSERT(ctrlr->vcprop.cap.bits.dstrd == 0);
	CU_ASSERT(ctrlr->vcprop.cap.bits.css == SPDK_NVME_CAP_CSS_NVM);
	CU_ASSERT(ctrlr->vcprop.cap.bits.mpsmin == 0);
	CU_ASSERT(ctrlr->vcprop.cap.bits.mpsmax == 0);
	CU_ASSERT(ctrlr->vcprop.vs.bits.mjr == 1);
	CU_ASSERT(ctrlr->vcprop.vs.bits.mnr == 3);
	CU_ASSERT(ctrlr->vcprop.vs.bits.ter == 0);
	CU_ASSERT(ctrlr->vcprop.cc.raw == 0);
	CU_ASSERT(ctrlr->vcprop.cc.bits.en == 0);
	CU_ASSERT(ctrlr->vcprop.csts.raw == 0);
	CU_ASSERT(ctrlr->vcprop.csts.bits.rdy == 0);
	CU_ASSERT(ctrlr->dif_insert_or_strip == true);

	ctrlr->in_destruct = true;
	nvmf_ctrlr_destruct(ctrlr);
	poll_threads();
	CU_ASSERT(TAILQ_EMPTY(&subsystem.ctrlrs));
	CU_ASSERT(TAILQ_EMPTY(&qpair.outstanding));
}

static void
test_nvmf_ctrlr_use_zcopy(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_transport transport = {};
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	union nvmf_h2c_msg cmd = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ns *subsys_ns[1] = {};
	struct spdk_bdev bdev = {};
	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	struct spdk_nvmf_subsystem_pg_ns_info ns_info = {};
	struct spdk_io_channel io_ch = {};
	int opc;

	subsystem.subtype = SPDK_NVMF_SUBTYPE_NVME;
	ns.bdev = &bdev;

	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsys_ns[0] = &ns;
	subsystem.ns = (struct spdk_nvmf_ns **)&subsys_ns;

	ctrlr.subsys = &subsystem;

	transport.opts.zcopy = true;

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	qpair.qid = 1;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	qpair.transport = &transport;

	group.thread = spdk_get_thread();
	group.num_sgroups = 1;
	sgroups.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	sgroups.num_ns = 1;
	ns_info.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info.channel = &io_ch;
	sgroups.ns_info = &ns_info;
	TAILQ_INIT(&sgroups.queued);
	group.sgroups = &sgroups;
	TAILQ_INIT(&qpair.outstanding);

	req.qpair = &qpair;
	req.cmd = &cmd;
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Admin queue */
	qpair.qid = 0;
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req) == false);
	qpair.qid = 1;

	/* Invalid Opcodes */
	for (opc = 0; opc <= 255; opc++) {
		cmd.nvme_cmd.opc = (enum spdk_nvme_nvm_opcode) opc;
		if ((cmd.nvme_cmd.opc != SPDK_NVME_OPC_READ) &&
		    (cmd.nvme_cmd.opc != SPDK_NVME_OPC_WRITE)) {
			CU_ASSERT(nvmf_ctrlr_use_zcopy(&req) == false);
		}
	}
	cmd.nvme_cmd.opc = SPDK_NVME_OPC_WRITE;

	/* Fused WRITE */
	cmd.nvme_cmd.fuse = SPDK_NVME_CMD_FUSE_SECOND;
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req) == false);
	cmd.nvme_cmd.fuse = SPDK_NVME_CMD_FUSE_NONE;

	/* Non bdev */
	cmd.nvme_cmd.nsid = 4;
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req) == false);
	cmd.nvme_cmd.nsid = 1;

	/* ZCOPY Not supported */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req) == false);
	ns.zcopy = true;

	/* ZCOPY disabled on transport level */
	transport.opts.zcopy = false;
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req) == false);
	transport.opts.zcopy = true;

	/* Success */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
}

static void
qpair_state_change_done(void *cb_arg, int status)
{
}

static void
test_spdk_nvmf_request_zcopy_start(void)
{
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_transport transport = {};
	struct spdk_nvme_cmd cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ns *subsys_ns[1] = {};
	enum spdk_nvme_ana_state ana_state[1];
	struct spdk_nvmf_subsystem_listener listener = { .ana_state = ana_state };
	struct spdk_bdev bdev = { .blockcnt = 100, .blocklen = 512};

	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	struct spdk_nvmf_subsystem_pg_ns_info ns_info = {};
	struct spdk_io_channel io_ch = {};

	ns.bdev = &bdev;
	ns.zcopy = true;
	ns.anagrpid = 1;

	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsys_ns[0] = &ns;
	subsystem.ns = (struct spdk_nvmf_ns **)&subsys_ns;

	listener.ana_state[0] = SPDK_NVME_ANA_OPTIMIZED_STATE;

	/* Enable controller */
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.subsys = (struct spdk_nvmf_subsystem *)&subsystem;
	ctrlr.listener = &listener;

	transport.opts.zcopy = true;

	group.thread = spdk_get_thread();
	group.num_sgroups = 1;
	sgroups.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	sgroups.num_ns = 1;
	ns_info.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info.channel = &io_ch;
	sgroups.ns_info = &ns_info;
	TAILQ_INIT(&sgroups.queued);
	group.sgroups = &sgroups;
	TAILQ_INIT(&qpair.outstanding);

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	qpair.transport = &transport;
	qpair.qid = 1;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;

	cmd.nsid = 1;

	req.qpair = &qpair;
	req.cmd = (union nvmf_h2c_msg *)&cmd;
	req.rsp = &rsp;
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;
	cmd.opc = SPDK_NVME_OPC_READ;

	/* Fail because no controller */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	qpair.ctrlr = NULL;
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT_EQUAL(req.zcopy_phase, NVMF_ZCOPY_PHASE_INIT_FAILED);
	CU_ASSERT_EQUAL(rsp.nvme_cpl.status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(rsp.nvme_cpl.status.sc, SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR);
	qpair.ctrlr = &ctrlr;
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Fail because bad NSID */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	cmd.nsid = 0;
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT_EQUAL(req.zcopy_phase, NVMF_ZCOPY_PHASE_INIT_FAILED);
	CU_ASSERT_EQUAL(rsp.nvme_cpl.status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(rsp.nvme_cpl.status.sc, SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	cmd.nsid = 1;
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Fail because bad Channel */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	ns_info.channel = NULL;
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT_EQUAL(req.zcopy_phase, NVMF_ZCOPY_PHASE_INIT_FAILED);
	CU_ASSERT_EQUAL(rsp.nvme_cpl.status.sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(rsp.nvme_cpl.status.sc, SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
	ns_info.channel = &io_ch;
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Queue the requet because NSID is not active */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	ns_info.state = SPDK_NVMF_SUBSYSTEM_PAUSING;
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT_EQUAL(req.zcopy_phase, NVMF_ZCOPY_PHASE_INIT);
	CU_ASSERT_EQUAL(TAILQ_FIRST(&sgroups.queued), &req);
	ns_info.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	TAILQ_REMOVE(&sgroups.queued, &req, link);
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Fail because QPair is not active */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	qpair.state = SPDK_NVMF_QPAIR_DEACTIVATING;
	qpair.state_cb = qpair_state_change_done;
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT_FAILED);
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;
	qpair.state_cb = NULL;
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Fail because nvmf_bdev_ctrlr_zcopy_start fails */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	cmd.cdw10 = bdev.blockcnt;	/* SLBA: CDW10 and CDW11 */
	cmd.cdw12 = 100;	/* NLB: CDW12 bits 15:00, 0's based */
	req.length = (cmd.cdw12 + 1) * bdev.blocklen;
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT_EQUAL(req.zcopy_phase, NVMF_ZCOPY_PHASE_INIT_FAILED);
	cmd.cdw10 = 0;
	cmd.cdw12 = 0;
	req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Success */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE);
}

static void
test_zcopy_read(void)
{
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_transport transport = {};
	struct spdk_nvme_cmd cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ns *subsys_ns[1] = {};
	enum spdk_nvme_ana_state ana_state[1];
	struct spdk_nvmf_subsystem_listener listener = { .ana_state = ana_state };
	struct spdk_bdev bdev = { .blockcnt = 100, .blocklen = 512};

	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	struct spdk_nvmf_subsystem_pg_ns_info ns_info = {};
	struct spdk_io_channel io_ch = {};

	ns.bdev = &bdev;
	ns.zcopy = true;
	ns.anagrpid = 1;

	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsys_ns[0] = &ns;
	subsystem.ns = (struct spdk_nvmf_ns **)&subsys_ns;

	listener.ana_state[0] = SPDK_NVME_ANA_OPTIMIZED_STATE;

	/* Enable controller */
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.subsys = (struct spdk_nvmf_subsystem *)&subsystem;
	ctrlr.listener = &listener;

	transport.opts.zcopy = true;

	group.thread = spdk_get_thread();
	group.num_sgroups = 1;
	sgroups.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	sgroups.num_ns = 1;
	ns_info.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info.channel = &io_ch;
	sgroups.ns_info = &ns_info;
	TAILQ_INIT(&sgroups.queued);
	group.sgroups = &sgroups;
	TAILQ_INIT(&qpair.outstanding);

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	qpair.transport = &transport;
	qpair.qid = 1;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;

	cmd.nsid = 1;

	req.qpair = &qpair;
	req.cmd = (union nvmf_h2c_msg *)&cmd;
	req.rsp = &rsp;
	cmd.opc = SPDK_NVME_OPC_READ;

	/* Prepare for zcopy */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	CU_ASSERT(qpair.outstanding.tqh_first == NULL);
	CU_ASSERT(ns_info.io_outstanding == 0);

	/* Perform the zcopy start */
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE);
	CU_ASSERT(req.zcopy_bdev_io == zcopy_start_bdev_io_read);
	CU_ASSERT(qpair.outstanding.tqh_first == &req);
	CU_ASSERT(ns_info.io_outstanding == 1);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));

	/* Perform the zcopy end */
	spdk_nvmf_request_zcopy_end(&req, false);
	CU_ASSERT(req.zcopy_bdev_io == NULL);
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_COMPLETE);
	CU_ASSERT(qpair.outstanding.tqh_first == NULL);
	CU_ASSERT(ns_info.io_outstanding == 0);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
}

static void
test_zcopy_write(void)
{
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_transport transport = {};
	struct spdk_nvme_cmd cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ns *subsys_ns[1] = {};
	enum spdk_nvme_ana_state ana_state[1];
	struct spdk_nvmf_subsystem_listener listener = { .ana_state = ana_state };
	struct spdk_bdev bdev = { .blockcnt = 100, .blocklen = 512};

	struct spdk_nvmf_poll_group group = {};
	struct spdk_nvmf_subsystem_poll_group sgroups = {};
	struct spdk_nvmf_subsystem_pg_ns_info ns_info = {};
	struct spdk_io_channel io_ch = {};

	ns.bdev = &bdev;
	ns.zcopy = true;
	ns.anagrpid = 1;

	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsys_ns[0] = &ns;
	subsystem.ns = (struct spdk_nvmf_ns **)&subsys_ns;

	listener.ana_state[0] = SPDK_NVME_ANA_OPTIMIZED_STATE;

	/* Enable controller */
	ctrlr.vcprop.cc.bits.en = 1;
	ctrlr.subsys = (struct spdk_nvmf_subsystem *)&subsystem;
	ctrlr.listener = &listener;

	transport.opts.zcopy = true;

	group.thread = spdk_get_thread();
	group.num_sgroups = 1;
	sgroups.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	sgroups.num_ns = 1;
	ns_info.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	ns_info.channel = &io_ch;
	sgroups.ns_info = &ns_info;
	TAILQ_INIT(&sgroups.queued);
	group.sgroups = &sgroups;
	TAILQ_INIT(&qpair.outstanding);

	qpair.ctrlr = &ctrlr;
	qpair.group = &group;
	qpair.transport = &transport;
	qpair.qid = 1;
	qpair.state = SPDK_NVMF_QPAIR_ACTIVE;

	cmd.nsid = 1;

	req.qpair = &qpair;
	req.cmd = (union nvmf_h2c_msg *)&cmd;
	req.rsp = &rsp;
	cmd.opc = SPDK_NVME_OPC_WRITE;

	/* Prepare for zcopy */
	CU_ASSERT(nvmf_ctrlr_use_zcopy(&req));
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
	CU_ASSERT(qpair.outstanding.tqh_first == NULL);
	CU_ASSERT(ns_info.io_outstanding == 0);

	/* Perform the zcopy start */
	spdk_nvmf_request_zcopy_start(&req);
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE);
	CU_ASSERT(req.zcopy_bdev_io == zcopy_start_bdev_io_write);
	CU_ASSERT(qpair.outstanding.tqh_first == &req);
	CU_ASSERT(ns_info.io_outstanding == 1);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));

	/* Perform the zcopy end */
	spdk_nvmf_request_zcopy_end(&req, true);
	CU_ASSERT(req.zcopy_bdev_io == NULL);
	CU_ASSERT(req.zcopy_phase == NVMF_ZCOPY_PHASE_COMPLETE);
	CU_ASSERT(qpair.outstanding.tqh_first == NULL);
	CU_ASSERT(ns_info.io_outstanding == 0);
	CU_ASSERT(nvme_status_success(&rsp.nvme_cpl.status));
}

static void
test_nvmf_property_set(void)
{
	int rc;
	struct spdk_nvmf_request req = {};
	struct spdk_nvmf_qpair qpair = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};

	req.qpair = &qpair;
	qpair.ctrlr = &ctrlr;
	req.cmd = &cmd;
	req.rsp = &rsp;

	/* Invalid parameters */
	cmd.prop_set_cmd.attrib.size = SPDK_NVMF_PROP_SIZE_4;
	cmd.prop_set_cmd.ofst = offsetof(struct spdk_nvme_registers, vs);

	rc = nvmf_property_set(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);

	cmd.prop_set_cmd.ofst = offsetof(struct spdk_nvme_registers, intms);

	rc = nvmf_property_get(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(rsp.nvme_cpl.status.sct == SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT(rsp.nvme_cpl.status.sc == SPDK_NVMF_FABRIC_SC_INVALID_PARAM);

	/* Set cc with same property size */
	memset(req.rsp, 0, sizeof(union nvmf_c2h_msg));
	cmd.prop_set_cmd.ofst = offsetof(struct spdk_nvme_registers, cc);

	rc = nvmf_property_set(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);

	/* Emulate cc data */
	ctrlr.vcprop.cc.raw = 0xDEADBEEF;

	rc = nvmf_property_get(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->prop_get_rsp.value.u64 == 0xDEADBEEF);

	/* Set asq with different property size */
	memset(req.rsp, 0, sizeof(union nvmf_c2h_msg));
	cmd.prop_set_cmd.attrib.size = SPDK_NVMF_PROP_SIZE_4;
	cmd.prop_set_cmd.ofst = offsetof(struct spdk_nvme_registers, asq);

	rc = nvmf_property_set(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);

	/* Emulate asq data */
	ctrlr.vcprop.asq = 0xAADDADBEEF;

	rc = nvmf_property_get(&req);
	CU_ASSERT(rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE);
	CU_ASSERT(req.rsp->prop_get_rsp.value.u64 == 0xDDADBEEF);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);
	CU_ADD_TEST(suite, test_get_log_page);
	CU_ADD_TEST(suite, test_process_fabrics_cmd);
	CU_ADD_TEST(suite, test_connect);
	CU_ADD_TEST(suite, test_get_ns_id_desc_list);
	CU_ADD_TEST(suite, test_identify_ns);
	CU_ADD_TEST(suite, test_reservation_write_exclusive);
	CU_ADD_TEST(suite, test_reservation_exclusive_access);
	CU_ADD_TEST(suite, test_reservation_write_exclusive_regs_only_and_all_regs);
	CU_ADD_TEST(suite, test_reservation_exclusive_access_regs_only_and_all_regs);
	CU_ADD_TEST(suite, test_reservation_notification_log_page);
	CU_ADD_TEST(suite, test_get_dif_ctx);
	CU_ADD_TEST(suite, test_set_get_features);
	CU_ADD_TEST(suite, test_identify_ctrlr);
	CU_ADD_TEST(suite, test_custom_admin_cmd);
	CU_ADD_TEST(suite, test_fused_compare_and_write);
	CU_ADD_TEST(suite, test_multi_async_event_reqs);
	CU_ADD_TEST(suite, test_get_ana_log_page_one_ns_per_anagrp);
	CU_ADD_TEST(suite, test_get_ana_log_page_multi_ns_per_anagrp);
	CU_ADD_TEST(suite, test_multi_async_events);
	CU_ADD_TEST(suite, test_rae);
	CU_ADD_TEST(suite, test_nvmf_ctrlr_create_destruct);
	CU_ADD_TEST(suite, test_nvmf_ctrlr_use_zcopy);
	CU_ADD_TEST(suite, test_spdk_nvmf_request_zcopy_start);
	CU_ADD_TEST(suite, test_zcopy_read);
	CU_ADD_TEST(suite, test_zcopy_write);
	CU_ADD_TEST(suite, test_nvmf_property_set);

	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
