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

#include "common/lib/test_env.c"
#include "spdk_cunit.h"

#include "iscsi/conn.c"

#include "spdk_internal/mock.h"

SPDK_LOG_REGISTER_COMPONENT("iscsi", SPDK_LOG_ISCSI)

#define DMIN32(A,B) ((uint32_t) ((uint32_t)(A) > (uint32_t)(B) ? (uint32_t)(B) : (uint32_t)(A)))

struct spdk_scsi_lun {
	uint8_t reserved;
};

static ssize_t g_sock_writev_bytes = 0;

DEFINE_STUB(spdk_app_get_shm_id, int, (void), 0);

DEFINE_STUB(spdk_sock_getaddr, int,
	    (struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
	     char *caddr, int clen, uint16_t *cport),
	    0);

int
spdk_sock_close(struct spdk_sock **sock)
{
	*sock = NULL;
	return 0;
}

DEFINE_STUB(spdk_sock_recv, ssize_t,
	    (struct spdk_sock *sock, void *buf, size_t len), 0);

DEFINE_STUB(spdk_sock_readv, ssize_t,
	    (struct spdk_sock *sock, struct iovec *iov, int iovcnt), 0);

ssize_t
spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	return g_sock_writev_bytes;
}

DEFINE_STUB(spdk_sock_set_recvlowat, int, (struct spdk_sock *s, int nbytes), 0);

DEFINE_STUB(spdk_sock_set_recvbuf, int, (struct spdk_sock *sock, int sz), 0);

DEFINE_STUB(spdk_sock_set_sendbuf, int, (struct spdk_sock *sock, int sz), 0);

DEFINE_STUB(spdk_sock_group_add_sock, int,
	    (struct spdk_sock_group *group, struct spdk_sock *sock,
	     spdk_sock_cb cb_fn, void *cb_arg),
	    0);

DEFINE_STUB(spdk_sock_group_remove_sock, int,
	    (struct spdk_sock_group *group, struct spdk_sock *sock), 0);

void
spdk_scsi_task_put(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *task;

	CU_ASSERT(scsi_task->ref > 0);
	scsi_task->ref--;

	task = spdk_iscsi_task_from_scsi_task(scsi_task);
	if (task->parent) {
		spdk_scsi_task_put(&task->parent->scsi);
	}
}

DEFINE_STUB(spdk_scsi_dev_get_lun, struct spdk_scsi_lun *,
	    (struct spdk_scsi_dev *dev, int lun_id), NULL);

DEFINE_STUB(spdk_scsi_dev_has_pending_tasks, bool,
	    (const struct spdk_scsi_dev *dev, const struct spdk_scsi_port *initiator_port),
	    true);

DEFINE_STUB(spdk_scsi_lun_open, int,
	    (struct spdk_scsi_lun *lun, spdk_scsi_lun_remove_cb_t hotremove_cb,
	     void *hotremove_ctx, struct spdk_scsi_lun_desc **desc),
	    0);

DEFINE_STUB_V(spdk_scsi_lun_close, (struct spdk_scsi_lun_desc *desc));

DEFINE_STUB(spdk_scsi_lun_allocate_io_channel, int,
	    (struct spdk_scsi_lun_desc *desc), 0);

DEFINE_STUB_V(spdk_scsi_lun_free_io_channel, (struct spdk_scsi_lun_desc *desc));

DEFINE_STUB(spdk_scsi_lun_get_id, int, (const struct spdk_scsi_lun *lun), 0);

DEFINE_STUB(spdk_scsi_port_get_name, const char *,
	    (const struct spdk_scsi_port *port), NULL);

void
spdk_scsi_task_copy_status(struct spdk_scsi_task *dst,
			   struct spdk_scsi_task *src)
{
	dst->status = src->status;
}

DEFINE_STUB_V(spdk_put_pdu, (struct spdk_iscsi_pdu *pdu));

DEFINE_STUB_V(spdk_iscsi_param_free, (struct iscsi_param *params));

DEFINE_STUB(spdk_iscsi_conn_params_init, int, (struct iscsi_param **params), 0);

DEFINE_STUB_V(spdk_clear_all_transfer_task,
	      (struct spdk_iscsi_conn *conn, struct spdk_scsi_lun *lun,
	       struct spdk_iscsi_pdu *pdu));

DEFINE_STUB(spdk_iscsi_build_iovs, int,
	    (struct spdk_iscsi_conn *conn, struct iovec *iov, int num_iovs,
	     struct spdk_iscsi_pdu *pdu, uint32_t *mapped_length),
	    0);

