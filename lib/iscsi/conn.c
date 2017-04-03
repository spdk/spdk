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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include "spdk/queue.h"
#include <sys/ioctl.h>
#include <sys/epoll.h>

#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/trace.h"
#include "spdk/net.h"

#include "spdk_internal/log.h"

#include "iscsi/task.h"
#include "iscsi/conn.h"
#include "iscsi/tgt_node.h"
#include "iscsi/portal_grp.h"
#include "spdk/scsi.h"

#define SPDK_ISCSI_CONNECTION_MEMSET(conn)		\
	memset(&(conn)->portal, 0, sizeof(*(conn)) -	\
		offsetof(struct spdk_iscsi_conn, portal));

#define MICROSECOND_TO_TSC(x) ((x) * spdk_get_ticks_hz()/1000000)
static int64_t g_conn_idle_interval_in_tsc = -1;

#define DEFAULT_CONNECTIONS_PER_LCORE	4
#define SPDK_MAX_POLLERS_PER_CORE	4096
static int g_connections_per_lcore = DEFAULT_CONNECTIONS_PER_LCORE;
static uint32_t g_num_connections[SPDK_MAX_LCORE];

struct spdk_iscsi_conn *g_conns_array;
static char g_shm_name[64];

static pthread_mutex_t g_conns_mutex;

static struct spdk_poller *g_shutdown_timer = NULL;

static uint32_t spdk_iscsi_conn_allocate_reactor(uint64_t cpumask);
static void __add_idle_conn(void *arg1, void *arg2);

/** Global variables used for managing idle connections. */
static int g_epoll_fd = 0;
static struct spdk_poller *g_idle_conn_poller;
static STAILQ_HEAD(idle_list, spdk_iscsi_conn) g_idle_conn_list_head;

void spdk_iscsi_conn_login_do_work(void *arg);
void spdk_iscsi_conn_full_feature_do_work(void *arg);
void spdk_iscsi_conn_idle_do_work(void *arg);

static void spdk_iscsi_conn_full_feature_migrate(void *arg1, void *arg2);
static struct spdk_event *spdk_iscsi_conn_get_migrate_event(struct spdk_iscsi_conn *conn,
		int *lcore);
static void spdk_iscsi_conn_stop_poller(struct spdk_iscsi_conn *conn, spdk_event_fn fn_after_stop,
					int lcore);

void spdk_iscsi_set_min_conn_idle_interval(int interval_in_us)
{
	g_conn_idle_interval_in_tsc = MICROSECOND_TO_TSC(interval_in_us);
}

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

static int
init_idle_conns(void)
{
	assert(g_epoll_fd == 0);
	g_epoll_fd = epoll_create1(0);
	if (g_epoll_fd < 0) {
		SPDK_ERRLOG("epoll_create1 failed master lcore\n");
		return -1;
	}

	return 0;
}

static int
add_idle_conn(struct spdk_iscsi_conn *conn)
{
	struct epoll_event event;
	int rc;

	event.events = EPOLLIN;
	event.data.u64 = 0LL;
	event.data.ptr = conn;

	rc = epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, conn->sock, &event);
	if (rc == 0) {
		return 0;
	} else {
		SPDK_ERRLOG("conn epoll_ctl failed\n");
		return -1;
	}
}

static int
del_idle_conn(struct spdk_iscsi_conn *conn)
{
	struct epoll_event event;
	int rc;

	/*
	 * The event parameter is ignored but needs to be non-NULL to work around a bug in old
	 * kernel versions.
	 */
	rc = epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, conn->sock, &event);
	if (rc == 0) {
		return 0;
	} else {
		SPDK_ERRLOG("epoll_ctl(EPOLL_CTL_DEL) failed\n");
		return -1;
	}
}

static void
check_idle_conns(void)
{
	struct epoll_event events[SPDK_MAX_POLLERS_PER_CORE];
	int i;
	int nfds;
	struct spdk_iscsi_conn *conn;

	/* if nothing idle, can exit now */
	if (STAILQ_EMPTY(&g_idle_conn_list_head)) {
		/* this epoll_wait is needed to finish socket closing process */
		epoll_wait(g_epoll_fd, events, SPDK_MAX_POLLERS_PER_CORE, 0);
	}

	/* Perform a non-blocking epoll */
	nfds = epoll_wait(g_epoll_fd, events, SPDK_MAX_POLLERS_PER_CORE, 0);
	if (nfds < 0) {
		SPDK_ERRLOG("epoll_wait failed! (ret: %d)\n", nfds);
		return;
	}

	if (nfds > SPDK_MAX_POLLERS_PER_CORE) {
		SPDK_ERRLOG("epoll_wait events exceeded limit! %d > %d\n", nfds,
			    SPDK_MAX_POLLERS_PER_CORE);
		assert(0);
	}

	/*
	 * In the case of any event cause (EPOLLIN or EPOLLERR)
	 * just make the connection active for normal process loop.
	 */
	for (i = 0; i < nfds; i++) {

		conn = (struct spdk_iscsi_conn *)events[i].data.ptr;

		/*
		 * Flag the connection that an event was noticed
		 * such that during the list scan process it will
		 * be re-inserted into the active ring
		 */
		conn->pending_activate_event = true;
	}
}

