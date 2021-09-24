/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/trace.h"
#include "spdk/sock.h"
#include "spdk/string.h"

#include "spdk/log.h"

#include "iscsi/task.h"
#include "iscsi/conn.h"
#include "iscsi/tgt_node.h"
#include "iscsi/portal_grp.h"

#define MAKE_DIGEST_WORD(BUF, CRC32C) \
        (   ((*((uint8_t *)(BUF)+0)) = (uint8_t)((uint32_t)(CRC32C) >> 0)), \
            ((*((uint8_t *)(BUF)+1)) = (uint8_t)((uint32_t)(CRC32C) >> 8)), \
            ((*((uint8_t *)(BUF)+2)) = (uint8_t)((uint32_t)(CRC32C) >> 16)), \
            ((*((uint8_t *)(BUF)+3)) = (uint8_t)((uint32_t)(CRC32C) >> 24)))

#define SPDK_ISCSI_CONNECTION_MEMSET(conn)		\
	memset(&(conn)->portal, 0, sizeof(*(conn)) -	\
		offsetof(struct spdk_iscsi_conn, portal));

#define SPDK_ISCSI_CONNECTION_STATUS(status, rnstr) case(status): return(rnstr)

static struct spdk_iscsi_conn *g_conns_array = NULL;

static TAILQ_HEAD(, spdk_iscsi_conn) g_free_conns = TAILQ_HEAD_INITIALIZER(g_free_conns);
static TAILQ_HEAD(, spdk_iscsi_conn) g_active_conns = TAILQ_HEAD_INITIALIZER(g_active_conns);

static pthread_mutex_t g_conns_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_poller *g_shutdown_timer = NULL;

static void iscsi_conn_sock_cb(void *arg, struct spdk_sock_group *group,
			       struct spdk_sock *sock);

static struct spdk_iscsi_conn *
allocate_conn(void)
{
	struct spdk_iscsi_conn	*conn;

	pthread_mutex_lock(&g_conns_mutex);
	conn = TAILQ_FIRST(&g_free_conns);
	if (conn != NULL) {
		assert(!conn->is_valid);
		TAILQ_REMOVE(&g_free_conns, conn, conn_link);
		SPDK_ISCSI_CONNECTION_MEMSET(conn);
		conn->is_valid = 1;

		TAILQ_INSERT_TAIL(&g_active_conns, conn, conn_link);
	}
	pthread_mutex_unlock(&g_conns_mutex);

	return conn;
}

static void
_free_conn(struct spdk_iscsi_conn *conn)
{
	TAILQ_REMOVE(&g_active_conns, conn, conn_link);

	memset(conn->portal_host, 0, sizeof(conn->portal_host));
	memset(conn->portal_port, 0, sizeof(conn->portal_port));
	conn->is_valid = 0;

	TAILQ_INSERT_TAIL(&g_free_conns, conn, conn_link);
}

static void
free_conn(struct spdk_iscsi_conn *conn)
{
	pthread_mutex_lock(&g_conns_mutex);
	_free_conn(conn);
	pthread_mutex_unlock(&g_conns_mutex);
}

static void
_iscsi_conns_cleanup(void)
{
	free(g_conns_array);
}

int initialize_iscsi_conns(void)
{
	uint32_t i;

	SPDK_DEBUGLOG(iscsi, "spdk_iscsi_init\n");

	g_conns_array = calloc(MAX_ISCSI_CONNECTIONS, sizeof(struct spdk_iscsi_conn));
	if (g_conns_array == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		g_conns_array[i].id = i;
		TAILQ_INSERT_TAIL(&g_free_conns, &g_conns_array[i], conn_link);
	}

	return 0;
}

static void
iscsi_poll_group_add_conn(struct spdk_iscsi_poll_group *pg, struct spdk_iscsi_conn *conn)
{
	int rc;

	rc = spdk_sock_group_add_sock(pg->sock_group, conn->sock, iscsi_conn_sock_cb, conn);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add sock=%p of conn=%p\n", conn->sock, conn);
		return;
	}

	conn->is_stopped = false;
	STAILQ_INSERT_TAIL(&pg->connections, conn, pg_link);
}

static void
iscsi_poll_group_remove_conn(struct spdk_iscsi_poll_group *pg, struct spdk_iscsi_conn *conn)
{
	int rc;

	assert(conn->sock != NULL);
	rc = spdk_sock_group_remove_sock(pg->sock_group, conn->sock);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to remove sock=%p of conn=%p\n", conn->sock, conn);
	}

	conn->is_stopped = true;
	STAILQ_REMOVE(&pg->connections, conn, spdk_iscsi_conn, pg_link);
}

static int
login_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->state < ISCSI_CONN_STATE_EXITING) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	}
	spdk_poller_unregister(&conn->login_timer);

	return SPDK_POLLER_BUSY;
}

static void
iscsi_conn_start(void *ctx)
{
	struct spdk_iscsi_conn *conn = ctx;

	iscsi_poll_group_add_conn(conn->pg, conn);

	conn->login_timer = SPDK_POLLER_REGISTER(login_timeout, conn, ISCSI_LOGIN_TIMEOUT * 1000000);
}

int
iscsi_conn_construct(struct spdk_iscsi_portal *portal,
		     struct spdk_sock *sock)
{
	struct spdk_iscsi_poll_group *pg;
	struct spdk_iscsi_conn *conn;
	int i, rc;

