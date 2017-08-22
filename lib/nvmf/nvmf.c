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

#include "ctrlr.h"
#include "subsystem.h"
#include "transport.h"

SPDK_LOG_REGISTER_TRACE_FLAG("nvmf", SPDK_TRACE_NVMF)

#define MAX_SUBSYSTEMS 4

#define SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_DEFAULT_MAX_QPAIRS_PER_CTRLR 64
#define SPDK_NVMF_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_DEFAULT_MAX_IO_SIZE 131072

struct spdk_nvmf_tgt g_nvmf_tgt;

void
spdk_nvmf_tgt_opts_init(struct spdk_nvmf_tgt_opts *opts)
{
	opts->max_queue_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr = SPDK_NVMF_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size = SPDK_NVMF_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size = SPDK_NVMF_DEFAULT_MAX_IO_SIZE;
}

struct spdk_nvmf_tgt *
spdk_nvmf_tgt_create(struct spdk_nvmf_tgt_opts *opts)
{
	struct spdk_nvmf_tgt *tgt;

	tgt = &g_nvmf_tgt;

	if (!opts) {
		spdk_nvmf_tgt_opts_init(&tgt->opts);
	} else {
		tgt->opts = *opts;
	}

	tgt->discovery_genctr = 0;
	tgt->discovery_log_page = NULL;
	tgt->discovery_log_page_size = 0;
	tgt->current_subsystem_id = 0;
	TAILQ_INIT(&tgt->subsystems);
	TAILQ_INIT(&tgt->listen_addrs);
	TAILQ_INIT(&tgt->transports);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max Queue Pairs Per Controller: %d\n",
		      tgt->opts.max_qpairs_per_ctrlr);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max Queue Depth: %d\n", tgt->opts.max_queue_depth);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max In Capsule Data: %d bytes\n",
		      tgt->opts.in_capsule_data_size);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Max I/O Size: %d bytes\n", tgt->opts.max_io_size);

	return tgt;
}

void
spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_listen_addr *listen_addr, *listen_addr_tmp;
	struct spdk_nvmf_transport *transport, *transport_tmp;

	TAILQ_FOREACH_SAFE(listen_addr, &tgt->listen_addrs, link, listen_addr_tmp) {
		TAILQ_REMOVE(&tgt->listen_addrs, listen_addr, link);
		tgt->discovery_genctr++;

		spdk_nvmf_listen_addr_destroy(listen_addr);
	}

	TAILQ_FOREACH_SAFE(transport, &tgt->transports, link, transport_tmp) {
		TAILQ_REMOVE(&tgt->transports, transport, link);
		spdk_nvmf_transport_destroy(transport);
	}
}

struct spdk_nvmf_listen_addr *
spdk_nvmf_tgt_listen(struct spdk_nvmf_tgt *tgt,
		     struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listen_addr *listen_addr;
	struct spdk_nvmf_transport *transport;
	int rc;

	TAILQ_FOREACH(listen_addr, &tgt->listen_addrs, link) {
		if (spdk_nvme_transport_id_compare(&listen_addr->trid, trid) == 0) {
			return listen_addr;
		}
	}

	transport = spdk_nvmf_tgt_get_transport(tgt, trid->trtype);
	if (!transport) {
		transport = spdk_nvmf_transport_create(tgt, trid->trtype);
		if (!transport) {
			SPDK_ERRLOG("Transport initialization failed\n");
			return NULL;
		}
		TAILQ_INSERT_TAIL(&tgt->transports, transport, link);
	}


	listen_addr = spdk_nvmf_listen_addr_create(trid);
	if (!listen_addr) {
		return NULL;
	}

	rc = spdk_nvmf_transport_listen(transport, trid);
	if (rc < 0) {
		free(listen_addr);
		SPDK_ERRLOG("Unable to listen on address '%s'\n", trid->traddr);
		return NULL;
	}

	TAILQ_INSERT_HEAD(&tgt->listen_addrs, listen_addr, link);
	tgt->discovery_genctr++;

	return listen_addr;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;

	if (!subnqn) {
		return NULL;
	}

	TAILQ_FOREACH(subsystem, &tgt->subsystems, entries) {
		if (strcmp(subnqn, subsystem->subnqn) == 0) {
			return subsystem;
		}
	}

	return NULL;
}

uint16_t
spdk_nvmf_tgt_gen_cntlid(struct spdk_nvmf_tgt *tgt)
{
	static uint16_t cntlid = 0; /* cntlid is static, so its value is preserved */
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_ctrlr *ctrlr;
	uint16_t count;

	count = UINT16_MAX - 1;
	do {
		/* cntlid is an unsigned 16-bit integer, so let it overflow
		 * back to 0 if necessary.
		 */
		cntlid++;
		if (cntlid == 0) {
			/* 0 is not a valid cntlid because it is the reserved value in the RDMA
			 * private data for cntlid. This is the value sent by pre-NVMe-oF 1.1
			 * initiators.
			 */
			cntlid++;
		}

		/* Check if a subsystem with this cntlid currently exists. This could
		 * happen for a very long-lived ctrlr on a target with many short-lived
		 * ctrlrs, where cntlid wraps around.
		 */
		TAILQ_FOREACH(subsystem, &tgt->subsystems, entries) {
			TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
				if (ctrlr->cntlid == cntlid) {
					break;
				}
			}
		}

		count--;

	} while (subsystem != NULL && count > 0);

	if (count == 0) {
		return 0;
	}

	return cntlid;
}

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, enum spdk_nvme_transport_type type)
{
	struct spdk_nvmf_transport *transport;

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		if (transport->ops->type == type) {
			return transport;
		}
	}

	return NULL;
}

struct spdk_nvmf_listen_addr *
spdk_nvmf_listen_addr_create(struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listen_addr *listen_addr;

	listen_addr = calloc(1, sizeof(*listen_addr));
	if (!listen_addr) {
		return NULL;
	}

	listen_addr->trid = *trid;

	return listen_addr;
}

void
spdk_nvmf_listen_addr_destroy(struct spdk_nvmf_listen_addr *addr)
{
	struct spdk_nvmf_transport *transport;

	transport = spdk_nvmf_tgt_get_transport(&g_nvmf_tgt, addr->trid.trtype);
	if (!transport) {
		SPDK_ERRLOG("Attempted to destroy listener without a valid transport\n");
		return;
	}

	spdk_nvmf_transport_stop_listen(transport, &addr->trid);
	free(addr);
}

void
spdk_nvmf_tgt_accept(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_transport *transport, *tmp;

	TAILQ_FOREACH_SAFE(transport, &tgt->transports, link, tmp) {
		spdk_nvmf_transport_accept(transport);
	}
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