int spdk_initialize_iscsi_conns(void)
{
	size_t conns_size;
	int conns_array_fd;
	int i, rc;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_iscsi_init\n");

	rc = pthread_mutex_init(&g_conns_mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("mutex_init() failed\n");
		return -1;
	}

	snprintf(g_shm_name, sizeof(g_shm_name), "spdk_iscsi_conns.%d", spdk_app_get_shm_id());
	conns_array_fd = shm_open(g_shm_name, O_RDWR | O_CREAT, 0600);
	if (conns_array_fd < 0) {
		SPDK_ERRLOG("could not shm_open %s\n", g_shm_name);
		return -1;
	}

	conns_size = sizeof(struct spdk_iscsi_conn) * MAX_ISCSI_CONNECTIONS;

	if (ftruncate(conns_array_fd, conns_size) != 0) {
		SPDK_ERRLOG("could not ftruncate\n");
		close(conns_array_fd);
		shm_unlink(g_shm_name);
		return -1;
	}
	g_conns_array = mmap(0, conns_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			     conns_array_fd, 0);

	memset(g_conns_array, 0, conns_size);

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		g_conns_array[i].id = i;
	}

	for (i = 0; i < SPDK_MAX_LCORE; i++) {
		g_num_connections[i] = 0;
	}

	if (g_conn_idle_interval_in_tsc == -1)
		spdk_iscsi_set_min_conn_idle_interval(spdk_net_framework_idle_time());

	STAILQ_INIT(&g_idle_conn_list_head);
	if (init_idle_conns() < 0) {
		return -1;
	}

	spdk_poller_register(&g_idle_conn_poller, spdk_iscsi_conn_idle_do_work, NULL,
			     spdk_get_master_lcore(), 0);

	return 0;
}

/**

\brief Create an iSCSI connection from the given parameters and schedule it
       on a reactor.

\code

# identify reactor where the new connections work item will be scheduled
reactor = spdk_iscsi_conn_allocate_reactor()
allocate spdk_iscsi_conn object
initialize spdk_iscsi_conn object
schedule iSCSI connection work item on reactor

\endcode

*/
int
spdk_iscsi_conn_construct(struct spdk_iscsi_portal *portal,
			  int sock)
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
	conn->sock = sock;

	conn->state = ISCSI_CONN_STATE_INVALID;
	conn->login_phase = ISCSI_SECURITY_NEGOTIATION_PHASE;
	conn->ttt = 0;

	conn->partial_text_parameter = NULL;

	for (i = 0; i < MAX_CONNECTION_PARAMS; i++)
		conn->conn_param_state_negotiated[i] = false;

	for (i = 0; i < MAX_SESSION_PARAMS; i++)
		conn->sess_param_state_negotiated[i] = false;

	for (i = 0; i < DEFAULT_MAXR2T; i++)
		conn->outstanding_r2t_tasks[i] = NULL;

	TAILQ_INIT(&conn->write_pdu_list);
	TAILQ_INIT(&conn->snack_pdu_list);
	TAILQ_INIT(&conn->queued_r2t_tasks);
	TAILQ_INIT(&conn->active_r2t_tasks);
	TAILQ_INIT(&conn->queued_datain_tasks);

	rc = spdk_sock_getaddr(sock, conn->target_addr,
			       sizeof conn->target_addr,
			       conn->initiator_addr, sizeof conn->initiator_addr);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed\n");
		goto error_return;
	}

	bufsize = 2 * 1024 * 1024;
	rc = spdk_sock_set_recvbuf(conn->sock, bufsize);
	if (rc != 0)
		SPDK_ERRLOG("spdk_sock_set_recvbuf failed\n");

	bufsize = 32 * 1024 * 1024 / g_spdk_iscsi.MaxConnections;
	if (bufsize > 2 * 1024 * 1024) {
		bufsize = 2 * 1024 * 1024;
	}
	rc = spdk_sock_set_sendbuf(conn->sock, bufsize);
	if (rc != 0)
		SPDK_ERRLOG("spdk_sock_set_sendbuf failed\n");

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
	conn->is_idle = 0;
	conn->logout_timer = NULL;
	conn->shutdown_timer = NULL;
	SPDK_NOTICELOG("Launching connection on acceptor thread\n");
	conn->last_activity_tsc = spdk_get_ticks();
	conn->pending_task_cnt = 0;
	conn->pending_activate_event = false;

	/*
	 * Since we are potentially moving control of this socket to a different
	 *  core, suspend the connection here.  This ensures any necessary libuns
	 *  housekeeping for TCP socket to lcore associations gets cleared.
	 */
	conn->lcore = spdk_app_get_current_core();
	spdk_net_framework_clear_socket_association(conn->sock);
	__sync_fetch_and_add(&g_num_connections[conn->lcore], 1);
	spdk_poller_register(&conn->poller, spdk_iscsi_conn_login_do_work, conn,
			     conn->lcore, 0);

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
				spdk_iscsi_conn_handle_queued_tasks(conn);
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
	struct spdk_iscsi_pdu *pdu;
	struct spdk_iscsi_task *iscsi_task;

	while (!TAILQ_EMPTY(&conn->write_pdu_list)) {
		pdu = TAILQ_FIRST(&conn->write_pdu_list);
		TAILQ_REMOVE(&conn->write_pdu_list, pdu, tailq);
		if (pdu->task) {
			spdk_iscsi_task_put(pdu->task);
		}
		spdk_put_pdu(pdu);
	}

	while (!TAILQ_EMPTY(&conn->snack_pdu_list)) {
		pdu = TAILQ_FIRST(&conn->snack_pdu_list);
		TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
		if (pdu->task) {
			spdk_iscsi_task_put(pdu->task);
		}
		spdk_put_pdu(pdu);
	}

	while (!TAILQ_EMPTY(&conn->queued_datain_tasks)) {
		iscsi_task = TAILQ_FIRST(&conn->queued_datain_tasks);
		TAILQ_REMOVE(&conn->queued_datain_tasks, iscsi_task, link);
		pdu = iscsi_task->pdu;
		spdk_iscsi_task_put(iscsi_task);
		spdk_put_pdu(pdu);
	}

	if (conn->pending_task_cnt)
		return -1;

	return 0;

}

