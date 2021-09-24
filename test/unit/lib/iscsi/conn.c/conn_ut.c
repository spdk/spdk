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

SPDK_LOG_REGISTER_COMPONENT(iscsi)

DEFINE_STUB(iscsi_get_pdu, struct spdk_iscsi_pdu *,
	    (struct spdk_iscsi_conn *conn), NULL);
DEFINE_STUB(iscsi_param_eq_val, int,
	    (struct iscsi_param *params, const char *key, const char *val), 0);
DEFINE_STUB(iscsi_pdu_calc_data_digest, uint32_t, (struct spdk_iscsi_pdu *pdu), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_named_int32, int, (struct spdk_json_write_ctx *w,
		const char *name, int32_t val), 0);
DEFINE_STUB(spdk_json_write_named_string, int, (struct spdk_json_write_ctx *w,
		const char *name, const char *val), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB_V(spdk_sock_writev_async,
	      (struct spdk_sock *sock, struct spdk_sock_request *req));

struct spdk_scsi_lun {
	uint8_t reserved;
};

struct spdk_iscsi_globals g_iscsi = {
	.MaxLargeDataInPerConnection = DEFAULT_MAX_LARGE_DATAIN_PER_CONNECTION,
};

static TAILQ_HEAD(read_tasks_head, spdk_iscsi_task) g_ut_read_tasks =
	TAILQ_HEAD_INITIALIZER(g_ut_read_tasks);
static struct spdk_iscsi_task *g_new_task = NULL;
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

struct spdk_iscsi_task *
iscsi_task_get(struct spdk_iscsi_conn *conn,
	       struct spdk_iscsi_task *parent,
	       spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_iscsi_task *task;

	task = g_new_task;
	if (task == NULL) {
		return NULL;
	}
	memset(task, 0, sizeof(*task));

	task->scsi.ref = 1;
	task->conn = conn;
	task->scsi.cpl_fn = cpl_fn;
	if (parent) {
		parent->scsi.ref++;
		task->parent = parent;
		task->scsi.dxfer_dir = parent->scsi.dxfer_dir;
		task->scsi.transfer_len = parent->scsi.transfer_len;
		task->scsi.lun = parent->scsi.lun;
		if (conn && (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV)) {
			conn->data_in_cnt++;
		}
	}

	return task;
}

void
spdk_scsi_task_put(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *task;

	CU_ASSERT(scsi_task->ref > 0);
	scsi_task->ref--;

	task = iscsi_task_from_scsi_task(scsi_task);
	if (task->parent) {
		spdk_scsi_task_put(&task->parent->scsi);
	}
}

DEFINE_STUB(spdk_scsi_dev_get_lun, struct spdk_scsi_lun *,
	    (struct spdk_scsi_dev *dev, int lun_id), NULL);

DEFINE_STUB(spdk_scsi_dev_get_first_lun, struct spdk_scsi_lun *,
	    (struct spdk_scsi_dev *dev), NULL);

DEFINE_STUB(spdk_scsi_dev_get_next_lun, struct spdk_scsi_lun *,
	    (struct spdk_scsi_lun *prev_lun), NULL);

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

DEFINE_STUB_V(spdk_scsi_task_set_data, (struct spdk_scsi_task *task, void *data, uint32_t len));

DEFINE_STUB_V(spdk_scsi_task_process_null_lun, (struct spdk_scsi_task *task));

DEFINE_STUB_V(spdk_scsi_task_process_abort, (struct spdk_scsi_task *task));

DEFINE_STUB_V(iscsi_put_pdu, (struct spdk_iscsi_pdu *pdu));

DEFINE_STUB_V(iscsi_param_free, (struct iscsi_param *params));

DEFINE_STUB(iscsi_conn_params_init, int, (struct iscsi_param **params), 0);

DEFINE_STUB_V(iscsi_clear_all_transfer_task,
	      (struct spdk_iscsi_conn *conn, struct spdk_scsi_lun *lun,
	       struct spdk_iscsi_pdu *pdu));

DEFINE_STUB(iscsi_build_iovs, int,
	    (struct spdk_iscsi_conn *conn, struct iovec *iov, int num_iovs,
	     struct spdk_iscsi_pdu *pdu, uint32_t *mapped_length),
	    0);

DEFINE_STUB_V(iscsi_queue_task,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(iscsi_task_response,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(iscsi_task_mgmt_response,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(iscsi_send_nopin, (struct spdk_iscsi_conn *conn));

bool
iscsi_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t task_tag)
{
	struct spdk_iscsi_task *task;

	task = TAILQ_FIRST(&conn->active_r2t_tasks);
	if (task == NULL || task->tag != task_tag) {
		return false;
	}

	TAILQ_REMOVE(&conn->active_r2t_tasks, task, link);
	task->is_r2t_active = false;
	iscsi_task_put(task);

	return true;
}

DEFINE_STUB(iscsi_handle_incoming_pdus, int, (struct spdk_iscsi_conn *conn), 0);

DEFINE_STUB_V(iscsi_free_sess, (struct spdk_iscsi_sess *sess));

DEFINE_STUB(iscsi_tgt_node_cleanup_luns, int,
	    (struct spdk_iscsi_conn *conn, struct spdk_iscsi_tgt_node *target),
	    0);

DEFINE_STUB(iscsi_pdu_calc_header_digest, uint32_t,
	    (struct spdk_iscsi_pdu *pdu), 0);

DEFINE_STUB(spdk_iscsi_pdu_calc_data_digest, uint32_t,
	    (struct spdk_iscsi_pdu *pdu), 0);

DEFINE_STUB_V(shutdown_iscsi_conns_done, (void));

static struct spdk_iscsi_task *
ut_conn_task_get(struct spdk_iscsi_task *parent)
{
	struct spdk_iscsi_task *task;

	task = calloc(1, sizeof(*task));
	SPDK_CU_ASSERT_FATAL(task != NULL);

	task->scsi.ref = 1;

	if (parent) {
		task->parent = parent;
		parent->scsi.ref++;
	}
	return task;
}

static void
ut_conn_create_read_tasks(struct spdk_iscsi_task *primary)
{
	struct spdk_iscsi_task *subtask;
	uint32_t remaining_size = 0;

	while (1) {
		if (primary->current_data_offset < primary->scsi.transfer_len) {
			remaining_size = primary->scsi.transfer_len - primary->current_data_offset;

			subtask = ut_conn_task_get(primary);

			subtask->scsi.offset = primary->current_data_offset;
			subtask->scsi.length = spdk_min(SPDK_BDEV_LARGE_BUF_MAX_SIZE, remaining_size);
			subtask->scsi.status = SPDK_SCSI_STATUS_GOOD;

			primary->current_data_offset += subtask->scsi.length;

			TAILQ_INSERT_TAIL(&g_ut_read_tasks, subtask, link);
		}

		if (primary->current_data_offset == primary->scsi.transfer_len) {
			break;
		}
	}
}

static void
read_task_split_in_order_case(void)
{
	struct spdk_iscsi_task primary = {};
	struct spdk_iscsi_task *task, *tmp;
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_sess sess = {};

	conn.sess = &sess;
	conn.sess->DataSequenceInOrder = true;

	primary.scsi.transfer_len = SPDK_BDEV_LARGE_BUF_MAX_SIZE * 8;
	TAILQ_INIT(&primary.subtask_list);
	primary.current_data_offset = 0;
	primary.bytes_completed = 0;
	primary.scsi.ref = 1;

	ut_conn_create_read_tasks(&primary);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_ut_read_tasks));

	TAILQ_FOREACH(task, &g_ut_read_tasks, link) {
		CU_ASSERT(&primary == iscsi_task_get_primary(task));
		process_read_task_completion(&conn, task, &primary);
	}

	CU_ASSERT(primary.bytes_completed == primary.scsi.transfer_len);
	CU_ASSERT(primary.scsi.ref == 0);

	TAILQ_FOREACH_SAFE(task, &g_ut_read_tasks, link, tmp) {
		CU_ASSERT(task->scsi.ref == 0);
		TAILQ_REMOVE(&g_ut_read_tasks, task, link);
		free(task);
	}

}

static void
read_task_split_reverse_order_case(void)
{
	struct spdk_iscsi_task primary = {};
	struct spdk_iscsi_task *task, *tmp;
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_sess sess = {};

	conn.sess = &sess;
	conn.sess->DataSequenceInOrder = true;

	primary.scsi.transfer_len = SPDK_BDEV_LARGE_BUF_MAX_SIZE * 8;
	TAILQ_INIT(&primary.subtask_list);
	primary.current_data_offset = 0;
	primary.bytes_completed = 0;
	primary.scsi.ref = 1;

	ut_conn_create_read_tasks(&primary);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_ut_read_tasks));

	TAILQ_FOREACH_REVERSE(task, &g_ut_read_tasks, read_tasks_head, link) {
		CU_ASSERT(&primary == iscsi_task_get_primary(task));
		process_read_task_completion(&conn, task, &primary);
	}

	CU_ASSERT(primary.bytes_completed == primary.scsi.transfer_len);
	CU_ASSERT(primary.scsi.ref == 0);

	TAILQ_FOREACH_SAFE(task, &g_ut_read_tasks, link, tmp) {
		CU_ASSERT(task->scsi.ref == 0);
		TAILQ_REMOVE(&g_ut_read_tasks, task, link);
		free(task);
	}
}

