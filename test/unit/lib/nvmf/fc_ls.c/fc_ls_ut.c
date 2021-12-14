/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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

/* NVMF FC LS Command Processor Unit Test */

#include "spdk/env.h"
#include "spdk_cunit.h"
#include "spdk/nvmf.h"
#include "spdk/endian.h"
#include "spdk/trace.h"
#include "spdk/log.h"

#include "ut_multithread.c"

#include "transport.h"
#include "nvmf_internal.h"
#include "nvmf_fc.h"

#include "fc_ls.c"

#define LAST_RSLT_STOP_TEST 999

void spdk_set_thread(struct spdk_thread *thread);

/*
 * SPDK Stuff
 */

DEFINE_STUB(spdk_nvmf_request_complete, int, (struct spdk_nvmf_request *req), -ENOSPC);
DEFINE_STUB(spdk_nvmf_subsystem_host_allowed, bool,
	    (struct spdk_nvmf_subsystem *subsystem, const char *hostnqn), true);
DEFINE_STUB_V(spdk_nvme_trid_populate_transport, (struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_transport_type trtype));
DEFINE_STUB(rte_hash_del_key, int32_t, (const struct rte_hash *h, const void *key), 0);
DEFINE_STUB(rte_hash_lookup_data, int, (const struct rte_hash *h, const void *key, void **data),
	    -ENOENT);
DEFINE_STUB(rte_hash_add_key_data, int, (const struct rte_hash *h, const void *key, void *data), 0);
DEFINE_STUB(rte_hash_create, struct rte_hash *, (const struct rte_hash_parameters *params),
	    (void *)1);
DEFINE_STUB_V(rte_hash_free, (struct rte_hash *h));
DEFINE_STUB(nvmf_fc_poll_group_valid, bool, (struct spdk_nvmf_fc_poll_group *fgroup), true);

static const char *fc_ut_subsystem_nqn =
	"nqn.2017-11.io.spdk:sn.390c0dc7c87011e786b300a0989adc53:subsystem.good";
static struct spdk_nvmf_host fc_ut_initiator = {
	.nqn = "nqn.2017-11.fc_host",
};
static struct spdk_nvmf_host *fc_ut_host = &fc_ut_initiator;
static struct spdk_nvmf_tgt g_nvmf_tgt;
static struct spdk_nvmf_transport_opts g_nvmf_transport_opts = {
	.max_queue_depth = 128,
	.max_qpairs_per_ctrlr = 4,
	.max_aq_depth = 32,
};
static struct spdk_nvmf_subsystem g_nvmf_subsystem;

void nvmf_fc_request_abort(struct spdk_nvmf_fc_request *fc_req, bool send_abts,
			   spdk_nvmf_fc_caller_cb cb, void *cb_args);
void spdk_bdev_io_abort(struct spdk_bdev_io *bdev_io, void *ctx);
void nvmf_fc_request_abort_complete(void *arg1);
bool nvmf_fc_req_in_xfer(struct spdk_nvmf_fc_request *fc_req);

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	if (!strcmp(subnqn, g_nvmf_subsystem.subnqn)) {
		return &g_nvmf_subsystem;
	}
	return NULL;
}

int
spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	qpair->state = SPDK_NVMF_QPAIR_ACTIVE;
	return 0;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_fc = {
	.type = (enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC,
	.create = NULL,
	.destroy = NULL,

	.listen = NULL,
	.stop_listen = NULL,

	.listener_discover = NULL,

	.poll_group_create = NULL,
	.poll_group_destroy = NULL,
	.poll_group_add = NULL,
	.poll_group_poll = NULL,

	.req_complete = NULL,

	.qpair_fini = NULL,

};

struct spdk_nvmf_transport g_nvmf_transport = {
	.ops = &spdk_nvmf_transport_fc,
	.tgt = &g_nvmf_tgt,
};

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, const char *transport_name)
{
	return &g_nvmf_transport;
}

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair, nvmf_qpair_disconnect_cb cb_fn, void *ctx)
{
	cb_fn(ctx);
	return 0;
}

