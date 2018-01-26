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

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/assert.h"
#include "spdk/io_channel.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/*
 RDMA Connection Resouce Defaults
 */
#define NVMF_DEFAULT_TX_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

/* The RDMA completion queue size */
#define NVMF_RDMA_CQ_SIZE	4096

/* AIO backend requires block size aligned data buffers,
 * extra 4KiB aligned data buffer should work for most devices.
 */
#define SHIFT_4KB			12
#define NVMF_DATA_BUFFER_ALIGNMENT	(1 << SHIFT_4KB)
#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1)

enum spdk_nvmf_rdma_request_state {
	/* The request is not currently in use */
	RDMA_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	RDMA_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	RDMA_REQUEST_STATE_NEED_BUFFER,

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the host to the controller.
	 */
	RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER,

	/* The request is currently transferring data from the host to the controller. */
	RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	RDMA_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	RDMA_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	RDMA_REQUEST_STATE_EXECUTED,

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data from the controller to the host.
	 */
	RDMA_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST,

	/* The request is ready to send a completion */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request currently has a completion outstanding */
	RDMA_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	RDMA_REQUEST_STATE_COMPLETED,
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_rdma_recv {
	struct ibv_recv_wr		wr;
	struct ibv_sge			sgl[NVMF_DEFAULT_RX_SGE];

	struct spdk_nvmf_rdma_qpair	*qpair;

	/* In-capsule data buffer */
	uint8_t				*buf;

	TAILQ_ENTRY(spdk_nvmf_rdma_recv) link;
};

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;
	void					*data_from_pool;

	enum spdk_nvmf_rdma_request_state	state;

	struct spdk_nvmf_rdma_recv		*recv;

	struct {
		struct	ibv_send_wr		wr;
		struct	ibv_sge			sgl[NVMF_DEFAULT_TX_SGE];
	} rsp;

	struct {
		struct ibv_send_wr		wr;
		struct ibv_sge			sgl[NVMF_DEFAULT_TX_SGE];
	} data;

	TAILQ_ENTRY(spdk_nvmf_rdma_request)	link;
};

struct spdk_nvmf_rdma_qpair {
	struct spdk_nvmf_qpair			qpair;

	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_poller		*poller;

	struct rdma_cm_id			*cm_id;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* The maximum number of active RDMA READ and WRITE operations at one time */
	uint16_t				max_rw_depth;

	/* The current number of I/O outstanding on this connection. This number
	 * includes all I/O from the time the capsule is first received until it is
	 * completed.
	 */
	uint16_t				cur_queue_depth;

	/* The number of RDMA READ and WRITE requests that are outstanding */
	uint16_t				cur_rdma_rw_depth;

	/* Receives that are waiting for a request object */
	TAILQ_HEAD(, spdk_nvmf_rdma_recv)	incoming_queue;

	/* Requests that are not in use */
	TAILQ_HEAD(, spdk_nvmf_rdma_request)	free_queue;

	/* Requests that are waiting to perform an RDMA READ or WRITE */
	TAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_rw_queue;

	/* Array of size "max_queue_depth" containing RDMA requests. */
	struct spdk_nvmf_rdma_request		*reqs;

	/* Array of size "max_queue_depth" containing RDMA recvs. */
	struct spdk_nvmf_rdma_recv		*recvs;

	/* Array of size "max_queue_depth" containing 64 byte capsules
	 * used for receive.
	 */
	union nvmf_h2c_msg			*cmds;
	struct ibv_mr				*cmds_mr;

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	union nvmf_c2h_msg			*cpls;
	struct ibv_mr				*cpls_mr;

	/* Array of size "max_queue_depth * InCapsuleDataSize" containing
	 * buffers to be used for in capsule data.
	 */
	void					*bufs;
	struct ibv_mr				*bufs_mr;

	TAILQ_ENTRY(spdk_nvmf_rdma_qpair)	link;
	TAILQ_ENTRY(spdk_nvmf_rdma_qpair)	pending_link;

	/* Mgmt channel */
	struct spdk_io_channel			*mgmt_channel;
	struct spdk_nvmf_rdma_mgmt_channel	*ch;
};

struct spdk_nvmf_rdma_poller {
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poll_group	*group;

	struct ibv_cq				*cq;

	TAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs;

	TAILQ_ENTRY(spdk_nvmf_rdma_poller)	link;
};

struct spdk_nvmf_rdma_poll_group {
	struct spdk_nvmf_transport_poll_group	group;

	TAILQ_HEAD(, spdk_nvmf_rdma_poller)	pollers;
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
struct spdk_nvmf_rdma_device {
	struct ibv_device_attr			attr;
	struct ibv_context			*context;

	struct spdk_mem_map			*map;
	struct ibv_pd				*pd;

	TAILQ_ENTRY(spdk_nvmf_rdma_device)	link;
};

struct spdk_nvmf_rdma_port {
	struct spdk_nvme_transport_id		trid;
	struct rdma_cm_id			*id;
	struct spdk_nvmf_rdma_device		*device;
	uint32_t				ref;
	TAILQ_ENTRY(spdk_nvmf_rdma_port)	link;
};

struct spdk_nvmf_rdma_transport {
	struct spdk_nvmf_transport	transport;

	struct rdma_event_channel	*event_channel;

	struct spdk_mempool		*data_buf_pool;

	pthread_mutex_t 		lock;

	uint16_t 			max_queue_depth;
	uint32_t 			max_io_size;
	uint32_t 			in_capsule_data_size;

	TAILQ_HEAD(, spdk_nvmf_rdma_device)	devices;
	TAILQ_HEAD(, spdk_nvmf_rdma_port)	ports;
};

struct spdk_nvmf_rdma_mgmt_channel {
	/* Requests that are waiting to obtain a data buffer */
	TAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_data_buf_queue;
};

static int
spdk_nvmf_rdma_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_rdma_mgmt_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->pending_data_buf_queue);
	return 0;
}

static int
spdk_nvmf_rdma_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_rdma_mgmt_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->pending_data_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on channel destruction\n");
	}

	return 0;
}

