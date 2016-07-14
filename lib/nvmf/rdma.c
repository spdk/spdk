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

#include "rdma.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <unistd.h>
#include <stdint.h>

#include <rte_config.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_malloc.h>

#include "conn.h"
#include "request.h"
#include "port.h"
#include "host.h"
#include "spdk/assert.h"
#include "spdk/log.h"
#include "spdk/trace.h"

#define ACCEPT_TIMEOUT (rte_get_timer_hz() >> 10) /* ~1ms */

/*
 RDMA Connection Resouce Defaults
 */
#define NVMF_DEFAULT_TX_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

struct spdk_nvmf_rdma_conn {
	struct spdk_nvmf_conn			conn;

	struct rdma_cm_id			*cm_id;
	struct ibv_context			*ctx;
	struct ibv_comp_channel			*comp_channel;
	struct ibv_cq				*cq;
	struct ibv_qp				*qp;

	uint16_t				queue_depth;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	rdma_reqs;

	TAILQ_ENTRY(spdk_nvmf_rdma_conn)	link;
};

/* List of RDMA connections that have not yet received a CONNECT capsule */
static TAILQ_HEAD(, spdk_nvmf_rdma_conn) g_pending_conns = TAILQ_HEAD_INITIALIZER(g_pending_conns);

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;
	STAILQ_ENTRY(spdk_nvmf_rdma_request)	link;

	union nvmf_h2c_msg			cmd;
	struct ibv_mr				*cmd_mr;

	union nvmf_c2h_msg			rsp;
	struct ibv_mr				*rsp_mr;

	struct ibv_sge				send_sgl;
	struct ibv_sge				recv_sgl[2];

	struct ibv_mr				*bb_mr;
	uint8_t					*bb;
	uint32_t				bb_len;
};

struct spdk_nvmf_rdma {
	struct rte_timer		acceptor_timer;
	struct rdma_event_channel	*acceptor_event_channel;
	struct rdma_cm_id		*acceptor_listen_id;
};

static struct spdk_nvmf_rdma g_rdma = { };


static struct spdk_nvmf_rdma_conn *
allocate_rdma_conn(void)
{
	struct spdk_nvmf_rdma_conn *rdma_conn;

	rdma_conn = calloc(1, sizeof(struct spdk_nvmf_rdma_conn));
	if (rdma_conn == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		return NULL;
	}

	return rdma_conn;
}

static inline struct spdk_nvmf_rdma_conn *
get_rdma_conn(struct spdk_nvmf_conn *conn)
{
	return (struct spdk_nvmf_rdma_conn *)((uintptr_t)conn + offsetof(struct spdk_nvmf_rdma_conn, conn));
}

static inline struct spdk_nvmf_rdma_request *
get_rdma_req(struct spdk_nvmf_request *req)
{
	return (struct spdk_nvmf_rdma_request *)((uintptr_t)req + offsetof(struct spdk_nvmf_rdma_request,
			req));
}

static int
nvmf_rdma_queue_init(struct spdk_nvmf_conn *conn,
		     struct ibv_context *verbs)
{
	struct spdk_nvmf_rdma_conn	*rdma_conn = get_rdma_conn(conn);
	int				rc;
	struct ibv_qp_init_attr		attr;

	if (rdma_conn->ctx) {
		SPDK_ERRLOG("context already set!\n");
		goto return_error;
	}
	rdma_conn->ctx = verbs;

	rdma_conn->comp_channel = ibv_create_comp_channel(verbs);
	if (!rdma_conn->comp_channel) {
		SPDK_ERRLOG("create completion channel error!\n");
		goto return_error;
	}
	rc = fcntl(rdma_conn->comp_channel->fd, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("fcntl to set comp channel to non-blocking failed\n");
		goto cq_error;
	}

	/*
	 * Size the CQ to handle completions for RECV, SEND, and either READ or WRITE.
	 */
	rdma_conn->cq = ibv_create_cq(verbs, (rdma_conn->queue_depth * 3), conn, rdma_conn->comp_channel,
				      0);
	if (!rdma_conn->cq) {
		SPDK_ERRLOG("create cq error!\n");
		goto cq_error;
	}

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.send_cq		= rdma_conn->cq;
	attr.recv_cq		= rdma_conn->cq;
	attr.cap.max_send_wr	= rdma_conn->queue_depth * 2; /* SEND, READ, and WRITE operations */
	attr.cap.max_recv_wr	= rdma_conn->queue_depth; /* RECV operations */
	attr.cap.max_send_sge	= NVMF_DEFAULT_TX_SGE;
	attr.cap.max_recv_sge	= NVMF_DEFAULT_RX_SGE;

