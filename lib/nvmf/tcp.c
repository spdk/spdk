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

#include "spdk/assert.h"
#include "spdk/io_channel.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/*
 * AIO backend requires block size aligned data buffers,
 * extra 4KiB aligned data buffer should work for most devices.
 */
#define SHIFT_4KB			12u
#define NVMF_DATA_BUFFER_ALIGNMENT	(1u << SHIFT_4KB)
#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1u)

enum spdk_nvmf_tcp_request_state {

	// TODO: this is all copied from RDMA - clean up/remove states as needed

	/* The request is not currently in use */
	TCP_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	TCP_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	TCP_REQUEST_STATE_NEED_BUFFER,

	/* The request is waiting on TCP queue depth availability
	 * to transfer data from the host to the controller.
	 */
	TCP_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER,

	/* The request is currently transferring data from the host to the controller. */
	TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	TCP_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	TCP_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	TCP_REQUEST_STATE_EXECUTED,

	/* The request is waiting on TCP queue depth availability
	 * to transfer data from the controller to the host.
	 */
	TCP_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST,

	/* The request is ready to send a completion */
	TCP_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request currently has a completion outstanding */
	TCP_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	TCP_REQUEST_STATE_COMPLETED,
};

struct spdk_nvmf_tcp_request {
	struct spdk_nvmf_request		req;
	void					*in_capsule_data;
	void					*data_from_pool;
	union nvmf_h2c_msg			cmd;
	union nvmf_c2h_msg			rsp;
	enum spdk_nvmf_tcp_request_state	state;
	TAILQ_ENTRY(spdk_nvmf_tcp_request)	link;
};

enum spdk_nvmf_tcp_qpair_recv_state {
	TQPAIR_RECV_STATE_INVALID,

	/* New connection waiting for ICReq */
	TQPAIR_RECV_STATE_AWAIT_ICREQ,

	/* Active connection waiting for any PDU type */
	TQPAIR_RECV_STATE_AWAIT_PDU_HDR,

	/* Active connection waiting for CapsuleCmd (already received header) */
	TQPAIR_RECV_STATE_AWAIT_CAPSULE_CMD,
	// TODO: state for receiving capsules, etc.
};

union spdk_nvmf_tcp_recv_pdu {
	uint8_t				raw[128];
	struct spdk_nvme_tcp_pdu_hdr	hdr;
	struct spdk_nvme_tcp_ic_req	ic_req;
	struct spdk_nvme_tcp_term_req	term_req;
	struct spdk_nvme_tcp_cmd	capsule_cmd;
	struct spdk_nvme_tcp_h2c_data	h2c_data;
};

union spdk_nvmf_tcp_send_pdu {
	uint8_t				raw[128];
	struct spdk_nvme_tcp_pdu_hdr	hdr;
	struct spdk_nvme_tcp_ic_resp	ic_resp;
	struct spdk_nvme_tcp_term_resp	term_resp;
	struct spdk_nvme_tcp_rsp	capsule_resp;
	struct spdk_nvme_tcp_c2h_data	c2h_data;
	struct spdk_nvme_tcp_r2t	r2t;
};

struct spdk_nvmf_tcp_qpair;

typedef void (*spdk_nvmf_tcp_qpair_xfer_complete_cb)(struct spdk_nvmf_tcp_qpair *tqpair,
		void *cb_arg);

/* Data to be sent to host */
struct spdk_nvmf_tcp_send_data {
	STAILQ_ENTRY(spdk_nvmf_tcp_send_data)	link;
	void					*buf;
	uint32_t				remaining;

	spdk_nvmf_tcp_qpair_xfer_complete_cb	complete_cb;
	void					*cb_arg;
};

struct spdk_nvmf_tcp_qpair {
	TAILQ_ENTRY(spdk_nvmf_tcp_qpair)	link;
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_tcp_port		*port;
	struct spdk_sock			*sock;

	enum spdk_nvmf_tcp_qpair_recv_state	recv_state;

	// TODO: try to line up recv_pdu on a nice alignment
	// (or explicitly align to 64)
	union spdk_nvmf_tcp_recv_pdu		recv_pdu;

	spdk_nvmf_tcp_qpair_xfer_complete_cb	recv_complete_cb;
	void					*recv_complete_cb_arg;

	/*
	 * Current buffer to receive into for this state.
	 * Updated by _spdk_nvmf_tcp_qpair_set_recv_state()
	 */
	void					*recv_buf;
	uint32_t				recv_offset;
	uint32_t				recv_remaining;

	STAILQ_HEAD(, spdk_nvmf_tcp_send_data)	send_queue;
	STAILQ_HEAD(, spdk_nvmf_tcp_send_data)	send_queue_free;

	uint32_t				c2hdoff_bytes;


	uint64_t				maxr2t;

	uint32_t				host_hdgst_enable : 1;
	uint32_t				host_ddgst_enable : 1;



	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* Requests that are not in use */
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	free_queue;

	/* Array of size "max_queue_depth" containing requests. */
	struct spdk_nvmf_tcp_request		*reqs;



	/* Mgmt channel */
	struct spdk_io_channel			*mgmt_channel;
	struct spdk_nvmf_tcp_mgmt_channel	*ch;
};

struct spdk_nvmf_tcp_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	struct spdk_sock_group			*sock_group;
	TAILQ_HEAD(, spdk_nvmf_tcp_qpair)	qpairs;
};

struct spdk_nvmf_tcp_port {
	struct spdk_nvme_transport_id		trid;
	struct spdk_sock			*listen_sock;
	uint32_t				ref;
	TAILQ_ENTRY(spdk_nvmf_tcp_port)		link;
};

struct spdk_nvmf_tcp_transport {
	struct spdk_nvmf_transport		transport;

	pthread_mutex_t				lock;

	struct spdk_mempool			*data_buf_pool;

	uint16_t				max_queue_depth;
	uint32_t				max_io_size;
	uint32_t				in_capsule_data_size;

	TAILQ_HEAD(, spdk_nvmf_tcp_port)	ports;
};

struct spdk_nvmf_tcp_mgmt_channel {
	/* Requests that are waiting to obtain a data buffer */
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	pending_data_buf_queue;
};

static int
spdk_nvmf_tcp_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tcp_mgmt_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->pending_data_buf_queue);
	return 0;
}