void
spdk_nvmf_tgt_new_qpair(struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_ls_add_conn_api_data *api_data = NULL;
	struct spdk_nvmf_fc_port *fc_port;
	static int hwqp_idx = 0;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	api_data = &fc_conn->create_opd->u.add_conn;

	fc_port = fc_conn->fc_assoc->tgtport->fc_port;
	hwqp = &fc_port->io_queues[hwqp_idx];

	if (!nvmf_fc_assign_conn_to_hwqp(hwqp,
					 &fc_conn->conn_id,
					 fc_conn->max_queue_depth)) {
		goto err;
	}

	fc_conn->hwqp = hwqp;

	/* If this is for ADMIN connection, then update assoc ID. */
	if (fc_conn->qpair.qid == 0) {
		fc_conn->fc_assoc->assoc_id = fc_conn->conn_id;
	}

	nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION, &api_data->args);
	hwqp_idx++;
	return;
err:
	nvmf_fc_ls_add_conn_failure(api_data->assoc, api_data->ls_rqst,
				    api_data->args.fc_conn, api_data->aq_conn);
}

void
nvmf_fc_free_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn)
{
}

int
nvmf_fc_create_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn)
{
	return 0;
}

/*
 * LLD functions
 */

bool
nvmf_fc_assign_conn_to_hwqp(struct spdk_nvmf_fc_hwqp *hwqp,
			    uint64_t *conn_id, uint32_t sq_size)
{
	static uint16_t conn_cnt = 0;

	SPDK_DEBUGLOG(nvmf_fc_ls, "Assign connection to HWQP\n");

	/* create connection ID */
	*conn_id = ((uint64_t)hwqp->hwqp_id | (conn_cnt++ << 8));

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "New connection assigned to HWQP%d, conn_id 0x%lx\n",
		      hwqp->hwqp_id, *conn_id);
	return true;
}

struct spdk_nvmf_fc_hwqp *
nvmf_fc_get_hwqp_from_conn_id(struct spdk_nvmf_fc_hwqp *queues,
			      uint32_t num_queues, uint64_t conn_id)
{
	return &queues[(conn_id & 0xff) % num_queues];
}

struct spdk_nvmf_fc_srsr_bufs *
nvmf_fc_alloc_srsr_bufs(size_t rqst_len, size_t rsp_len)
{
	struct spdk_nvmf_fc_srsr_bufs *srsr_bufs;

	srsr_bufs = calloc(1, sizeof(struct spdk_nvmf_fc_srsr_bufs));
	if (!srsr_bufs) {
		return NULL;
	}

	srsr_bufs->rqst = calloc(1, rqst_len + rsp_len);
	if (srsr_bufs->rqst) {
		srsr_bufs->rqst_len = rqst_len;
		srsr_bufs->rsp = srsr_bufs->rqst + rqst_len;
		srsr_bufs->rsp_len = rsp_len;
	} else {
		free(srsr_bufs);
		srsr_bufs = NULL;
	}

	return srsr_bufs;
}

void
nvmf_fc_free_srsr_bufs(struct spdk_nvmf_fc_srsr_bufs *srsr_bufs)
{
	if (srsr_bufs) {
		free(srsr_bufs->rqst);
		free(srsr_bufs);
	}
}

/*
 *  The Tests
 */

enum _test_run_type {
	TEST_RUN_TYPE_CREATE_ASSOC = 1,
	TEST_RUN_TYPE_CREATE_CONN,
	TEST_RUN_TYPE_DISCONNECT,
	TEST_RUN_TYPE_CONN_BAD_ASSOC,
	TEST_RUN_TYPE_FAIL_LS_RSP,
	TEST_RUN_TYPE_DISCONNECT_BAD_ASSOC,
	TEST_RUN_TYPE_CREATE_MAX_ASSOC,
};

static uint32_t g_test_run_type = 0;
static uint64_t g_curr_assoc_id = 0;
static uint16_t g_create_conn_test_cnt = 0;
static int g_last_rslt = 0;
static bool g_spdk_nvmf_fc_xmt_srsr_req = false;
static struct spdk_nvmf_fc_remote_port_info g_rem_port;

static void
run_create_assoc_test(const char *subnqn,
		      struct spdk_nvmf_host *host,
		      struct spdk_nvmf_fc_nport *tgt_port)
{
	struct spdk_nvmf_fc_ls_rqst ls_rqst;
	struct spdk_nvmf_fc_ls_cr_assoc_rqst ca_rqst;
	uint8_t respbuf[128];

