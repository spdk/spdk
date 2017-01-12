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

#define NVME_HOST_MAX_ENTRIES_PER_QUEUE (128)

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

	struct rdma_event_channel		*cm_channel;

	struct rdma_cm_id			*cm_id;

	struct	spdk_nvme_rdma_req		*rdma_reqs;

	uint16_t				num_entries;

	/* Parallel arrays of response buffers + response SGLs of size num_entries */
	struct ibv_sge				*rsp_sgls;
	struct spdk_nvme_cpl			*rsps;

	struct ibv_recv_wr			*rsp_recv_wrs;

	/* Memory region describing all rsps for this qpair */
	struct ibv_mr				*rsp_mr;

	/*
	 * Array of num_entries NVMe commands registered as RDMA message buffers.
	 * Indexed by rdma_req->id.
	 */
	struct spdk_nvme_cmd			*cmds;

	/* Memory region describing all cmds for this qpair */
	struct ibv_mr				*cmd_mr;

	STAILQ_HEAD(, spdk_nvme_rdma_req)	free_reqs;
};

struct spdk_nvme_rdma_req {
	int					id;

	struct ibv_send_wr			send_wr;

	struct nvme_request 			*req;

	enum spdk_nvme_data_transfer		xfer;

	struct ibv_sge				send_sgl;

	struct ibv_mr				*bb_mr;

	/* Cached value of bb_mr->rkey */
	uint32_t				bb_rkey;

	uint8_t					*bb;

	STAILQ_ENTRY(spdk_nvme_rdma_req)	link;
};

static int nvme_rdma_qpair_destroy(struct spdk_nvme_qpair *qpair);

static inline struct nvme_rdma_qpair *
nvme_rdma_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_RDMA);
	return (struct nvme_rdma_qpair *)((uintptr_t)qpair - offsetof(struct nvme_rdma_qpair, qpair));
}

static inline struct nvme_rdma_ctrlr *
nvme_rdma_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_RDMA);
	return (struct nvme_rdma_ctrlr *)((uintptr_t)ctrlr - offsetof(struct nvme_rdma_ctrlr, ctrlr));
}

static struct spdk_nvme_rdma_req *
nvme_rdma_req_get(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;

	rdma_req = STAILQ_FIRST(&rqpair->free_reqs);
	if (rdma_req) {
		STAILQ_REMOVE_HEAD(&rqpair->free_reqs, link);
	}

	return rdma_req;
}

static void
nvme_rdma_req_put(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	STAILQ_INSERT_HEAD(&rqpair->free_reqs, rdma_req, link);
}

static void
nvme_rdma_req_complete(struct nvme_request *req,
		       struct spdk_nvme_cpl *rsp)
{
	req->cb_fn(req->cb_arg, rsp);
	nvme_free_request(req);
}

static struct rdma_cm_event *
nvme_rdma_get_event(struct rdma_event_channel *channel,
		    enum rdma_cm_event_type evt)
{
	struct rdma_cm_event	*event;
	int			rc;

	rc = rdma_get_cm_event(channel, &event);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to get event from CM event channel. Error %d (%s)\n",
			    errno, strerror(errno));
		return NULL;
	}

	if (event->event != evt) {
		SPDK_ERRLOG("Received event %d from CM event channel, but expected event %d\n",
			    event->event, evt);
		return NULL;
	}

	return event;
}

static int
nvme_rdma_qpair_init(struct nvme_rdma_qpair *rqpair)
{
	int			rc;
	struct ibv_qp_init_attr	attr;

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.cap.max_send_wr	= rqpair->num_entries; /* SEND operations */
	attr.cap.max_recv_wr	= rqpair->num_entries; /* RECV operations */
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

#define nvme_rdma_trace_ibv_sge(sg_list) \
	if (sg_list) { \
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "local addr %p length 0x%x lkey 0x%x\n", \
			      (void *)(sg_list)->addr, (sg_list)->length, (sg_list)->lkey); \
	}

