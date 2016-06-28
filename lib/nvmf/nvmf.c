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

#include <rte_config.h>
#include <rte_mempool.h>

#include "spdk/log.h"
#include "spdk/conf.h"
#include "conf.h"
#include "conn.h"
#include "controller.h"
#include "port.h"
#include "host.h"
#include "rdma.h"
#include "subsystem_grp.h"
#include "spdk/trace.h"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

#define MAX_SUBSYSTEMS 4

/*
 * Define the global pool sizes for the maximum possible
 * requests across all target connection queues.
 *
 * SPDK_NVMF_ADMINQ_POOL_SIZE: There is a single admin queue
 * for each subsystem session.
 *
 * SPDK_NVMF_IOQ_POOL_SIZE: MaxConnectionsPerSession is config
 * option that defines the total connection queues per session,
 * so we -1 here to not account for the admin queue.
 *
 * SPDK_NVMF_DESC_POOL_SIZE: The total number of RDMA descriptors
 * needed for all possible admin and I/O queue requests.
 */
#define SPDK_NVMF_ADMINQ_POOL_SIZE(spdk)	(MAX_SUBSYSTEMS * \
						 (spdk->MaxSessionsPerSubsystem) * \
						 spdk->MaxQueueDepth)

#define SPDK_NVMF_IOQ_POOL_SIZE(spdk)	(MAX_SUBSYSTEMS * \
					 (spdk->MaxSessionsPerSubsystem) * \
					 (spdk->MaxConnectionsPerSession - 1) * \
					 spdk->MaxQueueDepth)

#define SPDK_NVMF_DESC_POOL_SIZE(spdk)	(SPDK_NVMF_ADMINQ_POOL_SIZE(spdk) + \
					 SPDK_NVMF_IOQ_POOL_SIZE(spdk))

#define SPDK_NVMF_MAX_CONNECTIONS(spdk)	(MAX_SUBSYSTEMS * \
					 ((spdk)->MaxSessionsPerSubsystem) * \
					 ((spdk)->MaxConnectionsPerSession))

struct spdk_nvmf_globals g_nvmf_tgt;

extern struct rte_mempool *request_mempool;

static int
spdk_nvmf_initialize_pools(struct spdk_nvmf_globals *spdk_nvmf)
{
	SPDK_NOTICELOG("\n*** NVMf Pool Creation ***\n");

	/* create NVMe backend request pool */
	spdk_nvmf->nvme_request_pool = rte_mempool_create("NVMe_Pool",
				       SPDK_NVMF_DESC_POOL_SIZE(spdk_nvmf),
				       spdk_nvme_request_size(),
				       128, 0,
				       NULL, NULL, NULL, NULL,
				       SOCKET_ID_ANY, 0);
	if (!spdk_nvmf->nvme_request_pool) {
		SPDK_ERRLOG("create NVMe request pool failed\n");
		return -1;
	}
	/* set global pointer for this pool referenced by libraries */
	request_mempool = spdk_nvmf->nvme_request_pool;
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "NVMe request_mempool %p, size 0x%u bytes\n",
		      request_mempool,
		      (unsigned int)(SPDK_NVMF_DESC_POOL_SIZE(spdk_nvmf) * spdk_nvme_request_size()));

	return 0;
}

static int spdk_nvmf_check_pool(struct rte_mempool *pool, uint32_t count)
{
	if (rte_mempool_count(pool) != count) {
		SPDK_ERRLOG("rte_mempool_count(%s) == %d, should be %d\n",
			    pool->name, rte_mempool_count(pool), count);
		return -1;
	} else {
		return 0;
	}
}

static int
spdk_nvmf_check_pools(void)
{
	struct spdk_nvmf_globals *spdk_nvmf = &g_nvmf_tgt;
	int rc = 0;

	rc += spdk_nvmf_check_pool(spdk_nvmf->nvme_request_pool, SPDK_NVMF_DESC_POOL_SIZE(spdk_nvmf));

	if (rc == 0) {
		return 0;
	} else {
		return -1;
	}
}