static void
spdk_nvmf_tcp_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tcp_mgmt_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->pending_data_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on channel destruction\n");
	}
}

static void
spdk_nvmf_tcp_qpair_destroy(struct spdk_nvmf_tcp_qpair *tqpair)
{
	unsigned i;

#if 0
	if (tqpair->poller) {
		TAILQ_REMOVE(&tqpair->poller->qpairs, tqpair, link);
	}
#endif

	spdk_sock_close(&tqpair->sock);
	// TODO: close sock

	if (tqpair->mgmt_channel) {
		spdk_put_io_channel(tqpair->mgmt_channel);
	}

	for (i = 0; i < tqpair->max_queue_depth; i++) {
		struct spdk_nvmf_tcp_request *treq = &tqpair->reqs[i];

		spdk_dma_free(treq->in_capsule_data);
	}

	free(tqpair->reqs);
	free(tqpair);
}

#if 0
static int
spdk_nvmf_tcp_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				i;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New TCP Connection: %p\n", qpair);

	tqpair->reqs = calloc(tqpair->max_queue_depth, sizeof(*tqpair->reqs));
	if (!tqpair->reqs) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for TCP queue.\n");
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return -1;
	}

#if 0
	for (i = 0; i < tqpair->max_queue_depth; i++) {
		tcp_recv = &tqpair->recvs[i];
		tcp_recv->qpair = tqpair;

		/* Set up memory to receive commands */
		tcp_recv->buf = (void *)((uintptr_t)tqpair->bufs + (i * ttransport->in_capsule_data_size));

		tcp_recv->sgl[0].addr = (uintptr_t)&tqpair->cmds[i];
		tcp_recv->sgl[0].length = sizeof(tqpair->cmds[i]);
		tcp_recv->sgl[0].lkey = tqpair->cmds_mr->lkey;

		tcp_recv->sgl[1].addr = (uintptr_t)tcp_recv->buf;
		tcp_recv->sgl[1].length = ttransport->in_capsule_data_size;
		tcp_recv->sgl[1].lkey = tqpair->bufs_mr->lkey;

		tcp_recv->wr.wr_id = (uintptr_t)tcp_recv;
		tcp_recv->wr.sg_list = tcp_recv->sgl;
		tcp_recv->wr.num_sge = SPDK_COUNTOF(tcp_recv->sgl);

		rc = ibv_post_recv(tqpair->cm_id->qp, &tcp_recv->wr, &bad_wr);
		if (rc) {
			SPDK_ERRLOG("Unable to post capsule for TCP RECV\n");
			spdk_nvmf_tcp_qpair_destroy(tqpair);
			return -1;
		}
	}
#endif

	for (i = 0; i < tqpair->max_queue_depth; i++) {
		struct spdk_nvmf_tcp_request *treq = &tqpair->reqs[i];

		treq->req.qpair = &tqpair->qpair;
		treq->req.cmd = &treq->cmd;
		treq->req.rsp = &treq->rsp;
		treq->in_capsule_data = spdk_dma_zmalloc(ttransport->in_capsule_data_size,
					NVMF_DATA_BUFFER_ALIGNMENT,
					NULL);
		if (treq->in_capsule_data == NULL) {
			// TODO - need to go back and free previous requests
		}

		TAILQ_INSERT_TAIL(&tqpair->free_queue, treq, link);
	}

	return 0;
}
#endif

#if 0
static int
request_transfer_in(struct spdk_nvmf_request *req)
{
	int				rc;
	struct spdk_nvmf_tcp_request	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct ibv_send_wr		*bad_wr = NULL;

	qpair = req->qpair;
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);

	tqpair->cur_tcp_rw_depth++;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP READ POSTED. Request: %p Connection: %p\n", req, qpair);
	spdk_trace_record(TRACE_TCP_READ_START, 0, 0, (uintptr_t)req, 0);

	tcp_req->data.wr.opcode = IBV_WR_TCP_READ;
	tcp_req->data.wr.next = NULL;
	rc = ibv_post_send(tqpair->cm_id->qp, &tcp_req->data.wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to transfer data from host to target\n");

		/* Decrement r/w counter back since data transfer
		 * has not started.
		 */
		tqpair->cur_tcp_rw_depth--;
		return -1;
	}

	return 0;
}

static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	int				rc;
	struct spdk_nvmf_tcp_request	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	struct ibv_send_wr		*send_wr, *bad_send_wr = NULL;

	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	/* Post the capsule to the recv buffer */
	assert(tcp_req->recv != NULL);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP RECV POSTED. Recv: %p Connection: %p\n", tcp_req->recv,
		      tqpair);
	rc = ibv_post_recv(tqpair->cm_id->qp, &tcp_req->recv->wr, &bad_recv_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return rc;
	}
	tcp_req->recv = NULL;

	/* Build the response which consists of an optional
	 * TCP WRITE to transfer data, plus an TCP SEND
	 * containing the response.
	 */
	send_wr = &tcp_req->rsp.wr;

	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP WRITE POSTED. Request: %p Connection: %p\n", req, qpair);
		spdk_trace_record(TRACE_TCP_WRITE_START, 0, 0, (uintptr_t)req, 0);

		tqpair->cur_tcp_rw_depth++;
		tcp_req->data.wr.opcode = IBV_WR_TCP_WRITE;

		tcp_req->data.wr.next = send_wr;
		send_wr = &tcp_req->data.wr;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP SEND POSTED. Request: %p Connection: %p\n", req, qpair);
	spdk_trace_record(TRACE_NVMF_IO_COMPLETE, 0, 0, (uintptr_t)req, 0);

	/* Send the completion */
	rc = ibv_post_send(tqpair->cm_id->qp, send_wr, &bad_send_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to send response capsule\n");

		if (tcp_req->data.wr.opcode == IBV_WR_TCP_WRITE) {
			/* Decrement r/w counter back since data transfer
			 * has not started.
			 */
			tqpair->cur_tcp_rw_depth--;
		}
	}

	return rc;
}
#endif

#if 0
static void
nvmf_tcp_handle_disconnect(void *ctx)
{
	struct spdk_nvmf_qpair		*qpair = ctx;
	struct spdk_nvmf_ctrlr		*ctrlr;
	struct spdk_nvmf_tcp_qpair	*tqpair;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	ctrlr = qpair->ctrlr;
	if (ctrlr == NULL) {
		/* No ctrlr has been established yet, so destroy
		 * the connection.
		 */
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return;
	}

	spdk_nvmf_ctrlr_disconnect(qpair);
}