static int
nvme_rdma_post_recv(struct nvme_rdma_qpair *rqpair, uint16_t rsp_idx)
{
	struct ibv_recv_wr *wr, *bad_wr = NULL;
	int rc;

	wr = &rqpair->rsp_recv_wrs[rsp_idx];
	wr->wr_id = rsp_idx;
	wr->next = NULL;
	wr->sg_list = &rqpair->rsp_sgls[rsp_idx];
	wr->num_sge = 1;

	nvme_rdma_trace_ibv_sge(wr->sg_list);

	rc = ibv_post_recv(rqpair->cm_id->qp, wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma recv, rc = 0x%x\n", rc);
	}

	return rc;
}

static void
nvme_rdma_free_rsps(struct nvme_rdma_qpair *rqpair)
{
	if (rqpair->rsp_mr && rdma_dereg_mr(rqpair->rsp_mr)) {
		SPDK_ERRLOG("Unable to de-register rsp_mr\n");
	}
	rqpair->rsp_mr = NULL;

	free(rqpair->rsps);
	rqpair->rsps = NULL;
	free(rqpair->rsp_sgls);
	rqpair->rsp_sgls = NULL;
	free(rqpair->rsp_recv_wrs);
	rqpair->rsp_recv_wrs = NULL;
}

static int
nvme_rdma_alloc_rsps(struct nvme_rdma_qpair *rqpair)
{
	uint16_t i;

	rqpair->rsp_mr = NULL;
	rqpair->rsps = NULL;
	rqpair->rsp_recv_wrs = NULL;

	rqpair->rsp_sgls = calloc(rqpair->num_entries, sizeof(*rqpair->rsp_sgls));
	if (!rqpair->rsp_sgls) {
		SPDK_ERRLOG("Failed to allocate rsp_sgls\n");
		goto fail;
	}

	rqpair->rsp_recv_wrs = calloc(rqpair->num_entries,
				      sizeof(*rqpair->rsp_recv_wrs));
	if (!rqpair->rsp_recv_wrs) {
		SPDK_ERRLOG("Failed to allocate rsp_recv_wrs\n");
		goto fail;
	}

	rqpair->rsps = calloc(rqpair->num_entries, sizeof(*rqpair->rsps));
	if (!rqpair->rsps) {
		SPDK_ERRLOG("can not allocate rdma rsps\n");
		goto fail;
	}

	rqpair->rsp_mr = rdma_reg_msgs(rqpair->cm_id, rqpair->rsps,
				       rqpair->num_entries * sizeof(*rqpair->rsps));
	if (rqpair->rsp_mr == NULL) {
		SPDK_ERRLOG("Unable to register rsp_mr\n");
		goto fail;
	}

	for (i = 0; i < rqpair->num_entries; i++) {
		struct ibv_sge *rsp_sgl = &rqpair->rsp_sgls[i];

		rsp_sgl->addr = (uint64_t)&rqpair->rsps[i];
		rsp_sgl->length = sizeof(rqpair->rsps[i]);
		rsp_sgl->lkey = rqpair->rsp_mr->lkey;

		if (nvme_rdma_post_recv(rqpair, i)) {
			SPDK_ERRLOG("Unable to post connection rx desc\n");
			goto fail;
		}
	}

	return 0;

fail:
	nvme_rdma_free_rsps(rqpair);
	return -ENOMEM;
}

static void
nvme_rdma_free_reqs(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;
	int i;

	if (!rqpair->rdma_reqs) {
		return;
	}

	for (i = 0; i < rqpair->num_entries; i++) {
		rdma_req = &rqpair->rdma_reqs[i];

		if (rdma_req->bb_mr && ibv_dereg_mr(rdma_req->bb_mr)) {
			SPDK_ERRLOG("Unable to de-register bb_mr\n");
		}

		if (rdma_req->bb) {
			free(rdma_req->bb);
		}
	}

	if (rqpair->cmd_mr && rdma_dereg_mr(rqpair->cmd_mr)) {
		SPDK_ERRLOG("Unable to de-register cmd_mr\n");
	}
	rqpair->cmd_mr = NULL;

	free(rqpair->cmds);
	rqpair->cmds = NULL;

	free(rqpair->rdma_reqs);
	rqpair->rdma_reqs = NULL;
}

