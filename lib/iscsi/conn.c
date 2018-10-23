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
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/trace.h"
#include "spdk/net.h"
#include "spdk/sock.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

#include "iscsi/task.h"
#include "iscsi/conn.h"
#include "iscsi/tgt_node.h"
#include "iscsi/portal_grp.h"

#define SPDK_ISCSI_CONNECTION_MEMSET(conn)		\
	memset(&(conn)->portal, 0, sizeof(*(conn)) -	\
		offsetof(struct spdk_iscsi_conn, portal));

static int g_connections_per_lcore;
static uint32_t *g_num_connections;

struct spdk_iscsi_conn *g_conns_array = MAP_FAILED;
static int g_conns_array_fd = -1;
static char g_shm_name[64];

static pthread_mutex_t g_conns_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_poller *g_shutdown_timer = NULL;

static uint32_t spdk_iscsi_conn_allocate_reactor(const struct spdk_cpuset *cpumask);

static void spdk_iscsi_conn_full_feature_migrate(void *arg1, void *arg2);
static void spdk_iscsi_conn_stop(struct spdk_iscsi_conn *conn);
static void spdk_iscsi_conn_sock_cb(void *arg, struct spdk_sock_group *group,
				    struct spdk_sock *sock);

static struct spdk_iscsi_conn *
allocate_conn(void)
{
	struct spdk_iscsi_conn	*conn;
	int				i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid) {
			SPDK_ISCSI_CONNECTION_MEMSET(conn);
			conn->is_valid = 1;
			pthread_mutex_unlock(&g_conns_mutex);
			return conn;
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);

	return NULL;
}

static void
free_conn(struct spdk_iscsi_conn *conn)
{
	free(conn->portal_host);
	free(conn->portal_port);
	conn->is_valid = 0;
}

static struct spdk_iscsi_conn *
spdk_find_iscsi_connection_by_id(int cid)
{
	if (g_conns_array[cid].is_valid == 1) {
		return &g_conns_array[cid];
	} else {
		return NULL;
	}
}

int spdk_initialize_iscsi_conns(void)
{
	size_t conns_size = sizeof(struct spdk_iscsi_conn) * MAX_ISCSI_CONNECTIONS;
	uint32_t i, last_core;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_init\n");

	snprintf(g_shm_name, sizeof(g_shm_name), "/spdk_iscsi_conns.%d", spdk_app_get_shm_id());
	g_conns_array_fd = shm_open(g_shm_name, O_RDWR | O_CREAT, 0600);
	if (g_conns_array_fd < 0) {
		SPDK_ERRLOG("could not shm_open %s\n", g_shm_name);
		goto err;
	}

	if (ftruncate(g_conns_array_fd, conns_size) != 0) {
		SPDK_ERRLOG("could not ftruncate\n");
		goto err;
	}
	g_conns_array = mmap(0, conns_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			     g_conns_array_fd, 0);

	if (g_conns_array == MAP_FAILED) {
		fprintf(stderr, "could not mmap cons array file %s (%d)\n", g_shm_name, errno);
		goto err;
	}

	memset(g_conns_array, 0, conns_size);

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		g_conns_array[i].id = i;
	}

	last_core = spdk_env_get_last_core();
	g_num_connections = calloc(last_core + 1, sizeof(uint32_t));
	if (!g_num_connections) {
		SPDK_ERRLOG("Could not allocate array size=%u for g_num_connections\n",
			    last_core + 1);
		goto err;
	}

	return 0;

err:
	if (g_conns_array != MAP_FAILED) {
		munmap(g_conns_array, conns_size);
		g_conns_array = MAP_FAILED;
	}

	if (g_conns_array_fd >= 0) {
		close(g_conns_array_fd);
		g_conns_array_fd = -1;
		shm_unlink(g_shm_name);
	}

	return -1;
}

static void
spdk_iscsi_poll_group_add_conn_sock(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_poll_group *poll_group;
	int rc;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_iscsi.poll_group[conn->lcore];

	rc = spdk_sock_group_add_sock(poll_group->sock_group, conn->sock, spdk_iscsi_conn_sock_cb, conn);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add sock=%p of conn=%p\n", conn->sock, conn);
	}
}

static void
spdk_iscsi_poll_group_remove_conn_sock(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_poll_group *poll_group;
	int rc;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_iscsi.poll_group[conn->lcore];

	rc = spdk_sock_group_remove_sock(poll_group->sock_group, conn->sock);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to remove sock=%p of conn=%p\n", conn->sock, conn);
	}
}

static void
spdk_iscsi_poll_group_add_conn(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_poll_group *poll_group;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_iscsi.poll_group[conn->lcore];

	conn->is_stopped = false;
	STAILQ_INSERT_TAIL(&poll_group->connections, conn, link);
	spdk_iscsi_poll_group_add_conn_sock(conn);
}

static void
spdk_iscsi_poll_group_remove_conn(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_poll_group *poll_group;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_iscsi.poll_group[conn->lcore];

	conn->is_stopped = true;
	STAILQ_REMOVE(&poll_group->connections, conn, spdk_iscsi_conn, link);
}

