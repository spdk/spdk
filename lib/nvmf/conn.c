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

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <inttypes.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_timer.h>

#include "spdk/nvmf_spec.h"
#include "conn.h"
#include "rdma.h"
#include "request.h"
#include "session.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/trace.h"


/** \file

*/

static rte_atomic32_t g_num_connections[RTE_MAX_LCORE];

static int g_max_conns;
static struct spdk_nvmf_conn *g_conns_array;

static pthread_mutex_t g_conns_mutex;

static struct rte_timer g_shutdown_timer;

static int nvmf_allocate_reactor(uint64_t cpumask);
static void spdk_nvmf_conn_do_work(void *arg);

static struct spdk_nvmf_conn *
allocate_conn(void)
{
	struct spdk_nvmf_conn	*conn;
	int				i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < g_max_conns; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid) {
			memset(conn, 0, sizeof(*conn));
			conn->is_valid = 1;
			pthread_mutex_unlock(&g_conns_mutex);
			return conn;
		}
	}
	pthread_mutex_unlock(&g_conns_mutex);

	return NULL;
}

static void
free_conn(struct spdk_nvmf_conn *conn)
{
	conn->sess = NULL;
	conn->is_valid = 0;
}

int spdk_initialize_nvmf_conns(int max_connections)
{
	int i, rc;

	rc = pthread_mutex_init(&g_conns_mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("mutex_init() failed\n");
		return -1;
	}

	g_max_conns = max_connections;
	g_conns_array = calloc(g_max_conns, sizeof(struct spdk_nvmf_conn));

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		rte_atomic32_set(&g_num_connections[i], 0);
	}

	return 0;
}

struct spdk_nvmf_conn *
spdk_nvmf_allocate_conn(void)
{
	struct spdk_nvmf_conn *conn;

	conn = allocate_conn();
	if (conn == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		goto err0;
	}

	/* all new connections initially default as AQ until nvmf connect */
	conn->type = CONN_TYPE_AQ;

	/* no session association until nvmf connect */
	conn->sess = NULL;

	conn->state = CONN_STATE_INVALID;
	conn->sq_head = 0;

	return conn;

err0:
	return NULL;
}

/**

\brief Create an NVMf fabric connection from the given parameters and schedule it
       on a reactor thread.

\code

# identify reactor where the new connections work item will be scheduled
reactor = nvmf_allocate_reactor()
schedule fabric connection work item on reactor

\endcode

*/
int
spdk_nvmf_startup_conn(struct spdk_nvmf_conn *conn)
{
	int lcore;
	uint64_t nvmf_session_core = spdk_app_get_core_mask();

	lcore = nvmf_allocate_reactor(nvmf_session_core);
	if (lcore < 0) {
		SPDK_ERRLOG("Unable to find core to launch connection.\n");
		goto err0;
	}

	conn->state = CONN_STATE_RUNNING;
	SPDK_NOTICELOG("Launching nvmf connection[qid=%d] on core: %d\n",
		       conn->qid, lcore);
	conn->poller.fn = spdk_nvmf_conn_do_work;
	conn->poller.arg = conn;

	rte_atomic32_inc(&g_num_connections[lcore]);
	spdk_poller_register(&conn->poller, lcore, NULL);

	return 0;
err0:
	free_conn(conn);
	return -1;
}

static void
_conn_destruct(spdk_event_t event)
{
	struct spdk_nvmf_conn *conn = spdk_event_get_arg1(event);

	/*
	 * Notify NVMf library of the fabric connection
	 * going away.  If this is the AQ connection then
	 * set state for other connections to abort.
	 */
	nvmf_disconnect(conn->sess, conn);

	if (conn->type == CONN_TYPE_AQ) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "AQ connection destruct, trigger session closure\n");
		/* Trigger all I/O connections to shutdown */
		conn->state = CONN_STATE_FABRIC_DISCONNECT;
	}

	nvmf_rdma_conn_cleanup(conn);

	pthread_mutex_lock(&g_conns_mutex);
	free_conn(conn);
	pthread_mutex_unlock(&g_conns_mutex);
}

static void spdk_nvmf_conn_destruct(struct spdk_nvmf_conn *conn)
{
	struct spdk_event *event;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "conn %p\n", conn);
	conn->state = CONN_STATE_INVALID;

	event = spdk_event_allocate(rte_lcore_id(), _conn_destruct, conn, NULL, NULL);
	spdk_poller_unregister(&conn->poller, event);
	rte_atomic32_dec(&g_num_connections[rte_lcore_id()]);
}

static int
spdk_nvmf_get_active_conns(void)
{
	struct spdk_nvmf_conn *conn;
	int num = 0;
	int i;

	pthread_mutex_lock(&g_conns_mutex);
	for (i = 0; i < g_max_conns; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid)
			continue;
		num++;
	}
	pthread_mutex_unlock(&g_conns_mutex);
	return num;
}

static void
spdk_nvmf_cleanup_conns(void)
{
	free(g_conns_array);
}

static void
spdk_nvmf_conn_check_shutdown(struct rte_timer *timer, void *arg)
{
	if (spdk_nvmf_get_active_conns() == 0) {
		RTE_VERIFY(timer == &g_shutdown_timer);
		rte_timer_stop(timer);
		spdk_nvmf_cleanup_conns();
		spdk_app_stop(0);
	}
}

void spdk_shutdown_nvmf_conns(void)
{
	struct spdk_nvmf_conn	*conn;
	int				i;

	pthread_mutex_lock(&g_conns_mutex);

	for (i = 0; i < g_max_conns; i++) {
		conn = &g_conns_array[i];
		if (!conn->is_valid)
			continue;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Set conn %d state to exiting\n", i);
		conn->state = CONN_STATE_EXITING;
	}

	pthread_mutex_unlock(&g_conns_mutex);
	rte_timer_init(&g_shutdown_timer);
	rte_timer_reset(&g_shutdown_timer, rte_get_timer_hz() / 1000, PERIODICAL,
			rte_get_master_lcore(), spdk_nvmf_conn_check_shutdown, NULL);
}

static void
spdk_nvmf_conn_do_work(void *arg)
{
	struct spdk_nvmf_conn *conn = arg;

	/* process pending NVMe device completions */
	if (conn->sess) {
		if (conn->type == CONN_TYPE_AQ) {
			nvmf_check_admin_completions(conn->sess);
		} else {
			nvmf_check_io_completions(conn->sess);
		}
	}

	/* process pending RDMA completions */
	if (nvmf_check_rdma_completions(conn) < 0) {
		SPDK_ERRLOG("Transport poll failed for conn %p; closing connection\n", conn);
		conn->state = CONN_STATE_EXITING;
	}

	if (conn->state == CONN_STATE_EXITING ||
	    conn->state == CONN_STATE_FABRIC_DISCONNECT) {
		spdk_nvmf_conn_destruct(conn);
	}
}

static int
nvmf_allocate_reactor(uint64_t cpumask)
{
	return rte_get_master_lcore();
}