	conn = allocate_conn();
	if (conn == NULL) {
		SPDK_ERRLOG("Could not allocate connection.\n");
		return -1;
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	conn->timeout = g_iscsi.timeout * spdk_get_ticks_hz(); /* seconds to TSC */
	conn->nopininterval = g_iscsi.nopininterval;
	conn->nopininterval *= spdk_get_ticks_hz(); /* seconds to TSC */
	conn->last_nopin = spdk_get_ticks();
	conn->nop_outstanding = false;
	conn->data_out_cnt = 0;
	conn->data_in_cnt = 0;
	conn->disable_chap = portal->group->disable_chap;
	conn->require_chap = portal->group->require_chap;
	conn->mutual_chap = portal->group->mutual_chap;
	conn->chap_group = portal->group->chap_group;
	pthread_mutex_unlock(&g_iscsi.mutex);
	conn->MaxRecvDataSegmentLength = 8192; /* RFC3720(12.12) */

	conn->portal = portal;
	conn->pg_tag = portal->group->tag;
	memcpy(conn->portal_host, portal->host, strlen(portal->host));
	memcpy(conn->portal_port, portal->port, strlen(portal->port));
	conn->sock = sock;

	conn->state = ISCSI_CONN_STATE_INVALID;
	conn->login_phase = ISCSI_SECURITY_NEGOTIATION_PHASE;
	conn->ttt = 0;

	conn->partial_text_parameter = NULL;

	for (i = 0; i < MAX_CONNECTION_PARAMS; i++) {
		conn->conn_param_state_negotiated[i] = false;
	}

	for (i = 0; i < MAX_SESSION_PARAMS; i++) {
		conn->sess_param_state_negotiated[i] = false;
	}

	conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_READY;

	TAILQ_INIT(&conn->write_pdu_list);
	TAILQ_INIT(&conn->snack_pdu_list);
	TAILQ_INIT(&conn->queued_r2t_tasks);
	TAILQ_INIT(&conn->active_r2t_tasks);
	TAILQ_INIT(&conn->queued_datain_tasks);
	TAILQ_INIT(&conn->luns);

	rc = spdk_sock_getaddr(sock, conn->target_addr, sizeof conn->target_addr, NULL,
			       conn->initiator_addr, sizeof conn->initiator_addr, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed\n");
		goto error_return;
	}

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(conn->sock, 1);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed\n");
		goto error_return;
	}

	/* set default params */
	rc = iscsi_conn_params_init(&conn->params);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_conn_params_init() failed\n");
		goto error_return;
	}
	conn->logout_request_timer = NULL;
	conn->logout_timer = NULL;
	conn->shutdown_timer = NULL;
	SPDK_DEBUGLOG(iscsi, "Launching connection on acceptor thread\n");
	conn->pending_task_cnt = 0;

	/* Get the first poll group. */
	pg = TAILQ_FIRST(&g_iscsi.poll_group_head);
	if (pg == NULL) {
		SPDK_ERRLOG("There is no poll group.\n");
		assert(false);
		goto error_return;
	}

	conn->pg = pg;
	spdk_thread_send_msg(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(pg)),
			     iscsi_conn_start, conn);
	return 0;

error_return:
	iscsi_param_free(conn->params);
	free_conn(conn);
	return -1;
}

void
iscsi_conn_free_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	iscsi_conn_xfer_complete_cb cb_fn;
	void *cb_arg;

	cb_fn = pdu->cb_fn;
	cb_arg = pdu->cb_arg;

	assert(cb_fn != NULL);
	pdu->cb_fn = NULL;

	if (pdu->task) {
		iscsi_task_put(pdu->task);
	}
	iscsi_put_pdu(pdu);

	cb_fn(cb_arg);
}

static int
iscsi_conn_free_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu, *tmp_pdu;
	struct spdk_iscsi_task *iscsi_task, *tmp_iscsi_task;

	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, tmp_pdu) {
		TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
		iscsi_conn_free_pdu(conn, pdu);
	}

	TAILQ_FOREACH_SAFE(iscsi_task, &conn->queued_datain_tasks, link, tmp_iscsi_task) {
		if (!iscsi_task->is_queued) {
			TAILQ_REMOVE(&conn->queued_datain_tasks, iscsi_task, link);
			iscsi_task_put(iscsi_task);
		}
	}

	/* We have to parse conn->write_pdu_list in the end.  In iscsi_conn_free_pdu(),
	 *  iscsi_conn_handle_queued_datain_tasks() may be called, and
	 *  iscsi_conn_handle_queued_datain_tasks() will parse conn->queued_datain_tasks
	 *  and may stack some PDUs to conn->write_pdu_list.  Hence when we come here, we
	 *  have to ensure there is no associated task in conn->queued_datain_tasks.
	 */
	TAILQ_FOREACH_SAFE(pdu, &conn->write_pdu_list, tailq, tmp_pdu) {
		TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);
		iscsi_conn_free_pdu(conn, pdu);
	}

	if (conn->pending_task_cnt) {
		return -1;
	}

	return 0;
}

static void
iscsi_conn_cleanup_backend(struct spdk_iscsi_conn *conn)
{
	int rc;
	struct spdk_iscsi_tgt_node *target;

	if (conn->sess->connections > 1) {
		/* connection specific cleanup */
	} else if (!g_iscsi.AllowDuplicateIsid) {
		/* clean up all tasks to all LUNs for session */
		target = conn->sess->target;
		if (target != NULL) {
			rc = iscsi_tgt_node_cleanup_luns(conn, target);
			if (rc < 0) {
				SPDK_ERRLOG("target abort failed\n");
			}
		}
	}
}

static void
iscsi_conn_free(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_sess *sess;
	int idx;
	uint32_t i;

	pthread_mutex_lock(&g_conns_mutex);

	if (conn->sess == NULL) {
		goto end;
	}

	idx = -1;
	sess = conn->sess;
	conn->sess = NULL;

	for (i = 0; i < sess->connections; i++) {
		if (sess->conns[i] == conn) {
			idx = i;
			break;
		}
	}

	if (idx < 0) {
		SPDK_ERRLOG("remove conn not found\n");
	} else {
		for (i = idx; i < sess->connections - 1; i++) {
			sess->conns[i] = sess->conns[i + 1];
		}
		sess->conns[sess->connections - 1] = NULL;
		sess->connections--;

		if (sess->connections == 0) {
			/* cleanup last connection */
			SPDK_DEBUGLOG(iscsi,
				      "cleanup last conn free sess\n");
			iscsi_free_sess(sess);
		}
	}

	SPDK_DEBUGLOG(iscsi, "Terminating connections(tsih %d): %d\n",
		      sess->tsih, sess->connections);

end:
	SPDK_DEBUGLOG(iscsi, "cleanup free conn\n");
	iscsi_param_free(conn->params);
	_free_conn(conn);

	pthread_mutex_unlock(&g_conns_mutex);
}

static void
iscsi_conn_close_lun(struct spdk_iscsi_conn *conn,
		     struct spdk_iscsi_lun *iscsi_lun)
{
	if (iscsi_lun == NULL) {
		return;
	}

	spdk_scsi_lun_free_io_channel(iscsi_lun->desc);
	spdk_scsi_lun_close(iscsi_lun->desc);
	spdk_poller_unregister(&iscsi_lun->remove_poller);

	TAILQ_REMOVE(&conn->luns, iscsi_lun, tailq);

	free(iscsi_lun);

}

static void
iscsi_conn_close_luns(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_lun *iscsi_lun, *tmp;

	TAILQ_FOREACH_SAFE(iscsi_lun, &conn->luns, tailq, tmp) {
		iscsi_conn_close_lun(conn, iscsi_lun);
	}
}