static void
spdk_nvmf_rdma_qpair_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (rqpair->poller) {
		TAILQ_REMOVE(&rqpair->poller->qpairs, rqpair, link);
	}

	if (rqpair->cmds_mr) {
		ibv_dereg_mr(rqpair->cmds_mr);
	}

	if (rqpair->cpls_mr) {
		ibv_dereg_mr(rqpair->cpls_mr);
	}

	if (rqpair->bufs_mr) {
		ibv_dereg_mr(rqpair->bufs_mr);
	}

	if (rqpair->cm_id) {
		rdma_destroy_qp(rqpair->cm_id);
		rdma_destroy_id(rqpair->cm_id);
	}

	if (rqpair->mgmt_channel) {
		spdk_put_io_channel(rqpair->mgmt_channel);
	}

	/* Free all memory */
	spdk_dma_free(rqpair->cmds);
	spdk_dma_free(rqpair->cpls);
	spdk_dma_free(rqpair->bufs);
	free(rqpair->reqs);
	free(rqpair->recvs);
	free(rqpair);
}

static int
spdk_nvmf_rdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	int				rc, i;
	struct ibv_qp_init_attr		attr;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_request	*rdma_req;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.send_cq		= rqpair->poller->cq;
	attr.recv_cq		= rqpair->poller->cq;
	attr.cap.max_send_wr	= rqpair->max_queue_depth * 2; /* SEND, READ, and WRITE operations */
	attr.cap.max_recv_wr	= rqpair->max_queue_depth; /* RECV operations */
	attr.cap.max_send_sge	= NVMF_DEFAULT_TX_SGE;
	attr.cap.max_recv_sge	= NVMF_DEFAULT_RX_SGE;

	rc = rdma_create_qp(rqpair->cm_id, NULL, &attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed: errno %d: %s\n", errno, spdk_strerror(errno));
		rdma_destroy_id(rqpair->cm_id);
		rqpair->cm_id = NULL;
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "New RDMA Connection: %p\n", qpair);

	rqpair->reqs = calloc(rqpair->max_queue_depth, sizeof(*rqpair->reqs));
	rqpair->recvs = calloc(rqpair->max_queue_depth, sizeof(*rqpair->recvs));
	rqpair->cmds = spdk_dma_zmalloc(rqpair->max_queue_depth * sizeof(*rqpair->cmds),
					0x1000, NULL);
	rqpair->cpls = spdk_dma_zmalloc(rqpair->max_queue_depth * sizeof(*rqpair->cpls),
					0x1000, NULL);
	rqpair->bufs = spdk_dma_zmalloc(rqpair->max_queue_depth * rtransport->in_capsule_data_size,
					0x1000, NULL);
	if (!rqpair->reqs || !rqpair->recvs || !rqpair->cmds ||
	    !rqpair->cpls || !rqpair->bufs) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}

	rqpair->cmds_mr = ibv_reg_mr(rqpair->cm_id->pd, rqpair->cmds,
				     rqpair->max_queue_depth * sizeof(*rqpair->cmds),
				     IBV_ACCESS_LOCAL_WRITE);
	rqpair->cpls_mr = ibv_reg_mr(rqpair->cm_id->pd, rqpair->cpls,
				     rqpair->max_queue_depth * sizeof(*rqpair->cpls),
				     0);
	rqpair->bufs_mr = ibv_reg_mr(rqpair->cm_id->pd, rqpair->bufs,
				     rqpair->max_queue_depth * rtransport->in_capsule_data_size,
				     IBV_ACCESS_LOCAL_WRITE |
				     IBV_ACCESS_REMOTE_WRITE);
	if (!rqpair->cmds_mr || !rqpair->cpls_mr || !rqpair->bufs_mr) {
		SPDK_ERRLOG("Unable to register required memory for RDMA queue.\n");
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Command Array: %p Length: %lx LKey: %x\n",
		      rqpair->cmds, rqpair->max_queue_depth * sizeof(*rqpair->cmds), rqpair->cmds_mr->lkey);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Completion Array: %p Length: %lx LKey: %x\n",
		      rqpair->cpls, rqpair->max_queue_depth * sizeof(*rqpair->cpls), rqpair->cpls_mr->lkey);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "In Capsule Data Array: %p Length: %x LKey: %x\n",
		      rqpair->bufs, rqpair->max_queue_depth * rtransport->in_capsule_data_size, rqpair->bufs_mr->lkey);

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		struct ibv_recv_wr *bad_wr = NULL;

		rdma_recv = &rqpair->recvs[i];
		rdma_recv->qpair = rqpair;

		/* Set up memory to receive commands */
		rdma_recv->buf = (void *)((uintptr_t)rqpair->bufs + (i * rtransport->in_capsule_data_size));

		rdma_recv->sgl[0].addr = (uintptr_t)&rqpair->cmds[i];
		rdma_recv->sgl[0].length = sizeof(rqpair->cmds[i]);
		rdma_recv->sgl[0].lkey = rqpair->cmds_mr->lkey;

		rdma_recv->sgl[1].addr = (uintptr_t)rdma_recv->buf;
		rdma_recv->sgl[1].length = rtransport->in_capsule_data_size;
		rdma_recv->sgl[1].lkey = rqpair->bufs_mr->lkey;

		rdma_recv->wr.wr_id = (uintptr_t)rdma_recv;
		rdma_recv->wr.sg_list = rdma_recv->sgl;
		rdma_recv->wr.num_sge = SPDK_COUNTOF(rdma_recv->sgl);

		rc = ibv_post_recv(rqpair->cm_id->qp, &rdma_recv->wr, &bad_wr);
		if (rc) {
			SPDK_ERRLOG("Unable to post capsule for RDMA RECV\n");
			spdk_nvmf_rdma_qpair_destroy(rqpair);
			return -1;
		}
	}

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		rdma_req = &rqpair->reqs[i];

		rdma_req->req.qpair = &rqpair->qpair;
		rdma_req->req.cmd = NULL;

		/* Set up memory to send responses */
		rdma_req->req.rsp = &rqpair->cpls[i];

		rdma_req->rsp.sgl[0].addr = (uintptr_t)&rqpair->cpls[i];
		rdma_req->rsp.sgl[0].length = sizeof(rqpair->cpls[i]);
		rdma_req->rsp.sgl[0].lkey = rqpair->cpls_mr->lkey;

		rdma_req->rsp.wr.wr_id = (uintptr_t)rdma_req;
		rdma_req->rsp.wr.next = NULL;
		rdma_req->rsp.wr.opcode = IBV_WR_SEND;
		rdma_req->rsp.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->rsp.wr.sg_list = rdma_req->rsp.sgl;
		rdma_req->rsp.wr.num_sge = SPDK_COUNTOF(rdma_req->rsp.sgl);

		/* Set up memory for data buffers */
		rdma_req->data.wr.wr_id = (uint64_t)rdma_req;
		rdma_req->data.wr.next = NULL;
		rdma_req->data.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->data.wr.sg_list = rdma_req->data.sgl;
		rdma_req->data.wr.num_sge = SPDK_COUNTOF(rdma_req->data.sgl);

		TAILQ_INSERT_TAIL(&rqpair->free_queue, rdma_req, link);
	}

	return 0;
}

