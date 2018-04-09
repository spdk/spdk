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

#include "spdk/stdinc.h"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "spdk/assert.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/event.h"
#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/endian.h"

#include "nvme_internal.h"

#define NVME_RDMA_TIME_OUT_IN_MS 2000
#define NVME_RDMA_RW_BUFFER_SIZE 131072

/*
NVME RDMA qpair Resouce Defaults
 */
#define NVME_RDMA_DEFAULT_TX_SGE		2
#define NVME_RDMA_DEFAULT_RX_SGE		1

/* Mapping from virtual address to ibv_mr pointer for a protection domain */
struct spdk_nvme_rdma_mr_map {
	struct ibv_pd				*pd;
	struct spdk_mem_map			*map;
	uint64_t				ref;
	LIST_ENTRY(spdk_nvme_rdma_mr_map)	link;
};

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

	struct ibv_cq				*cq;

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

	struct spdk_nvme_rdma_mr_map		*mr_map;

	STAILQ_HEAD(, spdk_nvme_rdma_req)	free_reqs;
};

struct spdk_nvme_rdma_req {
	int					id;

	struct ibv_send_wr			send_wr;

	struct nvme_request			*req;

	struct ibv_sge				send_sgl;

	STAILQ_ENTRY(spdk_nvme_rdma_req)	link;
};

static const char *rdma_cm_event_str[] = {
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

static LIST_HEAD(, spdk_nvme_rdma_mr_map) g_rdma_mr_maps = LIST_HEAD_INITIALIZER(&g_rdma_mr_maps);
static pthread_mutex_t g_rdma_mr_maps_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static const char *
nvme_rdma_cm_event_str_get(uint32_t event)
{
	if (event < SPDK_COUNTOF(rdma_cm_event_str)) {
		return rdma_cm_event_str[event];
	} else {
		return "Undefined";
	}
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
			    errno, spdk_strerror(errno));
		return NULL;
	}

	if (event->event != evt) {
		SPDK_ERRLOG("Expected %s but received %s (%d) from CM event channel (status = %d)\n",
			    nvme_rdma_cm_event_str_get(evt),
			    nvme_rdma_cm_event_str_get(event->event), event->event, event->status);
		rdma_ack_cm_event(event);
		return NULL;
	}

	return event;
}

