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
#include "iscsi/acceptor.h"
#include "iscsi/portal_grp.h"
#include "scsi/scsi_internal.h"
#include "common/lib/test_env.c"

#include "spdk_internal/mock.h"

#define UT_TARGET_NAME1		"iqn.2017-11.spdk.io:t0001"
#define UT_TARGET_NAME2		"iqn.2017-11.spdk.io:t0002"
#define UT_INITIATOR_NAME1	"iqn.2017-11.spdk.io:i0001"
#define UT_INITIATOR_NAME2	"iqn.2017-11.spdk.io:i0002"

struct spdk_iscsi_tgt_node *
spdk_iscsi_find_tgt_node(const char *target_name)
{
	if (strcasecmp(target_name, UT_TARGET_NAME1) == 0) {
		return (struct spdk_iscsi_tgt_node *)1;
	} else {
		return NULL;
	}
}

bool
spdk_iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
			   struct spdk_iscsi_tgt_node *target,
			   const char *iqn, const char *addr)
{
	if (strcasecmp(conn->initiator_name, UT_INITIATOR_NAME1) == 0) {
		return true;
	} else {
		return false;
	}
}

DEFINE_STUB(spdk_iscsi_send_tgts, int,
	    (struct spdk_iscsi_conn *conn, const char *iiqn, const char *iaddr,
	     const char *tiqn, uint8_t *data, int alloc_len, int data_len),
	    0);

DEFINE_STUB_V(spdk_iscsi_portal_grp_close_all, (void));

DEFINE_STUB_V(spdk_iscsi_conn_migration, (struct spdk_iscsi_conn *conn));

DEFINE_STUB_V(spdk_iscsi_conn_free_pdu,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu));

DEFINE_STUB(spdk_iscsi_chap_get_authinfo, int,
	    (struct iscsi_chap_auth *auth, const char *authuser, int ag_tag),
	    0);

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

static void
op_login_check_target_test(void)
{
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_pdu rsp_pdu;
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
maxburstlength_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_scsi_dev dev;
	struct spdk_scsi_lun lun;
	struct spdk_iscsi_pdu *req_pdu, *data_out_pdu, *r2t_pdu;
	struct iscsi_bhs_scsi_req *req;
	struct iscsi_bhs_r2t *r2t;
	struct iscsi_bhs_data_out *data_out;
	struct spdk_iscsi_pdu *response_pdu;
	int rc;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&dev, 0, sizeof(dev));
	memset(&lun, 0, sizeof(lun));

	req_pdu = spdk_get_pdu();
	data_out_pdu = spdk_get_pdu();

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

	TAILQ_INIT(&g_write_pdu_list);

	req_pdu->bhs.opcode = ISCSI_OP_SCSI;
	req_pdu->data_segment_len = 0;

	req = (struct iscsi_bhs_scsi_req *)&req_pdu->bhs;

	to_be32(&req->cmd_sn, 0);
	to_be32(&req->expected_data_xfer_len, 1028);
	to_be32(&req->itt, 0x1234);
	req->write_bit = 1;
	req->final_bit = 1;

	rc = spdk_iscsi_execute(&conn, req_pdu);
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

	rc = spdk_iscsi_execute(&conn, data_out_pdu);
	CU_ASSERT(rc == SPDK_ISCSI_CONNECTION_FATAL);

	SPDK_CU_ASSERT_FATAL(response_pdu->task != NULL);
	spdk_iscsi_task_disassociate_pdu(response_pdu->task);
	spdk_iscsi_task_put(response_pdu->task);
	spdk_put_pdu(response_pdu);

	r2t_pdu = TAILQ_FIRST(&g_write_pdu_list);
	CU_ASSERT(r2t_pdu != NULL);
	TAILQ_REMOVE(&g_write_pdu_list, r2t_pdu, tailq);
	spdk_put_pdu(r2t_pdu);

	spdk_put_pdu(data_out_pdu);
	spdk_put_pdu(req_pdu);
}

static void
underflow_for_read_transfer_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task task;
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_data_in *datah;
	uint32_t residual_count = 0;

	TAILQ_INIT(&g_write_pdu_list);

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&task, 0, sizeof(task));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	pdu = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
	scsi_req->read_bit = 1;

	spdk_iscsi_task_set_pdu(&task, pdu);
	task.parent = NULL;

	task.scsi.iovs = &task.scsi.iov;
	task.scsi.iovcnt = 1;
	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;
	task.scsi.data_transferred = 256;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;

	spdk_iscsi_task_response(&conn, &task);
	spdk_put_pdu(pdu);

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
	spdk_put_pdu(pdu);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