static bool
iscsi_conn_check_tasks_for_lun(struct spdk_iscsi_conn *conn,
			       struct spdk_scsi_lun *lun)
{
	struct spdk_iscsi_pdu *pdu, *tmp_pdu;
	struct spdk_iscsi_task *task;

	assert(lun != NULL);

	/* We can remove deferred PDUs safely because they are already flushed. */
	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, tmp_pdu) {
		if (lun == pdu->task->scsi.lun) {
			TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
			iscsi_conn_free_pdu(conn, pdu);
		}
	}

	TAILQ_FOREACH(task, &conn->queued_datain_tasks, link) {
		if (lun == task->scsi.lun) {
			return false;
		}
	}

	/* This check loop works even when connection exits in the middle of LUN hotplug
	 *  because all PDUs in write_pdu_list are removed in iscsi_conn_free_tasks().
	 */
	TAILQ_FOREACH(pdu, &conn->write_pdu_list, tailq) {
		if (pdu->task && lun == pdu->task->scsi.lun) {
			return false;
		}
	}

	return true;
}

static int
iscsi_conn_remove_lun(void *ctx)
{
	struct spdk_iscsi_lun *iscsi_lun = ctx;
	struct spdk_iscsi_conn *conn = iscsi_lun->conn;
	struct spdk_scsi_lun *lun = iscsi_lun->lun;

	if (!iscsi_conn_check_tasks_for_lun(conn, lun)) {
		return SPDK_POLLER_BUSY;
	}
	iscsi_conn_close_lun(conn, iscsi_lun);
	return SPDK_POLLER_BUSY;
}

static void
_iscsi_conn_hotremove_lun(void *ctx)
{
	struct spdk_iscsi_lun *iscsi_lun = ctx;
	struct spdk_iscsi_conn *conn = iscsi_lun->conn;
	struct spdk_scsi_lun *lun = iscsi_lun->lun;

	assert(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)) ==
	       spdk_get_thread());

	/* If a connection is already in stating status, just return */
	if (conn->state >= ISCSI_CONN_STATE_EXITING) {
		return;
	}

	iscsi_clear_all_transfer_task(conn, lun, NULL);

	iscsi_lun->remove_poller = SPDK_POLLER_REGISTER(iscsi_conn_remove_lun, iscsi_lun,
				   1000);
}

static void
iscsi_conn_hotremove_lun(struct spdk_scsi_lun *lun, void *remove_ctx)
{
	struct spdk_iscsi_lun *iscsi_lun = remove_ctx;
	struct spdk_iscsi_conn *conn = iscsi_lun->conn;

	spdk_thread_send_msg(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)),
			     _iscsi_conn_hotremove_lun, iscsi_lun);
}

static int
iscsi_conn_open_lun(struct spdk_iscsi_conn *conn, struct spdk_scsi_lun *lun)
{
	int rc;
	struct spdk_iscsi_lun *iscsi_lun;

	iscsi_lun = calloc(1, sizeof(*iscsi_lun));
	if (iscsi_lun == NULL) {
		return -ENOMEM;
	}

	iscsi_lun->conn = conn;
	iscsi_lun->lun = lun;

	rc = spdk_scsi_lun_open(lun, iscsi_conn_hotremove_lun, iscsi_lun, &iscsi_lun->desc);
	if (rc != 0) {
		free(iscsi_lun);
		return rc;
	}

	rc = spdk_scsi_lun_allocate_io_channel(iscsi_lun->desc);
	if (rc != 0) {
		spdk_scsi_lun_close(iscsi_lun->desc);
		free(iscsi_lun);
		return rc;
	}

	TAILQ_INSERT_TAIL(&conn->luns, iscsi_lun, tailq);

	return 0;
}

static void
iscsi_conn_open_luns(struct spdk_iscsi_conn *conn)
{
	int rc;
	struct spdk_scsi_lun *lun;

	for (lun = spdk_scsi_dev_get_first_lun(conn->dev); lun != NULL;
	     lun = spdk_scsi_dev_get_next_lun(lun)) {
		rc = iscsi_conn_open_lun(conn, lun);
		if (rc != 0) {
			goto error;
		}
	}

	return;

error:
	iscsi_conn_close_luns(conn);
}

/**
 *  This function will stop executing the specified connection.
 */
static void
iscsi_conn_stop(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_tgt_node *target;

	assert(conn->state == ISCSI_CONN_STATE_EXITED);
	assert(conn->data_in_cnt == 0);
	assert(conn->data_out_cnt == 0);

	if (conn->sess != NULL &&
	    conn->sess->session_type == SESSION_TYPE_NORMAL &&
	    conn->full_feature) {
		target = conn->sess->target;
		pthread_mutex_lock(&target->mutex);
		target->num_active_conns--;
		pthread_mutex_unlock(&target->mutex);

		iscsi_conn_close_luns(conn);
	}

	assert(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)) ==
	       spdk_get_thread());
}

static int
_iscsi_conn_check_shutdown(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	rc = iscsi_conn_free_tasks(conn);
	if (rc < 0) {
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&conn->shutdown_timer);

	iscsi_conn_stop(conn);
	iscsi_conn_free(conn);

	return SPDK_POLLER_BUSY;
}

static void
_iscsi_conn_destruct(struct spdk_iscsi_conn *conn)
{
	int rc;

	iscsi_poll_group_remove_conn(conn->pg, conn);
	spdk_sock_close(&conn->sock);
	iscsi_clear_all_transfer_task(conn, NULL, NULL);
	spdk_poller_unregister(&conn->logout_request_timer);
	spdk_poller_unregister(&conn->logout_timer);

	rc = iscsi_conn_free_tasks(conn);
	if (rc < 0) {
		/* The connection cannot be freed yet. Check back later. */
		conn->shutdown_timer = SPDK_POLLER_REGISTER(_iscsi_conn_check_shutdown, conn, 1000);
	} else {
		iscsi_conn_stop(conn);
		iscsi_conn_free(conn);
	}
}

static int
_iscsi_conn_check_pending_tasks(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->dev != NULL &&
	    spdk_scsi_dev_has_pending_tasks(conn->dev, conn->initiator_port)) {
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&conn->shutdown_timer);

	_iscsi_conn_destruct(conn);

	return SPDK_POLLER_BUSY;
}