static int
nvmf_tcp_disconnect(struct spdk_sock *sock)
{
#if 0
	struct spdk_nvmf_qpair	*qpair;
	struct spdk_io_channel	*ch;

	qpair = evt->id->context;
	if (qpair == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}
	/* ack the disconnect event before tcp_destroy_id */
	tcp_ack_cm_event(evt);

	ch = spdk_io_channel_from_ctx(qpair->group);
	spdk_thread_send_msg(spdk_io_channel_get_thread(ch), nvmf_tcp_handle_disconnect, qpair);
#endif

	return 0;
}

static enum spdk_nvme_data_transfer spdk_nvmf_tcp_request_get_xfer(struct spdk_nvmf_tcp_request
		*tcp_req)
{
	enum spdk_nvme_data_transfer xfer;
	struct spdk_nvme_cmd *cmd = &tcp_req->req.cmd->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;

	/* Figure out data transfer direction */
	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		xfer = spdk_nvme_opc_get_data_transfer(tcp_req->req.cmd->nvmf_cmd.fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

		/* Some admin commands are special cases */
		if ((tcp_req->req.qpair->qid == 0) &&
		    ((cmd->opc == SPDK_NVME_OPC_GET_FEATURES) ||
		     (cmd->opc == SPDK_NVME_OPC_SET_FEATURES))) {
			switch (cmd->cdw10 & 0xff) {
			case SPDK_NVME_FEAT_LBA_RANGE_TYPE:
			case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION:
			case SPDK_NVME_FEAT_HOST_IDENTIFIER:
				break;
			default:
				xfer = SPDK_NVME_DATA_NONE;
			}
		}
	}

	if (xfer == SPDK_NVME_DATA_NONE) {
		return xfer;
	}

	/* Even for commands that may transfer data, they could have specified 0 length.
	 * We want those to show up with xfer SPDK_NVME_DATA_NONE.
	 */
	switch (sgl->generic.type) {
	case SPDK_NVME_SGL_TYPE_DATA_BLOCK:
	case SPDK_NVME_SGL_TYPE_BIT_BUCKET:
	case SPDK_NVME_SGL_TYPE_SEGMENT:
	case SPDK_NVME_SGL_TYPE_LAST_SEGMENT:
	case SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK:
		if (sgl->unkeyed.length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	case SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK:
		if (sgl->keyed.length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	}

	return xfer;
}

static int
spdk_nvmf_tcp_request_parse_sgl(struct spdk_nvmf_tcp_transport *ttransport,
				struct spdk_nvmf_tcp_device *device,
				struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;

	cmd = &tcp_req->req.cmd->nvme_cmd;
	rsp = &tcp_req->req.rsp->nvme_cpl;
	sgl = &cmd->dptr.sgl1;

	// TODO: TCP is supposed to use transport-specific SGL stuff - check spec

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
		if (sgl->keyed.length > ttransport->max_io_size) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    sgl->keyed.length, ttransport->max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		tcp_req->req.length = sgl->keyed.length;
		tcp_req->data_from_pool = spdk_mempool_get(ttransport->data_buf_pool);
		if (!tcp_req->data_from_pool) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "No available large data buffers. Queueing request %p\n", tcp_req);
			return 0;
		}
		/* AIO backend requires block size aligned data buffers,
		 * 4KiB aligned data buffer should work for most devices.
		 */
		tcp_req->req.data = (void *)((uintptr_t)(tcp_req->data_from_pool + NVMF_DATA_BUFFER_MASK)
					     & ~NVMF_DATA_BUFFER_MASK);
		tcp_req->data.sgl[0].addr = (uintptr_t)tcp_req->req.data;
		tcp_req->data.sgl[0].length = sgl->keyed.length;
		tcp_req->data.sgl[0].lkey = ((struct ibv_mr *)spdk_mem_map_translate(device->map,
					     (uint64_t)tcp_req->req.data))->lkey;
		tcp_req->data.wr.wr.tcp.rkey = sgl->keyed.key;
		tcp_req->data.wr.wr.tcp.remote_addr = sgl->address;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Request %p took buffer from central pool\n", tcp_req);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = ttransport->in_capsule_data_size;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, sgl->unkeyed.length);

		if (offset > max_len) {
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
				    offset, max_len);
			rsp->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
			return -1;
		}
		max_len -= (uint32_t)offset;

		if (sgl->unkeyed.length > max_len) {
			SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
				    sgl->unkeyed.length, max_len);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		tcp_req->req.data = tcp_req->recv->buf + offset;
		tcp_req->data_from_pool = NULL;
		tcp_req->req.length = sgl->unkeyed.length;
		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}
#endif

