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
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/queue.h"
#include "spdk/trace.h"
#include "spdk/net.h"
#include "spdk/sock.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

#include "memcached/conn.h"
#include "memcached/tgt_node.h"
#include "memcached/portal_grp.h"
#include "memcached/memcached_cmd.h"

static int g_connections_per_lcore;
static uint32_t *g_num_connections;

struct spdk_memcached_conn *g_conns_array = MAP_FAILED;
static int g_conns_array_fd = -1;
static char g_shm_name[64];

static pthread_mutex_t g_conns_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_poller *g_shutdown_timer = NULL;

static void
memcached_poll_group_remove_conn(struct spdk_memcached_conn *conn);

void
spdk_memcached_conn_set_min_per_core(int count)
{
	g_connections_per_lcore = count;
}

int
spdk_memcached_conn_get_min_per_core(void)
{
	return g_connections_per_lcore;
}

static struct spdk_memcached_conn *
spdk_find_memcached_connection_by_id(int cid)
{
	if (g_conns_array[cid].is_valid == 1) {
		return &g_conns_array[cid];
	} else {
		return NULL;
	}
}

#if 1 // conns initialize and shutdown
#define SPDK_MEMCACHED_CONNECTION_MEMSET(conn)		\
	memset(&(conn)->portal, 0, sizeof(*(conn)) -	\
		offsetof(struct spdk_memcached_conn, portal));

static struct spdk_memcached_conn *
memcached_allocate_conn(void)
{
	struct spdk_memcached_conn	*conn;
	int				i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < MAX_MEMCACHED_CONNECTIONS; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid) {
			SPDK_MEMCACHED_CONNECTION_MEMSET(conn);
			conn->is_valid = 1;
			pthread_mutex_unlock(&g_conns_mutex);
			return conn;
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);

	return NULL;
}

static void
memcached_free_conn(struct spdk_memcached_conn *conn)
{
	free(conn->portal_host);
	free(conn->portal_port);
	conn->is_valid = 0;
}

static int
spdk_memcached_get_active_conns(void)
{
	struct spdk_memcached_conn *conn;
	int num = 0;
	int i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < MAX_MEMCACHED_CONNECTIONS; i++) {
		conn = spdk_find_memcached_connection_by_id(i);
		if (conn == NULL) {
			continue;
		}
		num++;
	}
	pthread_mutex_unlock(&g_conns_mutex);
	return num;
}

static void
spdk_memcached_conns_cleanup(void)
{
	free(g_num_connections);
	munmap(g_conns_array, sizeof(struct spdk_memcached_conn) *
	       MAX_MEMCACHED_CONNECTIONS);
	shm_unlink(g_shm_name);
	if (g_conns_array_fd >= 0) {
		close(g_conns_array_fd);
		g_conns_array_fd = -1;
	}
}

static void
spdk_memcached_conn_check_shutdown_cb(void *arg1, void *arg2)
{
	spdk_memcached_conns_cleanup();
	spdk_shutdown_memcached_conns_done();
}

static int
spdk_memcached_conn_check_shutdown(void *arg)
{
	struct spdk_event *event;

	if (spdk_memcached_get_active_conns() != 0) {
		return 1;
	}

	spdk_poller_unregister(&g_shutdown_timer);
	event = spdk_event_allocate(spdk_env_get_current_core(),
				    spdk_memcached_conn_check_shutdown_cb, NULL, NULL);
	spdk_event_call(event);

	return 1;
}

/**
 *  This function will stop executing the specified connection.
 */
static void
spdk_memcached_conn_stop(struct spdk_memcached_conn *conn)
{
	struct spdk_memcached_tgt_node *target;

	if (conn->state == MEMCACHED_CONN_STATE_EXITED) {
		target = conn->target;
		pthread_mutex_lock(&target->mutex);
		target->num_active_conns--;
		pthread_mutex_unlock(&target->mutex);

		//TODO: replace memcached service
//		spdk_memcached_conn_close_luns(conn);
	}

	assert(conn->lcore == spdk_env_get_current_core());

	__sync_fetch_and_sub(&g_num_connections[conn->lcore], 1);
	memcached_poll_group_remove_conn(conn);
}

void spdk_shutdown_memcached_conns(void)
{
	struct spdk_memcached_conn	*conn;
	int			i;

	pthread_mutex_lock(&g_conns_mutex);

	for (i = 0; i < MAX_MEMCACHED_CONNECTIONS; i++) {
		conn = spdk_find_memcached_connection_by_id(i);
		if (conn == NULL) {
			continue;
		}

		/* Do not set conn->state if the connection has already started exiting.
		  * This ensures we do not move a connection from EXITED state back to EXITING.
		  */
		if (conn->state < MEMCACHED_CONN_STATE_EXITING) {
			conn->state = MEMCACHED_CONN_STATE_EXITING;
		}
	}

	pthread_mutex_unlock(&g_conns_mutex);
	g_shutdown_timer = spdk_poller_register(spdk_memcached_conn_check_shutdown, NULL,
						1000);
}
#endif


