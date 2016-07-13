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

static int nvmf_allocate_reactor(uint64_t cpumask);
static void spdk_nvmf_conn_do_work(void *arg);

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
		return -1;
	}

	conn->state = CONN_STATE_RUNNING;
	SPDK_NOTICELOG("Launching nvmf connection on core: %d\n", lcore);
	conn->poller.fn = spdk_nvmf_conn_do_work;
	conn->poller.arg = conn;

	spdk_poller_register(&conn->poller, lcore, NULL);

	return 0;
}

void
spdk_nvmf_conn_destruct(struct spdk_nvmf_conn *conn)
{
	spdk_poller_unregister(&conn->poller, NULL);

	nvmf_disconnect(conn->sess, conn);
	nvmf_rdma_conn_cleanup(conn);
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