static void spdk_iscsi_conn_free(struct spdk_iscsi_conn *conn)
{

	if (conn == NULL)
		return;

	spdk_iscsi_param_free(conn->params);

	/*
	 * Each connection pre-allocates its next PDU - make sure these get
	 *  freed here.
	 */
	spdk_put_pdu(conn->pdu_in_progress);

	free(conn->auth.user);
	free(conn->auth.secret);
	free(conn->auth.muser);
	free(conn->auth.msecret);
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
		SPDK_TRACELOG(SPDK_TRACE_DEBUG,
			      "cleanup last conn free sess\n");
		spdk_free_sess(sess);
	}


	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "cleanup free conn\n");
	spdk_iscsi_conn_free(conn);
}

static void
spdk_iscsi_conn_cleanup_backend(struct spdk_iscsi_conn *conn)
{
	int rc;

	if (conn->sess->connections > 1) {
		/* connection specific cleanup */
	} else if (!g_spdk_iscsi.AllowDuplicateIsid) {
		/* clean up all tasks to all LUNs for session */
		rc = spdk_iscsi_tgt_node_cleanup_luns(conn,
						      conn->sess->target);
		if (rc < 0)
			SPDK_ERRLOG("target abort failed\n");
	}
}

static void
_spdk_iscsi_conn_free(void *arg1, void *arg2)
{
	struct spdk_iscsi_conn	*conn = arg1;

	pthread_mutex_lock(&g_conns_mutex);
	spdk_iscsi_remove_conn(conn);
	pthread_mutex_unlock(&g_conns_mutex);

	__sync_fetch_and_sub(&g_num_connections[spdk_app_get_current_core()], 1);
}

static void
_spdk_iscsi_conn_check_shutdown(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;
	int rc;

	rc = spdk_iscsi_conn_free_tasks(conn);
	if (rc < 0) {
		return;
	}

	spdk_poller_unregister(&conn->shutdown_timer, NULL);

	spdk_iscsi_conn_stop_poller(conn, _spdk_iscsi_conn_free, spdk_app_get_current_core());
}

void spdk_iscsi_conn_destruct(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_tgt_node	*target;
	int				rc;

	conn->state = ISCSI_CONN_STATE_EXITED;

	if (conn->sess != NULL && conn->pending_task_cnt > 0) {
		target = conn->sess->target;
		if (target != NULL) {
			spdk_iscsi_conn_cleanup_backend(conn);
		}
	}

	spdk_clear_all_transfer_task(conn, NULL);
	spdk_sock_close(conn->sock);
	spdk_poller_unregister(&conn->logout_timer, NULL);

	rc = spdk_iscsi_conn_free_tasks(conn);
	if (rc < 0) {
		/* The connection cannot be freed yet. Check back later. */
		spdk_poller_register(&conn->shutdown_timer, _spdk_iscsi_conn_check_shutdown, conn,
				     spdk_app_get_current_core(), 1000);
	} else {
		spdk_iscsi_conn_stop_poller(conn, _spdk_iscsi_conn_free, spdk_app_get_current_core());
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
		if (conn == NULL)
			continue;
		num++;
	}
	pthread_mutex_unlock(&g_conns_mutex);
	return num;
}