int
spdk_memcached_conn_read_data(struct spdk_memcached_conn *conn, int bytes,
			      void *buf)
{
	int ret;

	if (bytes == 0) {
		return 0;
	}

	ret = spdk_sock_recv(conn->sock, buf, bytes);

	if (ret > 0) {
//		spdk_trace_record(TRACE_ISCSI_READ_FROM_SOCKET_DONE, conn->id, ret, 0, 0);
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno == ECONNRESET) {
			SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "spdk_sock_recv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return SPDK_MEMCACHED_CONNECTION_FATAL;
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
spdk_memcached_conn_flush_cmds_internal(struct spdk_memcached_conn *conn)
{
	const int num_iovs = 32;
	struct iovec iovs[num_iovs];
	struct iovec *iov = iovs;
	int iovcnt = 0;
	int bytes = 0;
	uint32_t total_length = 0;
	uint32_t mapped_length = 0;
	struct spdk_memcached_cmd *cmd;
	int cmd_length;

	cmd = TAILQ_FIRST(&conn->write_cmd_list);

	if (cmd == NULL) {
		return 0;
	}

	/*
	 * Build up a list of iovecs for the first few PDUs in the
	 *  connection's write_pdu_list. For the first PDU, check if it was
	 *  partially written out the last time this function was called, and
	 *  if so adjust the iovec array accordingly. This check is done in
	 *  spdk_iscsi_build_iovs() and so applied to remaining PDUs too.
	 *  But extra overhead is negligible.
	 */
	while (cmd != NULL && ((num_iovs - iovcnt) > 0)) {
		iovcnt += spdk_memcached_cmd_build_iovs(&iovs[iovcnt], num_iovs - iovcnt,
							cmd, &mapped_length);
		total_length += mapped_length;
		cmd = TAILQ_NEXT(cmd, tailq);
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "iovcnt %d; iov first len is %d\n", iovcnt, iov[0].iov_len);
	bytes = spdk_sock_writev(conn->sock, iov, iovcnt);
	if (bytes == -1) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return 1;
		} else {
			SPDK_ERRLOG("spdk_sock_writev() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
			return -1;
		}
	}

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "Actual sent len: %d\n", bytes);

	cmd = TAILQ_FIRST(&conn->write_cmd_list);

	/*
	 * Free any PDUs that were fully written.  If a PDU was only
	 *  partially written, update its writev_offset so that next
	 *  time only the unwritten portion will be sent to writev().
	 */
	while (cmd && bytes > 0) {
		cmd_length = spdk_memcached_cmd_get_sendlen(cmd);
		cmd_length -= spdk_memcached_cmd_get_sendoff(cmd);

		if (bytes >= cmd_length) {
			bytes -= cmd_length;
			TAILQ_REMOVE(&conn->write_cmd_list, cmd, tailq);

			spdk_memcached_conn_free_cmd(conn, cmd);

			cmd = TAILQ_FIRST(&conn->write_cmd_list);
		} else {
			spdk_memcached_cmd_incr_sendoff(cmd, bytes);
			bytes = 0;
		}
	}

	return TAILQ_EMPTY(&conn->write_cmd_list) ? 0 : 1;
}

int
spdk_memcached_conn_flush_cmds(void *_conn)
{
	struct spdk_memcached_conn *conn = _conn;
	int rc;

	if (conn->state == MEMCACHED_CONN_STATE_RUNNING) {
		rc = spdk_memcached_conn_flush_cmds_internal(conn);
		if (rc == 0 && conn->flush_poller != NULL) {
			spdk_poller_unregister(&conn->flush_poller);
		} else if (rc == 1 && conn->flush_poller == NULL) {
			conn->flush_poller = spdk_poller_register(spdk_memcached_conn_flush_cmds,
					     conn, 50);
		}
	} else {
		/*
		 * If the connection state is not RUNNING, then
		 * keep trying to flush PDUs until our list is
		 * empty - to make sure all data is sent before
		 * closing the connection.
		 */
		do {
			rc = spdk_memcached_conn_flush_cmds_internal(conn);
		} while (rc == 1);
	}

	if (rc < 0 && conn->state < MEMCACHED_CONN_STATE_EXITING) {
		/*
		 * If the poller has already started destruction of the connection,
		 *  i.e. the socket read failed, then the connection state may already
		 *  be EXITED.  We don't want to set it back to EXITING in that case.
		 */
		conn->state = MEMCACHED_CONN_STATE_EXITING;
	}

	return 1;
}