void
iscsi_conn_destruct(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu;
	struct spdk_iscsi_task *task;
	int opcode;

	/* If a connection is already in exited status, just return */
	if (conn->state >= ISCSI_CONN_STATE_EXITED) {
		return;
	}

	conn->state = ISCSI_CONN_STATE_EXITED;

	/*
	 * Each connection pre-allocates its next PDU - make sure these get
	 *  freed here.
	 */
	pdu = conn->pdu_in_progress;
	if (pdu) {
		/* remove the task left in the PDU too. */
		task = pdu->task;
		if (task) {
			opcode = pdu->bhs.opcode;
			switch (opcode) {
			case ISCSI_OP_SCSI:
			case ISCSI_OP_SCSI_DATAOUT:
				spdk_scsi_task_process_abort(&task->scsi);
				iscsi_task_cpl(&task->scsi);
				break;
			default:
				SPDK_ERRLOG("unexpected opcode %x\n", opcode);
				iscsi_task_put(task);
				break;
			}
		}
		iscsi_put_pdu(pdu);
		conn->pdu_in_progress = NULL;
	}

	if (conn->sess != NULL && conn->pending_task_cnt > 0) {
		iscsi_conn_cleanup_backend(conn);
	}

	if (conn->dev != NULL &&
	    spdk_scsi_dev_has_pending_tasks(conn->dev, conn->initiator_port)) {
		conn->shutdown_timer = SPDK_POLLER_REGISTER(_iscsi_conn_check_pending_tasks, conn, 1000);
	} else {
		_iscsi_conn_destruct(conn);
	}
}

int
iscsi_get_active_conns(struct spdk_iscsi_tgt_node *target)
{
	struct spdk_iscsi_conn *conn;
	int num = 0;

	if (g_conns_array == MAP_FAILED) {
		return 0;
	}

	pthread_mutex_lock(&g_conns_mutex);
	TAILQ_FOREACH(conn, &g_active_conns, conn_link) {
		if (target == NULL || conn->target == target) {
			num++;
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);
	return num;
}

static void
iscsi_conn_check_shutdown_cb(void *arg1)
{
	_iscsi_conns_cleanup();
	shutdown_iscsi_conns_done();
}

static int
iscsi_conn_check_shutdown(void *arg)
{
	if (iscsi_get_active_conns(NULL) != 0) {
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&g_shutdown_timer);

	spdk_thread_send_msg(spdk_get_thread(), iscsi_conn_check_shutdown_cb, NULL);

	return SPDK_POLLER_BUSY;
}

static void
iscsi_send_logout_request(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_async *rsph;

	rsp_pdu = iscsi_get_pdu(conn);
	assert(rsp_pdu != NULL);

	rsph = (struct iscsi_bhs_async *)&rsp_pdu->bhs;
	rsp_pdu->data = NULL;

	rsph->opcode = ISCSI_OP_ASYNC;
	to_be32(&rsph->ffffffff, 0xFFFFFFFF);
	rsph->async_event = 1;
	to_be16(&rsph->param3, ISCSI_LOGOUT_REQUEST_TIMEOUT);

	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;
	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);
}

static int
logout_request_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->state < ISCSI_CONN_STATE_EXITING) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	}

	return SPDK_POLLER_BUSY;
}

/* If the connection is running and logout is not requested yet, request logout
 * to initiator and wait for the logout process to start.
 */
static void
_iscsi_conn_request_logout(void *ctx)
{
	struct spdk_iscsi_conn *conn = ctx;

	if (conn->state > ISCSI_CONN_STATE_RUNNING ||
	    conn->logout_request_timer != NULL) {
		return;
	}

	iscsi_send_logout_request(conn);

	conn->logout_request_timer = SPDK_POLLER_REGISTER(logout_request_timeout,
				     conn, ISCSI_LOGOUT_REQUEST_TIMEOUT * 1000000);
}

static void
iscsi_conn_request_logout(struct spdk_iscsi_conn *conn)
{
	struct spdk_thread *thread;

	if (conn->state == ISCSI_CONN_STATE_INVALID) {
		/* Move it to EXITING state if the connection is in login. */
		conn->state = ISCSI_CONN_STATE_EXITING;
	} else if (conn->state == ISCSI_CONN_STATE_RUNNING &&
		   conn->logout_request_timer == NULL) {
		thread = spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg));
		spdk_thread_send_msg(thread, _iscsi_conn_request_logout, conn);
	}
}

void
iscsi_conns_request_logout(struct spdk_iscsi_tgt_node *target, int pg_tag)
{
	struct spdk_iscsi_conn	*conn;

	if (g_conns_array == MAP_FAILED) {
		return;
	}

	pthread_mutex_lock(&g_conns_mutex);
	TAILQ_FOREACH(conn, &g_active_conns, conn_link) {
		if ((target == NULL) ||
		    (conn->target == target && (pg_tag < 0 || conn->pg_tag == pg_tag))) {
			iscsi_conn_request_logout(conn);
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);
}

void
shutdown_iscsi_conns(void)
{
	iscsi_conns_request_logout(NULL, -1);

	g_shutdown_timer = SPDK_POLLER_REGISTER(iscsi_conn_check_shutdown, NULL, 1000);
}

/* Do not set conn->state if the connection has already started exiting.
 *  This ensures we do not move a connection from EXITED state back to EXITING.
 */
static void
_iscsi_conn_drop(void *ctx)
{
	struct spdk_iscsi_conn *conn = ctx;

	if (conn->state < ISCSI_CONN_STATE_EXITING) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	}
}

int
iscsi_drop_conns(struct spdk_iscsi_conn *conn, const char *conn_match,
		 int drop_all)
{
	struct spdk_iscsi_conn	*xconn;
	const char		*xconn_match;
	struct spdk_thread	*thread;
	int			num;

	SPDK_DEBUGLOG(iscsi, "iscsi_drop_conns\n");

	num = 0;
	pthread_mutex_lock(&g_conns_mutex);
	if (g_conns_array == MAP_FAILED) {
		goto exit;
	}

	TAILQ_FOREACH(xconn, &g_active_conns, conn_link) {
		if (xconn == conn) {
			continue;
		}

		if (!drop_all && xconn->initiator_port == NULL) {
			continue;
		}

		xconn_match =
			drop_all ? xconn->initiator_name : spdk_scsi_port_get_name(xconn->initiator_port);

		if (!strcasecmp(conn_match, xconn_match) &&
		    conn->target == xconn->target) {

			if (num == 0) {
				/*
				 * Only print this message before we report the
				 *  first dropped connection.
				 */
				SPDK_ERRLOG("drop old connections %s by %s\n",
					    conn->target->name, conn_match);
			}

			SPDK_ERRLOG("exiting conn by %s (%s)\n",
				    xconn_match, xconn->initiator_addr);
			if (xconn->sess != NULL) {
				SPDK_DEBUGLOG(iscsi, "TSIH=%u\n", xconn->sess->tsih);
			} else {
				SPDK_DEBUGLOG(iscsi, "TSIH=xx\n");
			}

			SPDK_DEBUGLOG(iscsi, "CID=%u\n", xconn->cid);

			thread = spdk_io_channel_get_thread(spdk_io_channel_from_ctx(xconn->pg));
			spdk_thread_send_msg(thread, _iscsi_conn_drop, xconn);

			num++;
		}
	}

exit:
	pthread_mutex_unlock(&g_conns_mutex);

	if (num != 0) {
		SPDK_ERRLOG("exiting %d conns\n", num);
	}

	return 0;
}

