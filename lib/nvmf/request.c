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

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/log.h"

static void
spdk_nvmf_qpair_request_cleanup(struct spdk_nvmf_qpair *qpair)
{
	if (qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING) {
		assert(qpair->state_cb != NULL);

		if (TAILQ_EMPTY(&qpair->outstanding)) {
			qpair->state_cb(qpair->state_cb_arg, 0);
		}
	} else {
		assert(qpair->state == SPDK_NVMF_QPAIR_ACTIVE);
	}
}

int
spdk_nvmf_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;

	TAILQ_REMOVE(&qpair->outstanding, req, link);
	if (spdk_nvmf_transport_req_free(req)) {
		SPDK_ERRLOG("Unable to free transport level request resources.\n");
	}

	spdk_nvmf_qpair_request_cleanup(qpair);

	return 0;
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_qpair *qpair;

	rsp->sqid = 0;
	rsp->status.p = 0;
	rsp->cid = req->cmd->nvme_cmd.cid;

	qpair = req->qpair;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF,
		      "cpl: cid=%u cdw0=0x%08x rsvd1=%u status=0x%04x\n",
		      rsp->cid, rsp->cdw0, rsp->rsvd1,
		      *(uint16_t *)&rsp->status);

	TAILQ_REMOVE(&qpair->outstanding, req, link);
	if (spdk_nvmf_transport_req_complete(req)) {
		SPDK_ERRLOG("Transport request completion error!\n");
	}

	spdk_nvmf_qpair_request_cleanup(qpair);

	return 0;
}

static void
nvmf_trace_command(union nvmf_h2c_msg *h2c_msg, bool is_admin_queue)
{
	struct spdk_nvmf_capsule_cmd *cap_hdr = &h2c_msg->nvmf_cmd;
	struct spdk_nvme_cmd *cmd = &h2c_msg->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
	uint8_t opc;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		opc = cap_hdr->fctype;
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "%s Fabrics cmd: fctype 0x%02x cid %u\n",
			      is_admin_queue ? "Admin" : "I/O",
			      cap_hdr->fctype, cap_hdr->cid);
	} else {
		opc = cmd->opc;
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "%s cmd: opc 0x%02x fuse %u cid %u nsid %u cdw10 0x%08x\n",
			      is_admin_queue ? "Admin" : "I/O",
			      cmd->opc, cmd->fuse, cmd->cid, cmd->nsid, cmd->cdw10);
		if (cmd->mptr) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "mptr 0x%" PRIx64 "\n", cmd->mptr);
		}
		if (cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_CONTIG &&
		    cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_SGL) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "psdt %u\n", cmd->psdt);
		}
	}

	if (spdk_nvme_opc_get_data_transfer(opc) != SPDK_NVME_DATA_NONE) {
		if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF,
				      "SGL: Keyed%s: addr 0x%" PRIx64 " key 0x%x len 0x%x\n",
				      sgl->generic.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY ? " (Inv)" : "",
				      sgl->address, sgl->keyed.key, sgl->keyed.length);
		} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "SGL: Data block: %s 0x%" PRIx64 " len 0x%x\n",
				      sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET ? "offs" : "addr",
				      sgl->address, sgl->unkeyed.length);
		} else {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "SGL type 0x%x subtype 0x%x\n",
				      sgl->generic.type, sgl->generic.subtype);
		}
	}
}

void
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	spdk_nvmf_request_exec_status status;

	nvmf_trace_command(req->cmd, spdk_nvmf_qpair_is_admin_queue(qpair));

	if (qpair->state != SPDK_NVMF_QPAIR_ACTIVE) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		/* Place the request on the outstanding list so we can keep track of it */
		TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
		spdk_nvmf_request_complete(req);
		return;
	}

	/* Check if the subsystem is paused (if there is a subsystem) */
	if (qpair->ctrlr) {
		struct spdk_nvmf_subsystem_poll_group *sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
		if (sgroup->state != SPDK_NVMF_SUBSYSTEM_ACTIVE) {
			/* The subsystem is not currently active. Queue this request. */
			TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
			return;
		}

	}

	/* Place the request on the outstanding list so we can keep track of it */
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC)) {
		status = spdk_nvmf_ctrlr_process_fabrics_cmd(req);
	} else if (spdk_unlikely(spdk_nvmf_qpair_is_admin_queue(qpair))) {
		status = spdk_nvmf_ctrlr_process_admin_cmd(req);
	} else {
		status = spdk_nvmf_ctrlr_process_io_cmd(req);
	}

	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}
}

int
spdk_nvmf_request_abort(struct spdk_nvmf_request *req)
{
	/* TODO: implement abort, at least for commands that are still queued in software */
	return -1;
}
