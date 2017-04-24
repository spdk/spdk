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

#include "subsystem.h"
#include "session.h"
#include "request.h"

#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/event.h"

#include "spdk_internal/log.h"

static void
nvmf_direct_ctrlr_get_data(struct spdk_nvmf_session *session)
{
	const struct spdk_nvme_ctrlr_data	*cdata;

	cdata = spdk_nvme_ctrlr_get_data(session->subsys->dev.direct.ctrlr);
	memcpy(&session->vcdata, cdata, sizeof(struct spdk_nvme_ctrlr_data));
}

static void
nvmf_direct_ctrlr_poll_for_admin_completions(void *arg)
{
	struct spdk_nvmf_subsystem *subsystem = arg;

	spdk_nvme_ctrlr_process_admin_completions(subsystem->dev.direct.ctrlr);
}

static void
nvmf_direct_ctrlr_poll_for_completions(struct spdk_nvmf_subsystem *subsystem)
{
	if (subsystem->dev.direct.outstanding_admin_cmd_count > 0) {
		nvmf_direct_ctrlr_poll_for_admin_completions(subsystem);
	}

	if (subsystem->dev.direct.admin_poller == NULL) {
		int lcore = spdk_env_get_current_core();

		spdk_poller_register(&subsystem->dev.direct.admin_poller,
				     nvmf_direct_ctrlr_poll_for_admin_completions,
				     subsystem, lcore, 10000);
	}

	spdk_nvme_qpair_process_completions(subsystem->dev.direct.io_qpair, 0);
}

static void
nvmf_direct_ctrlr_complete_cmd(void *ctx, const struct spdk_nvme_cpl *cmp)
{
	struct spdk_nvmf_request *req = ctx;

	spdk_trace_record(TRACE_NVMF_LIB_COMPLETE, 0, 0, (uint64_t)req, 0);

	req->rsp->nvme_cpl = *cmp;

	spdk_nvmf_request_complete(req);
}

static void
nvmf_direct_ctrlr_complete_admin_cmd(void *ctx, const struct spdk_nvme_cpl *cmp)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;

	subsystem->dev.direct.outstanding_admin_cmd_count--;

	nvmf_direct_ctrlr_complete_cmd(ctx, cmp);
}

static int
nvmf_direct_ctrlr_admin_identify_nslist(struct spdk_nvme_ctrlr *ctrlr,
					struct spdk_nvmf_request *req)
{
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint32_t req_ns_id = cmd->nsid;
	uint32_t i, num_ns, count = 0;
	struct spdk_nvme_ns_list *ns_list;

	if (req_ns_id >= 0xfffffffeUL) {
		return -1;
	}
	memset(req->data, 0, req->length);

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	ns_list = (struct spdk_nvme_ns_list *)req->data;
	for (i = 1; i <= num_ns; i++) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, i);
		if (!spdk_nvme_ns_is_active(ns)) {
			continue;
		}
		if (i <= req_ns_id) {
			continue;
		}

		ns_list->ns_list[count++] = i;
		if (count == SPDK_COUNTOF(ns_list->ns_list)) {
			break;
		}
	}
	return 0;
}