	memset(&ca_rqst, 0, sizeof(struct spdk_nvmf_fc_ls_cr_assoc_rqst));

	ca_rqst.w0.ls_cmd = FCNVME_LS_CREATE_ASSOCIATION;
	to_be32(&ca_rqst.desc_list_len,
		sizeof(struct spdk_nvmf_fc_ls_cr_assoc_rqst) -
		(2 * sizeof(uint32_t)));
	to_be32(&ca_rqst.assoc_cmd.desc_tag, FCNVME_LSDESC_CREATE_ASSOC_CMD);
	to_be32(&ca_rqst.assoc_cmd.desc_len,
		sizeof(struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd) -
		(2 * sizeof(uint32_t)));
	to_be16(&ca_rqst.assoc_cmd.ersp_ratio, (g_nvmf_transport.opts.max_aq_depth / 2));
	to_be16(&ca_rqst.assoc_cmd.sqsize,  g_nvmf_transport.opts.max_aq_depth - 1);
	snprintf(&ca_rqst.assoc_cmd.subnqn[0], strlen(subnqn) + 1, "%s", subnqn);
	snprintf(&ca_rqst.assoc_cmd.hostnqn[0], strlen(host->nqn) + 1, "%s", host->nqn);
	ls_rqst.rqstbuf.virt = &ca_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct spdk_nvmf_fc_ls_cr_assoc_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;
	ls_rqst.s_id = 0;
	ls_rqst.nport = tgt_port;
	ls_rqst.rport = &g_rem_port;
	ls_rqst.nvmf_tgt = &g_nvmf_tgt;

	nvmf_fc_handle_ls_rqst(&ls_rqst);
	poll_thread(0);
}

static void
run_create_conn_test(struct spdk_nvmf_host *host,
		     struct spdk_nvmf_fc_nport *tgt_port,
		     uint64_t assoc_id,
		     uint16_t qid)
{
	struct spdk_nvmf_fc_ls_rqst ls_rqst;
	struct spdk_nvmf_fc_ls_cr_conn_rqst cc_rqst;
	uint8_t respbuf[128];

	memset(&cc_rqst, 0, sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst));

	/* fill in request descriptor */
	cc_rqst.w0.ls_cmd = FCNVME_LS_CREATE_CONNECTION;
	to_be32(&cc_rqst.desc_list_len,
		sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst) -
		(2 * sizeof(uint32_t)));

	/* fill in connect command descriptor */
	to_be32(&cc_rqst.connect_cmd.desc_tag, FCNVME_LSDESC_CREATE_CONN_CMD);
	to_be32(&cc_rqst.connect_cmd.desc_len,
		sizeof(struct spdk_nvmf_fc_lsdesc_cr_conn_cmd) -
		(2 * sizeof(uint32_t)));

	to_be16(&cc_rqst.connect_cmd.ersp_ratio, (g_nvmf_transport.opts.max_queue_depth / 2));
	to_be16(&cc_rqst.connect_cmd.sqsize, g_nvmf_transport.opts.max_queue_depth - 1);
	to_be16(&cc_rqst.connect_cmd.qid, qid);

	/* fill in association id descriptor */
	to_be32(&cc_rqst.assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID),
		to_be32(&cc_rqst.assoc_id.desc_len,
			sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) -
			(2 * sizeof(uint32_t)));
	cc_rqst.assoc_id.association_id = assoc_id; /* already be64 */

	ls_rqst.rqstbuf.virt = &cc_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;
	ls_rqst.s_id = 0;
	ls_rqst.nport = tgt_port;
	ls_rqst.rport = &g_rem_port;
	ls_rqst.nvmf_tgt = &g_nvmf_tgt;

	nvmf_fc_handle_ls_rqst(&ls_rqst);
	poll_thread(0);
}