	rc = rdma_create_qp(rdma_conn->cm_id, NULL, &attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed\n");
		goto cq_error;
	}
	rdma_conn->qp = rdma_conn->cm_id->qp;

	return 0;

cq_error:
	ibv_destroy_comp_channel(rdma_conn->comp_channel);
return_error:
	return -1;
}

static void
free_rdma_req(struct spdk_nvmf_rdma_request *rdma_req)
{
	if (rdma_req->cmd_mr && rdma_dereg_mr(rdma_req->cmd_mr)) {
		SPDK_ERRLOG("Unable to de-register cmd_mr\n");
	}

	if (rdma_req->rsp_mr && rdma_dereg_mr(rdma_req->rsp_mr)) {
		SPDK_ERRLOG("Unable to de-register rsp_mr\n");
	}

	if (rdma_req->bb_mr && rdma_dereg_mr(rdma_req->bb_mr)) {
		SPDK_ERRLOG("Unable to de-register bb_mr\n");
	}

	rte_free(rdma_req->bb);
	rte_free(rdma_req);
}

static void
spdk_nvmf_rdma_free_req(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(req->conn);
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);

	STAILQ_REMOVE(&rdma_conn->rdma_reqs, rdma_req, spdk_nvmf_rdma_request, link);
	free_rdma_req(rdma_req);
}

static void
spdk_nvmf_rdma_free_reqs(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req;

	STAILQ_FOREACH(rdma_req, &rdma_conn->rdma_reqs, link) {
		STAILQ_REMOVE(&rdma_conn->rdma_reqs, rdma_req, spdk_nvmf_rdma_request, link);
		free_rdma_req(rdma_req);
	}
}

static struct spdk_nvmf_rdma_request *
alloc_rdma_req(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req;

	rdma_req = rte_zmalloc("nvmf_rdma_req", sizeof(*rdma_req), 0);
	if (!rdma_req) {
		SPDK_ERRLOG("Unable to allocate rdma_req\n");
		return NULL;
	}

	rdma_req->cmd_mr = rdma_reg_msgs(rdma_conn->cm_id, &rdma_req->cmd, sizeof(rdma_req->cmd));
	if (rdma_req->cmd_mr == NULL) {
		SPDK_ERRLOG("Unable to register cmd_mr\n");
		free_rdma_req(rdma_req);
		return NULL;
	}

	rdma_req->recv_sgl[0].addr = (uint64_t)&rdma_req->cmd;
	rdma_req->recv_sgl[0].length = sizeof(rdma_req->cmd);
	rdma_req->recv_sgl[0].lkey = rdma_req->cmd_mr->lkey;

	rdma_req->bb_len = DEFAULT_BB_SIZE;
	rdma_req->bb = rte_zmalloc("nvmf_bb", rdma_req->bb_len, 0);
	if (!rdma_req->bb) {
		SPDK_ERRLOG("Unable to get %u byte bounce buffer\n", rdma_req->bb_len);
		free_rdma_req(rdma_req);
		return NULL;
	}
	rdma_req->bb_mr = rdma_reg_read(rdma_conn->cm_id,
					(void *)rdma_req->bb,
					rdma_req->bb_len);
	if (rdma_req->bb_mr == NULL) {
		SPDK_ERRLOG("Unable to register bb_mr\n");
		free_rdma_req(rdma_req);
		return NULL;
	}

	/* initialize data buffer sgl */
	rdma_req->recv_sgl[1].addr = (uint64_t)rdma_req->bb;
	rdma_req->recv_sgl[1].length = rdma_req->bb_len;
	rdma_req->recv_sgl[1].lkey = rdma_req->bb_mr->lkey;

	rdma_req->rsp_mr = rdma_reg_msgs(rdma_conn->cm_id, &rdma_req->rsp, sizeof(rdma_req->rsp));
	if (rdma_req->rsp_mr == NULL) {
		SPDK_ERRLOG("Unable to register rsp_mr\n");
		free_rdma_req(rdma_req);
		return NULL;
	}

	/* initialize send_sgl */
	rdma_req->send_sgl.addr = (uint64_t)&rdma_req->rsp;
	rdma_req->send_sgl.length = sizeof(rdma_req->rsp);
	rdma_req->send_sgl.lkey = rdma_req->rsp_mr->lkey;

	rdma_req->req.cmd = &rdma_req->cmd;
	rdma_req->req.rsp = &rdma_req->rsp;
	rdma_req->req.conn = conn;

	return rdma_req;
}

