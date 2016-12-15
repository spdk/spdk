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
#include <fcntl.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

#include "nvmf_internal.h"
#include "request.h"
#include "session.h"
#include "subsystem.h"
#include "transport.h"

#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"

#include "spdk_internal/log.h"

/*
 RDMA Connection Resouce Defaults
 */
#define NVMF_DEFAULT_TX_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

struct spdk_nvmf_rdma_buf {
	SLIST_ENTRY(spdk_nvmf_rdma_buf) link;
};

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;

	/* In Capsule data buffer */
	uint8_t					*buf;

	union {
		struct ibv_recv_wr 		recv;
		struct ibv_send_wr		send;
	} wr;
	struct ibv_sge 				sg_list[2];

	TAILQ_ENTRY(spdk_nvmf_rdma_request)	link;
};

struct spdk_nvmf_rdma_conn {
	struct spdk_nvmf_conn			conn;

	struct rdma_cm_id			*cm_id;
	struct ibv_cq				*cq;

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

	/* Requests that are waiting to obtain a data buffer */
	TAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_data_buf_queue;

	/* Requests that are waiting to perform an RDMA READ or WRITE */
	TAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_rw_queue;

	/* Array of size "max_queue_depth" containing RDMA requests. */
	struct spdk_nvmf_rdma_request		*reqs;

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

	TAILQ_ENTRY(spdk_nvmf_rdma_conn)	link;
};

/* List of RDMA connections that have not yet received a CONNECT capsule */
static TAILQ_HEAD(, spdk_nvmf_rdma_conn) g_pending_conns = TAILQ_HEAD_INITIALIZER(g_pending_conns);

struct spdk_nvmf_rdma_session {
	struct spdk_nvmf_session		session;

	SLIST_HEAD(, spdk_nvmf_rdma_buf)	data_buf_pool;

	struct ibv_context			*verbs;

	uint8_t					*buf;
	struct ibv_mr				*buf_mr;
};

struct spdk_nvmf_rdma_listen_addr {
	char					*traddr;
	char					*trsvcid;
	struct rdma_cm_id			*id;
	struct ibv_device_attr 			attr;
	struct ibv_comp_channel			*comp_channel;
	TAILQ_ENTRY(spdk_nvmf_rdma_listen_addr)	link;
};

struct spdk_nvmf_rdma {
	struct rdma_event_channel	*event_channel;

	pthread_mutex_t 		lock;

	uint16_t 			max_queue_depth;
	uint32_t 			max_io_size;
	uint32_t 			in_capsule_data_size;

	TAILQ_HEAD(, spdk_nvmf_rdma_listen_addr)	listen_addrs;
};

static struct spdk_nvmf_rdma g_rdma = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.listen_addrs = TAILQ_HEAD_INITIALIZER(g_rdma.listen_addrs),
};

static inline struct spdk_nvmf_rdma_conn *
get_rdma_conn(struct spdk_nvmf_conn *conn)
{
	return (struct spdk_nvmf_rdma_conn *)((uintptr_t)conn - offsetof(struct spdk_nvmf_rdma_conn, conn));
}

static inline struct spdk_nvmf_rdma_request *
get_rdma_req(struct spdk_nvmf_request *req)
{
	return (struct spdk_nvmf_rdma_request *)((uintptr_t)req - offsetof(struct spdk_nvmf_rdma_request,
			req));
}

static inline struct spdk_nvmf_rdma_session *
get_rdma_sess(struct spdk_nvmf_session *sess)
{
	return (struct spdk_nvmf_rdma_session *)((uintptr_t)sess - offsetof(struct spdk_nvmf_rdma_session,
			session));
}

static int nvmf_post_rdma_recv(struct spdk_nvmf_request *req);

static void
spdk_nvmf_rdma_conn_destroy(struct spdk_nvmf_rdma_conn *rdma_conn)
{
	if (rdma_conn->cmds_mr) {
		ibv_dereg_mr(rdma_conn->cmds_mr);
	}

	if (rdma_conn->cpls_mr) {
		ibv_dereg_mr(rdma_conn->cpls_mr);
	}

	if (rdma_conn->bufs_mr) {
		ibv_dereg_mr(rdma_conn->bufs_mr);
	}

	if (rdma_conn->cm_id) {
		rdma_destroy_qp(rdma_conn->cm_id);
		rdma_destroy_id(rdma_conn->cm_id);
	}

	if (rdma_conn->cq) {
		ibv_destroy_cq(rdma_conn->cq);
	}

	/* Free all memory */
	spdk_free(rdma_conn->cmds);
	spdk_free(rdma_conn->cpls);
	spdk_free(rdma_conn->bufs);
	free(rdma_conn->reqs);
	free(rdma_conn);
}