underflow_for_zero_read_transfer_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task task;
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_scsi_resp *resph;
	uint32_t residual_count = 0, data_segment_len;

	TAILQ_INIT(&g_write_pdu_list);

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&task, 0, sizeof(task));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	pdu = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
	scsi_req->read_bit = 1;

	spdk_iscsi_task_set_pdu(&task, pdu);
	task.parent = NULL;

	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;
	task.scsi.data_transferred = 0;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;

	spdk_iscsi_task_response(&conn, &task);
	spdk_put_pdu(pdu);

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
	spdk_put_pdu(pdu);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
underflow_for_request_sense_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task task;
	struct spdk_iscsi_pdu *pdu1, *pdu2;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_data_in *datah;
	struct iscsi_bhs_scsi_resp *resph;
	uint32_t residual_count = 0, data_segment_len;

	TAILQ_INIT(&g_write_pdu_list);

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&task, 0, sizeof(task));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	pdu1 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu1->bhs;
	scsi_req->read_bit = 1;

	spdk_iscsi_task_set_pdu(&task, pdu1);
	task.parent = NULL;

	task.scsi.iovs = &task.scsi.iov;
	task.scsi.iovcnt = 1;
	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;

	task.scsi.sense_data_len = 18;
	task.scsi.data_transferred = 18;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;

	spdk_iscsi_task_response(&conn, &task);
	spdk_put_pdu(pdu1);

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
	spdk_put_pdu(pdu1);

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
	spdk_put_pdu(pdu2);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
underflow_for_check_condition_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task task;
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct iscsi_bhs_scsi_resp *resph;
	uint32_t data_segment_len;

	TAILQ_INIT(&g_write_pdu_list);

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&task, 0, sizeof(task));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;

	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 8192;

	pdu = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
	scsi_req->read_bit = 1;

	spdk_iscsi_task_set_pdu(&task, pdu);
	task.parent = NULL;

	task.scsi.iovs = &task.scsi.iov;
	task.scsi.iovcnt = 1;
	task.scsi.length = 512;
	task.scsi.transfer_len = 512;
	task.bytes_completed = 512;

	task.scsi.sense_data_len = 18;
	task.scsi.data_transferred = 18;
	task.scsi.status = SPDK_SCSI_STATUS_CHECK_CONDITION;

	spdk_iscsi_task_response(&conn, &task);
	spdk_put_pdu(pdu);

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
	spdk_put_pdu(pdu);

	CU_ASSERT(TAILQ_EMPTY(&g_write_pdu_list));
}

static void
add_transfer_task_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task task;
	struct spdk_iscsi_pdu *pdu, *tmp;
	struct iscsi_bhs_r2t *r2th;
	int rc, count = 0;
	uint32_t buffer_offset, desired_xfer_len;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&task, 0, sizeof(task));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;	/* 1M */
	sess.MaxOutstandingR2T = DEFAULT_MAXR2T;	/* 4 */

	conn.sess = &sess;
	TAILQ_INIT(&conn.queued_r2t_tasks);
	TAILQ_INIT(&conn.active_r2t_tasks);

	pdu = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu != NULL);

	pdu->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;	/* 64K */
	task.scsi.transfer_len = 16 * 1024 * 1024;
	spdk_iscsi_task_set_pdu(&task, pdu);

	/* The following tests if the task is queued because R2T tasks are full. */
	conn.pending_r2t = DEFAULT_MAXR2T;

	rc = add_transfer_task(&conn, &task);

	CU_ASSERT(rc == SPDK_SUCCESS);
	CU_ASSERT(TAILQ_FIRST(&conn.queued_r2t_tasks) == &task);

	TAILQ_REMOVE(&conn.queued_r2t_tasks, &task, link);
	CU_ASSERT(TAILQ_EMPTY(&conn.queued_r2t_tasks));

	/* The following tests if multiple R2Ts are issued. */
	conn.pending_r2t = 0;

	rc = add_transfer_task(&conn, &task);

	CU_ASSERT(rc == SPDK_SUCCESS);
	CU_ASSERT(TAILQ_FIRST(&conn.active_r2t_tasks) == &task);

	TAILQ_REMOVE(&conn.active_r2t_tasks, &task, link);
	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));

	CU_ASSERT(conn.data_out_cnt == 255);
	CU_ASSERT(conn.pending_r2t == 1);
	CU_ASSERT(conn.outstanding_r2t_tasks[0] == &task);
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

		spdk_put_pdu(tmp);
		count++;
	}

	CU_ASSERT(count == DEFAULT_MAXR2T);

	spdk_put_pdu(pdu);
}