static int
nvmf_direct_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	union spdk_nvme_vs_register vs;
	int rc = 0;
	uint8_t feature;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		if (req->data == NULL || req->length < 4096) {
			SPDK_ERRLOG("identify command with invalid buffer\n");
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		if ((cmd->cdw10 & 0xFF) == SPDK_NVME_IDENTIFY_CTRLR) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Controller\n");
			/* pull from virtual controller context */
			memcpy(req->data, &session->vcdata, sizeof(struct spdk_nvme_ctrlr_data));
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else if ((cmd->cdw10 & 0xFF) == SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST) {
			vs = spdk_nvme_ctrlr_get_regs_vs(subsystem->dev.direct.ctrlr);
			if (vs.raw < SPDK_NVME_VERSION(1, 1, 0)) {
				/* fill in identify ns list with virtual controller information */
				rc = nvmf_direct_ctrlr_admin_identify_nslist(subsystem->dev.direct.ctrlr, req);
				if (rc < 0) {
					SPDK_ERRLOG("Invalid Namespace or Format\n");
					response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
				}
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
		}

		goto passthrough;

	case SPDK_NVME_OPC_GET_FEATURES:
		feature = cmd->cdw10 & 0xff; /* mask out the FID value */
		switch (feature) {
		case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
			return spdk_nvmf_session_get_features_number_of_queues(req);
		case SPDK_NVME_FEAT_HOST_IDENTIFIER:
			return spdk_nvmf_session_get_features_host_identifier(req);
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return spdk_nvmf_session_get_features_keep_alive_timer(req);
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return spdk_nvmf_session_get_features_async_event_configuration(req);
		default:
			goto passthrough;
		}
		break;
	case SPDK_NVME_OPC_SET_FEATURES:
		feature = cmd->cdw10 & 0xff; /* mask out the FID value */
		switch (feature) {
		case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
			return spdk_nvmf_session_set_features_number_of_queues(req);
		case SPDK_NVME_FEAT_HOST_IDENTIFIER:
			return spdk_nvmf_session_set_features_host_identifier(req);
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return spdk_nvmf_session_set_features_keep_alive_timer(req);
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return spdk_nvmf_session_set_features_async_event_configuration(req);
		default:
			goto passthrough;
		}
		break;
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return spdk_nvmf_session_async_event_request(req);

	case SPDK_NVME_OPC_KEEP_ALIVE:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Keep Alive\n");
		/*
		 * To handle keep alive just clear or reset the
		 * session based keep alive duration counter.
		 * When added, a separate timer based process
		 * will monitor if the time since last recorded
		 * keep alive has exceeded the max duration and
		 * take appropriate action.
		 */
		//session->keep_alive_timestamp = ;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		SPDK_ERRLOG("Admin opc 0x%02X not allowed in NVMf\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;

	default:
passthrough:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "admin_cmd passthrough: opc 0x%02x\n", cmd->opc);
		rc = spdk_nvme_ctrlr_cmd_admin_raw(subsystem->dev.direct.ctrlr,
						   cmd,
						   req->data, req->length,
						   nvmf_direct_ctrlr_complete_admin_cmd,
						   req);
		if (rc) {
			SPDK_ERRLOG("Error submitting admin opc 0x%02x\n", cmd->opc);
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		subsystem->dev.direct.outstanding_admin_cmd_count++;

		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}

}

static int
nvmf_direct_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;
	int rc;

	rc = spdk_nvme_ctrlr_cmd_io_raw(subsystem->dev.direct.ctrlr,
					subsystem->dev.direct.io_qpair,
					&req->cmd->nvme_cmd,
					req->data, req->length,
					nvmf_direct_ctrlr_complete_cmd,
					req);

	if (rc) {
		SPDK_ERRLOG("Failed to submit request %p\n", req);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
nvmf_direct_ctrlr_detach(struct spdk_nvmf_subsystem *subsystem)
{
	if (subsystem->dev.direct.ctrlr) {
		if (subsystem->dev.direct.admin_poller != NULL) {
			spdk_poller_unregister(&subsystem->dev.direct.admin_poller, NULL);
		}

		spdk_nvme_detach(subsystem->dev.direct.ctrlr);
	}
}

static void
nvmf_direct_ctrlr_complete_aer(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvmf_subsystem *subsystem = (struct spdk_nvmf_subsystem *) arg;
	struct spdk_nvmf_session *session;

	TAILQ_FOREACH(session, &subsystem->sessions, link) {
		if (session->aer_req) {
			nvmf_direct_ctrlr_complete_cmd(session->aer_req, cpl);
			session->aer_req = NULL;
		}
	}
}

static int
nvmf_direct_ctrlr_attach(struct spdk_nvmf_subsystem *subsystem)
{
	subsystem->dev.direct.io_qpair = spdk_nvme_ctrlr_alloc_io_qpair(subsystem->dev.direct.ctrlr, 0);
	if (subsystem->dev.direct.io_qpair == NULL) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -1;
	}

	spdk_nvme_ctrlr_register_aer_callback(subsystem->dev.direct.ctrlr,
					      nvmf_direct_ctrlr_complete_aer, subsystem);

	return 0;
}

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_direct_ctrlr_ops = {
	.attach				= nvmf_direct_ctrlr_attach,
	.ctrlr_get_data			= nvmf_direct_ctrlr_get_data,
	.process_admin_cmd		= nvmf_direct_ctrlr_process_admin_cmd,
	.process_io_cmd			= nvmf_direct_ctrlr_process_io_cmd,
	.poll_for_completions		= nvmf_direct_ctrlr_poll_for_completions,
	.detach				= nvmf_direct_ctrlr_detach,
};