static int
nvme_rdma_qpair_init(struct nvme_rdma_qpair *rqpair)
{
	int			rc;
	struct ibv_qp_init_attr	attr;

	rqpair->cq = ibv_create_cq(rqpair->cm_id->verbs, rqpair->num_entries * 2, rqpair, NULL, 0);
	if (!rqpair->cq) {
		SPDK_ERRLOG("Unable to create completion queue: errno %d: %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.send_cq		= rqpair->cq;
	attr.recv_cq		= rqpair->cq;
	attr.cap.max_send_wr	= rqpair->num_entries; /* SEND operations */
	attr.cap.max_recv_wr	= rqpair->num_entries; /* RECV operations */
	attr.cap.max_send_sge	= NVME_RDMA_DEFAULT_TX_SGE;
	attr.cap.max_recv_sge	= NVME_RDMA_DEFAULT_RX_SGE;

	rc = rdma_create_qp(rqpair->cm_id, NULL, &attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed\n");
		return -1;
	}

	rqpair->cm_id->context = &rqpair->qpair;

	return 0;
}

#define nvme_rdma_trace_ibv_sge(sg_list) \
	if (sg_list) { \
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "local addr %p length 0x%x lkey 0x%x\n", \
			      (void *)(sg_list)->addr, (sg_list)->length, (sg_list)->lkey); \
	}

static int
nvme_rdma_post_recv(struct nvme_rdma_qpair *rqpair, uint16_t rsp_idx)
{
	struct ibv_recv_wr *wr, *bad_wr = NULL;
	int rc;

	wr = &rqpair->rsp_recv_wrs[rsp_idx];
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

		rqpair->rsp_recv_wrs[i].wr_id = i;
		rqpair->rsp_recv_wrs[i].next = NULL;
		rqpair->rsp_recv_wrs[i].sg_list = rsp_sgl;
		rqpair->rsp_recv_wrs[i].num_sge = 1;

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
	if (!rqpair->rdma_reqs) {
		return;
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

		rdma_req->send_wr.wr_id = (uint64_t)rdma_req;
		rdma_req->send_wr.next = NULL;
		rdma_req->send_wr.opcode = IBV_WR_SEND;
		rdma_req->send_wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->send_wr.sg_list = &rdma_req->send_sgl;
		rdma_req->send_wr.num_sge = 1;
		rdma_req->send_wr.imm_data = 0;

		STAILQ_INSERT_TAIL(&rqpair->free_reqs, rdma_req, link);
	}

	return 0;

fail:
	nvme_rdma_free_reqs(rqpair);
	return -ENOMEM;
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

	req = rdma_req->req;
	nvme_rdma_req_complete(req, rsp);

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
		       struct sockaddr *src_addr,
		       struct sockaddr *dst_addr,
		       struct rdma_event_channel *cm_channel)
{
	int ret;
	struct rdma_cm_event *event;

	ret = rdma_resolve_addr(rqpair->cm_id, src_addr, dst_addr,
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
	struct spdk_nvmf_rdma_request_private_data	request_data = {};
	struct spdk_nvmf_rdma_accept_private_data	*accept_data;
	struct ibv_device_attr				attr;
	int						ret;
	struct rdma_cm_event				*event;
	struct spdk_nvme_ctrlr				*ctrlr;
	struct nvme_rdma_ctrlr				*rctrlr;

	ret = ibv_query_device(rqpair->cm_id->verbs, &attr);
	if (ret != 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		return ret;
	}

	param.responder_resources = spdk_min(rqpair->num_entries, attr.max_qp_rd_atom);

	ctrlr = rqpair->qpair.ctrlr;
	if (!ctrlr) {
		return -1;
	}

	rctrlr = nvme_rdma_ctrlr(ctrlr);

	request_data.qid = rqpair->qpair.id;
	request_data.hrqsize = rqpair->num_entries;
	request_data.hsqsize = rqpair->num_entries - 1;
	request_data.cntlid = rctrlr->cntlid;

	param.private_data = &request_data;
	param.private_data_len = sizeof(request_data);
	param.retry_count = 7;

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
		rdma_ack_cm_event(event);
		SPDK_ERRLOG("NVMe-oF target did not return accept data\n");
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Requested queue depth %d. Actually got queue depth %d.\n",
		      rqpair->num_entries, accept_data->crqsize);

	rqpair->num_entries = spdk_min(rqpair->num_entries, accept_data->crqsize);

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
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(ret), ret);
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

	nvmf_data = spdk_dma_zmalloc(sizeof(*nvmf_data), 0, NULL);
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

	SPDK_STATIC_ASSERT(sizeof(nvmf_data->hostid) == sizeof(ctrlr->opts.extended_host_id),
			   "host ID size mismatch");
	memcpy(nvmf_data->hostid, ctrlr->opts.extended_host_id, sizeof(nvmf_data->hostid));
	snprintf(nvmf_data->hostnqn, sizeof(nvmf_data->hostnqn), "%s", ctrlr->opts.hostnqn);
	snprintf(nvmf_data->subnqn, sizeof(nvmf_data->subnqn), "%s", ctrlr->trid.subnqn);

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

	if (nvme_qpair_is_admin_queue(&rqpair->qpair)) {
		rsp = (struct spdk_nvmf_fabric_connect_rsp *)&status.cpl;
		rctrlr->cntlid = rsp->status_code_specific.success.cntlid;
	}
ret:
	spdk_dma_free(nvmf_data);
	return rc;
}

static int
nvme_rdma_mr_map_notify(void *cb_ctx, struct spdk_mem_map *map,
			enum spdk_mem_map_notify_action action,
			void *vaddr, size_t size)
{
	struct ibv_pd *pd = cb_ctx;
	struct ibv_mr *mr;
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		mr = ibv_reg_mr(pd, vaddr, size,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
		if (mr == NULL) {
			SPDK_ERRLOG("ibv_reg_mr() failed\n");
			return -EFAULT;
		} else {
			rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)mr);
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)vaddr);
		rc = spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);
		if (mr) {
			ibv_dereg_mr(mr);
		}
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return rc;
}


static int
nvme_rdma_register_mem(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_pd *pd = rqpair->cm_id->qp->pd;
	struct spdk_nvme_rdma_mr_map *mr_map;

	pthread_mutex_lock(&g_rdma_mr_maps_mutex);

	/* Look up existing mem map registration for this pd */
	LIST_FOREACH(mr_map, &g_rdma_mr_maps, link) {
		if (mr_map->pd == pd) {
			mr_map->ref++;
			rqpair->mr_map = mr_map;
			pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
			return 0;
		}
	}

	mr_map = calloc(1, sizeof(*mr_map));
	if (mr_map == NULL) {
		SPDK_ERRLOG("calloc() failed\n");
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		return -1;
	}

	mr_map->ref = 1;
	mr_map->pd = pd;
	mr_map->map = spdk_mem_map_alloc((uint64_t)NULL, nvme_rdma_mr_map_notify, pd);
	if (mr_map == NULL) {
		SPDK_ERRLOG("spdk_mem_map_alloc() failed\n");
		free(mr_map);
		pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
		return -1;
	}

	rqpair->mr_map = mr_map;
	LIST_INSERT_HEAD(&g_rdma_mr_maps, mr_map, link);

	pthread_mutex_unlock(&g_rdma_mr_maps_mutex);

	return 0;
}