/**
 * \brief Create an iSCSI connection from the given parameters and schedule it
 *        on a reactor.
 *
 * \code
 *
 * # identify reactor where the new connections work item will be scheduled
 * reactor = spdk_iscsi_conn_allocate_reactor()
 * allocate spdk_iscsi_conn object
 * initialize spdk_iscsi_conn object
 * schedule iSCSI connection work item on reactor
 *
 * \endcode
 */
int
spdk_iscsi_conn_construct(struct spdk_iscsi_portal *portal,
			  struct spdk_sock *sock)
{
	struct spdk_iscsi_conn *conn;
	int bufsize, i, rc;

	conn = allocate_conn();
	if (conn == NULL) {
		SPDK_ERRLOG("Could not allocate connection.\n");
		return -1;
	}

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	conn->timeout = g_spdk_iscsi.timeout;
	conn->nopininterval = g_spdk_iscsi.nopininterval;
	conn->nopininterval *= spdk_get_ticks_hz(); /* seconds to TSC */
	conn->nop_outstanding = false;
	conn->data_out_cnt = 0;
	conn->data_in_cnt = 0;
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);
	conn->MaxRecvDataSegmentLength = 8192; // RFC3720(12.12)

	conn->portal = portal;
	conn->pg_tag = portal->group->tag;
	conn->portal_host = strdup(portal->host);
	conn->portal_port = strdup(portal->port);
	conn->portal_cpumask = portal->cpumask;
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

	for (i = 0; i < DEFAULT_MAXR2T; i++) {
		conn->outstanding_r2t_tasks[i] = NULL;
	}

	TAILQ_INIT(&conn->write_pdu_list);
	TAILQ_INIT(&conn->snack_pdu_list);
	TAILQ_INIT(&conn->queued_r2t_tasks);
	TAILQ_INIT(&conn->active_r2t_tasks);
	TAILQ_INIT(&conn->queued_datain_tasks);
	memset(&conn->open_lun_descs, 0, sizeof(conn->open_lun_descs));

	rc = spdk_sock_getaddr(sock, conn->target_addr, sizeof conn->target_addr, NULL,
			       conn->initiator_addr, sizeof conn->initiator_addr, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed\n");
		goto error_return;
	}

	bufsize = 2 * 1024 * 1024;
	rc = spdk_sock_set_recvbuf(conn->sock, bufsize);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvbuf failed\n");
	}

	bufsize = 32 * 1024 * 1024 / g_spdk_iscsi.MaxConnections;
	if (bufsize > 2 * 1024 * 1024) {
		bufsize = 2 * 1024 * 1024;
	}
	rc = spdk_sock_set_sendbuf(conn->sock, bufsize);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_sendbuf failed\n");
	}

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(conn->sock, 1);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed\n");
		goto error_return;
	}

	/* set default params */
	rc = spdk_iscsi_conn_params_init(&conn->params);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_conn_params_init() failed\n");
error_return:
		spdk_iscsi_param_free(conn->params);
		free_conn(conn);
		return -1;
	}
	conn->logout_timer = NULL;
	conn->shutdown_timer = NULL;
	SPDK_NOTICELOG("Launching connection on acceptor thread\n");
	conn->pending_task_cnt = 0;
	conn->pending_activate_event = false;

	conn->lcore = spdk_env_get_current_core();
	__sync_fetch_and_add(&g_num_connections[conn->lcore], 1);

	spdk_iscsi_poll_group_add_conn(conn);
	return 0;
}

void
spdk_iscsi_conn_free_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	if (pdu->task) {
		if (pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN) {
			if (pdu->task->scsi.offset > 0) {
				conn->data_in_cnt--;
				if (pdu->bhs.flags & ISCSI_DATAIN_STATUS) {
					/* Free the primary task after the last subtask done */
					conn->data_in_cnt--;
					spdk_iscsi_task_put(spdk_iscsi_task_get_primary(pdu->task));
				}
			}
		} else if (pdu->bhs.opcode == ISCSI_OP_SCSI_RSP &&
			   pdu->task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
			if (pdu->task->scsi.offset > 0) {
				spdk_iscsi_task_put(spdk_iscsi_task_get_primary(pdu->task));
			}
		}
		spdk_iscsi_task_put(pdu->task);
	}
	spdk_put_pdu(pdu);
}

static int spdk_iscsi_conn_free_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu, *tmp_pdu;
	struct spdk_iscsi_task *iscsi_task, *tmp_iscsi_task;

	TAILQ_FOREACH_SAFE(pdu, &conn->write_pdu_list, tailq, tmp_pdu) {
		TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);
		spdk_iscsi_conn_free_pdu(conn, pdu);
	}

	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, tmp_pdu) {
		TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
		if (pdu->task) {
			spdk_iscsi_task_put(pdu->task);
		}
		spdk_put_pdu(pdu);
	}

	TAILQ_FOREACH_SAFE(iscsi_task, &conn->queued_datain_tasks, link, tmp_iscsi_task) {
		if (!iscsi_task->is_queued) {
			TAILQ_REMOVE(&conn->queued_datain_tasks, iscsi_task, link);
			spdk_iscsi_task_put(iscsi_task);
		}
	}

	if (conn->pending_task_cnt) {
		return -1;
	}

	return 0;
}