static void
nvmf_drain_cq(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct ibv_wc wc;

	/* drain the cq before destruction */
	while (ibv_poll_cq(rdma_conn->cq, 1, &wc) > 0) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "drain cq event\n");
		//ibv_ack_cq_events(conn->cq, 1);
	}

}

void
nvmf_rdma_conn_cleanup(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	int rc;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");

	rdma_destroy_qp(rdma_conn->cm_id);

	spdk_nvmf_rdma_free_reqs(conn);

	nvmf_drain_cq(conn);
	rc = ibv_destroy_cq(rdma_conn->cq);
	if (rc) {
		SPDK_ERRLOG("ibv_destroy_cq error\n");
	}

	ibv_destroy_comp_channel(rdma_conn->comp_channel);
	rdma_destroy_id(rdma_conn->cm_id);

	free(rdma_conn);
}

static void
nvmf_trace_ibv_sge(struct ibv_sge *sg_list)
{
	if (sg_list) {
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "local addr %p length 0x%x lkey 0x%x\n",
			      (void *)sg_list->addr, sg_list->length, sg_list->lkey);
	}
}

static void
nvmf_ibv_send_wr_init(struct ibv_send_wr *wr,
		      struct spdk_nvmf_request *req,
		      struct ibv_sge *sg_list,
		      uint64_t wr_id,
		      enum ibv_wr_opcode opcode,
		      int send_flags)
{
	RTE_VERIFY(wr != NULL);
	RTE_VERIFY(sg_list != NULL);

	memset(wr, 0, sizeof(*wr));
	wr->wr_id = wr_id;
	wr->next = NULL;
	wr->opcode = opcode;
	wr->send_flags = send_flags;
	wr->sg_list = sg_list;
	wr->num_sge = 1;

	if (req != NULL) {
		struct spdk_nvme_sgl_descriptor *sgl = &req->cmd->nvme_cmd.dptr.sgl1;

		RTE_VERIFY(sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK);

		wr->wr.rdma.rkey = sgl->keyed.key;
		wr->wr.rdma.remote_addr = sgl->address;

		SPDK_TRACELOG(SPDK_TRACE_RDMA, "rkey %x remote_addr %p\n",
			      wr->wr.rdma.rkey, (void *)wr->wr.rdma.remote_addr);
	}

	nvmf_trace_ibv_sge(wr->sg_list);
}

int
nvmf_post_rdma_read(struct spdk_nvmf_conn *conn,
		    struct spdk_nvmf_request *req)
{
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);
	int rc;

	/* temporarily adjust SGE to only copy what the host is prepared to send. */
	rdma_req->recv_sgl[1].length = req->length;

	nvmf_ibv_send_wr_init(&wr, req, &rdma_req->recv_sgl[1], (uint64_t)rdma_req,
			      IBV_WR_RDMA_READ, IBV_SEND_SIGNALED);

	spdk_trace_record(TRACE_RDMA_READ_START, 0, 0, (uint64_t)req, 0);
	rc = ibv_post_send(rdma_conn->qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma read send, rc = 0x%x\n", rc);
	}
	return (rc);
}

static int
nvmf_post_rdma_write(struct spdk_nvmf_conn *conn,
		     struct spdk_nvmf_request *req)
{
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);
	int rc;

	/* temporarily adjust SGE to only copy what the host is prepared to receive. */
	rdma_req->recv_sgl[1].length = req->length;

	nvmf_ibv_send_wr_init(&wr, req, &rdma_req->recv_sgl[1], (uint64_t)rdma_req,
			      IBV_WR_RDMA_WRITE, 0);

	spdk_trace_record(TRACE_RDMA_WRITE_START, 0, 0, (uint64_t)req, 0);
	rc = ibv_post_send(rdma_conn->qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma write send, rc = 0x%x\n", rc);
	}
	return (rc);
}

static int
nvmf_post_rdma_recv(struct spdk_nvmf_conn *conn,
		    struct spdk_nvmf_request *req)
{
	struct ibv_recv_wr wr, *bad_wr = NULL;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);
	int rc;

	/* Update Connection SQ Tracking, increment
	   the SQ head counter opening up another
	   RX recv slot.
	*/
	conn->sq_head < (rdma_conn->queue_depth - 1) ? (conn->sq_head++) : (conn->sq_head = 0);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sq_head %x, sq_depth %x\n", conn->sq_head, rdma_conn->queue_depth);

	wr.wr_id = (uintptr_t)rdma_req;
	wr.next = NULL;
	wr.sg_list = &rdma_req->recv_sgl[0];
	wr.num_sge = 2;

	nvmf_trace_ibv_sge(&rdma_req->recv_sgl[0]);

	rc = ibv_post_recv(rdma_conn->qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma recv, rc = 0x%x\n", rc);
	}
	return (rc);
}