static void
run_disconn_test(struct spdk_nvmf_fc_nport *tgt_port,
		 uint64_t assoc_id)
{
	struct spdk_nvmf_fc_ls_rqst ls_rqst;
	struct spdk_nvmf_fc_ls_disconnect_rqst dc_rqst;
	uint8_t respbuf[128];

	memset(&dc_rqst, 0, sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst));

	/* fill in request descriptor */
	dc_rqst.w0.ls_cmd = FCNVME_LS_DISCONNECT;
	to_be32(&dc_rqst.desc_list_len,
		sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst) -
		(2 * sizeof(uint32_t)));

	/* fill in disconnect command descriptor */
	to_be32(&dc_rqst.disconn_cmd.desc_tag, FCNVME_LSDESC_DISCONN_CMD);
	to_be32(&dc_rqst.disconn_cmd.desc_len,
		sizeof(struct spdk_nvmf_fc_lsdesc_disconn_cmd) -
		(2 * sizeof(uint32_t)));

	/* fill in association id descriptor */
	to_be32(&dc_rqst.assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID),
		to_be32(&dc_rqst.assoc_id.desc_len,
			sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) -
			(2 * sizeof(uint32_t)));
	dc_rqst.assoc_id.association_id = assoc_id; /* already be64 */

	ls_rqst.rqstbuf.virt = &dc_rqst;
	ls_rqst.rspbuf.virt = respbuf;
	ls_rqst.rqst_len = sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst);
	ls_rqst.rsp_len = 0;
	ls_rqst.rpi = 5000;
	ls_rqst.private_data = NULL;
	ls_rqst.s_id = 0;
	ls_rqst.nport = tgt_port;
	ls_rqst.rport = &g_rem_port;
	ls_rqst.nvmf_tgt = &g_nvmf_tgt;

	nvmf_fc_handle_ls_rqst(&ls_rqst);
	poll_thread(0);
}

static int
handle_ca_rsp(struct spdk_nvmf_fc_ls_rqst *ls_rqst, bool max_assoc_test)
{
	struct spdk_nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct spdk_nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;


	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_CREATE_ASSOCIATION) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_ACC) {
			struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =
				(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;

			CU_ASSERT(from_be32(&acc_hdr->desc_list_len) ==
				  sizeof(struct spdk_nvmf_fc_ls_cr_assoc_acc) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_rqst) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			CU_ASSERT(from_be32(&acc->assoc_id.desc_tag) ==
				  FCNVME_LSDESC_ASSOC_ID);
			CU_ASSERT(from_be32(&acc->assoc_id.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) - 8);
			CU_ASSERT(from_be32(&acc->conn_id.desc_tag) ==
				  FCNVME_LSDESC_CONN_ID);
			CU_ASSERT(from_be32(&acc->conn_id.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_conn_id) - 8);

			g_curr_assoc_id = acc->assoc_id.association_id;
			g_create_conn_test_cnt++;
			return 0;
		} else if (max_assoc_test) {
			/* reject reason code should be insufficient resources */
			struct spdk_nvmf_fc_ls_rjt *rjt =
				(struct spdk_nvmf_fc_ls_rjt *)ls_rqst->rspbuf.virt;
			if (rjt->rjt.reason_code == FCNVME_RJT_RC_INSUFF_RES) {
				return LAST_RSLT_STOP_TEST;
			}
		}
		CU_FAIL("Unexpected reject response for create association");
	} else {
		CU_FAIL("Response not for create association");
	}

	return -EINVAL;
}

static int
handle_cc_rsp(struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct spdk_nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;

	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_CREATE_CONNECTION) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_ACC) {
			struct spdk_nvmf_fc_ls_cr_conn_acc *acc =
				(struct spdk_nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;

			CU_ASSERT(from_be32(&acc_hdr->desc_list_len) ==
				  sizeof(struct spdk_nvmf_fc_ls_cr_conn_acc) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_rqst) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			CU_ASSERT(from_be32(&acc->conn_id.desc_tag) ==
				  FCNVME_LSDESC_CONN_ID);
			CU_ASSERT(from_be32(&acc->conn_id.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_conn_id) - 8);
			g_create_conn_test_cnt++;
			return 0;
		}

		if (acc_hdr->w0.ls_cmd == FCNVME_LS_RJT) {
			struct spdk_nvmf_fc_ls_rjt *rjt =
				(struct spdk_nvmf_fc_ls_rjt *)ls_rqst->rspbuf.virt;
			if (g_create_conn_test_cnt == g_nvmf_transport.opts.max_qpairs_per_ctrlr) {
				/* expected to get reject for too many connections */
				CU_ASSERT(rjt->rjt.reason_code ==
					  FCNVME_RJT_RC_INV_PARAM);
				CU_ASSERT(rjt->rjt.reason_explanation ==
					  FCNVME_RJT_EXP_INV_Q_ID);
			}
		} else {
			CU_FAIL("Unexpected response code for create connection");
		}
	} else {
		CU_FAIL("Response not for create connection");
	}

	return -EINVAL;
}