static void spdk_iscsi_conn_free(struct spdk_iscsi_conn *conn)
{
	if (conn == NULL) {
		return;
	}

	spdk_iscsi_param_free(conn->params);

	/*
	 * Each connection pre-allocates its next PDU - make sure these get
	 *  freed here.
	 */
	spdk_put_pdu(conn->pdu_in_progress);

	free_conn(conn);
}

static void spdk_iscsi_remove_conn(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_sess *sess;
	int idx;
	uint32_t i, j;

	idx = -1;
	sess = conn->sess;
	conn->sess = NULL;
	if (sess == NULL) {
		spdk_iscsi_conn_free(conn);
		return;
	}

	for (i = 0; i < sess->connections; i++) {
		if (sess->conns[i] == conn) {
			idx = i;
			break;
		}
	}

	if (sess->connections < 1) {
		SPDK_ERRLOG("zero connection\n");
		sess->connections = 0;
	} else {
		if (idx < 0) {
			SPDK_ERRLOG("remove conn not found\n");
		} else {
			for (j = idx; j < sess->connections - 1; j++) {
				sess->conns[j] = sess->conns[j + 1];
			}
			sess->conns[sess->connections - 1] = NULL;
		}
		sess->connections--;
	}

	SPDK_NOTICELOG("Terminating connections(tsih %d): %d\n", sess->tsih, sess->connections);

	if (sess->connections == 0) {
		/* cleanup last connection */
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "cleanup last conn free sess\n");
		spdk_free_sess(sess);
	}

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "cleanup free conn\n");
	spdk_iscsi_conn_free(conn);
}

static void
spdk_iscsi_conn_cleanup_backend(struct spdk_iscsi_conn *conn)
{
	int rc;
	struct spdk_iscsi_tgt_node *target;

	if (conn->sess->connections > 1) {
		/* connection specific cleanup */
	} else if (!g_spdk_iscsi.AllowDuplicateIsid) {
		/* clean up all tasks to all LUNs for session */
		target = conn->sess->target;
		if (target != NULL) {
			rc = spdk_iscsi_tgt_node_cleanup_luns(conn, target);
			if (rc < 0) {
				SPDK_ERRLOG("target abort failed\n");
			}
		}
	}
}

static void
_spdk_iscsi_conn_free(struct spdk_iscsi_conn *conn)
{
	pthread_mutex_lock(&g_conns_mutex);
	spdk_iscsi_remove_conn(conn);
	pthread_mutex_unlock(&g_conns_mutex);
}

static int
_spdk_iscsi_conn_check_shutdown(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	rc = spdk_iscsi_conn_free_tasks(conn);
	if (rc < 0) {
		return -1;
	}

	spdk_poller_unregister(&conn->shutdown_timer);

	spdk_iscsi_conn_stop(conn);
	_spdk_iscsi_conn_free(conn);

	return -1;
}

static void
_spdk_iscsi_conn_destruct(struct spdk_iscsi_conn *conn)
{
	int rc;

	spdk_clear_all_transfer_task(conn, NULL);
	spdk_iscsi_poll_group_remove_conn_sock(conn);
	spdk_sock_close(&conn->sock);
	spdk_poller_unregister(&conn->logout_timer);
	spdk_poller_unregister(&conn->flush_poller);

	rc = spdk_iscsi_conn_free_tasks(conn);
	if (rc < 0) {
		/* The connection cannot be freed yet. Check back later. */
		conn->shutdown_timer = spdk_poller_register(_spdk_iscsi_conn_check_shutdown, conn, 1000);
	} else {
		spdk_iscsi_conn_stop(conn);
		_spdk_iscsi_conn_free(conn);
	}
}

static int
_spdk_iscsi_conn_check_pending_tasks(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->dev != NULL && spdk_scsi_dev_has_pending_tasks(conn->dev)) {
		return -1;
	}

	spdk_poller_unregister(&conn->shutdown_timer);

	_spdk_iscsi_conn_destruct(conn);

	return -1;
}

void
spdk_iscsi_conn_destruct(struct spdk_iscsi_conn *conn)
{
	conn->state = ISCSI_CONN_STATE_EXITED;

	if (conn->sess != NULL && conn->pending_task_cnt > 0) {
		spdk_iscsi_conn_cleanup_backend(conn);
	}

	if (conn->dev != NULL && spdk_scsi_dev_has_pending_tasks(conn->dev)) {
		conn->shutdown_timer = spdk_poller_register(_spdk_iscsi_conn_check_pending_tasks, conn, 1000);
	} else {
		_spdk_iscsi_conn_destruct(conn);
	}
}

static int
spdk_iscsi_get_active_conns(void)
{
	struct spdk_iscsi_conn *conn;
	int num = 0;
	int i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		conn = spdk_find_iscsi_connection_by_id(i);
		if (conn == NULL) {
			continue;
		}
		num++;
	}
	pthread_mutex_unlock(&g_conns_mutex);
	return num;
}

static void
spdk_iscsi_conns_cleanup(void)
{
	free(g_num_connections);
	munmap(g_conns_array, sizeof(struct spdk_iscsi_conn) *
	       MAX_ISCSI_CONNECTIONS);
	shm_unlink(g_shm_name);
	if (g_conns_array_fd >= 0) {
		close(g_conns_array_fd);
		g_conns_array_fd = -1;
	}
}