static int
nvmf_post_rdma_send(struct spdk_nvmf_conn *conn,
		    struct spdk_nvmf_request *req)
{
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req = get_rdma_req(req);
	int rc;

	/* restore the SGL length that may have been modified */
	rdma_req->recv_sgl[1].length = rdma_req->bb_len;

	nvmf_ibv_send_wr_init(&wr, NULL, &rdma_req->send_sgl, (uint64_t)rdma_req,
			      IBV_WR_SEND, IBV_SEND_SIGNALED);

	spdk_trace_record(TRACE_NVMF_IO_COMPLETE, 0, 0, (uint64_t)req, 0);
	rc = ibv_post_send(rdma_conn->qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma send for NVMf completion, rc = 0x%x\n", rc);
	}
	return (rc);
}

int
spdk_nvmf_rdma_request_complete(struct spdk_nvmf_conn *conn,
				struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	int ret;

	/* Was the command successful? */
	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* Need to transfer data via RDMA Write */
		ret = nvmf_post_rdma_write(conn, req);
		if (ret) {
			SPDK_ERRLOG("Unable to post rdma write tx descriptor\n");
			return -1;
		}
	}

	ret = nvmf_post_rdma_send(conn, req);
	if (ret) {
		SPDK_ERRLOG("Unable to send response capsule\n");
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_rdma_request_release(struct spdk_nvmf_conn *conn,
			       struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		struct spdk_nvmf_capsule_cmd *capsule = &req->cmd->nvmf_cmd;

		if (capsule->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			/* Special case: connect is always the first capsule and new
			 * work queue entries are allocated in response to this command.
			 * Instead of re-posting this entry, just free it.
			 */
			spdk_nvmf_rdma_free_req(req);
			return 0;
		}
	}

	if (nvmf_post_rdma_recv(conn, req)) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return -1;
	}

	return 0;
}

int
spdk_nvmf_rdma_alloc_reqs(struct spdk_nvmf_conn *conn)
{
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req;
	int i;

	for (i = 0; i < rdma_conn->queue_depth; i++) {
		rdma_req = alloc_rdma_req(conn);
		if (rdma_req == NULL) {
			goto fail;
		}

		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rdma_req %p: req %p, rsp %p\n",
			      rdma_req, &rdma_req->req,
			      rdma_req->req.rsp);

		if (nvmf_post_rdma_recv(conn, &rdma_req->req)) {
			SPDK_ERRLOG("Unable to post connection rx desc\n");
			goto fail;
		}

		STAILQ_INSERT_TAIL(&rdma_conn->rdma_reqs, rdma_req, link);
	}

	return 0;

fail:
	STAILQ_FOREACH(rdma_req, &rdma_conn->rdma_reqs, link) {
		STAILQ_REMOVE(&rdma_conn->rdma_reqs, rdma_req, spdk_nvmf_rdma_request, link);
		free_rdma_req(rdma_req);
	}

	return -ENOMEM;
}