static int
nvme_rdma_alloc_reqs(struct nvme_rdma_qpair *rqpair)
{
	int i;

	rqpair->rdma_reqs = calloc(rqpair->num_entries, sizeof(struct spdk_nvme_rdma_req));
	if (rqpair->rdma_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate rdma_reqs\n");
		goto fail;
	}

	rqpair->cmds = calloc(rqpair->num_entries, sizeof(*rqpair->cmds));
	if (!rqpair->cmds) {
		SPDK_ERRLOG("Failed to allocate RDMA cmds\n");
		goto fail;
	}

	rqpair->cmd_mr = rdma_reg_msgs(rqpair->cm_id, rqpair->cmds,
				       rqpair->num_entries * sizeof(*rqpair->cmds));
	if (!rqpair->cmd_mr) {
		SPDK_ERRLOG("Unable to register cmd_mr\n");
		goto fail;
	}

	STAILQ_INIT(&rqpair->free_reqs);
	for (i = 0; i < rqpair->num_entries; i++) {
		struct spdk_nvme_rdma_req	*rdma_req;
		struct spdk_nvme_cmd		*cmd;

		rdma_req = &rqpair->rdma_reqs[i];
		cmd = &rqpair->cmds[i];

		rdma_req->id = i;

		rdma_req->send_sgl.addr = (uint64_t)cmd;
		rdma_req->send_sgl.length = sizeof(*cmd);
		rdma_req->send_sgl.lkey = rqpair->cmd_mr->lkey;

		rdma_req->bb = calloc(1, NVME_RDMA_RW_BUFFER_SIZE);
		if (!rdma_req->bb) {
			SPDK_ERRLOG("Unable to register allocate read/write buffer\n");
			goto fail;
		}

		rdma_req->bb_mr = ibv_reg_mr(rqpair->cm_id->qp->pd, rdma_req->bb, NVME_RDMA_RW_BUFFER_SIZE,
					     IBV_ACCESS_LOCAL_WRITE |
					     IBV_ACCESS_REMOTE_READ |
					     IBV_ACCESS_REMOTE_WRITE);
		if (!rdma_req->bb_mr) {
			SPDK_ERRLOG("Unable to register bb_mr\n");
			goto fail;
		}

		rdma_req->bb_rkey = rdma_req->bb_mr->rkey;

		STAILQ_INSERT_TAIL(&rqpair->free_reqs, rdma_req, link);
	}

	return 0;

fail:
	nvme_rdma_free_reqs(rqpair);
	return -ENOMEM;
}

static int
nvme_rdma_copy_mem(struct spdk_nvme_rdma_req *rdma_req, bool copy_from_user)
{
	int rc;
	uint32_t remaining_transfer_len, len, offset = 0;
	void *addr, *src, *dst;
	struct spdk_nvme_sgl_descriptor *nvme_sgl;
	struct nvme_request *req = rdma_req->req;

	if (!req->payload_size) {
		return 0;
	}

	nvme_sgl = &req->cmd.dptr.sgl1;
	if (req->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		addr = (void *)((uint64_t)req->payload.u.contig + req->payload_offset);
		if (!addr) {
			return -1;
		}

		len = req->payload_size;
		if (copy_from_user) {
			src = addr;
			dst = (void *)nvme_sgl->address;
		} else {
			src = (void *)nvme_sgl->address;
			dst = addr;
		}
		memcpy(dst, src, len);

	} else {
		if (!req->payload.u.sgl.reset_sgl_fn ||
		    !req->payload.u.sgl.next_sge_fn) {
			return -1;
		}

		req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);
		remaining_transfer_len = req->payload_size;

		while (remaining_transfer_len > 0) {
			rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg,
							    &addr, &len);
			if (rc || !addr) {
				SPDK_ERRLOG("Invalid address returned from user next_sge_fn callback\n");
				return -1;
			}

			len = nvme_min(remaining_transfer_len, len);
			remaining_transfer_len -= len;

			if (copy_from_user) {
				src = addr;
				dst = (void *)nvme_sgl->address + offset;
			} else {
				src = (void *)nvme_sgl->address + offset;
				dst = addr;
			}
			memcpy(dst, src, len);

			offset += len;
		}
	}

	return 0;
}