#if 0
static bool
spdk_nvmf_tcp_request_process(struct spdk_nvmf_tcp_transport *ttransport,
			      struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvmf_tcp_device	*device;
	struct spdk_nvme_cpl		*rsp = &tcp_req->req.rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_tcp_recv	*tcp_recv;
	enum spdk_nvmf_tcp_request_state prev_state;
	bool				progress = false;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	device = tqpair->port->device;

	assert(tcp_req->state != TCP_REQUEST_STATE_FREE);

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tcp_req->state;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Request %p entering state %d\n", tcp_req, prev_state);

		switch (tcp_req->state) {
		case TCP_REQUEST_STATE_FREE:
			/* Some external code must kick a request into TCP_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_NEW:
			tqpair->cur_queue_depth++;
			tcp_recv = tcp_req->recv;

			/* The first element of the SGL is the NVMe command */
			tcp_req->req.cmd = (union nvmf_h2c_msg *)tcp_recv->sgl[0].addr;
			memset(tcp_req->req.rsp, 0, sizeof(*tcp_req->req.rsp));

			TAILQ_REMOVE(&tqpair->incoming_queue, tcp_recv, link);
			TAILQ_REMOVE(&tqpair->free_queue, tcp_req, link);

			/* The next state transition depends on the data transfer needs of this request. */
			tcp_req->req.xfer = spdk_nvmf_tcp_request_get_xfer(tcp_req);

			/* If no data to transfer, ready to execute. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_NONE) {
				tcp_req->state = TCP_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}

			tcp_req->state = TCP_REQUEST_STATE_NEED_BUFFER;
			TAILQ_INSERT_TAIL(&tqpair->ch->pending_data_buf_queue, tcp_req, link);
			break;
		case TCP_REQUEST_STATE_NEED_BUFFER:
			assert(tcp_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (tcp_req != TAILQ_FIRST(&tqpair->ch->pending_data_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = spdk_nvmf_tcp_request_parse_sgl(ttransport, device, tcp_req);
			if (rc < 0) {
				TAILQ_REMOVE(&tqpair->ch->pending_data_buf_queue, tcp_req, link);
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				tcp_req->state = TCP_REQUEST_STATE_READY_TO_COMPLETE;
				break;
			}

			if (!tcp_req->req.data) {
				/* No buffers available. */
				break;
			}

			TAILQ_REMOVE(&tqpair->ch->pending_data_buf_queue, tcp_req, link);

			/* If data is transferring from host to controller and the data didn't
			 * arrive using in capsule data, we need to do a transfer from the host.
			 */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER && tcp_req->data_from_pool != NULL) {
				tcp_req->state = TCP_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER;
				TAILQ_INSERT_TAIL(&tqpair->pending_tcp_rw_queue, tcp_req, link);
				break;
			}

			tcp_req->state = TCP_REQUEST_STATE_READY_TO_EXECUTE;
			break;
		case TCP_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER:
			if (tcp_req != TAILQ_FIRST(&tqpair->pending_tcp_rw_queue)) {
				/* This request needs to wait in line to perform TCP */
				break;
			}

			if (tqpair->cur_tcp_rw_depth < tqpair->max_rw_depth) {
				TAILQ_REMOVE(&tqpair->pending_tcp_rw_queue, tcp_req, link);
				tcp_req->state = TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;
				rc = request_transfer_in(&tcp_req->req);
				if (rc) {
					rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					tcp_req->state = TCP_REQUEST_STATE_READY_TO_COMPLETE;
				}
			}
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			/* Some external code must kick a request into TCP_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_READY_TO_EXECUTE:
			tcp_req->state = TCP_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(&tcp_req->req);
			break;
		case TCP_REQUEST_STATE_EXECUTING:
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_EXECUTED:
			if (tcp_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				tcp_req->state = TCP_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST;
				TAILQ_INSERT_TAIL(&tqpair->pending_tcp_rw_queue, tcp_req, link);
			} else {
				tcp_req->state = TCP_REQUEST_STATE_READY_TO_COMPLETE;
			}
			break;
		case TCP_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST:
			if (tcp_req != TAILQ_FIRST(&tqpair->pending_tcp_rw_queue)) {
				/* This request needs to wait in line to perform TCP */
				break;
			}

			if (tqpair->cur_tcp_rw_depth < tqpair->max_rw_depth) {
				tcp_req->state = TCP_REQUEST_STATE_READY_TO_COMPLETE;
				TAILQ_REMOVE(&tqpair->pending_tcp_rw_queue, tcp_req, link);
			}
			break;
		case TCP_REQUEST_STATE_READY_TO_COMPLETE:
			tcp_req->state = TCP_REQUEST_STATE_COMPLETING;

			rc = request_transfer_out(&tcp_req->req);
			assert(rc == 0); /* No good way to handle this currently */
			break;
		case TCP_REQUEST_STATE_COMPLETING:
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_COMPLETED:
			assert(tqpair->cur_queue_depth > 0);
			tqpair->cur_queue_depth--;

			if (tcp_req->data_from_pool) {
				/* Put the buffer back in the pool */
				spdk_mempool_put(ttransport->data_buf_pool, tcp_req->data_from_pool);
				tcp_req->data_from_pool = NULL;
			}
			tcp_req->req.length = 0;
			tcp_req->req.data = NULL;
			tcp_req->state = TCP_REQUEST_STATE_FREE;
			TAILQ_INSERT_TAIL(&tqpair->free_queue, tcp_req, link);
			break;
		}

		if (tcp_req->state != prev_state) {
			progress = true;
		}
	} while (tcp_req->state != prev_state);

	return progress;
}
#endif



static struct spdk_nvmf_transport *
spdk_nvmf_tcp_create(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_tcp_transport *ttransport;

	ttransport = calloc(1, sizeof(*ttransport));
	if (!ttransport) {
		return NULL;
	}

	pthread_mutex_init(&ttransport->lock, NULL);
	TAILQ_INIT(&ttransport->ports);

	ttransport->transport.tgt = tgt;
	ttransport->transport.ops = &spdk_nvmf_transport_tcp;

	SPDK_NOTICELOG("*** TCP Transport Init ***\n");

	ttransport->max_queue_depth = tgt->opts.max_queue_depth;
	ttransport->max_io_size = tgt->opts.max_io_size;
	ttransport->in_capsule_data_size = tgt->opts.in_capsule_data_size;

	// TODO: create poll group for listen sockets

	ttransport->data_buf_pool = spdk_mempool_create("spdk_nvmf_tcp",
				    ttransport->max_queue_depth * 4, /* The 4 is arbitrarily chosen. Needs to be configurable. */
				    ttransport->max_io_size + NVMF_DATA_BUFFER_ALIGNMENT,
				    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!ttransport->data_buf_pool) {
		SPDK_ERRLOG("Unable to allocate buffer pool for poll group\n");
		free(ttransport);
		return NULL;
	}

	spdk_io_device_register(ttransport, spdk_nvmf_tcp_mgmt_channel_create,
				spdk_nvmf_tcp_mgmt_channel_destroy,
				sizeof(struct spdk_nvmf_tcp_mgmt_channel));

	return &ttransport->transport;
}

static int
spdk_nvmf_tcp_destroy(struct spdk_nvmf_transport *transport)
{
	// TODO
#if 0
	struct spdk_nvmf_tcp_transport	*ttransport;
	struct spdk_nvmf_tcp_port	*port, *port_tmp;
	struct spdk_nvmf_tcp_device	*device, *device_tmp;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	TAILQ_FOREACH_SAFE(port, &ttransport->ports, link, port_tmp) {
		TAILQ_REMOVE(&ttransport->ports, port, link);
		tcp_destroy_id(port->id);
		free(port);
	}

	TAILQ_FOREACH_SAFE(device, &ttransport->devices, link, device_tmp) {
		TAILQ_REMOVE(&ttransport->devices, device, link);
		if (device->map) {
			spdk_mem_map_free(&device->map);
		}
		free(device);
	}

	if (spdk_mempool_count(ttransport->data_buf_pool) != (ttransport->max_queue_depth * 4)) {
		SPDK_ERRLOG("transport buffer pool count is %zu but should be %u\n",
			    spdk_mempool_count(ttransport->data_buf_pool),
			    ttransport->max_queue_depth * 4);
	}

	spdk_mempool_free(ttransport->data_buf_pool);
	spdk_io_device_unregister(ttransport, NULL);
	free(ttransport);
#endif
	return 0;
}

