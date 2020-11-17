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

#include "spdk/endian.h"
#include "spdk/scsi.h"
#include "spdk_cunit.h"

#include "CUnit/Basic.h"

#include "iscsi/iscsi.c"

#include "../common.c"
#include "iscsi/portal_grp.h"
#include "scsi/scsi_internal.h"
#include "common/lib/test_env.c"

#include "spdk_internal/mock.h"

#define UT_TARGET_NAME1		"iqn.2017-11.spdk.io:t0001"
#define UT_TARGET_NAME2		"iqn.2017-11.spdk.io:t0002"
#define UT_INITIATOR_NAME1	"iqn.2017-11.spdk.io:i0001"
#define UT_INITIATOR_NAME2	"iqn.2017-11.spdk.io:i0002"
#define UT_ISCSI_TSIH		256

struct spdk_iscsi_tgt_node	g_tgt;

struct spdk_iscsi_tgt_node *
iscsi_find_tgt_node(const char *target_name)
{
	if (strcasecmp(target_name, UT_TARGET_NAME1) == 0) {
		g_tgt.dev = NULL;
		return (struct spdk_iscsi_tgt_node *)&g_tgt;
	} else {
		return NULL;
	}
}

bool
iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
		      struct spdk_iscsi_tgt_node *target,
		      const char *iqn, const char *addr)
{
	if (strcasecmp(conn->initiator_name, UT_INITIATOR_NAME1) == 0) {
		return true;
	} else {
		return false;
	}
}

DEFINE_STUB(iscsi_tgt_node_is_redirected, bool,
	    (struct spdk_iscsi_conn *conn, struct spdk_iscsi_tgt_node *target,
	     char *buf, int buf_len),
	    false);

DEFINE_STUB(iscsi_send_tgts, int,
	    (struct spdk_iscsi_conn *conn, const char *iiqn,
	     const char *tiqn, uint8_t *data, int alloc_len, int data_len),
	    0);

DEFINE_STUB(iscsi_tgt_node_is_destructed, bool,
	    (struct spdk_iscsi_tgt_node *target), false);

DEFINE_STUB_V(iscsi_portal_grp_close_all, (void));

DEFINE_STUB_V(iscsi_conn_schedule, (struct spdk_iscsi_conn *conn));

DEFINE_STUB_V(iscsi_conn_free_pdu,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu));

DEFINE_STUB_V(iscsi_conn_pdu_generic_complete, (void *cb_arg));

DEFINE_STUB(iscsi_conn_handle_queued_datain_tasks, int,
	    (struct spdk_iscsi_conn *conn), 0);

DEFINE_STUB(iscsi_conn_abort_queued_datain_task, int,
	    (struct spdk_iscsi_conn *conn, uint32_t ref_task_tag), 0);

DEFINE_STUB(iscsi_conn_abort_queued_datain_tasks, int,
	    (struct spdk_iscsi_conn *conn, struct spdk_scsi_lun *lun,
	     struct spdk_iscsi_pdu *pdu), 0);

DEFINE_STUB(iscsi_chap_get_authinfo, int,
	    (struct iscsi_chap_auth *auth, const char *authuser, int ag_tag),
	    0);

DEFINE_STUB(spdk_sock_set_recvbuf, int, (struct spdk_sock *sock, int sz), 0);

int
spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun)
{
	return lun->id;
}

DEFINE_STUB(spdk_scsi_lun_is_removing, bool, (const struct spdk_scsi_lun *lun),
	    true);

struct spdk_scsi_lun *
spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id)
{
	if (lun_id < 0 || lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		return NULL;
	}

	return dev->lun[lun_id];
}

DEFINE_STUB(spdk_scsi_lun_id_int_to_fmt, uint64_t, (int lun_id), 0);

DEFINE_STUB(spdk_scsi_lun_id_fmt_to_int, int, (uint64_t lun_fmt), 0);

DEFINE_STUB(spdk_scsi_lun_get_dif_ctx, bool,
	    (struct spdk_scsi_lun *lun, struct spdk_scsi_task *task,
	     struct spdk_dif_ctx *dif_ctx), false);

static void
op_login_check_target_test(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu rsp_pdu = {};
	struct spdk_iscsi_tgt_node *target;
	int rc;

	/* expect success */
	snprintf(conn.initiator_name, sizeof(conn.initiator_name),
		 "%s", UT_INITIATOR_NAME1);

	rc = iscsi_op_login_check_target(&conn, &rsp_pdu,
					 UT_TARGET_NAME1, &target);
	CU_ASSERT(rc == 0);

	/* expect failure */
	snprintf(conn.initiator_name, sizeof(conn.initiator_name),
		 "%s", UT_INITIATOR_NAME1);

	rc = iscsi_op_login_check_target(&conn, &rsp_pdu,
					 UT_TARGET_NAME2, &target);
	CU_ASSERT(rc != 0);

	/* expect failure */
	snprintf(conn.initiator_name, sizeof(conn.initiator_name),
		 "%s", UT_INITIATOR_NAME2);

	rc = iscsi_op_login_check_target(&conn, &rsp_pdu,
					 UT_TARGET_NAME1, &target);
	CU_ASSERT(rc != 0);
}

