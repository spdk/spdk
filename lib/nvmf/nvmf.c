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

#include "spdk/conf.h"
#include "spdk/nvmf.h"
#include "spdk/trace.h"

#include "spdk_internal/log.h"

#include "subsystem.h"
#include "transport.h"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

#define MAX_SUBSYSTEMS 4

struct spdk_nvmf_tgt g_nvmf_tgt;

int
spdk_nvmf_tgt_init(uint16_t max_queue_depth, uint16_t max_queues_per_sess,
		   uint32_t in_capsule_data_size, uint32_t max_io_size)
{
	int rc;

	g_nvmf_tgt.max_queues_per_session = max_queues_per_sess;
	g_nvmf_tgt.max_queue_depth = max_queue_depth;
	g_nvmf_tgt.in_capsule_data_size = in_capsule_data_size;
	g_nvmf_tgt.max_io_size = max_io_size;
	g_nvmf_tgt.discovery_genctr = 0;
	g_nvmf_tgt.discovery_log_page = NULL;
	g_nvmf_tgt.discovery_log_page_size = 0;
	g_nvmf_tgt.current_subsystem_id = 0;
	TAILQ_INIT(&g_nvmf_tgt.subsystems);
	TAILQ_INIT(&g_nvmf_tgt.listen_addrs);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max Queues Per Session: %d\n", max_queues_per_sess);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max Queue Depth: %d\n", max_queue_depth);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max In Capsule Data: %d bytes\n", in_capsule_data_size);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max I/O Size: %d bytes\n", max_io_size);

	rc = spdk_nvmf_transport_init();
	if (rc < 0) {
		SPDK_ERRLOG("Transport initialization failed\n");
		return -1;
	}

	return 0;
}

int
spdk_nvmf_tgt_fini(void)
{
	struct spdk_nvmf_listen_addr *listen_addr, *listen_addr_tmp;

	TAILQ_FOREACH_SAFE(listen_addr, &g_nvmf_tgt.listen_addrs, link, listen_addr_tmp) {
		TAILQ_REMOVE(&g_nvmf_tgt.listen_addrs, listen_addr, link);
		g_nvmf_tgt.discovery_genctr++;

		spdk_nvmf_listen_addr_destroy(listen_addr);
	}

	spdk_nvmf_transport_fini();

	return 0;
}

struct spdk_nvmf_listen_addr *
spdk_nvmf_listen_addr_create(const char *trname, const char *traddr, const char *trsvcid)
{
	struct spdk_nvmf_listen_addr *listen_addr;
	const struct spdk_nvmf_transport *transport;

	transport = spdk_nvmf_transport_get(trname);
	if (!transport) {
		return NULL;
	}

	listen_addr = calloc(1, sizeof(*listen_addr));
	if (!listen_addr) {
		return NULL;
	}

	listen_addr->traddr = strdup(traddr);
	if (!listen_addr->traddr) {
		free(listen_addr);
		return NULL;
	}

	listen_addr->trsvcid = strdup(trsvcid);
	if (!listen_addr->trsvcid) {
		free(listen_addr->traddr);
		free(listen_addr);
		return NULL;
	}

	listen_addr->trname = strdup(trname);
	if (!listen_addr->trname) {
		free(listen_addr->traddr);
		free(listen_addr->trsvcid);
		free(listen_addr);
		return NULL;
	}

	return listen_addr;
}

void
spdk_nvmf_listen_addr_destroy(struct spdk_nvmf_listen_addr *addr)
{
	const struct spdk_nvmf_transport *transport;

	transport = spdk_nvmf_transport_get(addr->trname);
	assert(transport != NULL);
	transport->listen_addr_remove(addr);

	spdk_nvmf_listen_addr_cleanup(addr);
}

void
spdk_nvmf_listen_addr_cleanup(struct spdk_nvmf_listen_addr *addr)
{
	free(addr->trname);
	free(addr->trsvcid);
	free(addr->traddr);
	free(addr);
}

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
