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

#include "CUnit/Basic.h"

#include "iscsi/iscsi.c"

#include "../common.c"
#include "iscsi/acceptor.h"
#include "iscsi/portal_grp.h"
#include "scsi/scsi_internal.h"

struct spdk_iscsi_tgt_node *
spdk_iscsi_find_tgt_node(const char *target_name)
{
	return NULL;
}

int
spdk_iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
			   struct spdk_iscsi_tgt_node *target,
			   const char *iqn, const char *addr)
{
	return 0;
}

int
spdk_iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
		     const char *iaddr,
		     const char *tiqn, uint8_t *data, int alloc_len, int data_len)
{
	return 0;
}

void
spdk_iscsi_acceptor_stop(void)
{
}

int
spdk_iscsi_portal_grp_close_all(void)
{
	return 0;
}

void
spdk_iscsi_conn_free_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
}

int
spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun)
{
	return lun->id;
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id)
{
	if (lun_id < 0 || lun_id >= SPDK_SCSI_DEV_MAX_LUN) {
		return NULL;
	}

	return dev->lun[lun_id];
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

	req_pdu->bhs.opcode = ISCSI_OP_SCSI;
	req_pdu->data_segment_len = 0;

	req = (struct iscsi_bhs_scsi_req *)&req_pdu->bhs;

	to_be32(&req->cmd_sn, 0);
	to_be32(&req->expected_data_xfer_len, 1028);
	to_be32(&req->itt, 0x1234);
	req->write_bit = 1;
	req->final_bit = 1;

	g_spdk_iscsi.MaxRecvDataSegmentLength = 1024;

	rc = spdk_iscsi_execute(&conn, req_pdu);
	CU_ASSERT_FATAL(rc == 0);

	response_pdu = TAILQ_FIRST(&conn.write_pdu_list);
	CU_ASSERT(response_pdu != NULL);

	/*
	 * Confirm that a correct R2T reply was sent in reponse to the
	 *  SCSI request.
	 */
	TAILQ_REMOVE(&conn.write_pdu_list, response_pdu, tailq);
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

	CU_ASSERT(response_pdu->task != NULL);
	spdk_iscsi_task_disassociate_pdu(response_pdu->task);
	spdk_iscsi_task_put(response_pdu->task);
	spdk_put_pdu(response_pdu);

	r2t_pdu = TAILQ_FIRST(&conn.write_pdu_list);
	CU_ASSERT(r2t_pdu != NULL);
	TAILQ_REMOVE(&conn.write_pdu_list, r2t_pdu, tailq);
	spdk_put_pdu(r2t_pdu);

	spdk_put_pdu(data_out_pdu);
	spdk_put_pdu(req_pdu);
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
		CU_add_test(suite, "maxburstlength test", maxburstlength_test) == NULL
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