void
spdk_memcached_conn_handle_nop(struct spdk_memcached_conn *conn)
{

}

#define GET_CMD_LOOP_COUNT	16

static int
spdk_memcached_conn_handle_incoming_pdus(struct spdk_memcached_conn *conn)
{
	struct spdk_memcached_cmd *cmd;
	int i, rc;

	/* Read new CMDs from network */
	for (i = 0; i < GET_CMD_LOOP_COUNT; i++) {
		rc = spdk_memcached_cmd_read(conn, &cmd);
		if (rc == 0) {
			break;
		} else if (rc < 0){
			SPDK_ERRLOG("cmd received after logout\n");
			return SPDK_MEMCACHED_CONNECTION_FATAL;
		}

		assert(rc == 1);
		rc = spdk_memcached_cmd_execute(conn, cmd);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_memcached_execute() fatal error\n");
			return rc;
		}

		if (conn->is_stopped) {
			break;
		}
	}

	return i;
}

#if 1 /* conn & poll_group */
static void
_memcached_conn_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_memcached_conn *conn = arg;
	int rc;

	assert(conn != NULL);

	if ((conn->state == MEMCACHED_CONN_STATE_EXITED) ||
	    (conn->state == MEMCACHED_CONN_STATE_EXITING)) {
		return;
	}

	/* Handle incoming PDUs */
	rc = spdk_memcached_conn_handle_incoming_pdus(conn);
	if (rc < 0) {
		conn->state = MEMCACHED_CONN_STATE_EXITING;
		spdk_memcached_conn_flush_cmds(conn);
	}
}

static void
memcached_poll_group_add_conn_sock(struct spdk_memcached_conn *conn)
{
	struct spdk_memcached_poll_group *poll_group;
	int rc;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_memcached.poll_group[conn->lcore];

	rc = spdk_sock_group_add_sock(poll_group->sock_group, conn->sock, _memcached_conn_sock_cb, conn);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add sock=%p of conn=%p\n", conn->sock, conn);
	}
}

static void
memcached_poll_group_remove_conn_sock(struct spdk_memcached_conn *conn)
{
	struct spdk_memcached_poll_group *poll_group;
	int rc;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_memcached.poll_group[conn->lcore];

	rc = spdk_sock_group_remove_sock(poll_group->sock_group, conn->sock);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to remove sock=%p of conn=%p\n", conn->sock, conn);
	}
}

static void
memcached_poll_group_add_conn(struct spdk_memcached_conn *conn)
{
	struct spdk_memcached_poll_group *poll_group;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_memcached.poll_group[conn->lcore];

	conn->is_stopped = false;
	STAILQ_INSERT_TAIL(&poll_group->connections, conn, link);
	memcached_poll_group_add_conn_sock(conn);
}

static void
memcached_poll_group_remove_conn(struct spdk_memcached_conn *conn)
{
	struct spdk_memcached_poll_group *poll_group;

	assert(conn->lcore == spdk_env_get_current_core());

	poll_group = &g_spdk_memcached.poll_group[conn->lcore];

	conn->is_stopped = true;
	STAILQ_REMOVE(&poll_group->connections, conn, spdk_memcached_conn, link);
}
#endif

#if 1 /* spdk_memcached_conn_construct start */
static uint32_t
memcached_conn_allocate_reactor(const struct spdk_cpuset *cpumask)
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

static void
_memcached_conn_migration(void *arg1, void *arg2)
{
	struct spdk_memcached_conn *conn = arg1;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED_CONN, "Launching connection on polling thread\n");

	/* The poller has been unregistered, so now we can re-register it on the new core. */
	assert(conn->lcore == spdk_env_get_current_core());
	memcached_poll_group_add_conn(conn);
}

static void
memcached_conn_migration(struct spdk_memcached_conn *conn)
{
	struct spdk_event		*event;
	struct spdk_memcached_tgt_node *target = conn->target;

	conn->lcore = memcached_conn_allocate_reactor(conn->portal->cpumask);
	pthread_mutex_lock(&target->mutex);
	target->num_active_conns++;
	pthread_mutex_unlock(&target->mutex);
	__sync_fetch_and_add(&g_num_connections[conn->lcore], 1);

	event = spdk_event_allocate(conn->lcore, _memcached_conn_migration,
				    conn, NULL);
	spdk_event_call(event);
}

