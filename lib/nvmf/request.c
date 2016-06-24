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

#include "conn.h"
#include "rdma.h"
#include "request.h"
#include "session.h"
#include "subsystem_grp.h"

#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"

int
spdk_nvmf_request_complete(struct nvmf_request *req)
{
	struct nvme_qp_tx_desc *tx_desc = req->tx_desc;
	struct nvme_qp_rx_desc *rx_desc = req->rx_desc;
	struct spdk_nvme_cpl *response;
	int ret;

	response = &req->rsp->nvme_cpl;

	/* Was the command successful */
	if (response->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* data to be copied to host via memory RDMA */

		/* temporarily adjust SGE to only copy what the host is prepared to receive. */
		rx_desc->bb_sgl.length = req->length;

		ret = nvmf_post_rdma_write(tx_desc->conn, tx_desc);
		if (ret) {
			SPDK_ERRLOG("Unable to post rdma write tx descriptor\n");
			goto command_fail;
		}
	}

	/* Now send back the response */
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "send nvme cmd capsule response\n");

	response->sqid = 0;
	response->status.p = 0;
	response->sqhd = tx_desc->conn->sq_head;
	response->cid = req->cid;

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "cpl: cdw0=0x%x rsvd1=0x%x sqhd=0x%x sqid=0x%x cid=0x%x status=0x%x\n",
		      response->cdw0, response->rsvd1, response->sqhd, response->sqid, response->cid,
		      *(uint16_t *)&response->status);

	ret = nvmf_post_rdma_send(tx_desc->conn, req->tx_desc);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		goto command_fail;
	}

	return ret;

command_fail:
	nvmf_deactive_tx_desc(tx_desc);
	return ret;
}