static void
nvme_rdma_unregister_mem(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_mr_map *mr_map;

	mr_map = rqpair->mr_map;
	rqpair->mr_map = NULL;

	if (mr_map == NULL) {
		return;
	}

	pthread_mutex_lock(&g_rdma_mr_maps_mutex);

	assert(mr_map->ref > 0);
	mr_map->ref--;
	if (mr_map->ref == 0) {
		LIST_REMOVE(mr_map, link);
		spdk_mem_map_free(&mr_map->map);
		free(mr_map);
	}

	pthread_mutex_unlock(&g_rdma_mr_maps_mutex);
}

static int
nvme_rdma_qpair_connect(struct nvme_rdma_qpair *rqpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	bool src_addr_specified;
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

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_rdma_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_rdma_parse_addr() failed\n");
		return -1;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_rdma_parse_addr(&src_addr, family, ctrlr->opts.src_addr, ctrlr->opts.src_svcid);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_rdma_parse_addr() failed\n");
			return -1;
		}
		src_addr_specified = true;
	} else {
		src_addr_specified = false;
	}

	rc = rdma_create_id(rqpair->cm_channel, &rqpair->cm_id, rqpair, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		return -1;
	}

	rc = nvme_rdma_resolve_addr(rqpair,
				    src_addr_specified ? (struct sockaddr *)&src_addr : NULL,
				    (struct sockaddr *)&dst_addr, rqpair->cm_channel);
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
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate rqpair  RDMA requests\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA requests allocated\n");

	rc = nvme_rdma_alloc_rsps(rqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to allocate rqpair RDMA responses\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA responses allocated\n");

	rc = nvme_rdma_register_mem(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to register memory for RDMA\n");
		return -1;
	}

	rc = nvme_rdma_qpair_fabric_connect(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		return -1;
	}

	return 0;
}

/*
 * Build SGL describing empty payload.
 */
static int
nvme_rdma_build_null_request(struct nvme_request *req)
{
	struct spdk_nvme_sgl_descriptor *nvme_sgl;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

	nvme_sgl = &req->cmd.dptr.sgl1;
	nvme_sgl->keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	nvme_sgl->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	nvme_sgl->keyed.length = 0;
	nvme_sgl->keyed.key = 0;
	nvme_sgl->address = 0;

	return 0;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
static int
nvme_rdma_build_contig_request(struct nvme_rdma_qpair *rqpair, struct nvme_request *req)
{
	void *payload = req->payload.u.contig + req->payload_offset;
	struct ibv_mr *mr;

	assert(req->payload_size != 0);
	assert(req->payload.type == NVME_PAYLOAD_TYPE_CONTIG);

	mr = (struct ibv_mr *)spdk_mem_map_translate(rqpair->mr_map->map, (uint64_t)payload);
	if (mr == NULL) {
		return -1;
	}

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.keyed.length = req->payload_size;
	req->cmd.dptr.sgl1.keyed.key = mr->rkey;
	req->cmd.dptr.sgl1.address = (uint64_t)payload;

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static int
nvme_rdma_build_sgl_request(struct nvme_rdma_qpair *rqpair, struct nvme_request *req)
{
	int rc;
	void *virt_addr;
	struct ibv_mr *mr;
	uint32_t length;

	assert(req->payload_size != 0);
	assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.u.sgl.reset_sgl_fn != NULL);
	assert(req->payload.u.sgl.next_sge_fn != NULL);
	req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);

	/* TODO: for now, we only support a single SGL entry */
	rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg, &virt_addr, &length);
	if (rc) {
		return -1;
	}

	if (length < req->payload_size) {
		SPDK_ERRLOG("multi-element SGL currently not supported for RDMA\n");
		return -1;
	}

	mr = (struct ibv_mr *)spdk_mem_map_translate(rqpair->mr_map->map, (uint64_t)virt_addr);
	if (mr == NULL) {
		return -1;
	}

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.keyed.length = req->payload_size;
	req->cmd.dptr.sgl1.keyed.key = mr->rkey;
	req->cmd.dptr.sgl1.address = (uint64_t)virt_addr;

	return 0;
}

