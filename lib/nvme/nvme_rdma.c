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

/*
 * NVMe over RDMA transport
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>

#include "spdk/assert.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/event.h"
#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"

#include "nvme_internal.h"

#define NVME_RDMA_TIME_OUT_IN_MS 2000
#define NVME_RDMA_RW_BUFFER_SIZE 131072
#define NVME_HOST_ID_DEFAULT "12345679890"
#define NVME_HOST_NQN  "nqn.2016-06.io.spdk:host"

/*
NVME RDMA qpair Resouce Defaults
 */
#define NVME_RDMA_DEFAULT_TX_SGE		2
#define NVME_RDMA_DEFAULT_RX_SGE		1

/* NVMe RDMA transport extensions for spdk_nvme_ctrlr */
struct nvme_rdma_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;

	uint16_t				cntlid;
};

/* NVMe RDMA qpair extensions for spdk_nvme_qpair */
struct nvme_rdma_qpair {
	struct spdk_nvme_qpair			qpair;

	uint16_t				outstanding_reqs;

	struct rdma_event_channel		*cm_channel;

	struct rdma_cm_id			*cm_id;

	uint16_t				max_queue_depth;

	struct	spdk_nvme_rdma_req		*rdma_reqs;

	struct	spdk_nvme_rdma_rsp		*rdma_rsps;

	STAILQ_HEAD(, spdk_nvme_rdma_req)	free_reqs;
};

struct spdk_nvme_rdma_req {
	int					id;

	struct nvme_request 			*req;

	enum spdk_nvme_data_transfer		xfer;

	struct nvme_rdma_qpair			*rqpair;

	struct spdk_nvme_cmd			cmd;

	struct ibv_mr				*cmd_mr;

	struct ibv_sge				send_sgl;

	struct ibv_sge				bb_sgl;

	struct ibv_mr				*bb_mr;

	uint8_t					*bb;

	uint32_t				bb_len;

	STAILQ_ENTRY(spdk_nvme_rdma_req)	link;
};

struct spdk_nvme_rdma_rsp {
	struct spdk_nvme_cpl	rsp;

	struct ibv_mr		*rsp_mr;

	struct ibv_sge		recv_sgl;
};

static inline struct nvme_rdma_qpair *
nvme_rdma_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->transport == SPDK_NVME_TRANSPORT_RDMA);
	return (struct nvme_rdma_qpair *)((uintptr_t)qpair - offsetof(struct nvme_rdma_qpair, qpair));
}

static inline struct nvme_rdma_ctrlr *
nvme_rdma_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->transport == SPDK_NVME_TRANSPORT_RDMA);
	return (struct nvme_rdma_ctrlr *)((uintptr_t)ctrlr - offsetof(struct nvme_rdma_ctrlr, ctrlr));
}

static struct spdk_nvme_rdma_req *
nvme_rdma_req_get(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;

	if (!rqpair || STAILQ_EMPTY(&rqpair->free_reqs)) {
		return NULL;
	}

	rdma_req = STAILQ_FIRST(&rqpair->free_reqs);
	STAILQ_REMOVE(&rqpair->free_reqs, rdma_req, spdk_nvme_rdma_req, link);

	rqpair->outstanding_reqs++;
	return rdma_req;
}

static void
nvme_rdma_req_put(struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_rdma_qpair *rqpair;

	if (!rdma_req) {
		return;
	}

	rqpair = rdma_req->rqpair;
	if (!rqpair) {
		return;
	}

	if (rqpair->outstanding_reqs) {
		rqpair->outstanding_reqs--;
		STAILQ_INSERT_HEAD(&rqpair->free_reqs, rdma_req, link);
	} else {
		SPDK_ERRLOG("There is no outstanding IOs\n");
	}
}

static void
nvme_rdma_req_complete(struct nvme_request *req,
		       struct spdk_nvme_cpl *rsp)
{
	req->cb_fn(req->cb_arg, rsp);
	nvme_free_request(req);
}

static int
nvme_rdma_qpair_init(struct nvme_rdma_qpair *rqpair)
{
	int			rc;
	struct ibv_qp_init_attr	attr;

	rqpair->max_queue_depth = rqpair->qpair.num_entries;

	SPDK_NOTICELOG("rqpair depth =%d\n", rqpair->max_queue_depth);
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.cap.max_send_wr	= rqpair->max_queue_depth; /* SEND operations */
	attr.cap.max_recv_wr	= rqpair->max_queue_depth; /* RECV operations */
	attr.cap.max_send_sge	= NVME_RDMA_DEFAULT_TX_SGE;
	attr.cap.max_recv_sge	= NVME_RDMA_DEFAULT_RX_SGE;

	rc = rdma_create_qp(rqpair->cm_id, NULL, &attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed\n");
		return -1;
	}

	rc = fcntl(rqpair->cm_id->send_cq_channel->fd, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("fcntl to set comp channel to non-blocking failed\n");
		return -1;
	}

	rc = fcntl(rqpair->cm_id->recv_cq_channel->fd, F_SETFL, O_NONBLOCK);
	if (rc < 0) {
		SPDK_ERRLOG("fcntl to set comp channel to non-blocking failed\n");
		return -1;
	}

	rqpair->cm_id->context = &rqpair->qpair;

	return 0;
}