static void
get_transfer_task_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task task1, task2, *task;
	struct spdk_iscsi_pdu *pdu1, *pdu2, *pdu;
	int rc;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&task1, 0, sizeof(task1));
	memset(&task2, 0, sizeof(task2));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	sess.MaxOutstandingR2T = 1;

	conn.sess = &sess;
	TAILQ_INIT(&conn.active_r2t_tasks);

	pdu1 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	pdu1->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task1.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	spdk_iscsi_task_set_pdu(&task1, pdu1);

	rc = add_transfer_task(&conn, &task1);
	CU_ASSERT(rc == SPDK_SUCCESS);

	pdu2 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	pdu2->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task2.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	spdk_iscsi_task_set_pdu(&task2, pdu2);

	rc = add_transfer_task(&conn, &task2);
	CU_ASSERT(rc == SPDK_SUCCESS);

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
		spdk_put_pdu(pdu);
	}

	spdk_put_pdu(pdu2);
	spdk_put_pdu(pdu1);
}

static void
del_transfer_task_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task task1, task2, task3, task4, task5, *task;
	struct spdk_iscsi_pdu *pdu1, *pdu2, *pdu3, *pdu4, *pdu5, *pdu;
	int rc;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&task1, 0, sizeof(task1));
	memset(&task2, 0, sizeof(task2));
	memset(&task3, 0, sizeof(task3));
	memset(&task4, 0, sizeof(task4));
	memset(&task5, 0, sizeof(task5));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	sess.MaxOutstandingR2T = 1;

	conn.sess = &sess;
	TAILQ_INIT(&conn.active_r2t_tasks);
	TAILQ_INIT(&conn.queued_r2t_tasks);

	pdu1 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	pdu1->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task1.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	spdk_iscsi_task_set_pdu(&task1, pdu1);
	task1.tag = 11;

	rc = add_transfer_task(&conn, &task1);
	CU_ASSERT(rc == SPDK_SUCCESS);

	pdu2 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	pdu2->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task2.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	spdk_iscsi_task_set_pdu(&task2, pdu2);
	task2.tag = 12;

	rc = add_transfer_task(&conn, &task2);
	CU_ASSERT(rc == SPDK_SUCCESS);

	pdu3 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu3 != NULL);

	pdu3->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task3.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	spdk_iscsi_task_set_pdu(&task3, pdu3);
	task3.tag = 13;

	rc = add_transfer_task(&conn, &task3);
	CU_ASSERT(rc == SPDK_SUCCESS);

	pdu4 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu4 != NULL);

	pdu4->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task4.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	spdk_iscsi_task_set_pdu(&task4, pdu4);
	task4.tag = 14;

	rc = add_transfer_task(&conn, &task4);
	CU_ASSERT(rc == SPDK_SUCCESS);

	pdu5 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu5 != NULL);

	pdu5->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task5.scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	spdk_iscsi_task_set_pdu(&task5, pdu5);
	task5.tag = 15;

	rc = add_transfer_task(&conn, &task5);
	CU_ASSERT(rc == SPDK_SUCCESS);

	CU_ASSERT(get_transfer_task(&conn, 1) == &task1);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);
	spdk_del_transfer_task(&conn, 11);
	CU_ASSERT(get_transfer_task(&conn, 1) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 5) == &task5);

	CU_ASSERT(get_transfer_task(&conn, 2) == &task2);
	spdk_del_transfer_task(&conn, 12);
	CU_ASSERT(get_transfer_task(&conn, 2) == NULL);

	CU_ASSERT(get_transfer_task(&conn, 3) == &task3);
	spdk_del_transfer_task(&conn, 13);
	CU_ASSERT(get_transfer_task(&conn, 3) == NULL);

	CU_ASSERT(get_transfer_task(&conn, 4) == &task4);
	spdk_del_transfer_task(&conn, 14);
	CU_ASSERT(get_transfer_task(&conn, 4) == NULL);

	CU_ASSERT(get_transfer_task(&conn, 5) == &task5);
	spdk_del_transfer_task(&conn, 15);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);

	while (!TAILQ_EMPTY(&conn.active_r2t_tasks)) {
		task = TAILQ_FIRST(&conn.active_r2t_tasks);
		TAILQ_REMOVE(&conn.active_r2t_tasks, task, link);
	}

	while (!TAILQ_EMPTY(&g_write_pdu_list)) {
		pdu = TAILQ_FIRST(&g_write_pdu_list);
		TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
		spdk_put_pdu(pdu);
	}

	spdk_put_pdu(pdu5);
	spdk_put_pdu(pdu4);
	spdk_put_pdu(pdu3);
	spdk_put_pdu(pdu2);
	spdk_put_pdu(pdu1);
}