static int
nvme_rdma_recv(struct nvme_rdma_qpair *rqpair, uint64_t rsp_idx)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct spdk_nvme_rdma_req *rdma_req;
	struct spdk_nvme_cpl *rsp;
	struct nvme_request *req;

	assert(rsp_idx < rqpair->num_entries);
	rsp = &rqpair->rsps[rsp_idx];
	rdma_req = &rqpair->rdma_reqs[rsp->cid];

	if (rdma_req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		if (nvme_rdma_copy_mem(rdma_req, false) < 0) {
			SPDK_ERRLOG("Failed to copy to user memory\n");
			goto done;
		}
	}

	req = rdma_req->req;
	nvme_rdma_req_complete(req, rsp);

done:
	nvme_rdma_req_put(rqpair, rdma_req);
	if (nvme_rdma_post_recv(rqpair, rsp_idx)) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return -1;
	}

	if (!STAILQ_EMPTY(&qpair->queued_req) && !qpair->ctrlr->is_resetting) {
		req = STAILQ_FIRST(&qpair->queued_req);
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
		nvme_qpair_submit_request(qpair, req);
	}

	return 0;
}

static int
nvme_rdma_resolve_addr(struct nvme_rdma_qpair *rqpair,
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

	event = nvme_rdma_get_event(cm_channel, RDMA_CM_EVENT_ADDR_RESOLVED);
	if (event == NULL) {
		SPDK_ERRLOG("RDMA address resolution error\n");
		return -1;
	}
	rdma_ack_cm_event(event);

	ret = rdma_resolve_route(rqpair->cm_id, NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		SPDK_ERRLOG("rdma_resolve_route\n");
		return ret;
	}

	event = nvme_rdma_get_event(cm_channel, RDMA_CM_EVENT_ROUTE_RESOLVED);
	if (event == NULL) {
		SPDK_ERRLOG("RDMA route resolution error\n");
		return -1;
	}
	rdma_ack_cm_event(event);

	return 0;
}

static int
nvme_rdma_connect(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_conn_param				param = {};
	struct spdk_nvmf_rdma_request_private_data 	request_data = {};
	struct spdk_nvmf_rdma_accept_private_data	*accept_data;
	struct ibv_device_attr 				attr;
	int 						ret;
	struct rdma_cm_event 				*event;

	ret = ibv_query_device(rqpair->cm_id->verbs, &attr);
	if (ret != 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		return ret;
	}

	param.responder_resources = nvme_min(rqpair->num_entries, attr.max_qp_rd_atom);

	request_data.qid = rqpair->qpair.id;
	request_data.hrqsize = rqpair->num_entries;
	request_data.hsqsize = rqpair->num_entries - 1;

	param.private_data = &request_data;
	param.private_data_len = sizeof(request_data);

	ret = rdma_connect(rqpair->cm_id, &param);
	if (ret) {
		SPDK_ERRLOG("nvme rdma connect error\n");
		return ret;
	}

	event = nvme_rdma_get_event(rqpair->cm_channel, RDMA_CM_EVENT_ESTABLISHED);
	if (event == NULL) {
		SPDK_ERRLOG("RDMA connect error\n");
		return -1;
	}

	accept_data = (struct spdk_nvmf_rdma_accept_private_data *)event->param.conn.private_data;
	if (accept_data == NULL) {
		SPDK_ERRLOG("NVMe-oF target did not return accept data\n");
		return -1;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVME, "Requested queue depth %d. Actually got queue depth %d.\n",
		      rqpair->num_entries, accept_data->crqsize);

	rqpair->num_entries  = nvme_min(rqpair->num_entries , accept_data->crqsize);

	rdma_ack_cm_event(event);

	return 0;
}