static int
_iscsi_conn_abort_queued_datain_task(struct spdk_iscsi_conn *conn,
				     struct spdk_iscsi_task *task)
{
	struct spdk_iscsi_task *subtask;
	uint32_t remaining_size;

	if (conn->data_in_cnt >= g_iscsi.MaxLargeDataInPerConnection) {
		return -1;
	}

	assert(task->current_data_offset <= task->scsi.transfer_len);
	/* Stop split and abort read I/O for remaining data. */
	if (task->current_data_offset < task->scsi.transfer_len) {
		remaining_size = task->scsi.transfer_len - task->current_data_offset;
		subtask = iscsi_task_get(conn, task, iscsi_task_cpl);
		assert(subtask != NULL);
		subtask->scsi.offset = task->current_data_offset;
		subtask->scsi.length = remaining_size;
		spdk_scsi_task_set_data(&subtask->scsi, NULL, 0);
		task->current_data_offset += subtask->scsi.length;

		subtask->scsi.transfer_len = subtask->scsi.length;
		spdk_scsi_task_process_abort(&subtask->scsi);
		iscsi_task_cpl(&subtask->scsi);
	}

	/* Remove the primary task from the list because all subtasks are submitted
	 *  or aborted.
	 */
	assert(task->current_data_offset == task->scsi.transfer_len);
	TAILQ_REMOVE(&conn->queued_datain_tasks, task, link);
	return 0;
}

int
iscsi_conn_abort_queued_datain_task(struct spdk_iscsi_conn *conn,
				    uint32_t ref_task_tag)
{
	struct spdk_iscsi_task *task;

	TAILQ_FOREACH(task, &conn->queued_datain_tasks, link) {
		if (task->tag == ref_task_tag) {
			return _iscsi_conn_abort_queued_datain_task(conn, task);
		}
	}

	return 0;
}

int
iscsi_conn_abort_queued_datain_tasks(struct spdk_iscsi_conn *conn,
				     struct spdk_scsi_lun *lun,
				     struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task *task, *task_tmp;
	struct spdk_iscsi_pdu *pdu_tmp;
	int rc;

	TAILQ_FOREACH_SAFE(task, &conn->queued_datain_tasks, link, task_tmp) {
		pdu_tmp = iscsi_task_get_pdu(task);
		if ((lun == NULL || lun == task->scsi.lun) &&
		    (pdu == NULL || (spdk_sn32_lt(pdu_tmp->cmd_sn, pdu->cmd_sn)))) {
			rc = _iscsi_conn_abort_queued_datain_task(conn, task);
			if (rc != 0) {
				return rc;
			}
		}
	}

	return 0;
}

int
iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_task *task;

	while (!TAILQ_EMPTY(&conn->queued_datain_tasks) &&
	       conn->data_in_cnt < g_iscsi.MaxLargeDataInPerConnection) {
		task = TAILQ_FIRST(&conn->queued_datain_tasks);
		assert(task->current_data_offset <= task->scsi.transfer_len);
		if (task->current_data_offset < task->scsi.transfer_len) {
			struct spdk_iscsi_task *subtask;
			uint32_t remaining_size = 0;

			remaining_size = task->scsi.transfer_len - task->current_data_offset;
			subtask = iscsi_task_get(conn, task, iscsi_task_cpl);
			assert(subtask != NULL);
			subtask->scsi.offset = task->current_data_offset;
			spdk_scsi_task_set_data(&subtask->scsi, NULL, 0);

			if (spdk_scsi_dev_get_lun(conn->dev, task->lun_id) == NULL) {
				/* Stop submitting split read I/Os for remaining data. */
				TAILQ_REMOVE(&conn->queued_datain_tasks, task, link);
				task->current_data_offset += remaining_size;
				assert(task->current_data_offset == task->scsi.transfer_len);
				subtask->scsi.transfer_len = remaining_size;
				spdk_scsi_task_process_null_lun(&subtask->scsi);
				iscsi_task_cpl(&subtask->scsi);
				return 0;
			}

			subtask->scsi.length = spdk_min(SPDK_BDEV_LARGE_BUF_MAX_SIZE, remaining_size);
			task->current_data_offset += subtask->scsi.length;
			iscsi_queue_task(conn, subtask);
		}
		if (task->current_data_offset == task->scsi.transfer_len) {
			TAILQ_REMOVE(&conn->queued_datain_tasks, task, link);
		}
	}
	return 0;
}

void
iscsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *task = iscsi_task_from_scsi_task(scsi_task);

	iscsi_task_mgmt_response(task->conn, task);
	iscsi_task_put(task);
}

static void
process_completed_read_subtask_list_in_order(struct spdk_iscsi_conn *conn,
		struct spdk_iscsi_task *primary)
{
	struct spdk_iscsi_task *subtask, *tmp;

	TAILQ_FOREACH_SAFE(subtask, &primary->subtask_list, subtask_link, tmp) {
		if (subtask->scsi.offset == primary->bytes_completed) {
			TAILQ_REMOVE(&primary->subtask_list, subtask, subtask_link);
			primary->bytes_completed += subtask->scsi.length;
			if (primary->bytes_completed == primary->scsi.transfer_len) {
				iscsi_task_put(primary);
			}
			iscsi_task_response(conn, subtask);
			iscsi_task_put(subtask);
		} else {
			break;
		}
	}
}

static void
process_read_task_completion(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_task *task,
			     struct spdk_iscsi_task *primary)
{
	struct spdk_iscsi_task *tmp;