static int
handle_disconn_rsp(struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct spdk_nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;

	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_DISCONNECT) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_ACC) {
			CU_ASSERT(from_be32(&acc_hdr->desc_list_len) ==
				  sizeof(struct spdk_nvmf_fc_ls_disconnect_acc) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_rqst) - 8);
			CU_ASSERT(from_be32(&acc_hdr->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			return 0;
		} else {
			CU_FAIL("Unexpected reject response for disconnect");
		}
	} else {
		CU_FAIL("Response not for create connection");
	}

	return -EINVAL;
}

static int
handle_conn_bad_assoc_rsp(struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct spdk_nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;

	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_CREATE_CONNECTION) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_RJT) {
			struct spdk_nvmf_fc_ls_rjt *rjt =
				(struct spdk_nvmf_fc_ls_rjt *)ls_rqst->rspbuf.virt;

			CU_ASSERT(from_be32(&rjt->desc_list_len) ==
				  sizeof(struct spdk_nvmf_fc_ls_rjt) - 8);
			CU_ASSERT(from_be32(&rjt->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			CU_ASSERT(from_be32(&rjt->rjt.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_rjt) - 8);
			CU_ASSERT(from_be32(&rjt->rjt.desc_tag) ==
				  FCNVME_LSDESC_RJT);
			CU_ASSERT(rjt->rjt.reason_code ==
				  FCNVME_RJT_RC_INV_ASSOC);
			CU_ASSERT(rjt->rjt.reason_explanation ==
				  FCNVME_RJT_EXP_NONE);
			/* make sure reserved fields are 0 */
			CU_ASSERT(rjt->rjt.rsvd8 == 0);
			CU_ASSERT(rjt->rjt.rsvd12 == 0);
			return 0;
		} else {
			CU_FAIL("Unexpected accept response for create conn. on bad assoc_id");
		}
	} else {
		CU_FAIL("Response not for create connection on bad assoc_id");
	}

	return -EINVAL;
}

static int
handle_disconn_bad_assoc_rsp(struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_acc_hdr *acc_hdr =
		(struct spdk_nvmf_fc_ls_acc_hdr *) ls_rqst->rspbuf.virt;

	if (acc_hdr->rqst.w0.ls_cmd == FCNVME_LS_DISCONNECT) {
		if (acc_hdr->w0.ls_cmd == FCNVME_LS_RJT) {
			struct spdk_nvmf_fc_ls_rjt *rjt =
				(struct spdk_nvmf_fc_ls_rjt *)ls_rqst->rspbuf.virt;

			CU_ASSERT(from_be32(&rjt->desc_list_len) ==
				  sizeof(struct spdk_nvmf_fc_ls_rjt) - 8);
			CU_ASSERT(from_be32(&rjt->rqst.desc_tag) ==
				  FCNVME_LSDESC_RQST);
			CU_ASSERT(from_be32(&rjt->rjt.desc_len) ==
				  sizeof(struct spdk_nvmf_fc_lsdesc_rjt) - 8);
			CU_ASSERT(from_be32(&rjt->rjt.desc_tag) ==
				  FCNVME_LSDESC_RJT);
			CU_ASSERT(rjt->rjt.reason_code ==
				  FCNVME_RJT_RC_INV_ASSOC);
			CU_ASSERT(rjt->rjt.reason_explanation ==
				  FCNVME_RJT_EXP_NONE);
			return 0;
		} else {
			CU_FAIL("Unexpected accept response for disconnect on bad assoc_id");
		}
	} else {
		CU_FAIL("Response not for dsconnect on bad assoc_id");
	}

	return -EINVAL;
}


static struct spdk_nvmf_fc_port g_fc_port = {
	.num_io_queues = 16,
};

static struct spdk_nvmf_fc_nport g_tgt_port;