static int
request_transfer_in(struct spdk_nvmf_request *req)
{
	int				rc;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair 		*qpair;
	struct spdk_nvmf_rdma_qpair 	*rqpair;
	struct ibv_send_wr		*bad_wr = NULL;

	qpair = req->qpair;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);

	rqpair->cur_rdma_rw_depth++;

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA READ POSTED. Request: %p Connection: %p\n", req, qpair);
	spdk_trace_record(TRACE_RDMA_READ_START, 0, 0, (uintptr_t)req, 0);

	rdma_req->data.wr.opcode = IBV_WR_RDMA_READ;
	rdma_req->data.wr.next = NULL;
	rc = ibv_post_send(rqpair->cm_id->qp, &rdma_req->data.wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to transfer data from host to target\n");

		/* Decrement r/w counter back since data transfer
		 * has not started.
		 */
		rqpair->cur_rdma_rw_depth--;
		return -1;
	}

	return 0;
}

static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	int 				rc;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair 	*rqpair;
	struct spdk_nvme_cpl		*rsp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	struct ibv_send_wr		*send_wr, *bad_send_wr = NULL;

	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	/* Post the capsule to the recv buffer */
	assert(rdma_req->recv != NULL);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA RECV POSTED. Recv: %p Connection: %p\n", rdma_req->recv,
		      rqpair);
	rc = ibv_post_recv(rqpair->cm_id->qp, &rdma_req->recv->wr, &bad_recv_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return rc;
	}
	rdma_req->recv = NULL;

	/* Build the response which consists of an optional
	 * RDMA WRITE to transfer data, plus an RDMA SEND
	 * containing the response.
	 */
	send_wr = &rdma_req->rsp.wr;

	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA WRITE POSTED. Request: %p Connection: %p\n", req, qpair);
		spdk_trace_record(TRACE_RDMA_WRITE_START, 0, 0, (uintptr_t)req, 0);

		rqpair->cur_rdma_rw_depth++;
		rdma_req->data.wr.opcode = IBV_WR_RDMA_WRITE;

		rdma_req->data.wr.next = send_wr;
		send_wr = &rdma_req->data.wr;
	}

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA SEND POSTED. Request: %p Connection: %p\n", req, qpair);
	spdk_trace_record(TRACE_NVMF_IO_COMPLETE, 0, 0, (uintptr_t)req, 0);

	/* Send the completion */
	rc = ibv_post_send(rqpair->cm_id->qp, send_wr, &bad_send_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to send response capsule\n");

		if (rdma_req->data.wr.opcode == IBV_WR_RDMA_WRITE) {
			/* Decrement r/w counter back since data transfer
			 * has not started.
			 */
			rqpair->cur_rdma_rw_depth--;
		}
	}

	return rc;
}

static int
spdk_nvmf_rdma_event_accept(struct rdma_cm_id *id, struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_accept_private_data	accept_data;
	struct rdma_conn_param				ctrlr_event_data = {};
	int						rc;

	accept_data.recfmt = 0;
	accept_data.crqsize = rqpair->max_queue_depth;

	ctrlr_event_data.private_data = &accept_data;
	ctrlr_event_data.private_data_len = sizeof(accept_data);
	if (id->ps == RDMA_PS_TCP) {
		ctrlr_event_data.responder_resources = 0; /* We accept 0 reads from the host */
		ctrlr_event_data.initiator_depth = rqpair->max_rw_depth;
	}

	rc = rdma_accept(id, &ctrlr_event_data);
	if (rc) {
		SPDK_ERRLOG("Error %d on rdma_accept\n", errno);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Sent back the accept\n");
	}

	return rc;
}

static void
spdk_nvmf_rdma_event_reject(struct rdma_cm_id *id, enum spdk_nvmf_rdma_transport_error error)
{
	struct spdk_nvmf_rdma_reject_private_data	rej_data;

	rej_data.recfmt = 0;
	rej_data.sts = error;

	rdma_reject(id, &rej_data, sizeof(rej_data));
}