static int
nvme_rdma_parse_addr(struct sockaddr_storage *sa, int family, const char *addr, const char *service)
{
	struct addrinfo *res;
	struct addrinfo hints;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	ret = getaddrinfo(addr, service, &hints, &res);
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
	cmd.sqsize = rqpair->num_entries - 1;
	cmd.kato = ctrlr->opts.keep_alive_timeout_ms;

	if (nvme_qpair_is_admin_queue(&rqpair->qpair)) {
		nvmf_data->cntlid = 0xFFFF;
	} else {
		nvmf_data->cntlid = rctrlr->cntlid;
	}

	strncpy((char *)&nvmf_data->hostid, (char *)NVME_HOST_ID_DEFAULT,
		strlen((char *)NVME_HOST_ID_DEFAULT));
	strncpy((char *)nvmf_data->hostnqn, ctrlr->opts.hostnqn, sizeof(nvmf_data->hostnqn));
	strncpy((char *)nvmf_data->subnqn, ctrlr->trid.subnqn, sizeof(nvmf_data->subnqn));

	rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, &rqpair->qpair,
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
nvme_rdma_qpair_connect(struct nvme_rdma_qpair *rqpair)
{
	struct sockaddr_storage  sin;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr;
	int family;

	rqpair->cm_channel = rdma_create_event_channel();
	if (rqpair->cm_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed\n");
		return -1;
	}

	ctrlr = rqpair->qpair.ctrlr;

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		return -1;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVME, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&sin, 0, sizeof(struct sockaddr_storage));

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_rdma_parse_addr(&sin, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid);
	if (rc != 0) {
		SPDK_ERRLOG("nvme_rdma_parse_addr() failed\n");
		return -1;
	}

	rc = rdma_create_id(rqpair->cm_channel, &rqpair->cm_id, rqpair, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		return -1;
	}

	rc = nvme_rdma_resolve_addr(rqpair, &sin, rqpair->cm_channel);
	if (rc < 0) {
		SPDK_ERRLOG("nvme_rdma_resolve_addr() failed\n");
		return -1;
	}

	rc = nvme_rdma_qpair_init(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("nvme_rdma_qpair_init() failed\n");
		return -1;
	}

	rc = nvme_rdma_connect(rqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to connect the rqpair\n");
		return -1;
	}

	rc = nvme_rdma_alloc_reqs(rqpair);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate rqpair  RDMA requests\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "RDMA requests allocated\n");

	rc = nvme_rdma_alloc_rsps(rqpair);
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "rc =%d\n", rc);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to allocate rqpair RDMA responses\n");
		return -1;
	}
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "RDMA responses allocated\n");

	rc = nvme_rdma_qpair_fabric_connect(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		return -1;
	}

	return 0;
}

/**
 * Build SGL list describing scattered payload buffer.
 */