#define FC_LS_UT_MAX_IO_QUEUES 16
struct spdk_nvmf_fc_hwqp g_fc_hwqp[FC_LS_UT_MAX_IO_QUEUES];
struct spdk_nvmf_fc_poll_group g_fgroup[FC_LS_UT_MAX_IO_QUEUES];
struct spdk_nvmf_poll_group g_poll_group[FC_LS_UT_MAX_IO_QUEUES];
static bool threads_allocated = false;

static void
ls_assign_hwqp_threads(void)
{
	uint32_t i;

	for (i = 0; i < g_fc_port.num_io_queues; i++) {
		struct spdk_nvmf_fc_hwqp *hwqp = &g_fc_port.io_queues[i];
		if (hwqp->thread == NULL) {
			hwqp->thread = spdk_get_thread();
		}
	}
}

static void
ls_prepare_threads(void)
{
	if (threads_allocated == false) {
		allocate_threads(8);
		set_thread(0);
	}
	threads_allocated = true;
}

static void
setup_polling_threads(void)
{
	ls_prepare_threads();
	set_thread(0);
	ls_assign_hwqp_threads();
}

static int
ls_tests_init(void)
{
	uint16_t i;

	bzero(&g_nvmf_tgt, sizeof(g_nvmf_tgt));

	g_nvmf_transport.opts = g_nvmf_transport_opts;

	snprintf(g_nvmf_subsystem.subnqn, sizeof(g_nvmf_subsystem.subnqn), "%s", fc_ut_subsystem_nqn);
	g_fc_port.hw_port_status = SPDK_FC_PORT_ONLINE;
	g_fc_port.io_queues = g_fc_hwqp;
	for (i = 0; i < g_fc_port.num_io_queues; i++) {
		struct spdk_nvmf_fc_hwqp *hwqp = &g_fc_port.io_queues[i];
		hwqp->lcore_id = i;
		hwqp->hwqp_id = i;
		hwqp->thread = NULL;
		hwqp->fc_port = &g_fc_port;
		hwqp->num_conns = 0;
		TAILQ_INIT(&hwqp->in_use_reqs);

		bzero(&g_poll_group[i], sizeof(struct spdk_nvmf_poll_group));
		bzero(&g_fgroup[i], sizeof(struct spdk_nvmf_fc_poll_group));
		TAILQ_INIT(&g_poll_group[i].tgroups);
		TAILQ_INIT(&g_poll_group[i].qpairs);
		g_fgroup[i].group.transport = &g_nvmf_transport;
		g_fgroup[i].group.group = &g_poll_group[i];
		hwqp->fgroup = &g_fgroup[i];
	}

	nvmf_fc_ls_init(&g_fc_port);
	bzero(&g_tgt_port, sizeof(struct spdk_nvmf_fc_nport));
	g_tgt_port.fc_port = &g_fc_port;
	TAILQ_INIT(&g_tgt_port.rem_port_list);
	TAILQ_INIT(&g_tgt_port.fc_associations);

	bzero(&g_rem_port, sizeof(struct spdk_nvmf_fc_remote_port_info));
	TAILQ_INSERT_TAIL(&g_tgt_port.rem_port_list, &g_rem_port, link);

	return 0;
}

static int
ls_tests_fini(void)
{
	nvmf_fc_ls_fini(&g_fc_port);
	free_threads();
	return 0;
}

static void
create_single_assoc_test(void)
{
	setup_polling_threads();
	/* main test driver */
	g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;
	run_create_assoc_test(fc_ut_subsystem_nqn, fc_ut_host, &g_tgt_port);

	if (g_last_rslt == 0) {
		/* disconnect the association */
		g_test_run_type = TEST_RUN_TYPE_DISCONNECT;
		run_disconn_test(&g_tgt_port, g_curr_assoc_id);
		g_create_conn_test_cnt = 0;
	}
}

