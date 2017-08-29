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

#include "spdk/io_channel.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/log.h"

static void
spdk_nvmf_request_complete_on_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->sqid = 0;
	rsp->status.p = 0;
	rsp->cid = req->cmd->nvme_cmd.cid;

	SPDK_DEBUGLOG(SPDK_TRACE_NVMF,
		      "cpl: cid=%u cdw0=0x%08x rsvd1=%u status=0x%04x\n",
		      rsp->cid, rsp->cdw0, rsp->rsvd1,
		      *(uint16_t *)&rsp->status);

	if (spdk_nvmf_transport_req_complete(req)) {
		SPDK_ERRLOG("Transport request completion error!\n");
	}
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC ||
	    req->qpair->type == QPAIR_TYPE_AQ) {
		/* Pass a message back to the originating thread. */
		spdk_thread_send_msg(req->qpair->thread,
				     spdk_nvmf_request_complete_on_qpair,
				     req);
	} else {
		spdk_nvmf_request_complete_on_qpair(req);
	}

	return 0;
}

static spdk_nvmf_request_exec_status
nvmf_process_property_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd;

	cmd = &req->cmd->prop_get_cmd;
	response = &req->rsp->prop_get_rsp;

	spdk_nvmf_property_get(req->qpair->ctrlr, cmd, response);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static spdk_nvmf_request_exec_status
nvmf_process_property_set(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_set_cmd *cmd;

	cmd = &req->cmd->prop_set_cmd;

	spdk_nvmf_property_set(req->qpair->ctrlr, cmd, &req->rsp->nvme_cpl);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static void
invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp, uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
	rsp->status_code_specific.invalid.iattr = iattr;
	rsp->status_code_specific.invalid.ipo = ipo;
}

static spdk_nvmf_request_exec_status
nvmf_process_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tgt		*tgt;
	struct spdk_nvmf_subsystem	*subsystem;
	struct spdk_nvmf_fabric_connect_data *data = (struct spdk_nvmf_fabric_connect_data *)
			req->data;
	struct spdk_nvmf_fabric_connect_cmd *cmd = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	void *end;

#define INVALID_CONNECT_DATA(field) invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))

	if (cmd->recfmt != 0) {
		SPDK_ERRLOG("Connect command unsupported RECFMT %u\n", cmd->recfmt);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* Ensure that subnqn and hostnqn are null terminated */
	end = memchr(data->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	if (!end) {
		SPDK_ERRLOG("Connect SUBNQN is not null terminated\n");
		INVALID_CONNECT_DATA(subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	end = memchr(data->hostnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	if (!end) {
		SPDK_ERRLOG("Connect HOSTNQN is not null terminated\n");
		INVALID_CONNECT_DATA(hostnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	tgt = req->qpair->transport->tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Could not find subsystem '%s'\n", data->subnqn);
		INVALID_CONNECT_DATA(subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, data->hostnqn)) {
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s'\n", data->subnqn, data->hostnqn);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_HOST;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_nvmf_ctrlr_connect(req->qpair, cmd, req->data, rsp);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static spdk_nvmf_request_exec_status
nvmf_process_fabrics_command(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = &req->cmd->nvmf_cmd;

	if (qpair->ctrlr == NULL) {
		/* No ctrlr established yet; the only valid command is Connect */
		if (cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			return nvmf_process_connect(req);
		} else {
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "Got fctype 0x%x, expected Connect\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else if (qpair->type == QPAIR_TYPE_AQ) {
		/*
		 * Controller session is established, and this is an admin queue.
		 * Disallow Connect and allow other fabrics commands.
		 */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
			return nvmf_process_property_set(req);
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
			return nvmf_process_property_get(req);
		default:
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "recv capsule header type invalid [%x]!\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		/* Controller session is established, and this is an I/O queue */
		/* For now, no I/O-specific Fabrics commands are implemented (other than Connect) */
		SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "Unexpected I/O fctype 0x%x\n", cap_hdr->fctype);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static void
nvmf_trace_command(union nvmf_h2c_msg *h2c_msg, enum spdk_nvmf_qpair_type qpair_type)
{
	struct spdk_nvmf_capsule_cmd *cap_hdr = &h2c_msg->nvmf_cmd;
	struct spdk_nvme_cmd *cmd = &h2c_msg->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
	uint8_t opc;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		opc = cap_hdr->fctype;
		SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "%s Fabrics cmd: fctype 0x%02x cid %u\n",
			      qpair_type == QPAIR_TYPE_AQ ? "Admin" : "I/O",
			      cap_hdr->fctype, cap_hdr->cid);
	} else {
		opc = cmd->opc;
		SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "%s cmd: opc 0x%02x fuse %u cid %u nsid %u cdw10 0x%08x\n",
			      qpair_type == QPAIR_TYPE_AQ ? "Admin" : "I/O",
			      cmd->opc, cmd->fuse, cmd->cid, cmd->nsid, cmd->cdw10);
		if (cmd->mptr) {
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "mptr 0x%" PRIx64 "\n", cmd->mptr);
		}
		if (cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_CONTIG &&
		    cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_SGL) {
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "psdt %u\n", cmd->psdt);
		}
	}

	if (spdk_nvme_opc_get_data_transfer(opc) != SPDK_NVME_DATA_NONE) {
		if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF,
				      "SGL: Keyed%s: addr 0x%" PRIx64 " key 0x%x len 0x%x\n",
				      sgl->generic.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY ? " (Inv)" : "",
				      sgl->address, sgl->keyed.key, sgl->keyed.length);
		} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "SGL: Data block: %s 0x%" PRIx64 " len 0x%x\n",
				      sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET ? "offs" : "addr",
				      sgl->address, sgl->unkeyed.length);
		} else {
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "SGL type 0x%x subtype 0x%x\n",
				      sgl->generic.type, sgl->generic.subtype);
		}
	}
}