static void
spdk_iscsi_conn_check_shutdown_cb(void *arg1, void *arg2)
{
	spdk_iscsi_conns_cleanup();
	spdk_shutdown_iscsi_conns_done();
}

static int
spdk_iscsi_conn_check_shutdown(void *arg)
{
	struct spdk_event *event;

	if (spdk_iscsi_get_active_conns() == 0) {
		spdk_poller_unregister(&g_shutdown_timer);
		event = spdk_event_allocate(spdk_env_get_current_core(),
					    spdk_iscsi_conn_check_shutdown_cb, NULL, NULL);
		spdk_event_call(event);
	}

	return -1;
}

static void
spdk_iscsi_conn_close_lun(struct spdk_iscsi_conn *conn, int lun_id)
{
	struct spdk_scsi_desc *desc;

	desc = conn->open_lun_descs[lun_id];
	if (desc != NULL) {
		spdk_scsi_lun_free_io_channel(desc);
		spdk_scsi_lun_close(desc);
		conn->open_lun_descs[lun_id] = NULL;
	}
}

static void
spdk_iscsi_conn_close_luns(struct spdk_iscsi_conn *conn)
{
	int i;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		spdk_iscsi_conn_close_lun(conn, i);
	}
}

static void
_iscsi_conn_remove_lun(void *arg1, void *arg2)
{
	struct spdk_iscsi_conn *conn = arg1;
	struct spdk_scsi_lun *lun = arg2;
	int lun_id = spdk_scsi_lun_get_id(lun);
	struct spdk_iscsi_pdu *pdu, *tmp_pdu;
	struct spdk_iscsi_task *iscsi_task, *tmp_iscsi_task;

	/* If a connection is already in stating status, just return */
	if (conn->state >= ISCSI_CONN_STATE_EXITING) {
		return;
	}

	spdk_clear_all_transfer_task(conn, lun);
	TAILQ_FOREACH_SAFE(pdu, &conn->write_pdu_list, tailq, tmp_pdu) {
		if (pdu->task && (lun == pdu->task->scsi.lun)) {
			TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);
			spdk_iscsi_conn_free_pdu(conn, pdu);
		}
	}

	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, tmp_pdu) {
		if (pdu->task && (lun == pdu->task->scsi.lun)) {
			TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
			spdk_iscsi_task_put(pdu->task);
			spdk_put_pdu(pdu);
		}
	}

	TAILQ_FOREACH_SAFE(iscsi_task, &conn->queued_datain_tasks, link, tmp_iscsi_task) {
		if ((!iscsi_task->is_queued) && (lun == iscsi_task->scsi.lun)) {
			TAILQ_REMOVE(&conn->queued_datain_tasks, iscsi_task, link);
			spdk_iscsi_task_put(iscsi_task);
		}
	}

	spdk_iscsi_conn_close_lun(conn, lun_id);
}

static void
spdk_iscsi_conn_remove_lun(struct spdk_scsi_lun *lun, void *remove_ctx)
{
	struct spdk_iscsi_conn *conn = remove_ctx;
	struct spdk_event *event;

	event = spdk_event_allocate(conn->lcore, _iscsi_conn_remove_lun,
				    conn, lun);
	spdk_event_call(event);
}

static void
spdk_iscsi_conn_open_luns(struct spdk_iscsi_conn *conn)
{
	int i, rc;
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_desc *desc;

	for (i = 0; i < SPDK_SCSI_DEV_MAX_LUN; i++) {
		lun = spdk_scsi_dev_get_lun(conn->dev, i);
		if (lun == NULL) {
			continue;
		}

		rc = spdk_scsi_lun_open(lun, spdk_iscsi_conn_remove_lun, conn, &desc);
		if (rc != 0) {
			goto error;
		}

		rc = spdk_scsi_lun_allocate_io_channel(desc);
		if (rc != 0) {
			spdk_scsi_lun_close(desc);
			goto error;
		}

		conn->open_lun_descs[i] = desc;
	}

	return;

error:
	spdk_iscsi_conn_close_luns(conn);
}

/**
 *  This function will stop executing the specified connection.
 */
static void
spdk_iscsi_conn_stop(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_tgt_node *target;

	if (conn->state == ISCSI_CONN_STATE_EXITED && conn->sess != NULL &&
	    conn->sess->session_type == SESSION_TYPE_NORMAL &&
	    conn->full_feature) {
		target = conn->sess->target;
		pthread_mutex_lock(&target->mutex);
		target->num_active_conns--;
		pthread_mutex_unlock(&target->mutex);

		spdk_iscsi_conn_close_luns(conn);
	}

	assert(conn->lcore == spdk_env_get_current_core());

	__sync_fetch_and_sub(&g_num_connections[conn->lcore], 1);
	spdk_iscsi_poll_group_remove_conn(conn);
}