static void
spdk_iscsi_conns_cleanup(void)
{
	munmap(g_conns_array, sizeof(struct spdk_iscsi_conn) *
	       MAX_ISCSI_CONNECTIONS);
	shm_unlink(g_shm_name);
}

static void
spdk_iscsi_conn_check_shutdown(void *arg)
{
	if (spdk_iscsi_get_active_conns() == 0) {
		spdk_poller_unregister(&g_shutdown_timer, NULL);
		spdk_iscsi_conns_cleanup();
		spdk_app_stop(0);
	}
}

static struct spdk_event *
spdk_iscsi_conn_get_migrate_event(struct spdk_iscsi_conn *conn, int *_lcore)
{
	struct spdk_event *event;
	struct spdk_iscsi_tgt_node *target;
	int lcore;

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
	if (_lcore != NULL) {
		*_lcore = lcore;
	}

	event = spdk_event_allocate(lcore, spdk_iscsi_conn_full_feature_migrate, conn, NULL);

	return event;
}

/**
 *  This function will stop the poller for the specified connection, and then call function
 *  fn_after_stop() on the specified lcore.
 */
static void
spdk_iscsi_conn_stop_poller(struct spdk_iscsi_conn *conn, spdk_event_fn fn_after_stop, int lcore)
{
	struct spdk_event *event;
	struct spdk_iscsi_tgt_node *target;

	if (conn->sess != NULL && conn->sess->session_type == SESSION_TYPE_NORMAL) {
		target = conn->sess->target;
		pthread_mutex_lock(&target->mutex);
		target->num_active_conns--;
		pthread_mutex_unlock(&target->mutex);

		assert(conn->dev != NULL);
		spdk_scsi_dev_free_io_channels(conn->dev);
	}
	__sync_fetch_and_sub(&g_num_connections[spdk_app_get_current_core()], 1);
	spdk_net_framework_clear_socket_association(conn->sock);
	event = spdk_event_allocate(lcore, fn_after_stop, conn, NULL);
	spdk_poller_unregister(&conn->poller, event);
}

void spdk_shutdown_iscsi_conns(void)
{
	struct spdk_iscsi_conn	*conn, *tmp;
	int				i;

	/* cleanup - move conns from list back into ring
	   where they will get cleaned up
	 */
	STAILQ_FOREACH_SAFE(conn, &g_idle_conn_list_head, link, tmp) {
		STAILQ_REMOVE(&g_idle_conn_list_head, conn, spdk_iscsi_conn, link);
		spdk_event_call(spdk_iscsi_conn_get_migrate_event(conn, NULL));
		conn->is_idle = 0;
		del_idle_conn(conn);
	}

	pthread_mutex_lock(&g_conns_mutex);

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		conn = spdk_find_iscsi_connection_by_id(i);
		if (conn == NULL)
			continue;
		conn->state = ISCSI_CONN_STATE_EXITING;
	}

	pthread_mutex_unlock(&g_conns_mutex);
	spdk_poller_register(&g_shutdown_timer, spdk_iscsi_conn_check_shutdown, NULL,
			     spdk_get_master_lcore(), 1000);
}