static int
nvmf_rdma_connect(struct rdma_cm_event *event)
{
	struct spdk_nvmf_host		*host;
	struct spdk_nvmf_fabric_intf	*fabric_intf;
	struct rdma_cm_id		*conn_id;
	struct spdk_nvmf_rdma_conn	*rdma_conn = NULL;
	struct spdk_nvmf_conn		*conn;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct ibv_device_attr		ibdev_attr;
	struct sockaddr_in		*addr;
	struct rdma_conn_param		*host_event_data = NULL;
	struct rdma_conn_param		ctrlr_event_data;
	struct spdk_nvmf_rdma_accept_private_data accept_data;
	uint16_t			sts = 0;
	char addr_str[INET_ADDRSTRLEN];
	int 				rc, qp_depth, rw_depth;


	/* Check to make sure we know about this rdma device */
	if (event->id == NULL) {
		SPDK_ERRLOG("connect request: missing cm_id\n");
		goto err0;
	}
	conn_id = event->id;

	if (conn_id->verbs == NULL) {
		SPDK_ERRLOG("connect request: missing cm_id ibv_context\n");
		goto err0;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Connect Recv on fabric intf name %s, dev_name %s\n",
		      conn_id->verbs->device->name, conn_id->verbs->device->dev_name);
	addr = (struct sockaddr_in *)rdma_get_local_addr(conn_id);
	inet_ntop(AF_INET, &(addr->sin_addr), addr_str, INET_ADDRSTRLEN);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Connect Route: local addr %s\n",
		      addr_str);

	fabric_intf = spdk_nvmf_port_find_fabric_intf_by_addr(addr_str);
	if (fabric_intf == NULL) {
		SPDK_ERRLOG("connect request: rdma device does not exist!\n");
		goto err1;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Found existing RDMA Device %p\n", fabric_intf);

	/* validate remote address is within a provisioned initiator group */
	addr = (struct sockaddr_in *)rdma_get_peer_addr(conn_id);
	inet_ntop(AF_INET, &(addr->sin_addr), addr_str, INET_ADDRSTRLEN);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Connect Route: peer addr %s\n",
		      addr_str);

	host = spdk_nvmf_host_find_by_addr(addr_str);
	if (host == NULL) {
		SPDK_ERRLOG("connect request: remote host addr not provisioned!\n");
		goto err1;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "Found approved remote host %p\n", host);

	/* Init the NVMf rdma transport connection */
	rdma_conn = allocate_rdma_conn();
	if (rdma_conn == NULL) {
		SPDK_ERRLOG("Error on nvmf connection creation\n");
		goto err1;
	}

	conn = &rdma_conn->conn;

	/*
	 * Save the rdma_cm context id in our fabric connection context.  This
	 * ptr can be used to get indirect access to ibv_context (cm_id->verbs)
	 * and also to ibv_device (cm_id->verbs->device)
	 */
	rdma_conn->cm_id = conn_id;
	conn_id->context = conn;

	rc = ibv_query_device(conn_id->verbs, &ibdev_attr);
	if (rc) {
		SPDK_ERRLOG(" Failed on query for device attributes\n");
		goto err1;
	}

	host_event_data = &event->param.conn;
	if (host_event_data->private_data == NULL ||
	    host_event_data->private_data_len < sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		/* No private data, so use defaults. */
		qp_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
		rw_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	} else {
		const struct spdk_nvmf_rdma_request_private_data *private_data = host_event_data->private_data;
		qp_depth = nvmf_min(private_data->hrqsize, private_data->hsqsize);
		rw_depth = host_event_data->initiator_depth;
	}
	qp_depth = nvmf_min(qp_depth, ibdev_attr.max_qp_wr);
	rw_depth = nvmf_min(rw_depth, ibdev_attr.max_qp_rd_atom);
	rdma_conn->queue_depth = nvmf_min(qp_depth, rw_depth);

	rc = nvmf_rdma_queue_init(conn, conn_id->verbs);
	if (rc) {
		SPDK_ERRLOG("connect request: rdma conn init failure!\n");
		goto err1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "NVMf fabric connection initialized\n");

	STAILQ_INIT(&rdma_conn->rdma_reqs);

	/* Allocate 1 buffer suitable for the CONNECT capsule.
	 * Once that is received, the full queue depth will be allocated.
	 */
	rdma_req = alloc_rdma_req(conn);
	if (nvmf_post_rdma_recv(conn, &rdma_req->req)) {
		SPDK_ERRLOG("Unable to post connection rx desc\n");
		goto err1;
	}
	STAILQ_INSERT_TAIL(&rdma_conn->rdma_reqs, rdma_req, link);

	/* Add this RDMA connection to the global list until a CONNECT capsule
	 * is received. */
	TAILQ_INSERT_TAIL(&g_pending_conns, rdma_conn, link);

	accept_data.recfmt = 0;
	accept_data.crqsize = rdma_conn->queue_depth;
	if (host_event_data != NULL) {
		memcpy(&ctrlr_event_data, host_event_data, sizeof(ctrlr_event_data));
	}
	ctrlr_event_data.private_data = &accept_data;
	ctrlr_event_data.private_data_len = sizeof(accept_data);
	if (conn_id->ps == RDMA_PS_TCP) {
		ctrlr_event_data.responder_resources = 0; /* We accept 0 reads from the host */
		ctrlr_event_data.initiator_depth = rdma_conn->queue_depth;
	}

	rc = rdma_accept(event->id, &ctrlr_event_data);
	if (rc) {
		SPDK_ERRLOG("Error on rdma_accept\n");
		goto err1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Sent back the accept\n");

	return 0;

err1: {
		struct spdk_nvmf_rdma_reject_private_data rej_data;

		rej_data.status.sc = sts;
		rdma_reject(conn_id, &ctrlr_event_data, sizeof(rej_data));
		free(rdma_conn);
	}
err0:
	return -1;
}