static int
_spdk_nvmf_tcp_trsvcid_to_int(const char *trsvcid)
{
	unsigned long long ull;
	char *end = NULL;

	ull = strtoull(trsvcid, &end, 10);
	if (end == NULL || end == trsvcid || *end != '\0') {
		return -1;
	}

	/* Valid TCP/IP port numbers are in [0, 65535] */
	if (ull > 65535) {
		return -1;
	}

	return (int)ull;
}

/**
 * Canonicalize a listen address trid.
 */
static int
_spdk_nvmf_tcp_canon_listen_trid(struct spdk_nvme_transport_id *canon_trid,
				 const struct spdk_nvme_transport_id *trid)
{
	int trsvcid_int;

	trsvcid_int = _spdk_nvmf_tcp_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		return -EINVAL;
	}

	memset(canon_trid, 0, sizeof(*canon_trid));
	canon_trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	canon_trid->adrfam = trid->adrfam;
	snprintf(canon_trid->traddr, sizeof(canon_trid->traddr), "%s", trid->traddr);
	snprintf(canon_trid->trsvcid, sizeof(canon_trid->trsvcid), "%d", trsvcid_int);

	return 0;
}

/**
 * Find an existing listening port.
 *
 * Caller must hold ttransport->lock.
 */
static struct spdk_nvmf_tcp_port *
_spdk_nvmf_tcp_find_port(struct spdk_nvmf_tcp_transport *ttransport,
			 const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_transport_id canon_trid;
	struct spdk_nvmf_tcp_port *port;

	if (_spdk_nvmf_tcp_canon_listen_trid(&canon_trid, trid) != 0) {
		return NULL;
	}

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		if (spdk_nvme_transport_id_compare(&canon_trid, &port->trid) == 0) {
			return port;
		}
	}

	return NULL;
}


static int
spdk_nvmf_tcp_listen(struct spdk_nvmf_transport *transport,
		     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	int trsvcid_int;
	uint8_t adrfam;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	trsvcid_int = _spdk_nvmf_tcp_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		SPDK_ERRLOG("Invalid trsvcid '%s'\n", trid->trsvcid);
		return -EINVAL;
	}

	pthread_mutex_lock(&ttransport->lock);

	port = _spdk_nvmf_tcp_find_port(ttransport, trid);
	if (port) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Already listening on %s port %s\n",
			      trid->traddr, trid->trsvcid);
		port->ref++;
		pthread_mutex_unlock(&ttransport->lock);
		return 0;
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	port->ref = 1;

	if (_spdk_nvmf_tcp_canon_listen_trid(&port->trid, trid) != 0) {
		SPDK_ERRLOG("Invalid traddr %s / trsvcid %s\n",
			    trid->traddr, trid->trsvcid);
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	port->listen_sock = spdk_sock_listen(trid->traddr, trsvcid_int);
	if (port->listen_sock == NULL) {
		SPDK_ERRLOG("spdk_sock_listen(%s, %d) failed: %s (%d)\n",
			    trid->traddr, trsvcid_int,
			    spdk_strerror(errno), errno);
		free(port);
		pthread_mutex_unlock(&ttransport->lock);
		return -errno;
	}

	if (spdk_sock_is_ipv4(port->listen_sock)) {
		adrfam = SPDK_NVMF_ADRFAM_IPV4;
	} else if (spdk_sock_is_ipv6(port->listen_sock)) {
		adrfam = SPDK_NVMF_ADRFAM_IPV6;
	} else {
		SPDK_ERRLOG("Unhandled socket type\n");
		adrfam = 0;
	}

	if (adrfam != trid->adrfam) {
		SPDK_ERRLOG("Socket address family mismatch\n");
		spdk_sock_close(&port->listen_sock);
		free(port);
		pthread_mutex_unlock(&ttransport->lock);
		return -EINVAL;
	}

	SPDK_NOTICELOG("*** NVMe/TCP Target Listening on %s port %d ***\n",
		       trid->traddr, trsvcid_int);

	TAILQ_INSERT_TAIL(&ttransport->ports, port, link);
	pthread_mutex_unlock(&ttransport->lock);

	return 0;
}

static int
spdk_nvmf_tcp_stop_listen(struct spdk_nvmf_transport *transport,
			  const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	int rc;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Removing listen address %s port %s\n",
		      trid->traddr, trid->trsvcid);

	pthread_mutex_lock(&ttransport->lock);
	port = _spdk_nvmf_tcp_find_port(ttransport, trid);
	if (port) {
		assert(port->ref > 0);
		port->ref--;
		if (port->ref == 0) {
			TAILQ_REMOVE(&ttransport->ports, port, link);
			// TODO: remove sock from acceptor listen group
			spdk_sock_close(&port->listen_sock);
			free(port);
		}
		rc = 0;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Port not found\n");
		rc = -ENOENT;
	}
	pthread_mutex_unlock(&ttransport->lock);

	return rc;
}

static void
_spdk_nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
				    enum spdk_nvmf_tcp_qpair_recv_state recv_state,
				    void *recv_buf,
				    size_t recv_buf_size,
				    spdk_nvmf_tcp_qpair_xfer_complete_cb recv_complete_cb,
				    void *cb_arg)
{
	assert(recv_buf != NULL);
	assert(recv_buf_size != 0);

	tqpair->recv_state = recv_state;
	tqpair->recv_complete_cb = recv_complete_cb;
	tqpair->recv_complete_cb_arg = cb_arg;
	tqpair->recv_buf = recv_buf;
	tqpair->recv_offset = 0;
	tqpair->recv_remaining = recv_buf_size;
}