static int
nvmf_rdma_connect(struct spdk_nvmf_transport *transport, struct rdma_cm_event *event,
		  new_qpair_fn cb_fn)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct spdk_nvmf_rdma_port 	*port;
	struct rdma_conn_param		*rdma_param = NULL;
	const struct spdk_nvmf_rdma_request_private_data *private_data = NULL;
	uint16_t			max_queue_depth;
	uint16_t			max_rw_depth;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	assert(event->id != NULL); /* Impossible. Can't even reject the connection. */
	assert(event->id->verbs != NULL); /* Impossible. No way to handle this. */

	rdma_param = &event->param.conn;
	if (rdma_param->private_data == NULL ||
	    rdma_param->private_data_len < sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		SPDK_ERRLOG("connect request: no private data provided\n");
		spdk_nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH);
		return -1;
	}

	private_data = rdma_param->private_data;
	if (private_data->recfmt != 0) {
		SPDK_ERRLOG("Received RDMA private data with RECFMT != 0\n");
		spdk_nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT);
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Connect Recv on fabric intf name %s, dev_name %s\n",
		      event->id->verbs->device->name, event->id->verbs->device->dev_name);

	port = event->listen_id->context;
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Listen Id was %p with verbs %p. ListenAddr: %p\n",
		      event->listen_id, event->listen_id->verbs, port);

	/* Figure out the supported queue depth. This is a multi-step process
	 * that takes into account hardware maximums, host provided values,
	 * and our target's internal memory limits */

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Calculating Queue Depth\n");

	/* Start with the maximum queue depth allowed by the target */
	max_queue_depth = rtransport->max_queue_depth;
	max_rw_depth = rtransport->max_queue_depth;
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Target Max Queue Depth: %d\n", rtransport->max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_DEBUGLOG(SPDK_LOG_RDMA,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      port->device->attr.max_qp_wr, port->device->attr.max_qp_rd_atom);
	max_queue_depth = spdk_min(max_queue_depth, port->device->attr.max_qp_wr);
	max_rw_depth = spdk_min(max_rw_depth, port->device->attr.max_qp_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	SPDK_DEBUGLOG(SPDK_LOG_RDMA,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	if (rdma_param->initiator_depth > 0) {
		max_rw_depth = spdk_min(max_rw_depth, rdma_param->initiator_depth);
	}

	/* Finally check for the host software requested values, which are
	 * optional. */
	if (rdma_param->private_data != NULL &&
	    rdma_param->private_data_len >= sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Host Receive Queue Size: %d\n", private_data->hrqsize);
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Host Send Queue Size: %d\n", private_data->hsqsize);
		max_queue_depth = spdk_min(max_queue_depth, private_data->hrqsize);
		max_queue_depth = spdk_min(max_queue_depth, private_data->hsqsize + 1);
	}

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Final Negotiated Queue Depth: %d R/W Depth: %d\n",
		      max_queue_depth, max_rw_depth);

	rqpair = calloc(1, sizeof(struct spdk_nvmf_rdma_qpair));
	if (rqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		return -1;
	}

	rqpair->port = port;
	rqpair->max_queue_depth = max_queue_depth;
	rqpair->max_rw_depth = max_rw_depth;
	rqpair->cm_id = event->id;
	rqpair->qpair.transport = transport;
	TAILQ_INIT(&rqpair->incoming_queue);
	TAILQ_INIT(&rqpair->free_queue);
	TAILQ_INIT(&rqpair->pending_rdma_rw_queue);

	event->id->context = &rqpair->qpair;

	cb_fn(&rqpair->qpair);

	return 0;
}

static void
nvmf_rdma_handle_disconnect(void *ctx)
{
	struct spdk_nvmf_qpair 		*qpair = ctx;
	struct spdk_nvmf_ctrlr		*ctrlr;
	struct spdk_nvmf_rdma_qpair 	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	ctrlr = qpair->ctrlr;
	if (ctrlr == NULL) {
		/* No ctrlr has been established yet, so destroy
		 * the connection.
		 */
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return;
	}

	spdk_nvmf_ctrlr_disconnect(qpair);
}

static int
nvmf_rdma_disconnect(struct rdma_cm_event *evt)
{
	struct spdk_nvmf_qpair	*qpair;
	struct spdk_io_channel 	*ch;

	if (evt->id == NULL) {
		SPDK_ERRLOG("disconnect request: missing cm_id\n");
		return -1;
	}

	qpair = evt->id->context;
	if (qpair == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}
	/* ack the disconnect event before rdma_destroy_id */
	rdma_ack_cm_event(evt);

	ch = spdk_io_channel_from_ctx(qpair->group);
	spdk_thread_send_msg(spdk_io_channel_get_thread(ch), nvmf_rdma_handle_disconnect, qpair);

	return 0;
}

#ifdef DEBUG
static const char *CM_EVENT_STR[] = {
	"RDMA_CM_EVENT_ADDR_RESOLVED",
	"RDMA_CM_EVENT_ADDR_ERROR",
	"RDMA_CM_EVENT_ROUTE_RESOLVED",
	"RDMA_CM_EVENT_ROUTE_ERROR",
	"RDMA_CM_EVENT_CONNECT_REQUEST",
	"RDMA_CM_EVENT_CONNECT_RESPONSE",
	"RDMA_CM_EVENT_CONNECT_ERROR",
	"RDMA_CM_EVENT_UNREACHABLE",
	"RDMA_CM_EVENT_REJECTED",
	"RDMA_CM_EVENT_ESTABLISHED",
	"RDMA_CM_EVENT_DISCONNECTED",
	"RDMA_CM_EVENT_DEVICE_REMOVAL",
	"RDMA_CM_EVENT_MULTICAST_JOIN",
	"RDMA_CM_EVENT_MULTICAST_ERROR",
	"RDMA_CM_EVENT_ADDR_CHANGE",
	"RDMA_CM_EVENT_TIMEWAIT_EXIT"
};
#endif /* DEBUG */

static int
spdk_nvmf_rdma_mem_notify(void *cb_ctx, struct spdk_mem_map *map,
			  enum spdk_mem_map_notify_action action,
			  void *vaddr, size_t size)
{
	struct spdk_nvmf_rdma_device *device = cb_ctx;
	struct ibv_pd *pd = device->pd;
	struct ibv_mr *mr;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		mr = ibv_reg_mr(pd, vaddr, size,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
		if (mr == NULL) {
			SPDK_ERRLOG("ibv_reg_mr() failed\n");
			return -1;
		} else {
			spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)mr);
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)vaddr);
		spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);
		if (mr) {
			ibv_dereg_mr(mr);
		}
		break;
	}

	return 0;
}

typedef enum spdk_nvme_data_transfer spdk_nvme_data_transfer_t;

