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

#include "nvmf.h"
#include "nvmf_internal.h"
#include "session.h"
#include "subsystem_grp.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"
#include "spdk/pci.h"
#include "spdk/trace.h"

int
nvmf_process_io_cmd(struct nvmf_request *req)
{
	struct nvmf_session *session = req->session;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response;
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	struct spdk_nvmf_namespace *nvmf_ns;
	struct spdk_nvme_ctrlr *ctrlr = NULL;
	struct spdk_nvme_ns *ns = NULL;
	struct spdk_nvme_qpair *qpair;
	uint32_t nsid = 0;
	struct nvme_read_cdw12 *cdw12;
	uint64_t lba_address;
	uint32_t lba_count;
	uint32_t io_flags;
	int rc = 0;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_io_cmd: req %p\n", req);

	/* pre-set response details for this command */
	response = &req->rsp->nvme_cpl;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	response->cid = cmd->cid;

	/* verify subsystem */
	if (subsystem == NULL) {
		SPDK_ERRLOG("nvmf_process_io_cmd: Subsystem Not Initialized!\n");
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return -1;
	}

	/* verify that the contoller is ready to process commands */
	if (session->vcprop.csts.bits.rdy == 0) {
		SPDK_ERRLOG("nvmf_process_io_cmd: Subsystem Controller Not Ready!\n");
		response->status.sc = SPDK_NVME_SC_NAMESPACE_NOT_READY;
		return -1;
	}

	/* verify namespace id */
	if (cmd->nsid == 0 || cmd->nsid > MAX_PER_SUBSYSTEM_NAMESPACES) {
		SPDK_ERRLOG("nvmf_process_io_cmd: Invalid NS_ID %x\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return -1;
	}

	nvmf_ns = &subsystem->ns_list_map[cmd->nsid - 1];
	ctrlr = nvmf_ns->ctrlr;
	nsid = nvmf_ns->nvme_ns_id;
	ns = nvmf_ns->ns;
	qpair = nvmf_ns->qpair;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
		cdw12 = (struct nvme_read_cdw12 *)&cmd->cdw12;
		/* NVMe library read/write interface expects non-0based lba_count value */
		lba_count = cdw12->nlb + 1;
		lba_address = cmd->cdw11;
		lba_address = (lba_address << 32) + cmd->cdw10;
		io_flags = cmd->cdw12 & 0xFFFF0000U;

		if (cmd->opc == SPDK_NVME_OPC_READ) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_io_cmd: Read; lba address %lx, lba count %x\n",
				      lba_address, lba_count);
			spdk_trace_record(TRACE_NVMF_LIB_READ_START, 0, 0,
					  (uint64_t)req->fabric_rx_ctx, 0);
			rc = spdk_nvme_ns_cmd_read(ns, qpair,
						   req->data, lba_address, lba_count,
						   nvmf_complete_cmd,
						   req, io_flags);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_io_cmd: Write; lba address %lx, lba count %x\n",
				      lba_address, lba_count);
			spdk_trace_record(TRACE_NVMF_LIB_WRITE_START, 0, 0,
					  (uint64_t)req->fabric_rx_ctx, 0);
			rc = spdk_nvme_ns_cmd_write(ns, qpair,
						    req->data, lba_address, lba_count,
						    nvmf_complete_cmd,
						    req, io_flags);
		}
		break;
	default:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "RAW Passthrough: I/O Opcode %x\n", cmd->opc);
		cmd->nsid = nsid;
		rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair,
						cmd,
						req->data, req->length,
						nvmf_complete_cmd,
						req);
		break;
	}

	if (rc) {
		SPDK_ERRLOG("nvmf_process_io_cmd: Failed to submit Opcode %x\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}
	return rc;
}

void
nvmf_check_io_completions(struct nvmf_session *session)
{
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	struct spdk_nvme_qpair *qpair, *prev_qpair = NULL;
	int i;

	for (i = 0; i < MAX_PER_SUBSYSTEM_NAMESPACES; i++) {
		qpair = subsystem->ns_list_map[i].qpair;
		if (qpair == NULL)
			continue;
		if (qpair != NULL && qpair != prev_qpair) {
			spdk_nvme_qpair_process_completions(qpair, 0);
			prev_qpair = qpair;
		}
	}
}