static struct spdk_nvmf_rdma_conn *
spdk_nvmf_rdma_conn_create(struct rdma_cm_id *id, struct ibv_comp_channel *channel,
			   uint16_t max_queue_depth, uint16_t max_rw_depth)
{
	struct spdk_nvmf_rdma_conn	*rdma_conn;
	struct spdk_nvmf_conn		*conn;
	int				rc, i;
	struct ibv_qp_init_attr		attr;
	struct spdk_nvmf_rdma_request	*rdma_req;

	rdma_conn = calloc(1, sizeof(struct spdk_nvmf_rdma_conn));
	if (rdma_conn == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		return NULL;
	}

	rdma_conn->max_queue_depth = max_queue_depth;
	rdma_conn->max_rw_depth = max_rw_depth;
	TAILQ_INIT(&rdma_conn->pending_data_buf_queue);
	TAILQ_INIT(&rdma_conn->pending_rdma_rw_queue);

	rdma_conn->cq = ibv_create_cq(id->verbs, max_queue_depth * 2, rdma_conn, channel, 0);
	if (!rdma_conn->cq) {
		SPDK_ERRLOG("Unable to create completion queue\n");
		SPDK_ERRLOG("Completion Channel: %p Id: %p Verbs: %p\n", channel, id, id->verbs);
		SPDK_ERRLOG("Errno %d: %s\n", errno, strerror(errno));
		rdma_destroy_id(id);
		spdk_nvmf_rdma_conn_destroy(rdma_conn);
		return NULL;
	}

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.send_cq		= rdma_conn->cq;
	attr.recv_cq		= rdma_conn->cq;
	attr.cap.max_send_wr	= max_queue_depth; /* SEND, READ, and WRITE operations */
	attr.cap.max_recv_wr	= max_queue_depth; /* RECV operations */
	attr.cap.max_send_sge	= NVMF_DEFAULT_TX_SGE;
	attr.cap.max_recv_sge	= NVMF_DEFAULT_RX_SGE;

	rc = rdma_create_qp(id, NULL, &attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed\n");
		SPDK_ERRLOG("Errno %d: %s\n", errno, strerror(errno));
		rdma_destroy_id(id);
		spdk_nvmf_rdma_conn_destroy(rdma_conn);
		return NULL;
	}

	conn = &rdma_conn->conn;
	conn->transport = &spdk_nvmf_transport_rdma;
	id->context = conn;
	rdma_conn->cm_id = id;

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "New RDMA Connection: %p\n", conn);

	rdma_conn->reqs = calloc(max_queue_depth, sizeof(*rdma_conn->reqs));
	rdma_conn->cmds = spdk_zmalloc(max_queue_depth * sizeof(*rdma_conn->cmds),
				       0x1000, NULL);
	rdma_conn->cpls = spdk_zmalloc(max_queue_depth * sizeof(*rdma_conn->cpls),
				       0x1000, NULL);
	rdma_conn->bufs = spdk_zmalloc(max_queue_depth * g_rdma.in_capsule_data_size,
				       0x1000, NULL);
	if (!rdma_conn->reqs || !rdma_conn->cmds || !rdma_conn->cpls || !rdma_conn->bufs) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
		spdk_nvmf_rdma_conn_destroy(rdma_conn);
		return NULL;
	}

	rdma_conn->cmds_mr = ibv_reg_mr(id->pd, rdma_conn->cmds,
					max_queue_depth * sizeof(*rdma_conn->cmds),
					IBV_ACCESS_LOCAL_WRITE);
	rdma_conn->cpls_mr = ibv_reg_mr(id->pd, rdma_conn->cpls,
					max_queue_depth * sizeof(*rdma_conn->cpls),
					0);
	rdma_conn->bufs_mr = ibv_reg_mr(id->pd, rdma_conn->bufs,
					max_queue_depth * g_rdma.in_capsule_data_size,
					IBV_ACCESS_LOCAL_WRITE |
					IBV_ACCESS_REMOTE_WRITE);
	if (!rdma_conn->cmds_mr || !rdma_conn->cpls_mr || !rdma_conn->bufs_mr) {
		SPDK_ERRLOG("Unable to register required memory for RDMA queue.\n");
		spdk_nvmf_rdma_conn_destroy(rdma_conn);
		return NULL;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Command Array: %p Length: %lx LKey: %x\n",
		      rdma_conn->cmds, max_queue_depth * sizeof(*rdma_conn->cmds), rdma_conn->cmds_mr->lkey);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Completion Array: %p Length: %lx LKey: %x\n",
		      rdma_conn->cpls, max_queue_depth * sizeof(*rdma_conn->cpls), rdma_conn->cpls_mr->lkey);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "In Capsule Data Array: %p Length: %x LKey: %x\n",
		      rdma_conn->bufs, max_queue_depth * g_rdma.in_capsule_data_size, rdma_conn->bufs_mr->lkey);

	for (i = 0; i < max_queue_depth; i++) {
		rdma_req = &rdma_conn->reqs[i];
		rdma_req->buf = (void *)((uintptr_t)rdma_conn->bufs + (i * g_rdma.in_capsule_data_size));
		rdma_req->req.cmd = &rdma_conn->cmds[i];
		rdma_req->req.rsp = &rdma_conn->cpls[i];
		rdma_req->req.conn = &rdma_conn->conn;

		if (nvmf_post_rdma_recv(&rdma_req->req)) {
			SPDK_ERRLOG("Unable to post capsule for RDMA RECV\n");
			spdk_nvmf_rdma_conn_destroy(rdma_conn);
			return NULL;
		}
	}

	return rdma_conn;
}

static inline void
nvmf_trace_ibv_sge(struct ibv_sge *sg_list)
{
	if (sg_list) {
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "local addr %p length 0x%x lkey 0x%x\n",
			      (void *)sg_list->addr, sg_list->length, sg_list->lkey);
	}
}

static inline void
nvmf_ibv_send_wr_init(struct ibv_send_wr *wr,
		      struct spdk_nvmf_request *req,
		      struct ibv_sge *sg_list,
		      enum ibv_wr_opcode opcode,
		      int send_flags)
{
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);
	assert(wr != NULL);
	assert(sg_list != NULL);

	memset(wr, 0, sizeof(*wr));

	wr->wr_id = (uint64_t)rdma_req;
	wr->opcode = opcode;
	wr->send_flags = send_flags;
	wr->sg_list = sg_list;
	wr->num_sge = 1;
}

static inline void
nvmf_ibv_send_wr_set_rkey(struct ibv_send_wr *wr, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_sgl_descriptor *sgl = &req->cmd->nvme_cmd.dptr.sgl1;

	assert(sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);

	wr->wr.rdma.rkey = sgl->keyed.key;
	wr->wr.rdma.remote_addr = sgl->address;

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "rkey %x remote_addr %p\n",
		      wr->wr.rdma.rkey, (void *)wr->wr.rdma.remote_addr);
}