	if (task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		if (primary->scsi.status == SPDK_SCSI_STATUS_GOOD) {
			/* If the status of the completed subtask, task, is the
			 * first failure, copy it to out-of-order subtasks, and
			 * remember it as the status of the SCSI Read Command.
			 */
			TAILQ_FOREACH(tmp, &primary->subtask_list, subtask_link) {
				spdk_scsi_task_copy_status(&tmp->scsi, &task->scsi);
			}
			spdk_scsi_task_copy_status(&primary->scsi, &task->scsi);
		}
	} else if (primary->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		/* Even if the status of the completed subtask is success,
		 * if there are any failed subtask ever, copy the first failed
		 * status to it.
		 */
		spdk_scsi_task_copy_status(&task->scsi, &primary->scsi);
	}

	if (task == primary) {
		/* If read I/O size is not larger than SPDK_BDEV_LARGE_BUF_MAX_SIZE,
		 * the primary task which processes the SCSI Read Command PDU is
		 * submitted directly. Hence send SCSI Response PDU for the primary
		 * task simply.
		 */
		primary->bytes_completed = task->scsi.length;
		assert(primary->bytes_completed == task->scsi.transfer_len);
		iscsi_task_response(conn, task);
		iscsi_task_put(task);
	} else if (!conn->sess->DataSequenceInOrder) {
		/* If DataSequenceInOrder is No, send SCSI Response PDU for the completed
		 * subtask without any deferral.
		 */
		primary->bytes_completed += task->scsi.length;
		if (primary->bytes_completed == primary->scsi.transfer_len) {
			iscsi_task_put(primary);
		}
		iscsi_task_response(conn, task);
		iscsi_task_put(task);
	} else {
		/* If DataSequenceInOrder is Yes, if the completed subtask is out-of-order,
		 * it is deferred until all preceding subtasks send SCSI Response PDU.
		 */
		if (task->scsi.offset != primary->bytes_completed) {
			TAILQ_FOREACH(tmp, &primary->subtask_list, subtask_link) {
				if (task->scsi.offset < tmp->scsi.offset) {
					TAILQ_INSERT_BEFORE(tmp, task, subtask_link);
					return;
				}
			}

			TAILQ_INSERT_TAIL(&primary->subtask_list, task, subtask_link);
		} else {
			TAILQ_INSERT_HEAD(&primary->subtask_list, task, subtask_link);
			process_completed_read_subtask_list_in_order(conn, primary);
		}
	}
}

static void
process_non_read_task_completion(struct spdk_iscsi_conn *conn,
				 struct spdk_iscsi_task *task,
				 struct spdk_iscsi_task *primary)
{
	primary->bytes_completed += task->scsi.length;

	if (task == primary) {
		/* This was a small write with no R2T. */
		iscsi_task_response(conn, task);
		iscsi_task_put(task);
		return;
	}

	if (task->scsi.status == SPDK_SCSI_STATUS_GOOD) {
		primary->scsi.data_transferred += task->scsi.data_transferred;
	} else if (primary->scsi.status == SPDK_SCSI_STATUS_GOOD) {
		/* If the status of this subtask is the first failure, copy it to
		 * the primary task.
		 */
		spdk_scsi_task_copy_status(&primary->scsi, &task->scsi);
	}

	if (primary->bytes_completed == primary->scsi.transfer_len) {
		/* If LUN is removed in the middle of the iSCSI write sequence,
		 *  primary might complete the write to the initiator because it is not
		 *  ensured that the initiator will send all data requested by R2Ts.
		 *
		 * We check it and skip the following if primary is completed. (see
		 *  iscsi_clear_all_transfer_task() in iscsi.c.)
		 */
		if (primary->is_r2t_active) {
			iscsi_task_response(conn, primary);
			iscsi_del_transfer_task(conn, primary->tag);
		} else {
			iscsi_task_response(conn, task);
		}
	}
	iscsi_task_put(task);
}

void
iscsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *primary;
	struct spdk_iscsi_task *task = iscsi_task_from_scsi_task(scsi_task);
	struct spdk_iscsi_conn *conn = task->conn;
	struct spdk_iscsi_pdu *pdu = task->pdu;

	spdk_trace_record(TRACE_ISCSI_TASK_DONE, conn->id, 0, (uintptr_t)task);

	task->is_queued = false;
	primary = iscsi_task_get_primary(task);

	if (iscsi_task_is_read(primary)) {
		process_read_task_completion(conn, task, primary);
	} else {
		process_non_read_task_completion(conn, task, primary);
	}
	if (!task->parent) {
		spdk_trace_record(TRACE_ISCSI_PDU_COMPLETED, 0, 0, (uintptr_t)pdu);
	}
}

static void
iscsi_conn_send_nopin(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_nop_in *rsp;
	/* Only send nopin if we have logged in and are in a normal session. */
	if (conn->sess == NULL ||
	    !conn->full_feature ||
	    !iscsi_param_eq_val(conn->sess->params, "SessionType", "Normal")) {
		return;
	}
	SPDK_DEBUGLOG(iscsi, "send NOPIN isid=%"PRIx64", tsih=%u, cid=%u\n",
		      conn->sess->isid, conn->sess->tsih, conn->cid);
	SPDK_DEBUGLOG(iscsi, "StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      conn->StatSN, conn->sess->ExpCmdSN,
		      conn->sess->MaxCmdSN);
	rsp_pdu = iscsi_get_pdu(conn);
	rsp = (struct iscsi_bhs_nop_in *) &rsp_pdu->bhs;
	rsp_pdu->data = NULL;
	/*
	 * iscsi_get_pdu() memset's the PDU for us, so only fill out the needed
	 *  fields.
	 */
	rsp->opcode = ISCSI_OP_NOPIN;
	rsp->flags = 0x80;
	/*
	 * Technically the to_be32() is not needed here, since
	 *  to_be32(0xFFFFFFFU) returns 0xFFFFFFFFU.
	 */
	to_be32(&rsp->itt, 0xFFFFFFFFU);
	to_be32(&rsp->ttt, conn->id);
	to_be32(&rsp->stat_sn, conn->StatSN);
	to_be32(&rsp->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsp->max_cmd_sn, conn->sess->MaxCmdSN);
	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);
	conn->last_nopin = spdk_get_ticks();
	conn->nop_outstanding = true;
}