static int
nvme_rdma_build_sgl_request(struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_sgl_descriptor *nvme_sgl;
	struct nvme_request *req = rdma_req->req;

	if (req->payload_size > rdma_req->bb_mr->length) {
		return -1;
	}

	if ((req->payload.type != NVME_PAYLOAD_TYPE_CONTIG) &&
	    (req->payload.type != NVME_PAYLOAD_TYPE_SGL)) {
		return -1;
	}

	/* setup the RDMA SGL details */
	nvme_sgl = &req->cmd.dptr.sgl1;
	nvme_sgl->keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	nvme_sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	nvme_sgl->keyed.length = req->payload_size;
	nvme_sgl->keyed.key = rdma_req->bb_rkey;
	nvme_sgl->address = (uint64_t)rdma_req->bb;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;

	if (rdma_req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		if (nvme_rdma_copy_mem(rdma_req, true) < 0) {
			SPDK_ERRLOG("Failed to copy from user memory\n");
			return -1;
		}
	}

	return 0;
}

static int
nvme_rdma_req_init(struct nvme_rdma_qpair *rqpair, struct nvme_request *req,
		   struct spdk_nvme_rdma_req *rdma_req)
{

	rdma_req->req = req;
	req->cmd.cid = rdma_req->id;

	if (req->cmd.opc == SPDK_NVME_OPC_FABRIC) {
		struct spdk_nvmf_capsule_cmd *nvmf_cmd = (struct spdk_nvmf_capsule_cmd *)&req->cmd;
		rdma_req->xfer = spdk_nvme_opc_get_data_transfer(nvmf_cmd->fctype);
	} else {
		rdma_req->xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
	}

	/* We do not support bi-directional transfer yet */
	if (rdma_req->xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
		SPDK_ERRLOG("Do not support bi-directional data transfer\n");
		return -1;
	}

	if (nvme_rdma_build_sgl_request(rdma_req) < 0) {
		return -1;
	}

	memcpy(&rqpair->cmds[rdma_req->id], &req->cmd, sizeof(req->cmd));
	return 0;
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
					   NULL, 0, nvme_completion_poll_cb,
					   &status);

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

static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			     uint16_t qid, uint32_t qsize,
			     enum spdk_nvme_qprio qprio)
{
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	rqpair = calloc(1, sizeof(struct nvme_rdma_qpair));
	if (!rqpair) {
		SPDK_ERRLOG("failed to get create rqpair\n");
		return NULL;
	}

	rqpair->num_entries = qsize;

	qpair = &rqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio);
	if (rc != 0) {
		return NULL;
	}

	rc = nvme_rdma_qpair_connect(rqpair);
	if (rc < 0) {
		nvme_rdma_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

static int
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

struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				enum spdk_nvme_qprio qprio)
{
	return nvme_rdma_ctrlr_create_qpair(ctrlr, qid, ctrlr->opts.io_queue_size, qprio);
}

int
nvme_rdma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	/* do nothing here */
	return 0;
}

