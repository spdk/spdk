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
#include <rte_debug.h>

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
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "send nvme cmd capsule response\n");

	response->sqid = 0;
	response->status.p = 0;
	response->sqhd = req->conn->sq_head;
	response->cid = req->cid;

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "cpl: cdw0=0x%x rsvd1=0x%x sqhd=0x%x sqid=0x%x cid=0x%x status=0x%x\n",
		      response->cdw0, response->rsvd1, response->sqhd, response->sqid, response->cid,
		      *(uint16_t *)&response->status);

	if (spdk_nvmf_rdma_request_complete(req->conn, req)) {
		SPDK_ERRLOG("Transport request completion error!\n");
		return -1;
	}

	return 0;
}

static int
nvmf_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct nvmf_session *session = req->conn->sess;
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
			rc = 1;
		} else if (cmd->cdw10 == 1) {
			/* identify controller */
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Controller\n");
			/* pull from virtual controller context */
			memcpy(req->data, (char *)&session->vcdata, sizeof(struct spdk_nvme_ctrlr_data));
			rc = 1;
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Namespace List\n");
			response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			rc = -1;
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
		if (session->aer_req == NULL) {
			session->aer_req = req;
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

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		SPDK_ERRLOG("Admin opc 0x%02X not allowed in NVMf\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		rc = -1;
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

static int
nvmf_process_admin_command(struct spdk_nvmf_request *req)
{
	int	ret;

	ret = nvmf_process_admin_cmd(req);
	if (ret) {
		/* library failed the request and should have
		   Updated the response */
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "send nvme admin cmd capsule sync response\n");
		ret = spdk_nvmf_request_complete(req);
		if (ret) {
			SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
			return -1;
		}
	}

	return 0;
}

static int
nvmf_process_io_cmd(struct spdk_nvmf_request *req)
{
	struct nvmf_session *session = req->conn->sess;
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

static int
nvmf_process_io_command(struct spdk_nvmf_request *req)
{
	int	ret;

	/* send to NVMf library for backend NVMe processing */
	ret = nvmf_process_io_cmd(req);
	if (ret) {
		/* library failed the request and should have
		   Updated the response */
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "send nvme io cmd capsule error response\n");
		ret = spdk_nvmf_request_complete(req);
		if (ret) {
			SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
			return -1;
		}
	}

	return 0;
}

static int
nvmf_process_property_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd;
	int	ret;

	cmd = &req->cmd->prop_get_cmd;
	response = &req->rsp->prop_get_rsp;

	nvmf_property_get(req->conn->sess, cmd, response);

	/* send the nvmf response if setup by NVMf library */
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "send property get capsule response\n");
	ret = spdk_nvmf_request_complete(req);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		return -1;
	}

	return 0;
}

static int
nvmf_process_property_set(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_set_rsp *response;
	struct spdk_nvmf_fabric_prop_set_cmd *cmd;
	bool	shutdown = false;
	int	ret;

	cmd = &req->cmd->prop_set_cmd;
	response = &req->rsp->prop_set_rsp;

	nvmf_property_set(req->conn->sess, cmd, response, &shutdown);

	/* TODO: This is not right. It should shut down the whole session.
	if (shutdown == true) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Call to set properties has indicated shutdown\n");
		conn->state = CONN_STATE_FABRIC_DISCONNECT;
	}
	*/

	/* send the nvmf response if setup by NVMf library */
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "send property set capsule response\n");
	ret = spdk_nvmf_request_complete(req);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		return -1;
	}

	return 0;
}

static int
nvmf_process_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_cmd *connect;
	struct spdk_nvmf_fabric_connect_data *connect_data;
	struct spdk_nvmf_fabric_connect_rsp *response;
	struct spdk_nvmf_conn *conn = req->conn;
	struct nvmf_session *session;
	int ret;

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		return -1;
	}

	connect = &req->cmd->connect_cmd;
	connect_data = (struct spdk_nvmf_fabric_connect_data *)req->data;

	RTE_VERIFY(connect_data != NULL);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** Connect Capsule *** %p\n", connect);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** cid              = %x ***\n", connect->cid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** recfmt           = %x ***\n", connect->recfmt);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** qid              = %x ***\n", connect->qid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** sqsize           = %x ***\n", connect->sqsize);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** Connect Capsule Data *** %p\n", connect_data);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** cntlid  = %x ***\n", connect_data->cntlid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** hostid = %04x%04x-%04x-%04x-%04x-%04x%04x%04x ***\n",
		      htons(*(unsigned short *) &connect_data->hostid[0]),
		      htons(*(unsigned short *) &connect_data->hostid[2]),
		      htons(*(unsigned short *) &connect_data->hostid[4]),
		      htons(*(unsigned short *) &connect_data->hostid[6]),
		      htons(*(unsigned short *) &connect_data->hostid[8]),
		      htons(*(unsigned short *) &connect_data->hostid[10]),
		      htons(*(unsigned short *) &connect_data->hostid[12]),
		      htons(*(unsigned short *) &connect_data->hostid[14]));
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** subsiqn = %s ***\n", (char *)&connect_data->subnqn[0]);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** hostiqn = %s ***\n", (char *)&connect_data->hostnqn[0]);

	response = &req->rsp->connect_rsp;

	session = nvmf_connect((void *)conn, connect, connect_data, response);
	if (session != NULL) {
		conn->sess = session;
		conn->qid = connect->qid;
		if (connect->qid > 0) {
			conn->type = CONN_TYPE_IOQ; /* I/O Connection */
		} else {
			/* When session first created, set some attributes */
			nvmf_init_conn_properites(conn, session, response);
		}
	}

	/* synchronous call, nvmf library expected to init
	   response status.
	 */
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "send connect capsule response\n");
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "    *** cntlid  = %x ***\n",
		      response->status_code_specific.success.cntlid);
	ret = spdk_nvmf_request_complete(req);
	if (ret) {
		SPDK_ERRLOG("Unable to send aq qp tx descriptor\n");
		return ret;
	}

	return 0;
}

