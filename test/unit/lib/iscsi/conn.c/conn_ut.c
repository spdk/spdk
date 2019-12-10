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

struct spdk_trace_histories *g_trace_histories;
DEFINE_STUB_V(spdk_trace_add_register_fn, (struct spdk_trace_register_fn *reg_fn));
DEFINE_STUB_V(spdk_trace_register_owner, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_object, (uint8_t type, char id_prefix));
DEFINE_STUB_V(spdk_trace_register_description, (const char *name,
		uint16_t tpoint_id, uint8_t owner_type, uint8_t object_type, uint8_t new_object,
		uint8_t arg1_type, const char *arg1_name));
DEFINE_STUB_V(_spdk_trace_record, (uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
				   uint32_t size, uint64_t object_id, uint64_t arg1));

struct spdk_scsi_lun {
	uint8_t reserved;
};

struct spdk_iscsi_globals g_spdk_iscsi;
static TAILQ_HEAD(read_tasks_head, spdk_iscsi_task) g_ut_read_tasks =
	TAILQ_HEAD_INITIALIZER(g_ut_read_tasks);
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

DEFINE_STUB(spdk_iscsi_task_get, struct spdk_iscsi_task *,
	    (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *parent,
	     spdk_scsi_task_cpl cpl_fn), NULL);

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

DEFINE_STUB_V(spdk_scsi_task_set_data, (struct spdk_scsi_task *task, void *data, uint32_t len));

DEFINE_STUB_V(spdk_scsi_task_process_null_lun, (struct spdk_scsi_task *task));

DEFINE_STUB_V(spdk_scsi_task_process_abort, (struct spdk_scsi_task *task));

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

DEFINE_STUB_V(spdk_iscsi_queue_task,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(spdk_iscsi_task_response,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(spdk_iscsi_task_mgmt_response,
	      (struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task));

DEFINE_STUB_V(spdk_iscsi_send_nopin, (struct spdk_iscsi_conn *conn));

DEFINE_STUB(spdk_del_transfer_task, bool,
	    (struct spdk_iscsi_conn *conn, uint32_t task_tag), true);

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
		if (primary->current_datain_offset < primary->scsi.transfer_len) {
			remaining_size = primary->scsi.transfer_len - primary->current_datain_offset;

			subtask = ut_conn_task_get(primary);

			subtask->scsi.offset = primary->current_datain_offset;
			subtask->scsi.length = spdk_min(SPDK_BDEV_LARGE_BUF_MAX_SIZE, remaining_size);
			subtask->scsi.status = SPDK_SCSI_STATUS_GOOD;

			primary->current_datain_offset += subtask->scsi.length;

			TAILQ_INSERT_TAIL(&g_ut_read_tasks, subtask, link);
		}

		if (primary->current_datain_offset == primary->scsi.transfer_len) {
			break;
		}
	}
}