static int
nvme_rdma_pre_copy_mem(struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_sgl_descriptor *nvme_sgl;
	void *address;

	assert(rdma_req->bb_mr != NULL);
	assert(rdma_req->bb != NULL);

	nvme_sgl = &rdma_req->req->cmd.dptr.sgl1;
	address = (void *)nvme_sgl->address;

	if (address != NULL) {
		rdma_req->cmd.dptr.sgl1.address = (uint64_t)rdma_req->bb;
		if (rdma_req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER ||
		    rdma_req->xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
			memcpy(rdma_req->bb, address, nvme_sgl->keyed.length);
		}
	}

	nvme_sgl = &rdma_req->cmd.dptr.sgl1;
	nvme_sgl->keyed.key = rdma_req->bb_sgl.lkey;

	return 0;
}

static void
nvme_rdma_post_copy_mem(struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_sgl_descriptor *nvme_sgl;
	void *address;

	assert(rdma_req != NULL);
	assert(rdma_req->req != NULL);

	nvme_sgl = &rdma_req->req->cmd.dptr.sgl1;
	address = (void *)nvme_sgl->address;

	if ((address != NULL) &&
	    (rdma_req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST ||
	     rdma_req->xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
		memcpy(address, rdma_req->bb, nvme_sgl->keyed.length);
	}
}

static void
nvme_rdma_trace_ibv_sge(struct ibv_sge *sg_list)
{
	if (sg_list) {
		SPDK_NOTICELOG("local addr %p length 0x%x lkey 0x%x\n",
			       (void *)sg_list->addr, sg_list->length, sg_list->lkey);
	}
}

static void
nvme_rdma_ibv_send_wr_init(struct ibv_send_wr *wr,
			   struct spdk_nvme_rdma_req *req,
			   struct ibv_sge *sg_list,
			   uint64_t wr_id,
			   enum ibv_wr_opcode opcode,
			   int send_flags)
{
	if (!wr || !sg_list) {
		return;
	}

	memset(wr, 0, sizeof(*wr));
	wr->wr_id = wr_id;
	wr->next = NULL;
	wr->opcode = opcode;
	wr->send_flags = send_flags;
	wr->sg_list = sg_list;
	wr->num_sge = 1;

	if (req != NULL) {
		struct spdk_nvme_sgl_descriptor *sgl = &req->cmd.dptr.sgl1;

		wr->wr.rdma.rkey = sgl->keyed.key;
		wr->wr.rdma.remote_addr = sgl->address;

		SPDK_NOTICELOG("rkey %x remote_addr %p\n",
			       wr->wr.rdma.rkey, (void *)wr->wr.rdma.remote_addr);
	}

	nvme_rdma_trace_ibv_sge(wr->sg_list);
}

static int
nvme_rdma_post_recv(struct nvme_rdma_qpair *rqpair,
		    struct spdk_nvme_rdma_rsp *rsp)
{
	struct ibv_recv_wr wr, *bad_wr = NULL;
	int rc;

	wr.wr_id = (uintptr_t)rsp;
	wr.next = NULL;
	wr.sg_list = &rsp->recv_sgl;
	wr.num_sge = 1;

	nvme_rdma_trace_ibv_sge(&rsp->recv_sgl);

	rc = ibv_post_recv(rqpair->cm_id->qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma recv, rc = 0x%x\n", rc);
	}

	return rc;
}

static struct spdk_nvme_rdma_rsp *
config_rdma_rsp(struct nvme_rdma_qpair *rqpair, int i)
{
	struct spdk_nvme_rdma_rsp *rdma_rsp;

	rdma_rsp = &rqpair->rdma_rsps[i];
	if (!rdma_rsp) {
		return NULL;
	}

	rdma_rsp->rsp_mr = rdma_reg_msgs(rqpair->cm_id, &rqpair->rdma_rsps[i].rsp,
					 sizeof(rqpair->rdma_rsps[i].rsp));
	if (rdma_rsp->rsp_mr == NULL) {
		SPDK_ERRLOG("Unable to register rsp_mr\n");
		return NULL;
	}

	/* initialize recv_sgl */
	rdma_rsp->recv_sgl.addr = (uint64_t)&rqpair->rdma_rsps[i].rsp;
	rdma_rsp->recv_sgl.length = sizeof(rqpair->rdma_rsps[i].rsp);
	rdma_rsp->recv_sgl.lkey = rdma_rsp->rsp_mr->lkey;

	return rdma_rsp;
}

static void
nvme_rdma_free_rsps(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_rsp *rdma_rsp;
	int i;

	if (!rqpair->rdma_rsps) {
		return;
	}

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		rdma_rsp = &rqpair->rdma_rsps[i];
		if (rdma_rsp->rsp_mr && rdma_dereg_mr(rdma_rsp->rsp_mr)) {
			SPDK_ERRLOG("Unable to de-register rsp_mr\n");
		}
	}

	free(rqpair->rdma_rsps);
}