void spdk_shutdown_iscsi_conns(void)
{
	struct spdk_iscsi_conn	*conn;
	int			i;

	pthread_mutex_lock(&g_conns_mutex);

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		conn = spdk_find_iscsi_connection_by_id(i);
		if (conn == NULL) {
			continue;
		}

		/* Do not set conn->state if the connection has already started exiting.
		  * This ensures we do not move a connection from EXITED state back to EXITING.
		  */
		if (conn->state < ISCSI_CONN_STATE_EXITING) {
			conn->state = ISCSI_CONN_STATE_EXITING;
		}
	}

	pthread_mutex_unlock(&g_conns_mutex);
	g_shutdown_timer = spdk_poller_register(spdk_iscsi_conn_check_shutdown, NULL,
						1000);
}

int
spdk_iscsi_drop_conns(struct spdk_iscsi_conn *conn, const char *conn_match,
		      int drop_all)
{
	struct spdk_iscsi_conn	*xconn;
	const char			*xconn_match;
	int				i, num;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_drop_conns\n");

	num = 0;
	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		xconn = spdk_find_iscsi_connection_by_id(i);

		if (xconn == NULL) {
			continue;
		}

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
				SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "TSIH=%u\n", xconn->sess->tsih);
			} else {
				SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "TSIH=xx\n");
			}

			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "CID=%u\n", xconn->cid);

			/* Do not set xconn->state if the connection has already started exiting.
			  * This ensures we do not move a connection from EXITED state back to EXITING.
			  */
			if (xconn->state < ISCSI_CONN_STATE_EXITING) {
				xconn->state = ISCSI_CONN_STATE_EXITING;
			}
			num++;
		}
	}

	pthread_mutex_unlock(&g_conns_mutex);

	if (num != 0) {
		SPDK_ERRLOG("exiting %d conns\n", num);
	}

	return 0;
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
spdk_iscsi_conn_read_data(struct spdk_iscsi_conn *conn, int bytes,
			  void *buf)
{
	int ret;

	if (bytes == 0) {
		return 0;
	}

	ret = spdk_sock_recv(conn->sock, buf, bytes);

	if (ret > 0) {
		spdk_trace_record(TRACE_ISCSI_READ_FROM_SOCKET_DONE, conn->id, ret, 0, 0);
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno == ECONNRESET) {
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_sock_recv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return SPDK_ISCSI_CONNECTION_FATAL;
}

void
spdk_iscsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *task = spdk_iscsi_task_from_scsi_task(scsi_task);

	spdk_iscsi_task_mgmt_response(task->conn, task);
	spdk_iscsi_task_put(task);
}

static void
process_completed_read_subtask_list(struct spdk_iscsi_conn *conn,
				    struct spdk_iscsi_task *primary)
{
	struct spdk_iscsi_task *subtask, *tmp;

	TAILQ_FOREACH_SAFE(subtask, &primary->subtask_list, subtask_link, tmp) {
		if (subtask->scsi.offset == primary->bytes_completed) {
			TAILQ_REMOVE(&primary->subtask_list, subtask, subtask_link);
			primary->bytes_completed += subtask->scsi.length;
			spdk_iscsi_task_response(conn, subtask);
			spdk_iscsi_task_put(subtask);
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
		TAILQ_FOREACH(tmp, &primary->subtask_list, subtask_link) {
			spdk_scsi_task_copy_status(&tmp->scsi, &task->scsi);
		}
	}

	if ((task != primary) &&
	    (task->scsi.offset != primary->bytes_completed)) {
		TAILQ_FOREACH(tmp, &primary->subtask_list, subtask_link) {
			if (task->scsi.offset < tmp->scsi.offset) {
				TAILQ_INSERT_BEFORE(tmp, task, subtask_link);
				return;
			}
		}

		TAILQ_INSERT_TAIL(&primary->subtask_list, task, subtask_link);
		return;
	}

	primary->bytes_completed += task->scsi.length;
	spdk_iscsi_task_response(conn, task);

	if ((task != primary) ||
	    (task->scsi.transfer_len == task->scsi.length)) {
		spdk_iscsi_task_put(task);
	}
	process_completed_read_subtask_list(conn, primary);

	spdk_iscsi_conn_handle_queued_datain_tasks(conn);
}

void
spdk_iscsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *primary;
	struct spdk_iscsi_task *task = spdk_iscsi_task_from_scsi_task(scsi_task);
	struct spdk_iscsi_conn *conn = task->conn;
	struct spdk_iscsi_pdu *pdu = task->pdu;

	spdk_trace_record(TRACE_ISCSI_TASK_DONE, conn->id, 0, (uintptr_t)task, 0);

	task->is_queued = false;
	primary = spdk_iscsi_task_get_primary(task);

	if (spdk_iscsi_task_is_read(primary)) {
		process_read_task_completion(conn, task, primary);
	} else {
		primary->bytes_completed += task->scsi.length;
		if (task != primary) {
			if (task->scsi.status == SPDK_SCSI_STATUS_GOOD) {
				primary->scsi.data_transferred += task->scsi.data_transferred;
			} else {
				spdk_scsi_task_copy_status(&primary->scsi, &task->scsi);
			}
		}

		if (primary->bytes_completed == primary->scsi.transfer_len) {
			spdk_del_transfer_task(conn, primary->tag);
			spdk_iscsi_task_response(conn, primary);
			/*
			 * Check if this is the last task completed for an iSCSI write
			 *  that required child subtasks.  If task != primary, we know
			 *  for sure that it was part of an iSCSI write with child subtasks.
			 *  The trickier case is when the last task completed was the initial
			 *  task - in this case the task will have a smaller length than
			 *  the overall transfer length.
			 */
			if (task != primary || task->scsi.length != task->scsi.transfer_len) {
				TAILQ_REMOVE(&conn->active_r2t_tasks, primary, link);
				spdk_iscsi_task_put(primary);
			}
		}
		spdk_iscsi_task_put(task);
	}
	if (!task->parent) {
		spdk_trace_record(TRACE_ISCSI_PDU_COMPLETED, 0, 0, (uintptr_t)pdu, 0);
	}
}

