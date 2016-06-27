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
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_malloc.h>

#include "conn.h"
#include "rdma.h"
#include "nvmf.h"
#include "port.h"
#include "host.h"
#include "spdk/log.h"
#include "spdk/trace.h"

#define ACCEPT_TIMEOUT (rte_get_timer_hz() >> 10) /* ~1ms */
#define MAX_RDMA_DEVICES 4
#define MAX_SESSIONS_PER_DEVICE	1 /* for now accept only single session per device */

/*
 RDMA Connection Resouce Defaults
 */
#define NVMF_DEFAULT_TX_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

static int alloc_qp_rx_desc(struct spdk_nvmf_conn *conn);
static int alloc_qp_tx_desc(struct spdk_nvmf_conn *conn);

struct spdk_nvmf_rdma {
	struct rte_timer		acceptor_timer;
	struct rdma_event_channel	*acceptor_event_channel;
	struct rdma_cm_id		*acceptor_listen_id;
};

static struct spdk_nvmf_rdma g_rdma = { };

static void
nvmf_active_tx_desc(struct nvme_qp_tx_desc *tx_desc)
{
	struct spdk_nvmf_conn *conn;

	RTE_VERIFY(tx_desc != NULL);
	conn = tx_desc->conn;
	RTE_VERIFY(conn != NULL);

	STAILQ_REMOVE(&conn->rdma.qp_tx_desc, tx_desc, nvme_qp_tx_desc, link);
	STAILQ_INSERT_TAIL(&conn->rdma.qp_tx_active_desc, tx_desc, link);
}

static void
nvmf_deactive_tx_desc(struct nvme_qp_tx_desc *tx_desc)
{
	struct spdk_nvmf_conn *conn;

	RTE_VERIFY(tx_desc != NULL);
	conn = tx_desc->conn;
	RTE_VERIFY(tx_desc->conn != NULL);

	STAILQ_REMOVE(&conn->rdma.qp_tx_active_desc, tx_desc, nvme_qp_tx_desc, link);
	STAILQ_INSERT_TAIL(&conn->rdma.qp_tx_desc, tx_desc, link);
}

static int
nvmf_rdma_queue_init(struct spdk_nvmf_conn *conn,
		     struct ibv_context *verbs)
{
	int			rc;
	struct ibv_qp_init_attr	attr;

	if (conn->rdma.ctx) {
		SPDK_ERRLOG("context already set!\n");
		goto return_error;
	}
	conn->rdma.ctx = verbs;

	conn->rdma.comp_channel = ibv_create_comp_channel(verbs);
	if (!conn->rdma.comp_channel) {
		SPDK_ERRLOG("create completion channel error!\n");
		goto return_error;
	}
	rc = fcntl(conn->rdma.comp_channel->fd, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("fcntl to set comp channel to non-blocking failed\n");
		goto cq_error;
	}

	/*
	 * Size the CQ to handle Rx completions + Tx completions + rdma_read or write
	 * completions.  Three times the target connection SQ depth should be more
	 * than enough.
	 */
	conn->rdma.cq = ibv_create_cq(verbs, (conn->rdma.sq_depth * 3), conn, conn->rdma.comp_channel, 0);
	if (!conn->rdma.cq) {
		SPDK_ERRLOG("create cq error!\n");
		goto cq_error;
	}

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.send_cq		= conn->rdma.cq;
	attr.recv_cq		= conn->rdma.cq;
	attr.cap.max_send_wr	= conn->rdma.cq_depth;
	attr.cap.max_recv_wr	= conn->rdma.sq_depth;
	attr.cap.max_send_sge	= NVMF_DEFAULT_TX_SGE;
	attr.cap.max_recv_sge	= NVMF_DEFAULT_RX_SGE;

	rc = rdma_create_qp(conn->rdma.cm_id, NULL, &attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed\n");
		goto cq_error;
	}
	conn->rdma.qp = conn->rdma.cm_id->qp;

	return 0;

cq_error:
	ibv_destroy_comp_channel(conn->rdma.comp_channel);
return_error:
	return -1;
}