static void
propagate_scsi_error_status_for_split_read_tasks(void)
{
	struct spdk_iscsi_task primary = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, task3 = {}, task4 = {}, task5 = {}, task6 = {};

	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_sess sess = {};

	conn.sess = &sess;
	conn.sess->DataSequenceInOrder = true;

	primary.scsi.transfer_len = 512 * 6;
	primary.scsi.status = SPDK_SCSI_STATUS_GOOD;
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
	process_read_task_completion(&conn, &task4, &primary);
	process_read_task_completion(&conn, &task3, &primary);
	process_read_task_completion(&conn, &task2, &primary);
	process_read_task_completion(&conn, &task1, &primary);
	process_read_task_completion(&conn, &task5, &primary);
	process_read_task_completion(&conn, &task6, &primary);

	CU_ASSERT(primary.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
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

static void
process_non_read_task_completion_test(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task primary = {};
	struct spdk_iscsi_task task = {};

	TAILQ_INIT(&conn.active_r2t_tasks);

	primary.bytes_completed = 0;
	primary.scsi.transfer_len = 4096 * 3;
	primary.scsi.status = SPDK_SCSI_STATUS_GOOD;
	primary.scsi.ref = 1;
	TAILQ_INSERT_TAIL(&conn.active_r2t_tasks, &primary, link);
	primary.is_r2t_active = true;
	primary.tag = 1;

	/* First subtask which failed. */
	task.scsi.length = 4096;
	task.scsi.data_transferred = 4096;
	task.scsi.status = SPDK_SCSI_STATUS_CHECK_CONDITION;
	task.scsi.ref = 1;
	task.parent = &primary;
	primary.scsi.ref++;

	process_non_read_task_completion(&conn, &task, &primary);
	CU_ASSERT(!TAILQ_EMPTY(&conn.active_r2t_tasks));
	CU_ASSERT(primary.bytes_completed == 4096);
	CU_ASSERT(primary.scsi.data_transferred == 0);
	CU_ASSERT(primary.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.scsi.ref == 0);
	CU_ASSERT(primary.scsi.ref == 1);

	/* Second subtask which succeeded. */
	task.scsi.length = 4096;
	task.scsi.data_transferred = 4096;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task.scsi.ref = 1;
	task.parent = &primary;
	primary.scsi.ref++;

	process_non_read_task_completion(&conn, &task, &primary);
	CU_ASSERT(!TAILQ_EMPTY(&conn.active_r2t_tasks));
	CU_ASSERT(primary.bytes_completed == 4096 * 2);
	CU_ASSERT(primary.scsi.data_transferred == 4096);
	CU_ASSERT(primary.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.scsi.ref == 0);
	CU_ASSERT(primary.scsi.ref == 1);

	/* Third and final subtask which succeeded. */
	task.scsi.length = 4096;
	task.scsi.data_transferred = 4096;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task.scsi.ref = 1;
	task.parent = &primary;
	primary.scsi.ref++;

	process_non_read_task_completion(&conn, &task, &primary);
	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));
	CU_ASSERT(primary.bytes_completed == 4096 * 3);
	CU_ASSERT(primary.scsi.data_transferred == 4096 * 2);
	CU_ASSERT(primary.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(task.scsi.ref == 0);
	CU_ASSERT(primary.scsi.ref == 0);

	/* A tricky case that the R2T was already terminated when the last task completed. */
	primary.scsi.ref = 0;
	primary.bytes_completed = 4096 * 2;
	primary.scsi.data_transferred = 4096 * 2;
	primary.scsi.transfer_len = 4096 * 3;
	primary.scsi.status = SPDK_SCSI_STATUS_CHECK_CONDITION;
	primary.is_r2t_active = false;
	task.scsi.length = 4096;
	task.scsi.data_transferred = 4096;
	task.scsi.status = SPDK_SCSI_STATUS_GOOD;
	task.scsi.ref = 1;
	task.parent = &primary;
	primary.scsi.ref++;

	process_non_read_task_completion(&conn, &task, &primary);
	CU_ASSERT(primary.bytes_completed == 4096 * 3);
	CU_ASSERT(primary.scsi.data_transferred == 4096 * 3);
	CU_ASSERT(primary.scsi.status == SPDK_SCSI_STATUS_CHECK_CONDITION);
	CU_ASSERT(primary.scsi.ref == 0);
}

static bool
dequeue_pdu(void *_head, struct spdk_iscsi_pdu *pdu)
{
	TAILQ_HEAD(queued_pdus, spdk_iscsi_pdu) *head = _head;
	struct spdk_iscsi_pdu *tmp;

	TAILQ_FOREACH(tmp, head, tailq) {
		if (tmp == pdu) {
			TAILQ_REMOVE(head, tmp, tailq);
			return true;
		}
	}
	return false;
}

static bool
dequeue_task(void *_head, struct spdk_iscsi_task *task)
{
	TAILQ_HEAD(queued_tasks, spdk_iscsi_task) *head = _head;
	struct spdk_iscsi_task *tmp;

	TAILQ_FOREACH(tmp, head, link) {
		if (tmp == task) {
			TAILQ_REMOVE(head, tmp, link);
			return true;
		}
	}
	return false;
}

static void iscsi_conn_pdu_dummy_complete(void *arg)
{
}

static void
free_tasks_on_connection(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu1 = {}, pdu2 = {}, pdu3 = {}, pdu4 = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, task3 = {};
	struct spdk_scsi_lun lun1 = {}, lun2 = {};

	TAILQ_INIT(&conn.write_pdu_list);
	TAILQ_INIT(&conn.snack_pdu_list);
	TAILQ_INIT(&conn.queued_datain_tasks);
	conn.data_in_cnt = g_iscsi.MaxLargeDataInPerConnection;

	pdu1.task = &task1;
	pdu2.task = &task2;
	pdu3.task = &task3;

	pdu1.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu2.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu3.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu4.cb_fn = iscsi_conn_pdu_dummy_complete;

	task1.scsi.lun = &lun1;
	task2.scsi.lun = &lun2;

	task1.is_queued = false;
	task2.is_queued = false;
	task3.is_queued = true;

	/* Test conn->write_pdu_list. */

	task1.scsi.ref = 1;
	task2.scsi.ref = 1;
	task3.scsi.ref = 1;
	TAILQ_INSERT_TAIL(&conn.write_pdu_list, &pdu1, tailq);
	TAILQ_INSERT_TAIL(&conn.write_pdu_list, &pdu2, tailq);
	TAILQ_INSERT_TAIL(&conn.write_pdu_list, &pdu3, tailq);
	TAILQ_INSERT_TAIL(&conn.write_pdu_list, &pdu4, tailq);

	/* Free all PDUs when exiting connection. */
	iscsi_conn_free_tasks(&conn);

	CU_ASSERT(TAILQ_EMPTY(&conn.write_pdu_list));
	CU_ASSERT(task1.scsi.ref == 0);
	CU_ASSERT(task2.scsi.ref == 0);
	CU_ASSERT(task3.scsi.ref == 0);

	/* Test conn->snack_pdu_list */

	task1.scsi.ref = 1;
	task2.scsi.ref = 1;
	task3.scsi.ref = 1;
	pdu1.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu2.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu3.cb_fn = iscsi_conn_pdu_dummy_complete;
	TAILQ_INSERT_TAIL(&conn.snack_pdu_list, &pdu1, tailq);
	TAILQ_INSERT_TAIL(&conn.snack_pdu_list, &pdu2, tailq);
	TAILQ_INSERT_TAIL(&conn.snack_pdu_list, &pdu3, tailq);

	/* Free all PDUs and associated tasks when exiting connection. */
	iscsi_conn_free_tasks(&conn);

	CU_ASSERT(!dequeue_pdu(&conn.snack_pdu_list, &pdu1));
	CU_ASSERT(!dequeue_pdu(&conn.snack_pdu_list, &pdu2));
	CU_ASSERT(!dequeue_pdu(&conn.snack_pdu_list, &pdu3));
	CU_ASSERT(task1.scsi.ref == 0);
	CU_ASSERT(task2.scsi.ref == 0);
	CU_ASSERT(task3.scsi.ref == 0);

	/* Test conn->queued_datain_tasks */

	task1.scsi.ref = 1;
	task2.scsi.ref = 1;
	task3.scsi.ref = 1;
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task1, link);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task2, link);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task3, link);

	/* Free all tasks which is not queued when exiting connection. */
	iscsi_conn_free_tasks(&conn);

	CU_ASSERT(!dequeue_task(&conn.queued_datain_tasks, &task1));
	CU_ASSERT(!dequeue_task(&conn.queued_datain_tasks, &task2));
	CU_ASSERT(dequeue_task(&conn.queued_datain_tasks, &task3));
	CU_ASSERT(task1.scsi.ref == 0);
	CU_ASSERT(task2.scsi.ref == 0);
	CU_ASSERT(task3.scsi.ref == 1);
}