static int
spdk_iscsi_get_pdu_length(struct spdk_iscsi_pdu *pdu, int header_digest,
			  int data_digest)
{
	int data_len, enable_digest, total;

	enable_digest = 1;
	if (pdu->bhs.opcode == ISCSI_OP_LOGIN_RSP) {
		enable_digest = 0;
	}

	total = ISCSI_BHS_LEN;

	total += (4 * pdu->bhs.total_ahs_len);

	if (enable_digest && header_digest) {
		total += ISCSI_DIGEST_LEN;
	}

	data_len = DGET24(pdu->bhs.data_segment_len);
	if (data_len > 0) {
		total += ISCSI_ALIGN(data_len);
		if (enable_digest && data_digest) {
			total += ISCSI_DIGEST_LEN;
		}
	}

	return total;
}

void
spdk_iscsi_conn_handle_nop(struct spdk_iscsi_conn *conn)
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
		if ((tsc - conn->last_nopin) > (conn->timeout  * spdk_get_ticks_hz())) {
			SPDK_ERRLOG("Timed out waiting for NOP-Out response from initiator\n");
			SPDK_ERRLOG("  tsc=0x%lx, last_nopin=0x%lx\n", tsc, conn->last_nopin);
			SPDK_ERRLOG("  initiator=%s, target=%s\n", conn->initiator_name,
				    conn->target_short_name);
			conn->state = ISCSI_CONN_STATE_EXITING;
		}
	} else if (tsc - conn->last_nopin > conn->nopininterval) {
		spdk_iscsi_send_nopin(conn);
	}
}

/**
 * \brief Makes one attempt to flush response PDUs back to the initiator.
 *
 * Builds a list of iovecs for response PDUs that must be sent back to the
 * initiator and passes it to writev().
 *
 * Since the socket is non-blocking, writev() may not be able to flush all
 * of the iovecs, and may even partially flush one of the iovecs.  In this
 * case, the partially flushed PDU will remain on the write_pdu_list with
 * an offset pointing to the next byte to be flushed.
 *
 * Returns 0 if all PDUs were flushed.
 *
 * Returns 1 if some PDUs could not be flushed due to lack of send buffer
 * space.
 *
 * Returns -1 if an exception error occurred indicating the TCP connection
 * should be closed.
 */
static int
spdk_iscsi_conn_flush_pdus_internal(struct spdk_iscsi_conn *conn)
{
	const int array_size = 32;
	struct iovec	iovec_array[array_size];
	struct iovec	*iov = iovec_array;
	int iovec_cnt = 0;
	int bytes = 0;
	int total_length = 0;
	uint32_t writev_offset;
	struct spdk_iscsi_pdu *pdu;
	int pdu_length;

	pdu = TAILQ_FIRST(&conn->write_pdu_list);

	if (pdu == NULL) {
		return 0;
	}

	/*
	 * Build up a list of iovecs for the first few PDUs in the
	 *  connection's write_pdu_list.
	 */
	while (pdu != NULL && ((array_size - iovec_cnt) >= 5)) {
		pdu_length = spdk_iscsi_get_pdu_length(pdu,
						       conn->header_digest,
						       conn->data_digest);
		iovec_cnt += spdk_iscsi_build_iovecs(conn,
						     &iovec_array[iovec_cnt],
						     pdu);
		total_length += pdu_length;
		pdu = TAILQ_NEXT(pdu, tailq);
	}

	/*
	 * Check if the first PDU was partially written out the last time
	 *  this function was called, and if so adjust the iovec array
	 *  accordingly.
	 */
	writev_offset = TAILQ_FIRST(&conn->write_pdu_list)->writev_offset;
	total_length -= writev_offset;
	while (writev_offset > 0) {
		if (writev_offset >= iov->iov_len) {
			writev_offset -= iov->iov_len;
			iov++;
			iovec_cnt--;
		} else {
			iov->iov_len -= writev_offset;
			iov->iov_base = (char *)iov->iov_base + writev_offset;
			writev_offset = 0;
		}
	}

	spdk_trace_record(TRACE_ISCSI_FLUSH_WRITEBUF_START, conn->id, total_length, 0, iovec_cnt);

	bytes = spdk_sock_writev(conn->sock, iov, iovec_cnt);
	if (bytes == -1) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return 1;
		} else {
			SPDK_ERRLOG("spdk_sock_writev() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
			return -1;
		}
	}

	spdk_trace_record(TRACE_ISCSI_FLUSH_WRITEBUF_DONE, conn->id, bytes, 0, 0);

	pdu = TAILQ_FIRST(&conn->write_pdu_list);

	/*
	 * Free any PDUs that were fully written.  If a PDU was only
	 *  partially written, update its writev_offset so that next
	 *  time only the unwritten portion will be sent to writev().
	 */
	while (bytes > 0) {
		pdu_length = spdk_iscsi_get_pdu_length(pdu,
						       conn->header_digest,
						       conn->data_digest);
		pdu_length -= pdu->writev_offset;

		if (bytes >= pdu_length) {
			bytes -= pdu_length;
			TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);

			if ((conn->full_feature) &&
			    (conn->sess->ErrorRecoveryLevel >= 1) &&
			    spdk_iscsi_is_deferred_free_pdu(pdu)) {
				SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "stat_sn=%d\n",
					      from_be32(&pdu->bhs.stat_sn));
				TAILQ_INSERT_TAIL(&conn->snack_pdu_list, pdu,
						  tailq);
			} else {
				spdk_iscsi_conn_free_pdu(conn, pdu);
			}

			pdu = TAILQ_FIRST(&conn->write_pdu_list);
		} else {
			pdu->writev_offset += bytes;
			bytes = 0;
		}
	}

	return TAILQ_EMPTY(&conn->write_pdu_list) ? 0 : 1;
}