int
nvmf_process_admin_cmd(struct nvmf_request *req)
{
	struct nvmf_session *session = req->session;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	struct spdk_nvme_ctrlr *ctrlr = NULL;
	uint32_t nsid = 0;
	int rc = 0;
	uint8_t feature;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_admin_cmd: req %p\n",
		      req);

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	response->cid = cmd->cid;

	/* verify subsystem */
	if (subsystem == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_admin_cmd: Subsystem Not Initialized!\n");
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return -1;
	}

	if (cmd->nsid == 0) {
		/* may be valid for the requested command. but need
		   to at least map to a known valid controller.
		   Note:  Issue when in multi-controller subsystem
		   mode, commands that do not provide ns_id can not
		   be mapped to valid HW ctrlr!  This is where
		   definition of a virtual controller is required */
		ctrlr = subsystem->ns_list_map[0].ctrlr;
		nsid = 0;
	} else {
		/* verify namespace id */
		if (cmd->nsid > MAX_PER_SUBSYSTEM_NAMESPACES) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_admin_cmd: Invalid NS_ID %x\n",
				      cmd->nsid);
			response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
			return -1;
		}

		ctrlr = subsystem->ns_list_map[cmd->nsid - 1].ctrlr;
		nsid = subsystem->ns_list_map[cmd->nsid - 1].nvme_ns_id;
	}
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_admin_cmd: ctrlr %p nvme ns_id %d\n", ctrlr, nsid);

	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		if (req->data == NULL) {
			SPDK_ERRLOG("identify command with no buffer\n");
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			rc = -1;
			break;
		}
		if (cmd->cdw10 == 0) {
			/* identify namespace */
			struct spdk_nvme_ns *ns;
			const struct spdk_nvme_ns_data *nsdata;

			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Namespace\n");
			if (nsid == 0) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_admin_cmd: Invalid NS_ID = 0\n");
				response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
				rc = -1;
				break;
			}
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (ns == NULL) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, "Unsuccessful query for Namespace reference\n");
				response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
				rc = -1;
				break;
			}
			nsdata = spdk_nvme_ns_get_data(ns);
			memcpy(req->data, (char *)nsdata, sizeof(struct spdk_nvme_ns_data));
			spdk_nvmf_request_complete(req);
		} else if (cmd->cdw10 == 1) {
			/* identify controller */
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Controller\n");
			/* pull from virtual controller context */
			memcpy(req->data, (char *)&session->vcdata, sizeof(struct spdk_nvme_ctrlr_data));
			spdk_nvmf_request_complete(req);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Namespace List\n");
			response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			rc = -1;
		}
		break;
	case SPDK_NVME_OPC_DELETE_IO_SQ: {
		uint16_t qid = cmd->cdw10 & 0xffff;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Delete IO SQ, QID %x\n", qid);

		if (qid >= MAX_SESSION_IO_QUEUES) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, " Exceeded Session QP Index Limit\n");
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			rc = -1;
		} else if (session->qps[qid].sq_active == 0) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, " Session SQ QP Index %x was not active!\n", qid);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			rc = -1;
		} else {
			session->qps[qid].sq_size = 0;
			session->qps[qid].sq_active = 0;
			if (session->qps[qid].cq_active)
				session->active_queues--;
			rc = 1;
		}
	}
	break;
	case SPDK_NVME_OPC_DELETE_IO_CQ: {
		uint16_t qid = cmd->cdw10 & 0xffff;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Delete IO CQ, QID %x\n", qid);

		if (qid >= MAX_SESSION_IO_QUEUES) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, " Exceeded Session QP Index Limit\n");
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			rc = -1;
		} else if (session->qps[qid].cq_active == 0) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, " Session CQ QP Index %x was not active!\n", qid);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			rc = -1;
		} else {
			session->qps[qid].cq_size = 0;
			session->qps[qid].cq_active = 0;
			if (session->qps[qid].sq_active)
				session->active_queues--;
			rc = 1;
		}
	}
	break;
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Create IO SQ\n");
		/* queues have already been initialized for this session.
		   so for now save details in the session for which QPs
		   the remote host attempts to enable.
		*/
		{
			uint16_t qid = cmd->cdw10 & 0xffff;
			uint16_t qsize = cmd->cdw10 >> 16;
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	QID %x, Queue Size %x, CDW11 %x\n",
				      qid, qsize, cmd->cdw11);

			if (qid >= MAX_SESSION_IO_QUEUES) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, " Exceeded Session QP Index Limit\n");
				response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
				rc = -1;
			} else if (session->qps[qid].sq_active > 0) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, " Session SQ QP Index %x Already active!\n", qid);
				response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
				rc = -1;
			} else {
				session->qps[qid].sq_size = qsize;
				session->qps[qid].sq_active = 1;
				if (session->qps[qid].cq_active)
					session->active_queues++;
				rc = 1;
			}
		}
		break;
	case SPDK_NVME_OPC_CREATE_IO_CQ:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Create IO CQ\n");
		/* queues have already been initialized for this session.
		   so for now save details in the session for which QPs
		   the remote host attempts to enable.
		*/
		{
			uint16_t qid = cmd->cdw10 & 0xffff;
			uint16_t qsize = cmd->cdw10 >> 16;
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "	QID %x, Queue Size %x, CDW11 %x\n",
				      qid, qsize, cmd->cdw11);

			if (qid >= MAX_SESSION_IO_QUEUES) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, " Exceeded Session QP Index Limit\n");
				response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
				rc = -1;
			} else if (session->qps[qid].cq_active > 0) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, " Session CQ QP Index %x Already active!\n", qid);
				response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
				rc = -1;
			} else {
				session->qps[qid].cq_size = qsize;
				session->qps[qid].cq_active = 1;
				if (session->qps[qid].sq_active)
					session->active_queues++;
				rc = 1;
			}
		}
		break;
	case SPDK_NVME_OPC_GET_FEATURES:
		feature = cmd->cdw10 & 0xff; /* mask out the FID value */
		switch (feature) {
		case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Number of Queues\n");
			response->cdw0 = ((session->max_io_queues - 1) << 16) | (session->max_io_queues - 1);
			rc = 1; /* immediate completion */
			break;
		case SPDK_NVME_FEAT_LBA_RANGE_TYPE:
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - LBA Range Type\n");
			cmd->nsid = nsid;
			goto passthrough;
			break;
		default:
			goto passthrough;
			break;
		}
		break;
	case SPDK_NVME_OPC_SET_FEATURES:
		feature = cmd->cdw10 & 0xff; /* mask out the FID value */
		switch (feature) {
		case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Number of Queues, cdw11 %x\n", cmd->cdw11);

			/* verify that the contoller is ready to process commands */
			if (session->active_queues != 0) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, "Queue pairs already active!\n");
				response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			} else {
				response->cdw0 = ((session->max_io_queues - 1) << 16) | (session->max_io_queues - 1);
			}
			rc = 1; /* immediate completion */
			break;
		default:
			goto passthrough;
			break;
		}
		break;
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Async Event Request\n");
		/*
		  Trap request here and save in the session context
		  until NVMe library indicates some event.
		*/
		if (session->aer_req_state == NULL) {
			session->aer_req_state = req;
		} else {
			/* AER already recorded, send error response */
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "AER already active!\n");
			response->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
			rc = 1; /* immediate completion */
		}
		break;
	case SPDK_NVME_OPC_KEEP_ALIVE:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Keep Alive\n");
		/*
		  To handle keep alive just clear or reset the
		  session based keep alive duration counter.
		  When added, a separate timer based process
		  will monitor if the time since last recorded
		  keep alive has exceeded the max duration and
		  take appropriate action.
		*/
		//session->keep_alive_timestamp = ;
		rc = 1; /* immediate completion */
		break;
	default:
passthrough:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "RAW Passthrough: Admin Opcode %x for ctrlr %p\n",
			      cmd->opc, ctrlr);
		cmd->nsid = nsid;
		rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr,
						   cmd,
						   req->data, req->length,
						   nvmf_complete_cmd,
						   req);
		if (rc) {
			SPDK_ERRLOG("nvmf_process_admin_cmd: Error to submit Admin Opcode %x\n", cmd->opc);
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		break;
	}

	return rc;
}

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
			spdk_trace_record(TRACE_NVMF_LIB_READ_START, 0, 0, (uint64_t)req, 0);
			rc = spdk_nvme_ns_cmd_read(ns, qpair,
						   req->data, lba_address, lba_count,
						   nvmf_complete_cmd,
						   req, io_flags);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_process_io_cmd: Write; lba address %lx, lba count %x\n",
				      lba_address, lba_count);
			spdk_trace_record(TRACE_NVMF_LIB_WRITE_START, 0, 0, (uint64_t)req, 0);
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