static int
nvmf_rdma_disconnect(struct rdma_cm_event *event)
{
	struct rdma_cm_id		*conn_id;
	struct spdk_nvmf_conn	*conn;

	/* Check to make sure we know about this rdma device */
	if (event->id == NULL) {
		SPDK_ERRLOG("disconnect request: missing cm_id\n");
		goto err0;
	}
	conn_id = event->id;

	conn = conn_id->context;
	if (conn == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		goto err0;
	}

	/*
	 * Modify connection state to trigger async termination
	 * next time the connection poller executes
	 */
	conn->state = CONN_STATE_FABRIC_DISCONNECT;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rdma connection %p state set to CONN_STATE_FABRIC_DISCONNECT\n",
		      conn);
	return 0;
err0:
	return -1;
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
nvmf_recv(struct spdk_nvmf_rdma_request *rdma_req, struct ibv_wc *wc)
{
	int ret;
	struct spdk_nvmf_request *req;

	if (wc->byte_len < sizeof(struct spdk_nvmf_capsule_cmd)) {
		SPDK_ERRLOG("recv length %u less than capsule header\n", wc->byte_len);
		return -1;
	}

	req = &rdma_req->req;

	ret = spdk_nvmf_request_prep_data(req,
					  rdma_req->bb, wc->byte_len - sizeof(struct spdk_nvmf_capsule_cmd),
					  rdma_req->bb, rdma_req->recv_sgl[1].length);
	if (ret < 0) {
		SPDK_ERRLOG("prep_data failed\n");
		return spdk_nvmf_request_complete(req);
	}

	if (ret == 0) {
		/* Data is available now; execute command immediately. */
		ret = spdk_nvmf_request_exec(req);
		if (ret < 0) {
			SPDK_ERRLOG("Command execution failed\n");
			return -1;
		}

		return 1;
	}

	/*
	 * Pending transfer from host to controller; command will continue
	 * once transfer is complete.
	 */
	return 0;
}