/**
 * \brief Flushes response PDUs back to the initiator.
 *
 * This function may return without all PDUs having flushed to the
 * underlying TCP socket buffer - for example, in the case where the
 * socket buffer is already full.
 *
 * During normal RUNNING connection state, if not all PDUs are flushed,
 * then subsequent calls to this routine will eventually flush
 * remaining PDUs.
 *
 * During other connection states (EXITING or LOGGED_OUT), this
 * function will spin until all PDUs have successfully been flushed.
 *
 * Returns 0 for success and when all PDUs were able to be flushed.
 *
 * Returns 1 for success but when some PDUs could not be flushed due
 * to lack of TCP buffer space.
 *
 * Returns -1 for an exceptional error indicating the TCP connection
 * should be closed.
 */
static int
spdk_iscsi_conn_flush_pdus(void *_conn)
{
	struct spdk_iscsi_conn *conn = _conn;
	int rc;

	if (conn->state == ISCSI_CONN_STATE_RUNNING) {
		rc = spdk_iscsi_conn_flush_pdus_internal(conn);
		if (rc == 0 && conn->flush_poller != NULL) {
			spdk_poller_unregister(&conn->flush_poller);
		} else if (rc == 1 && conn->flush_poller == NULL) {
			conn->flush_poller = spdk_poller_register(spdk_iscsi_conn_flush_pdus, conn, 50);
		}
	} else {
		/*
		 * If the connection state is not RUNNING, then
		 * keep trying to flush PDUs until our list is
		 * empty - to make sure all data is sent before
		 * closing the connection.
		 */
		do {
			rc = spdk_iscsi_conn_flush_pdus_internal(conn);
		} while (rc == 1);
	}

	if (rc < 0 && conn->state < ISCSI_CONN_STATE_EXITING) {
		/*
		 * If the poller has already started destruction of the connection,
		 *  i.e. the socket read failed, then the connection state may already
		 *  be EXITED.  We don't want to set it back to EXITING in that case.
		 */
		conn->state = ISCSI_CONN_STATE_EXITING;
	}

	return -1;
}

void
spdk_iscsi_conn_write_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	TAILQ_INSERT_TAIL(&conn->write_pdu_list, pdu, tailq);
	spdk_iscsi_conn_flush_pdus(conn);
}

#define GET_PDU_LOOP_COUNT	16

static int
spdk_iscsi_conn_handle_incoming_pdus(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *pdu;
	int i, rc;

	/* Read new PDUs from network */
	for (i = 0; i < GET_PDU_LOOP_COUNT; i++) {
		rc = spdk_iscsi_read_pdu(conn, &pdu);
		if (rc == 0) {
			break;
		} else if (rc == SPDK_ISCSI_CONNECTION_FATAL) {
			return rc;
		}

		if (conn->state == ISCSI_CONN_STATE_LOGGED_OUT) {
			SPDK_ERRLOG("pdu received after logout\n");
			spdk_put_pdu(pdu);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}

		rc = spdk_iscsi_execute(conn, pdu);
		spdk_put_pdu(pdu);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_iscsi_execute() fatal error on %s(%s)\n",
				    conn->target_port != NULL ? spdk_scsi_port_get_name(conn->target_port) : "NULL",
				    conn->initiator_port != NULL ? spdk_scsi_port_get_name(conn->initiator_port) : "NULL");
			return rc;
		}

		spdk_trace_record(TRACE_ISCSI_TASK_EXECUTED, 0, 0, (uintptr_t)pdu, 0);
		if (conn->is_stopped) {
			break;
		}
	}

	return i;
}

static void
spdk_iscsi_conn_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	assert(conn != NULL);

	if ((conn->state == ISCSI_CONN_STATE_EXITED) ||
	    (conn->state == ISCSI_CONN_STATE_EXITING)) {
		return;
	}

	/* Handle incoming PDUs */
	rc = spdk_iscsi_conn_handle_incoming_pdus(conn);
	if (rc < 0) {
		conn->state = ISCSI_CONN_STATE_EXITING;
		spdk_iscsi_conn_flush_pdus(conn);
	}
}