static void
free_tasks_with_queued_datain(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_pdu pdu1 = {}, pdu2 = {}, pdu3 = {}, pdu4 = {}, pdu5 = {}, pdu6 = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, task3 = {}, task4 = {}, task5 = {}, task6 = {};

	TAILQ_INIT(&conn.write_pdu_list);
	TAILQ_INIT(&conn.snack_pdu_list);
	TAILQ_INIT(&conn.queued_datain_tasks);

	pdu1.task = &task1;
	pdu2.task = &task2;
	pdu3.task = &task3;
	pdu1.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu2.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu3.cb_fn = iscsi_conn_pdu_dummy_complete;

	task1.scsi.ref = 1;
	task2.scsi.ref = 1;
	task3.scsi.ref = 1;

	pdu3.bhs.opcode = ISCSI_OP_SCSI_DATAIN;
	task3.scsi.offset = 1;
	conn.data_in_cnt = 1;

	TAILQ_INSERT_TAIL(&conn.write_pdu_list, &pdu1, tailq);
	TAILQ_INSERT_TAIL(&conn.write_pdu_list, &pdu2, tailq);
	TAILQ_INSERT_TAIL(&conn.write_pdu_list, &pdu3, tailq);

	task4.scsi.ref = 1;
	task5.scsi.ref = 1;
	task6.scsi.ref = 1;

	task4.pdu = &pdu4;
	task5.pdu = &pdu5;
	task6.pdu = &pdu6;
	pdu4.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu5.cb_fn = iscsi_conn_pdu_dummy_complete;
	pdu6.cb_fn = iscsi_conn_pdu_dummy_complete;

	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task4, link);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task5, link);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task6, link);

	iscsi_conn_free_tasks(&conn);

	CU_ASSERT(TAILQ_EMPTY(&conn.write_pdu_list));
	CU_ASSERT(TAILQ_EMPTY(&conn.queued_datain_tasks));
}