void
iscsi_conn_handle_nop(struct spdk_iscsi_conn *conn)
{
	uint64_t	tsc;

	/**
	  * This function will be executed by nop_poller of iSCSI polling group, so
	  * we need to check the connection state first, then do the nop interval
	  * expiration check work.
	  */
	if ((conn->state == ISCSI_CONN_STATE_EXITED) ||
	    (conn->state == ISCSI_CONN_STATE_EXITING)) {
		return;
	}

	/* Check for nop interval expiration */
	tsc = spdk_get_ticks();
	if (conn->nop_outstanding) {
		if ((tsc - conn->last_nopin) > conn->timeout) {
			SPDK_ERRLOG("Timed out waiting for NOP-Out response from initiator\n");
			SPDK_ERRLOG("  tsc=0x%" PRIx64 ", last_nopin=0x%" PRIx64 "\n", tsc, conn->last_nopin);
			SPDK_ERRLOG("  initiator=%s, target=%s\n", conn->initiator_name,
				    conn->target_short_name);
			conn->state = ISCSI_CONN_STATE_EXITING;
		}
	} else if (tsc - conn->last_nopin > conn->nopininterval) {
		iscsi_conn_send_nopin(conn);
	}
}

/**
 * \brief Reads data for the specified iSCSI connection from its TCP socket.
 *
 * The TCP socket is marked as non-blocking, so this function may not read
 * all data requested.
 *
 * Returns SPDK_ISCSI_CONNECTION_FATAL if the recv() operation indicates a fatal
 * error with the TCP connection (including if the TCP connection was closed
 * unexpectedly.
 *
 * Otherwise returns the number of bytes successfully read.
 */
int
iscsi_conn_read_data(struct spdk_iscsi_conn *conn, int bytes,
		     void *buf)
{
	int ret;

	if (bytes == 0) {
		return 0;
	}

	ret = spdk_sock_recv(conn->sock, buf, bytes);

	if (ret > 0) {
		spdk_trace_record(TRACE_ISCSI_READ_FROM_SOCKET_DONE, conn->id, ret, 0);
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno == ECONNRESET) {
			SPDK_DEBUGLOG(iscsi, "spdk_sock_recv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return SPDK_ISCSI_CONNECTION_FATAL;
}

int
iscsi_conn_readv_data(struct spdk_iscsi_conn *conn,
		      struct iovec *iov, int iovcnt)
{
	int ret;

	if (iov == NULL || iovcnt == 0) {
		return 0;
	}

	if (iovcnt == 1) {
		return iscsi_conn_read_data(conn, iov[0].iov_len,
					    iov[0].iov_base);
	}

	ret = spdk_sock_readv(conn->sock, iov, iovcnt);

	if (ret > 0) {
		spdk_trace_record(TRACE_ISCSI_READ_FROM_SOCKET_DONE, conn->id, ret, 0);
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno == ECONNRESET) {
			SPDK_DEBUGLOG(iscsi, "spdk_sock_readv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_readv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return SPDK_ISCSI_CONNECTION_FATAL;
}

static bool
iscsi_is_free_pdu_deferred(struct spdk_iscsi_pdu *pdu)
{
	if (pdu == NULL) {
		return false;
	}

	if (pdu->bhs.opcode == ISCSI_OP_R2T ||
	    pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN) {
		return true;
	}

	return false;
}

static int
iscsi_dif_verify(struct spdk_iscsi_pdu *pdu, struct spdk_dif_ctx *dif_ctx)
{
	struct iovec iov;
	struct spdk_dif_error err_blk = {};
	uint32_t num_blocks;
	int rc;

	iov.iov_base = pdu->data;
	iov.iov_len = pdu->data_buf_len;
	num_blocks = pdu->data_buf_len / dif_ctx->block_size;

	rc = spdk_dif_verify(&iov, 1, num_blocks, dif_ctx, &err_blk);
	if (rc != 0) {
		SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
			    err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

static void
_iscsi_conn_pdu_write_done(void *cb_arg, int err)
{
	struct spdk_iscsi_pdu *pdu = cb_arg;
	struct spdk_iscsi_conn *conn = pdu->conn;

	assert(conn != NULL);

	if (spdk_unlikely(conn->state >= ISCSI_CONN_STATE_EXITING)) {
		/* The other policy will recycle the resource */
		return;
	}

	TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);

	if (err != 0) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	} else {
		spdk_trace_record(TRACE_ISCSI_FLUSH_WRITEBUF_DONE, conn->id, pdu->mapped_length, (uintptr_t)pdu);
	}

	if ((conn->full_feature) &&
	    (conn->sess->ErrorRecoveryLevel >= 1) &&
	    iscsi_is_free_pdu_deferred(pdu)) {
		SPDK_DEBUGLOG(iscsi, "stat_sn=%d\n",
			      from_be32(&pdu->bhs.stat_sn));
		TAILQ_INSERT_TAIL(&conn->snack_pdu_list, pdu,
				  tailq);
	} else {
		iscsi_conn_free_pdu(conn, pdu);
	}
}

void
iscsi_conn_pdu_generic_complete(void *cb_arg)
{
}

void
iscsi_conn_write_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
		     iscsi_conn_xfer_complete_cb cb_fn,
		     void *cb_arg)
{
	uint32_t crc32c;
	ssize_t rc;

	if (spdk_unlikely(pdu->dif_insert_or_strip)) {
		rc = iscsi_dif_verify(pdu, &pdu->dif_ctx);
		if (rc != 0) {
			iscsi_conn_free_pdu(conn, pdu);
			conn->state = ISCSI_CONN_STATE_EXITING;
			return;
		}
	}

	if (pdu->bhs.opcode != ISCSI_OP_LOGIN_RSP) {
		/* Header Digest */
		if (conn->header_digest) {
			crc32c = iscsi_pdu_calc_header_digest(pdu);
			MAKE_DIGEST_WORD(pdu->header_digest, crc32c);
		}

		/* Data Digest */
		if (conn->data_digest && DGET24(pdu->bhs.data_segment_len) != 0) {
			crc32c = iscsi_pdu_calc_data_digest(pdu);
			MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
		}
	}

	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;
	TAILQ_INSERT_TAIL(&conn->write_pdu_list, pdu, tailq);

	if (spdk_unlikely(conn->state >= ISCSI_CONN_STATE_EXITING)) {
		return;
	}
	pdu->sock_req.iovcnt = iscsi_build_iovs(conn, pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
						&pdu->mapped_length);
	pdu->sock_req.cb_fn = _iscsi_conn_pdu_write_done;
	pdu->sock_req.cb_arg = pdu;

	spdk_trace_record(TRACE_ISCSI_FLUSH_WRITEBUF_START, conn->id, pdu->mapped_length, (uintptr_t)pdu,
			  pdu->sock_req.iovcnt);
	spdk_sock_writev_async(conn->sock, &pdu->sock_req);
}

static void
iscsi_conn_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	assert(conn != NULL);

	if ((conn->state == ISCSI_CONN_STATE_EXITED) ||
	    (conn->state == ISCSI_CONN_STATE_EXITING)) {
		return;
	}

	/* Handle incoming PDUs */
	rc = iscsi_handle_incoming_pdus(conn);
	if (rc < 0) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	}
}

static void
iscsi_conn_full_feature_migrate(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->state >= ISCSI_CONN_STATE_EXITING) {
		/* Connection is being exited before this callback is executed. */
		SPDK_DEBUGLOG(iscsi, "Connection is already exited.\n");
		return;
	}

	if (conn->sess->session_type == SESSION_TYPE_NORMAL) {
		iscsi_conn_open_luns(conn);
	}

	/* Add this connection to the assigned poll group. */
	iscsi_poll_group_add_conn(conn->pg, conn);
}