static int
nvmf_post_rdma_read(struct spdk_nvmf_request *req)
{
	struct ibv_send_wr	*bad_wr = NULL;
	struct spdk_nvmf_conn 	*conn = req->conn;
	struct spdk_nvmf_rdma_request 	*rdma_req = get_rdma_req(req);
	struct spdk_nvmf_rdma_conn 	*rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_session 	*rdma_sess;
	int 			rc;

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "RDMA READ POSTED. Request: %p Connection: %p\n", req, conn);

	rdma_req->sg_list[0].addr = (uintptr_t)req->data;
	if (req->length > g_rdma.in_capsule_data_size) {
		rdma_sess = get_rdma_sess(conn->sess);
		rdma_req->sg_list[0].lkey = rdma_sess->buf_mr->lkey;
	} else {
		rdma_req->sg_list[0].lkey = rdma_conn->bufs_mr->lkey;
	}
	rdma_req->sg_list[0].length = req->length;
	nvmf_trace_ibv_sge(&rdma_req->sg_list[0]);

	nvmf_ibv_send_wr_init(&rdma_req->wr.send, req, rdma_req->sg_list, IBV_WR_RDMA_READ,
			      IBV_SEND_SIGNALED);
	nvmf_ibv_send_wr_set_rkey(&rdma_req->wr.send, req);

	spdk_trace_record(TRACE_RDMA_READ_START, 0, 0, (uintptr_t)req, 0);
	rc = ibv_post_send(rdma_conn->cm_id->qp, &rdma_req->wr.send, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma read send, rc = 0x%x\n", rc);
	}

	return rc;
}

static int
nvmf_post_rdma_write(struct spdk_nvmf_request *req)
{
	struct ibv_send_wr	*bad_wr = NULL;
	struct spdk_nvmf_conn 	*conn = req->conn;
	struct spdk_nvmf_rdma_request 	*rdma_req = get_rdma_req(req);
	struct spdk_nvmf_rdma_conn 	*rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_session 	*rdma_sess;
	int 			rc;

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "RDMA WRITE POSTED. Request: %p Connection: %p\n", req, conn);

	rdma_req->sg_list[0].addr = (uintptr_t)req->data;
	if (req->length > g_rdma.in_capsule_data_size) {
		rdma_sess = get_rdma_sess(conn->sess);
		rdma_req->sg_list[0].lkey = rdma_sess->buf_mr->lkey;
	} else {
		rdma_req->sg_list[0].lkey = rdma_conn->bufs_mr->lkey;
	}
	rdma_req->sg_list[0].length = req->length;
	nvmf_trace_ibv_sge(&rdma_req->sg_list[0]);

	nvmf_ibv_send_wr_init(&rdma_req->wr.send, req, rdma_req->sg_list, IBV_WR_RDMA_WRITE,
			      IBV_SEND_SIGNALED);
	nvmf_ibv_send_wr_set_rkey(&rdma_req->wr.send, req);

	spdk_trace_record(TRACE_RDMA_WRITE_START, 0, 0, (uintptr_t)req, 0);
	rc = ibv_post_send(rdma_conn->cm_id->qp, &rdma_req->wr.send, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma write send, rc = 0x%x\n", rc);
	}

	return rc;
}

static int
nvmf_post_rdma_recv(struct spdk_nvmf_request *req)
{
	struct ibv_recv_wr *bad_wr = NULL;
	struct spdk_nvmf_conn *conn = req->conn;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);
	int rc;

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "RDMA RECV POSTED. Request: %p Connection: %p\n", req, conn);

	rdma_req->sg_list[0].addr = (uintptr_t)req->cmd;
	rdma_req->sg_list[0].length = sizeof(*req->cmd);
	rdma_req->sg_list[0].lkey = rdma_conn->cmds_mr->lkey;
	nvmf_trace_ibv_sge(&rdma_req->sg_list[0]);

	rdma_req->sg_list[1].addr = (uintptr_t)rdma_req->buf;
	rdma_req->sg_list[1].length = g_rdma.in_capsule_data_size;
	rdma_req->sg_list[1].lkey = rdma_conn->bufs_mr->lkey;
	nvmf_trace_ibv_sge(&rdma_req->sg_list[1]);

	memset(&rdma_req->wr.recv, 0, sizeof(struct ibv_recv_wr));
	rdma_req->wr.recv.wr_id = (uintptr_t)rdma_req;
	rdma_req->wr.recv.next = NULL;
	rdma_req->wr.recv.sg_list = rdma_req->sg_list;
	rdma_req->wr.recv.num_sge = 2;

	rc = ibv_post_recv(rdma_conn->cm_id->qp, &rdma_req->wr.recv, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma recv, rc = 0x%x\n", rc);
	}

	return rc;
}

static int
nvmf_post_rdma_send(struct spdk_nvmf_request *req)
{
	struct ibv_send_wr	*bad_wr = NULL;
	struct spdk_nvmf_conn 	*conn = req->conn;
	struct spdk_nvmf_rdma_request 	*rdma_req = get_rdma_req(req);
	struct spdk_nvmf_rdma_conn 	*rdma_conn = get_rdma_conn(conn);
	int 			rc;

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "RDMA SEND POSTED. Request: %p Connection: %p\n", req, conn);

	rdma_req->sg_list[0].addr = (uintptr_t)req->rsp;
	rdma_req->sg_list[0].length = sizeof(*req->rsp);
	rdma_req->sg_list[0].lkey = rdma_conn->cpls_mr->lkey;
	nvmf_trace_ibv_sge(&rdma_req->sg_list[0]);

	nvmf_ibv_send_wr_init(&rdma_req->wr.send, req, rdma_req->sg_list, IBV_WR_SEND, IBV_SEND_SIGNALED);

	spdk_trace_record(TRACE_NVMF_IO_COMPLETE, 0, 0, (uintptr_t)req, 0);
	rc = ibv_post_send(rdma_conn->cm_id->qp, &rdma_req->wr.send, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma send for NVMf completion, rc = 0x%x\n", rc);
	}

	return rc;
}