static int
nvmf_process_fabrics_command(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = &req->cmd->nvmf_cmd;

	switch (cap_hdr->fctype) {
	case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
		return nvmf_process_property_set(req);
	case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
		return nvmf_process_property_get(req);
	case SPDK_NVMF_FABRIC_COMMAND_CONNECT:
		return nvmf_process_connect(req);
	default:
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "recv capsule header type invalid [%x]!\n",
			      cap_hdr->fctype);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return spdk_nvmf_request_complete(req);
	}
}

int
spdk_nvmf_request_prep_data(struct spdk_nvmf_request *req,
			    void *in_cap_data, uint32_t in_cap_len,
			    void *bb, uint32_t bb_len)
{
	struct spdk_nvmf_conn *conn = req->conn;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	enum spdk_nvme_data_transfer xfer;
	int ret;

	req->length = 0;
	req->xfer = SPDK_NVME_DATA_NONE;
	req->data = NULL;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd->nvmf_cmd.fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
	}

	if (xfer != SPDK_NVME_DATA_NONE) {
		struct spdk_nvme_sgl_descriptor *sgl = (struct spdk_nvme_sgl_descriptor *)&cmd->dptr.sgl1;

		if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
		    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
		     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "Keyed data block: raddr 0x%" PRIx64 ", rkey 0x%x, length 0x%x\n",
				      sgl->address, sgl->keyed.key, sgl->keyed.length);

			if (sgl->keyed.length > bb_len) {
				SPDK_ERRLOG("SGL length 0x%x exceeds BB length 0x%x\n",
					    sgl->keyed.length, bb_len);
				return -1;
			}

			req->data = bb;
			req->remote_addr = sgl->address;
			req->rkey = sgl->keyed.key;
			req->length = sgl->keyed.length;
		} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
			   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
			uint64_t offset = sgl->address;
			uint32_t max_len = in_cap_len;

			SPDK_TRACELOG(SPDK_TRACE_RDMA, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
				      offset, sgl->unkeyed.length);

			if (conn->type == CONN_TYPE_AQ) {
				SPDK_ERRLOG("In-capsule data not allowed for admin queue\n");
				return -1;
			}

			if (offset > max_len) {
				SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
					    offset, max_len);
				return -1;
			}
			max_len -= (uint32_t)offset;

			if (sgl->unkeyed.length > max_len) {
				SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
					    sgl->unkeyed.length, max_len);
				return -1;
			}

			req->data = in_cap_data + offset;
			req->length = sgl->unkeyed.length;
		} else {
			SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
				    sgl->generic.type, sgl->generic.subtype);
			return -1;
		}

		if (req->length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
			req->data = NULL;
		}

		req->xfer = xfer;

		/*
		 * For any I/O that requires data to be
		 * pulled into target BB before processing by
		 * the backend NVMe device
		 */
		if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {
				SPDK_TRACELOG(SPDK_TRACE_RDMA, "Issuing RDMA Read to get host data\n");
				ret = nvmf_post_rdma_read(conn, req);
				if (ret) {
					SPDK_ERRLOG("Unable to post rdma read tx descriptor\n");
					return -1;
				}

				/* Wait for transfer to complete before executing command. */
				return 1;
			}
		}
	}

	if (xfer == SPDK_NVME_DATA_NONE) {
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "No data to transfer\n");
		RTE_VERIFY(req->data == NULL);
		RTE_VERIFY(req->length == 0);
	} else {
		RTE_VERIFY(req->data != NULL);
		RTE_VERIFY(req->length != 0);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "%s data ready\n",
			      xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER ? "Host to Controller" :
			      "Controller to Host");
	}

	return 0;
}

int
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		return nvmf_process_fabrics_command(req);
	} else if (req->conn->type == CONN_TYPE_AQ) {
		return nvmf_process_admin_command(req);
	} else {
		return nvmf_process_io_command(req);
	}
}