static void
create_max_conns_test(void)
{
	uint16_t qid = 1;

	setup_polling_threads();
	/* main test driver */
	g_test_run_type = TEST_RUN_TYPE_CREATE_ASSOC;
	run_create_assoc_test(fc_ut_subsystem_nqn, fc_ut_host, &g_tgt_port);

	if (g_last_rslt == 0) {
		g_test_run_type = TEST_RUN_TYPE_CREATE_CONN;
		/* create connections until we get too many connections error */
		while (g_last_rslt == 0) {
			if (g_create_conn_test_cnt > g_nvmf_transport.opts.max_qpairs_per_ctrlr) {
				CU_FAIL("Did not get CIOC failure for too many connections");
				break;
			}
			run_create_conn_test(fc_ut_host, &g_tgt_port, g_curr_assoc_id, qid++);
		}

		/* disconnect the association */
		g_last_rslt = 0;
		g_test_run_type = TEST_RUN_TYPE_DISCONNECT;
		run_disconn_test(&g_tgt_port, g_curr_assoc_id);
		g_create_conn_test_cnt = 0;
	}
}

static void
invalid_connection_test(void)
{
	setup_polling_threads();
	/* run test to create connection to invalid association */
	g_test_run_type = TEST_RUN_TYPE_CONN_BAD_ASSOC;
	run_create_conn_test(fc_ut_host, &g_tgt_port, g_curr_assoc_id, 1);
}

static void
xmt_ls_rsp_failure_test(void)
{
	setup_polling_threads();
	g_test_run_type = TEST_RUN_TYPE_FAIL_LS_RSP;
	run_create_assoc_test(fc_ut_subsystem_nqn, fc_ut_host, &g_tgt_port);
	if (g_last_rslt == 0) {
		/* check target port for associations */
		CU_ASSERT(g_tgt_port.assoc_count == 0);
	}
}

static void
disconnect_bad_assoc_test(void)
{
	setup_polling_threads();
	g_test_run_type = TEST_RUN_TYPE_DISCONNECT_BAD_ASSOC;
	run_disconn_test(&g_tgt_port, 0xffff);
}

/*
 * SPDK functions that are called by LS processing
 */

int
nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *g_tgt_port,
		   struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	switch (g_test_run_type) {
	case TEST_RUN_TYPE_CREATE_ASSOC:
		g_last_rslt = handle_ca_rsp(ls_rqst, false);
		break;
	case TEST_RUN_TYPE_CREATE_CONN:
		g_last_rslt = handle_cc_rsp(ls_rqst);
		break;
	case TEST_RUN_TYPE_DISCONNECT:
		g_last_rslt = handle_disconn_rsp(ls_rqst);
		break;
	case TEST_RUN_TYPE_CONN_BAD_ASSOC:
		g_last_rslt = handle_conn_bad_assoc_rsp(ls_rqst);
		break;
	case TEST_RUN_TYPE_FAIL_LS_RSP:
		g_last_rslt = handle_ca_rsp(ls_rqst, false);
		return 1;
	case TEST_RUN_TYPE_DISCONNECT_BAD_ASSOC:
		g_last_rslt = handle_disconn_bad_assoc_rsp(ls_rqst);
		break;
	case TEST_RUN_TYPE_CREATE_MAX_ASSOC:
		g_last_rslt = handle_ca_rsp(ls_rqst, true);
		break;

	default:
		CU_FAIL("LS Response for Invalid Test Type");
		g_last_rslt = 1;
	}

	return 0;
}

int
nvmf_fc_xmt_srsr_req(struct spdk_nvmf_fc_hwqp *hwqp,
		     struct spdk_nvmf_fc_srsr_bufs *srsr_bufs,
		     spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	struct spdk_nvmf_fc_ls_disconnect_rqst *dc_rqst =
		(struct spdk_nvmf_fc_ls_disconnect_rqst *)
		srsr_bufs->rqst;

	CU_ASSERT(dc_rqst->w0.ls_cmd == FCNVME_LS_DISCONNECT);
	CU_ASSERT(from_be32(&dc_rqst->desc_list_len) ==
		  sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst) -
		  (2 * sizeof(uint32_t)));
	CU_ASSERT(from_be32(&dc_rqst->assoc_id.desc_tag) ==
		  FCNVME_LSDESC_ASSOC_ID);
	CU_ASSERT(from_be32(&dc_rqst->assoc_id.desc_len) ==
		  sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) -
		  (2 * sizeof(uint32_t)));

	g_spdk_nvmf_fc_xmt_srsr_req = true;

	if (cb) {
		cb(hwqp, 0, cb_args);
	}

	return 0;
}