static void
read_task_split_in_order_case(void)
{
	struct spdk_iscsi_task primary = {};
	struct spdk_iscsi_task *task, *tmp;

	primary.scsi.transfer_len = SPDK_BDEV_LARGE_BUF_MAX_SIZE * 8;
	TAILQ_INIT(&primary.subtask_list);
	primary.current_datain_offset = 0;
	primary.bytes_completed = 0;
	primary.scsi.ref = 1;

	ut_conn_create_read_tasks(&primary);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_ut_read_tasks));

	TAILQ_FOREACH(task, &g_ut_read_tasks, link) {
		CU_ASSERT(&primary == spdk_iscsi_task_get_primary(task));
		process_read_task_completion(NULL, task, &primary);
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

	primary.scsi.transfer_len = SPDK_BDEV_LARGE_BUF_MAX_SIZE * 8;
	TAILQ_INIT(&primary.subtask_list);
	primary.current_datain_offset = 0;
	primary.bytes_completed = 0;
	primary.scsi.ref = 1;

	ut_conn_create_read_tasks(&primary);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_ut_read_tasks));

	TAILQ_FOREACH_REVERSE(task, &g_ut_read_tasks, read_tasks_head, link) {
		CU_ASSERT(&primary == spdk_iscsi_task_get_primary(task));
		process_read_task_completion(NULL, task, &primary);
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
ut_init_read_subtask(struct spdk_iscsi_task *task, uint32_t offset, uint32_t length,
		     enum spdk_scsi_status status, struct spdk_iscsi_task *primary)
{
	task->scsi.offset = offset;
	task->scsi.length = length;
	task->scsi.status = status;
	task->scsi.ref = 1;
	task->parent = primary;
}

static void
ut_check_read_subtask_completion(struct spdk_iscsi_task *task, enum spdk_scsi_status status)
{
	CU_ASSERT(task->scsi.status == status);
	CU_ASSERT(task->scsi.ref == 0);
}

static void
propagate_scsi_error_status_for_split_read_tasks(void)
{
	struct spdk_iscsi_task primary = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, task3 = {}, task4 = {}, task5 = {}, task6 = {};

	primary.scsi.transfer_len = 512 * 6;
	primary.rsp_scsi_status = SPDK_SCSI_STATUS_GOOD;
	TAILQ_INIT(&primary.subtask_list);
	primary.scsi.ref = 7;

	ut_init_read_subtask(&task1, 0, 512, SPDK_SCSI_STATUS_GOOD, &primary);
	ut_init_read_subtask(&task2, 512, 512, SPDK_SCSI_STATUS_CHECK_CONDITION, &primary);
	ut_init_read_subtask(&task3, 512 * 2, 512, SPDK_SCSI_STATUS_GOOD, &primary);
	ut_init_read_subtask(&task4, 512 * 3, 512, SPDK_SCSI_STATUS_GOOD, &primary);
	ut_init_read_subtask(&task5, 512 * 4, 512, SPDK_SCSI_STATUS_GOOD, &primary);
	ut_init_read_subtask(&task6, 512 * 5, 512, SPDK_SCSI_STATUS_GOOD, &primary);

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
	CU_ASSERT(primary.bytes_completed == primary.scsi.transfer_len);
	CU_ASSERT(TAILQ_EMPTY(&primary.subtask_list));
	CU_ASSERT(primary.scsi.ref == 0);
	ut_check_read_subtask_completion(&task1, SPDK_SCSI_STATUS_CHECK_CONDITION);
	ut_check_read_subtask_completion(&task2, SPDK_SCSI_STATUS_CHECK_CONDITION);
	ut_check_read_subtask_completion(&task3, SPDK_SCSI_STATUS_CHECK_CONDITION);
	ut_check_read_subtask_completion(&task4, SPDK_SCSI_STATUS_CHECK_CONDITION);
	ut_check_read_subtask_completion(&task5, SPDK_SCSI_STATUS_CHECK_CONDITION);
	ut_check_read_subtask_completion(&task6, SPDK_SCSI_STATUS_CHECK_CONDITION);
}

static void
ut_init_non_read_subtask(struct spdk_iscsi_task *task, uint32_t length,
			 enum spdk_scsi_status status,
			 struct spdk_iscsi_task *primary)
{
	task->scsi.length = length;
	task->scsi.data_transferred = length;
	task->scsi.status = status;
	task->scsi.ref = 1;
	task->parent = primary;
	primary->scsi.ref++;
}

static void
ut_check_non_read_primary_completion(struct spdk_iscsi_task *task,
				     uint32_t bytes_completed, uint32_t data_transferred,
				     enum spdk_scsi_status status, uint32_t ref)
{
	CU_ASSERT(task->bytes_completed == bytes_completed);
	CU_ASSERT(task->scsi.data_transferred == data_transferred);
	CU_ASSERT(task->rsp_scsi_status == status);
	CU_ASSERT(task->scsi.ref == ref);
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
	primary.rsp_scsi_status = SPDK_SCSI_STATUS_GOOD;
	primary.scsi.ref = 1;
	TAILQ_INSERT_TAIL(&conn.active_r2t_tasks, &primary, link);
	primary.is_r2t_active = true;

	/* First subtask which failed. */
	ut_init_non_read_subtask(&task, 4096, SPDK_SCSI_STATUS_CHECK_CONDITION, &primary);

	process_non_read_task_completion(&conn, &task, &primary);
	CU_ASSERT(!TAILQ_EMPTY(&conn.active_r2t_tasks));
	CU_ASSERT(task.scsi.ref == 0);
	ut_check_non_read_primary_completion(&primary, 4096, 0, SPDK_SCSI_STATUS_CHECK_CONDITION, 1);

	/* Second subtask which succeeded. */
	ut_init_non_read_subtask(&task, 4096, SPDK_SCSI_STATUS_GOOD, &primary);

	process_non_read_task_completion(&conn, &task, &primary);
	CU_ASSERT(!TAILQ_EMPTY(&conn.active_r2t_tasks));
	CU_ASSERT(task.scsi.ref == 0);
	ut_check_non_read_primary_completion(&primary, 4096 * 2, 4096, SPDK_SCSI_STATUS_CHECK_CONDITION, 1);

	/* Third and final subtask which succeeded. */
	ut_init_non_read_subtask(&task, 4096, SPDK_SCSI_STATUS_GOOD, &primary);

	process_non_read_task_completion(&conn, &task, &primary);
	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));
	CU_ASSERT(task.scsi.ref == 0);
	ut_check_non_read_primary_completion(&primary, 4096 * 3, 4096 * 2, SPDK_SCSI_STATUS_CHECK_CONDITION,
					     0);

	/* Tricky case when the last task completed was the initial task. */
	primary.scsi.length = 4096;
	primary.bytes_completed = 4096 * 2;
	primary.scsi.data_transferred = 4096 * 2;
	primary.scsi.status = SPDK_SCSI_STATUS_GOOD;
	primary.rsp_scsi_status = SPDK_SCSI_STATUS_GOOD;
	primary.scsi.ref = 2;
	TAILQ_INSERT_TAIL(&conn.active_r2t_tasks, &primary, link);
	primary.is_r2t_active = true;

	process_non_read_task_completion(&conn, &primary, &primary);
	CU_ASSERT(TAILQ_EMPTY(&conn.active_r2t_tasks));
	ut_check_non_read_primary_completion(&primary, 4096 * 3, 4096 * 2, SPDK_SCSI_STATUS_GOOD, 0);

	/* Further tricky case when the last task completed was the initial task,
	 * and the R2T was already terminated.
	 */
	primary.scsi.length = 4096;
	primary.bytes_completed = 4096 * 2;
	primary.scsi.data_transferred = 4096 * 2;
	primary.scsi.status = SPDK_SCSI_STATUS_GOOD;
	primary.rsp_scsi_status = SPDK_SCSI_STATUS_GOOD;
	primary.scsi.ref = 1;
	primary.is_r2t_active = false;

	process_non_read_task_completion(&conn, &primary, &primary);

	ut_check_non_read_primary_completion(&primary, 4096 * 3, 4096 * 2, SPDK_SCSI_STATUS_GOOD, 0);
}