static void
nvmf_rdma_accept(struct rte_timer *timer, void *arg)
{
	struct rdma_cm_event		*event;
	int				rc;
	struct spdk_nvmf_rdma_conn	*rdma_conn, *tmp;
	struct spdk_nvmf_rdma_request	*rdma_req;

	if (g_rdma.acceptor_event_channel == NULL) {
		return;
	}

	/* Process pending connections for incoming capsules. The only capsule
	 * this should ever find is a CONNECT request. */
	TAILQ_FOREACH_SAFE(rdma_conn, &g_pending_conns, link, tmp) {
		struct ibv_wc wc;

		rc = ibv_poll_cq(rdma_conn->cq, 1, &wc);
		if (rc == 0) {
			continue;
		} else if (rc < 0) {
			SPDK_ERRLOG("Error polling RDMA completion queue: %d (%s)\n",
				    errno, strerror(errno));
			TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
			nvmf_rdma_conn_cleanup(&rdma_conn->conn);
			continue;
		}

		if (wc.status) {
			SPDK_ERRLOG("Error polling RDMA completion queue: %d (%s)\n",
				    wc.status, ibv_wc_status_str(wc.status));
			TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
			nvmf_rdma_conn_cleanup(&rdma_conn->conn);
			continue;
		}

		if (wc.opcode == IBV_WC_RECV) {
			/* New incoming capsule. */
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "Received new capsule on pending connection.\n");
			spdk_trace_record(TRACE_NVMF_IO_START, 0, 0, wc.wr_id, 0);
			rdma_req = (struct spdk_nvmf_rdma_request *)wc.wr_id;
			rc = nvmf_recv(rdma_req, &wc);
			if (rc < 0) {
				SPDK_ERRLOG("nvmf_recv processing failure\n");
				TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
				nvmf_rdma_conn_cleanup(&rdma_conn->conn);
				continue;
			} else if (rc > 0) {
				TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
			}
		} else if (wc.opcode == IBV_WC_RDMA_READ) {
			/* A previously received capsule finished grabbing
			 * its associated data */
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "RDMA read for a request on the pending connection completed\n");
			rdma_req = (struct spdk_nvmf_rdma_request *)wc.wr_id;
			spdk_trace_record(TRACE_RDMA_READ_COMPLETE, 0, 0, (uint64_t)&rdma_req->req, 0);
			rc = spdk_nvmf_request_exec(&rdma_req->req);
			if (rc) {
				SPDK_ERRLOG("request_exec error %d after RDMA Read completion\n", rc);
				TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
				nvmf_rdma_conn_cleanup(&rdma_conn->conn);
				continue;
			}
			TAILQ_REMOVE(&g_pending_conns, rdma_conn, link);
		} else {
			/* No other completion types are expected here */
			SPDK_ERRLOG("Unexpected RDMA completion.\n");
			continue;
		}
	}

	while (1) {
		rc = rdma_get_cm_event(g_rdma.acceptor_event_channel, &event);
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
				break;
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

int nvmf_acceptor_start(void)
{
	struct sockaddr_in	addr;
	uint16_t		sin_port;
	int			rc;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = g_nvmf_tgt.sin_port;

	/* create an event channel with rdmacm to receive
	   connection oriented requests and notifications */
	g_rdma.acceptor_event_channel = rdma_create_event_channel();
	if (g_rdma.acceptor_event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed\n");
		return -1;
	}
	rc = fcntl(g_rdma.acceptor_event_channel->fd, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("fcntl to set fd to non-blocking failed\n");
		goto create_id_error;
	}

	rc = rdma_create_id(g_rdma.acceptor_event_channel, &g_rdma.acceptor_listen_id, NULL, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		goto create_id_error;
	}

	rc = rdma_bind_addr(g_rdma.acceptor_listen_id, (struct sockaddr *)&addr);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_bind_addr() failed\n");
		goto listen_error;
	}

	rc = rdma_listen(g_rdma.acceptor_listen_id, 10); /* 10 = backlog */
	if (rc < 0) {
		SPDK_ERRLOG("rdma_listen() failed\n");
		goto listen_error;
	}
	sin_port = ntohs(rdma_get_src_port(g_rdma.acceptor_listen_id));
	SPDK_NOTICELOG("*** NVMf Target Listening on port %d ***\n", sin_port);

	rte_timer_init(&g_rdma.acceptor_timer);
	rte_timer_reset(&g_rdma.acceptor_timer, ACCEPT_TIMEOUT, PERIODICAL,
			rte_lcore_id(), nvmf_rdma_accept, NULL);
	return (rc);

listen_error:
	rdma_destroy_id(g_rdma.acceptor_listen_id);
create_id_error:
	rdma_destroy_event_channel(g_rdma.acceptor_event_channel);
	return -1;
}

void nvmf_acceptor_stop(void)
{
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "nvmf_acceptor_stop: shutdown\n");
	rte_timer_stop_sync(&g_rdma.acceptor_timer);
}

/*

Initialize with RDMA transport.  Query OFED for device list.

*/
int
spdk_nvmf_rdma_init(void)
{
	struct ibv_device **dev_list;
	struct ibv_context *ibdev_ctx = NULL;
	struct ibv_device_attr ibdev_attr;
	int num_of_rdma_devices;
	int num_devices_found = 0;
	int i, ret;

	SPDK_NOTICELOG("*** RDMA Transport Init ***\n");

	dev_list = ibv_get_device_list(&num_of_rdma_devices);
	if (!dev_list) {
		SPDK_ERRLOG(" No RDMA verbs devices found\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "    %d RDMA verbs device(s) discovered\n", num_of_rdma_devices);

	/* Look through the list of devices for one we support */
	for (i = 0; i < num_of_rdma_devices; i++) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, " RDMA Device %d:\n", i);
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "   Node type: %d\n", (int)dev_list[i]->node_type);
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "   Transport type: %d\n", (int)dev_list[i]->transport_type);
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "   Name: %s\n", dev_list[i]->name);
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "   Device Name: %s\n", dev_list[i]->dev_name);

		ibdev_ctx = ibv_open_device(dev_list[i]);
		if (!ibdev_ctx) {
			SPDK_ERRLOG(" No rdma context returned for device %d\n", i);
			continue;
		}

		ret = ibv_query_device(ibdev_ctx, &ibdev_attr);
		if (ret) {
			SPDK_ERRLOG(" Failed on query for device %d\n", i);
			ibv_close_device(ibdev_ctx);
			continue;
		}

		/* display device specific attributes */
		SPDK_TRACELOG(SPDK_TRACE_RDMA, " RDMA Device Attributes:\n");
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max MR Size: 0x%llx\n", (long long int)ibdev_attr.max_mr_size);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Page Size Cap: 0x%llx\n",
			      (long long int)ibdev_attr.page_size_cap);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max QPs: 0x%x\n", (int)ibdev_attr.max_qp);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max QP WRs: 0x%x\n", (int)ibdev_attr.max_qp_wr);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max SGE: 0x%x\n", (int)ibdev_attr.max_sge);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max CQs: 0x%x\n", (int)ibdev_attr.max_cq);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max CQE per CQ: 0x%x\n", (int)ibdev_attr.max_cqe);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max MR: 0x%x\n", (int)ibdev_attr.max_mr);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max PD: 0x%x\n", (int)ibdev_attr.max_pd);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max QP RD Atom: 0x%x\n", (int)ibdev_attr.max_qp_rd_atom);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max QP Init RD Atom: 0x%x\n",
			      (int)ibdev_attr.max_qp_init_rd_atom);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max Res RD Atom: 0x%x\n", (int)ibdev_attr.max_res_rd_atom);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max EE: 0x%x\n", (int)ibdev_attr.max_ee);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max SRQ: 0x%x\n", (int)ibdev_attr.max_srq);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max SRQ WR: 0x%x\n", (int)ibdev_attr.max_srq_wr);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max SRQ SGE: 0x%x\n", (int)ibdev_attr.max_srq_sge);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Max PKeys: 0x%x\n", (int)ibdev_attr.max_pkeys);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "   Phys Port Cnt: %d\n", (int)ibdev_attr.phys_port_cnt);

		num_devices_found++;
	}

	ibv_free_device_list(dev_list);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "    %d Fabric Intf(s) active\n", num_devices_found);
	return num_devices_found;
}