/**
 * REQUEST COMPLETION HANDLING
 *
 * Request completion consists of three steps:
 *
 * 1) Transfer any data to the host using an RDMA Write. If no data or an NVMe write,
 *    this step is unnecessary. (spdk_nvmf_rdma_request_transfer_data)
 * 2) Upon transfer completion, update sq_head, re-post the recv capsule,
 *    and send the completion. (spdk_nvmf_rdma_request_send_completion)
 * 3) Upon getting acknowledgement of the completion, decrement the internal
 *    count of number of outstanding requests. (spdk_nvmf_rdma_request_ack_completion)
 *
 * There are two public interfaces to initiate the process of completing a request,
 * exposed as callbacks in the transport layer.
 *
 * 1) spdk_nvmf_rdma_request_complete, which attempts to do all three steps.
 * 2) spdk_nvmf_rdma_request_release, which skips straight to step 3.
**/

static int
spdk_nvmf_rdma_request_transfer_data(struct spdk_nvmf_request *req)
{
	int rc;
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);
	struct spdk_nvmf_conn *conn = req->conn;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);

	assert(req->xfer != SPDK_NVME_DATA_NONE);

	if (rdma_conn->cur_rdma_rw_depth < rdma_conn->max_rw_depth) {
		if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			rc = nvmf_post_rdma_write(req);
			if (rc) {
				SPDK_ERRLOG("Unable to transfer data from target to host\n");
				return -1;
			}
		} else if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			rc = nvmf_post_rdma_read(req);
			if (rc) {
				SPDK_ERRLOG("Unable to transfer data from host to target\n");
				return -1;
			}
		}
		rdma_conn->cur_rdma_rw_depth++;
	} else {
		TAILQ_INSERT_TAIL(&rdma_conn->pending_rdma_rw_queue, rdma_req, link);
	}

	return 0;
}

static int
spdk_nvmf_rdma_request_send_completion(struct spdk_nvmf_request *req)
{
	int rc;
	struct spdk_nvmf_conn		*conn = req->conn;
	struct spdk_nvme_cpl		*rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_rdma_session	*rdma_sess;
	struct spdk_nvmf_rdma_buf	*buf;

	if (req->length > g_rdma.in_capsule_data_size) {
		/* Put the buffer back in the pool */
		rdma_sess = get_rdma_sess(conn->sess);
		buf = req->data;

		SLIST_INSERT_HEAD(&rdma_sess->data_buf_pool, buf, link);
		req->data = NULL;
		req->length = 0;
	}

	/* Advance our sq_head pointer */
	if (conn->sq_head == conn->sq_head_max) {
		conn->sq_head = 0;
	} else {
		conn->sq_head++;
	}
	rsp->sqhd = conn->sq_head;

	/* Post the capsule to the recv buffer */
	rc = nvmf_post_rdma_recv(req);
	if (rc) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return rc;
	}

	/* Send the completion */
	rc = nvmf_post_rdma_send(req);
	if (rc) {
		SPDK_ERRLOG("Unable to send response capsule\n");
	}

	return rc;
}

static int
spdk_nvmf_rdma_request_ack_completion(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_conn *conn = req->conn;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);

	/* Advance our sq_head pointer */
	if (conn->sq_head == conn->sq_head_max) {
		conn->sq_head = 0;
	} else {
		conn->sq_head++;
	}

	rdma_conn->cur_queue_depth--;

	return 0;
}

static int
nvmf_rdma_connect(struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_conn	*rdma_conn = NULL;
	struct spdk_nvmf_rdma_listen_addr *addr;
	struct rdma_conn_param		*rdma_param = NULL;
	struct rdma_conn_param		ctrlr_event_data;
	const struct spdk_nvmf_rdma_request_private_data *private_data = NULL;
	struct spdk_nvmf_rdma_accept_private_data accept_data;
	uint16_t			sts = 0;
	uint16_t			max_queue_depth;
	uint16_t			max_rw_depth;
	int 				rc;

	if (event->id == NULL) {
		SPDK_ERRLOG("connect request: missing cm_id\n");
		goto err0;
	}

	if (event->id->verbs == NULL) {
		SPDK_ERRLOG("connect request: missing cm_id ibv_context\n");
		goto err0;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Connect Recv on fabric intf name %s, dev_name %s\n",
		      event->id->verbs->device->name, event->id->verbs->device->dev_name);

	addr = event->listen_id->context;
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Listen Id was %p with verbs %p. ListenAddr: %p\n",
		      event->listen_id, event->listen_id->verbs, addr);

	/* Figure out the supported queue depth. This is a multi-step process
	 * that takes into account hardware maximums, host provided values,
	 * and our target's internal memory limits */

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Calculating Queue Depth\n");

	/* Start with the maximum queue depth allowed by the target */
	max_queue_depth = g_rdma.max_queue_depth;
	max_rw_depth = g_rdma.max_queue_depth;
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Target Max Queue Depth: %d\n", g_rdma.max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_TRACELOG(SPDK_TRACE_RDMA,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      addr->attr.max_qp_wr, addr->attr.max_qp_rd_atom);
	max_queue_depth = nvmf_min(max_queue_depth, addr->attr.max_qp_wr);
	max_rw_depth = nvmf_min(max_rw_depth, addr->attr.max_qp_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	rdma_param = &event->param.conn;
	SPDK_TRACELOG(SPDK_TRACE_RDMA,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	if (rdma_param->initiator_depth > 0) {
		max_rw_depth = nvmf_min(max_rw_depth, rdma_param->initiator_depth);
	}

	/* Finally check for the host software requested values, which are
	 * optional. */
	if (rdma_param->private_data != NULL &&
	    rdma_param->private_data_len >= sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		private_data = rdma_param->private_data;
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "Host Receive Queue Size: %d\n", private_data->hrqsize);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "Host Send Queue Size: %d\n", private_data->hsqsize);
		max_queue_depth = nvmf_min(max_queue_depth, private_data->hrqsize);
		max_queue_depth = nvmf_min(max_queue_depth, private_data->hsqsize);
	}

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Final Negotiated Queue Depth: %d R/W Depth: %d\n",
		      max_queue_depth, max_rw_depth);

	/* Init the NVMf rdma transport connection */
	rdma_conn = spdk_nvmf_rdma_conn_create(event->id, addr->comp_channel, max_queue_depth,
					       max_rw_depth);
	if (rdma_conn == NULL) {
		SPDK_ERRLOG("Error on nvmf connection creation\n");
		goto err1;
	}

	accept_data.recfmt = 0;
	accept_data.crqsize = max_queue_depth;
	ctrlr_event_data = *rdma_param;
	ctrlr_event_data.private_data = &accept_data;
	ctrlr_event_data.private_data_len = sizeof(accept_data);
	if (event->id->ps == RDMA_PS_TCP) {
		ctrlr_event_data.responder_resources = 0; /* We accept 0 reads from the host */
		ctrlr_event_data.initiator_depth = max_rw_depth;
	}

	rc = rdma_accept(event->id, &ctrlr_event_data);
	if (rc) {
		SPDK_ERRLOG("Error on rdma_accept\n");
		goto err2;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Sent back the accept\n");

	/* Add this RDMA connection to the global list until a CONNECT capsule
	 * is received. */
	TAILQ_INSERT_TAIL(&g_pending_conns, rdma_conn, link);

	return 0;

err2:
	spdk_nvmf_rdma_conn_destroy(rdma_conn);

err1: {
		struct spdk_nvmf_rdma_reject_private_data rej_data;

		rej_data.status.sc = sts;
		rdma_reject(event->id, &ctrlr_event_data, sizeof(rej_data));
	}
err0:
	return -1;
}