static spdk_nvme_data_transfer_t
spdk_nvmf_rdma_request_get_xfer(struct spdk_nvmf_rdma_request *rdma_req)
{
	enum spdk_nvme_data_transfer xfer;
	struct spdk_nvme_cmd *cmd = &rdma_req->req.cmd->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;

	/* Figure out data transfer direction */
	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		xfer = spdk_nvme_opc_get_data_transfer(rdma_req->req.cmd->nvmf_cmd.fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

		/* Some admin commands are special cases */
		if ((rdma_req->req.qpair->qid == 0) &&
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
spdk_nvmf_rdma_request_parse_sgl(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_device *device,
				 struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;

	cmd = &rdma_req->req.cmd->nvme_cmd;
	rsp = &rdma_req->req.rsp->nvme_cpl;
	sgl = &cmd->dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
		if (sgl->keyed.length > rtransport->max_io_size) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    sgl->keyed.length, rtransport->max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		rdma_req->req.length = sgl->keyed.length;
		rdma_req->data_from_pool = spdk_mempool_get(rtransport->data_buf_pool);
		if (!rdma_req->data_from_pool) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		}
		/* AIO backend requires block size aligned data buffers,
		 * 4KiB aligned data buffer should work for most devices.
		 */
		rdma_req->req.data = (void *)((uintptr_t)(rdma_req->data_from_pool + NVMF_DATA_BUFFER_MASK)
					      & ~NVMF_DATA_BUFFER_MASK);
		rdma_req->data.sgl[0].addr = (uintptr_t)rdma_req->req.data;
		rdma_req->data.sgl[0].length = sgl->keyed.length;
		rdma_req->data.sgl[0].lkey = ((struct ibv_mr *)spdk_mem_map_translate(device->map,
					      (uint64_t)rdma_req->req.data))->lkey;
		rdma_req->data.wr.wr.rdma.rkey = sgl->keyed.key;
		rdma_req->data.wr.wr.rdma.remote_addr = sgl->address;

		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Request %p took buffer from central pool\n", rdma_req);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = rtransport->in_capsule_data_size;

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

		rdma_req->req.data = rdma_req->recv->buf + offset;
		rdma_req->data_from_pool = NULL;
		rdma_req->req.length = sgl->unkeyed.length;
		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static bool
spdk_nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
			       struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvme_cpl		*rsp = &rdma_req->req.rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	enum spdk_nvmf_rdma_request_state prev_state;
	bool				progress = false;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->port->device;

	assert(rdma_req->state != RDMA_REQUEST_STATE_FREE);

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = rdma_req->state;

		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Request %p entering state %d\n", rdma_req, prev_state);

		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_NEW:
			rqpair->cur_queue_depth++;
			rdma_recv = rdma_req->recv;

			/* The first element of the SGL is the NVMe command */
			rdma_req->req.cmd = (union nvmf_h2c_msg *)rdma_recv->sgl[0].addr;
			memset(rdma_req->req.rsp, 0, sizeof(*rdma_req->req.rsp));

			TAILQ_REMOVE(&rqpair->incoming_queue, rdma_recv, link);
			TAILQ_REMOVE(&rqpair->free_queue, rdma_req, link);

			/* The next state transition depends on the data transfer needs of this request. */
			rdma_req->req.xfer = spdk_nvmf_rdma_request_get_xfer(rdma_req);

			/* If no data to transfer, ready to execute. */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_NONE) {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_NEED_BUFFER;
			TAILQ_INSERT_TAIL(&rqpair->ch->pending_data_buf_queue, rdma_req, link);
			break;
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			assert(rdma_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (rdma_req != TAILQ_FIRST(&rqpair->ch->pending_data_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = spdk_nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
			if (rc < 0) {
				TAILQ_REMOVE(&rqpair->ch->pending_data_buf_queue, rdma_req, link);
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
				break;
			}

			if (!rdma_req->req.data) {
				/* No buffers available. */
				break;
			}

			TAILQ_REMOVE(&rqpair->ch->pending_data_buf_queue, rdma_req, link);

			/* If data is transferring from host to controller and the data didn't
			 * arrive using in capsule data, we need to do a transfer from the host.
			 */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER && rdma_req->data_from_pool != NULL) {
				rdma_req->state = RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER;
				TAILQ_INSERT_TAIL(&rqpair->pending_rdma_rw_queue, rdma_req, link);
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
			break;
		case RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER:
			if (rdma_req != TAILQ_FIRST(&rqpair->pending_rdma_rw_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}

			if (rqpair->cur_rdma_rw_depth < rqpair->max_rw_depth) {
				TAILQ_REMOVE(&rqpair->pending_rdma_rw_queue, rdma_req, link);
				rdma_req->state = RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;
				rc = request_transfer_in(&rdma_req->req);
				if (rc) {
					rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
				}
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_READY_TO_EXECUTE:
			rdma_req->state = RDMA_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(&rdma_req->req);
			break;
		case RDMA_REQUEST_STATE_EXECUTING:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_EXECUTED:
			if (rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				rdma_req->state = RDMA_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST;
				TAILQ_INSERT_TAIL(&rqpair->pending_rdma_rw_queue, rdma_req, link);
			} else {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST:
			if (rdma_req != TAILQ_FIRST(&rqpair->pending_rdma_rw_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}

			if (rqpair->cur_rdma_rw_depth < rqpair->max_rw_depth) {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
				TAILQ_REMOVE(&rqpair->pending_rdma_rw_queue, rdma_req, link);
			}
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE:
			rdma_req->state = RDMA_REQUEST_STATE_COMPLETING;

			rc = request_transfer_out(&rdma_req->req);
			assert(rc == 0); /* No good way to handle this currently */
			break;
		case RDMA_REQUEST_STATE_COMPLETING:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETED:
			assert(rqpair->cur_queue_depth > 0);
			rqpair->cur_queue_depth--;

			if (rdma_req->data_from_pool) {
				/* Put the buffer back in the pool */
				spdk_mempool_put(rtransport->data_buf_pool, rdma_req->data_from_pool);
				rdma_req->data_from_pool = NULL;
			}
			rdma_req->req.length = 0;
			rdma_req->req.data = NULL;
			rdma_req->state = RDMA_REQUEST_STATE_FREE;
			TAILQ_INSERT_TAIL(&rqpair->free_queue, rdma_req, link);
			break;
		}

		if (rdma_req->state != prev_state) {
			progress = true;
		}
	} while (rdma_req->state != prev_state);

	return progress;
}

/* Public API callbacks begin here */

static struct spdk_nvmf_transport *
spdk_nvmf_rdma_create(struct spdk_nvmf_tgt *tgt)
{
	int rc;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device	*device, *tmp;
	struct ibv_context		**contexts;
	uint32_t			i;
	int				flag;

	rtransport = calloc(1, sizeof(*rtransport));
	if (!rtransport) {
		return NULL;
	}

	pthread_mutex_init(&rtransport->lock, NULL);
	TAILQ_INIT(&rtransport->devices);
	TAILQ_INIT(&rtransport->ports);

	rtransport->transport.tgt = tgt;
	rtransport->transport.ops = &spdk_nvmf_transport_rdma;

	SPDK_NOTICELOG("*** RDMA Transport Init ***\n");

	rtransport->max_queue_depth = tgt->opts.max_queue_depth;
	rtransport->max_io_size = tgt->opts.max_io_size;
	rtransport->in_capsule_data_size = tgt->opts.in_capsule_data_size;

	rtransport->event_channel = rdma_create_event_channel();
	if (rtransport->event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed, %s\n", spdk_strerror(errno));
		free(rtransport);
		return NULL;
	}

	flag = fcntl(rtransport->event_channel->fd, F_GETFL);
	if (fcntl(rtransport->event_channel->fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%s)\n",
			    rtransport->event_channel->fd, spdk_strerror(errno));
		free(rtransport);
		return NULL;
	}

	rtransport->data_buf_pool = spdk_mempool_create("spdk_nvmf_rdma",
				    rtransport->max_queue_depth * 4, /* The 4 is arbitrarily chosen. Needs to be configurable. */
				    rtransport->max_io_size + NVMF_DATA_BUFFER_ALIGNMENT,
				    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!rtransport->data_buf_pool) {
		SPDK_ERRLOG("Unable to allocate buffer pool for poll group\n");
		free(rtransport);
		return NULL;
	}

	spdk_io_device_register(rtransport, spdk_nvmf_rdma_mgmt_channel_create,
				spdk_nvmf_rdma_mgmt_channel_destroy,
				sizeof(struct spdk_nvmf_rdma_mgmt_channel));

	contexts = rdma_get_devices(NULL);
	i = 0;
	rc = 0;
	while (contexts[i] != NULL) {
		device = calloc(1, sizeof(*device));
		if (!device) {
			SPDK_ERRLOG("Unable to allocate memory for RDMA devices.\n");
			rc = -ENOMEM;
			break;
		}
		device->context = contexts[i];
		rc = ibv_query_device(device->context, &device->attr);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
			free(device);
			break;

		}

		device->pd = NULL;
		device->map = NULL;

		TAILQ_INSERT_TAIL(&rtransport->devices, device, link);
		i++;
	}

	if (rc < 0) {
		TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
			TAILQ_REMOVE(&rtransport->devices, device, link);
			free(device);
		}
		spdk_mempool_free(rtransport->data_buf_pool);
		rdma_destroy_event_channel(rtransport->event_channel);
		free(rtransport);
		rdma_free_devices(contexts);
		return NULL;
	}

	rdma_free_devices(contexts);

	return &rtransport->transport;
}

static int
spdk_nvmf_rdma_destroy(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_port	*port, *port_tmp;
	struct spdk_nvmf_rdma_device	*device, *device_tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, port_tmp) {
		TAILQ_REMOVE(&rtransport->ports, port, link);
		rdma_destroy_id(port->id);
		free(port);
	}

	if (rtransport->event_channel != NULL) {
		rdma_destroy_event_channel(rtransport->event_channel);
	}

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		TAILQ_REMOVE(&rtransport->devices, device, link);
		if (device->map) {
			spdk_mem_map_free(&device->map);
		}
		free(device);
	}

	if (spdk_mempool_count(rtransport->data_buf_pool) != (rtransport->max_queue_depth * 4)) {
		SPDK_ERRLOG("transport buffer pool count is %zu but should be %u\n",
			    spdk_mempool_count(rtransport->data_buf_pool),
			    rtransport->max_queue_depth * 4);
	}

	spdk_mempool_free(rtransport->data_buf_pool);
	spdk_io_device_unregister(rtransport, NULL);
	free(rtransport);

	return 0;
}

static int
spdk_nvmf_rdma_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_port 	*port_tmp, *port;
	struct addrinfo			*res;
	struct addrinfo			hints;
	int				family;
	int				rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	port = calloc(1, sizeof(*port));
	if (!port) {
		return -ENOMEM;
	}

	/* Selectively copy the trid. Things like NQN don't matter here - that
	 * mapping is enforced elsewhere.
	 */
	port->trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	port->trid.adrfam = trid->adrfam;
	snprintf(port->trid.traddr, sizeof(port->trid.traddr), "%s", trid->traddr);
	snprintf(port->trid.trsvcid, sizeof(port->trid.trsvcid), "%s", trid->trsvcid);

	pthread_mutex_lock(&rtransport->lock);
	assert(rtransport->event_channel != NULL);
	TAILQ_FOREACH(port_tmp, &rtransport->ports, link) {
		if (spdk_nvme_transport_id_compare(&port_tmp->trid, &port->trid) == 0) {
			port_tmp->ref++;
			free(port);
			/* Already listening at this address */
			pthread_mutex_unlock(&rtransport->lock);
			return 0;
		}
	}

	rc = rdma_create_id(rtransport->event_channel, &port->id, port, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return rc;
	}

	switch (port->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", port->trid.adrfam);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	rc = getaddrinfo(port->trid.traddr, port->trid.trsvcid, &hints, &res);
	if (rc) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(rc), rc);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return -EINVAL;
	}

	rc = rdma_bind_addr(port->id, res->ai_addr);
	freeaddrinfo(res);

	if (rc < 0) {
		SPDK_ERRLOG("rdma_bind_addr() failed\n");
		rdma_destroy_id(port->id);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return rc;
	}

	rc = rdma_listen(port->id, 10); /* 10 = backlog */
	if (rc < 0) {
		SPDK_ERRLOG("rdma_listen() failed\n");
		rdma_destroy_id(port->id);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return rc;
	}

	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->context == port->id->verbs) {
			port->device = device;
			break;
		}
	}
	if (!port->device) {
		SPDK_ERRLOG("Accepted a connection with verbs %p, but unable to find a corresponding device.\n",
			    port->id->verbs);
		rdma_destroy_id(port->id);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return -EINVAL;
	}

	if (!device->map) {
		device->pd = port->id->pd;
		device->map = spdk_mem_map_alloc(0, spdk_nvmf_rdma_mem_notify, device);
		if (!device->map) {
			SPDK_ERRLOG("Unable to allocate memory map for new poll group\n");
			return -1;
		}
	} else {
		assert(device->pd == port->id->pd);
	}

	SPDK_NOTICELOG("*** NVMf Target Listening on %s port %d ***\n",
		       port->trid.traddr, ntohs(rdma_get_src_port(port->id)));

	port->ref = 1;

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);
	pthread_mutex_unlock(&rtransport->lock);

	return 0;
}