static struct spdk_iscsi_poll_group *g_next_pg = NULL;

void
iscsi_conn_schedule(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_poll_group	*pg;
	struct spdk_iscsi_tgt_node	*target;

	if (conn->sess->session_type != SESSION_TYPE_NORMAL) {
		/* Leave all non-normal sessions on the acceptor
		 * thread. */
		return;
	}
	pthread_mutex_lock(&g_iscsi.mutex);

	target = conn->sess->target;
	pthread_mutex_lock(&target->mutex);
	target->num_active_conns++;
	if (target->num_active_conns == 1) {
		/**
		 * This is the only active connection for this target node.
		 *  Pick a poll group using round-robin.
		 */
		if (g_next_pg == NULL) {
			g_next_pg = TAILQ_FIRST(&g_iscsi.poll_group_head);
			assert(g_next_pg != NULL);
		}

		pg = g_next_pg;
		g_next_pg = TAILQ_NEXT(g_next_pg, link);

		/* Save the pg in the target node so it can be used for any other connections to this target node. */
		target->pg = pg;
	} else {
		/**
		 * There are other active connections for this target node.
		 */
		pg = target->pg;
	}

	pthread_mutex_unlock(&target->mutex);
	pthread_mutex_unlock(&g_iscsi.mutex);

	assert(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(conn->pg)) ==
	       spdk_get_thread());

	/* Remove this connection from the previous poll group */
	iscsi_poll_group_remove_conn(conn->pg, conn);

	conn->pg = pg;

	spdk_thread_send_msg(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(pg)),
			     iscsi_conn_full_feature_migrate, conn);
}

static int
logout_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->state < ISCSI_CONN_STATE_EXITING) {
		conn->state = ISCSI_CONN_STATE_EXITING;
	}

	return SPDK_POLLER_BUSY;
}

void
iscsi_conn_logout(struct spdk_iscsi_conn *conn)
{
	conn->is_logged_out = true;
	conn->logout_timer = SPDK_POLLER_REGISTER(logout_timeout, conn, ISCSI_LOGOUT_TIMEOUT * 1000000);
}

static const char *
iscsi_conn_get_state(struct spdk_iscsi_conn *conn)
{
	switch (conn->state) {
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_INVALID, "invalid");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_RUNNING, "running");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_EXITING, "exiting");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_CONN_STATE_EXITED, "exited");
	}
	return "unknown";
}

static const char *
iscsi_conn_get_login_phase(struct spdk_iscsi_conn *conn)
{
	switch (conn->login_phase) {
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_SECURITY_NEGOTIATION_PHASE, "security_negotiation_phase");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_OPERATIONAL_NEGOTIATION_PHASE, "operational_negotiation_phase");
		SPDK_ISCSI_CONNECTION_STATUS(ISCSI_FULL_FEATURE_PHASE, "full_feature_phase");
	}
	return "not_started";
}

SPDK_TRACE_REGISTER_FN(iscsi_conn_trace, "iscsi_conn", TRACE_GROUP_ISCSI)
{
	spdk_trace_register_owner(OWNER_ISCSI_CONN, 'c');
	spdk_trace_register_object(OBJECT_ISCSI_PDU, 'p');
	spdk_trace_register_description("ISCSI_READ_DONE", TRACE_ISCSI_READ_FROM_SOCKET_DONE,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("ISCSI_WRITE_START", TRACE_ISCSI_FLUSH_WRITEBUF_START,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 1,
					SPDK_TRACE_ARG_TYPE_INT, "iovec");
	spdk_trace_register_description("ISCSI_WRITE_DONE", TRACE_ISCSI_FLUSH_WRITEBUF_DONE,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("ISCSI_READ_PDU", TRACE_ISCSI_READ_PDU,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 1,
					SPDK_TRACE_ARG_TYPE_INT, "opc");
	spdk_trace_register_description("ISCSI_TASK_DONE", TRACE_ISCSI_TASK_DONE,
					OWNER_ISCSI_CONN, OBJECT_SCSI_TASK, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("ISCSI_TASK_QUEUE", TRACE_ISCSI_TASK_QUEUE,
					OWNER_ISCSI_CONN, OBJECT_SCSI_TASK, 1,
					SPDK_TRACE_ARG_TYPE_PTR, "pdu");
	spdk_trace_register_description("ISCSI_TASK_EXECUTED", TRACE_ISCSI_TASK_EXECUTED,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("ISCSI_PDU_COMPLETED", TRACE_ISCSI_PDU_COMPLETED,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
}

void
iscsi_conn_info_json(struct spdk_json_write_ctx *w, struct spdk_iscsi_conn *conn)
{
	uint16_t tsih;

	if (!conn->is_valid) {
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_int32(w, "id", conn->id);

	spdk_json_write_named_int32(w, "cid", conn->cid);

	/*
	 * If we try to return data for a connection that has not
	 *  logged in yet, the session will not be set.  So in this
	 *  case, return -1 for the tsih rather than segfaulting
	 *  on the null conn->sess.
	 */
	if (conn->sess == NULL) {
		tsih = -1;
	} else {
		tsih = conn->sess->tsih;
	}
	spdk_json_write_named_int32(w, "tsih", tsih);

	spdk_json_write_named_string(w, "state", iscsi_conn_get_state(conn));

	spdk_json_write_named_string(w, "login_phase", iscsi_conn_get_login_phase(conn));

	spdk_json_write_named_string(w, "initiator_addr", conn->initiator_addr);

	spdk_json_write_named_string(w, "target_addr", conn->target_addr);

	spdk_json_write_named_string(w, "target_node_name", conn->target_short_name);

	spdk_json_write_named_string(w, "thread_name",
				     spdk_thread_get_name(spdk_get_thread()));

	spdk_json_write_object_end(w);
}