static int
nvme_fabrics_get_log_discovery_page(struct spdk_nvme_ctrlr *ctrlr,
				    void *log_page, uint32_t size)
{
	struct nvme_completion_poll_status status;
	int rc;

	status.done = false;
	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_DISCOVERY, 0, log_page, size, 0,
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

static void
nvme_rdma_discovery_probe(struct spdk_nvmf_discovery_log_page_entry *entry,
			  void *cb_ctx, spdk_nvme_probe_cb probe_cb)
{
	struct spdk_nvme_transport_id trid;
	uint8_t *end;
	size_t len;

	memset(&trid, 0, sizeof(trid));

	if (entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		SPDK_WARNLOG("Skipping unsupported discovery service referral\n");
		return;
	} else if (entry->subtype != SPDK_NVMF_SUBTYPE_NVME) {
		SPDK_WARNLOG("Skipping unknown subtype %u\n", entry->subtype);
		return;
	}

	trid.trtype = entry->trtype;
	if (!spdk_nvme_transport_available(trid.trtype)) {
		SPDK_WARNLOG("NVMe transport type %u not available; skipping probe\n",
			     trid.trtype);
		return;
	}

	trid.adrfam = entry->adrfam;

	/* Ensure that subnqn is null terminated. */
	end = memchr(entry->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN);
	if (!end) {
		SPDK_ERRLOG("Discovery entry SUBNQN is not null terminated\n");
		return;
	}
	len = end - entry->subnqn;
	memcpy(trid.subnqn, entry->subnqn, len);
	trid.subnqn[len] = '\0';

	/* Convert traddr to a null terminated string. */
	len = spdk_strlen_pad(entry->traddr, sizeof(entry->traddr), ' ');
	memcpy(trid.traddr, entry->traddr, len);

	/* Convert trsvcid to a null terminated string. */
	len = spdk_strlen_pad(entry->trsvcid, sizeof(entry->trsvcid), ' ');
	memcpy(trid.trsvcid, entry->trsvcid, len);

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "subnqn=%s, trtype=%u, traddr=%s, trsvcid=%s\n",
		      trid.subnqn, trid.trtype,
		      trid.traddr, trid.trsvcid);

	nvme_ctrlr_probe(&trid, NULL, probe_cb, cb_ctx);
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
int
nvme_rdma_ctrlr_scan(const struct spdk_nvme_transport_id *discovery_trid,
		     void *cb_ctx,
		     spdk_nvme_probe_cb probe_cb,
		     spdk_nvme_remove_cb remove_cb)
{
	struct spdk_nvme_ctrlr_opts discovery_opts;
	struct spdk_nvme_ctrlr *discovery_ctrlr;
	struct spdk_nvmf_discovery_log_page *log_page;
	union spdk_nvme_cc_register cc;
	char buffer[4096];
	int rc;
	uint64_t i, numrec, buffer_max_entries;

	spdk_nvme_ctrlr_opts_set_defaults(&discovery_opts);
	/* For discovery_ctrlr set the timeout to 0 */
	discovery_opts.keep_alive_timeout_ms = 0;

	memset(buffer, 0x0, 4096);
	discovery_ctrlr = nvme_rdma_ctrlr_construct(discovery_trid, &discovery_opts, NULL);
	if (discovery_ctrlr == NULL) {
		return -1;
	}

	/* TODO: this should be using the normal NVMe controller initialization process */
	cc.raw = 0;
	cc.bits.en = 1;
	cc.bits.iosqes = 6; /* SQ entry size == 64 == 2^6 */
	cc.bits.iocqes = 4; /* CQ entry size == 16 == 2^4 */
	rc = nvme_transport_ctrlr_set_reg_4(discovery_ctrlr, offsetof(struct spdk_nvme_registers, cc.raw),
					    cc.raw);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to set cc\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		return -1;
	}

	rc = nvme_fabrics_get_log_discovery_page(discovery_ctrlr, buffer, sizeof(buffer));
	if (rc < 0) {
		SPDK_TRACELOG(SPDK_TRACE_NVME, "nvme_fabrics_get_log_discovery_page error\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		/* It is not a discovery_ctrlr info and try to directly connect it */
		rc = nvme_ctrlr_probe(discovery_trid, NULL, probe_cb, cb_ctx);
		return rc;
	}

	log_page = (struct spdk_nvmf_discovery_log_page *)buffer;

	/*
	 * For now, only support retrieving one buffer of discovery entries.
	 * This could be extended to call Get Log Page multiple times as needed.
	 */
	buffer_max_entries = (sizeof(buffer) - offsetof(struct spdk_nvmf_discovery_log_page, entries[0])) /
			     sizeof(struct spdk_nvmf_discovery_log_page_entry);
	numrec = nvme_min(log_page->numrec, buffer_max_entries);
	if (numrec != log_page->numrec) {
		SPDK_WARNLOG("Discovery service returned %" PRIu64 " entries,"
			     "but buffer can only hold %" PRIu64 "\n",
			     log_page->numrec, numrec);
	}

	for (i = 0; i < numrec; i++) {
		nvme_rdma_discovery_probe(&log_page->entries[i], cb_ctx, probe_cb);
	}

	nvme_ctrlr_destruct(discovery_ctrlr);
	return 0;
}

struct spdk_nvme_ctrlr *nvme_rdma_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct nvme_rdma_ctrlr *rctrlr;
	union spdk_nvme_cap_register cap;
	int rc;

	rctrlr = calloc(1, sizeof(struct nvme_rdma_ctrlr));
	if (rctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	rctrlr->ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	rctrlr->ctrlr.opts = *opts;
	memcpy(&rctrlr->ctrlr.trid, trid, sizeof(rctrlr->ctrlr.trid));

	rc = nvme_ctrlr_construct(&rctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&rctrlr->ctrlr);
		return NULL;
	}

	rctrlr->ctrlr.adminq = nvme_rdma_ctrlr_create_qpair(&rctrlr->ctrlr, 0,
			       SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES, 0);
	if (!rctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		return NULL;
	}

	if (nvme_ctrlr_get_cap(&rctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		nvme_ctrlr_destruct(&rctrlr->ctrlr);
		return NULL;
	}

	nvme_ctrlr_init_cap(&rctrlr->ctrlr, &cap);

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "succesully initialized the nvmf ctrlr\n");
	return &rctrlr->ctrlr;
}