DEFINE_STUB(spdk_iscsi_is_deferred_free_pdu, bool,
	    (struct spdk_iscsi_pdu *pdu), false);

DEFINE_STUB_V(spdk_iscsi_task_response,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(spdk_iscsi_task_mgmt_response,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(spdk_iscsi_send_nopin, (struct spdk_iscsi_conn *conn));

DEFINE_STUB(spdk_del_transfer_task, bool,
	    (struct spdk_iscsi_conn *conn, uint32_t task_tag), true);

int
spdk_iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn)
{
	return 0;
}

DEFINE_STUB(spdk_iscsi_handle_incoming_pdus, int, (struct spdk_iscsi_conn *conn), 0);

DEFINE_STUB_V(spdk_free_sess, (struct spdk_iscsi_sess *sess));

DEFINE_STUB(spdk_iscsi_tgt_node_cleanup_luns, int,
	    (struct spdk_iscsi_conn *conn, struct spdk_iscsi_tgt_node *target),
	    0);

DEFINE_STUB(spdk_iscsi_pdu_calc_header_digest, uint32_t,
	    (struct spdk_iscsi_pdu *pdu), 0);

DEFINE_STUB(spdk_iscsi_pdu_calc_data_digest, uint32_t,
	    (struct spdk_iscsi_pdu *pdu), 0);

DEFINE_STUB_V(spdk_shutdown_iscsi_conns_done, (void));

/* Case1: Simulate the failure of large read task */
static void
propagate_scsi_error_status_for_split_read_tasks(void)
{
	struct spdk_iscsi_task primary = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, task3 = {}, task4 = {}, task5 = {}, task6 = {};

	primary.scsi.transfer_len = 512 * 6;
	primary.rsp_scsi_status = SPDK_SCSI_STATUS_GOOD;
	TAILQ_INIT(&primary.subtask_list);
	primary.scsi.ref = 7;

	task1.scsi.offset = 0;
	task1.scsi.length = 512;
	task1.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task1.scsi.ref = 1;
	task1.parent = &primary;

	task2.scsi.offset = 512;
	task2.scsi.length = 512;
	task2.scsi.status = SPDK_SCSI_STATUS_CHECK_CONDITION;
	task2.scsi.ref = 1;
	task2.parent = &primary;

	task3.scsi.offset = 512 * 2;
	task3.scsi.length = 512;
	task3.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task3.scsi.ref = 1;
	task3.parent = &primary;

	task4.scsi.offset = 512 * 3;
	task4.scsi.length = 512;
	task4.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task4.scsi.ref = 1;
	task4.parent = &primary;

	task5.scsi.offset = 512 * 4;
	task5.scsi.length = 512;
	task5.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task5.scsi.ref = 1;
	task5.parent = &primary;

	task6.scsi.offset = 512 * 5;
	task6.scsi.length = 512;
	task6.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task6.scsi.ref = 1;
	task6.parent = &primary;

	/* task2 has check condition status, and verify if the check condition
	 * status is propagated to remaining tasks correctly when these tasks complete
	 * by the following order, task4, task3, task2, task1, primary, task5, and task6.
	 */
	process_read_task_completion(NULL, &task4, &primary);
	process_read_task_completion(NULL, &task3, &primary);
	process_read_task_completion(NULL, &task2, &primary);
	process_read_task_completion(NULL, &task1, &primary);
	process_read_task_completion(NULL, &task5, &primary);
	process_read_task_completion(NULL, &task6, &primary);

	CU_ASSERT(primary.rsp_scsi_status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task1.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task2.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task3.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task4.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task5.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task6.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(primary.bytes_completed == primary.scsi.transfer_len);
	CU_ASSERT(TAILQ_EMPTY(&primary.subtask_list));
	CU_ASSERT(primary.scsi.ref == 0);
	CU_ASSERT(task1.scsi.ref == 0);
	CU_ASSERT(task2.scsi.ref == 0);
	CU_ASSERT(task3.scsi.ref == 0);
	CU_ASSERT(task4.scsi.ref == 0);
	CU_ASSERT(task5.scsi.ref == 0);
	CU_ASSERT(task6.scsi.ref == 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("hoplug_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "propagate_scsi_error_status_for_split_read_tasks",
			    propagate_scsi_error_status_for_split_read_tasks) == NULL
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