static int
nvme_rdma_alloc_rsps(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_rsp *rdma_rsp;
	int i;

	rqpair->rdma_rsps = calloc(rqpair->max_queue_depth, sizeof(struct spdk_nvme_rdma_rsp));

	if (!rqpair->rdma_rsps) {
		SPDK_ERRLOG("can not allocate rdma rsps\n");
		return -1;
	}

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		rdma_rsp = config_rdma_rsp(rqpair, i);
		if (rdma_rsp == NULL) {
			goto fail;
		}

		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rdma_rsq %p: rsp %p\n",
			      rdma_rsp, &rdma_rsp->rsp);

		if (nvme_rdma_post_recv(rqpair, rdma_rsp)) {
			SPDK_ERRLOG("Unable to post connection rx desc\n");
			goto fail;
		}
	}

	return 0;
fail:
	nvme_rdma_free_rsps(rqpair);
	return -ENOMEM;
}

static struct spdk_nvme_rdma_req *
config_rdma_req(struct nvme_rdma_qpair *rqpair, int i)
{
	struct spdk_nvme_rdma_req *rdma_req;

	rdma_req = &rqpair->rdma_reqs[i];

	if (!rdma_req) {
		return NULL;
	}

	rdma_req->cmd_mr = rdma_reg_msgs(rqpair->cm_id, &rdma_req->cmd,
					 sizeof(rdma_req->cmd));

	if (!rdma_req->cmd_mr) {
		SPDK_ERRLOG("Unable to register cmd_mr\n");
		return NULL;
	}

	/* initialize send_sgl */
	rdma_req->send_sgl.addr = (uint64_t)&rdma_req->cmd;
	rdma_req->send_sgl.length = sizeof(rdma_req->cmd);
	rdma_req->send_sgl.lkey = rdma_req->cmd_mr->lkey;

	rdma_req->bb = calloc(1, NVME_RDMA_RW_BUFFER_SIZE);
	if (!rdma_req->bb) {
		SPDK_ERRLOG("Unable to register allocate read/write buffer\n");
		return NULL;
	}

	rdma_req->bb_len = NVME_RDMA_RW_BUFFER_SIZE;
	rdma_req->bb_mr = ibv_reg_mr(rqpair->cm_id->qp->pd, rdma_req->bb, rdma_req->bb_len,
				     IBV_ACCESS_LOCAL_WRITE |
				     IBV_ACCESS_REMOTE_READ |
				     IBV_ACCESS_REMOTE_WRITE);

	if (!rdma_req->bb_mr) {
		SPDK_ERRLOG("Unable to register bb_mr\n");
		return NULL;
	}

	/* initialize bb_sgl */
	rdma_req->bb_sgl.addr = (uint64_t)rdma_req->bb;
	rdma_req->bb_sgl.length = rdma_req->bb_len;
	rdma_req->bb_sgl.lkey = rdma_req->bb_mr->lkey;

	return rdma_req;
}

static void
nvme_rdma_free_reqs(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;
	int i;

	if (!rqpair->rdma_reqs) {
		return;
	}

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		rdma_req = &rqpair->rdma_reqs[i];
		if (rdma_req->cmd_mr && rdma_dereg_mr(rdma_req->cmd_mr)) {
			SPDK_ERRLOG("Unable to de-register cmd_mr\n");
		}
		if (rdma_req->bb_mr && ibv_dereg_mr(rdma_req->bb_mr)) {
			SPDK_ERRLOG("Unable to de-register bb_mr\n");
		}

		if (rdma_req->bb) {
			free(rdma_req->bb);
		}
	}

	free(rqpair->rdma_reqs);
}

static int
nvme_rdma_alloc_reqs(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;
	int i;

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		rdma_req = config_rdma_req(rqpair, i);
		if (rdma_req == NULL) {
			goto fail;
		}

		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rdma_req %p: cmd %p\n",
			      rdma_req, &rdma_req->cmd);
	}

	return 0;

fail:
	nvme_rdma_free_reqs(rqpair);
	return -ENOMEM;
}

static int
nvme_rdma_recv(struct nvme_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	struct spdk_nvme_rdma_req *rdma_req;
	struct spdk_nvme_rdma_rsp *rdma_rsp;
	struct nvme_request *req;

	rdma_rsp = (struct spdk_nvme_rdma_rsp *)wc->wr_id;

	if (wc->byte_len < sizeof(struct spdk_nvmf_fabric_connect_rsp)) {
		SPDK_ERRLOG("recv length %u less than capsule header\n", wc->byte_len);
		return -1;
	}

	rdma_req = &rqpair->rdma_reqs[rdma_rsp->rsp.cid];

	nvme_rdma_post_copy_mem(rdma_req);
	req = rdma_req->req;
	nvme_rdma_req_complete(req, &rdma_rsp->rsp);
	nvme_rdma_req_put(rdma_req);

	if (nvme_rdma_post_recv(rqpair, rdma_rsp)) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return -1;
	}

	return 0;
}

static int
nvme_rdma_bind_addr(struct nvme_rdma_qpair *rqpair,
		    struct sockaddr_storage *sin,
		    struct rdma_event_channel *cm_channel)
{
	int ret;
	struct rdma_cm_event *event;