static int
nvme_rdma_req_init(struct nvme_rdma_qpair *rqpair, struct nvme_request *req,
		   struct spdk_nvme_rdma_req *rdma_req)
{
	int rc;

	rdma_req->req = req;
	req->cmd.cid = rdma_req->id;

	if (req->payload_size == 0) {
		rc = nvme_rdma_build_null_request(req);
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_rdma_build_contig_request(rqpair, req);
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_SGL) {
		rc = nvme_rdma_build_sgl_request(rqpair, req);
	} else {
		rc = -1;
	}

	if (rc) {
		return rc;
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
			     enum spdk_nvme_qprio qprio,
			     uint32_t num_requests)
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

	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests);
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

	nvme_rdma_unregister_mem(rqpair);
	nvme_rdma_free_reqs(rqpair);
	nvme_rdma_free_rsps(rqpair);

	if (rqpair->cm_id) {
		if (rqpair->cm_id->qp) {
			rdma_destroy_qp(rqpair->cm_id);
		}
		rdma_destroy_id(rqpair->cm_id);
	}

	if (rqpair->cq) {
		ibv_destroy_cq(rqpair->cq);
	}

	if (rqpair->cm_channel) {
		rdma_destroy_event_channel(rqpair->cm_channel);
	}

	free(rqpair);

	return 0;
}

struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_rdma_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					    opts->io_queue_requests);
}

int
nvme_rdma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	/* do nothing here */
	return 0;
}

static int
nvme_fabrics_get_log_discovery_page(struct spdk_nvme_ctrlr *ctrlr,
				    void *log_page, uint32_t size, uint64_t offset)
{
	struct nvme_completion_poll_status status;
	int rc;

	status.done = false;
	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_DISCOVERY, 0, log_page, size, offset,
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
	end = memchr(entry->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
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
	if (spdk_str_chomp(trid.traddr) != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Trailing newlines removed from discovery TRADDR\n");
	}

	/* Convert trsvcid to a null terminated string. */
	len = spdk_strlen_pad(entry->trsvcid, sizeof(entry->trsvcid), ' ');
	memcpy(trid.trsvcid, entry->trsvcid, len);
	if (spdk_str_chomp(trid.trsvcid) != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Trailing newlines removed from discovery TRSVCID\n");
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "subnqn=%s, trtype=%u, traddr=%s, trsvcid=%s\n",
		      trid.subnqn, trid.trtype,
		      trid.traddr, trid.trsvcid);

	nvme_ctrlr_probe(&trid, NULL, probe_cb, cb_ctx);
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
int
nvme_rdma_ctrlr_scan(const struct spdk_nvme_transport_id *discovery_trid,
		     void *cb_ctx,
		     spdk_nvme_probe_cb probe_cb,
		     spdk_nvme_remove_cb remove_cb,
		     bool direct_connect)
{
	struct spdk_nvme_ctrlr_opts discovery_opts;
	struct spdk_nvme_ctrlr *discovery_ctrlr;
	struct spdk_nvmf_discovery_log_page *log_page;
	struct spdk_nvmf_discovery_log_page_entry *log_page_entry;
	union spdk_nvme_cc_register cc;
	char buffer[4096];
	int rc;
	uint64_t i, numrec, buffer_max_entries_first, buffer_max_entries, log_page_offset = 0;
	uint64_t remaining_num_rec = 0;
	uint16_t recfmt;
	struct nvme_completion_poll_status status;

	if (strcmp(discovery_trid->subnqn, SPDK_NVMF_DISCOVERY_NQN) != 0) {
		/* It is not a discovery_ctrlr info and try to directly connect it */
		rc = nvme_ctrlr_probe(discovery_trid, NULL, probe_cb, cb_ctx);
		return rc;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&discovery_opts, sizeof(discovery_opts));
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

	/* get the cdata info */
	status.done = false;
	rc = nvme_ctrlr_cmd_identify(discovery_ctrlr, SPDK_NVME_IDENTIFY_CTRLR, 0, 0,
				     &discovery_ctrlr->cdata, sizeof(discovery_ctrlr->cdata),
				     nvme_completion_poll_cb, &status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to identify cdata\n");
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(discovery_ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_identify_controller failed!\n");
		return -ENXIO;
	}

	/* Direct attach through spdk_nvme_connect() API */
	if (direct_connect == true) {
		/* Set the ready state to skip the normal init process */
		discovery_ctrlr->state = NVME_CTRLR_STATE_READY;
		nvme_ctrlr_connected(discovery_ctrlr);
		nvme_ctrlr_add_process(discovery_ctrlr, 0);
		return 0;
	}

	buffer_max_entries_first = (sizeof(buffer) - offsetof(struct spdk_nvmf_discovery_log_page,
				    entries[0])) /
				   sizeof(struct spdk_nvmf_discovery_log_page_entry);
	buffer_max_entries = sizeof(buffer) / sizeof(struct spdk_nvmf_discovery_log_page_entry);
	do {
		rc = nvme_fabrics_get_log_discovery_page(discovery_ctrlr, buffer, sizeof(buffer), log_page_offset);
		if (rc < 0) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "nvme_fabrics_get_log_discovery_page error\n");
			nvme_ctrlr_destruct(discovery_ctrlr);
			return rc;
		}

		if (!remaining_num_rec) {
			log_page = (struct spdk_nvmf_discovery_log_page *)buffer;
			recfmt = from_le16(&log_page->recfmt);
			if (recfmt != 0) {
				SPDK_ERRLOG("Unrecognized discovery log record format %" PRIu16 "\n", recfmt);
				nvme_ctrlr_destruct(discovery_ctrlr);
				return -EPROTO;
			}
			remaining_num_rec = log_page->numrec;
			log_page_offset = offsetof(struct spdk_nvmf_discovery_log_page, entries[0]);
			log_page_entry = &log_page->entries[0];
			numrec = spdk_min(remaining_num_rec, buffer_max_entries_first);
		} else {
			numrec = spdk_min(remaining_num_rec, buffer_max_entries);
			log_page_entry = (struct spdk_nvmf_discovery_log_page_entry *)buffer;
		}


		for (i = 0; i < numrec; i++) {
			nvme_rdma_discovery_probe(log_page_entry++, cb_ctx, probe_cb);
		}
		remaining_num_rec -= numrec;
		log_page_offset += numrec * sizeof(struct spdk_nvmf_discovery_log_page_entry);
	} while (remaining_num_rec != 0);

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
			       SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES, 0, SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES);
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

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "succesully initialized the nvmf ctrlr\n");
	return &rctrlr->ctrlr;
}