static void
op_login_session_normal_test(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_portal portal = {};
	struct spdk_iscsi_portal_grp group = {};
	struct spdk_iscsi_pdu rsp_pdu = {};
	struct iscsi_bhs_login_rsp *rsph;
	struct spdk_iscsi_sess sess = {};
	struct iscsi_param param = {};
	int rc;

	/* setup related data structures */
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu.bhs;
	rsph->tsih = 0;
	memset(rsph->isid, 0, sizeof(rsph->isid));
	conn.portal = &portal;
	portal.group = &group;
	conn.portal->group->tag = 0;
	conn.params = NULL;

	/* expect failure: NULL params for target name */
	rc = iscsi_op_login_session_normal(&conn, &rsp_pdu, UT_INITIATOR_NAME1,
					   NULL, 0);
	CU_ASSERT(rc != 0);
	CU_ASSERT(rsph->status_class == ISCSI_CLASS_INITIATOR_ERROR);
	CU_ASSERT(rsph->status_detail == ISCSI_LOGIN_MISSING_PARMS);

	/* expect failure: incorrect key for target name */
	param.next = NULL;
	rc = iscsi_op_login_session_normal(&conn, &rsp_pdu, UT_INITIATOR_NAME1,
					   &param, 0);
	CU_ASSERT(rc != 0);
	CU_ASSERT(rsph->status_class == ISCSI_CLASS_INITIATOR_ERROR);
	CU_ASSERT(rsph->status_detail == ISCSI_LOGIN_MISSING_PARMS);

	/* expect failure: NULL target name */
	param.key = "TargetName";
	param.val = NULL;
	rc = iscsi_op_login_session_normal(&conn, &rsp_pdu, UT_INITIATOR_NAME1,
					   &param, 0);
	CU_ASSERT(rc != 0);
	CU_ASSERT(rsph->status_class == ISCSI_CLASS_INITIATOR_ERROR);
	CU_ASSERT(rsph->status_detail == ISCSI_LOGIN_MISSING_PARMS);

	/* expect failure: session not found */
	param.key = "TargetName";
	param.val = "iqn.2017-11.spdk.io:t0001";
	snprintf(conn.initiator_name, sizeof(conn.initiator_name),
		 "%s", UT_INITIATOR_NAME1);
	rsph->tsih = 1; /* to append the session */
	rc = iscsi_op_login_session_normal(&conn, &rsp_pdu, UT_INITIATOR_NAME1,
					   &param, 0);
	CU_ASSERT(conn.target_port == NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(rsph->status_class == ISCSI_CLASS_INITIATOR_ERROR);
	CU_ASSERT(rsph->status_detail == ISCSI_LOGIN_CONN_ADD_FAIL);

	/* expect failure: session found while tag is wrong */
	g_iscsi.MaxSessions = UT_ISCSI_TSIH * 2;
	g_iscsi.session = calloc(1, sizeof(void *) * g_iscsi.MaxSessions);
	g_iscsi.session[UT_ISCSI_TSIH - 1] = &sess;
	sess.tsih = UT_ISCSI_TSIH;
	rsph->tsih = UT_ISCSI_TSIH >> 8; /* to append the session */
	sess.tag = 1;
	rc = iscsi_op_login_session_normal(&conn, &rsp_pdu, UT_INITIATOR_NAME1,
					   &param, 0);
	CU_ASSERT(conn.target_port == NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(rsph->status_class == ISCSI_CLASS_INITIATOR_ERROR);
	CU_ASSERT(rsph->status_detail == ISCSI_LOGIN_CONN_ADD_FAIL);

	/* expect suceess: drop the session */
	rsph->tsih = 0; /* to create the session */
	g_iscsi.AllowDuplicateIsid = false;
	rc = iscsi_op_login_session_normal(&conn, &rsp_pdu, UT_INITIATOR_NAME1,
					   &param, 0);
	CU_ASSERT(rc == 0);

	/* expect suceess: create the session */
	rsph->tsih = 0; /* to create the session */
	g_iscsi.AllowDuplicateIsid = true;
	rc = iscsi_op_login_session_normal(&conn, &rsp_pdu, UT_INITIATOR_NAME1,
					   &param, 0);
	CU_ASSERT(rc == 0);

	free(g_iscsi.session);
}

static void
maxburstlength_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct spdk_iscsi_pdu *req_pdu, *data_out_pdu, *r2t_pdu;
	struct iscsi_bhs_scsi_req *req;
	struct iscsi_bhs_r2t *r2t;
	struct iscsi_bhs_data_out *data_out;
	struct spdk_iscsi_pdu *response_pdu;
	int rc;

	g_iscsi.MaxR2TPerConnection = DEFAULT_MAXR2T;

	req_pdu = iscsi_get_pdu(&conn);
	data_out_pdu = iscsi_get_pdu(&conn);

	sess.ExpCmdSN = 0;
	sess.MaxCmdSN = 64;
	sess.session_type = SESSION_TYPE_NORMAL;
	sess.MaxBurstLength = 1024;

	lun.id = 0;

	dev.lun[0] = &lun;

	conn.full_feature = 1;
	conn.sess = &sess;
	conn.dev = &dev;
	conn.state = ISCSI_CONN_STATE_RUNNING;
	TAILQ_INIT(&conn.write_pdu_list);
	TAILQ_INIT(&conn.active_r2t_tasks);

	req_pdu->bhs.opcode = ISCSI_OP_SCSI;
	req_pdu->data_segment_len = 0;

	req = (struct iscsi_bhs_scsi_req *)&req_pdu->bhs;

	to_be32(&req->cmd_sn, 0);
	to_be32(&req->expected_data_xfer_len, 1028);
	to_be32(&req->itt, 0x1234);
	req->write_bit = 1;
	req->final_bit = 1;

	rc = iscsi_pdu_hdr_handle(&conn, req_pdu);
	if (rc == 0 && !req_pdu->is_rejected) {
		rc = iscsi_pdu_payload_handle(&conn, req_pdu);
	}
	CU_ASSERT(rc == 0);

	response_pdu = TAILQ_FIRST(&g_write_pdu_list);
	SPDK_CU_ASSERT_FATAL(response_pdu != NULL);

	/*
	 * Confirm that a correct R2T reply was sent in response to the
	 *  SCSI request.
	 */
	TAILQ_REMOVE(&g_write_pdu_list, response_pdu, tailq);
	CU_ASSERT(response_pdu->bhs.opcode == ISCSI_OP_R2T);
	r2t = (struct iscsi_bhs_r2t *)&response_pdu->bhs;
	CU_ASSERT(from_be32(&r2t->desired_xfer_len) == 1024);
	CU_ASSERT(from_be32(&r2t->buffer_offset) == 0);
	CU_ASSERT(from_be32(&r2t->itt) == 0x1234);

	data_out_pdu->bhs.opcode = ISCSI_OP_SCSI_DATAOUT;
	data_out_pdu->bhs.flags = ISCSI_FLAG_FINAL;
	data_out_pdu->data_segment_len = 1028;
	data_out = (struct iscsi_bhs_data_out *)&data_out_pdu->bhs;
	data_out->itt = r2t->itt;
	data_out->ttt = r2t->ttt;
	DSET24(data_out->data_segment_len, 1028);

	rc = iscsi_pdu_hdr_handle(&conn, data_out_pdu);
	if (rc == 0 && !data_out_pdu->is_rejected) {
		rc = iscsi_pdu_payload_handle(&conn, data_out_pdu);
	}
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	SPDK_CU_ASSERT_FATAL(response_pdu->task != NULL);
	iscsi_task_disassociate_pdu(response_pdu->task);
	iscsi_task_put(response_pdu->task);
	iscsi_put_pdu(response_pdu);

	r2t_pdu = TAILQ_FIRST(&g_write_pdu_list);
	CU_ASSERT(r2t_pdu != NULL);
	TAILQ_REMOVE(&g_write_pdu_list, r2t_pdu, tailq);
	iscsi_put_pdu(r2t_pdu);

	iscsi_put_pdu(data_out_pdu);
	iscsi_put_pdu(req_pdu);
}

static void
underflow_for_read_transfer_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_data_in *datah;
	uint32_t residual_count = 0;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	dev.lun[0] = &lun;
	conn.dev = &dev;

	pdu = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
	scsi_req->read_bit = 1;

	iscsi_task_set_pdu(&task, pdu);
	task.parent = NULL;

	task.scsi.iovs = &task.scsi.iov;
	task.scsi.iovcnt = 1;
	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;
	task.scsi.data_transferred = 256;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;

	iscsi_task_response(&conn, &task);
	iscsi_put_pdu(pdu);

	/*
	 * In this case, a SCSI Data-In PDU should contain the Status
	 * for the data transfer.
	 */
	to_be32(&residual_count, 256);

	pdu = TAILQ_FIRST(&g_write_pdu_list);
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	CU_ASSERT(pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN);

	datah = (struct iscsi_bhs_data_in *)&pdu->bhs;

	CU_ASSERT(datah->flags == (ISCSI_DATAIN_UNDERFLOW | ISCSI_FLAG_FINAL | ISCSI_DATAIN_STATUS));
	CU_ASSERT(datah->res_cnt == residual_count);

	TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
	iscsi_put_pdu(pdu);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
underflow_for_zero_read_transfer_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_scsi_resp *resph;
	uint32_t residual_count = 0, data_segment_len;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	dev.lun[0] = &lun;
	conn.dev = &dev;

	pdu = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
	scsi_req->read_bit = 1;

	iscsi_task_set_pdu(&task, pdu);
	task.parent = NULL;

	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;
	task.scsi.data_transferred = 0;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;

	iscsi_task_response(&conn, &task);
	iscsi_put_pdu(pdu);

	/*
	 * In this case, only a SCSI Response PDU is expected and
	 * underflow must be set in it.
	 * */
	to_be32(&residual_count, 512);

	pdu = TAILQ_FIRST(&g_write_pdu_list);
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	CU_ASSERT(pdu->bhs.opcode == ISCSI_OP_SCSI_RSP);

	resph = (struct iscsi_bhs_scsi_resp *)&pdu->bhs;

	CU_ASSERT(resph->flags == (ISCSI_SCSI_UNDERFLOW | 0x80));

	data_segment_len = DGET24(resph->data_segment_len);
	CU_ASSERT(data_segment_len == 0);
	CU_ASSERT(resph->res_cnt == residual_count);

	TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
	iscsi_put_pdu(pdu);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
underflow_for_request_sense_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct spdk_iscsi_pdu *pdu1, *pdu2;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_data_in *datah;
	struct iscsi_bhs_scsi_resp *resph;
	uint32_t residual_count = 0, data_segment_len;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	dev.lun[0] = &lun;
	conn.dev = &dev;

	pdu1 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu1->bhs;
	scsi_req->read_bit = 1;

	iscsi_task_set_pdu(&task, pdu1);
	task.parent = NULL;

	task.scsi.iovs = &task.scsi.iov;
	task.scsi.iovcnt = 1;
	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;

	task.scsi.sense_data_len = 18;
	task.scsi.data_transferred = 18;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;

	iscsi_task_response(&conn, &task);
	iscsi_put_pdu(pdu1);

	/*
	 * In this case, a SCSI Data-In PDU and a SCSI Response PDU are returned.
	 * Sense data are set both in payload and sense area.
	 * The SCSI Data-In PDU sets FINAL and the SCSI Response PDU sets UNDERFLOW.
	 *
	 * Probably there will be different implementation but keeping current SPDK
	 * implementation by adding UT will be valuable for any implementation.
	 */
	to_be32(&residual_count, 494);

	pdu1 = TAILQ_FIRST(&g_write_pdu_list);
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	CU_ASSERT(pdu1->bhs.opcode == ISCSI_OP_SCSI_DATAIN);

	datah = (struct iscsi_bhs_data_in *)&pdu1->bhs;

	CU_ASSERT(datah->flags == ISCSI_FLAG_FINAL);

	data_segment_len = DGET24(datah->data_segment_len);
	CU_ASSERT(data_segment_len == 18);
	CU_ASSERT(datah->res_cnt == 0);

	TAILQ_REMOVE(&g_write_pdu_list, pdu1, tailq);
	iscsi_put_pdu(pdu1);

	pdu2 = TAILQ_FIRST(&g_write_pdu_list);
	/* inform scan-build (clang 6) that these pointers are not the same */
	SPDK_CU_ASSERT_FATAL(pdu1 != pdu2);
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	CU_ASSERT(pdu2->bhs.opcode == ISCSI_OP_SCSI_RSP);

	resph = (struct iscsi_bhs_scsi_resp *)&pdu2->bhs;

	CU_ASSERT(resph->flags == (ISCSI_SCSI_UNDERFLOW | 0x80));

	data_segment_len = DGET24(resph->data_segment_len);
	CU_ASSERT(data_segment_len == task.scsi.sense_data_len + 2);
	CU_ASSERT(resph->res_cnt == residual_count);

	TAILQ_REMOVE(&g_write_pdu_list, pdu2, tailq);
	iscsi_put_pdu(pdu2);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
underflow_for_check_condition_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_scsi_resp *resph;
	uint32_t data_segment_len;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	dev.lun[0] = &lun;
	conn.dev = &dev;

	pdu = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
	scsi_req->read_bit = 1;

	iscsi_task_set_pdu(&task, pdu);
	task.parent = NULL;

	task.scsi.iovs = &task.scsi.iov;
	task.scsi.iovcnt = 1;
	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;

	task.scsi.sense_data_len = 18;
	task.scsi.data_transferred = 18;
	task.scsi.status = SPDK_SCSI_STATUS_CHECK_CONDITION;

	iscsi_task_response(&conn, &task);
	iscsi_put_pdu(pdu);

	/*
	 * In this case, a SCSI Response PDU is returned.
	 * Sense data is set in sense area.
	 * Underflow is not set.
	 */
	pdu = TAILQ_FIRST(&g_write_pdu_list);
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	CU_ASSERT(pdu->bhs.opcode == ISCSI_OP_SCSI_RSP);

	resph = (struct iscsi_bhs_scsi_resp *)&pdu->bhs;

	CU_ASSERT(resph->flags == 0x80);

	data_segment_len = DGET24(resph->data_segment_len);
	CU_ASSERT(data_segment_len == task.scsi.sense_data_len + 2);
	CU_ASSERT(resph->res_cnt == 0);

	TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
	iscsi_put_pdu(pdu);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
add_transfer_task_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task = {};
	struct spdk_iscsi_pdu *pdu, *tmp;
	struct iscsi_bhs_r2t *r2th;
	int rc, count = 0;
	uint32_t buffer_offset, desired_xfer_len;

	g_iscsi.MaxR2TPerConnection = DEFAULT_MAXR2T;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;	/* 1M */
	sess.MaxOutstandingR2T = DEFAULT_MAXR2T;	/* 4 */

	conn.sess = &sess;
	TAILQ_INIT(&conn.queued_r2t_tasks);
	TAILQ_INIT(&conn.active_r2t_tasks);

	pdu = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	pdu->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;	/* 64K */
	task.scsi.transfer_len = 16 * 1024 * 1024;
	iscsi_task_set_pdu(&task, pdu);

	/* The following tests if the task is queued because R2T tasks are full. */
	conn.pending_r2t = DEFAULT_MAXR2T;

	rc = add_transfer_task(&conn, &task);

	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_FIRST(&conn.queued_r2t_tasks) == &task);

	TAILQ_REMOVE(&conn.queued_r2t_tasks, &task, link);
	CU_ASSERT(TAILQ_EMPTY(&conn.queued_r2t_tasks));

	/* The following tests if multiple R2Ts are issued. */
	conn.pending_r2t = 0;

	rc = add_transfer_task(&conn, &task);

	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_FIRST(&conn.active_r2t_tasks) == &task);

	TAILQ_REMOVE(&conn.active_r2t_tasks, &task, link);
	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));

	CU_ASSERT(conn.data_out_cnt == 255);
	CU_ASSERT(conn.pending_r2t == 1);
	CU_ASSERT(conn.ttt == 1);

	CU_ASSERT(task.data_out_cnt == 255);
	CU_ASSERT(task.ttt == 1);
	CU_ASSERT(task.outstanding_r2t == sess.MaxOutstandingR2T);
	CU_ASSERT(task.next_r2t_offset ==
		  pdu->data_segment_len + sess.MaxBurstLength * sess.MaxOutstandingR2T);


	while (!TAILQ_EMPTY(&g_write_pdu_list)) {
		tmp = TAILQ_FIRST(&g_write_pdu_list);
		TAILQ_REMOVE(&g_write_pdu_list, tmp, tailq);

		r2th = (struct iscsi_bhs_r2t *)&tmp->bhs;

		buffer_offset = from_be32(&r2th->buffer_offset);
		CU_ASSERT(buffer_offset == pdu->data_segment_len + sess.MaxBurstLength * count);

		desired_xfer_len = from_be32(&r2th->desired_xfer_len);
		CU_ASSERT(desired_xfer_len == sess.MaxBurstLength);

		iscsi_put_pdu(tmp);
		count++;
	}

	CU_ASSERT(count == DEFAULT_MAXR2T);

	iscsi_put_pdu(pdu);
}