	ret = rdma_resolve_addr(rqpair->cm_id, NULL, (struct sockaddr *) sin,
				NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		SPDK_ERRLOG("rdma_resolve_addr, %d\n", errno);
		return ret;
	}

	ret = rdma_get_cm_event(cm_channel, &event);
	if (ret) {
		SPDK_ERRLOG("rdma address resolution error\n");
		return ret;
	}
	if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
		return -1;
	}
	rdma_ack_cm_event(event);


	ret = rdma_resolve_route(rqpair->cm_id, NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		SPDK_ERRLOG("rdma_resolve_route\n");
		return ret;
	}
	ret = rdma_get_cm_event(cm_channel, &event);
	if (ret) {
		SPDK_ERRLOG("rdma address resolution error\n");
		return ret;
	}
	if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
		SPDK_ERRLOG("rdma route resolution error\n");
		return -1;
	}
	rdma_ack_cm_event(event);

	SPDK_NOTICELOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static int
nvme_rdma_connect(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_conn_param conn_param;
	struct spdk_nvmf_rdma_request_private_data pdata;
	const union spdk_nvmf_rdma_private_data	*data;
	struct rdma_cm_event *event;
	int ret;

	memset(&conn_param, 0, sizeof(conn_param));
	/* Note:  the following parameters apply only for PS = RDMA_PS_TCP,
	   and even then it appears that any values supplied here by host
	   application are over-written by the rdma_cm layer for the given
	   device.  Verified at target side that private data arrived as
	   specified here, but the other param values either zeroed out or
	   replaced.
	*/
	conn_param.responder_resources = 1;  /* 0 or 1*/
	conn_param.initiator_depth = rqpair->max_queue_depth;
	conn_param.retry_count = 7;
	conn_param.rnr_retry_count = 7;

	/* init private data for connect */
	memset(&pdata, 0, sizeof(pdata));
	pdata.qid = rqpair->qpair.id;
	pdata.hrqsize = rqpair->max_queue_depth;
	pdata.hsqsize = rqpair->max_queue_depth;
	conn_param.private_data = &pdata;
	conn_param.private_data_len = sizeof(pdata);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "qid =%d\n", pdata.qid);

	ret = rdma_connect(rqpair->cm_id, &conn_param);
	if (ret) {
		SPDK_ERRLOG("nvme rdma connect eror");
		return ret;
	}
	ret = rdma_get_cm_event(rqpair->cm_channel, &event);
	if (ret) {
		SPDK_ERRLOG("rdma address resolution error\n");
		return ret;
	}
	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		SPDK_ERRLOG("rdma connect error\n");
		return -1;
	}
	rdma_ack_cm_event(event);


	/* Look for any rdma connection returned by server */
	data = event->param.conn.private_data;

	if (event->param.conn.private_data_len >= sizeof(union spdk_nvmf_rdma_private_data) &&
	    data != NULL) {
		if (data->pd_accept.recfmt != 0) {
			SPDK_ERRLOG("NVMF fabric connect accept: invalid private data format!\n");
		} else {
			SPDK_NOTICELOG("NVMF fabric connect accept, Private data length %d\n",
				       event->param.conn.private_data_len);
			SPDK_NOTICELOG("NVMF fabric connect accept, RECFMT %d\n",
				       data->pd_accept.recfmt);
			SPDK_NOTICELOG("NVMF fabric connect accept, CRQSIZE %d\n",
				       data->pd_accept.crqsize);
		}
	}

	SPDK_NOTICELOG("connect successful\n");
	return 0;
}

static int
nvme_rdma_parse_addr(struct sockaddr_storage *sa, const char *addr, const char *service)
{
	struct addrinfo *res;
	int ret;

	ret = getaddrinfo(addr, service, NULL, &res);
	if (ret) {
		SPDK_ERRLOG("getaddrinfo failed - invalid hostname or IP address\n");
		return ret;
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n", (size_t)res->ai_addrlen);
		ret = EINVAL;
	} else {
		memcpy(sa, res->ai_addr, res->ai_addrlen);
	}

	freeaddrinfo(res);
	return ret;
}

static int
nvmf_cm_construct(struct nvme_rdma_qpair *rqpair)
{
	/* create an event channel with rdmacm to receive
	   connection oriented requests and notifications */
	rqpair->cm_channel = rdma_create_event_channel();
	if (rqpair->cm_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed\n");
		return -1;
	}

	return 0;
}