static void
clear_all_transfer_tasks_test(void)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task *task1, *task2, *task3, *task4, *task5, *task6;
	struct spdk_iscsi_pdu *pdu1, *pdu2, *pdu3, *pdu4, *pdu5, *pdu6, *pdu;
	struct spdk_iscsi_pdu *mgmt_pdu1, *mgmt_pdu2;
	struct spdk_scsi_lun lun1, lun2;
	uint32_t alloc_cmd_sn;
	int rc;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&lun1, 0, sizeof(lun1));
	memset(&lun2, 0, sizeof(lun2));

	sess.MaxBurstLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	sess.MaxOutstandingR2T = 1;

	conn.sess = &sess;
	TAILQ_INIT(&conn.active_r2t_tasks);
	TAILQ_INIT(&conn.queued_r2t_tasks);

	alloc_cmd_sn = 10;

	task1 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task1 != NULL);
	pdu1 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	pdu1->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu1->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task1->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task1->scsi.lun = &lun1;
	spdk_iscsi_task_set_pdu(task1, pdu1);

	rc = add_transfer_task(&conn, task1);
	CU_ASSERT(rc == SPDK_SUCCESS);

	mgmt_pdu1 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(mgmt_pdu1 != NULL);

	mgmt_pdu1->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	task2 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task2 != NULL);
	pdu2 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	pdu2->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu2->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task2->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task2->scsi.lun = &lun1;
	spdk_iscsi_task_set_pdu(task2, pdu2);

	rc = add_transfer_task(&conn, task2);
	CU_ASSERT(rc == SPDK_SUCCESS);

	task3 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task3 != NULL);
	pdu3 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu3 != NULL);

	pdu3->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu3->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task3->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task3->scsi.lun = &lun1;
	spdk_iscsi_task_set_pdu(task3, pdu3);

	rc = add_transfer_task(&conn, task3);
	CU_ASSERT(rc == SPDK_SUCCESS);

	task4 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task4 != NULL);
	pdu4 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu4 != NULL);

	pdu4->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu4->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task4->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task4->scsi.lun = &lun2;
	spdk_iscsi_task_set_pdu(task4, pdu4);

	rc = add_transfer_task(&conn, task4);
	CU_ASSERT(rc == SPDK_SUCCESS);

	task5 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task5 != NULL);
	pdu5 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu5 != NULL);

	pdu5->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu5->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task5->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task5->scsi.lun = &lun2;
	spdk_iscsi_task_set_pdu(task5, pdu5);

	rc = add_transfer_task(&conn, task5);
	CU_ASSERT(rc == SPDK_SUCCESS);

	mgmt_pdu2 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(mgmt_pdu2 != NULL);

	mgmt_pdu2->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	task6 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task6 != NULL);
	pdu6 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu6 != NULL);

	pdu6->data_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	pdu6->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task5->scsi.transfer_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	task6->scsi.lun = &lun2;
	spdk_iscsi_task_set_pdu(task6, pdu6);

	rc = add_transfer_task(&conn, task6);
	CU_ASSERT(rc == SPDK_SUCCESS);

	CU_ASSERT(conn.ttt == 4);

	CU_ASSERT(get_transfer_task(&conn, 1) == task1);
	CU_ASSERT(get_transfer_task(&conn, 2) == task2);
	CU_ASSERT(get_transfer_task(&conn, 3) == task3);
	CU_ASSERT(get_transfer_task(&conn, 4) == task4);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);

	spdk_clear_all_transfer_task(&conn, &lun1, mgmt_pdu1);

	CU_ASSERT(!TAILQ_EMPTY(&conn.queued_r2t_tasks));
	CU_ASSERT(get_transfer_task(&conn, 1) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 2) == task2);
	CU_ASSERT(get_transfer_task(&conn, 3) == task3);
	CU_ASSERT(get_transfer_task(&conn, 4) == task4);
	CU_ASSERT(get_transfer_task(&conn, 5) == task5);
	CU_ASSERT(get_transfer_task(&conn, 6) == NULL);

	spdk_clear_all_transfer_task(&conn, &lun1, NULL);

	CU_ASSERT(TAILQ_EMPTY(&conn.queued_r2t_tasks));
	CU_ASSERT(get_transfer_task(&conn, 1) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 2) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 3) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 4) == task4);
	CU_ASSERT(get_transfer_task(&conn, 5) == task5);
	CU_ASSERT(get_transfer_task(&conn, 6) == task6);

	spdk_clear_all_transfer_task(&conn, &lun2, mgmt_pdu2);

	CU_ASSERT(get_transfer_task(&conn, 4) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 5) == NULL);
	CU_ASSERT(get_transfer_task(&conn, 6) == task6);

	spdk_clear_all_transfer_task(&conn, NULL, NULL);

	CU_ASSERT(get_transfer_task(&conn, 6) == NULL);

	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));
	while (!TAILQ_EMPTY(&g_write_pdu_list)) {
		pdu = TAILQ_FIRST(&g_write_pdu_list);
		TAILQ_REMOVE(&g_write_pdu_list, pdu, tailq);
		spdk_put_pdu(pdu);
	}

	spdk_put_pdu(mgmt_pdu2);
	spdk_put_pdu(mgmt_pdu1);
	spdk_put_pdu(pdu6);
	spdk_put_pdu(pdu5);
	spdk_put_pdu(pdu4);
	spdk_put_pdu(pdu3);
	spdk_put_pdu(pdu2);
	spdk_put_pdu(pdu1);
}