int
nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_rdma_qpair_destroy(ctrlr->adminq);
	}

	free(rctrlr);

	return 0;
}

int
nvme_rdma_ctrlr_get_pci_id(struct spdk_nvme_ctrlr *ctrlr, struct spdk_pci_id *pci_id)
{
	return -1;
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
	struct ibv_send_wr *wr, *bad_wr = NULL;
	int rc;

	rqpair = nvme_rdma_qpair(qpair);
	assert(rqpair != NULL);
	assert(req != NULL);

	rdma_req = nvme_rdma_req_get(rqpair);
	if (!rdma_req) {
		/*
		 * No rdma_req is available.  Queue the request to be processed later.
		 */
		STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
		return 0;
	}

	if (nvme_rdma_req_init(rqpair, req, rdma_req)) {
		SPDK_ERRLOG("nvme_rdma_req_init() failed\n");
		nvme_rdma_req_put(rqpair, rdma_req);
		return -1;
	}

	wr = &rdma_req->send_wr;
	wr->wr_id = (uint64_t)rdma_req;
	wr->next = NULL;
	wr->opcode = IBV_WR_SEND;
	wr->send_flags = IBV_SEND_SIGNALED;
	wr->sg_list = &rdma_req->send_sgl;
	wr->num_sge = 1;
	wr->imm_data = 0;

	nvme_rdma_trace_ibv_sge(wr->sg_list);

	rc = ibv_post_send(rqpair->cm_id->qp, wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma send for NVMf completion: %d (%s)\n", rc, strerror(rc));
	}

	return rc;
}

int
nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return nvme_rdma_qpair_destroy(qpair);
}

int
nvme_rdma_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return nvme_rdma_qpair_connect(nvme_rdma_qpair(qpair));
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
	size = rqpair->num_entries - 1U;
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
			SPDK_ERRLOG("CQ completion error status %d, exiting handler\n",
				    wc.status);
			break;
		}

		if (wc.opcode == IBV_WC_SEND) {
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, "CQ send completion\n");
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
			SPDK_ERRLOG("CQ Completion Error For Response %lu: %d (%s)\n",
				    wc.wr_id, wc.status, ibv_wc_status_str(wc.status));
			break;
		}

		if (wc.opcode == IBV_WC_RECV) {
			SPDK_TRACELOG(SPDK_TRACE_DEBUG, "CQ recv completion\n");
			if (wc.byte_len < sizeof(struct spdk_nvme_cpl)) {
				SPDK_ERRLOG("recv length %u less than expected response size\n", wc.byte_len);
				return -1;
			}

			rc = nvme_rdma_recv(rqpair, wc.wr_id);
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

uint32_t
nvme_rdma_ctrlr_get_max_io_queue_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_HOST_MAX_ENTRIES_PER_QUEUE;
}