static int
nvmf_rdma_disconnect(struct rdma_cm_event *evt)
{
	struct spdk_nvmf_conn		*conn;
	struct spdk_nvmf_session	*session;
	struct spdk_nvmf_subsystem	*subsystem;
	struct spdk_nvmf_rdma_conn 	*rdma_conn;

	if (evt->id == NULL) {
		SPDK_ERRLOG("disconnect request: missing cm_id\n");
		return -1;
	}

	conn = evt->id->context;
	if (conn == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}
	/* ack the disconnect event before rdma_destroy_id */
	rdma_ack_cm_event(evt);

	rdma_conn = get_rdma_conn(conn);

	session = conn->sess;
	if (session == NULL) {
		/* No session has been established yet. That means the conn
		 * must be in the pending connections list. Remove it. */
		TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
		spdk_nvmf_rdma_conn_destroy(rdma_conn);
		return 0;
	}

	subsystem = session->subsys;

	subsystem->disconnect_cb(subsystem->cb_ctx, conn);

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

typedef enum _spdk_nvmf_request_prep_type {
	SPDK_NVMF_REQUEST_PREP_ERROR = -1,
	SPDK_NVMF_REQUEST_PREP_READY = 0,
	SPDK_NVMF_REQUEST_PREP_PENDING_BUFFER = 1,
	SPDK_NVMF_REQUEST_PREP_PENDING_DATA = 2,
} spdk_nvmf_request_prep_type;

static spdk_nvmf_request_prep_type
spdk_nvmf_request_prep_data(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd		*cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl		*rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_rdma_request	*rdma_req = get_rdma_req(req);
	struct spdk_nvmf_rdma_session	*rdma_sess;
	struct spdk_nvme_sgl_descriptor *sgl;

	req->length = 0;
	req->data = NULL;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		req->xfer = spdk_nvme_opc_get_data_transfer(req->cmd->nvmf_cmd.fctype);
	} else {
		req->xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);
	}

	if (req->xfer == SPDK_NVME_DATA_NONE) {
		return SPDK_NVMF_REQUEST_PREP_READY;
	}

	sgl = &cmd->dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
		if (sgl->keyed.length > g_rdma.max_io_size) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    sgl->keyed.length, g_rdma.max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return SPDK_NVMF_REQUEST_PREP_ERROR;
		}

		if (sgl->keyed.length == 0) {
			req->xfer = SPDK_NVME_DATA_NONE;
			return SPDK_NVMF_REQUEST_PREP_READY;
		}

		req->length = sgl->keyed.length;

		/* TODO: In Capsule Data Size should be tracked per queue (admin, for instance, should always have 4k and no more). */
		if (sgl->keyed.length > g_rdma.in_capsule_data_size) {
			rdma_sess = get_rdma_sess(req->conn->sess);
			req->data = SLIST_FIRST(&rdma_sess->data_buf_pool);
			if (!req->data) {
				/* No available buffers. Queue this request up. */
				SPDK_TRACELOG(SPDK_TRACE_RDMA, "No available large data buffers. Queueing request %p\n", req);
				return SPDK_NVMF_REQUEST_PREP_PENDING_BUFFER;
			}

			SPDK_TRACELOG(SPDK_TRACE_RDMA, "Request %p took buffer from central pool\n", req);
			SLIST_REMOVE_HEAD(&rdma_sess->data_buf_pool, link);
		} else {
			/* Use the in capsule data buffer, even though this isn't in capsule data */
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "Request using in capsule buffer for non-capsule data\n");
			req->data = rdma_req->buf;
		}
		if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			return SPDK_NVMF_REQUEST_PREP_PENDING_DATA;
		} else {
			return SPDK_NVMF_REQUEST_PREP_READY;
		}
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = g_rdma.in_capsule_data_size;

		SPDK_TRACELOG(SPDK_TRACE_NVMF, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, sgl->unkeyed.length);

		if (offset > max_len) {
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
				    offset, max_len);
			rsp->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
			return SPDK_NVMF_REQUEST_PREP_ERROR;
		}
		max_len -= (uint32_t)offset;

		if (sgl->unkeyed.length > max_len) {
			SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
				    sgl->unkeyed.length, max_len);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return SPDK_NVMF_REQUEST_PREP_ERROR;
		}

		if (sgl->unkeyed.length == 0) {
			req->xfer = SPDK_NVME_DATA_NONE;
			return SPDK_NVMF_REQUEST_PREP_READY;
		}

		req->data = rdma_req->buf + offset;
		req->length = sgl->unkeyed.length;
		return SPDK_NVMF_REQUEST_PREP_READY;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return SPDK_NVMF_REQUEST_PREP_ERROR;
}