static void
abort_queued_datain_task_test(void)
{
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task *task, *task2, *task3;
	int rc;

	TAILQ_INIT(&conn.queued_datain_tasks);

	task = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task != NULL);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task, link);

	/* Slot of data in tasks are full */
	conn.data_in_cnt = MAX_LARGE_DATAIN_PER_CONNECTION;

	rc = _iscsi_conn_abort_queued_datain_task(&conn, task);
	CU_ASSERT(rc != 0);

	/* Only one slot remains and no subtasks are submitted yet. */
	conn.data_in_cnt--;
	task->current_datain_offset = 0;

	rc = _iscsi_conn_abort_queued_datain_task(&conn, task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&conn.queued_datain_tasks));

	task = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task != NULL);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task, link);

	/* Only one slot remains and a subtask is submitted. */
	task->scsi.transfer_len = SPDK_BDEV_LARGE_BUF_MAX_SIZE * 3;
	task->current_datain_offset = SPDK_BDEV_LARGE_BUF_MAX_SIZE;

	rc = _iscsi_conn_abort_queued_datain_task(&conn, task);
	CU_ASSERT(rc != 0);
	CU_ASSERT(task->current_datain_offset == SPDK_BDEV_LARGE_BUF_MAX_SIZE * 2);
	CU_ASSERT(conn.data_in_cnt == MAX_LARGE_DATAIN_PER_CONNECTION);

	/* Additional one slot becomes vacant. */
	conn.data_in_cnt--;

	rc = _iscsi_conn_abort_queued_datain_task(&conn, task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&conn.queued_datain_tasks));

	spdk_iscsi_task_cpl(&task->scsi);

	/* Queue three data in tasks and abort each task sequentially */
	task = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task != NULL);
	task->tag = 1;
	task->current_datain_offset = 0;
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task, link);

	task2 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task2 != NULL);
	task2->tag = 2;
	task2->current_datain_offset = 0;
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task2, link);

	task3 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task3 != NULL);
	task3->tag = 3;
	task3->current_datain_offset = 0;
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task3, link);

	conn.data_in_cnt--;

	rc = iscsi_conn_abort_queued_datain_task(&conn, 1);
	CU_ASSERT(rc == 0);

	rc = iscsi_conn_abort_queued_datain_task(&conn, 2);
	CU_ASSERT(rc == 0);

	rc = iscsi_conn_abort_queued_datain_task(&conn, 3);
	CU_ASSERT(rc == 0);

	CU_ASSERT(TAILQ_EMPTY(&conn.queued_datain_tasks));
}