int
nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_rdma_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	free(rctrlr);

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

	nvme_rdma_trace_ibv_sge(wr->sg_list);

	rc = ibv_post_send(rqpair->cm_id->qp, wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Failure posting rdma send for NVMf completion: %d (%s)\n", rc, spdk_strerror(rc));
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

#define MAX_COMPLETIONS_PER_POLL 128

int
nvme_rdma_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct nvme_rdma_qpair	*rqpair = nvme_rdma_qpair(qpair);
	struct ibv_wc		wc[MAX_COMPLETIONS_PER_POLL];
	int			i, rc, batch_size;
	uint32_t		reaped;
	struct ibv_cq		*cq;

	if (max_completions == 0) {
		max_completions = rqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, rqpair->num_entries);
	}

	cq = rqpair->cq;

	reaped = 0;
	do {
		batch_size = spdk_min((max_completions - reaped),
				      MAX_COMPLETIONS_PER_POLL);
		rc = ibv_poll_cq(cq, batch_size, wc);
		if (rc < 0) {
			SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
				    errno, spdk_strerror(errno));
			return -1;
		} else if (rc == 0) {
			/* Ran out of completions */
			break;
		}

		for (i = 0; i < rc; i++) {
			if (wc[i].status) {
				SPDK_ERRLOG("CQ error on Queue Pair %p, Response Index %lu (%d): %s\n",
					    qpair, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));
				return -1;
			}

			switch (wc[i].opcode) {
			case IBV_WC_RECV:
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "CQ recv completion\n");

				reaped++;

				if (wc[i].byte_len < sizeof(struct spdk_nvme_cpl)) {
					SPDK_ERRLOG("recv length %u less than expected response size\n", wc[i].byte_len);
					return -1;
				}

				if (nvme_rdma_recv(rqpair, wc[i].wr_id)) {
					SPDK_ERRLOG("nvme_rdma_recv processing failure\n");
					return -1;
				}
				break;

			case IBV_WC_SEND:
				break;

			default:
				SPDK_ERRLOG("Received an unexpected opcode on the CQ: %d\n", wc[i].opcode);
				return -1;
			}
		}
	} while (reaped < max_completions);

	return reaped;
}

uint32_t
nvme_rdma_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* Todo, which should get from the NVMF target */
	return NVME_RDMA_RW_BUFFER_SIZE;
}

uint16_t
nvme_rdma_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	/*
	 * We do not support >1 SGE in the initiator currently,
	 *  so we can only return 1 here.  Once that support is
	 *  added, this should return ctrlr->cdata.nvmf_specific.msdbd
	 *  instead.
	 */
	return 1;
}

void *
nvme_rdma_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	return NULL;
}

int
nvme_rdma_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	return 0;
}