static int
spdk_nvmf_rdma_handle_pending_rdma_rw(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_conn	*rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_session	*rdma_sess;
	struct spdk_nvmf_rdma_request	*rdma_req, *tmp;
	int rc;
	int count = 0;

	/* First, try to assign free data buffers to requests that need one */
	if (conn->sess) {
		rdma_sess = get_rdma_sess(conn->sess);
		TAILQ_FOREACH_SAFE(rdma_req, &rdma_conn->pending_data_buf_queue, link, tmp) {
			assert(rdma_req->req.data == NULL);
			rdma_req->req.data = SLIST_FIRST(&rdma_sess->data_buf_pool);
			if (!rdma_req->req.data) {
				break;
			}
			SLIST_REMOVE_HEAD(&rdma_sess->data_buf_pool, link);
			TAILQ_REMOVE(&rdma_conn->pending_data_buf_queue, rdma_req, link);
			if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				TAILQ_INSERT_TAIL(&rdma_conn->pending_rdma_rw_queue, rdma_req, link);
			} else {
				rc = spdk_nvmf_request_exec(&rdma_req->req);
				if (rc < 0) {
					return -1;
				}
				count++;
			}
		}
	}

	/* Try to initiate RDMA Reads or Writes on requests that have data buffers */
	while (rdma_conn->cur_rdma_rw_depth < rdma_conn->max_rw_depth) {
		if (TAILQ_EMPTY(&rdma_conn->pending_rdma_rw_queue)) {
			break;
		}

		rdma_req = TAILQ_FIRST(&rdma_conn->pending_rdma_rw_queue);
		TAILQ_REMOVE(&rdma_conn->pending_rdma_rw_queue, rdma_req, link);

		SPDK_TRACELOG(SPDK_TRACE_RDMA, "Submitting previously queued for RDMA R/W request %p\n", rdma_req);

		rc = spdk_nvmf_rdma_request_transfer_data(&rdma_req->req);
		if (rc) {
			return -1;
		}
	}

	return count;
}

/* Public API callbacks begin here */