static int
nvme_rdma_qpair_connect(struct nvme_rdma_qpair *rqpair)
{
	struct sockaddr_storage  sin;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr;

	rc = nvmf_cm_construct(rqpair);
	if (rc < 0) {
		return nvme_transport_qpair_destroy(&rqpair->qpair);
	}

	ctrlr = rqpair->qpair.ctrlr;
	memset(&sin, 0, sizeof(struct sockaddr_storage));

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "trsvcid is %s\n", ctrlr->probe_info.trsvcid);
	rc = nvme_rdma_parse_addr(&sin, ctrlr->probe_info.traddr, ctrlr->probe_info.trsvcid);
	if (rc != 0) {
		goto err;
	}

	rc = rdma_create_id(rqpair->cm_channel, &rqpair->cm_id, rqpair, RDMA_PS_TCP);
	if (rc < 0) {
		goto err;
	}

	rc = nvme_rdma_bind_addr(rqpair, &sin, rqpair->cm_channel);
	if (rc < 0) {
		goto err;
	}

	rc = nvme_rdma_qpair_init(rqpair);
	if (rc < 0) {
		goto err;
	}
	rc = nvme_rdma_alloc_reqs(rqpair);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate rqpair  RDMA requests\n");
		goto err;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "RDMA requests allocated\n");

	rc = nvme_rdma_alloc_rsps(rqpair);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rc =%d\n", rc);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to allocate rqpair RDMA responses\n");
		goto err;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "RDMA responses allocated\n");

	rc = nvme_rdma_connect(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to conduct the rqpair\n");
		goto err;
	}

	return 0;
err:
	return nvme_transport_qpair_destroy(&rqpair->qpair);;
}

static struct spdk_nvme_rdma_req *
nvme_rdma_req_init(struct nvme_rdma_qpair *rqpair, struct nvme_request *req)
{
	struct spdk_nvme_rdma_req *rdma_req;
	struct spdk_nvme_sgl_descriptor *nvme_sgl;

	if (!rqpair || !req) {
		return NULL;
	}

	rdma_req = nvme_rdma_req_get(rqpair);
	if (!rdma_req) {
		return NULL;
	}

	rdma_req->req = req;
	req->cmd.cid = rdma_req->id;

	/* setup the RDMA SGL details */
	nvme_sgl = &req->cmd.dptr.sgl1;
	if (req->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		nvme_sgl->address = (uint64_t)req->payload.u.contig + req->payload_offset;
		nvme_sgl->keyed.length = req->payload_size;
	} else {
		nvme_rdma_req_put(rdma_req);
		/* Need to handle other case later */
		return NULL;
	}

	rdma_req->req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
	nvme_sgl->keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	nvme_sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;

	if (req->cmd.opc == SPDK_NVME_OPC_FABRIC) {
		struct spdk_nvmf_capsule_cmd *nvmf_cmd = (struct spdk_nvmf_capsule_cmd *)&req->cmd;
		rdma_req->xfer = spdk_nvme_opc_get_data_transfer(nvmf_cmd->fctype);
	} else {
		rdma_req->xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);

	}

	memcpy(&rdma_req->cmd, &req->cmd, sizeof(req->cmd));
	return rdma_req;
}

static int
nvme_rdma_qpair_fabric_connect(struct nvme_rdma_qpair *rqpair)
{
	struct nvme_completion_poll_status status;
	struct spdk_nvmf_fabric_connect_rsp *rsp;
	struct spdk_nvmf_fabric_connect_cmd cmd;
	struct spdk_nvmf_fabric_connect_data *nvmf_data;
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_rdma_ctrlr *rctrlr;
	int rc = 0;

	ctrlr = rqpair->qpair.ctrlr;
	if (!ctrlr) {
		return -1;
	}

	rctrlr = nvme_rdma_ctrlr(ctrlr);
	nvmf_data = calloc(1, sizeof(*nvmf_data));
	if (!nvmf_data) {
		SPDK_ERRLOG("nvmf_data allocation error\n");
		rc = -1;
		return rc;
	}

	memset(&cmd, 0, sizeof(cmd));
	memset(&status, 0, sizeof(struct nvme_completion_poll_status));

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	cmd.qid = rqpair->qpair.id;
	cmd.sqsize = rqpair->qpair.num_entries - 1;

	if (nvme_qpair_is_admin_queue(&rqpair->qpair)) {
		nvmf_data->cntlid = 0xFFFF;
	} else {
		nvmf_data->cntlid = rctrlr->cntlid;
	}

	strncpy((char *)&nvmf_data->hostid, (char *)NVME_HOST_ID_DEFAULT,
		strlen((char *)NVME_HOST_ID_DEFAULT));
	strncpy((char *)nvmf_data->hostnqn, NVME_HOST_NQN, sizeof(nvmf_data->hostnqn));
	strncpy((char *)nvmf_data->subnqn, ctrlr->probe_info.nqn, sizeof(nvmf_data->subnqn));

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr,
					   (struct spdk_nvme_cmd *)&cmd,
					   nvmf_data, sizeof(*nvmf_data),
					   nvme_completion_poll_cb, &status);

	if (rc < 0) {
		SPDK_ERRLOG("spdk_nvme_rdma_req_fabric_connect failed\n");
		rc = -1;
		goto ret;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&rqpair->qpair, 0);
	}

	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("Connect command failed\n");
		return -1;
	}

	rsp = (struct spdk_nvmf_fabric_connect_rsp *)&status.cpl;
	rctrlr->cntlid = rsp->status_code_specific.success.cntlid;
ret:
	free(nvmf_data);
	return rc;
}