int
spdk_iscsi_drop_conns(struct spdk_iscsi_conn *conn, const char *conn_match,
		      int drop_all)
{
	struct spdk_iscsi_conn	*xconn;
	const char			*xconn_match;
	int				i, num;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_iscsi_drop_conns\n");

	num = 0;
	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		xconn = spdk_find_iscsi_connection_by_id(i);

		if (xconn == NULL)
			continue;

		if (xconn == conn)
			continue;

		if (!drop_all && xconn->initiator_port == NULL)
			continue;

		xconn_match =
			drop_all ? xconn->initiator_name : xconn->initiator_port->name;

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

			SPDK_ERRLOG("exiting conn by %s (%s)",
				    xconn_match, xconn->initiator_addr);
			if (xconn->sess != NULL) {
				SPDK_TRACELOG(SPDK_TRACE_ISCSI, "TSIH=%u\n", xconn->sess->tsih);
			} else {
				SPDK_TRACELOG(SPDK_TRACE_ISCSI, "TSIH=xx\n");
			}

			SPDK_TRACELOG(SPDK_TRACE_ISCSI, "CID=%u\n", xconn->cid);
			xconn->state = ISCSI_CONN_STATE_EXITING;
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

 \brief Reads data for the specified iSCSI connection from its TCP socket.

 The TCP socket is marked as non-blocking, so this function may not read
 all data requested.

 Returns SPDK_ISCSI_CONNECTION_FATAL if the recv() operation indicates a fatal
 error with the TCP connection (including if the TCP connection was closed
 unexpectedly.

 Otherwise returns the number of bytes successfully read.

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
		spdk_trace_record(TRACE_READ_FROM_SOCKET_DONE, conn->id, ret, 0, 0);
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else
			SPDK_ERRLOG("Socket read error(%d): %s\n", errno, strerror(errno));
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	/* connection closed */
	if (ret == 0) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	return ret;
}

void
process_task_mgmt_completion(void *arg1, void *arg2)
{
	struct spdk_iscsi_conn *conn = arg1;
	struct spdk_iscsi_task *task = arg2;

	conn->last_activity_tsc = spdk_get_ticks();
	spdk_iscsi_task_mgmt_response(conn, task);
	spdk_iscsi_task_put(task);
}

static void
process_completed_read_subtask_list(struct spdk_iscsi_conn *conn,
				    struct spdk_iscsi_task *primary)
{
	struct spdk_scsi_task *tmp;

	while (!TAILQ_EMPTY(&primary->scsi.subtask_list)) {
		tmp = TAILQ_FIRST(&primary->scsi.subtask_list);
		if (tmp->offset == primary->scsi.bytes_completed) {
			TAILQ_REMOVE(&primary->scsi.subtask_list, tmp, scsi_link);
			primary->scsi.bytes_completed += tmp->length;
			spdk_iscsi_task_response(conn, (struct spdk_iscsi_task *)tmp);
			spdk_iscsi_task_put((struct spdk_iscsi_task *)tmp);
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
	struct spdk_scsi_task *tmp;
	bool flag = false;

	if ((task != primary) &&
	    (task->scsi.offset != primary->scsi.bytes_completed)) {
		TAILQ_FOREACH(tmp, &primary->scsi.subtask_list, scsi_link) {
			if (task->scsi.offset < tmp->offset) {
				TAILQ_INSERT_BEFORE(tmp, &task->scsi, scsi_link);
				flag = true;
				break;
			}
		}
		if (!flag) {
			TAILQ_INSERT_TAIL(&primary->scsi.subtask_list, &task->scsi, scsi_link);
		}
		return;
	}

	primary->scsi.bytes_completed += task->scsi.length;
	spdk_iscsi_task_response(conn, task);

	if ((task != primary) ||
	    (task->scsi.transfer_len == task->scsi.length)) {
		spdk_iscsi_task_put(task);
	}
	process_completed_read_subtask_list(conn, primary);
}

void process_task_completion(void *arg1, void *arg2)
{
	struct spdk_iscsi_conn *conn = arg1;
	struct spdk_iscsi_task *task = arg2;
	struct spdk_iscsi_task *primary;

	assert(task != NULL);
	spdk_trace_record(TRACE_ISCSI_TASK_DONE, conn->id, 0, (uintptr_t)task, 0);
	conn->last_activity_tsc = spdk_get_ticks();

	primary = spdk_iscsi_task_get_primary(task);

	if (spdk_iscsi_task_is_read(primary)) {
		process_read_task_completion(conn, task, primary);
	} else {
		primary->scsi.bytes_completed += task->scsi.length;
		if ((task != primary) &&
		    (task->scsi.status != SPDK_SCSI_STATUS_GOOD)) {
			memcpy(primary->scsi.sense_data, task->scsi.sense_data,
			       task->scsi.sense_data_len);
			primary->scsi.sense_data_len = task->scsi.sense_data_len;
			primary->scsi.status = task->scsi.status;
		}

		if (primary->scsi.bytes_completed == primary->scsi.transfer_len) {
			spdk_del_transfer_task(conn, primary->scsi.id);
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

static int
spdk_iscsi_conn_handle_nop(struct spdk_iscsi_conn *conn)
{
	uint64_t	tsc;

	/* Check for nop interval expiration */
	tsc = spdk_get_ticks();
	if (conn->nop_outstanding) {
		if ((tsc - conn->last_nopin) > (conn->timeout  * spdk_get_ticks_hz())) {
			SPDK_ERRLOG("Timed out waiting for NOP-Out response from initiator\n");
			SPDK_ERRLOG("  tsc=0x%lx, last_nopin=0x%lx\n", tsc, conn->last_nopin);
			return -1;
		}
	} else if (tsc - conn->last_nopin > conn->nopininterval) {
		conn->last_nopin = tsc;
		spdk_iscsi_send_nopin(conn);
	}

	return 0;
}

/**

 \brief Makes one attempt to flush response PDUs back to the initiator.

 Builds a list of iovecs for response PDUs that must be sent back to the
 initiator and passes it to writev().

 Since the socket is non-blocking, writev() may not be able to flush all
 of the iovecs, and may even partially flush one of the iovecs.  In this
 case, the partially flushed PDU will remain on the write_pdu_list with
 an offset pointing to the next byte to be flushed.

 Returns 0 if no exceptional error encountered.  This includes cases where
 there are no PDUs to flush or not all PDUs could be flushed.

 Returns -1 if an exception error occurred indicating the TCP connection
 should be closed.

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

	spdk_trace_record(TRACE_FLUSH_WRITEBUF_START, conn->id, total_length, 0, iovec_cnt);

	bytes = spdk_sock_writev(conn->sock, iov, iovec_cnt);
	if (bytes == -1) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return 0;
		} else {
			perror("writev");
			return -1;
		}
	}

	spdk_trace_record(TRACE_FLUSH_WRITEBUF_DONE, conn->id, bytes, 0, 0);

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
				SPDK_TRACELOG(SPDK_TRACE_DEBUG, "stat_sn=%d\n",
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

	return 0;
}

/**

 \brief Flushes response PDUs back to the initiator.

 This function may return without all PDUs having flushed to the
 underlying TCP socket buffer - for example, in the case where the
 socket buffer is already full.

 During normal RUNNING connection state, if not all PDUs are flushed,
 then subsequent calls to this routine will eventually flush
 remaining PDUs.

 During other connection states (EXITING or LOGGED_OUT), this
 function will spin until all PDUs have successfully been flushed.

 Returns 0 for success.

 Returns -1 for an exceptional error indicating the TCP connection
 should be closed.

*/
static int
spdk_iscsi_conn_flush_pdus(struct spdk_iscsi_conn *conn)
{
	int rc;

	if (conn->state == ISCSI_CONN_STATE_RUNNING) {
		rc = spdk_iscsi_conn_flush_pdus_internal(conn);
	} else {
		rc = 0;

		/*
		 * If the connection state is not RUNNING, then
		 * keep trying to flush PDUs until our list is
		 * empty - to make sure all data is sent before
		 * closing the connection.
		 */
		while (!TAILQ_EMPTY(&conn->write_pdu_list) > 0) {
			rc = spdk_iscsi_conn_flush_pdus_internal(conn);
			if (rc != 0) {
				break;
			}
		}
	}

	return rc;
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
				    conn->target_port != NULL ? conn->target_port->name : "NULL",
				    conn->initiator_port != NULL ? conn->initiator_port->name : "NULL");
			return rc;
		}
	}

	return i;
}

static void spdk_iscsi_conn_handle_idle(struct spdk_iscsi_conn *conn)
{
	uint64_t current_tsc = spdk_get_ticks();

	if (g_conn_idle_interval_in_tsc > 0 &&
	    ((int64_t)(current_tsc - conn->last_activity_tsc)) >= g_conn_idle_interval_in_tsc &&
	    conn->pending_task_cnt == 0) {

		spdk_trace_record(TRACE_ISCSI_CONN_IDLE, conn->id, 0, 0, 0);
		spdk_iscsi_conn_stop_poller(conn, __add_idle_conn, spdk_get_master_lcore());
	}
}

static int
spdk_iscsi_conn_execute(struct spdk_iscsi_conn *conn)
{
	int				rc = 0;
	uint64_t			tsc;
	bool				conn_active = false;

	if (conn->state == ISCSI_CONN_STATE_EXITED) {
		return -1;
	}

	if (conn->state == ISCSI_CONN_STATE_EXITING) {
		goto conn_exit;
	}
	/* Check for nop interval expiration */
	rc = spdk_iscsi_conn_handle_nop(conn);
	if (rc < 0) {
		conn->state = ISCSI_CONN_STATE_EXITING;
		goto conn_exit;
	}

	/* Handle incoming PDUs */
	rc = spdk_iscsi_conn_handle_incoming_pdus(conn);
	if (rc < 0) {
		conn->state = ISCSI_CONN_STATE_EXITING;
		spdk_iscsi_conn_flush_pdus(conn);
		goto conn_exit;
	} else if (rc > 0) {
		conn_active = true;
	}

	/* If flush timer has expired, flush all PDUs */
	tsc = spdk_get_ticks();
	if (tsc - conn->last_flush > g_spdk_iscsi.flush_timeout) {
		conn->last_flush = tsc;
		if (spdk_iscsi_conn_flush_pdus(conn) != 0) {
			conn->state = ISCSI_CONN_STATE_EXITING;
			goto conn_exit;
		}
	}

	if (conn_active) {
		return 1;
	}

conn_exit:
	if (conn->state == ISCSI_CONN_STATE_EXITING) {
		spdk_iscsi_conn_destruct(conn);
		return -1;
	}

	return 0;
}

static void
spdk_iscsi_conn_full_feature_migrate(void *arg1, void *arg2)
{
	struct spdk_iscsi_conn *conn = arg1;

	if (conn->sess->session_type == SESSION_TYPE_NORMAL) {
		assert(conn->dev != NULL);
		spdk_scsi_dev_allocate_io_channels(conn->dev);
	}

	/* The poller has been unregistered, so now we can re-register it on the new core. */
	conn->lcore = spdk_app_get_current_core();
	spdk_poller_register(&conn->poller, spdk_iscsi_conn_full_feature_do_work, conn,
			     conn->lcore, 0);
}

void
spdk_iscsi_conn_login_do_work(void *arg)
{
	struct spdk_iscsi_conn	*conn = arg;
	int				lcore;
	int				rc;
	struct spdk_event		*event;

	/* General connection processing */
	rc = spdk_iscsi_conn_execute(conn);
	if (rc < 0)
		return;

	/* Check if this connection transitioned to full feature phase. If it
	 * did, migrate it to a dedicated reactor for the target node.
	 */
	if (conn->login_phase == ISCSI_FULL_FEATURE_PHASE) {
		event = spdk_iscsi_conn_get_migrate_event(conn, &lcore);
		__sync_fetch_and_sub(&g_num_connections[spdk_app_get_current_core()], 1);
		__sync_fetch_and_add(&g_num_connections[lcore], 1);
		spdk_net_framework_clear_socket_association(conn->sock);
		spdk_poller_unregister(&conn->poller, event);
	}
}

void
spdk_iscsi_conn_full_feature_do_work(void *arg)
{
	struct spdk_iscsi_conn	*conn = arg;
	int				rc = 0;

	rc = spdk_iscsi_conn_execute(conn);
	if (rc < 0) {
		return;
	} else if (rc > 0) {
		conn->last_activity_tsc = spdk_get_ticks();
	}

	/* Check if the session was idle during this access pass. If it was,
	   and it was idle longer than the configured timeout, migrate this
	   session to the master core. */
	spdk_iscsi_conn_handle_idle(conn);
}

/**

\brief This is the main routine for the iSCSI 'idle' connection
work item.

This function handles processing of connecitons whose state have
been determined as 'idle' for lack of activity.  These connections
no longer reside in the reactor's poller ring, instead they have
been staged into an idle list.  This function utilizes the use of
epoll as a non-blocking means to test for new socket connection
events that indicate the connection should be moved back into the
active ring.

While in the idle list, this function must scan these connections
to process required timer based actions that must be maintained
even though the connection is considered 'idle'.

*/
void spdk_iscsi_conn_idle_do_work(void *arg)
{
	uint64_t	tsc;
	struct spdk_iscsi_conn *tconn;

	check_idle_conns();

	/* Now walk the idle list to process timer based actions */
	STAILQ_FOREACH(tconn, &g_idle_conn_list_head, link) {

		assert(tconn->is_idle == 1);

		if (tconn->pending_activate_event == false) {
			tsc = spdk_get_ticks();
			if (tsc - tconn->last_nopin > tconn->nopininterval) {
				tconn->pending_activate_event = true;
			}
		}

		if (tconn->pending_activate_event) {
			int lcore;

			spdk_trace_record(TRACE_ISCSI_CONN_ACTIVE, tconn->id, 0, 0, 0);

			/* remove connection from idle list */
			STAILQ_REMOVE(&g_idle_conn_list_head, tconn, spdk_iscsi_conn, link);
			tconn->last_activity_tsc = spdk_get_ticks();
			tconn->pending_activate_event = false;
			tconn->is_idle = 0;
			del_idle_conn(tconn);
			/* migrate work item to new core */
			spdk_net_framework_clear_socket_association(tconn->sock);
			spdk_event_call(spdk_iscsi_conn_get_migrate_event(tconn, &lcore));
			__sync_fetch_and_add(&g_num_connections[lcore], 1);
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, "add conn id = %d, cid = %d poller = %p to lcore = %d active\n",
				      tconn->id, tconn->cid, &tconn->poller, lcore);
		}
	} /* for each conn in idle list */
}

static void
__add_idle_conn(void *arg1, void *arg2)
{
	struct spdk_iscsi_conn *conn = arg1;
	int rc;

	/*
	 * The iSCSI target may have started shutting down when this connection was
	 *  determined as idle.  In that case, do not append the connection to the
	 *  idle list - just start the work item again so it can start its shutdown
	 *  process.
	 */
	if (conn->state == ISCSI_CONN_STATE_EXITING) {
		spdk_event_call(spdk_iscsi_conn_get_migrate_event(conn, NULL));
		return;
	}

	rc = add_idle_conn(conn);
	if (rc == 0) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "add conn id = %d, cid = %d poller = %p to idle\n",
			      conn->id, conn->cid, conn->poller);
		conn->is_idle = 1;
		STAILQ_INSERT_TAIL(&g_idle_conn_list_head, conn, link);
	} else {
		SPDK_ERRLOG("add_idle_conn() failed\n");
	}
}