static void
_spdk_nvmf_tcp_qpair_send(struct spdk_nvmf_tcp_qpair *tqpair,
			  void *send_buf,
			  size_t send_buf_size,
			  spdk_nvmf_tcp_qpair_xfer_complete_cb send_complete_cb,
			  void *cb_arg)
{
	struct spdk_nvmf_tcp_send_data *send_data;

	send_data = STAILQ_FIRST(&tqpair->send_queue_free);
	// TODO: ensure this can never happen, or add a return code and error handling
	assert(send_data != NULL);
	STAILQ_REMOVE_HEAD(&tqpair->send_queue_free, link);

	send_data->buf = send_buf;
	send_data->remaining = send_buf_size;
	send_data->complete_cb = send_complete_cb;
	send_data->cb_arg = cb_arg;

	STAILQ_INSERT_TAIL(&tqpair->send_queue, send_data, link);
}

/* recv states */
static void _spdk_nvmf_tcp_recv_icreq_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg);
static void _spdk_nvmf_tcp_recv_pdu_hdr_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg);
static void _spdk_nvmf_tcp_recv_capsule_cmd_complete(struct spdk_nvmf_tcp_qpair *tqpair,
		void *cb_arg);

/* send states */
static void _spdk_nvmf_tcp_send_icresp_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg);

static void
_spdk_nvmf_tcp_recv_icreq_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg)
{
	struct spdk_nvme_tcp_ic_req *ic_req = &tqpair->recv_pdu.ic_req;
	struct spdk_nvme_tcp_ic_resp *ic_resp;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Got PDU header\n");

	if (ic_req->hdr.pdu_type != SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		SPDK_ERRLOG("Expected ICReq pdu_type %u, got %u\n",
			    SPDK_NVME_TCP_PDU_TYPE_IC_REQ, ic_req->hdr.pdu_type);
		// TODO terminate connection response

		return;
	}

	if (ic_req->hdr.length != sizeof(struct spdk_nvme_tcp_ic_req)) {
		SPDK_ERRLOG("Expected ICReq PDU length %zu, got %u\n",
			    sizeof(struct spdk_nvme_tcp_ic_req), ic_req->hdr.length);
		// TODO terminate connection response
		return;
	}

	/* Only PFV 0 is defined currently */
	if (ic_req->pfv != 0) {
		SPDK_ERRLOG("Expected ICReq PFV %u, got %u\n",
			    0u, ic_req->pfv);
		// TODO: terminate connection response
		return;
	}

	/* MAXR2T is 0's based */
	tqpair->maxr2t = ic_req->maxr2t + 1ull;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "maxr2t = %" PRIu64 "\n", tqpair->maxr2t);

	if (ic_req->c2hdoff > SPDK_NVME_TCP_C2HDOFF_MAX) {
		SPDK_ERRLOG("C2HDOFF %u > max %u\n",
			    ic_req->c2hdoff, SPDK_NVME_TCP_C2HDOFF_MAX);
		// TODO: terminate connection response
		return;
	}

	tqpair->c2hdoff_bytes = ic_req->c2hdoff * SPDK_NVME_TCP_C2HDOFF_MULT;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "C2HDOFF = %u * %u = %u bytes\n",
		      ic_req->c2hdoff, SPDK_NVME_TCP_C2HDOFF_MULT, tqpair->c2hdoff_bytes);

	tqpair->host_hdgst_enable = ic_req->dgst.bits.hdgst_enable;
	tqpair->host_ddgst_enable = ic_req->dgst.bits.ddgst_enable;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	ic_resp = calloc(1, sizeof(*ic_resp)); // TODO: keep a pool of these instead
	if (ic_resp == NULL) {
		// TODO: handle failure somehow
		return;
	}

	ic_resp->hdr.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	ic_resp->hdr.length = sizeof(*ic_resp);
	ic_resp->pfv = 0;
	ic_resp->sts = SPDK_NVME_TCP_INIT_CONN_STS_SUCCESS;
	ic_resp->maxdata = 4096; // TODO - new nvmf_tgt config option? bdev large buffer size?
	ic_resp->h2cdoff = 0;
	ic_resp->dgst.bits.hdgst_enable = 1; // TODO: nvmf_tgt option
	ic_resp->dgst.bits.ddgst_enable = 1; // TODO: nvmf_tgt option

	_spdk_nvmf_tcp_qpair_send(tqpair, ic_resp, sizeof(*ic_resp),
				  _spdk_nvmf_tcp_send_icresp_complete, ic_resp);

	// TODO: build ICResp in send_pdu
	// -- TODO: this won't work generally, since there may be multiple responses queued - need to think about this more
	// TODO: set send state to send ICResp
	// TODO: set recv state to recv capsules (or set to invalid so that no data can be recvd until ICResp is received?)

	//_spdk_nvmf_tcp_qpair_set_recv_state(tqpair, ...,
}

static void
_spdk_nvmf_tcp_send_icresp_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg)
{
	struct spdk_nvme_tcp_ic_resp *ic_resp = cb_arg;

	free(ic_resp);

	// TODO: do more stuff here
	// -- now ready to receive capsules

	_spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TQPAIR_RECV_STATE_AWAIT_PDU_HDR,
					    &tqpair->recv_pdu.hdr, sizeof(tqpair->recv_pdu.hdr),
					    _spdk_nvmf_tcp_recv_pdu_hdr_complete, NULL);

}

static void
_spdk_nvmf_tcp_recv_pdu_hdr_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg)
{
	/* We got a PDU header - let's see what type it is */
	switch (tqpair->recv_pdu.hdr.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		// TODO: check length field

		_spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TQPAIR_RECV_STATE_AWAIT_CAPSULE_CMD,
						    &tqpair->recv_pdu.hdr + 1,
						    sizeof(tqpair->recv_pdu.capsule_cmd) - sizeof(tqpair->recv_pdu.hdr),
						    _spdk_nvmf_tcp_recv_capsule_cmd_complete, NULL);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		// TODO: recv h2c data headers + data
		break;
	default:
		// TODO - close connection? recv pdu and skip it?
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu.hdr.pdu_type);
		break;
	}
}