static void
free_qp_desc(struct spdk_nvmf_conn *conn)
{
	struct nvme_qp_rx_desc	*tmp_rx;
	struct nvme_qp_tx_desc	*tmp_tx;
	int			rc;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");

	STAILQ_FOREACH(tmp_rx, &conn->rdma.qp_rx_desc, link) {
		STAILQ_REMOVE(&conn->rdma.qp_rx_desc, tmp_rx, nvme_qp_rx_desc, link);

		rc = rdma_dereg_mr(tmp_rx->bb_mr);
		if (rc) {
			SPDK_ERRLOG("Unable to de-register rx bb mr\n");
		}

		rte_free(tmp_rx->bb);

		rc = rdma_dereg_mr(tmp_rx->cmd_mr);
		if (rc) {
			SPDK_ERRLOG("Unable to de-register rx mr\n");
		}

		rte_free(tmp_rx);
	}

	STAILQ_FOREACH(tmp_tx, &conn->rdma.qp_tx_desc, link) {
		STAILQ_REMOVE(&conn->rdma.qp_tx_desc, tmp_tx, nvme_qp_tx_desc, link);

		rc = rdma_dereg_mr(tmp_tx->rsp_mr);
		if (rc) {
			SPDK_ERRLOG("Unable to de-register tx mr\n");
		}

		rte_free(tmp_tx);
	}
}

static void
nvmf_drain_cq(struct spdk_nvmf_conn *conn)
{
	struct ibv_wc wc;

	/* drain the cq before destruction */
	while (ibv_poll_cq(conn->rdma.cq, 1, &wc) > 0) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "drain cq event\n");
		//ibv_ack_cq_events(conn->cq, 1);
	}

}

void
nvmf_rdma_conn_cleanup(struct spdk_nvmf_conn *conn)
{
	struct nvme_qp_tx_desc *pending_desc, *active_desc;
	int rc;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Enter\n");

	rdma_destroy_qp(conn->rdma.cm_id);

	while (!STAILQ_EMPTY(&conn->rdma.qp_pending_desc)) {
		pending_desc = STAILQ_FIRST(&conn->rdma.qp_pending_desc);
		STAILQ_REMOVE_HEAD(&conn->rdma.qp_pending_desc, link);
		STAILQ_INSERT_TAIL(&conn->rdma.qp_tx_desc, pending_desc, link);
	}

	/* Remove tx_desc from qp_tx_active_desc list to qp_tx_desc list */
	while (!STAILQ_EMPTY(&conn->rdma.qp_tx_active_desc)) {
		active_desc = STAILQ_FIRST(&conn->rdma.qp_tx_active_desc);
		STAILQ_REMOVE_HEAD(&conn->rdma.qp_tx_active_desc, link);
		STAILQ_INSERT_TAIL(&conn->rdma.qp_tx_desc, active_desc, link);
	}

	free_qp_desc(conn);

	nvmf_drain_cq(conn);
	rc = ibv_destroy_cq(conn->rdma.cq);
	if (rc) {
		SPDK_ERRLOG("ibv_destroy_cq error\n");
	}

	ibv_destroy_comp_channel(conn->rdma.comp_channel);
	rdma_destroy_id(conn->rdma.cm_id);
}

static void
nvmf_trace_ibv_sge(struct ibv_sge *sg_list)
{
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "local addr %p\n", (void *)sg_list->addr);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "length %x\n", sg_list->length);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "lkey %x\n", sg_list->lkey);
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
		wr->wr.rdma.rkey = req->rkey;
		wr->wr.rdma.remote_addr = req->remote_addr;

		SPDK_TRACELOG(SPDK_TRACE_RDMA, "rkey %x\n", wr->wr.rdma.rkey);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "remote addr %p\n",
			      (void *)wr->wr.rdma.remote_addr);
	}

	nvmf_trace_ibv_sge(wr->sg_list);
}

int
nvmf_post_rdma_read(struct spdk_nvmf_conn *conn,
		    struct spdk_nvmf_request *req)
{
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct nvme_qp_tx_desc *tx_desc = req->tx_desc;
	struct nvme_qp_rx_desc *rx_desc = req->rx_desc;

	int rc;

	if (rx_desc == NULL) {
		SPDK_ERRLOG("Rx descriptor does not exist at rdma read!\n");
		return -1;
	}