static void
get_transfer_task_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, *task;
	struct spdk_iscsi_pdu *pdu1, *pdu2, *pdu;
	int rc;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	sess.MaxOutstandingR2T = 1;

	conn.sess = &sess;
	TAILQ_INIT(&conn.active_r2t_tasks);

	pdu1 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	pdu1->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task1.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	iscsi_task_set_pdu(&task1, pdu1);

	rc = add_transfer_task(&conn, &task1);
	CU_ASSERT(rc == 0);

	pdu2 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	pdu2->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task2.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	iscsi_task_set_pdu(&task2, pdu2);

	rc = add_transfer_task(&conn, &task2);
	CU_ASSERT(rc == 0);

	task = get_transfer_task(&conn, 1);
	CU_ASSERT(task == &task1);

	task = get_transfer_task(&conn, 2);
	CU_ASSERT(task == &task2);

	while (!TAILQ_EMPTY(&conn.active_r2t_tasks)) {
		task = TAILQ_FIRST(&conn.active_r2t_tasks);
		TAILQ_REMOVE(&conn.active_r2t_tasks, task, link);
	}

	while (!TAILQ_EMPTY(&g_write_pdu_list)) {
		pdu = TAILQ_FIRST(&g_write_pdu_list);
		TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
		iscsi_put_pdu(pdu);
	}

	iscsi_put_pdu(pdu2);
	iscsi_put_pdu(pdu1);
}