static int
nvme_rdma_fabric_prop_set_cmd(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t offset, uint8_t size, uint64_t value)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};
	struct nvme_completion_poll_status status = {};
	int rc;

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
	cmd.ofst = offset;
	cmd.attrib.size = size;
	cmd.value.u64 = value;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					   NULL, 0,
					   nvme_completion_poll_cb, &status);

	if (rc < 0) {
		SPDK_ERRLOG("failed to send nvmf_fabric_prop_set_cmd\n");
		return -1;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}

	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_rdma_fabric_prop_get_cmd failed\n");
		return -1;
	}

	return 0;
}

static int
nvme_rdma_fabric_prop_get_cmd(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t offset, uint8_t size, uint64_t *value)
{
	struct spdk_nvmf_fabric_prop_set_cmd cmd = {};
	struct nvme_completion_poll_status status = {};
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	int rc;

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET;
	cmd.ofst = offset;
	cmd.attrib.size = size;

	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					   NULL, sizeof(uint32_t),
					   nvme_completion_poll_cb, &status);

	if (rc < 0) {
		SPDK_ERRLOG("failed to send nvme_rdma_fabric_prop_get_cmd\n");
		return -1;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}

	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_rdma_fabric_prop_get_cmd failed\n");
		return -1;
	}

	response = (struct spdk_nvmf_fabric_prop_get_rsp *)&status.cpl;

	if (!size) {
		*value = response->value.u32.low;
	} else {
		*value = response->value.u64;
	}

	return 0;
}


static int
_nvme_rdma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_qpair *qpair)
{
	int rc;
	struct nvme_rdma_qpair *rqpair;

	rqpair = nvme_rdma_qpair(qpair);
	rc = nvme_rdma_qpair_connect(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to connect through rdma qpair\n");
		goto err;
	}

	rc = nvme_rdma_qpair_fabric_connect(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to send/receive the qpair fabric request\n");
		goto err;
	}

	return 0;

err:
	nvme_transport_qpair_destroy(&rqpair->qpair);
	return rc;

}


static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			     enum spdk_nvme_qprio qprio)
{
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_qpair *qpair;
	struct nvme_rdma_ctrlr *rctrlr;
	uint32_t num_entries;
	int rc;

	rctrlr = nvme_rdma_ctrlr(ctrlr);

	rqpair = calloc(1, sizeof(struct nvme_rdma_qpair));
	if (!rqpair) {
		SPDK_ERRLOG("failed to get create rqpair\n");
		return NULL;
	}

	qpair = &rqpair->qpair;

	/* At this time, queue is not initialized,  so use the passing parameter qid */
	if (!qid) {
		num_entries = SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES;
		ctrlr->adminq = qpair;
	} else {
		num_entries = rctrlr->ctrlr.opts.queue_size;
	}

	rc = nvme_qpair_construct(qpair, qid, num_entries, ctrlr, qprio);
	if (rc != 0) {
		return NULL;
	}

	rc = _nvme_rdma_ctrlr_create_qpair(ctrlr, qpair);

	if (rc < 0) {
		return NULL;
	}

	return qpair;
}

int
nvme_rdma_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair;

	if (!qpair) {
		return -1;
	}

	rqpair = nvme_rdma_qpair(qpair);

	nvme_rdma_free_reqs(rqpair);
	nvme_rdma_free_rsps(rqpair);

	if (rqpair->cm_id) {
		if (rqpair->cm_id->qp) {
			rdma_destroy_qp(rqpair->cm_id);
		}
		rdma_destroy_id(rqpair->cm_id);
	}

	if (rqpair->cm_channel) {
		rdma_destroy_event_channel(rqpair->cm_channel);
	}

	free(rqpair);

	return 0;
}

static int
nvme_rdma_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr)
{
	struct spdk_nvme_qpair *qpair;
	union spdk_nvme_cc_register cc = {};
	int rc;

	qpair = nvme_rdma_ctrlr_create_qpair(ctrlr, 0, 0);
	if (!qpair) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		rc = -1;
		goto error;
	}

	/* Must enable CC.EN, otherwise we can not send nvme commands to the disabled controller */
	cc.raw = 0;
	cc.bits.en = 1;
	rc = nvme_transport_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc.raw),
					    cc.raw);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to set cc\n");
		rc = -1;
		goto error;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "successfully create admin qpair\n");
	return 0;

error:
	nvme_rdma_qpair_destroy(qpair);
	return rc;
}

struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				enum spdk_nvme_qprio qprio)
{
	return nvme_rdma_ctrlr_create_qpair(ctrlr, qid, qprio);
}

int
nvme_rdma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	/* do nothing here */
	return 0;
}

static int
nvme_fabrics_get_log_discovery_page(struct spdk_nvme_ctrlr *ctrlr,
				    char *log_page)
{
	struct spdk_nvme_cmd cmd = {};
	struct nvme_completion_poll_status status = {};
	int rc;

	cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.cdw10 = SPDK_NVME_LOG_DISCOVERY;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, (struct spdk_nvme_cmd *)&cmd,
					   (void *)log_page, 4096,
					   nvme_completion_poll_cb, &status);

	if (rc < 0) {
		return -1;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}

	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		return -1;
	}

	return 0;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