int
nvmf_tgt_init(char *nodebase, int max_in_capsule_data,
	      int max_sessions_per_subsystem,
	      int max_queue_depth, int max_conn_per_sess, int max_recv_seg_len, int listen_port)
{
	int rc;

	g_nvmf_tgt.nodebase = strdup(nodebase);
	if (!g_nvmf_tgt.nodebase) {
		SPDK_ERRLOG("No NodeBase provided\n");
		return -EINVAL;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "NodeBase: %s\n", g_nvmf_tgt.nodebase);

	if (max_in_capsule_data >= 16 &&
	    max_in_capsule_data % 16 == 0 &&
	    max_in_capsule_data <= SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE) {
		g_nvmf_tgt.MaxInCapsuleData = max_in_capsule_data;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "MaxInCapsuleData: to %d\n",
			      g_nvmf_tgt.MaxInCapsuleData);
	} else {
		SPDK_ERRLOG("Invalid MaxInCapsuleData: %d\n", max_in_capsule_data);
		return -EINVAL;
	}

	if (max_sessions_per_subsystem >= 1 &&
	    max_sessions_per_subsystem <= SPDK_NVMF_DEFAULT_MAX_SESSIONS_PER_SUBSYSTEM) {
		g_nvmf_tgt.MaxSessionsPerSubsystem = max_sessions_per_subsystem;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "MaxSessionsPerSubsystem: %d\n",
			      g_nvmf_tgt.MaxSessionsPerSubsystem);
	} else {
		SPDK_ERRLOG("Invalid MaxSessionsPerSubsystem: %d\n", max_sessions_per_subsystem);
		return -EINVAL;
	}

	if (max_queue_depth >= 1 &&
	    max_queue_depth <= SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH) {
		g_nvmf_tgt.MaxQueueDepth = max_queue_depth;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "MaxQueueDepth: %d\n",
			      g_nvmf_tgt.MaxQueueDepth);
	} else {
		SPDK_ERRLOG("Invalid MaxQueueDepth: %d\n", max_queue_depth);
		return -EINVAL;
	}

	if (max_conn_per_sess >= 1 &&
	    max_conn_per_sess <= SPDK_NVMF_DEFAULT_MAX_CONNECTIONS_PER_SESSION) {
		g_nvmf_tgt.MaxConnectionsPerSession = max_conn_per_sess;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "MaxConnectionsPerSession: %d\n",
			      g_nvmf_tgt.MaxConnectionsPerSession);
	} else {
		SPDK_ERRLOG("Invalid MaxConnectionsPerSession: %d\n", max_conn_per_sess);
		return -EINVAL;
	}


	g_nvmf_tgt.MaxRecvDataSegmentLength = max_recv_seg_len;
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "MaxRecvDataSegmentLength %d\n",
		      g_nvmf_tgt.MaxRecvDataSegmentLength);

	rc = pthread_mutex_init(&g_nvmf_tgt.mutex, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("mutex_init() failed\n");
		return -1;
	}

	/* init nvmf specific config options */
	if (!g_nvmf_tgt.sin_port) {
		g_nvmf_tgt.sin_port = htons(SPDK_NVMF_DEFAULT_SIN_PORT);
	}

	rc = spdk_nvmf_initialize_pools(&g_nvmf_tgt);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_nvmf_initialize_pools() failed\n");
		return rc;
	}

	return 0;
}

static int
nvmf_tgt_subsystem_initialize(void)
{
	int rc;

	/* initialize from configuration file */
	rc = spdk_nvmf_parse_conf();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_nvmf_parse_conf() failed\n");
		return rc;
	}

	/* initialize with the NVMf transport */
	rc = nvmf_rdma_init();
	if (rc <= 0) {
		SPDK_ERRLOG("nvmf_rdma_init() failed\n");
		return rc;
	}
	/* initialize NVMe/NVMf backend */

	SPDK_NOTICELOG("\n*** NVMf Library Init ***\n");
	rc = nvmf_initialize();
	if (rc < 0) {
		SPDK_ERRLOG("nvmf_initialize() failed\n");
		return rc;
	}

	rc = spdk_nvmf_init_nvme();
	if (rc < 0) {
		fprintf(stderr, "NVMf could not initialize NVMe devices.\n");
		return -1;
	}

	rc = spdk_initialize_nvmf_subsystems();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_initialize_nvmf_subsystems failed\n");
		return rc;
	}
	rc = spdk_initialize_nvmf_conns(SPDK_NVMF_MAX_CONNECTIONS(&g_nvmf_tgt));
	if (rc < 0) {
		SPDK_ERRLOG("spdk_initialize_nvmf_conns() failed\n");
		return rc;
	}
	return rc;
}

static int
nvmf_tgt_subsystem_fini(void)
{
	spdk_shutdown_nvmf_subsystems();
	nvmf_shutdown();
	spdk_nvmf_host_destroy_all();
	spdk_nvmf_port_destroy_all();
	free(g_nvmf_tgt.nodebase);

	pthread_mutex_destroy(&g_nvmf_tgt.mutex);

	if (spdk_nvmf_check_pools() != 0) {
		return -1;
	}

	return 0;
}

int
nvmf_initialize(void)
{
	if (request_mempool == NULL) {
		fprintf(stderr, "NVMf application has not created request mempool!\n");
		return -1;
	}

	return 0;
}

void
nvmf_shutdown(void)
{
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_shutdown\n");

	spdk_nvmf_shutdown_nvme();
}

SPDK_SUBSYSTEM_REGISTER(nvmf, nvmf_tgt_subsystem_initialize, nvmf_tgt_subsystem_fini, NULL)

SPDK_TRACE_REGISTER_FN(nvmf_trace)
{
	spdk_trace_register_object(OBJECT_NVMF_IO, 'r');
	spdk_trace_register_description("NVMF_IO_START", "", TRACE_NVMF_IO_START,
					OWNER_NONE, OBJECT_NVMF_IO, 1, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_READ_START", "", TRACE_RDMA_READ_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_WRITE_START", "", TRACE_RDMA_WRITE_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_READ_COMPLETE", "", TRACE_RDMA_READ_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_RDMA_WRITE_COMPLETE", "", TRACE_RDMA_WRITE_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_READ_START", "", TRACE_NVMF_LIB_READ_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_WRITE_START", "", TRACE_NVMF_LIB_WRITE_START,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_LIB_COMPLETE", "", TRACE_NVMF_LIB_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
	spdk_trace_register_description("NVMF_IO_COMPLETION_DONE", "", TRACE_NVMF_IO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_IO, 0, 0, 0, "");
}