static void
abort_queued_datain_task_test(void)
{
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task = {}, subtask = {};
	struct spdk_iscsi_pdu pdu = {};
	struct iscsi_bhs_scsi_req *scsi_req;
	int rc;

	struct spdk_iscsi_sess sess = {};

	conn.sess = &sess;
	conn.sess->DataSequenceInOrder = true;

	TAILQ_INIT(&conn.queued_datain_tasks);
	task.scsi.ref = 1;
	task.scsi.dxfer_dir = SPDK_SCSI_DIR_FROM_DEV;
	task.pdu = &pdu;
	TAILQ_INIT(&task.subtask_list);
	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu.bhs;
	scsi_req->read_bit = 1;

	g_new_task = &subtask;

	/* Case1: Queue one task, and this task is not executed */
	task.scsi.transfer_len = SPDK_BDEV_LARGE_BUF_MAX_SIZE * 3;
	task.scsi.offset = 0;
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task, link);

	/* No slots for sub read tasks */
	conn.data_in_cnt = g_iscsi.MaxLargeDataInPerConnection;
	rc = _iscsi_conn_abort_queued_datain_task(&conn, &task);
	CU_ASSERT(rc != 0);
	CU_ASSERT(!TAILQ_EMPTY(&conn.queued_datain_tasks));

	/* Have slots for sub read tasks */
	conn.data_in_cnt = 0;
	rc = _iscsi_conn_abort_queued_datain_task(&conn, &task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&conn.queued_datain_tasks));
	CU_ASSERT(task.current_data_offset == SPDK_BDEV_LARGE_BUF_MAX_SIZE * 3);
	CU_ASSERT(task.scsi.ref == 0);
	CU_ASSERT(subtask.scsi.offset == 0);
	CU_ASSERT(subtask.scsi.length == SPDK_BDEV_LARGE_BUF_MAX_SIZE * 3);
	CU_ASSERT(subtask.scsi.ref == 0);

	/* Case2: Queue one task, and this task is partially executed */
	task.scsi.ref = 1;
	task.scsi.transfer_len = SPDK_BDEV_LARGE_BUF_MAX_SIZE * 3;
	task.current_data_offset = SPDK_BDEV_LARGE_BUF_MAX_SIZE;
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task, link);

	/* No slots for sub read tasks */
	conn.data_in_cnt = g_iscsi.MaxLargeDataInPerConnection;
	rc = _iscsi_conn_abort_queued_datain_task(&conn, &task);
	CU_ASSERT(rc != 0);
	CU_ASSERT(!TAILQ_EMPTY(&conn.queued_datain_tasks));

	/* have slots for sub read tasks */
	conn.data_in_cnt = 0;
	rc = _iscsi_conn_abort_queued_datain_task(&conn, &task);
	CU_ASSERT(rc == 0);
	CU_ASSERT(task.current_data_offset == SPDK_BDEV_LARGE_BUF_MAX_SIZE * 3);
	CU_ASSERT(task.scsi.ref == 2);
	CU_ASSERT(TAILQ_FIRST(&task.subtask_list) == &subtask);
	CU_ASSERT(subtask.scsi.offset == SPDK_BDEV_LARGE_BUF_MAX_SIZE);
	CU_ASSERT(subtask.scsi.length == SPDK_BDEV_LARGE_BUF_MAX_SIZE * 2);
	CU_ASSERT(subtask.scsi.ref == 1);

	g_new_task = NULL;
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
	struct spdk_iscsi_conn conn = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, task3 = {}, task4 = {}, task5 = {}, task6 = {};
	struct spdk_iscsi_task subtask = {};
	struct spdk_iscsi_pdu pdu1 = {}, pdu2 = {}, pdu3 = {}, pdu4 = {}, pdu5 = {}, pdu6 = {};
	struct spdk_iscsi_pdu mgmt_pdu1 = {}, mgmt_pdu2 = {};
	struct spdk_scsi_lun lun1 = {}, lun2 = {};
	uint32_t alloc_cmd_sn;
	struct iscsi_bhs_scsi_req *scsi_req;
	int rc;
	struct spdk_iscsi_sess sess = {};

	TAILQ_INIT(&conn.queued_datain_tasks);
	conn.data_in_cnt = 0;

	conn.sess = &sess;
	conn.sess->DataSequenceInOrder = true;

	g_new_task = &subtask;

	alloc_cmd_sn = 88;

	pdu1.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu1.bhs;
	scsi_req->read_bit = 1;
	task1.scsi.ref = 1;
	task1.current_data_offset = 0;
	task1.scsi.transfer_len = 512;
	task1.scsi.lun = &lun1;
	iscsi_task_set_pdu(&task1, &pdu1);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task1, link);

	pdu2.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu2.bhs;
	scsi_req->read_bit = 1;
	task2.scsi.ref = 1;
	task2.current_data_offset = 0;
	task2.scsi.transfer_len = 512;
	task2.scsi.lun = &lun2;
	iscsi_task_set_pdu(&task2, &pdu2);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task2, link);

	mgmt_pdu1.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	pdu3.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu3.bhs;
	scsi_req->read_bit = 1;
	task3.scsi.ref = 1;
	task3.current_data_offset = 0;
	task3.scsi.transfer_len = 512;
	task3.scsi.lun = &lun1;
	iscsi_task_set_pdu(&task3, &pdu3);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task3, link);

	pdu4.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu4.bhs;
	scsi_req->read_bit = 1;
	task4.scsi.ref = 1;
	task4.current_data_offset = 0;
	task4.scsi.transfer_len = 512;
	task4.scsi.lun = &lun2;
	iscsi_task_set_pdu(&task4, &pdu4);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task4, link);

	pdu5.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu5.bhs;
	scsi_req->read_bit = 1;
	task5.scsi.ref = 1;
	task5.current_data_offset = 0;
	task5.scsi.transfer_len = 512;
	task5.scsi.lun = &lun1;
	iscsi_task_set_pdu(&task5, &pdu5);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task5, link);

	mgmt_pdu2.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;

	pdu6.cmd_sn = alloc_cmd_sn;
	alloc_cmd_sn++;
	scsi_req = (struct iscsi_bhs_scsi_req *)&pdu6.bhs;
	scsi_req->read_bit = 1;
	task6.scsi.ref = 1;
	task6.current_data_offset = 0;
	task6.scsi.transfer_len = 512;
	task6.scsi.lun = &lun2;
	iscsi_task_set_pdu(&task6, &pdu6);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task6, link);

	rc = iscsi_conn_abort_queued_datain_tasks(&conn, &lun1, &mgmt_pdu1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!datain_task_is_queued(&conn, &task1));
	CU_ASSERT(datain_task_is_queued(&conn, &task2));
	CU_ASSERT(datain_task_is_queued(&conn, &task3));
	CU_ASSERT(datain_task_is_queued(&conn, &task4));
	CU_ASSERT(datain_task_is_queued(&conn, &task5));
	CU_ASSERT(datain_task_is_queued(&conn, &task6));

	rc = iscsi_conn_abort_queued_datain_tasks(&conn, &lun2, &mgmt_pdu2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!datain_task_is_queued(&conn, &task2));
	CU_ASSERT(datain_task_is_queued(&conn, &task3));
	CU_ASSERT(!datain_task_is_queued(&conn, &task4));
	CU_ASSERT(datain_task_is_queued(&conn, &task5));
	CU_ASSERT(datain_task_is_queued(&conn, &task6));

	CU_ASSERT(task1.scsi.ref == 0);
	CU_ASSERT(task2.scsi.ref == 0);
	CU_ASSERT(task3.scsi.ref == 1);
	CU_ASSERT(task4.scsi.ref == 0);
	CU_ASSERT(task5.scsi.ref == 1);
	CU_ASSERT(task6.scsi.ref == 1);
	CU_ASSERT(subtask.scsi.ref == 0);

	g_new_task = NULL;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("conn_suite", NULL, NULL);

	CU_ADD_TEST(suite, read_task_split_in_order_case);
	CU_ADD_TEST(suite, read_task_split_reverse_order_case);
	CU_ADD_TEST(suite, propagate_scsi_error_status_for_split_read_tasks);
	CU_ADD_TEST(suite, process_non_read_task_completion_test);
	CU_ADD_TEST(suite, free_tasks_on_connection);
	CU_ADD_TEST(suite, free_tasks_with_queued_datain);
	CU_ADD_TEST(suite, abort_queued_datain_task_test);
	CU_ADD_TEST(suite, abort_queued_datain_tasks_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
