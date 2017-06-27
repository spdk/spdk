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

/*
 * NVMe over Fabrics discovery service
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "session.h"
#include "subsystem.h"
#include "request.h"
#include "transport.h"

#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

static void
nvmf_update_discovery_log(void)
{
	uint64_t numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_allowed_listener *allowed_listener;
	struct spdk_nvmf_listen_addr *listen_addr;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	const struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_discovery_log_page *disc_log;
	size_t cur_size;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Generating log page for genctr %" PRIu64 "\n",
		      g_nvmf_tgt.discovery_genctr);

	cur_size = sizeof(struct spdk_nvmf_discovery_log_page);
	disc_log = calloc(1, cur_size);
	if (disc_log == NULL) {
		SPDK_ERRLOG("Discovery log page memory allocation error\n");
		return;
	}

	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
			continue;
		}

		TAILQ_FOREACH(allowed_listener, &subsystem->allowed_listeners, link) {
			size_t new_size = cur_size + sizeof(*entry);
			void *new_log_page = realloc(disc_log, new_size);

			if (new_log_page == NULL) {
				SPDK_ERRLOG("Discovery log page memory allocation error\n");
				break;
			}

			listen_addr = allowed_listener->listen_addr;

			disc_log = new_log_page;
			cur_size = new_size;

			entry = &disc_log->entries[numrec];
			memset(entry, 0, sizeof(*entry));
			entry->portid = numrec;
			entry->cntlid = 0xffff;
			entry->asqsz = g_nvmf_tgt.max_queue_depth;
			entry->subtype = subsystem->subtype;
			snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn);

			transport = spdk_nvmf_transport_get(listen_addr->trname);
			assert(transport != NULL);

			transport->listen_addr_discover(listen_addr, entry);

			numrec++;
		}
	}

	disc_log->numrec = numrec;
	disc_log->genctr = g_nvmf_tgt.discovery_genctr;

	free(g_nvmf_tgt.discovery_log_page);

	g_nvmf_tgt.discovery_log_page = disc_log;
	g_nvmf_tgt.discovery_log_page_size = cur_size;
}

void
spdk_nvmf_get_discovery_log_page(void *buffer, uint64_t offset, uint32_t length)
{
	size_t copy_len = 0;
	size_t zero_len = length;

	if (g_nvmf_tgt.discovery_log_page == NULL ||
	    g_nvmf_tgt.discovery_log_page->genctr != g_nvmf_tgt.discovery_genctr) {
		nvmf_update_discovery_log();
	}

	/* Copy the valid part of the discovery log page, if any */
	if (g_nvmf_tgt.discovery_log_page && offset < g_nvmf_tgt.discovery_log_page_size) {
		copy_len = spdk_min(g_nvmf_tgt.discovery_log_page_size - offset, length);
		zero_len -= copy_len;
		memcpy(buffer, (char *)g_nvmf_tgt.discovery_log_page + offset, copy_len);
	}

	/* Zero out the rest of the buffer */
	if (zero_len) {
		memset((char *)buffer + copy_len, 0, zero_len);
	}

	/* We should have copied or zeroed every byte of the output buffer. */
	assert(copy_len + zero_len == length);
}

static inline uint32_t
nvmf_get_log_page_len(struct spdk_nvme_cmd *cmd)
{
	uint32_t numdl = (cmd->cdw10 >> 16) & 0xFFFFu;
	uint32_t numdu = (cmd->cdw11) & 0xFFFFu;
	return ((numdu << 16) + numdl + 1) * sizeof(uint32_t);
}

static int
nvmf_discovery_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t log_page_offset;
	uint32_t len;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;

	if (req->data == NULL) {
		SPDK_ERRLOG("discovery command with no buffer\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		/* Only identify controller can be supported */
		if ((cmd->cdw10 & 0xFF) == SPDK_NVME_IDENTIFY_CTRLR) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Controller\n");
			memcpy(req->data, (char *)&session->vcdata, sizeof(struct spdk_nvme_ctrlr_data));
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			SPDK_ERRLOG("Unsupported identify command\n");
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		break;
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		log_page_offset = (uint64_t)cmd->cdw12 | ((uint64_t)cmd->cdw13 << 32);
		if (log_page_offset & 3) {
			SPDK_ERRLOG("Invalid log page offset 0x%" PRIx64 "\n", log_page_offset);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		len = nvmf_get_log_page_len(cmd);
		if (len > req->length) {
			SPDK_ERRLOG("Get log page: len (%u) > buf size (%u)\n",
				    len, req->length);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		if ((cmd->cdw10 & 0xFF) == SPDK_NVME_LOG_DISCOVERY) {
			spdk_nvmf_get_discovery_log_page(req->data, log_page_offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			SPDK_ERRLOG("Unsupported log page %u\n", cmd->cdw10 & 0xFF);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		break;
	default:
		SPDK_ERRLOG("Unsupported Opcode 0x%x for Discovery service\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_discovery_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	/* Discovery controllers do not support I/O queues, so this code should be unreachable. */
	abort();
}

static void
nvmf_discovery_ctrlr_get_data(struct spdk_nvmf_session *session)
{
}

static void
nvmf_discovery_ctrlr_detach(struct spdk_nvmf_subsystem *subsystem)
{
}

static int
nvmf_discovery_ctrlr_attach(struct spdk_nvmf_subsystem *subsystem)
{
	return 0;
}

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_discovery_ctrlr_ops = {
	.attach				= nvmf_discovery_ctrlr_attach,
	.ctrlr_get_data			= nvmf_discovery_ctrlr_get_data,
	.process_admin_cmd		= nvmf_discovery_ctrlr_process_admin_cmd,
	.process_io_cmd			= nvmf_discovery_ctrlr_process_io_cmd,
	.detach				= nvmf_discovery_ctrlr_detach,
};