	/*
	 * Queue the rdma read if it would exceed max outstanding
	 * RDMA read limit.
	 */
	if (conn->rdma.pending_rdma_read_count == conn->rdma.initiator_depth) {
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "Insert rdma read into pending queue: tx_desc %p\n",
			      tx_desc);
		STAILQ_REMOVE(&conn->rdma.qp_tx_active_desc, tx_desc, nvme_qp_tx_desc, link);
		STAILQ_INSERT_TAIL(&conn->rdma.qp_pending_desc, tx_desc, link);
		return 0;
	}
	conn->rdma.pending_rdma_read_count++;

	/* temporarily adjust SGE to only copy what the host is prepared to send. */
	rx_desc->bb_sgl.length = req->length;

	nvmf_ibv_send_wr_init(&wr, req, &rx_desc->bb_sgl, (uint64_t)tx_desc,
			      IBV_WR_RDMA_READ, IBV_SEND_SIGNALED);

	spdk_trace_record(TRACE_RDMA_READ_START, 0, 0, (uint64_t)req, 0);
	rc = ibv_post_send(conn->rdma.qp, &wr, &bad_wr);
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
	struct nvme_qp_tx_desc *tx_desc = req->tx_desc;
	struct nvme_qp_rx_desc *rx_desc = req->rx_desc;
	int rc;

	if (rx_desc == NULL) {
		SPDK_ERRLOG("Rx descriptor does not exist at rdma write!\n");
		return -1;
	}

	/* temporarily adjust SGE to only copy what the host is prepared to receive. */
	rx_desc->bb_sgl.length = req->length;

	nvmf_ibv_send_wr_init(&wr, req, &rx_desc->bb_sgl, (uint64_t)tx_desc,
			      IBV_WR_RDMA_WRITE, 0);

	spdk_trace_record(TRACE_RDMA_WRITE_START, 0, 0, (uint64_t)req, 0);
	rc = ibv_post_send(conn->rdma.qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma write send, rc = 0x%x\n", rc);
	}
	return (rc);
}

static int
nvmf_post_rdma_recv(struct spdk_nvmf_conn *conn,
		    struct nvme_qp_rx_desc *rx_desc)
{
	struct ibv_recv_wr wr, *bad_wr = NULL;
	int rc;

	/* Update Connection SQ Tracking, increment
	   the SQ head counter opening up another
	   RX recv slot.
	*/
	conn->sq_head < (conn->rdma.sq_depth - 1) ? (conn->sq_head++) : (conn->sq_head = 0);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sq_head %x, sq_depth %x\n", conn->sq_head, conn->rdma.sq_depth);

	wr.wr_id = (uintptr_t)rx_desc;
	wr.next = NULL;
	wr.sg_list = &rx_desc->recv_sgl;
	wr.num_sge = 1;

	nvmf_trace_ibv_sge(&rx_desc->recv_sgl);

	/* for I/O queues we add bb sgl for in-capsule data use */
	if (conn->type == CONN_TYPE_IOQ) {
		wr.num_sge = 2;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sgl2 local addr %p\n",
			      (void *)rx_desc->bb_sgl.addr);
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sgl2 length %x\n", rx_desc->bb_sgl.length);
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sgl2 lkey %x\n", rx_desc->bb_sgl.lkey);
	}

	rc = ibv_post_recv(conn->rdma.qp, &wr, &bad_wr);
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
	struct nvme_qp_tx_desc *tx_desc = req->tx_desc;
	struct nvme_qp_rx_desc *rx_desc = req->rx_desc;
	int rc;

	RTE_VERIFY(rx_desc != NULL);

	/* restore the SGL length that may have been modified */
	rx_desc->bb_sgl.length = rx_desc->bb_len;

	/* Re-post recv */
	if (nvmf_post_rdma_recv(conn, rx_desc)) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return -1;
	}

	nvmf_ibv_send_wr_init(&wr, NULL, &tx_desc->send_sgl, (uint64_t)tx_desc,
			      IBV_WR_SEND, IBV_SEND_SIGNALED);

	SPDK_TRACELOG(SPDK_TRACE_RDMA, "tx_desc %p: req %p, rsp %p\n",
		      tx_desc, req, req->rsp);

	spdk_trace_record(TRACE_NVMF_IO_COMPLETE, 0, 0, (uint64_t)req, 0);
	rc = ibv_post_send(conn->rdma.qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma send for NVMf completion, rc = 0x%x\n", rc);
	}
	return (rc);
}