static void
spdk_iscsi_conn_full_feature_migrate(void *arg1, void *arg2)
{
	struct spdk_iscsi_conn *conn = arg1;

	if (conn->sess->session_type == SESSION_TYPE_NORMAL) {
		spdk_iscsi_conn_open_luns(conn);
	}

	/* The poller has been unregistered, so now we can re-register it on the new core. */
	conn->lcore = spdk_env_get_current_core();
	spdk_iscsi_poll_group_add_conn(conn);
}

void
spdk_iscsi_conn_migration(struct spdk_iscsi_conn *conn)
{
	int				lcore;
	struct spdk_event		*event;
	struct spdk_iscsi_tgt_node *target;

	lcore = spdk_iscsi_conn_allocate_reactor(conn->portal->cpumask);
	if (conn->sess->session_type == SESSION_TYPE_NORMAL) {
		target = conn->sess->target;
		pthread_mutex_lock(&target->mutex);
		target->num_active_conns++;
		if (target->num_active_conns == 1) {
			/**
			 * This is the only active connection for this target node.
			 *  Save the lcore in the target node so it can be used for
			 *  any other connections to this target node.
			 */
			target->lcore = lcore;
		} else {
			/**
			 * There are other active connections for this target node.
			 *  Ignore the lcore specified by the allocator and use the
			 *  the target node's lcore to ensure this connection runs on
			 *  the same lcore as other connections for this target node.
			 */
			lcore = target->lcore;
		}
		pthread_mutex_unlock(&target->mutex);
	}

	spdk_iscsi_poll_group_remove_conn_sock(conn);
	spdk_poller_unregister(&conn->flush_poller);
	spdk_iscsi_conn_stop(conn);

	__sync_fetch_and_add(&g_num_connections[lcore], 1);
	conn->last_nopin = spdk_get_ticks();
	event = spdk_event_allocate(lcore, spdk_iscsi_conn_full_feature_migrate,
				    conn, NULL);
	spdk_event_call(event);
}

void
spdk_iscsi_conn_set_min_per_core(int count)
{
	g_connections_per_lcore = count;
}

int
spdk_iscsi_conn_get_min_per_core(void)
{
	return g_connections_per_lcore;
}

static uint32_t
spdk_iscsi_conn_allocate_reactor(const struct spdk_cpuset *cpumask)
{
	uint32_t i, selected_core;
	int32_t num_pollers, min_pollers;

	min_pollers = INT_MAX;
	selected_core = spdk_env_get_first_core();

	SPDK_ENV_FOREACH_CORE(i) {
		if (!spdk_cpuset_get_cpu(cpumask, i)) {
			continue;
		}

		/* This core is running. Check how many pollers it already has. */
		num_pollers = g_num_connections[i];

		if ((num_pollers > 0) && (num_pollers < g_connections_per_lcore)) {
			/* Fewer than the maximum connections per core,
			 * but at least 1. Use this core.
			 */
			return i;
		} else if (num_pollers < min_pollers) {
			/* Track the core that has the minimum number of pollers
			 * to be used if no cores meet our criteria
			 */
			selected_core = i;
			min_pollers = num_pollers;
		}
	}

	return selected_core;
}

static int
logout_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	spdk_iscsi_conn_destruct(conn);

	return -1;
}

void
spdk_iscsi_conn_logout(struct spdk_iscsi_conn *conn)
{
	conn->state = ISCSI_CONN_STATE_LOGGED_OUT;
	conn->logout_timer = spdk_poller_register(logout_timeout, conn, ISCSI_LOGOUT_TIMEOUT * 1000000);
}

SPDK_TRACE_REGISTER_FN(iscsi_conn_trace)
{
	spdk_trace_register_owner(OWNER_ISCSI_CONN, 'c');
	spdk_trace_register_object(OBJECT_ISCSI_PDU, 'p');
	spdk_trace_register_description("ISCSI_READ_FROM_SOCKET_DONE", "",
					TRACE_ISCSI_READ_FROM_SOCKET_DONE,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("ISCSI_FLUSH_WRITEBUF_START", "", TRACE_ISCSI_FLUSH_WRITEBUF_START,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, "iovec: ");
	spdk_trace_register_description("ISCSI_FLUSH_WRITEBUF_DONE", "", TRACE_ISCSI_FLUSH_WRITEBUF_DONE,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("ISCSI_READ_PDU", "", TRACE_ISCSI_READ_PDU,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 1, 0, "opc:   ");
	spdk_trace_register_description("ISCSI_TASK_DONE", "", TRACE_ISCSI_TASK_DONE,
					OWNER_ISCSI_CONN, OBJECT_SCSI_TASK, 0, 0, "");
	spdk_trace_register_description("ISCSI_TASK_QUEUE", "", TRACE_ISCSI_TASK_QUEUE,
					OWNER_ISCSI_CONN, OBJECT_SCSI_TASK, 1, 1, "pdu:   ");
	spdk_trace_register_description("ISCSI_TASK_EXECUTED", "", TRACE_ISCSI_TASK_EXECUTED,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 0, 0, "");
	spdk_trace_register_description("ISCSI_PDU_COMPLETED", "", TRACE_ISCSI_PDU_COMPLETED,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 0, 0, "");
}