static void
del_transfer_task_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task *task1, *task2, *task3, *task4, *task5;
	struct spdk_iscsi_pdu *pdu1, *pdu2, *pdu3, *pdu4, *pdu5, *pdu;
	int rc;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	sess.MaxOutstandingR2T = 1;

	conn.sess = &sess;
	TAILQ_INIT(&conn.active_r2t_tasks);
	TAILQ_INIT(&conn.queued_r2t_tasks);

	pdu1 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	pdu1->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;

	task1 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task1 != NULL);

	task1->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	iscsi_task_set_pdu(task1, pdu1);
	task1->tag = 11;

	rc = add_transfer_task(&conn, task1);
	CU_ASSERT(rc == 0);

	pdu2 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	pdu2->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;

	task2 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task2 != NULL);

	task2->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	iscsi_task_set_pdu(task2, pdu2);
	task2->tag = 12;

	rc = add_transfer_task(&conn, task2);
	CU_ASSERT(rc == 0);

	pdu3 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu3 != NULL);

	pdu3->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;

	task3 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task3 != NULL);

	task3->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	iscsi_task_set_pdu(task3, pdu3);
	task3->tag = 13;

	rc = add_transfer_task(&conn, task3);
	CU_ASSERT(rc == 0);

	pdu4 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu4 != NULL);

	pdu4->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;

	task4 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task4 != NULL);

	task4->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	iscsi_task_set_pdu(task4, pdu4);
	task4->tag = 14;

	rc = add_transfer_task(&conn, task4);
	CU_ASSERT(rc == 0);

	pdu5 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu5 != NULL);

	pdu5->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;

	task5 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task5 != NULL);

	task5->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	iscsi_task_set_pdu(task5, pdu5);
	task5->tag = 15;

	rc = add_transfer_task(&conn, task5);
	CU_ASSERT(rc == 0);

	CU_ASSERT(get_transfer_task(&conn, 1) == task1);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);
	iscsi_del_transfer_task(&conn, 11);
	CU_ASSERT(get_transfer_task(&conn, 1) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 5) == task5);

	CU_ASSERT(get_transfer_task(&conn, 2) == task2);
	iscsi_del_transfer_task(&conn, 12);
	CU_ASSERT(get_transfer_task(&conn, 2) == NULL);

	CU_ASSERT(get_transfer_task(&conn, 3) == task3);
	iscsi_del_transfer_task(&conn, 13);
	CU_ASSERT(get_transfer_task(&conn, 3) == NULL);

	CU_ASSERT(get_transfer_task(&conn, 4) == task4);
	iscsi_del_transfer_task(&conn, 14);
	CU_ASSERT(get_transfer_task(&conn, 4) == NULL);

	CU_ASSERT(get_transfer_task(&conn, 5) == task5);
	iscsi_del_transfer_task(&conn, 15);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);

	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));

	while (!TAILQ_EMPTY(&g_write_pdu_list)) {
		pdu = TAILQ_FIRST(&g_write_pdu_list);
		TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
		iscsi_put_pdu(pdu);
	}

	iscsi_put_pdu(pdu5);
	iscsi_put_pdu(pdu4);
	iscsi_put_pdu(pdu3);
	iscsi_put_pdu(pdu2);
	iscsi_put_pdu(pdu1);
}

static void
clear_all_transfer_tasks_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task *task1, *task2, *task3, *task4, *task5, *task6;
	struct spdk_iscsi_pdu *pdu1, *pdu2, *pdu3, *pdu4, *pdu5, *pdu6, *pdu;
	struct spdk_iscsi_pdu *mgmt_pdu1, *mgmt_pdu2;
	struct spdk_scsi_lun lun1 = {}, lun2 = {};
	uint32_t alloc_cmd_sn;
	int rc;

	sess.MaxBurstLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	sess.MaxOutstandingR2T = 1;

	conn.sess = &sess;
	TAILQ_INIT(&conn.active_r2t_tasks);
	TAILQ_INIT(&conn.queued_r2t_tasks);

	alloc_cmd_sn = 10;

	task1 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task1 != NULL);
	pdu1 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	pdu1->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu1->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task1->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task1->scsi.lun = &lun1;
	iscsi_task_set_pdu(task1, pdu1);

	rc = add_transfer_task(&conn, task1);
	CU_ASSERT(rc == 0);

	mgmt_pdu1 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(mgmt_pdu1 != NULL);

	mgmt_pdu1->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	task2 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task2 != NULL);
	pdu2 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	pdu2->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu2->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task2->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task2->scsi.lun = &lun1;
	iscsi_task_set_pdu(task2, pdu2);

	rc = add_transfer_task(&conn, task2);
	CU_ASSERT(rc == 0);

	task3 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task3 != NULL);
	pdu3 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu3 != NULL);

	pdu3->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu3->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task3->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task3->scsi.lun = &lun1;
	iscsi_task_set_pdu(task3, pdu3);

	rc = add_transfer_task(&conn, task3);
	CU_ASSERT(rc == 0);

	task4 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task4 != NULL);
	pdu4 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu4 != NULL);

	pdu4->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu4->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task4->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task4->scsi.lun = &lun2;
	iscsi_task_set_pdu(task4, pdu4);

	rc = add_transfer_task(&conn, task4);
	CU_ASSERT(rc == 0);

	task5 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task5 != NULL);
	pdu5 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu5 != NULL);

	pdu5->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu5->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task5->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task5->scsi.lun = &lun2;
	iscsi_task_set_pdu(task5, pdu5);

	rc = add_transfer_task(&conn, task5);
	CU_ASSERT(rc == 0);

	mgmt_pdu2 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(mgmt_pdu2 != NULL);

	mgmt_pdu2->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	task6 = iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task6 != NULL);
	pdu6 = iscsi_get_pdu(&conn);
	SPDK_CU_ASSERT_FATAL(pdu6 != NULL);

	pdu6->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu6->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task5->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task6->scsi.lun = &lun2;
	iscsi_task_set_pdu(task6, pdu6);

	rc = add_transfer_task(&conn, task6);
	CU_ASSERT(rc == 0);

	CU_ASSERT(conn.ttt == 4);

	CU_ASSERT(get_transfer_task(&conn, 1) == task1);
	CU_ASSERT(get_transfer_task(&conn, 2) == task2);
	CU_ASSERT(get_transfer_task(&conn, 3) == task3);
	CU_ASSERT(get_transfer_task(&conn, 4) == task4);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);

	iscsi_clear_all_transfer_task(&conn, &lun1, mgmt_pdu1);

	CU_ASSERT(!TAILQ_EMPTY(&conn.queued_r2t_tasks));
	CU_ASSERT(get_transfer_task(&conn, 1) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 2) == task2);
	CU_ASSERT(get_transfer_task(&conn, 3) == task3);
	CU_ASSERT(get_transfer_task(&conn, 4) == task4);
	CU_ASSERT(get_transfer_task(&conn, 5) == task5);
	CU_ASSERT(get_transfer_task(&conn, 6) == NULL);

	iscsi_clear_all_transfer_task(&conn, &lun1, NULL);

	CU_ASSERT(TAILQ_EMPTY(&conn.queued_r2t_tasks));
	CU_ASSERT(get_transfer_task(&conn, 1) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 2) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 3) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 4) == task4);
	CU_ASSERT(get_transfer_task(&conn, 5) == task5);
	CU_ASSERT(get_transfer_task(&conn, 6) == task6);

	iscsi_clear_all_transfer_task(&conn, &lun2, mgmt_pdu2);

	CU_ASSERT(get_transfer_task(&conn, 4) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 6) == task6);

	iscsi_clear_all_transfer_task(&conn, NULL, NULL);

	CU_ASSERT(get_transfer_task(&conn, 6) == NULL);

	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));
	while (!TAILQ_EMPTY(&g_write_pdu_list)) {
		pdu = TAILQ_FIRST(&g_write_pdu_list);
		TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
		iscsi_put_pdu(pdu);
	}

	iscsi_put_pdu(mgmt_pdu2);
	iscsi_put_pdu(mgmt_pdu1);
	iscsi_put_pdu(pdu6);
	iscsi_put_pdu(pdu5);
	iscsi_put_pdu(pdu4);
	iscsi_put_pdu(pdu3);
	iscsi_put_pdu(pdu2);
	iscsi_put_pdu(pdu1);
}

