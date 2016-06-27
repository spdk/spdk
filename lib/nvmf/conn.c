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

#include "nvmf.h"
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
static char g_shm_name[64];
static int g_conns_array_fd;

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

static struct spdk_nvmf_conn *
spdk_find_nvmf_conn_by_cntlid(int cntlid)
{
	int i;

	for (i = 0; i < g_max_conns; i++) {
		if ((g_conns_array[i].is_valid == 1) &&
		    (g_conns_array[i].cntlid == cntlid) &&
		    (g_conns_array[i].qid == 0)) {
			return &g_conns_array[i];
		}
	}

	return NULL;
}

int spdk_initialize_nvmf_conns(int max_connections)
{
	size_t conns_size;
	int i, rc;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");

	rc = pthread_mutex_init(&g_conns_mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("mutex_init() failed\n");
		return -1;
	}

	sprintf(g_shm_name, "nvmf_conns.%d", spdk_app_get_instance_id());
	g_conns_array_fd = shm_open(g_shm_name, O_RDWR | O_CREAT, 0600);
	if (g_conns_array_fd < 0) {
		SPDK_ERRLOG("could not shm_open %s\n", g_shm_name);
		return -1;
	}

	g_max_conns = max_connections;
	conns_size = sizeof(struct spdk_nvmf_conn) * g_max_conns;

	if (ftruncate(g_conns_array_fd, conns_size) != 0) {
		SPDK_ERRLOG("could not ftruncate\n");
		shm_unlink(g_shm_name);
		close(g_conns_array_fd);
		return -1;
	}
	g_conns_array = mmap(0, conns_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			     g_conns_array_fd, 0);

	memset(g_conns_array, 0, conns_size);

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
	struct spdk_nvmf_conn *admin_conn;
	uint64_t nvmf_session_core = spdk_app_get_core_mask();

	/*
	 * if starting IO connection then determine core
	 * allocated to admin queue to request core mask.
	 * Can not assume nvmf session yet created at time
	 * of fabric connection setup.  Rely on fabric
	 * function to locate matching controller session.
	 */
	if (conn->type == CONN_TYPE_IOQ && conn->cntlid != 0) {
		admin_conn = spdk_find_nvmf_conn_by_cntlid(conn->cntlid);
		if (admin_conn != NULL) {
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Located admin conn session core %d\n",
				      admin_conn->poller.lcore);
			nvmf_session_core = 1ULL << admin_conn->poller.lcore;
		}
	}

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
	nvmf_disconnect((void *)conn, conn->sess);

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
	munmap(g_conns_array, sizeof(struct spdk_nvmf_conn) * g_max_conns);
	shm_unlink(g_shm_name);
	close(g_conns_array_fd);
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

void
nvmf_init_conn_properites(struct spdk_nvmf_conn *conn,
			  struct nvmf_session *session,
			  struct spdk_nvmf_fabric_connect_rsp *response)
{

	struct spdk_nvmf_extended_identify_ctrlr_data *lcdata;
	uint32_t mdts;

	conn->cntlid = response->status_code_specific.success.cntlid;
	session->max_connections_allowed = g_nvmf_tgt.MaxConnectionsPerSession;
	nvmf_init_session_properties(session, conn->rdma.sq_depth);

	/* Update the session logical controller data with any
	 * application fabric side limits
	 */
	/* reset mdts in vcdata to equal the application default maximum */
	mdts = SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE /
	       (1 << (12 + session->vcprop.cap_hi.bits.mpsmin));
	if (mdts == 0) {
		SPDK_ERRLOG("Min page size exceeds max transfer size!\n");
		SPDK_ERRLOG("Verify setting of SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE and mpsmin\n");
		session->vcdata.mdts = 1; /* Support single page for now */
	} else {
		/* set mdts as a power of 2 representing number of mpsmin units */
		session->vcdata.mdts = 0;
		while ((1ULL << session->vcdata.mdts) < mdts) {
			session->vcdata.mdts++;
		}
	}

	/* increase the I/O recv capsule size for in_capsule data */
	lcdata = (struct spdk_nvmf_extended_identify_ctrlr_data *)&session->vcdata.reserved5[1088];
	lcdata->ioccsz += (g_nvmf_tgt.MaxInCapsuleData / 16);

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
	int i, selected_core;
	enum rte_lcore_state_t state;
	int master_lcore = rte_get_master_lcore();
	int32_t num_pollers, min_pollers;

	cpumask &= spdk_app_get_core_mask();
	if (cpumask == 0) {
		return 0;
	}

	min_pollers = INT_MAX;
	selected_core = 0;

	/* we use u64 as CPU core mask */
	for (i = 0; i < RTE_MAX_LCORE && i < 64; i++) {
		if (!((1ULL << i) & cpumask)) {
			continue;
		}

		/*
		 * DPDK returns WAIT for the master lcore instead of RUNNING.
		 * So we always treat the reactor on master core as RUNNING.
		 */
		if (i == master_lcore) {
			state = RUNNING;
		} else {
			state = rte_eal_get_lcore_state(i);
		}
		if (state == FINISHED) {
			rte_eal_wait_lcore(i);
		}

		switch (state) {
		case WAIT:
		case FINISHED:
			/* Idle cores have 0 pollers */
			if (0 < min_pollers) {
				selected_core = i;
				min_pollers = 0;
			}
			break;
		case RUNNING:
			/* This lcore is running, check how many pollers it already has */
			num_pollers = rte_atomic32_read(&g_num_connections[i]);

			/* Fill each lcore to target minimum, else select least loaded lcore */
			if (num_pollers < (SPDK_NVMF_DEFAULT_NUM_SESSIONS_PER_LCORE *
					   g_nvmf_tgt.MaxConnectionsPerSession)) {
				/* If fewer than the target number of session connections
				 * exist then add to this lcore
				 */
				return i;
			} else if (num_pollers < min_pollers) {
				/* Track the lcore that has the minimum number of pollers
				 * to be used if no lcores have already met our criteria
				 */
				selected_core = i;
				min_pollers = num_pollers;
			}
			break;
		}
	}

	return selected_core;
}

