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

SPDK_LOG_REGISTER_COMPONENT("iscsi", SPDK_LOG_ISCSI)

#define DMIN32(A,B) ((uint32_t) ((uint32_t)(A) > (uint32_t)(B) ? (uint32_t)(B) : (uint32_t)(A)))

struct spdk_iscsi_globals g_spdk_iscsi;
static TAILQ_HEAD(, spdk_iscsi_task) g_ut_read_tasks = TAILQ_HEAD_INITIALIZER(g_ut_read_tasks);

int
spdk_app_get_shm_id(void)
{
	return 0;
}

uint32_t
spdk_env_get_current_core(void)
{
	return 0;
}

uint32_t
spdk_env_get_first_core(void)
{
	return 0;
}

uint32_t
spdk_env_get_last_core(void)
{
	return 0;
}

uint32_t
spdk_env_get_next_core(uint32_t prev_core)
{
	return 0;
}

struct spdk_event *
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	return NULL;
}

void
spdk_event_call(struct spdk_event *event)
{
}

int
spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
		  char *caddr, int clen, uint16_t *cport)
{
	return 0;
}

int
spdk_sock_close(struct spdk_sock **sock)
{
	*sock = NULL;
	return 0;
}

ssize_t
spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	return 0;
}

ssize_t
spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	return 0;
}

int
spdk_sock_set_recvlowat(struct spdk_sock *s, int nbytes)
{
	return 0;
}

int
spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz)
{
	return 0;
}

int
spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz)
{
	return 0;
}

int
spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			 spdk_sock_cb cb_fn, void *cb_arg)
{
	return 0;
}

int
spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock)
{
	return 0;
}

void
spdk_scsi_task_put(struct spdk_scsi_task *task)
{
}

struct spdk_scsi_lun *
spdk_scsi_dev_get_lun(struct spdk_scsi_dev *dev, int lun_id)
{
	return NULL;
}

bool
spdk_scsi_dev_has_pending_tasks(const struct spdk_scsi_dev *dev)
{
	return true;
}

int
spdk_scsi_lun_open(struct spdk_scsi_lun *lun, spdk_scsi_remove_cb_t hotremove_cb,
		   void *hotremove_ctx, struct spdk_scsi_desc **desc)
{
	return 0;
}

void
spdk_scsi_lun_close(struct spdk_scsi_desc *desc)
{
}

int spdk_scsi_lun_allocate_io_channel(struct spdk_scsi_desc *desc)
{
	return 0;
}

void spdk_scsi_lun_free_io_channel(struct spdk_scsi_desc *desc)
{
}

int
spdk_scsi_lun_get_id(const struct spdk_scsi_lun *lun)
{
	return 0;
}

const char *
spdk_scsi_port_get_name(const struct spdk_scsi_port *port)
{
	return NULL;
}

void
spdk_scsi_task_copy_status(struct spdk_scsi_task *dst,
			   struct spdk_scsi_task *src)
{
}

void
spdk_put_pdu(struct spdk_iscsi_pdu *pdu)
{
}

void
spdk_iscsi_param_free(struct iscsi_param *params)
{
}

int
spdk_iscsi_conn_params_init(struct iscsi_param **params)
{
	return 0;
}

void spdk_clear_all_transfer_task(struct spdk_iscsi_conn *conn,
				  struct spdk_scsi_lun *lun)
{
}

int
spdk_iscsi_build_iovecs(struct spdk_iscsi_conn *conn, struct iovec *iovec,
			struct spdk_iscsi_pdu *pdu)
{
	return 0;
}

bool spdk_iscsi_is_deferred_free_pdu(struct spdk_iscsi_pdu *pdu)
{
	return false;
}

void spdk_iscsi_task_response(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_task *task)
{
}

void
spdk_iscsi_task_mgmt_response(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_task *task)
{
}

void spdk_iscsi_send_nopin(struct spdk_iscsi_conn *conn)
{
}

int
spdk_iscsi_execute(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	return 0;
}

void spdk_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t task_tag)
{
}

int spdk_iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn)
{
	return 0;
}

int
spdk_iscsi_read_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu **_pdu)
{
	return 0;
}

void spdk_free_sess(struct spdk_iscsi_sess *sess)
{
}

int
spdk_iscsi_tgt_node_cleanup_luns(struct spdk_iscsi_conn *conn,
				 struct spdk_iscsi_tgt_node *target)
{
	return 0;
}

void
spdk_shutdown_iscsi_conns_done(void)
{
}

static struct spdk_iscsi_task *
ut_conn_task_get(struct spdk_iscsi_task *parent)
{
	struct spdk_iscsi_task *task;

	task = calloc(1, sizeof(*task));
	SPDK_CU_ASSERT_FATAL(task != NULL);

	if (parent) {
		task->parent = parent;
	}
	return task;
}

static void
ut_conn_create_read_tasks(int transfer_len)
{
	struct spdk_iscsi_task *task, *subtask;
	int32_t remaining_size = 0;

	task = ut_conn_task_get(NULL);

	task->scsi.transfer_len = transfer_len;
	task->scsi.offset = 0;
	task->scsi.length = DMIN32(SPDK_BDEV_LARGE_BUF_MAX_SIZE, task->scsi.transfer_len);
	task->scsi.status = SPDK_SCSI_STATUS_GOOD;

	remaining_size = task->scsi.transfer_len - task->scsi.length;
	task->current_datain_offset = 0;

	if (remaining_size == 0) {
		TAILQ_INSERT_TAIL(&g_ut_read_tasks, task, link);
		return;
	}

	while (1) {
		if (task->current_datain_offset == 0) {
			task->current_datain_offset = task->scsi.length;
			TAILQ_INSERT_TAIL(&g_ut_read_tasks, task, link);
			continue;
		}

		if (task->current_datain_offset < task->scsi.transfer_len) {
			remaining_size = task->scsi.transfer_len - task->current_datain_offset;

			subtask = ut_conn_task_get(task);

			subtask->scsi.offset = task->current_datain_offset;
			subtask->scsi.length = DMIN32(SPDK_BDEV_LARGE_BUF_MAX_SIZE, remaining_size);
			subtask->scsi.status = SPDK_SCSI_STATUS_GOOD;

			task->current_datain_offset += subtask->scsi.length;

			TAILQ_INSERT_TAIL(&g_ut_read_tasks, subtask, link);
		}

		if (task->current_datain_offset == task->scsi.transfer_len) {
			break;
		}
	}
}

static void
read_task_split_in_order_case(void)
{
	struct spdk_iscsi_task *primary, *task, *tmp;

	ut_conn_create_read_tasks(SPDK_BDEV_LARGE_BUF_MAX_SIZE * 8);

	TAILQ_FOREACH(task, &g_ut_read_tasks, link) {
		primary = spdk_iscsi_task_get_primary(task);
		process_read_task_completion(NULL, task, primary);
	}

	primary = TAILQ_FIRST(&g_ut_read_tasks);
	SPDK_CU_ASSERT_FATAL(primary != NULL);

	if (primary != NULL) {
		CU_ASSERT(primary->bytes_completed == primary->scsi.transfer_len);
	}

	TAILQ_FOREACH_SAFE(task, &g_ut_read_tasks, link, tmp) {
		TAILQ_REMOVE(&g_ut_read_tasks, task, link);
		free(task);
	}

	CU_ASSERT(TAILQ_EMPTY(&g_ut_read_tasks));
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
		CU_add_test(suite, "read task split in order", read_task_split_in_order_case) == NULL
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