void
spdk_iscsi_conn_set_min_per_core(int count)
{
	g_connections_per_lcore = count;
}

static uint32_t
spdk_iscsi_conn_allocate_reactor(uint64_t cpumask)
{
	uint32_t i, selected_core;
	enum spdk_lcore_state_t state;
	uint32_t master_lcore = spdk_get_master_lcore();
	int32_t num_pollers, min_pollers;

	cpumask &= spdk_app_get_core_mask();
	if (cpumask == 0) {
		return 0;
	}

	min_pollers = INT_MAX;
	selected_core = 0;

	/* we use u64 as CPU core mask */
	for (i = 0; i < SPDK_MAX_LCORE && i < 64; i++) {
		if (!((1ULL << i) & cpumask)) {
			continue;
		}

		/*
		 * DPDK returns WAIT for the master lcore instead of RUNNING.
		 * So we always treat the reactor on master core as RUNNING.
		 */
		if (i == master_lcore) {
			state = RUNNING_1;
		} else {
			state = spdk_get_lcore_state(i);
		}
		if (state == FINISHED_2) {
			spdk_wait_lcore(i);
		}

		switch (state) {
		case WAIT_0:
		case FINISHED_2:
			/* Idle cores have 0 pollers. */
			if (0 < min_pollers) {
				selected_core = i;
				min_pollers = 0;
			}
			break;
		case RUNNING_1:
			/* This lcore is running. Check how many pollers it already has. */
			num_pollers = g_num_connections[i];

			if ((num_pollers > 0) && (num_pollers < g_connections_per_lcore)) {
				/* Fewer than the maximum connections per lcore,
				 * but at least 1. Use this lcore.
				 */
				return i;
			} else if (num_pollers < min_pollers) {
				/* Track the core that has the minimum number of pollers
				 * to be used if no cores meet our criteria
				 */
				selected_core = i;
				min_pollers = num_pollers;
			}
			break;
		}
	}

	return selected_core;
}