int
nvme_rdma_ctrlr_scan(enum spdk_nvme_transport transport,
		     spdk_nvme_probe_cb probe_cb, void *cb_ctx, void *devhandle)
{
	struct spdk_nvme_discover_info *discover_info = devhandle;
	struct spdk_nvme_probe_info probe_info;
	struct spdk_nvme_ctrlr *discovery_ctrlr;
	struct spdk_nvmf_discovery_log_page *log_page;
	char buffer[4096];
	int rc;
	uint32_t i;

	probe_info.trtype = (uint8_t)transport;
	snprintf(probe_info.nqn, sizeof(probe_info.nqn), "%s", discover_info->nqn);
	snprintf(probe_info.traddr, sizeof(probe_info.traddr), "%s", discover_info->traddr);
	snprintf(probe_info.trsvcid, sizeof(probe_info.trsvcid), "%s", discover_info->trsvcid);

	memset(buffer, 0x0, 4096);
	discovery_ctrlr = nvme_attach(transport, &probe_info);
	if (discovery_ctrlr == NULL) {
		return -1;
	}

	rc = nvme_fabrics_get_log_discovery_page(discovery_ctrlr, buffer);
	if (rc < 0) {
		SPDK_ERRLOG("nvme_fabrics_get_log_discovery_page error\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
	}

	log_page = (struct spdk_nvmf_discovery_log_page *)buffer;
	for (i = 0; i < log_page->numrec; i++) {
		struct spdk_nvmf_discovery_log_page_entry *entry = &log_page->entries[i];
		uint8_t *end;
		size_t len;

		probe_info.trtype = entry->trtype;
		if (!spdk_nvme_transport_available(probe_info.trtype)) {
			SPDK_WARNLOG("NVMe transport type %u not available; skipping probe\n",
				     probe_info.trtype);
			continue;
		}

		/* Ensure that subnqn is null terminated. */
		end = memchr(entry->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN);
		if (!end) {
			SPDK_ERRLOG("Discovery entry %u: SUBNQN is not null terminated\n", i);
			continue;
		}
		len = end - entry->subnqn;
		memcpy(probe_info.nqn, entry->subnqn, len);
		probe_info.nqn[len] = '\0';

		/* Convert traddr to a null terminated string. */
		len = spdk_strlen_pad(entry->traddr, sizeof(entry->traddr), ' ');
		memcpy(probe_info.traddr, entry->traddr, len);

		/* Convert trsvcid to a null terminated string. */
		len = spdk_strlen_pad(entry->trsvcid, sizeof(entry->trsvcid), ' ');
		memcpy(probe_info.trsvcid, entry->trsvcid, len);

		SPDK_NOTICELOG("nqn=%s, trtype=%u, traddr=%s, trsvcid=%s\n", probe_info.nqn,
			       probe_info.trtype, probe_info.traddr, probe_info.trsvcid);
		/* Todo: need to differentiate the NVMe over fabrics to avoid duplicated connection */
		nvme_probe_one(entry->trtype, probe_cb, cb_ctx, &probe_info, &probe_info);
	}

	nvme_ctrlr_destruct(discovery_ctrlr);
	return 0;
}

struct spdk_nvme_ctrlr *nvme_rdma_ctrlr_construct(enum spdk_nvme_transport transport,
		void *devhandle)
{
	struct nvme_rdma_ctrlr *rctrlr;
	struct spdk_nvme_probe_info *probe_info;
	union spdk_nvme_cap_register cap;
	int rc;

	if (!devhandle) {
		SPDK_ERRLOG("devhandle is NULL\n");
		return NULL;
	}

	probe_info = devhandle;
	rctrlr = calloc(1, sizeof(struct nvme_rdma_ctrlr));
	if (rctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	rctrlr->ctrlr.transport = SPDK_NVME_TRANSPORT_RDMA;
	rctrlr->ctrlr.probe_info = *probe_info;

	rc = nvme_ctrlr_construct(&rctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&rctrlr->ctrlr);
		return NULL;
	}

	rc = nvme_rdma_ctrlr_construct_admin_qpair(&rctrlr->ctrlr);
	if (rc != 0) {
		SPDK_ERRLOG("create admin qpair failed\n");
		return NULL;
	}

	if (nvme_ctrlr_get_cap(&rctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		nvme_ctrlr_destruct(&rctrlr->ctrlr);
		return NULL;
	}

	rctrlr->ctrlr.cap = cap;

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "succesully initialized the nvmf ctrlr\n");
	return &rctrlr->ctrlr;
}

int
nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);

	free(rctrlr);

	return 0;
}

int
nvme_rdma_ctrlr_get_pci_id(struct spdk_nvme_ctrlr *ctrlr, struct spdk_pci_id *pci_id)
{
	assert(ctrlr != NULL);
	assert(pci_id != NULL);

	*pci_id = ctrlr->probe_info.pci_id;

	return 0;
}

int
nvme_rdma_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	return nvme_rdma_fabric_prop_set_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, value);
}

int
nvme_rdma_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	return nvme_rdma_fabric_prop_set_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
}