DEFINE_STUB_V(nvmf_fc_request_abort, (struct spdk_nvmf_fc_request *fc_req,
				      bool send_abts, spdk_nvmf_fc_caller_cb cb, void *cb_args));
DEFINE_STUB_V(spdk_bdev_io_abort, (struct spdk_bdev_io *bdev_io, void *ctx));
DEFINE_STUB_V(nvmf_fc_request_abort_complete, (void *arg1));

static void
usage(const char *program_name)
{
	printf("%s [options]\n", program_name);
	printf("options:\n");
	spdk_log_usage(stdout, "-t");
	printf(" -i value - Number of IO Queues (default: %u)\n",
	       g_fc_port.num_io_queues);
	printf(" -q value - SQ size (default: %u)\n",
	       g_nvmf_transport_opts.max_queue_depth);
	printf(" -c value - Connection count (default: %u)\n",
	       g_nvmf_transport_opts.max_qpairs_per_ctrlr);
	printf(" -u test# - Unit test# to run\n");
	printf("            0 : Run all tests (default)\n");
	printf("            1 : CASS/DISC create single assoc test\n");
	printf("            2 : Max. conns. test\n");
	printf("            3 : CIOC to invalid assoc_id connection test\n");
	printf("            4 : Create/delete max assoc conns test\n");
	printf("            5 : LS response failure test\n");
	printf("            6 : Disconnect bad assoc_id test\n");
}

int main(int argc, char **argv)
{
	unsigned int num_failures = 0;
	CU_pSuite suite = NULL;
	int test = 0;
	long int val;
	int op;

	while ((op = getopt(argc, argv, "a:q:c:t:u:d:i:")) != -1) {
		switch (op) {
		case 'q':
			val = spdk_strtol(optarg, 10);
			if (val < 16) {
				fprintf(stderr, "SQ size must be at least 16\n");
				return -EINVAL;
			}
			g_nvmf_transport_opts.max_queue_depth = (uint16_t)val;
			break;
		case 'c':
			val = spdk_strtol(optarg, 10);
			if (val < 2) {
				fprintf(stderr, "Connection count must be at least 2\n");
				return -EINVAL;
			}
			g_nvmf_transport_opts.max_qpairs_per_ctrlr = (uint16_t)val;
			break;
		case 't':
			if (spdk_log_set_flag(optarg) < 0) {
				fprintf(stderr, "Unknown trace flag '%s'\n", optarg);
				usage(argv[0]);
				return -EINVAL;
			}
			break;
		case 'u':
			test = (int)spdk_strtol(optarg, 10);
			break;
		case 'i':
			val = spdk_strtol(optarg, 10);
			if (val < 2) {
				fprintf(stderr, "Number of io queues must be at least 2\n");
				return -EINVAL;
			}
			if (val > FC_LS_UT_MAX_IO_QUEUES) {
				fprintf(stderr, "Number of io queues can't be greater than %d\n",
					FC_LS_UT_MAX_IO_QUEUES);
				return -EINVAL;
			}
			g_fc_port.num_io_queues = (uint32_t)val;
			break;


		default:
			usage(argv[0]);
			return -EINVAL;
		}
	}

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("FC-NVMe LS", ls_tests_init, ls_tests_fini);

	if (test == 0) {

		CU_ADD_TEST(suite, create_single_assoc_test);

		CU_ADD_TEST(suite, create_max_conns_test);
		CU_ADD_TEST(suite, invalid_connection_test);
		CU_ADD_TEST(suite, disconnect_bad_assoc_test);

		CU_ADD_TEST(suite, xmt_ls_rsp_failure_test);

	} else {

		switch (test) {
		case 1:
			CU_ADD_TEST(suite, create_single_assoc_test);
			break;
		case 2:
			CU_ADD_TEST(suite, create_max_conns_test);
			break;
		case 3:
			CU_ADD_TEST(suite, invalid_connection_test);
			break;
		case 5:
			CU_ADD_TEST(suite, xmt_ls_rsp_failure_test);
			break;
		case 6:
			CU_ADD_TEST(suite, disconnect_bad_assoc_test);
			break;

		default:
			fprintf(stderr, "Invalid test number\n");
			usage(argv[0]);
			CU_cleanup_registry();
			return -EINVAL;
		}
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