int
spdk_nvmf_rdma_request_complete(struct spdk_nvmf_conn *conn, struct spdk_nvmf_request *req)
{
	struct nvme_qp_tx_desc *tx_desc = req->tx_desc;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	int ret;

	/* Was the command successful? */
	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		/* Need to transfer data via RDMA Write */
		ret = nvmf_post_rdma_write(conn, req);
		if (ret) {
			SPDK_ERRLOG("Unable to post rdma write tx descriptor\n");
			goto command_fail;
		}
	}

	ret = nvmf_post_rdma_send(conn, req);
	if (ret) {
		SPDK_ERRLOG("Unable to send response capsule\n");
		goto command_fail;
	}

	return 0;

command_fail:
	nvmf_deactive_tx_desc(tx_desc);
	return -1;
}

static int
nvmf_rdma_connect(struct rdma_cm_event *event)
{
	struct spdk_nvmf_host		*host;
	struct spdk_nvmf_fabric_intf	*fabric_intf;
	struct rdma_cm_id		*conn_id;
	struct spdk_nvmf_conn		*conn;
	struct nvme_qp_rx_desc		*rx_desc;
	struct ibv_device_attr		ibdev_attr;
	struct sockaddr_in		*addr;
	struct rdma_conn_param		*param = NULL;
	const union spdk_nvmf_rdma_private_data	*pdata = NULL;
	union spdk_nvmf_rdma_private_data	acc_rej_pdata;
	uint16_t			sts = 0;
	char addr_str[INET_ADDRSTRLEN];
	int 		rc;


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
	conn = spdk_nvmf_allocate_conn();
	if (conn == NULL) {
		SPDK_ERRLOG("Error on nvmf connection creation\n");
		goto err1;
	}

	/*
	 * Save the rdma_cm context id in our fabric connection context.  This
	 * ptr can be used to get indirect access to ibv_context (cm_id->verbs)
	 * and also to ibv_device (cm_id->verbs->device)
	 */
	conn->rdma.cm_id = conn_id;
	conn_id->context = conn;

	/* check for private data */
	if (event->param.conn.private_data_len < sizeof(union spdk_nvmf_rdma_private_data)) {
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "No private nvmf connection setup data\n");
		conn->rdma.sq_depth	= SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH; /* assume max default */
		conn->rdma.cq_depth	= SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH; /* assume max default */
	} else {
		pdata = event->param.conn.private_data;
		if (pdata == NULL) {
			SPDK_ERRLOG("Invalid private nvmf connection setup data pointer\n");
			sts = SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT;
			goto err2;
		}

		/* Save private details for later validation and use */
		conn->rdma.sq_depth	= pdata->pd_request.hsqsize;
		conn->rdma.cq_depth	= pdata->pd_request.hrqsize;
		conn->qid		= pdata->pd_request.qid;
		/* double send queue size for R/W commands */
		conn->rdma.cq_depth *= 2;
		if (conn->qid > 0) {
			conn->type	= CONN_TYPE_IOQ;
		}
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Private Data: QID %x\n", conn->qid);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Private Data: CQ Depth %x\n", conn->rdma.cq_depth);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Private Data: SQ Depth %x\n", conn->rdma.sq_depth);
	}

	/* adjust conn settings to device limits */
	rc = ibv_query_device(conn_id->verbs, &ibdev_attr);
	if (rc) {
		SPDK_ERRLOG(" Failed on query for device attributes\n");
		goto err2;
	}

	if (conn->rdma.cq_depth > ibdev_attr.max_cqe) {
		conn->rdma.cq_depth = ibdev_attr.max_cqe;
	}
	if (conn->rdma.sq_depth > ibdev_attr.max_qp_wr) {
		conn->rdma.sq_depth = ibdev_attr.max_qp_wr;
	}
	conn->rdma.sq_depth = nvmf_min(conn->rdma.sq_depth, conn->rdma.cq_depth);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Adjusted CQ Depth %x\n", conn->rdma.cq_depth);
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Adjusted SQ Depth %x\n", conn->rdma.sq_depth);

	if (conn_id->ps == RDMA_PS_TCP) {
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect flow control: %x\n", event->param.conn.flow_control);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect retry count: %x\n", event->param.conn.retry_count);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect rnr retry count: %x\n",
			      event->param.conn.rnr_retry_count);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Responder Resources %x\n",
			      event->param.conn.responder_resources);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Initiator Depth %x\n",
			      event->param.conn.initiator_depth);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect SRQ %x\n", event->param.conn.srq);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect qp_num %x\n", event->param.conn.qp_num);

		conn->rdma.responder_resources = nvmf_min(event->param.conn.responder_resources,
						 ibdev_attr.max_qp_rd_atom);
		conn->rdma.initiator_depth = nvmf_min(event->param.conn.initiator_depth,
						      ibdev_attr.max_qp_init_rd_atom);
		if (event->param.conn.responder_resources != conn->rdma.responder_resources ||
		    event->param.conn.initiator_depth != conn->rdma.initiator_depth) {
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Adjusted Responder Resources %x\n",
				      conn->rdma.responder_resources);
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Adjusted Initiator Depth %x\n",
				      conn->rdma.initiator_depth);
		}
	}

	rc = nvmf_rdma_queue_init(conn, conn_id->verbs);
	if (rc) {
		SPDK_ERRLOG("connect request: rdma conn init failure!\n");
		goto err2;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "NVMf fabric connection initialized\n");

	STAILQ_INIT(&conn->rdma.qp_pending_desc);
	STAILQ_INIT(&conn->rdma.qp_rx_desc);
	STAILQ_INIT(&conn->rdma.qp_tx_desc);
	STAILQ_INIT(&conn->rdma.qp_tx_active_desc);

	/* Allocate AQ QP RX Buffers */
	rc = alloc_qp_rx_desc(conn);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate connection rx desc\n");
		goto err2;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Rx buffers allocated\n");

	/* Allocate AQ QP TX Buffers */
	rc = alloc_qp_tx_desc(conn);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate connection tx desc\n");
		goto err2;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Tx buffers allocated\n");

	/* Post all the RX descriptors */
	STAILQ_FOREACH(rx_desc, &conn->rdma.qp_rx_desc, link) {
		if (nvmf_post_rdma_recv(conn, rx_desc)) {
			SPDK_ERRLOG("Unable to post connection rx desc\n");
			goto err2;
		}
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "RX buffers posted\n");

	rc = spdk_nvmf_startup_conn(conn);
	if (rc) {
		SPDK_ERRLOG("Error on startup connection\n");
		goto err2;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "New Connection Scheduled\n");

	param = &event->param.conn;
	if (conn_id->ps == RDMA_PS_TCP) {
		event->param.conn.responder_resources = conn->rdma.responder_resources;
		event->param.conn.initiator_depth = conn->rdma.initiator_depth;
	}
	if (pdata != NULL) {
		event->param.conn.private_data = &acc_rej_pdata;
		event->param.conn.private_data_len = sizeof(acc_rej_pdata);
		memset((uint8_t *)&acc_rej_pdata, 0, sizeof(acc_rej_pdata));
		acc_rej_pdata.pd_accept.crqsize = conn->rdma.sq_depth;
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept Private Data Length %x\n",
			      param->private_data_len);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept Private Data: recfmt %x\n",
			      pdata->pd_accept.recfmt);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept Private Data: crqsize %x\n",
			      pdata->pd_accept.crqsize);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept flow control: %x\n", param->flow_control);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept retry count: %x\n", param->retry_count);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept rnr retry count: %x\n", param->rnr_retry_count);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept Responder Resources %x\n",
			      param->responder_resources);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept Initiator Depth %x\n", param->initiator_depth);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept SRQ %x\n", param->srq);
		SPDK_TRACELOG(SPDK_TRACE_RDMA, "    Connect Accept qp_num %x\n", param->qp_num);
	}

	rc = rdma_accept(event->id, param);
	if (rc) {
		SPDK_ERRLOG("Error on rdma_accept\n");
		goto err3;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Sent back the accept\n");

	return 0;

err3:
	/* halt the connection thread */
err2:
	/* free the connection and all it's resources */
err1:
	if (pdata != NULL) {
		memset((uint8_t *)&acc_rej_pdata, 0, sizeof(acc_rej_pdata));
		acc_rej_pdata.pd_reject.status.sc = sts;
		rc = rdma_reject(conn_id, &acc_rej_pdata, sizeof(acc_rej_pdata));
	} else {
		rc = rdma_reject(conn_id, NULL, 0);
	}
	if (rc)
		SPDK_ERRLOG("Error on rdma_reject\n");
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

const char *CM_EVENT_STR[] = {
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

static void
nvmf_rdma_accept(struct rte_timer *timer, void *arg)
{
	struct rdma_cm_event		*event;
	int				rc;

	if (g_rdma.acceptor_event_channel == NULL) {
		return;
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
	SPDK_NOTICELOG("\n*** NVMf Target Listening on port %d ***\n", sin_port);

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
nvmf_rdma_init(void)
{
	struct ibv_device **dev_list;
	struct ibv_context *ibdev_ctx = NULL;
	struct ibv_device_attr ibdev_attr;
	int num_of_rdma_devices;
	int num_devices_found = 0;
	int i, ret;

	SPDK_NOTICELOG("\n*** RDMA Transport Init ***\n");

	dev_list = ibv_get_device_list(&num_of_rdma_devices);
	if (!dev_list) {
		SPDK_ERRLOG(" No RDMA verbs devices found\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_RDMA, "    %d RDMA verbs device(s) discovered\n", num_of_rdma_devices);

	/* Look through the list of devices for one we support */
	for (i = 0; dev_list[i] && num_devices_found < MAX_RDMA_DEVICES; i++, ibdev_ctx = NULL) {
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

/* Populate the AQ QP Rx Buffer Resources */
static int
alloc_qp_rx_desc(struct spdk_nvmf_conn *conn)
{
	struct nvme_qp_rx_desc	*rx_desc, *tmp;
	int				i;
	int				rc;

	/* Allocate buffer for rx descriptors (RX WQE + Msg Buffer) */
	for (i = 0; i < conn->rdma.sq_depth; i++) {
		rx_desc = rte_zmalloc("nvmf_rx_desc", sizeof(*rx_desc), 0);
		if (!rx_desc) {
			SPDK_ERRLOG("Unable to get rx desc object\n");
			goto fail;
		}

		rx_desc->cmd_mr = rdma_reg_msgs(conn->rdma.cm_id, &rx_desc->cmd, sizeof(rx_desc->cmd));
		if (rx_desc->cmd_mr == NULL) {
			SPDK_ERRLOG("Unable to register rx desc buffer mr\n");
			goto fail;
		}

		rx_desc->conn = conn;

		/* initialize recv_sgl of tx_desc */
		rx_desc->recv_sgl.addr = (uint64_t)&rx_desc->cmd;
		rx_desc->recv_sgl.length = sizeof(rx_desc->cmd);
		rx_desc->recv_sgl.lkey = rx_desc->cmd_mr->lkey;

		/* pre-assign a data bb (bounce buffer) with each RX descriptor */
		/*
		  For admin queue, assign smaller BB size to support maximum data that
		  would be exchanged related to admin commands.  For IO queue, assign
		  the large BB size that is equal to the maximum I/O transfer supported
		  by the NVMe device. This large BB is also used for in-capsule receive
		  data.
		*/
		if (conn->type == CONN_TYPE_AQ) {
			rx_desc->bb_len = SMALL_BB_MAX_SIZE;
		} else { // for IO queues
			rx_desc->bb_len = LARGE_BB_MAX_SIZE;
		}
		rx_desc->bb = rte_zmalloc("nvmf_bb", rx_desc->bb_len, 0);
		if (!rx_desc->bb) {
			SPDK_ERRLOG("Unable to get %u-byte bounce buffer\n", rx_desc->bb_len);
			goto fail;
		}
		rx_desc->bb_mr = rdma_reg_read(conn->rdma.cm_id,
					       (void *)rx_desc->bb,
					       rx_desc->bb_len);
		if (rx_desc->bb_mr == NULL) {
			SPDK_ERRLOG("Unable to register rx bb mr\n");
			goto fail;
		}

		/* initialize bb_sgl of rx_desc */
		rx_desc->bb_sgl.addr = (uint64_t)rx_desc->bb;
		rx_desc->bb_sgl.length = rx_desc->bb_len;
		rx_desc->bb_sgl.lkey = rx_desc->bb_mr->lkey;

		STAILQ_INSERT_TAIL(&conn->rdma.qp_rx_desc, rx_desc, link);
	}

	return 0;

fail:
	/* cleanup any partial descriptor that failed during init loop */
	if (rx_desc != NULL) {
		if (rx_desc->bb_mr) {
			rc = rdma_dereg_mr(rx_desc->bb_mr);
			if (rc) {
				SPDK_ERRLOG("Unable to de-register rx bb mr\n");
			}
		}

		rte_free(rx_desc->bb);

		if (rx_desc->cmd_mr) {
			rc = rdma_dereg_mr(rx_desc->cmd_mr);
			if (rc) {
				SPDK_ERRLOG("Unable to de-register rx mr\n");
			}
		}

		rte_free(rx_desc);
	}

	STAILQ_FOREACH(tmp, &conn->rdma.qp_rx_desc, link) {
		STAILQ_REMOVE(&conn->rdma.qp_rx_desc, tmp, nvme_qp_rx_desc, link);

		rc = rdma_dereg_mr(tmp->bb_mr);
		if (rc) {
			SPDK_ERRLOG("Unable to de-register rx bb mr\n");
		}

		rte_free(tmp->bb);

		rc = rdma_dereg_mr(tmp->cmd_mr);
		if (rc) {
			SPDK_ERRLOG("Unable to de-register rx mr\n");
		}

		rte_free(tmp);
	}

	return -ENOMEM;
}

/*  Allocate the AQ QP Tx Buffer Resources */
static int
alloc_qp_tx_desc(struct spdk_nvmf_conn *conn)
{
	struct nvme_qp_tx_desc	*tx_desc, *tmp;
	int			i;
	int			rc;

	/* Initialize the tx descriptors */
	for (i = 0; i < conn->rdma.cq_depth; i++) {
		tx_desc = rte_zmalloc("nvmf_tx_desc", sizeof(*tx_desc), 0);
		if (!tx_desc) {
			SPDK_ERRLOG("Unable to get tx desc object\n");
			goto fail;
		}

		tx_desc->rsp_mr = rdma_reg_msgs(conn->rdma.cm_id, &tx_desc->rsp, sizeof(tx_desc->rsp));
		if (tx_desc->rsp_mr == NULL) {
			SPDK_ERRLOG("Unable to register tx desc buffer mr\n");
			goto fail;
		}

		tx_desc->conn = conn;

		/* initialize send_sgl of tx_desc */
		tx_desc->send_sgl.addr = (uint64_t)&tx_desc->rsp;
		tx_desc->send_sgl.length = sizeof(tx_desc->rsp);
		tx_desc->send_sgl.lkey = tx_desc->rsp_mr->lkey;

		/* init request state associated with each tx_desc */
		tx_desc->req.rsp = &tx_desc->rsp;
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "tx_desc %p: req %p, rsp %p\n",
			      tx_desc, &tx_desc->req,
			      tx_desc->req.rsp);

		STAILQ_INSERT_TAIL(&conn->rdma.qp_tx_desc, tx_desc, link);
	}

	return 0;
fail:
	/* cleanup any partial descriptor that failed during init loop */
	if (tx_desc != NULL) {

		if (tx_desc->rsp_mr) {
			rc = rdma_dereg_mr(tx_desc->rsp_mr);
			if (rc) {
				SPDK_ERRLOG("Unable to de-register tx mr\n");
			}
		}

		rte_free(tx_desc);
	}

	STAILQ_FOREACH(tmp, &conn->rdma.qp_tx_desc, link) {
		STAILQ_REMOVE(&conn->rdma.qp_tx_desc, tmp, nvme_qp_tx_desc, link);

		rc = rdma_dereg_mr(tmp->rsp_mr);
		if (rc) {
			SPDK_ERRLOG("Unable to de-register tx mr\n");
		}

		rte_free(tmp);
	}

	return -ENOMEM;
}

static int
nvmf_process_pending_rdma(struct spdk_nvmf_conn *conn)
{
	struct nvme_qp_tx_desc *tx_desc;
	int rc;

	conn->rdma.pending_rdma_read_count--;
	if (!STAILQ_EMPTY(&conn->rdma.qp_pending_desc)) {
		tx_desc = STAILQ_FIRST(&conn->rdma.qp_pending_desc);
		STAILQ_REMOVE_HEAD(&conn->rdma.qp_pending_desc, link);
		STAILQ_INSERT_TAIL(&conn->rdma.qp_tx_active_desc, tx_desc, link);

		SPDK_TRACELOG(SPDK_TRACE_RDMA, "Issue rdma read from pending queue: tx_desc %p\n",
			      tx_desc);

		rc = nvmf_post_rdma_read(conn, &tx_desc->req);
		if (rc) {
			SPDK_ERRLOG("Unable to post pending rdma read descriptor\n");
			return -1;
		}
	}

	return 0;
}


static int
nvmf_recv(struct spdk_nvmf_conn *conn, struct ibv_wc *wc)
{
	struct nvme_qp_rx_desc *rx_desc;
	struct nvme_qp_tx_desc *tx_desc;
	struct spdk_nvmf_request *req;
	int ret;

	rx_desc = (struct nvme_qp_rx_desc *)wc->wr_id;

	if (wc->byte_len < sizeof(struct spdk_nvmf_capsule_cmd)) {
		SPDK_ERRLOG("recv length less than capsule header\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "recv byte count 0x%x\n", wc->byte_len);

	/* get a response buffer */
	if (STAILQ_EMPTY(&conn->rdma.qp_tx_desc)) {
		SPDK_ERRLOG("tx desc pool empty!\n");
		return -1;
	}
	tx_desc = STAILQ_FIRST(&conn->rdma.qp_tx_desc);
	nvmf_active_tx_desc(tx_desc);

	req = &tx_desc->req;
	req->conn = conn;
	req->tx_desc = tx_desc;
	req->rx_desc = rx_desc;
	req->cmd = &rx_desc->cmd;

	ret = spdk_nvmf_request_prep_data(req,
					  rx_desc->bb, wc->byte_len - sizeof(struct spdk_nvmf_capsule_cmd),
					  rx_desc->bb, rx_desc->bb_sgl.length);
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

		return 0;
	}

	/*
	 * Pending transfer from host to controller; command will continue
	 * once transfer is complete.
	 */
	return 0;
}

int
nvmf_check_rdma_completions(struct spdk_nvmf_conn *conn)
{
	struct ibv_wc wc;
	struct nvme_qp_tx_desc *tx_desc;
	struct spdk_nvmf_request *req;
	int rc;
	int cq_count = 0;
	int i;

	for (i = 0; i < conn->rdma.sq_depth; i++) {
		rc = ibv_poll_cq(conn->rdma.cq, 1, &wc);
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
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "CQ completion error status %d, exiting handler\n",
				      wc.status);
			break;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ send completion\n");
			tx_desc = (struct nvme_qp_tx_desc *)wc.wr_id;
			nvmf_deactive_tx_desc(tx_desc);
			break;

		case IBV_WC_RDMA_WRITE:
			/*
			 * Will get this event only if we set IBV_SEND_SIGNALED
			 * flag in rdma_write, to trace rdma write latency
			 */
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ rdma write completion\n");
			tx_desc = (struct nvme_qp_tx_desc *)wc.wr_id;
			req = &tx_desc->req;
			spdk_trace_record(TRACE_RDMA_WRITE_COMPLETE, 0, 0, (uint64_t)req, 0);
			break;

		case IBV_WC_RDMA_READ:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ rdma read completion\n");
			tx_desc = (struct nvme_qp_tx_desc *)wc.wr_id;
			req = &tx_desc->req;
			spdk_trace_record(TRACE_RDMA_READ_COMPLETE, 0, 0, (uint64_t)req, 0);
			rc = spdk_nvmf_request_exec(req);
			if (rc) {
				SPDK_ERRLOG("request_exec error %d after RDMA Read completion\n", rc);
				nvmf_deactive_tx_desc(tx_desc);
				return -1;
			}

			rc = nvmf_process_pending_rdma(conn);
			if (rc) {
				SPDK_ERRLOG("nvmf_process_pending_rdma() failed: %d\n", rc);
				return -1;
			}
			break;

		case IBV_WC_RECV:
			SPDK_TRACELOG(SPDK_TRACE_RDMA, "\nCQ recv completion\n");
			spdk_trace_record(TRACE_NVMF_IO_START, 0, 0, wc.wr_id, 0);
			rc = nvmf_recv(conn, &wc);
			if (rc) {
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