static int
spdk_nvmf_rdma_stop_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *_trid)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_port *port, *tmp;
	struct spdk_nvme_transport_id trid = {};

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	/* Selectively copy the trid. Things like NQN don't matter here - that
	 * mapping is enforced elsewhere.
	 */
	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	trid.adrfam = _trid->adrfam;
	snprintf(trid.traddr, sizeof(port->trid.traddr), "%s", _trid->traddr);
	snprintf(trid.trsvcid, sizeof(port->trid.trsvcid), "%s", _trid->trsvcid);

	pthread_mutex_lock(&rtransport->lock);
	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(&port->trid, &trid) == 0) {
			assert(port->ref > 0);
			port->ref--;
			if (port->ref == 0) {
				TAILQ_REMOVE(&rtransport->ports, port, link);
				rdma_destroy_id(port->id);
				free(port);
			}
			break;
		}
	}

	pthread_mutex_unlock(&rtransport->lock);
	return 0;
}

static void
spdk_nvmf_rdma_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct rdma_cm_event		*event;
	int				rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (rtransport->event_channel == NULL) {
		return;
	}

	while (1) {
		rc = rdma_get_cm_event(rtransport->event_channel, &event);
		if (rc == 0) {
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Acceptor Event: %s\n", CM_EVENT_STR[event->event]);

			switch (event->event) {
			case RDMA_CM_EVENT_CONNECT_REQUEST:
				rc = nvmf_rdma_connect(transport, event, cb_fn);
				if (rc < 0) {
					SPDK_ERRLOG("Unable to process connect event. rc: %d\n", rc);
					break;
				}
				break;
			case RDMA_CM_EVENT_ESTABLISHED:
				break;
			case RDMA_CM_EVENT_ADDR_CHANGE:
			case RDMA_CM_EVENT_DISCONNECTED:
			case RDMA_CM_EVENT_DEVICE_REMOVAL:
			case RDMA_CM_EVENT_TIMEWAIT_EXIT:
				rc = nvmf_rdma_disconnect(event);
				if (rc < 0) {
					SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
					break;
				}
				continue;
			default:
				SPDK_ERRLOG("Unexpected Acceptor Event [%d]\n", event->event);
				break;
			}

			rdma_ack_cm_event(event);
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("Acceptor Event Error: %s\n", spdk_strerror(errno));
			}
			break;
		}
	}
}