int
spdk_memcached_conn_construct(struct spdk_memcached_portal *portal,
			      struct spdk_sock *sock)
{
	struct spdk_memcached_conn *conn;
	int bufsize, i, rc;
	(void)i;
	(void)rc;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED_CONN, "Prepare connection on acceptor thread\n");
	conn = memcached_allocate_conn();
	if (conn == NULL) {
		SPDK_ERRLOG("Could not allocate connection.\n");
		return -1;
	}

	pthread_mutex_lock(&g_spdk_memcached.mutex);
	conn->timeout = g_spdk_memcached.timeout;
	pthread_mutex_unlock(&g_spdk_memcached.mutex);

	conn->portal = portal;
	conn->pg_tag = portal->group->tag;
	conn->portal_host = strdup(portal->host);
	conn->portal_port = strdup(portal->port);
	conn->portal_cpumask = portal->cpumask;
	conn->sock = sock;

	conn->state = MEMCACHED_CONN_STATE_INVALID;

	TAILQ_INIT(&conn->write_cmd_list);

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

	bufsize = 32 * 1024 * 1024 / g_spdk_memcached.MaxConnections;
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

	conn->logout_timer = NULL;
	conn->shutdown_timer = NULL;

	conn->target = spdk_memcached_portal_grp_get_target(portal->group);

	memcached_conn_migration(conn);

	return 0;

error_return:
	memcached_free_conn(conn);
	return -1;
}
#endif

#if 1 /* spdk_memcached_conn_destruct */
static void
_spdk_memcached_conn_free(struct spdk_memcached_conn *conn)
{
	if (conn == NULL) {
		return;
	}

	/*
	 * Each connection pre-allocates its next PDU - make sure these get
	 *  freed here.
	 */
	spdk_memcached_put_cmd(conn->cmd_in_recv);

	memcached_free_conn(conn);
}

static void
spdk_memcached_conn_free(struct spdk_memcached_conn *conn)
{
	pthread_mutex_lock(&g_conns_mutex);

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "cleanup free conn\n");
	_spdk_memcached_conn_free(conn);

	pthread_mutex_unlock(&g_conns_mutex);
}

static int
memcached_conn_check_shutdown(void *arg)
{
	struct spdk_memcached_conn *conn = arg;
	int rc = 0;

//	rc = spdk_memcached_conn_free_tasks(conn);
	if (rc < 0) {
		return 1;
	}

	spdk_poller_unregister(&conn->shutdown_timer);

	spdk_memcached_conn_stop(conn);
	spdk_memcached_conn_free(conn);

	return 1;
}

static void
_spdk_memcached_conn_destruct(struct spdk_memcached_conn *conn)
{
	int rc = 0;

//	spdk_clear_all_transfer_task(conn, NULL, NULL);
	memcached_poll_group_remove_conn_sock(conn);
	spdk_sock_close(&conn->sock);
	spdk_poller_unregister(&conn->logout_timer);
	spdk_poller_unregister(&conn->flush_poller);

//	rc = spdk_memcached_conn_free_tasks(conn);
	if (rc < 0) {
		/* The connection cannot be freed yet. Check back later. */
		conn->shutdown_timer = spdk_poller_register(memcached_conn_check_shutdown, conn, 1000);
	} else {
		spdk_memcached_conn_stop(conn);
		spdk_memcached_conn_free(conn);
	}
}

void
spdk_memcached_conn_destruct(struct spdk_memcached_conn *conn)
{
	/* If a connection is already in exited status, just return */
	if (conn->state >= MEMCACHED_CONN_STATE_EXITED) {
		return;
	}

	conn->state = MEMCACHED_CONN_STATE_EXITED;
	_spdk_memcached_conn_destruct(conn);
}
#endif





void
spdk_memcached_conn_write_cmd(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd *cmd)
{
	TAILQ_INSERT_TAIL(&conn->write_cmd_list, cmd, tailq);
	spdk_memcached_conn_flush_cmds(conn);
}

void
spdk_memcached_conn_free_cmd(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd *cmd)
{
	spdk_memcached_put_cmd(cmd);
}









#if 1 /* spdk_memcached_initialze_conns */
int spdk_memcached_initialze_conns(void)
{
	size_t conns_size = sizeof(struct spdk_memcached_conn) * MAX_MEMCACHED_CONNECTIONS;
	uint32_t i, last_core;

	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "spdk_memcached_init\n");

	snprintf(g_shm_name, sizeof(g_shm_name), "/spdk_memcached_conns.%d", spdk_app_get_shm_id());
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

	for (i = 0; i < MAX_MEMCACHED_CONNECTIONS; i++) {
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
#endif

SPDK_LOG_REGISTER_COMPONENT("memcached_conn", SPDK_LOG_MEMCACHED_CONN)