static int
spdk_nvmf_rdma_init(uint16_t max_queue_depth, uint32_t max_io_size,
		    uint32_t in_capsule_data_size)
{
	int rc;

	SPDK_NOTICELOG("*** RDMA Transport Init ***\n");

	pthread_mutex_lock(&g_rdma.lock);
	g_rdma.max_queue_depth = max_queue_depth;
	g_rdma.max_io_size = max_io_size;
	g_rdma.in_capsule_data_size = in_capsule_data_size;

	g_rdma.event_channel = rdma_create_event_channel();
	if (g_rdma.event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed, %s\n", strerror(errno));
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	rc = fcntl(g_rdma.event_channel->fd, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("fcntl to set fd to non-blocking failed\n");
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	pthread_mutex_unlock(&g_rdma.lock);
	return 0;
}

static int
spdk_nvmf_rdma_fini(void)
{
	struct spdk_nvmf_rdma_listen_addr *addr, *tmp;

	pthread_mutex_lock(&g_rdma.lock);
	TAILQ_FOREACH_SAFE(addr, &g_rdma.listen_addrs, link, tmp) {
		TAILQ_REMOVE(&g_rdma.listen_addrs, addr, link);
		ibv_destroy_comp_channel(addr->comp_channel);
		rdma_destroy_id(addr->id);
	}

	if (g_rdma.event_channel != NULL) {
		rdma_destroy_event_channel(g_rdma.event_channel);
	}
	pthread_mutex_unlock(&g_rdma.lock);

	return 0;
}

static int
spdk_nvmf_rdma_poll(struct spdk_nvmf_conn *conn);

static void
spdk_nvmf_rdma_acceptor_poll(void)
{
	struct rdma_cm_event		*event;
	int				rc;
	struct spdk_nvmf_rdma_conn	*rdma_conn, *tmp;

	if (g_rdma.event_channel == NULL) {
		return;
	}

	/* Process pending connections for incoming capsules. The only capsule
	 * this should ever find is a CONNECT request. */
	TAILQ_FOREACH_SAFE(rdma_conn, &g_pending_conns, link, tmp) {
		rc = spdk_nvmf_rdma_poll(&rdma_conn->conn);
		if (rc < 0) {
			TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
			spdk_nvmf_rdma_conn_destroy(rdma_conn);
		} else if (rc > 0) {
			/* At least one request was processed which is assumed to be
			 * a CONNECT. Remove this connection from our list. */
			TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
		}
	}

	while (1) {
		rc = rdma_get_cm_event(g_rdma.event_channel, &event);
		if (rc == 0) {
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "Acceptor Event: %s\n", CM_EVENT_STR[event->event]);

			switch (event->event) {
			case RDMA_CM_EVENT_CONNECT_REQUEST:
				rc = nvmf_rdma_connect(event);
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
				SPDK_ERRLOG("Acceptor Event Error: %s\n", strerror(errno));
			}
			break;
		}
	}
}

static int
spdk_nvmf_rdma_listen(struct spdk_nvmf_listen_addr *listen_addr)
{
	struct spdk_nvmf_rdma_listen_addr *addr;
	struct sockaddr_in saddr;
	int rc;

	pthread_mutex_lock(&g_rdma.lock);
	assert(g_rdma.event_channel != NULL);
	TAILQ_FOREACH(addr, &g_rdma.listen_addrs, link) {
		if ((!strcasecmp(addr->traddr, listen_addr->traddr)) &&
		    (!strcasecmp(addr->trsvcid, listen_addr->trsvcid))) {
			/* Already listening at this address */
			pthread_mutex_unlock(&g_rdma.lock);
			return 0;
		}
	}

	addr = calloc(1, sizeof(*addr));
	if (!addr) {
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	addr->traddr = listen_addr->traddr;
	addr->trsvcid = listen_addr->trsvcid;

	rc = rdma_create_id(g_rdma.event_channel, &addr->id, addr, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		free(addr);
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = inet_addr(addr->traddr);
	saddr.sin_port = htons((uint16_t)strtoul(addr->trsvcid, NULL, 10));
	rc = rdma_bind_addr(addr->id, (struct sockaddr *)&saddr);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_bind_addr() failed\n");
		rdma_destroy_id(addr->id);
		free(addr);
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	rc = rdma_listen(addr->id, 10); /* 10 = backlog */
	if (rc < 0) {
		SPDK_ERRLOG("rdma_listen() failed\n");
		rdma_destroy_id(addr->id);
		free(addr);
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	rc = ibv_query_device(addr->id->verbs, &addr->attr);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		rdma_destroy_id(addr->id);
		free(addr);
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	addr->comp_channel = ibv_create_comp_channel(addr->id->verbs);
	if (!addr->comp_channel) {
		SPDK_ERRLOG("Failed to create completion channel\n");
		rdma_destroy_id(addr->id);
		free(addr);
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "For listen id %p with context %p, created completion channel %p\n",
		      addr->id, addr->id->verbs, addr->comp_channel);

	rc = fcntl(addr->comp_channel->fd, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("fcntl to set comp channel to non-blocking failed\n");
		rdma_destroy_id(addr->id);
		ibv_destroy_comp_channel(addr->comp_channel);
		free(addr);
		pthread_mutex_unlock(&g_rdma.lock);
		return -1;
	}

	TAILQ_INSERT_TAIL(&g_rdma.listen_addrs, addr, link);
	pthread_mutex_unlock(&g_rdma.lock);

	SPDK_NOTICELOG("*** NVMf Target Listening on %s port %d ***\n",
		       addr->traddr, ntohs(rdma_get_src_port(addr->id)));

	return 0;
}

static void
spdk_nvmf_rdma_discover(struct spdk_nvmf_listen_addr *listen_addr,
			struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	entry->adrfam = SPDK_NVMF_ADRFAM_IPV4;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, listen_addr->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, listen_addr->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.rdma.rdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	entry->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	entry->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static struct spdk_nvmf_session *
spdk_nvmf_rdma_session_init(void)
{
	struct spdk_nvmf_rdma_session	*rdma_sess;
	int				i;
	struct spdk_nvmf_rdma_buf	*buf;

	rdma_sess = calloc(1, sizeof(*rdma_sess));
	if (!rdma_sess) {
		return NULL;
	}

	/* TODO: Make the number of elements in this pool configurable. For now, one full queue
	 *       worth seems reasonable.
	 */
	rdma_sess->buf = spdk_zmalloc(g_rdma.max_queue_depth * g_rdma.max_io_size,
				      0x20000, NULL);
	if (!rdma_sess->buf) {
		SPDK_ERRLOG("Large buffer pool allocation failed (%d x %d)\n",
			    g_rdma.max_queue_depth, g_rdma.max_io_size);
		free(rdma_sess);
		return NULL;
	}

	SLIST_INIT(&rdma_sess->data_buf_pool);
	for (i = 0; i < g_rdma.max_queue_depth; i++) {
		buf = (struct spdk_nvmf_rdma_buf *)(rdma_sess->buf + (i * g_rdma.max_io_size));
		SLIST_INSERT_HEAD(&rdma_sess->data_buf_pool, buf, link);
	}

	rdma_sess->session.transport = &spdk_nvmf_transport_rdma;

	return &rdma_sess->session;
}

static void
spdk_nvmf_rdma_session_fini(struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_rdma_session *rdma_sess = get_rdma_sess(session);

	if (!rdma_sess) {
		return;
	}

	ibv_dereg_mr(rdma_sess->buf_mr);
	spdk_free(rdma_sess->buf);
	free(rdma_sess);
}

static int
spdk_nvmf_rdma_session_add_conn(struct spdk_nvmf_session *session,
				struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_session	*rdma_sess = get_rdma_sess(session);
	struct spdk_nvmf_rdma_conn	*rdma_conn = get_rdma_conn(conn);

	if (rdma_sess->verbs != NULL) {
		if (rdma_sess->verbs != rdma_conn->cm_id->verbs) {
			SPDK_ERRLOG("Two connections belonging to the same session cannot connect using different RDMA devices.\n");
			return -1;
		}

		/* Nothing else to do. */
		return 0;
	}

	rdma_sess->verbs = rdma_conn->cm_id->verbs;
	rdma_sess->buf_mr = ibv_reg_mr(rdma_conn->cm_id->pd, rdma_sess->buf,
				       g_rdma.max_queue_depth * g_rdma.max_io_size,
				       IBV_ACCESS_LOCAL_WRITE |
				       IBV_ACCESS_REMOTE_WRITE);
	if (!rdma_sess->buf_mr) {
		SPDK_ERRLOG("Large buffer pool registration failed (%d x %d)\n",
			    g_rdma.max_queue_depth, g_rdma.max_io_size);
		spdk_free(rdma_sess->buf);
		free(rdma_sess);
		return -1;
	}

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Session Shared Data Pool: %p Length: %x LKey: %x\n",
		      rdma_sess->buf,  g_rdma.max_queue_depth * g_rdma.max_io_size, rdma_sess->buf_mr->lkey);

	return 0;
}

static int
spdk_nvmf_rdma_session_remove_conn(struct spdk_nvmf_session *session,
				   struct spdk_nvmf_conn *conn)
{
	return 0;
}

static int
spdk_nvmf_rdma_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	int rc;

	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		rc = spdk_nvmf_rdma_request_transfer_data(req);
	} else {
		rc = spdk_nvmf_rdma_request_send_completion(req);
	}

	return rc;
}

static int
spdk_nvmf_rdma_request_release(struct spdk_nvmf_request *req)
{
	return spdk_nvmf_rdma_request_ack_completion(req);
}

static void
spdk_nvmf_rdma_close_conn(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);

	return spdk_nvmf_rdma_conn_destroy(rdma_conn);
}

/* Returns the number of times that spdk_nvmf_request_exec was called,
 * or -1 on error.
 */
static int
spdk_nvmf_rdma_poll(struct spdk_nvmf_conn *conn)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req;
	struct spdk_nvmf_request *req;
	int reaped, i, rc;
	int count = 0;

	/* Poll for completing operations. */
	rc = ibv_poll_cq(rdma_conn->cq, 32, wc);
	if (rc < 0) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, strerror(errno));
		return -1;
	}

	reaped = rc;
	for (i = 0; i < reaped; i++) {
		if (wc[i].status) {
			SPDK_ERRLOG("CQ error on Connection %p, Request 0x%lu (%d): %s\n",
				    conn, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));
			return -1;
		}

		rdma_req = (struct spdk_nvmf_rdma_request *)wc[i].wr_id;
		if (rdma_req == NULL) {
			SPDK_ERRLOG("NULL wr_id in RDMA work completion\n");
			return -1;
		}

		req = &rdma_req->req;

		switch (wc[i].opcode) {
		case IBV_WC_SEND:
			assert(rdma_conn->cur_queue_depth > 0);
			SPDK_TRACELOG(SPDK_TRACE_RDMA,
				      "RDMA SEND Complete. Request: %p Connection: %p Outstanding I/O: %d\n",
				      req, conn, rdma_conn->cur_queue_depth - 1);
			rc = spdk_nvmf_rdma_request_ack_completion(req);
			if (rc) {
				return -1;
			}
			break;

		case IBV_WC_RDMA_WRITE:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "RDMA WRITE Complete. Request: %p Connection: %p\n",
				      req, conn);
			spdk_trace_record(TRACE_RDMA_WRITE_COMPLETE, 0, 0, (uint64_t)req, 0);
			rc = spdk_nvmf_rdma_request_send_completion(req);
			if (rc) {
				return -1;
			}

			/* Since an RDMA R/W operation completed, try to submit from the pending list. */
			rdma_conn->cur_rdma_rw_depth--;
			rc = spdk_nvmf_rdma_handle_pending_rdma_rw(conn);
			if (rc < 0) {
				return -1;
			}
			count += rc;
			break;

		case IBV_WC_RDMA_READ:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "RDMA READ Complete. Request: %p Connection: %p\n",
				      req, conn);
			spdk_trace_record(TRACE_RDMA_READ_COMPLETE, 0, 0, (uint64_t)req, 0);
			rc = spdk_nvmf_request_exec(req);
			if (rc) {
				return -1;
			}
			count++;

			/* Since an RDMA R/W operation completed, try to submit from the pending list. */
			rdma_conn->cur_rdma_rw_depth--;
			rc = spdk_nvmf_rdma_handle_pending_rdma_rw(conn);
			if (rc < 0) {
				return -1;
			}
			count += rc;
			break;

		case IBV_WC_RECV:
			if (wc[i].byte_len < sizeof(struct spdk_nvmf_capsule_cmd)) {
				SPDK_ERRLOG("recv length %u less than capsule header\n", wc[i].byte_len);
				return -1;
			}

			rdma_conn->cur_queue_depth++;
			SPDK_TRACELOG(SPDK_TRACE_RDMA,
				      "RDMA RECV Complete. Request: %p Connection: %p Outstanding I/O: %d\n",
				      req, conn, rdma_conn->cur_queue_depth);
			spdk_trace_record(TRACE_NVMF_IO_START, 0, 0, (uint64_t)req, 0);

			memset(req->rsp, 0, sizeof(*req->rsp));
			rc = spdk_nvmf_request_prep_data(req);
			switch (rc) {
			case SPDK_NVMF_REQUEST_PREP_READY:
				SPDK_TRACELOG(SPDK_TRACE_RDMA, "Request %p is ready for execution\n", req);
				/* Data is immediately available */
				rc = spdk_nvmf_request_exec(req);
				if (rc < 0) {
					return -1;
				}
				count++;
				break;
			case SPDK_NVMF_REQUEST_PREP_PENDING_BUFFER:
				SPDK_TRACELOG(SPDK_TRACE_RDMA, "Request %p needs data buffer\n", req);
				TAILQ_INSERT_TAIL(&rdma_conn->pending_data_buf_queue, rdma_req, link);
				break;
			case SPDK_NVMF_REQUEST_PREP_PENDING_DATA:
				SPDK_TRACELOG(SPDK_TRACE_RDMA, "Request %p needs data transfer\n", req);
				rc = spdk_nvmf_rdma_request_transfer_data(req);
				if (rc < 0) {
					return -1;
				}
				break;
			case SPDK_NVMF_REQUEST_PREP_ERROR:
				spdk_nvmf_rdma_request_complete(req);
				break;
			}
			break;

		default:
			SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
			return -1;
		}
	}

	return count;
}

const struct spdk_nvmf_transport spdk_nvmf_transport_rdma = {
	.name = "rdma",
	.transport_init = spdk_nvmf_rdma_init,
	.transport_fini = spdk_nvmf_rdma_fini,

	.acceptor_poll = spdk_nvmf_rdma_acceptor_poll,

	.listen_addr_add = spdk_nvmf_rdma_listen,
	.listen_addr_discover = spdk_nvmf_rdma_discover,

	.session_init = spdk_nvmf_rdma_session_init,
	.session_fini = spdk_nvmf_rdma_session_fini,
	.session_add_conn = spdk_nvmf_rdma_session_add_conn,
	.session_remove_conn = spdk_nvmf_rdma_session_remove_conn,

	.req_complete = spdk_nvmf_rdma_request_complete,
	.req_release = spdk_nvmf_rdma_request_release,

	.conn_fini = spdk_nvmf_rdma_close_conn,
	.conn_poll = spdk_nvmf_rdma_poll,


};

SPDK_LOG_REGISTER_TRACE_FLAG("rdma", SPDK_TRACE_RDMA)