static void
spdk_nvmf_rdma_discover(struct spdk_nvmf_transport *transport,
			struct spdk_nvme_transport_id *trid,
			struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.rdma.rdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	entry->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	entry->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_rdma_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_device		*device;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rgroup = calloc(1, sizeof(*rgroup));
	if (!rgroup) {
		return NULL;
	}

	TAILQ_INIT(&rgroup->pollers);

	pthread_mutex_lock(&rtransport->lock);
	TAILQ_FOREACH(device, &rtransport->devices, link) {
		if (device->map == NULL) {
			/*
			 * The device is not in use (no listeners),
			 * so no protection domain has been constructed.
			 * Skip it.
			 */
			SPDK_NOTICELOG("Skipping unused RDMA device when creating poll group.\n");
			continue;
		}

		poller = calloc(1, sizeof(*poller));
		if (!poller) {
			SPDK_ERRLOG("Unable to allocate memory for new RDMA poller\n");
			free(rgroup);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}

		poller->device = device;
		poller->group = rgroup;

		TAILQ_INIT(&poller->qpairs);

		poller->cq = ibv_create_cq(device->context, NVMF_RDMA_CQ_SIZE, poller, NULL, 0);
		if (!poller->cq) {
			SPDK_ERRLOG("Unable to create completion queue\n");
			free(poller);
			free(rgroup);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}

		TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);
	}

	pthread_mutex_unlock(&rtransport->lock);
	return &rgroup->group;
}

static void
spdk_nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller, *tmp;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	if (!rgroup) {
		return;
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		TAILQ_REMOVE(&rgroup->pollers, poller, link);

		if (poller->cq) {
			ibv_destroy_cq(poller->cq);
		}

		free(poller);
	}

	free(rgroup);
}