static void
build_iovs_test(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct iovec iovs[5] = {};
	uint8_t *data;
	uint32_t mapped_length = 0;
	int rc;

	conn.header_digest = true;
	conn.data_digest = true;

	DSET24(&pdu.bhs.data_segment_len, 512);
	data = calloc(1, 512);
	SPDK_CU_ASSERT_FATAL(data != NULL);
	pdu.data = data;

	pdu.bhs.total_ahs_len = 0;
	pdu.bhs.opcode = ISCSI_OP_SCSI;

	pdu.writev_offset = 0;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 4);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[2].iov_len == 512);
	CU_ASSERT(iovs[3].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[3].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN / 2;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 4);
	CU_ASSERT(iovs[0].iov_base == (void *)((uint8_t *)&pdu.bhs + ISCSI_BHS_LEN / 2));
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN / 2);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[2].iov_len == 512);
	CU_ASSERT(iovs[3].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[3].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN / 2 + ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[1].iov_len == 512);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[2].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN / 2;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)((uint8_t *)pdu.header_digest + ISCSI_DIGEST_LEN / 2));
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN / 2);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[1].iov_len == 512);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[2].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN / 2 + 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[0].iov_len == 512);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN / 2;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)((uint8_t *)pdu.data_digest + ISCSI_DIGEST_LEN / 2));
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN / 2);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN / 2);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN;
	rc = iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 0);
	CU_ASSERT(mapped_length == 0);

	pdu.writev_offset = 0;
	rc = iscsi_build_iovs(&conn, iovs, 1, &pdu, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN);

	rc = iscsi_build_iovs(&conn, iovs, 2, &pdu, &mapped_length);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN);

	rc = iscsi_build_iovs(&conn, iovs, 3, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[2].iov_len == 512);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512);

	rc = iscsi_build_iovs(&conn, iovs, 4, &pdu, &mapped_length);
	CU_ASSERT(rc == 4);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[2].iov_len == 512);
	CU_ASSERT(iovs[3].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[3].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN);

	free(data);
}

static void
build_iovs_with_md_test(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct iovec iovs[6] = {};
	uint8_t *data;
	uint32_t mapped_length = 0;
	int rc;

	conn.header_digest = true;
	conn.data_digest = true;

	DSET24(&pdu.bhs.data_segment_len, 4096 * 2);
	data = calloc(1, (4096 + 128) * 2);
	SPDK_CU_ASSERT_FATAL(data != NULL);
	pdu.data = data;
	pdu.data_buf_len = (4096 + 128) * 2;

	pdu.bhs.total_ahs_len = 0;
	pdu.bhs.opcode = ISCSI_OP_SCSI;

	rc = spdk_dif_ctx_init(&pdu.dif_ctx, 4096 + 128, 128, true, false, SPDK_DIF_TYPE1,
			       0, 0, 0, 0, 0, 0);
	CU_ASSERT(rc == 0);

	pdu.dif_insert_or_strip = true;

	pdu.writev_offset = 0;
	rc = iscsi_build_iovs(&conn, iovs, 6, &pdu, &mapped_length);
	CU_ASSERT(rc == 5);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[2].iov_len == 4096);
	CU_ASSERT(iovs[3].iov_base == (void *)(pdu.data + 4096 + 128));
	CU_ASSERT(iovs[3].iov_len == 4096);
	CU_ASSERT(iovs[4].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[4].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 4096 * 2 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 2048;
	rc = iscsi_build_iovs(&conn, iovs, 6, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)(pdu.data + 2048));
	CU_ASSERT(iovs[0].iov_len == 2048);
	CU_ASSERT(iovs[1].iov_base == (void *)(pdu.data + 4096 + 128));
	CU_ASSERT(iovs[1].iov_len == 4096);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[2].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == 2048 + 4096 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 4096 * 2;
	rc = iscsi_build_iovs(&conn, iovs, 6, &pdu, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN);

	pdu.writev_offset = 0;
	rc = iscsi_build_iovs(&conn, iovs, 3, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[2].iov_len == 4096);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 4096);

	free(data);
}

static void
check_iscsi_reject(struct spdk_iscsi_pdu *pdu, uint8_t reason)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_reject *reject_bhs;

	CU_ASSERT(pdu->is_rejected == true);
	rsp_pdu = TAILQ_FIRST(&g_write_pdu_list);
	CU_ASSERT(rsp_pdu != NULL);
	reject_bhs = (struct iscsi_bhs_reject *)&rsp_pdu->bhs;
	CU_ASSERT(reject_bhs->reason == reason);

	TAILQ_REMOVE(&g_write_pdu_list, rsp_pdu, tailq);
	iscsi_put_pdu(rsp_pdu);
	pdu->is_rejected = false;
}

static void
check_login_response(uint8_t status_class, uint8_t status_detail)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_login_rsp *login_rsph;

	rsp_pdu = TAILQ_FIRST(&g_write_pdu_list);
	CU_ASSERT(rsp_pdu != NULL);
	login_rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	CU_ASSERT(login_rsph->status_class == status_class);
	CU_ASSERT(login_rsph->status_detail == status_detail);

	TAILQ_REMOVE(&g_write_pdu_list, rsp_pdu, tailq);
	iscsi_put_pdu(rsp_pdu);
}