int
spdk_nvmf_rdma_fini(void)
{
	/* Nothing to do */
	return 0;
}

int
nvmf_check_rdma_completions(struct spdk_nvmf_conn *conn)
{
	struct ibv_wc wc;
	struct spdk_nvmf_rdma_conn *rdma_conn = get_rdma_conn(conn);
	struct spdk_nvmf_rdma_request *rdma_req;
	struct spdk_nvmf_request *req;
	int rc;
	int cq_count = 0;
	int i;

	for (i = 0; i < rdma_conn->queue_depth; i++) {
		rc = ibv_poll_cq(rdma_conn->cq, 1, &wc);
		if (rc == 0) // No completions at this time
			break;

		if (rc < 0) {
			SPDK_ERRLOG("Poll CQ error!(%d): %s\n",
				    errno, strerror(errno));
			return -1;
		}

		/* OK, process the single successful cq event */
		cq_count += rc;

		if (wc.status) {
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ completion error status %d (%s), exiting handler\n",
				      wc.status, ibv_wc_status_str(wc.status));
			return -1;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ send completion\n");
			rdma_req = (struct spdk_nvmf_rdma_request *)wc.wr_id;
			req = &rdma_req->req;
			if (spdk_nvmf_rdma_request_release(conn, req)) {
				return -1;
			}
			break;

		case IBV_WC_RDMA_WRITE:
			/*
			 * Will get this event only if we set IBV_SEND_SIGNALED
			 * flag in rdma_write, to trace rdma write latency
			 */
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ rdma write completion\n");
			rdma_req = (struct spdk_nvmf_rdma_request *)wc.wr_id;
			req = &rdma_req->req;
			spdk_trace_record(TRACE_RDMA_WRITE_COMPLETE, 0, 0, (uint64_t)req, 0);
			break;

		case IBV_WC_RDMA_READ:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ rdma read completion\n");
			rdma_req = (struct spdk_nvmf_rdma_request *)wc.wr_id;
			req = &rdma_req->req;
			spdk_trace_record(TRACE_RDMA_READ_COMPLETE, 0, 0, (uint64_t)req, 0);
			rc = spdk_nvmf_request_exec(req);
			if (rc) {
				SPDK_ERRLOG("request_exec error %d after RDMA Read completion\n", rc);
				return -1;
			}
			break;

		case IBV_WC_RECV:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ recv completion\n");
			spdk_trace_record(TRACE_NVMF_IO_START, 0, 0, wc.wr_id, 0);
			rdma_req = (struct spdk_nvmf_rdma_request *)wc.wr_id;
			rc = nvmf_recv(rdma_req, &wc);
			if (rc < 0) {
				SPDK_ERRLOG("nvmf_recv processing failure\n");
				return -1;
			}
			break;

		default:
			SPDK_ERRLOG("Poll cq opcode type unknown!!!!! completion\n");
			return -1;
		}
	}
	return cq_count;
}

SPDK_LOG_REGISTER_TRACE_FLAG("rdma", SPDK_TRACE_RDMA)