static int
spdk_nvmf_rdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_qpair 		*rqpair;
	struct spdk_nvmf_rdma_device 		*device;
	struct spdk_nvmf_rdma_poller		*poller;
	int					rc;

	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	device = rqpair->port->device;

	if (device->pd != rqpair->cm_id->pd) {
		SPDK_ERRLOG("Mismatched protection domains\n");
		return -1;
	}

	TAILQ_FOREACH(poller, &rgroup->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		SPDK_ERRLOG("No poller found for device.\n");
		return -1;
	}

	TAILQ_INSERT_TAIL(&poller->qpairs, rqpair, link);
	rqpair->poller = poller;

	rc = spdk_nvmf_rdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		return -1;
	}

	rqpair->mgmt_channel = spdk_get_io_channel(rtransport);
	if (!rqpair->mgmt_channel) {
		spdk_nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}

	rqpair->ch = spdk_io_channel_get_ctx(rqpair->mgmt_channel);
	assert(rqpair->ch != NULL);

	rc = spdk_nvmf_rdma_event_accept(rqpair->cm_id, rqpair);
	if (rc) {
		/* Try to reject, but we probably can't */
		spdk_nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_rdma_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_qpair 		*rqpair;
	struct spdk_nvmf_rdma_device 		*device;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_qpair		*rq, *trq;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	device = rqpair->port->device;

	TAILQ_FOREACH(poller, &rgroup->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		SPDK_ERRLOG("No poller found for device.\n");
		return -1;
	}

	TAILQ_FOREACH_SAFE(rq, &poller->qpairs, link, trq) {
		if (rq == rqpair) {
			TAILQ_REMOVE(&poller->qpairs, rqpair, link);
			break;
		}
	}

	if (rq == NULL) {
		SPDK_ERRLOG("RDMA qpair cannot be removed from group (not in group).\n");
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_rdma_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);

	rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	spdk_nvmf_rdma_request_process(rtransport, rdma_req);

	return 0;
}

static void
spdk_nvmf_rdma_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	spdk_nvmf_rdma_qpair_destroy(SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair));
}

static void
spdk_nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
				     struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv, *recv_tmp;
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;

	/* We process I/O in the pending_rdma_rw queue at the highest priority. */
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_rw_queue, link, req_tmp) {
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}

	/* The second highest priority is I/O waiting on memory buffers. */
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->ch->pending_data_buf_queue, link, req_tmp) {
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}

	/* The lowest priority is processing newly received commands */
	TAILQ_FOREACH_SAFE(rdma_recv, &rqpair->incoming_queue, link, recv_tmp) {
		rdma_req = TAILQ_FIRST(&rqpair->free_queue);
		if (rdma_req == NULL) {
			/* Need to wait for more SEND completions */
			break;
		}

		rdma_req->recv = rdma_recv;
		rdma_req->state = RDMA_REQUEST_STATE_NEW;
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
}

static struct spdk_nvmf_rdma_request *
get_rdma_req_from_wc(struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_request *rdma_req;

	rdma_req = (struct spdk_nvmf_rdma_request *)wc->wr_id;
	assert(rdma_req != NULL);

#ifdef DEBUG
	struct spdk_nvmf_rdma_qpair *rqpair;
	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

	assert(rdma_req - rqpair->reqs >= 0);
	assert(rdma_req - rqpair->reqs < (ptrdiff_t)rqpair->max_queue_depth);
#endif

	return rdma_req;
}

static struct spdk_nvmf_rdma_recv *
get_rdma_recv_from_wc(struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_recv *rdma_recv;

	assert(wc->byte_len >= sizeof(struct spdk_nvmf_capsule_cmd));

	rdma_recv = (struct spdk_nvmf_rdma_recv *)wc->wr_id;
	assert(rdma_recv != NULL);

#ifdef DEBUG
	struct spdk_nvmf_rdma_qpair *rqpair = rdma_recv->qpair;

	assert(rdma_recv - rqpair->recvs >= 0);
	assert(rdma_recv - rqpair->recvs < (ptrdiff_t)rqpair->max_queue_depth);
#endif

	return rdma_recv;
}

static int
spdk_nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
			   struct spdk_nvmf_rdma_poller *rpoller)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_qpair	*rqpair;
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
			rdma_req = get_rdma_req_from_wc(&wc[i]);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			assert(rdma_req->state == RDMA_REQUEST_STATE_COMPLETING);
			rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;

			spdk_nvmf_rdma_request_process(rtransport, rdma_req);

			count++;

			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
			break;

		case IBV_WC_RDMA_WRITE:
			rdma_req = get_rdma_req_from_wc(&wc[i]);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			rqpair->cur_rdma_rw_depth--;

			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
			break;

		case IBV_WC_RDMA_READ:
			rdma_req = get_rdma_req_from_wc(&wc[i]);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			assert(rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
			rqpair->cur_rdma_rw_depth--;
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;

			spdk_nvmf_rdma_request_process(rtransport, rdma_req);

			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
			break;

		case IBV_WC_RECV:
			rdma_recv = get_rdma_recv_from_wc(&wc[i]);
			rqpair = rdma_recv->qpair;

			TAILQ_INSERT_TAIL(&rqpair->incoming_queue, rdma_recv, link);

			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
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

static int
spdk_nvmf_rdma_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller	*rpoller;
	int				count, rc;

	rtransport = SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_rdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	count = 0;
	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		rc = spdk_nvmf_rdma_poller_poll(rtransport, rpoller);
		if (rc < 0) {
			return rc;
		}
		count += rc;
	}

	return count;
}

static bool
spdk_nvmf_rdma_qpair_is_idle(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair *rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	if (rqpair->cur_queue_depth == 0 && rqpair->cur_rdma_rw_depth == 0) {
		return true;
	}
	return false;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.create = spdk_nvmf_rdma_create,
	.destroy = spdk_nvmf_rdma_destroy,

	.listen = spdk_nvmf_rdma_listen,
	.stop_listen = spdk_nvmf_rdma_stop_listen,
	.accept = spdk_nvmf_rdma_accept,

	.listener_discover = spdk_nvmf_rdma_discover,

	.poll_group_create = spdk_nvmf_rdma_poll_group_create,
	.poll_group_destroy = spdk_nvmf_rdma_poll_group_destroy,
	.poll_group_add = spdk_nvmf_rdma_poll_group_add,
	.poll_group_remove = spdk_nvmf_rdma_poll_group_remove,
	.poll_group_poll = spdk_nvmf_rdma_poll_group_poll,

	.req_complete = spdk_nvmf_rdma_request_complete,

	.qpair_fini = spdk_nvmf_rdma_close_qpair,
	.qpair_is_idle = spdk_nvmf_rdma_qpair_is_idle,

};

SPDK_LOG_REGISTER_COMPONENT("rdma", SPDK_LOG_RDMA)