static void
_spdk_nvmf_tcp_recv_capsule_cmd_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg)
{
#if 0
	if (!_spdk_nvmf_tcp_check_hdgst(&tqpair->recv_pdu.capsule, sizeof(tqpair->recv_pdu.capsule)) {
	// TODO
}
#endif

// TODO: command has arrived
// - check header digest (if present)
// - receive in-capsule data (if present)
// - receive
}


static void
_spdk_nvmf_tcp_handle_connect(struct spdk_nvmf_transport *transport,
			      struct spdk_nvmf_tcp_port *port,
			      struct spdk_sock *sock, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_qpair *tqpair;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New connection accepted on %s port %s\n",
		      port->trid.traddr, port->trid.trsvcid);

	tqpair = calloc(1, sizeof(struct spdk_nvmf_tcp_qpair));
	if (tqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_sock_close(&sock);
		return;
	}

	tqpair->sock = sock;
	tqpair->port = port;
	tqpair->max_queue_depth = ttransport->max_queue_depth;
	tqpair->qpair.transport = transport;
	TAILQ_INIT(&tqpair->free_queue);

	_spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TQPAIR_RECV_STATE_AWAIT_ICREQ,
					    &tqpair->recv_pdu.ic_req, sizeof(tqpair->recv_pdu.ic_req),
					    _spdk_nvmf_tcp_recv_icreq_complete, NULL);

	// TODO: add to port's list of qpairs?

	cb_fn(&tqpair->qpair);
}

static void
spdk_nvmf_tcp_port_accept(struct spdk_nvmf_transport *transport, struct spdk_nvmf_tcp_port *port,
			  new_qpair_fn cb_fn)
{
	struct spdk_sock *sock;

	// TODO: limit max number of sockets accepted per call?
	do {
		sock = spdk_sock_accept(port->listen_sock);
		if (sock) {
			_spdk_nvmf_tcp_handle_connect(transport, port, sock, cb_fn);
		}
	} while (sock);
}

static void
spdk_nvmf_tcp_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		spdk_nvmf_tcp_port_accept(transport, port, cb_fn);
	}
}

static void
spdk_nvmf_tcp_discover(struct spdk_nvmf_transport *transport,
		       struct spdk_nvme_transport_id *trid,
		       struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_TCP;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.tcp.tcp_secsoc = SPDK_NVME_TCP_SECURITY_NONE;
	entry->tsas.tcp.tcp_clist = 0;
	memset(entry->tsas.tcp.tcp_cipher, 0, sizeof(entry->tsas.tcp.tcp_cipher));
}

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_tcp_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;

	tgroup = calloc(1, sizeof(*tgroup));
	if (!tgroup) {
		return NULL;
	}

	tgroup->sock_group = spdk_sock_group_create();
	if (!tgroup->sock_group) {
		goto cleanup;
	}

	// TODO: init stuff
	// TODO: add to list in ttransport?

	return &tgroup->group;

cleanup:
	free(tgroup);
	return NULL;
}

static void
spdk_nvmf_tcp_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	spdk_sock_group_close(&tgroup->sock_group);
	free(tgroup);
}


static void
spdk_nvmf_tcp_qpair_send(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_send_data *send_data, *next;
	ssize_t rc;
	struct iovec iovs[32];
	struct iovec *iov;
	int iovcnt, iovcnt_max;

	/* Build up a list of buffers to send in iovs */
	iovcnt = 0;
	iovcnt_max = SPDK_COUNTOF(iovs);
	STAILQ_FOREACH(send_data, &tqpair->send_queue, link) {
		if (iovcnt == iovcnt_max) {
			break;
		}

		iov = &iovs[iovcnt++];
		iov->iov_base = send_data->buf;
		iov->iov_len = send_data->remaining;
	}

	rc = spdk_sock_writev(tqpair->sock, iovs, iovcnt);
	if (spdk_unlikely(rc < 0)) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return;
		}

		SPDK_ERRLOG("spdk_sock_writev() failed, errno %d: %s\n",
			    errno, spdk_strerror(errno));
		// TODO: close conn
		return;
	}

	/* Account for the data that was sent (could be less than requested) */
	send_data = STAILQ_FIRST(&tqpair->send_queue);
	while (rc > 0) {
		spdk_nvmf_tcp_qpair_xfer_complete_cb complete_cb;
		void *cb_arg;

		assert(send_data != NULL);

		if (rc < send_data->remaining) {
			/* Partial write */
			send_data->remaining -= rc;
			rc = 0;
			break;
		}

		/* This send_data has been fully written */
		assert(rc >= send_data->remaining);
		rc -= send_data->remaining;
		send_data->remaining = 0;

		complete_cb = send_data->complete_cb;
		cb_arg = send_data->cb_arg;
		next = STAILQ_NEXT(send_data, link);
		assert(send_data == STAILQ_FIRST(&tqpair->send_queue));
		STAILQ_REMOVE_HEAD(&tqpair->send_queue, link);
		STAILQ_INSERT_HEAD(&tqpair->send_queue_free, send_data, link);
		send_data = next;

		complete_cb(tqpair, cb_arg);
	}
	assert(rc == 0);
}

static void
spdk_nvmf_tcp_qpair_recv(struct spdk_nvmf_tcp_qpair *tqpair)
{
	ssize_t rc;

recv_again:
	assert(tqpair->recv_buf != NULL);
	assert(tqpair->recv_remaining > 0);
	rc = spdk_sock_recv(tqpair->sock, tqpair->recv_buf + tqpair->recv_offset,
			    tqpair->recv_remaining);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "recv_offset = %u recv_remaining = %u rc = %zd\n",
		      tqpair->recv_offset, tqpair->recv_remaining, rc);

	if (spdk_likely(rc > 0)) {
		tqpair->recv_offset += rc;
		tqpair->recv_remaining -= rc;

		if (tqpair->recv_remaining == 0) {
			tqpair->recv_complete_cb(tqpair, tqpair->recv_complete_cb_arg);

			// TODO: assert that state changed?

			goto recv_again;
		}

	} else if (rc == 0) {
		/* Connection closed by host */
		// TODO
	} else {
		/* Error */
		// TODO - check for EAGAIN and EWOULDBLOCK
	}
}

static void
spdk_nvmf_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;

	spdk_nvmf_tcp_qpair_recv(tqpair);
}