static void
ut_init_flushed_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
		    struct spdk_iscsi_task *task, enum iscsi_op opcode,
		    uint32_t offset, uint32_t data_len)
{
	pdu->bhs.opcode = opcode;
	task->scsi.offset = offset;
	DSET24(pdu->bhs.data_segment_len, data_len);

	g_sock_writev_bytes += ISCSI_BHS_LEN + data_len;

	pdu->task = task;
	task->scsi.ref = 1;
	TAILQ_INSERT_TAIL(&conn->write_pdu_list, pdu, tailq);
	conn->data_in_cnt++;
}

static void
recursive_flush_pdus_calls(void)
{
	struct spdk_iscsi_pdu pdu1 = {}, pdu2 = {}, pdu3 = {};
	struct spdk_iscsi_task task1 = {}, task2 = {}, task3 = {};
	struct spdk_iscsi_conn conn = {};
	int rc;

	TAILQ_INIT(&conn.write_pdu_list);

	ut_init_flushed_pdu(&conn, &pdu1, &task1, ISCSI_OP_SCSI_DATAIN, 512, 512);
	ut_init_flushed_pdu(&conn, &pdu2, &task2, ISCSI_OP_SCSI_DATAIN, 512 * 2, 512);
	ut_init_flushed_pdu(&conn, &pdu3, &task3, ISCSI_OP_SCSI_DATAIN, 512 * 3, 512);

	rc = iscsi_conn_flush_pdus_internal(&conn);
	CU_ASSERT(rc == 0);

	CU_ASSERT(task1.scsi.ref == 0);
	CU_ASSERT(task2.scsi.ref == 0);
	CU_ASSERT(task3.scsi.ref == 0);
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
	conn.data_in_cnt = MAX_LARGE_DATAIN_PER_CONNECTION;

	pdu1.task = &task1;
	pdu2.task = &task2;
	pdu3.task = &task3;

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

	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task4, link);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task5, link);
	TAILQ_INSERT_TAIL(&conn.queued_datain_tasks, &task6, link);

	iscsi_conn_free_tasks(&conn);

	CU_ASSERT(TAILQ_EMPTY(&conn.write_pdu_list));
	CU_ASSERT(TAILQ_EMPTY(&conn.queued_datain_tasks));
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("conn_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "read task split in order", read_task_split_in_order_case) == NULL ||
		CU_add_test(suite, "read task split reverse order",
			    read_task_split_reverse_order_case) == NULL ||
		CU_add_test(suite, "propagate_scsi_error_status_for_split_read_tasks",
			    propagate_scsi_error_status_for_split_read_tasks) == NULL ||
		CU_add_test(suite, "process_non_read_task_completion_test",
			    process_non_read_task_completion_test) == NULL ||
		CU_add_test(suite, "recursive_flush_pdus_calls", recursive_flush_pdus_calls) == NULL ||
		CU_add_test(suite, "free_tasks_on_connection", free_tasks_on_connection) == NULL ||
		CU_add_test(suite, "free_tasks_with_queued_datain", free_tasks_with_queued_datain) == NULL
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