static bool
datain_task_is_queued(struct spdk_iscsi_conn *conn,
		      struct spdk_iscsi_task *task)
{
	struct spdk_iscsi_task *tmp;

	TAILQ_FOREACH(tmp, &conn->queued_datain_tasks, link) {
		if (tmp == task) {
			return true;
		}
	}
	return false;
}

static void
abort_queued_datain_tasks_test(void)
{
	struct spdk_iscsi_conn conn;
	struct spdk_iscsi_task *task1, *task2, *task3, *task4, *task5, *task6;
	struct spdk_iscsi_task *task, *tmp;
	struct spdk_iscsi_pdu *pdu1, *pdu2, *pdu3, *pdu4, *pdu5, *pdu6;
	struct spdk_iscsi_pdu *mgmt_pdu1, *mgmt_pdu2;
	struct spdk_scsi_lun lun1, lun2;
	uint32_t alloc_cmd_sn;
	int rc;

	TAILQ_INIT(&conn.queued_datain_tasks);
	conn.data_in_cnt = 0;

	alloc_cmd_sn = 88;

	task1 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task1 != NULL);
	pdu1 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu1 != NULL);

	pdu1->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task1->current_datain_offset = 0;
	task1->scsi.lun = &lun1;
	spdk_iscsi_task_set_pdu(task1, pdu1);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task1, link);

	task2 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task2 != NULL);
	pdu2 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu2 != NULL);

	pdu2->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task2->current_datain_offset = 0;
	task2->scsi.lun = &lun2;
	spdk_iscsi_task_set_pdu(task2, pdu2);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task2, link);

	mgmt_pdu1 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(mgmt_pdu1 != NULL);

	mgmt_pdu1->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	task3 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task3 != NULL);
	pdu3 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu3 != NULL);

	pdu3->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task3->current_datain_offset = 0;
	task3->scsi.lun = &lun1;
	spdk_iscsi_task_set_pdu(task3, pdu3);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task3, link);

	task4 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task4 != NULL);
	pdu4 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu4 != NULL);

	pdu4->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task4->current_datain_offset = 0;
	task4->scsi.lun = &lun2;
	spdk_iscsi_task_set_pdu(task4, pdu4);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task4, link);

	task5 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task5 != NULL);
	pdu5 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu5 != NULL);

	pdu5->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task5->current_datain_offset = 0;
	task5->scsi.lun = &lun1;
	spdk_iscsi_task_set_pdu(task5, pdu5);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task5, link);

	mgmt_pdu2 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(mgmt_pdu2 != NULL);

	mgmt_pdu2->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	task6 = spdk_iscsi_task_get(&conn, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(task6 != NULL);
	pdu6 = spdk_get_pdu();
	SPDK_CU_ASSERT_FATAL(pdu6 != NULL);

	pdu6->cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	task6->current_datain_offset = 0;
	task6->scsi.lun = &lun2;
	spdk_iscsi_task_set_pdu(task6, pdu6);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, task6, link);

	rc = iscsi_conn_abort_queued_datain_tasks(&conn, &lun1, mgmt_pdu1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!datain_task_is_queued(&conn, task1));
	CU_ASSERT(datain_task_is_queued(&conn, task2));
	CU_ASSERT(datain_task_is_queued(&conn, task3));
	CU_ASSERT(datain_task_is_queued(&conn, task4));
	CU_ASSERT(datain_task_is_queued(&conn, task5));
	CU_ASSERT(datain_task_is_queued(&conn, task6));

	rc = iscsi_conn_abort_queued_datain_tasks(&conn, &lun2, mgmt_pdu2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!datain_task_is_queued(&conn, task2));
	CU_ASSERT(datain_task_is_queued(&conn, task3));
	CU_ASSERT(!datain_task_is_queued(&conn, task4));
	CU_ASSERT(datain_task_is_queued(&conn, task5));
	CU_ASSERT(datain_task_is_queued(&conn, task6));

	TAILQ_FOREACH_SAFE(task, &conn.queued_datain_tasks, link, tmp) {
		TAILQ_REMOVE(&conn.queued_datain_tasks, task, link);
		spdk_iscsi_task_cpl(&task->scsi);
	}

	spdk_put_pdu(mgmt_pdu2);
	spdk_put_pdu(mgmt_pdu1);
	spdk_put_pdu(pdu6);
	spdk_put_pdu(pdu5);
	spdk_put_pdu(pdu4);
	spdk_put_pdu(pdu3);
	spdk_put_pdu(pdu2);
	spdk_put_pdu(pdu1);
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
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
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
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
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
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[1].iov_len == 512);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[2].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN / 2;
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)((uint8_t *)pdu.header_digest + ISCSI_DIGEST_LEN / 2));
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN / 2);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[1].iov_len == 512);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[2].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN / 2 + 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN;
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[0].iov_len == 512);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == 512 + ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512;
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)pdu.data_digest);
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN / 2;
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)((uint8_t *)pdu.data_digest + ISCSI_DIGEST_LEN / 2));
	CU_ASSERT(iovs[0].iov_len == ISCSI_DIGEST_LEN / 2);
	CU_ASSERT(mapped_length == ISCSI_DIGEST_LEN / 2);

	pdu.writev_offset = ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512 + ISCSI_DIGEST_LEN;
	rc = spdk_iscsi_build_iovs(&conn, iovs, 5, &pdu, &mapped_length);
	CU_ASSERT(rc == 0);
	CU_ASSERT(mapped_length == 0);

	pdu.writev_offset = 0;
	rc = spdk_iscsi_build_iovs(&conn, iovs, 1, &pdu, &mapped_length);
	CU_ASSERT(rc == 1);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN);

	rc = spdk_iscsi_build_iovs(&conn, iovs, 2, &pdu, &mapped_length);
	CU_ASSERT(rc == 2);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN);

	rc = spdk_iscsi_build_iovs(&conn, iovs, 3, &pdu, &mapped_length);
	CU_ASSERT(rc == 3);
	CU_ASSERT(iovs[0].iov_base == (void *)&pdu.bhs);
	CU_ASSERT(iovs[0].iov_len == ISCSI_BHS_LEN);
	CU_ASSERT(iovs[1].iov_base == (void *)pdu.header_digest);
	CU_ASSERT(iovs[1].iov_len == ISCSI_DIGEST_LEN);
	CU_ASSERT(iovs[2].iov_base == (void *)pdu.data);
	CU_ASSERT(iovs[2].iov_len == 512);
	CU_ASSERT(mapped_length == ISCSI_BHS_LEN + ISCSI_DIGEST_LEN + 512);

	rc = spdk_iscsi_build_iovs(&conn, iovs, 4, &pdu, &mapped_length);
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

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("iscsi_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "login check target test", op_login_check_target_test) == NULL
		|| CU_add_test(suite, "maxburstlength test", maxburstlength_test) == NULL
		|| CU_add_test(suite, "underflow for read transfer test",
			       underflow_for_read_transfer_test) == NULL
		|| CU_add_test(suite, "underflow for zero read transfer test",
			       underflow_for_zero_read_transfer_test) == NULL
		|| CU_add_test(suite, "underflow for request sense test",
			       underflow_for_request_sense_test) == NULL
		|| CU_add_test(suite, "underflow for check condition test",
			       underflow_for_check_condition_test) == NULL
		|| CU_add_test(suite, "add transfer task test", add_transfer_task_test) == NULL
		|| CU_add_test(suite, "get transfer task test", get_transfer_task_test) == NULL
		|| CU_add_test(suite, "del transfer task test", del_transfer_task_test) == NULL
		|| CU_add_test(suite, "clear all transfer tasks test",
			       clear_all_transfer_tasks_test) == NULL
		|| CU_add_test(suite, "abort_queued_datain_task_test",
			       abort_queued_datain_task_test) == NULL
		|| CU_add_test(suite, "abort_queued_datain_tasks_test",
			       abort_queued_datain_tasks_test) == NULL
		|| CU_add_test(suite, "build_iovs_test", build_iovs_test) == NULL
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