static void
pdu_hdr_op_login_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct iscsi_bhs_login_req *login_reqh;
	int rc;

	login_reqh = (struct iscsi_bhs_login_req *)&pdu.bhs;

	/* Case 1 - On discovery session, target only accepts text requests with the
	 * SendTargets key and logout request with reason "close the session".
	 */
	sess.session_type = SESSION_TYPE_DISCOVERY;
	conn.full_feature = true;
	conn.sess = &sess;

	rc = iscsi_pdu_hdr_op_login(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 2 - Data segment length is limited to be not more than 8KB, the default
	 * FirstBurstLength, for login request.
	 */
	sess.session_type = SESSION_TYPE_INVALID;
	conn.full_feature = false;
	conn.sess = NULL;
	pdu.data_segment_len = SPDK_ISCSI_FIRST_BURST_LENGTH + 1;

	rc = iscsi_pdu_hdr_op_login(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 3 - PDU pool is empty */
	pdu.data_segment_len = SPDK_ISCSI_FIRST_BURST_LENGTH;
	g_pdu_pool_is_empty = true;

	rc = iscsi_pdu_hdr_op_login(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 4 - A login request with the C bit set to 1 must have the T bit set to 0. */
	g_pdu_pool_is_empty = false;
	login_reqh->flags |= ISCSI_LOGIN_TRANSIT;
	login_reqh->flags |= ISCSI_LOGIN_CONTINUE;

	rc = iscsi_pdu_hdr_op_login(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_login_response(ISCSI_CLASS_INITIATOR_ERROR, ISCSI_LOGIN_INITIATOR_ERROR);

	/* Case 5 - Both version-min and version-max must be set to 0x00. */
	login_reqh->flags = 0;
	login_reqh->version_min = ISCSI_VERSION + 1;

	rc = iscsi_pdu_hdr_op_login(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_login_response(ISCSI_CLASS_INITIATOR_ERROR, ISCSI_LOGIN_UNSUPPORTED_VERSION);

	/* Case 6 - T bit is set to 1 correctly but invalid stage code is set to NSG. */
	login_reqh->version_min = ISCSI_VERSION;
	login_reqh->flags |= ISCSI_LOGIN_TRANSIT;
	login_reqh->flags |= ISCSI_NSG_RESERVED_CODE;

	rc = iscsi_pdu_hdr_op_login(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_login_response(ISCSI_CLASS_INITIATOR_ERROR, ISCSI_LOGIN_INITIATOR_ERROR);

	/* Case 7 - Login request is correct.  Login response is initialized and set to
	 * the current connection.
	 */
	login_reqh->flags = 0;

	rc = iscsi_pdu_hdr_op_login(&conn, &pdu);
	CU_ASSERT(rc == 0);
	CU_ASSERT(conn.login_rsp_pdu != NULL);

	iscsi_put_pdu(conn.login_rsp_pdu);
}

static void
pdu_hdr_op_text_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct iscsi_bhs_text_req *text_reqh;
	int rc;

	text_reqh = (struct iscsi_bhs_text_req *)&pdu.bhs;

	conn.sess = &sess;

	/* Case 1 - Data segment length for text request must not be more than
	 * FirstBurstLength plus extra space to account for digests.
	 */
	pdu.data_segment_len = iscsi_get_max_immediate_data_size() + 1;

	rc = iscsi_pdu_hdr_op_text(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 2 - A text request with the C bit set to 1 must have the F bit set to 0. */
	pdu.data_segment_len = iscsi_get_max_immediate_data_size();
	text_reqh->flags |= ISCSI_FLAG_FINAL;
	text_reqh->flags |= ISCSI_TEXT_CONTINUE;

	rc = iscsi_pdu_hdr_op_text(&conn, &pdu);
	CU_ASSERT(rc == -1);

	/* Case 3 - ExpStatSN of the text request is expected to match StatSN of the current
	 * connection.  But StarPort iSCSI initiator didn't follow the expectation.  In this
	 * case we overwrite StatSN by ExpStatSN and processes the request as correct.
	 */
	text_reqh->flags = 0;
	to_be32(&text_reqh->exp_stat_sn, 1234);
	to_be32(&conn.StatSN, 4321);

	rc = iscsi_pdu_hdr_op_text(&conn, &pdu);
	CU_ASSERT(rc == 0);
	CU_ASSERT(conn.StatSN == 1234);

	/* Case 4 - Text request is the first in the sequence of text requests and responses,
	 * and so its ITT is hold to the current connection.
	 */
	sess.current_text_itt = 0xffffffffU;
	to_be32(&text_reqh->itt, 5678);

	rc = iscsi_pdu_hdr_op_text(&conn, &pdu);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sess.current_text_itt == 5678);

	/* Case 5 - If text request is sent as part of a sequence of text requests and responses,
	 * its ITT must be the same for all the text requests.  But it was not.  */
	sess.current_text_itt = 5679;

	rc = iscsi_pdu_hdr_op_text(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 6 - Different from case 5, its ITT matches the value saved in the connection. */
	text_reqh->flags = 0;
	sess.current_text_itt = 5678;

	rc = iscsi_pdu_hdr_op_text(&conn, &pdu);
	CU_ASSERT(rc == 0);
}

static void
check_logout_response(uint8_t response, uint32_t stat_sn, uint32_t exp_cmd_sn,
		      uint32_t max_cmd_sn)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_logout_resp *logout_rsph;

	rsp_pdu = TAILQ_FIRST(&g_write_pdu_list);
	CU_ASSERT(rsp_pdu != NULL);
	logout_rsph = (struct iscsi_bhs_logout_resp *)&rsp_pdu->bhs;
	CU_ASSERT(logout_rsph->response == response);
	CU_ASSERT(from_be32(&logout_rsph->stat_sn) == stat_sn);
	CU_ASSERT(from_be32(&logout_rsph->exp_cmd_sn) == exp_cmd_sn);
	CU_ASSERT(from_be32(&logout_rsph->max_cmd_sn) == max_cmd_sn);

	TAILQ_REMOVE(&g_write_pdu_list, rsp_pdu, tailq);
	iscsi_put_pdu(rsp_pdu);
}

static void
pdu_hdr_op_logout_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct iscsi_bhs_logout_req *logout_reqh;
	int rc;

	logout_reqh = (struct iscsi_bhs_logout_req *)&pdu.bhs;

	/* Case 1 - Target can accept logout request only with the reason "close the session"
	 * on discovery session.
	 */
	logout_reqh->reason = 1;
	conn.sess = &sess;
	sess.session_type = SESSION_TYPE_DISCOVERY;

	rc = iscsi_pdu_hdr_op_logout(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 2 - Session is not established yet but connection was closed successfully. */
	conn.sess = NULL;
	conn.StatSN = 1234;
	to_be32(&logout_reqh->exp_stat_sn, 1234);
	pdu.cmd_sn = 5678;

	rc = iscsi_pdu_hdr_op_logout(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_logout_response(0, 1234, 5678, 5678);
	CU_ASSERT(conn.StatSN == 1235);

	/* Case 3 - Session type is normal but CID was not found. Hence connection or session
	 * was not closed.
	 */
	sess.session_type = SESSION_TYPE_NORMAL;
	sess.ExpCmdSN = 5679;
	sess.connections = 1;
	conn.sess = &sess;
	conn.id = 1;

	rc = iscsi_pdu_hdr_op_logout(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_logout_response(1, 1235, 5679, 1);
	CU_ASSERT(conn.StatSN == 1236);
	CU_ASSERT(sess.MaxCmdSN == 1);

	/* Case 4 - Session type is normal and CID was found.  Connection or session was closed
	 * successfully.
	 */
	to_be16(&logout_reqh->cid, 1);

	rc = iscsi_pdu_hdr_op_logout(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_logout_response(0, 1236, 5679, 2);
	CU_ASSERT(conn.StatSN == 1237);
	CU_ASSERT(sess.MaxCmdSN == 2);

	/* Case 5 - PDU pool is empty. */
	g_pdu_pool_is_empty = true;

	rc = iscsi_pdu_hdr_op_logout(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	g_pdu_pool_is_empty = false;
}

static void
check_scsi_task(struct spdk_iscsi_pdu *pdu, enum spdk_scsi_data_dir dir)
{
	struct spdk_iscsi_task *task;

	task = pdu->task;
	CU_ASSERT(task != NULL);
	CU_ASSERT(task->pdu == pdu);
	CU_ASSERT(task->scsi.dxfer_dir == (uint32_t)dir);

	iscsi_task_put(task);
	pdu->task = NULL;
}

static void
pdu_hdr_op_scsi_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct iscsi_bhs_scsi_req *scsi_reqh;
	int rc;

	scsi_reqh = (struct iscsi_bhs_scsi_req *)&pdu.bhs;

	conn.sess = &sess;
	conn.dev = &dev;

	/* Case 1 - SCSI command is acceptable only on normal session. */
	sess.session_type = SESSION_TYPE_DISCOVERY;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 2 - Task pool is empty. */
	g_task_pool_is_empty = true;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	g_task_pool_is_empty = false;

	/* Case 3 - bidirectional operations (both R and W flags are set to 1) are not supported. */
	sess.session_type = SESSION_TYPE_NORMAL;
	scsi_reqh->read_bit = 1;
	scsi_reqh->write_bit = 1;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 4 - LUN is hot-removed, and return immediately. */
	scsi_reqh->write_bit = 0;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pdu.task == NULL);

	/* Case 5 - SCSI read command PDU is correct, and the configured iSCSI task is set to the PDU. */
	dev.lun[0] = &lun;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_scsi_task(&pdu, SPDK_SCSI_DIR_FROM_DEV);

	/* Case 6 - For SCSI write command PDU, its data segment length must not be more than
	 * FirstBurstLength plus extra space to account for digests.
	 */
	scsi_reqh->read_bit = 0;
	scsi_reqh->write_bit = 1;
	pdu.data_segment_len = iscsi_get_max_immediate_data_size() + 1;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 7 - For SCSI write command PDU, its data segment length must not be more than
	 * Expected Data Transfer Length (EDTL).
	 */
	pdu.data_segment_len = iscsi_get_max_immediate_data_size();
	to_be32(&scsi_reqh->expected_data_xfer_len, pdu.data_segment_len - 1);

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 8 - If ImmediateData is not enabled for the session, SCSI write command PDU
	 * cannot have data segment.
	 */
	to_be32(&scsi_reqh->expected_data_xfer_len, pdu.data_segment_len);

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 9 - For SCSI write command PDU, its data segment length must not be more
	 * than FirstBurstLength.
	 */
	sess.ImmediateData = true;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 10 - SCSI write command PDU is correct, and the configured iSCSI task is set to the PDU. */
	sess.FirstBurstLength = pdu.data_segment_len;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_scsi_task(&pdu, SPDK_SCSI_DIR_TO_DEV);

	/* Case 11 - R and W must not both be 0 when EDTL is not 0. */
	scsi_reqh->write_bit = 0;

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_INVALID_PDU_FIELD);

	/* Case 11 - R and W are both 0 and EDTL is also 0, and hence SCSI command PDU is accepted. */
	to_be32(&scsi_reqh->expected_data_xfer_len, 0);

	rc = iscsi_pdu_hdr_op_scsi(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_scsi_task(&pdu, SPDK_SCSI_DIR_NONE);
}

static void
check_iscsi_task_mgmt_response(uint8_t response, uint32_t task_tag, uint32_t stat_sn,
			       uint32_t exp_cmd_sn, uint32_t max_cmd_sn)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_task_resp *rsph;

	rsp_pdu = TAILQ_FIRST(&g_write_pdu_list);
	CU_ASSERT(rsp_pdu != NULL);
	rsph = (struct iscsi_bhs_task_resp *)&rsp_pdu->bhs;
	CU_ASSERT(rsph->response == response);
	CU_ASSERT(from_be32(&rsph->itt) == task_tag);
	CU_ASSERT(from_be32(&rsph->exp_cmd_sn) == exp_cmd_sn);
	CU_ASSERT(from_be32(&rsph->max_cmd_sn) == max_cmd_sn);

	TAILQ_REMOVE(&g_write_pdu_list, rsp_pdu, tailq);
	iscsi_put_pdu(rsp_pdu);
}

static void
pdu_hdr_op_task_mgmt_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct iscsi_bhs_task_req *task_reqh;
	int rc;

	/* TBD: This test covers only error paths before creating iSCSI task for now.
	 * Testing iSCSI task creation in iscsi_pdu_hdr_op_task() by UT is not simple
	 * and do it separately later.
	 */

	task_reqh = (struct iscsi_bhs_task_req *)&pdu.bhs;

	conn.sess = &sess;
	conn.dev = &dev;

	/* Case 1 - Task Management Function request PDU is acceptable only on normal session. */
	sess.session_type = SESSION_TYPE_DISCOVERY;

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 2 - LUN is hot removed.  "LUN does not exist" response is sent. */
	sess.session_type = SESSION_TYPE_NORMAL;
	task_reqh->immediate = 0;
	to_be32(&task_reqh->itt, 1234);

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_task_mgmt_response(ISCSI_TASK_FUNC_RESP_LUN_NOT_EXIST, 1234, 0, 0, 1);

	/* Case 3 - Unassigned function is specified.  "Function rejected" response is sent. */
	dev.lun[0] = &lun;
	task_reqh->flags = 0;

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_task_mgmt_response(ISCSI_TASK_FUNC_REJECTED, 1234, 0, 0, 2);

	/* Case 4 - CLEAR TASK SET is not supported.  "Task management function not supported"
	 * response is sent.
	 */
	task_reqh->flags = ISCSI_TASK_FUNC_CLEAR_TASK_SET;

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_task_mgmt_response(ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED, 1234, 0, 0, 3);

	/* Case 5 - CLEAR ACA is not supported.  "Task management function not supported" is sent. */
	task_reqh->flags = ISCSI_TASK_FUNC_CLEAR_ACA;

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_task_mgmt_response(ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED, 1234, 0, 0, 4);

	/* Case 6 - TARGET WARM RESET is not supported.  "Task management function not supported
	 * is sent.
	 */
	task_reqh->flags = ISCSI_TASK_FUNC_TARGET_WARM_RESET;

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_task_mgmt_response(ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED, 1234, 0, 0, 5);

	/* Case 7 - TARGET COLD RESET is not supported. "Task management function not supported
	 * is sent.
	 */
	task_reqh->flags = ISCSI_TASK_FUNC_TARGET_COLD_RESET;

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_task_mgmt_response(ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED, 1234, 0, 0, 6);

	/* Case 8 - TASK REASSIGN is not supported. "Task management function not supported" is sent. */
	task_reqh->flags = ISCSI_TASK_FUNC_TASK_REASSIGN;

	rc = iscsi_pdu_hdr_op_task(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_task_mgmt_response(ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED, 1234, 0, 0, 7);
}

static void
pdu_hdr_op_nopout_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct iscsi_bhs_nop_out *nopout_reqh;
	int rc;

	nopout_reqh = (struct iscsi_bhs_nop_out *)&pdu.bhs;

	conn.sess = &sess;

	/* Case 1 - NOP-Out PDU is acceptable only on normal session. */
	sess.session_type = SESSION_TYPE_DISCOVERY;

	rc = iscsi_pdu_hdr_op_nopout(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 2 - The length of the reflected ping data is limited to MaxRecvDataSegmentLength. */
	sess.session_type = SESSION_TYPE_NORMAL;
	pdu.data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH + 1;

	rc = iscsi_pdu_hdr_op_nopout(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 3 - If Initiator Task Tag contains 0xffffffff, the I bit must be set
	 * to 1 and Target Transfer Tag should be copied from NOP-In PDU.  This case
	 * satisfies the former but doesn't satisfy the latter, but ignore the error
	 * for now.
	 */
	pdu.data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	conn.id = 1234;
	to_be32(&nopout_reqh->ttt, 1235);
	to_be32(&nopout_reqh->itt, 0xffffffffU);
	nopout_reqh->immediate = 1;

	rc = iscsi_pdu_hdr_op_nopout(&conn, &pdu);
	CU_ASSERT(rc == 0);

	/* Case 4 - This case doesn't satisfy the above former. This error is not ignored. */
	nopout_reqh->immediate = 0;

	rc = iscsi_pdu_hdr_op_nopout(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);
}

static void
check_iscsi_r2t(struct spdk_iscsi_task *task, uint32_t len)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_r2t *rsph;

	rsp_pdu = TAILQ_FIRST(&g_write_pdu_list);
	CU_ASSERT(rsp_pdu != NULL);
	rsph = (struct iscsi_bhs_r2t *)&rsp_pdu->bhs;
	CU_ASSERT(rsph->opcode == ISCSI_OP_R2T);
	CU_ASSERT(from_be64(&rsph->lun) == spdk_scsi_lun_id_int_to_fmt(task->lun_id));
	CU_ASSERT(from_be32(&rsph->buffer_offset) == task->next_r2t_offset);
	CU_ASSERT(from_be32(&rsph->desired_xfer_len) == len);

	TAILQ_REMOVE(&g_write_pdu_list, rsp_pdu, tailq);
	iscsi_put_pdu(rsp_pdu);
}

static void
pdu_hdr_op_data_test(void)
{
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu = {};
	struct spdk_iscsi_task primary = {};
	struct spdk_scsi_dev dev = {};
	struct spdk_scsi_lun lun = {};
	struct iscsi_bhs_data_out *data_reqh;
	int rc;

	data_reqh = (struct iscsi_bhs_data_out *)&pdu.bhs;

	conn.sess = &sess;
	conn.dev = &dev;
	TAILQ_INIT(&conn.active_r2t_tasks);

	/* Case 1 - SCSI Data-Out PDU is acceptable only on normal session. */
	sess.session_type = SESSION_TYPE_DISCOVERY;

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 2 - Data segment length must not be more than MaxRecvDataSegmentLength. */
	sess.session_type = SESSION_TYPE_NORMAL;
	pdu.data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH + 1;

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 3 - R2T task whose Target Transfer Tag matches is not found. */
	pdu.data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_INVALID_PDU_FIELD);

	/* Case 4 - R2T task whose Target Transfer Tag matches is found but data segment length
	 * is more than Desired Data Transfer Length of the R2T.
	 */
	primary.desired_data_transfer_length = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH - 1;
	conn.pending_r2t = 1;
	TAILQ_INSERT_TAIL(&conn.active_r2t_tasks, &primary, link);

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 5 - Initiator task tag doesn't match tag of R2T task. */
	primary.desired_data_transfer_length = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	to_be32(&data_reqh->itt, 1);

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_INVALID_PDU_FIELD);

	/* Case 6 - DataSN doesn't match the Data-Out PDU number within the current
	 * output sequence.
	 */
	to_be32(&data_reqh->itt, 0);
	to_be32(&data_reqh->data_sn, 1);

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == 0);
	check_iscsi_reject(&pdu, ISCSI_REASON_PROTOCOL_ERROR);

	/* Case 7 - Output sequence must be in increasing buffer offset and must not
	 * be overlaid but they are not satisfied.
	 */
	to_be32(&data_reqh->data_sn, 0);
	to_be32(&data_reqh->buffer_offset, 4096);

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 8 - Data segment length must not exceed MaxBurstLength. */
	to_be32(&data_reqh->buffer_offset, 0);
	sess.MaxBurstLength = pdu.data_segment_len - 1;

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	/* Case 9 - LUN is hot removed. */
	sess.MaxBurstLength = pdu.data_segment_len * 4;
	to_be32(&data_reqh->data_sn, primary.r2t_datasn);
	to_be32(&data_reqh->buffer_offset, primary.next_expected_r2t_offset);

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pdu.task == NULL);

	/* Case 10 - SCSI Data-Out PDU is correct and processed. Created task is held
	 * to the PDU, but its F bit is 0 and hence R2T is not sent.
	 */
	dev.lun[0] = &lun;
	to_be32(&data_reqh->data_sn, primary.r2t_datasn);
	to_be32(&data_reqh->buffer_offset, primary.next_expected_r2t_offset);

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pdu.task != NULL);
	iscsi_task_put(pdu.task);
	pdu.task = NULL;

	/* Case 11 - SCSI Data-Out PDU is correct and processed. Created task is held
	 * to the PDU, and Its F bit is 1 and hence R2T is sent.
	 */
	data_reqh->flags |= ISCSI_FLAG_FINAL;
	to_be32(&data_reqh->data_sn, primary.r2t_datasn);
	to_be32(&data_reqh->buffer_offset, primary.next_expected_r2t_offset);
	primary.scsi.transfer_len = pdu.data_segment_len * 5;

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pdu.task != NULL);
	check_iscsi_r2t(pdu.task, pdu.data_segment_len * 4);
	iscsi_task_put(pdu.task);

	/* Case 12 - Task pool is empty. */
	to_be32(&data_reqh->data_sn, primary.r2t_datasn);
	to_be32(&data_reqh->buffer_offset, primary.next_expected_r2t_offset);
	g_task_pool_is_empty = true;

	rc = iscsi_pdu_hdr_op_data(&conn, &pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	g_task_pool_is_empty = false;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("iscsi_suite", NULL, NULL);

	CU_ADD_TEST(suite, op_login_check_target_test);
	CU_ADD_TEST(suite, op_login_session_normal_test);
	CU_ADD_TEST(suite, maxburstlength_test);
	CU_ADD_TEST(suite, underflow_for_read_transfer_test);
	CU_ADD_TEST(suite, underflow_for_zero_read_transfer_test);
	CU_ADD_TEST(suite, underflow_for_request_sense_test);
	CU_ADD_TEST(suite, underflow_for_check_condition_test);
	CU_ADD_TEST(suite, add_transfer_task_test);
	CU_ADD_TEST(suite, get_transfer_task_test);
	CU_ADD_TEST(suite, del_transfer_task_test);
	CU_ADD_TEST(suite, clear_all_transfer_tasks_test);
	CU_ADD_TEST(suite, build_iovs_test);
	CU_ADD_TEST(suite, build_iovs_with_md_test);
	CU_ADD_TEST(suite, pdu_hdr_op_login_test);
	CU_ADD_TEST(suite, pdu_hdr_op_text_test);
	CU_ADD_TEST(suite, pdu_hdr_op_logout_test);
	CU_ADD_TEST(suite, pdu_hdr_op_scsi_test);
	CU_ADD_TEST(suite, pdu_hdr_op_task_mgmt_test);
	CU_ADD_TEST(suite, pdu_hdr_op_nopout_test);
	CU_ADD_TEST(suite, pdu_hdr_op_data_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