static void
spdk_nvmf_request_exec_on_master(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	spdk_nvmf_request_exec_status status;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		status = nvmf_process_fabrics_command(req);
	} else if (ctrlr == NULL || !ctrlr->vcprop.cc.bits.en) {
		/* Only Fabric commands are allowed when the controller is disabled */
		SPDK_ERRLOG("Non-Fabric command sent to disabled controller\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	} else {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = ctrlr->subsys;
		assert(subsystem != NULL);

		if (subsystem->is_removed) {
			rsp->status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			status = spdk_nvmf_ctrlr_process_admin_cmd(req);
		}
	}

	switch (status) {
	case SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE:
		spdk_nvmf_request_complete(req);
		break;
	case SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS:
		break;
	default:
		SPDK_UNREACHABLE();
	}
}

int
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	spdk_nvmf_request_exec_status status;

	nvmf_trace_command(req->cmd, req->qpair->type);

	if (cmd->opc == SPDK_NVME_OPC_FABRIC ||
	    req->qpair->type == QPAIR_TYPE_AQ) {
		/* Fabric and admin commands are sent
		 * to the master core for synchronization
		 * reasons.
		 */
		spdk_thread_send_msg(req->qpair->transport->tgt->master_thread,
				     spdk_nvmf_request_exec_on_master,
				     req);
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else if (ctrlr == NULL ||
		   !ctrlr->vcprop.cc.bits.en) {
		/* TODO: The EN bit is modified by the master thread. This needs
		 * stronger synchronization.
		 */
		SPDK_ERRLOG("Non-Fabric command sent to disabled controller\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	} else {
		struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;

		assert(subsystem != NULL);

		/* TODO: subsystem->is_removed is touched by multiple threads.
		 * This needs stronger synchronization.
		 */
		if (subsystem->is_removed) {
			rsp->status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			status = spdk_nvmf_ctrlr_process_io_cmd(req);
		}
	}

	switch (status) {
	case SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE:
		return spdk_nvmf_request_complete(req);
	case SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS:
		return 0;
	default:
		SPDK_UNREACHABLE();
	}

	return 0;
}

int
spdk_nvmf_request_abort(struct spdk_nvmf_request *req)
{
	/* TODO: implement abort, at least for commands that are still queued in software */
	return -1;
}