int
nvme_rdma_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	uint64_t tmp_value;
	int rc;
	rc = nvme_rdma_fabric_prop_get_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_4, &tmp_value);

	if (!rc) {
		*value = (uint32_t)tmp_value;
	}
	return rc;
}

int
nvme_rdma_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	return nvme_rdma_fabric_prop_get_cmd(ctrlr, offset, SPDK_NVMF_PROP_SIZE_8, value);
}

int
nvme_rdma_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			       struct nvme_request *req)
{
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_rdma_req *rdma_req;
	struct ibv_send_wr wr, *bad_wr = NULL;
	int rc;

	rqpair = nvme_rdma_qpair(qpair);
	rdma_req = nvme_rdma_req_init(rqpair, req);
	if (!rdma_req) {
		SPDK_ERRLOG("spdk_nvme_rdma_req memory allocation failed duing read\n");
		return -1;
	}

	if (nvme_rdma_pre_copy_mem(rdma_req) < 0) {
		return -1;
	}

	nvme_rdma_ibv_send_wr_init(&wr, NULL, &rdma_req->send_sgl, (uint64_t)rdma_req,
				   IBV_WR_SEND, IBV_SEND_SIGNALED);

	rc = ibv_post_send(rqpair->cm_id->qp, &wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma send for NVMf completion, rc = 0x%x\n", rc);
	}

	return rc;
}

int
nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{

	return nvme_transport_qpair_destroy(qpair);
}

int
nvme_rdma_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return _nvme_rdma_ctrlr_create_qpair(ctrlr, qpair);
}

int
nvme_rdma_qpair_construct(struct spdk_nvme_qpair *qpair)
{
	int32_t i;
	struct nvme_rdma_qpair *rqpair;

	rqpair = nvme_rdma_qpair(qpair);
	rqpair->rdma_reqs = calloc(qpair->num_entries, sizeof(struct spdk_nvme_rdma_req));
	if (rqpair->rdma_reqs == NULL) {
		return -1;
	}

	rqpair->outstanding_reqs = 0;
	STAILQ_INIT(&rqpair->free_reqs);

	SPDK_NOTICELOG("qpair num entries = %d\n", qpair->num_entries);
	for (i = 0; i < qpair->num_entries; i++) {
		STAILQ_INSERT_TAIL(&rqpair->free_reqs, &rqpair->rdma_reqs[i], link);
		rqpair->rdma_reqs[i].rqpair = rqpair;
		rqpair->rdma_reqs[i].id = i;
	}

	return 0;
}

int
nvme_rdma_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	/* Currently, doing nothing here */
	return 0;
}

int
nvme_rdma_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	/* Currently, doing nothing here */
	return 0;
}

int
nvme_rdma_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	/* Currently, doing nothing here */
	return 0;
}

int
nvme_rdma_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	/* Currently, doing nothing here */
	return 0;
}

int
nvme_rdma_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct nvme_rdma_qpair *rqpair;
	struct ibv_wc wc;
	uint32_t size;
	int rc;
	uint32_t io_completed = 0;

	rqpair = nvme_rdma_qpair(qpair);
	size = qpair->num_entries - 1U;
	if (!max_completions || max_completions > size) {
		max_completions = size;
	}

	/* poll the send_cq */
	while (true) {
		rc = ibv_poll_cq(rqpair->cm_id->send_cq, 1, &wc);
		if (rc == 0) {
			break;
		}

		if (rc < 0) {
			SPDK_ERRLOG("Poll CQ error!(%d): %s\n",
				    errno, strerror(errno));
			return -1;
		}

		if (wc.status) {
			SPDK_NOTICELOG("CQ completion error status %d, exiting handler\n",
				       wc.status);
			break;
		}

		if (wc.opcode == IBV_WC_SEND) {
			SPDK_NOTICELOG("CQ send completion\n");
		} else {
			SPDK_ERRLOG("Poll cq opcode type unknown!!!!! completion\n");
			return -1;
		}
	}

	/* poll the recv_cq */
	while (true) {
		rc = ibv_poll_cq(rqpair->cm_id->recv_cq, 1, &wc);
		if (rc == 0) {
			break;
		}

		if (rc < 0) {
			SPDK_ERRLOG("Poll CQ error!(%d): %s\n",
				    errno, strerror(errno));
			return -1;
		}

		if (wc.status) {
			SPDK_NOTICELOG("CQ completion error status %d, exiting handler\n",
				       wc.status);
			break;
		}

		if (wc.opcode == IBV_WC_RECV) {
			SPDK_NOTICELOG("CQ recv completion\n");
			rc = nvme_rdma_recv(rqpair, &wc);
			if (rc) {
				SPDK_ERRLOG("nvme_rdma_recv processing failure\n");

				return -1;
			}
			io_completed++;
		} else {
			SPDK_ERRLOG("Poll cq opcode type unknown!!!!! completion\n");
			return -1;
		}

		if (io_completed == max_completions) {
			break;
		}
	}

	return io_completed;
}

uint32_t
nvme_rdma_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* Todo, which should get from the NVMF target */
	return NVME_RDMA_RW_BUFFER_SIZE;
}