static int
spdk_nvmf_tcp_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			     struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	rc = spdk_sock_group_add_sock(tgroup->sock_group, tqpair->sock,
				      spdk_nvmf_tcp_sock_cb, tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Could not add sock to sock_group: %s (%d)\n",
			    spdk_strerror(errno), errno);
		// TODO: cleanup? - need to reject the connection here
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_tcp_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				struct spdk_nvmf_qpair *qpair)
{
#if 0
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair		*tqpair;
	struct spdk_nvmf_tcp_device		*device;
	struct spdk_nvmf_tcp_poller		*poller;
	struct spdk_nvmf_tcp_qpair		*rq, *trq;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	device = tqpair->port->device;

	TAILQ_FOREACH(poller, &tgroup->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		SPDK_ERRLOG("No poller found for device.\n");
		return -1;
	}

	TAILQ_FOREACH_SAFE(rq, &poller->qpairs, link, trq) {
		if (rq == tqpair) {
			TAILQ_REMOVE(&poller->qpairs, tqpair, link);
			break;
		}
	}

	if (rq == NULL) {
		SPDK_ERRLOG("TCP qpair cannot be removed from group (not in group).\n");
		return -1;
	}
#endif

	return 0;
}

static int
spdk_nvmf_tcp_request_complete(struct spdk_nvmf_request *req)
{
#if 0
	struct spdk_nvmf_tcp_transport	*ttransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_tcp_transport, transport);
	struct spdk_nvmf_tcp_request	*tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);

	tcp_req->state = TCP_REQUEST_STATE_EXECUTED;
	spdk_nvmf_tcp_request_process(ttransport, tcp_req);
#endif
	return 0;
}

static void
spdk_nvmf_tcp_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	spdk_nvmf_tcp_qpair_destroy(SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair));
}

#if 0
static void
spdk_nvmf_tcp_qpair_process_pending(struct spdk_nvmf_tcp_transport *ttransport,
				    struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_recv	*tcp_recv, *recv_tmp;
	struct spdk_nvmf_tcp_request	*tcp_req, *req_tmp;

	/* We process I/O in the pending_tcp_rw queue at the highest priority. */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->pending_tcp_rw_queue, link, req_tmp) {
		if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
			break;
		}
	}

	/* The second highest priority is I/O waiting on memory buffers. */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->ch->pending_data_buf_queue, link, req_tmp) {
		if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
			break;
		}
	}

	/* The lowest priority is processing newly received commands */
	TAILQ_FOREACH_SAFE(tcp_recv, &tqpair->incoming_queue, link, recv_tmp) {
		tcp_req = TAILQ_FIRST(&tqpair->free_queue);
		if (tcp_req == NULL) {
			/* Need to wait for more SEND completions */
			break;
		}

		tcp_req->recv = tcp_recv;
		tcp_req->state = TCP_REQUEST_STATE_NEW;
		if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
			break;
		}
	}
}
#endif


#if 0
static int
spdk_nvmf_tcp_poller_poll(struct spdk_nvmf_tcp_transport *ttransport,
			  struct spdk_nvmf_tcp_poller *rpoller)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_tcp_request	*tcp_req;
	struct spdk_nvmf_tcp_recv	*tcp_recv;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int reaped, i;
	int count = 0;
	bool error = false;

	/* Poll for completing operations. */
	reaped = ibv_poll_cq(rpoller->cq, 32, wc);
	if (reaped < 0) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, spdk_strerror(errno));
		return -1;
	}

	for (i = 0; i < reaped; i++) {
		if (wc[i].status) {
			SPDK_ERRLOG("CQ error on CQ %p, Request 0x%lu (%d): %s\n",
				    rpoller->cq, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));
			error = true;
			continue;
		}

		switch (wc[i].opcode) {
		case IBV_WC_SEND:
			tcp_req = get_tcp_req_from_wc(&wc[i]);
			tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

			assert(tcp_req->state == TCP_REQUEST_STATE_COMPLETING);
			tcp_req->state = TCP_REQUEST_STATE_COMPLETED;

			spdk_nvmf_tcp_request_process(ttransport, tcp_req);

			count++;

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
			break;

		case IBV_WC_TCP_WRITE:
			tcp_req = get_tcp_req_from_wc(&wc[i]);
			tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

			tqpair->cur_tcp_rw_depth--;

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
			break;

		case IBV_WC_TCP_READ:
			tcp_req = get_tcp_req_from_wc(&wc[i]);
			tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

			assert(tcp_req->state == TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
			tqpair->cur_tcp_rw_depth--;
			tcp_req->state = TCP_REQUEST_STATE_READY_TO_EXECUTE;

			spdk_nvmf_tcp_request_process(ttransport, tcp_req);

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
			break;

		case IBV_WC_RECV:
			tcp_recv = get_tcp_recv_from_wc(&wc[i]);
			tqpair = tcp_recv->qpair;

			TAILQ_INSERT_TAIL(&tqpair->incoming_queue, tcp_recv, link);

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
			break;

		default:
			SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
			continue;
		}
	}

	if (error == true) {
		return -1;
	}

	return count;
}
#endif

static int
spdk_nvmf_tcp_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;
	struct spdk_nvmf_tcp_qpair *tqpair;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);

	TAILQ_FOREACH(tqpair, &tgroup->qpairs, link) {
		spdk_nvmf_tcp_qpair_send(tqpair);
	}

	return spdk_sock_group_poll(tgroup->sock_group);
}

static bool
spdk_nvmf_tcp_qpair_is_idle(struct spdk_nvmf_qpair *qpair)
{
	// TODO: this callback should probably be removed
	return false;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp = {
	.type = SPDK_NVME_TRANSPORT_TCP,
	.create = spdk_nvmf_tcp_create,
	.destroy = spdk_nvmf_tcp_destroy,

	.listen = spdk_nvmf_tcp_listen,
	.stop_listen = spdk_nvmf_tcp_stop_listen,
	.accept = spdk_nvmf_tcp_accept,

	.listener_discover = spdk_nvmf_tcp_discover,

	.poll_group_create = spdk_nvmf_tcp_poll_group_create,
	.poll_group_destroy = spdk_nvmf_tcp_poll_group_destroy,
	.poll_group_add = spdk_nvmf_tcp_poll_group_add,
	.poll_group_remove = spdk_nvmf_tcp_poll_group_remove,
	.poll_group_poll = spdk_nvmf_tcp_poll_group_poll,

	.req_complete = spdk_nvmf_tcp_request_complete,

	.qpair_fini = spdk_nvmf_tcp_close_qpair,
	.qpair_is_idle = spdk_nvmf_tcp_qpair_is_idle,
};

SPDK_LOG_REGISTER_COMPONENT("nvmf_tcp", SPDK_LOG_NVMF_TCP)