static void
logout_timeout(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	spdk_iscsi_conn_destruct(conn);
}

void
spdk_iscsi_conn_logout(struct spdk_iscsi_conn *conn)
{
	conn->state = ISCSI_CONN_STATE_LOGGED_OUT;
	spdk_poller_register(&conn->logout_timer, logout_timeout, conn, spdk_app_get_current_core(),
			     ISCSI_LOGOUT_TIMEOUT * 1000000);
}

SPDK_TRACE_REGISTER_FN(iscsi_conn_trace)
{
	spdk_trace_register_owner(OWNER_ISCSI_CONN, 'c');
	spdk_trace_register_object(OBJECT_ISCSI_PDU, 'p');
	spdk_trace_register_description("READ FROM SOCKET DONE", "", TRACE_READ_FROM_SOCKET_DONE,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, 0, "");
	spdk_trace_register_description("FLUSH WRITEBUF START", "", TRACE_FLUSH_WRITEBUF_START,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, 0, "iovec: ");
	spdk_trace_register_description("FLUSH WRITEBUF DONE", "", TRACE_FLUSH_WRITEBUF_DONE,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, 0, "");
	spdk_trace_register_description("READ PDU", "", TRACE_READ_PDU,
					OWNER_ISCSI_CONN, OBJECT_ISCSI_PDU, 1, 0, 0, "opc:   ");
	spdk_trace_register_description("ISCSI TASK DONE", "", TRACE_ISCSI_TASK_DONE,
					OWNER_ISCSI_CONN, OBJECT_SCSI_TASK, 0, 0, 0, "");
	spdk_trace_register_description("ISCSI TASK QUEUE", "", TRACE_ISCSI_TASK_QUEUE,
					OWNER_ISCSI_CONN, OBJECT_SCSI_TASK, 1, 1, 0, "pdu:   ");
	spdk_trace_register_description("ISCSI CONN ACTIVE", "", TRACE_ISCSI_CONN_ACTIVE,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, 0, "");
	spdk_trace_register_description("ISCSI CONN IDLE", "", TRACE_ISCSI_CONN_IDLE,
					OWNER_ISCSI_CONN, OBJECT_NONE, 0, 0, 0, "");
}
