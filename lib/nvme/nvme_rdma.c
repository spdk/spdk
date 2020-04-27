/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/likely.h"
#include "spdk/config.h"

#include "nvme_internal.h"

#define NVME_RDMA_TIME_OUT_IN_MS 2000
#define NVME_RDMA_RW_BUFFER_SIZE 131072

/*
 * NVME RDMA qpair Resource Defaults
 */
#define NVME_RDMA_DEFAULT_TX_SGE		2
#define NVME_RDMA_DEFAULT_RX_SGE		1

/* Max number of NVMe-oF SGL descriptors supported by the host */
#define NVME_RDMA_MAX_SGL_DESCRIPTORS		16

/* number of STAILQ entries for holding pending RDMA CM events. */
#define NVME_RDMA_NUM_CM_EVENTS			256

/* CM event processing timeout */
#define NVME_RDMA_QPAIR_CM_EVENT_TIMEOUT_US	1000000

/*
 * In the special case of a stale connection we don't expose a mechanism
 * for the user to retry the connection so we need to handle it internally.
 */
#define NVME_RDMA_STALE_CONN_RETRY_MAX		5
#define NVME_RDMA_STALE_CONN_RETRY_DELAY_US	10000

/*
 * Maximum value of transport_retry_count used by RDMA controller
 */
#define NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT	7

/*
 * Maximum value of transport_ack_timeout used by RDMA controller
 */
#define NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT	31

struct spdk_nvmf_cmd {
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_sgl_descriptor sgl[NVME_RDMA_MAX_SGL_DESCRIPTORS];
};

struct spdk_nvme_rdma_hooks g_nvme_hooks = {};

/* Mapping from virtual address to ibv_mr pointer for a protection domain */
struct spdk_nvme_rdma_mr_map {
	struct ibv_pd				*pd;
	struct spdk_mem_map			*map;
	uint64_t				ref;
	LIST_ENTRY(spdk_nvme_rdma_mr_map)	link;
};

/* STAILQ wrapper for cm events. */
struct nvme_rdma_cm_event_entry {
	struct rdma_cm_event			*evt;
	STAILQ_ENTRY(nvme_rdma_cm_event_entry)	link;
};

/* NVMe RDMA transport extensions for spdk_nvme_ctrlr */
struct nvme_rdma_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;

	struct ibv_pd				*pd;

	uint16_t				max_sge;

	struct rdma_event_channel		*cm_channel;

	STAILQ_HEAD(, nvme_rdma_cm_event_entry)	pending_cm_events;

	STAILQ_HEAD(, nvme_rdma_cm_event_entry)	free_cm_events;

	struct nvme_rdma_cm_event_entry		*cm_events;
};

struct nvme_rdma_poll_group {
	struct spdk_nvme_transport_poll_group group;
};

struct spdk_nvme_send_wr_list {
	struct ibv_send_wr	*first;
	struct ibv_send_wr	*last;
};

struct spdk_nvme_recv_wr_list {
	struct ibv_recv_wr	*first;
	struct ibv_recv_wr	*last;
};

/* NVMe RDMA qpair extensions for spdk_nvme_qpair */
struct nvme_rdma_qpair {
	struct spdk_nvme_qpair			qpair;

	struct rdma_cm_id			*cm_id;

	struct ibv_cq				*cq;

	struct	spdk_nvme_rdma_req		*rdma_reqs;

	uint32_t				max_send_sge;

	uint32_t				max_recv_sge;

	uint16_t				num_entries;

	bool					delay_cmd_submit;

	/* Parallel arrays of response buffers + response SGLs of size num_entries */
	struct ibv_sge				*rsp_sgls;
	struct spdk_nvme_cpl			*rsps;

	struct ibv_recv_wr			*rsp_recv_wrs;

	struct spdk_nvme_send_wr_list		sends_to_post;
	struct spdk_nvme_recv_wr_list		recvs_to_post;

	/* Memory region describing all rsps for this qpair */
	struct ibv_mr				*rsp_mr;

	/*
	 * Array of num_entries NVMe commands registered as RDMA message buffers.
	 * Indexed by rdma_req->id.
	 */
	struct spdk_nvmf_cmd			*cmds;

	/* Memory region describing all cmds for this qpair */
	struct ibv_mr				*cmd_mr;

	struct spdk_nvme_rdma_mr_map		*mr_map;

	TAILQ_HEAD(, spdk_nvme_rdma_req)	free_reqs;
	TAILQ_HEAD(, spdk_nvme_rdma_req)	outstanding_reqs;

	/* Placed at the end of the struct since it is not used frequently */
	struct rdma_cm_event			*evt;
};

enum NVME_RDMA_COMPLETION_FLAGS {
	NVME_RDMA_SEND_COMPLETED = 1u << 0,
	NVME_RDMA_RECV_COMPLETED = 1u << 1,
};

struct spdk_nvme_rdma_req {
	uint16_t				id;
	uint16_t				completion_flags: 2;
	uint16_t				reserved: 14;
	/* if completion of RDMA_RECV received before RDMA_SEND, we will complete nvme request
	 * during processing of RDMA_SEND. To complete the request we must know the index
	 * of nvme_cpl received in RDMA_RECV, so store it in this field */
	uint16_t				rsp_idx;

	struct ibv_send_wr			send_wr;

	struct nvme_request			*req;

	struct ibv_sge				send_sgl[NVME_RDMA_DEFAULT_TX_SGE];

	TAILQ_ENTRY(spdk_nvme_rdma_req)		link;
};

enum nvme_rdma_key_type {
	NVME_RDMA_MR_RKEY,
	NVME_RDMA_MR_LKEY
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

static int nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);

static inline struct nvme_rdma_qpair *
nvme_rdma_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_RDMA);
	return SPDK_CONTAINEROF(qpair, struct nvme_rdma_qpair, qpair);
}

static inline struct nvme_rdma_ctrlr *
nvme_rdma_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_RDMA);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_rdma_ctrlr, ctrlr);
}

static struct spdk_nvme_rdma_req *
nvme_rdma_req_get(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;

	rdma_req = TAILQ_FIRST(&rqpair->free_reqs);
	if (rdma_req) {
		TAILQ_REMOVE(&rqpair->free_reqs, rdma_req, link);
		TAILQ_INSERT_TAIL(&rqpair->outstanding_reqs, rdma_req, link);
	}

	return rdma_req;
}

static void
nvme_rdma_req_put(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	rdma_req->completion_flags = 0;
	rdma_req->req = NULL;
	TAILQ_REMOVE(&rqpair->outstanding_reqs, rdma_req, link);
	TAILQ_INSERT_HEAD(&rqpair->free_reqs, rdma_req, link);
}

static void
nvme_rdma_req_complete(struct nvme_request *req,
		       struct spdk_nvme_cpl *rsp)
{
	nvme_complete_request(req->cb_fn, req->cb_arg, req->qpair, req, rsp);
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


static int
nvme_rdma_qpair_process_cm_event(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_cm_event				*event = rqpair->evt;
	struct spdk_nvmf_rdma_accept_private_data	*accept_data;
	int						rc = 0;

	if (event) {
		switch (event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
		case RDMA_CM_EVENT_ROUTE_ERROR:
			break;
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:
			break;
		case RDMA_CM_EVENT_CONNECT_ERROR:
			break;
		case RDMA_CM_EVENT_UNREACHABLE:
		case RDMA_CM_EVENT_REJECTED:
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			accept_data = (struct spdk_nvmf_rdma_accept_private_data *)event->param.conn.private_data;
			if (accept_data == NULL) {
				rc = -1;
			} else {
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "Requested queue depth %d. Actually got queue depth %d.\n",
					      rqpair->num_entries, accept_data->crqsize);
				rqpair->num_entries = spdk_min(rqpair->num_entries, accept_data->crqsize);
			}
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
			break;
		case RDMA_CM_EVENT_MULTICAST_JOIN:
		case RDMA_CM_EVENT_MULTICAST_ERROR:
			break;
		case RDMA_CM_EVENT_ADDR_CHANGE:
			rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_LOCAL;
			break;
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			break;
		default:
			SPDK_ERRLOG("Unexpected Acceptor Event [%d]\n", event->event);
			break;
		}
		rqpair->evt = NULL;
		rdma_ack_cm_event(event);
	}

	return rc;
}

/*
 * This function must be called under the nvme controller's lock
 * because it touches global controller variables. The lock is taken
 * by the generic transport code before invoking a few of the functions
 * in this file: nvme_rdma_ctrlr_connect_qpair, nvme_rdma_ctrlr_delete_io_qpair,
 * and conditionally nvme_rdma_qpair_process_completions when it is calling
 * completions on the admin qpair. When adding a new call to this function, please
 * verify that it is in a situation where it falls under the lock.
 */
static int
nvme_rdma_poll_events(struct nvme_rdma_ctrlr *rctrlr)
{
	struct nvme_rdma_cm_event_entry	*entry, *tmp;
	struct nvme_rdma_qpair		*event_qpair;
	struct rdma_cm_event		*event;
	struct rdma_event_channel	*channel = rctrlr->cm_channel;

	STAILQ_FOREACH_SAFE(entry, &rctrlr->pending_cm_events, link, tmp) {
		event_qpair = nvme_rdma_qpair(entry->evt->id->context);
		if (event_qpair->evt == NULL) {
			event_qpair->evt = entry->evt;
			STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
			STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);
		}
	}

	while (rdma_get_cm_event(channel, &event) == 0) {
		event_qpair = nvme_rdma_qpair(event->id->context);
		if (event_qpair->evt == NULL) {
			event_qpair->evt = event;
		} else {
			assert(rctrlr == nvme_rdma_ctrlr(event_qpair->qpair.ctrlr));
			entry = STAILQ_FIRST(&rctrlr->free_cm_events);
			if (entry == NULL) {
				rdma_ack_cm_event(event);
				return -ENOMEM;
			}
			STAILQ_REMOVE(&rctrlr->free_cm_events, entry, nvme_rdma_cm_event_entry, link);
			entry->evt = event;
			STAILQ_INSERT_TAIL(&rctrlr->pending_cm_events, entry, link);
		}
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return 0;
	} else {
		return errno;
	}
}

static int
nvme_rdma_validate_cm_event(enum rdma_cm_event_type expected_evt_type,
			    struct rdma_cm_event *reaped_evt)
{
	int rc = -EBADMSG;

	if (expected_evt_type == reaped_evt->event) {
		return 0;
	}

	switch (expected_evt_type) {
	case RDMA_CM_EVENT_ESTABLISHED:
		/*
		 * There is an enum ib_cm_rej_reason in the kernel headers that sets 10 as
		 * IB_CM_REJ_STALE_CONN. I can't find the corresponding userspace but we get
		 * the same values here.
		 */
		if (reaped_evt->event == RDMA_CM_EVENT_REJECTED && reaped_evt->status == 10) {
			rc = -ESTALE;
		}
		break;
	default:
		break;
	}

	SPDK_ERRLOG("Expected %s but received %s (%d) from CM event channel (status = %d)\n",
		    nvme_rdma_cm_event_str_get(expected_evt_type),
		    nvme_rdma_cm_event_str_get(reaped_evt->event), reaped_evt->event,
		    reaped_evt->status);
	return rc;
}

static int
nvme_rdma_process_event(struct nvme_rdma_qpair *rqpair,
			struct rdma_event_channel *channel,
			enum rdma_cm_event_type evt)
{
	struct nvme_rdma_ctrlr	*rctrlr;
	uint64_t timeout_ticks;
	int	rc = 0, rc2;

	if (rqpair->evt != NULL) {
		rc = nvme_rdma_qpair_process_cm_event(rqpair);
		if (rc) {
			return rc;
		}
	}

	timeout_ticks = (NVME_RDMA_QPAIR_CM_EVENT_TIMEOUT_US * spdk_get_ticks_hz()) / SPDK_SEC_TO_USEC +
			spdk_get_ticks();
	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	assert(rctrlr != NULL);

	while (!rqpair->evt && spdk_get_ticks() < timeout_ticks && rc == 0) {
		rc = nvme_rdma_poll_events(rctrlr);
	}

	if (rc) {
		return rc;
	}

	if (rqpair->evt == NULL) {
		return -EADDRNOTAVAIL;
	}

	rc = nvme_rdma_validate_cm_event(evt, rqpair->evt);

	rc2 = nvme_rdma_qpair_process_cm_event(rqpair);
	/* bad message takes precedence over the other error codes from processing the event. */
	return rc == 0 ? rc2 : rc;
}

static int
nvme_rdma_qpair_init(struct nvme_rdma_qpair *rqpair)
{
	int			rc;
	struct ibv_qp_init_attr	attr;
	struct ibv_device_attr	dev_attr;
	struct nvme_rdma_ctrlr	*rctrlr;

	rc = ibv_query_device(rqpair->cm_id->verbs, &dev_attr);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		return -1;
	}

	rqpair->cq = ibv_create_cq(rqpair->cm_id->verbs, rqpair->num_entries * 2, rqpair, NULL, 0);
	if (!rqpair->cq) {
		SPDK_ERRLOG("Unable to create completion queue: errno %d: %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	if (g_nvme_hooks.get_ibv_pd) {
		rctrlr->pd = g_nvme_hooks.get_ibv_pd(&rctrlr->ctrlr.trid, rqpair->cm_id->verbs);
	} else {
		rctrlr->pd = NULL;
	}

	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.qp_type		= IBV_QPT_RC;
	attr.send_cq		= rqpair->cq;
	attr.recv_cq		= rqpair->cq;
	attr.cap.max_send_wr	= rqpair->num_entries; /* SEND operations */
	attr.cap.max_recv_wr	= rqpair->num_entries; /* RECV operations */
	attr.cap.max_send_sge	= spdk_min(NVME_RDMA_DEFAULT_TX_SGE, dev_attr.max_sge);
	attr.cap.max_recv_sge	= spdk_min(NVME_RDMA_DEFAULT_RX_SGE, dev_attr.max_sge);

	rc = rdma_create_qp(rqpair->cm_id, rctrlr->pd, &attr);

	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed\n");
		return -1;
	}

	/* ibv_create_qp will change the values in attr.cap. Make sure we store the proper value. */
	rqpair->max_send_sge = spdk_min(NVME_RDMA_DEFAULT_TX_SGE, attr.cap.max_send_sge);
	rqpair->max_recv_sge = spdk_min(NVME_RDMA_DEFAULT_RX_SGE, attr.cap.max_recv_sge);

	rctrlr->pd = rqpair->cm_id->qp->pd;

	rqpair->cm_id->context = &rqpair->qpair;

	return 0;
}

static inline int
nvme_rdma_qpair_submit_sends(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_send_wr *bad_send_wr;
	int rc;

	if (rqpair->sends_to_post.first) {
		rc = ibv_post_send(rqpair->cm_id->qp, rqpair->sends_to_post.first, &bad_send_wr);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to post WRs on send queue, errno %d (%s), bad_wr %p\n",
				    rc, spdk_strerror(rc), bad_send_wr);
			/* Restart queue from bad wr. If it failed during
			 * completion processing, controller will be moved to
			 * failed state. Otherwise it will likely fail again
			 * in next submit attempt from completion processing.
			 */
			rqpair->sends_to_post.first = bad_send_wr;
			return -1;
		}
		rqpair->sends_to_post.first = NULL;
	}
	return 0;
}

static inline int
nvme_rdma_qpair_submit_recvs(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_recv_wr *bad_recv_wr;
	int rc;

	if (rqpair->recvs_to_post.first) {
		rc = ibv_post_recv(rqpair->cm_id->qp, rqpair->recvs_to_post.first, &bad_recv_wr);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("Failed to post WRs on receive queue, errno %d (%s), bad_wr %p\n",
				    rc, spdk_strerror(rc), bad_recv_wr);
			/* Restart queue from bad wr. If it failed during
			 * completion processing, controller will be moved to
			 * failed state. Otherwise it will likely fail again
			 * in next submit attempt from completion processing.
			 */
			rqpair->recvs_to_post.first = bad_recv_wr;
			return -1;
		}
		rqpair->recvs_to_post.first = NULL;
	}
	return 0;
}

/* Append the given send wr structure to the qpair's outstanding sends list. */
/* This function accepts only a single wr. */
static inline int
nvme_rdma_qpair_queue_send_wr(struct nvme_rdma_qpair *rqpair, struct ibv_send_wr *wr)
{
	assert(wr->next == NULL);

	if (rqpair->sends_to_post.first == NULL) {
		rqpair->sends_to_post.first = wr;
	} else {
		rqpair->sends_to_post.last->next = wr;
	}

	rqpair->sends_to_post.last = wr;

	if (!rqpair->delay_cmd_submit) {
		return nvme_rdma_qpair_submit_sends(rqpair);
	}

	return 0;
}

/* Append the given recv wr structure to the qpair's outstanding recvs list. */
/* This function accepts only a single wr. */
static inline int
nvme_rdma_qpair_queue_recv_wr(struct nvme_rdma_qpair *rqpair, struct ibv_recv_wr *wr)
{
	assert(wr->next == NULL);

	if (rqpair->recvs_to_post.first == NULL) {
		rqpair->recvs_to_post.first = wr;
	} else {
		rqpair->recvs_to_post.last->next = wr;
	}

	rqpair->recvs_to_post.last = wr;

	if (!rqpair->delay_cmd_submit) {
		return nvme_rdma_qpair_submit_recvs(rqpair);
	}

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
	struct ibv_recv_wr *wr;

	wr = &rqpair->rsp_recv_wrs[rsp_idx];
	wr->next = NULL;
	nvme_rdma_trace_ibv_sge(wr->sg_list);
	return nvme_rdma_qpair_queue_recv_wr(rqpair, wr);
}

static void
nvme_rdma_unregister_rsps(struct nvme_rdma_qpair *rqpair)
{
	if (rqpair->rsp_mr && rdma_dereg_mr(rqpair->rsp_mr)) {
		SPDK_ERRLOG("Unable to de-register rsp_mr\n");
	}
	rqpair->rsp_mr = NULL;
}

static void
nvme_rdma_free_rsps(struct nvme_rdma_qpair *rqpair)
{
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

	return 0;
fail:
	nvme_rdma_free_rsps(rqpair);
	return -ENOMEM;
}

static int
nvme_rdma_register_rsps(struct nvme_rdma_qpair *rqpair)
{
	uint16_t i;
	int rc;

	rqpair->rsp_mr = rdma_reg_msgs(rqpair->cm_id, rqpair->rsps,
				       rqpair->num_entries * sizeof(*rqpair->rsps));
	if (rqpair->rsp_mr == NULL) {
		rc = -errno;
		SPDK_ERRLOG("Unable to register rsp_mr: %s (%d)\n", spdk_strerror(errno), errno);
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

		rc = nvme_rdma_post_recv(rqpair, i);
		if (rc) {
			goto fail;
		}
	}

	rc = nvme_rdma_qpair_submit_recvs(rqpair);
	if (rc) {
		goto fail;
	}

	return 0;

fail:
	nvme_rdma_unregister_rsps(rqpair);
	return rc;
}

static void
nvme_rdma_unregister_reqs(struct nvme_rdma_qpair *rqpair)
{
	if (rqpair->cmd_mr && rdma_dereg_mr(rqpair->cmd_mr)) {
		SPDK_ERRLOG("Unable to de-register cmd_mr\n");
	}
	rqpair->cmd_mr = NULL;
}

static void
nvme_rdma_free_reqs(struct nvme_rdma_qpair *rqpair)
{
	if (!rqpair->rdma_reqs) {
		return;
	}

	free(rqpair->cmds);
	rqpair->cmds = NULL;

	free(rqpair->rdma_reqs);
	rqpair->rdma_reqs = NULL;
}

static int
nvme_rdma_alloc_reqs(struct nvme_rdma_qpair *rqpair)
{
	uint16_t i;

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


	TAILQ_INIT(&rqpair->free_reqs);
	TAILQ_INIT(&rqpair->outstanding_reqs);
	for (i = 0; i < rqpair->num_entries; i++) {
		struct spdk_nvme_rdma_req	*rdma_req;
		struct spdk_nvmf_cmd		*cmd;

		rdma_req = &rqpair->rdma_reqs[i];
		cmd = &rqpair->cmds[i];

		rdma_req->id = i;

		/* The first RDMA sgl element will always point
		 * at this data structure. Depending on whether
		 * an NVMe-oF SGL is required, the length of
		 * this element may change. */
		rdma_req->send_sgl[0].addr = (uint64_t)cmd;
		rdma_req->send_wr.wr_id = (uint64_t)rdma_req;
		rdma_req->send_wr.next = NULL;
		rdma_req->send_wr.opcode = IBV_WR_SEND;
		rdma_req->send_wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->send_wr.sg_list = rdma_req->send_sgl;
		rdma_req->send_wr.imm_data = 0;

		TAILQ_INSERT_TAIL(&rqpair->free_reqs, rdma_req, link);
	}

	return 0;
fail:
	nvme_rdma_free_reqs(rqpair);
	return -ENOMEM;
}

static int
nvme_rdma_register_reqs(struct nvme_rdma_qpair *rqpair)
{
	int i;

	rqpair->cmd_mr = rdma_reg_msgs(rqpair->cm_id, rqpair->cmds,
				       rqpair->num_entries * sizeof(*rqpair->cmds));
	if (!rqpair->cmd_mr) {
		SPDK_ERRLOG("Unable to register cmd_mr\n");
		goto fail;
	}

	for (i = 0; i < rqpair->num_entries; i++) {
		rqpair->rdma_reqs[i].send_sgl[0].lkey = rqpair->cmd_mr->lkey;
	}

	return 0;

fail:
	nvme_rdma_unregister_reqs(rqpair);
	return -ENOMEM;
}

static int
nvme_rdma_resolve_addr(struct nvme_rdma_qpair *rqpair,
		       struct sockaddr *src_addr,
		       struct sockaddr *dst_addr,
		       struct rdma_event_channel *cm_channel)
{
	int ret;

	ret = rdma_resolve_addr(rqpair->cm_id, src_addr, dst_addr,
				NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		SPDK_ERRLOG("rdma_resolve_addr, %d\n", errno);
		return ret;
	}

	ret = nvme_rdma_process_event(rqpair, cm_channel, RDMA_CM_EVENT_ADDR_RESOLVED);
	if (ret) {
		SPDK_ERRLOG("RDMA address resolution error\n");
		return -1;
	}

	if (rqpair->qpair.ctrlr->opts.transport_ack_timeout != SPDK_NVME_TRANSPORT_ACK_TIMEOUT_DISABLED) {
#ifdef SPDK_CONFIG_RDMA_SET_ACK_TIMEOUT
		uint8_t timeout = rqpair->qpair.ctrlr->opts.transport_ack_timeout;
		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID,
				      RDMA_OPTION_ID_ACK_TIMEOUT,
				      &timeout, sizeof(timeout));
		if (ret) {
			SPDK_NOTICELOG("Can't apply RDMA_OPTION_ID_ACK_TIMEOUT %d, ret %d\n", timeout, ret);
		}
#else
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "transport_ack_timeout is not supported\n");
#endif
	}


	ret = rdma_resolve_route(rqpair->cm_id, NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		SPDK_ERRLOG("rdma_resolve_route\n");
		return ret;
	}

	ret = nvme_rdma_process_event(rqpair, cm_channel, RDMA_CM_EVENT_ROUTE_RESOLVED);
	if (ret) {
		SPDK_ERRLOG("RDMA route resolution error\n");
		return -1;
	}

	return 0;
}

static int
nvme_rdma_connect(struct nvme_rdma_qpair *rqpair)
{
	struct rdma_conn_param				param = {};
	struct spdk_nvmf_rdma_request_private_data	request_data = {};
	struct ibv_device_attr				attr;
	int						ret;
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
	assert(rctrlr != NULL);

	request_data.qid = rqpair->qpair.id;
	request_data.hrqsize = rqpair->num_entries;
	request_data.hsqsize = rqpair->num_entries - 1;
	request_data.cntlid = ctrlr->cntlid;

	param.private_data = &request_data;
	param.private_data_len = sizeof(request_data);
	param.retry_count = ctrlr->opts.transport_retry_count;
	param.rnr_retry_count = 7;

	ret = rdma_connect(rqpair->cm_id, &param);
	if (ret) {
		SPDK_ERRLOG("nvme rdma connect error\n");
		return ret;
	}

	ret = nvme_rdma_process_event(rqpair, rctrlr->cm_channel, RDMA_CM_EVENT_ESTABLISHED);
	if (ret == -ESTALE) {
		SPDK_NOTICELOG("Received a stale connection notice during connection.\n");
		return -EAGAIN;
	} else if (ret) {
		SPDK_ERRLOG("RDMA connect error\n");
		return -1;
	} else {
		return 0;
	}
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
nvme_rdma_mr_map_notify(void *cb_ctx, struct spdk_mem_map *map,
			enum spdk_mem_map_notify_action action,
			void *vaddr, size_t size)
{
	struct ibv_pd *pd = cb_ctx;
	struct ibv_mr *mr;
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (!g_nvme_hooks.get_rkey) {
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
		} else {
			rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size,
							  g_nvme_hooks.get_rkey(pd, vaddr, size));
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		if (!g_nvme_hooks.get_rkey) {
			mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)vaddr, NULL);
			if (mr) {
				ibv_dereg_mr(mr);
			}
		}
		rc = spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return rc;
}

static int
nvme_rdma_check_contiguous_entries(uint64_t addr_1, uint64_t addr_2)
{
	/* Two contiguous mappings will point to the same address which is the start of the RDMA MR. */
	return addr_1 == addr_2;
}

static int
nvme_rdma_register_mem(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_pd *pd = rqpair->cm_id->qp->pd;
	struct spdk_nvme_rdma_mr_map *mr_map;
	const struct spdk_mem_map_ops nvme_rdma_map_ops = {
		.notify_cb = nvme_rdma_mr_map_notify,
		.are_contiguous = nvme_rdma_check_contiguous_entries
	};

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
	mr_map->map = spdk_mem_map_alloc((uint64_t)NULL, &nvme_rdma_map_ops, pd);
	if (mr_map->map == NULL) {
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
_nvme_rdma_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	bool src_addr_specified;
	int rc;
	struct nvme_rdma_ctrlr *rctrlr;
	struct nvme_rdma_qpair *rqpair;
	int family;

	rqpair = nvme_rdma_qpair(qpair);
	rctrlr = nvme_rdma_ctrlr(ctrlr);
	assert(rctrlr != NULL);

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

	rc = rdma_create_id(rctrlr->cm_channel, &rqpair->cm_id, rqpair, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		return -1;
	}

	rc = nvme_rdma_resolve_addr(rqpair,
				    src_addr_specified ? (struct sockaddr *)&src_addr : NULL,
				    (struct sockaddr *)&dst_addr, rctrlr->cm_channel);
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
		return rc;
	}

	rc = nvme_rdma_register_reqs(rqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to register rqpair RDMA requests\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA requests registered\n");

	rc = nvme_rdma_register_rsps(rqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to register rqpair RDMA responses\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA responses registered\n");

	rc = nvme_rdma_register_mem(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to register memory for RDMA\n");
		return -1;
	}

	rc = nvme_fabric_qpair_connect(&rqpair->qpair, rqpair->num_entries);
	if (rc < 0) {
		rqpair->qpair.transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		return -1;
	}

	return 0;
}

static int
nvme_rdma_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc;
	int retry_count = 0;

	rc = _nvme_rdma_ctrlr_connect_qpair(ctrlr, qpair);

	/*
	 * -EAGAIN represents the special case where the target side still thought it was connected.
	 * Most NICs will fail the first connection attempt, and the NICs will clean up whatever
	 * state they need to. After that, subsequent connection attempts will succeed.
	 */
	if (rc == -EAGAIN) {
		SPDK_NOTICELOG("Detected stale connection on Target side for qpid: %d\n", qpair->id);
		do {
			nvme_delay(NVME_RDMA_STALE_CONN_RETRY_DELAY_US);
			nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
			rc = _nvme_rdma_ctrlr_connect_qpair(ctrlr, qpair);
			retry_count++;
		} while (rc == -EAGAIN && retry_count < NVME_RDMA_STALE_CONN_RETRY_MAX);
	}

	return rc == -EAGAIN ? -1 : rc;
}

/*
 * Build SGL describing empty payload.
 */
static int
nvme_rdma_build_null_request(struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	/* The RDMA SGL needs one element describing the NVMe command. */
	rdma_req->send_wr.num_sge = 1;

	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.keyed.length = 0;
	req->cmd.dptr.sgl1.keyed.key = 0;
	req->cmd.dptr.sgl1.address = 0;

	return 0;
}

static inline bool
nvme_rdma_get_key(struct spdk_mem_map *map, void *payload, uint64_t size,
		  enum nvme_rdma_key_type key_type, uint32_t *key)
{
	struct ibv_mr *mr;
	uint64_t real_size = size;
	uint32_t _key = 0;

	if (!g_nvme_hooks.get_rkey) {
		mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)payload, &real_size);

		if (spdk_unlikely(!mr)) {
			SPDK_ERRLOG("No translation for ptr %p, size %lu\n", payload, size);
			return false;
		}
		switch (key_type) {
		case NVME_RDMA_MR_RKEY:
			_key = mr->rkey;
			break;
		case NVME_RDMA_MR_LKEY:
			_key = mr->lkey;
			break;
		default:
			SPDK_ERRLOG("Invalid key type %d\n", key_type);
			assert(0);
			return false;
		}
	} else {
		_key = spdk_mem_map_translate(map, (uint64_t)payload, &real_size);
	}

	if (spdk_unlikely(real_size < size)) {
		SPDK_ERRLOG("Data buffer split over multiple RDMA Memory Regions\n");
		return false;
	}

	*key = _key;
	return true;
}

/*
 * Build inline SGL describing contiguous payload buffer.
 */
static int
nvme_rdma_build_contig_inline_request(struct nvme_rdma_qpair *rqpair,
				      struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	uint32_t lkey;
	void *payload;

	payload = req->payload.contig_or_cb_arg + req->payload_offset;
	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	if (spdk_unlikely(!nvme_rdma_get_key(rqpair->mr_map->map, payload, req->payload_size,
					     NVME_RDMA_MR_LKEY, &lkey))) {
		return -1;
	}

	rdma_req->send_sgl[1].lkey = lkey;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	rdma_req->send_sgl[1].addr = (uint64_t)payload;
	rdma_req->send_sgl[1].length = (uint32_t)req->payload_size;

	/* The RDMA SGL contains two elements. The first describes
	 * the NVMe command and the second describes the data
	 * payload. */
	rdma_req->send_wr.num_sge = 2;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)req->payload_size;
	/* Inline only supported for icdoff == 0 currently.  This function will
	 * not get called for controllers with other values. */
	req->cmd.dptr.sgl1.address = (uint64_t)0;

	return 0;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
static int
nvme_rdma_build_contig_request(struct nvme_rdma_qpair *rqpair,
			       struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	void *payload = req->payload.contig_or_cb_arg + req->payload_offset;
	uint32_t rkey;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	if (spdk_unlikely(!nvme_rdma_get_key(rqpair->mr_map->map, payload, req->payload_size,
					     NVME_RDMA_MR_RKEY, &rkey))) {
		return -1;
	}

	req->cmd.dptr.sgl1.keyed.key = rkey;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	/* The RDMA SGL needs one element describing the NVMe command. */
	rdma_req->send_wr.num_sge = 1;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
	req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.keyed.length = req->payload_size;
	req->cmd.dptr.sgl1.address = (uint64_t)payload;

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static int
nvme_rdma_build_sgl_request(struct nvme_rdma_qpair *rqpair,
			    struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct spdk_nvmf_cmd *cmd = &rqpair->cmds[rdma_req->id];
	void *virt_addr;
	uint32_t remaining_size;
	uint32_t sge_length;
	int rc, max_num_sgl, num_sgl_desc;
	uint32_t rkey;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	max_num_sgl = req->qpair->ctrlr->max_sges;

	remaining_size = req->payload_size;
	num_sgl_desc = 0;
	do {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &sge_length);
		if (rc) {
			return -1;
		}

		sge_length = spdk_min(remaining_size, sge_length);

		if (spdk_unlikely(!nvme_rdma_get_key(rqpair->mr_map->map, virt_addr, sge_length,
						     NVME_RDMA_MR_RKEY, &rkey))) {
			return -1;
		}

		cmd->sgl[num_sgl_desc].keyed.key = rkey;
		cmd->sgl[num_sgl_desc].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		cmd->sgl[num_sgl_desc].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		cmd->sgl[num_sgl_desc].keyed.length = sge_length;
		cmd->sgl[num_sgl_desc].address = (uint64_t)virt_addr;

		remaining_size -= sge_length;
		num_sgl_desc++;
	} while (remaining_size > 0 && num_sgl_desc < max_num_sgl);


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	if (remaining_size > 0) {
		return -1;
	}

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

	/* The RDMA SGL needs one element describing some portion
	 * of the spdk_nvmf_cmd structure. */
	rdma_req->send_wr.num_sge = 1;

	/*
	 * If only one SGL descriptor is required, it can be embedded directly in the command
	 * as a data block descriptor.
	 */
	if (num_sgl_desc == 1) {
		/* The first element of this SGL is pointing at an
		 * spdk_nvmf_cmd object. For this particular command,
		 * we only need the first 64 bytes corresponding to
		 * the NVMe command. */
		rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

		req->cmd.dptr.sgl1.keyed.type = cmd->sgl[0].keyed.type;
		req->cmd.dptr.sgl1.keyed.subtype = cmd->sgl[0].keyed.subtype;
		req->cmd.dptr.sgl1.keyed.length = cmd->sgl[0].keyed.length;
		req->cmd.dptr.sgl1.keyed.key = cmd->sgl[0].keyed.key;
		req->cmd.dptr.sgl1.address = cmd->sgl[0].address;
	} else {
		/*
		 * Otherwise, The SGL descriptor embedded in the command must point to the list of
		 * SGL descriptors used to describe the operation. In that case it is a last segment descriptor.
		 */
		rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd) + sizeof(struct
					       spdk_nvme_sgl_descriptor) * num_sgl_desc;

		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
		req->cmd.dptr.sgl1.unkeyed.length = num_sgl_desc * sizeof(struct spdk_nvme_sgl_descriptor);
		req->cmd.dptr.sgl1.address = (uint64_t)0;
	}

	return 0;
}

/*
 * Build inline SGL describing sgl payload buffer.
 */
static int
nvme_rdma_build_sgl_inline_request(struct nvme_rdma_qpair *rqpair,
				   struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	uint32_t lkey;
	uint32_t length;
	void *virt_addr;
	int rc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &length);
	if (rc) {
		return -1;
	}

	if (length < req->payload_size) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Inline SGL request split so sending separately.\n");
		return nvme_rdma_build_sgl_request(rqpair, rdma_req);
	}

	if (length > req->payload_size) {
		length = req->payload_size;
	}

	if (spdk_unlikely(!nvme_rdma_get_key(rqpair->mr_map->map, virt_addr, length,
					     NVME_RDMA_MR_LKEY, &lkey))) {
		return -1;
	}

	rdma_req->send_sgl[1].addr = (uint64_t)virt_addr;
	rdma_req->send_sgl[1].length = length;
	rdma_req->send_sgl[1].lkey = lkey;

	rdma_req->send_wr.num_sge = 2;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)req->payload_size;
	/* Inline only supported for icdoff == 0 currently.  This function will
	 * not get called for controllers with other values. */
	req->cmd.dptr.sgl1.address = (uint64_t)0;

	return 0;
}

static int
nvme_rdma_req_init(struct nvme_rdma_qpair *rqpair, struct nvme_request *req,
		   struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_ctrlr *ctrlr = rqpair->qpair.ctrlr;
	enum nvme_payload_type payload_type;
	bool icd_supported;
	int rc;

	assert(rdma_req->req == NULL);
	rdma_req->req = req;
	req->cmd.cid = rdma_req->id;
	payload_type = nvme_payload_type(&req->payload);
	/*
	 * Check if icdoff is non zero, to avoid interop conflicts with
	 * targets with non-zero icdoff.  Both SPDK and the Linux kernel
	 * targets use icdoff = 0.  For targets with non-zero icdoff, we
	 * will currently just not use inline data for now.
	 */
	icd_supported = spdk_nvme_opc_get_data_transfer(req->cmd.opc) == SPDK_NVME_DATA_HOST_TO_CONTROLLER
			&& req->payload_size <= ctrlr->ioccsz_bytes && ctrlr->icdoff == 0;

	if (req->payload_size == 0) {
		rc = nvme_rdma_build_null_request(rdma_req);
	} else if (payload_type == NVME_PAYLOAD_TYPE_CONTIG) {
		if (icd_supported) {
			rc = nvme_rdma_build_contig_inline_request(rqpair, rdma_req);
		} else {
			rc = nvme_rdma_build_contig_request(rqpair, rdma_req);
		}
	} else if (payload_type == NVME_PAYLOAD_TYPE_SGL) {
		if (icd_supported) {
			rc = nvme_rdma_build_sgl_inline_request(rqpair, rdma_req);
		} else {
			rc = nvme_rdma_build_sgl_request(rqpair, rdma_req);
		}
	} else {
		rc = -1;
	}

	if (rc) {
		return rc;
	}

	memcpy(&rqpair->cmds[rdma_req->id], &req->cmd, sizeof(req->cmd));
	return 0;
}

static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			     uint16_t qid, uint32_t qsize,
			     enum spdk_nvme_qprio qprio,
			     uint32_t num_requests,
			     bool delay_cmd_submit)
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
	rqpair->delay_cmd_submit = delay_cmd_submit;
	qpair = &rqpair->qpair;
	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests);
	if (rc != 0) {
		return NULL;
	}

	rc = nvme_rdma_alloc_reqs(rqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate rqpair RDMA requests\n");
		free(rqpair);
		return NULL;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA requests allocated\n");

	rc = nvme_rdma_alloc_rsps(rqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to allocate rqpair RDMA responses\n");
		nvme_rdma_free_reqs(rqpair);
		free(rqpair);
		return NULL;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA responses allocated\n");

	return qpair;
}

static void
nvme_rdma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_ctrlr *rctrlr = NULL;
	struct nvme_rdma_cm_event_entry *entry, *tmp;

	nvme_rdma_unregister_mem(rqpair);
	nvme_rdma_unregister_reqs(rqpair);
	nvme_rdma_unregister_rsps(rqpair);

	if (rqpair->evt) {
		rdma_ack_cm_event(rqpair->evt);
		rqpair->evt = NULL;
	}

	/*
	 * This works because we have the controller lock both in
	 * this function and in the function where we add new events.
	 */
	if (qpair->ctrlr != NULL) {
		rctrlr = nvme_rdma_ctrlr(qpair->ctrlr);
		STAILQ_FOREACH_SAFE(entry, &rctrlr->pending_cm_events, link, tmp) {
			if (nvme_rdma_qpair(entry->evt->id->context) == rqpair) {
				STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
				rdma_ack_cm_event(entry->evt);
				STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);
			}
		}
	}

	if (rqpair->cm_id) {
		rdma_disconnect(rqpair->cm_id);
		if (rctrlr != NULL) {
			if (nvme_rdma_process_event(rqpair, rctrlr->cm_channel, RDMA_CM_EVENT_DISCONNECTED)) {
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "Target did not respond to qpair disconnect.\n");
			}
		}

		if (rqpair->cm_id->qp) {
			rdma_destroy_qp(rqpair->cm_id);
		}
		rdma_destroy_id(rqpair->cm_id);
		rqpair->cm_id = NULL;
	}

	if (rqpair->cq) {
		ibv_destroy_cq(rqpair->cq);
		rqpair->cq = NULL;
	}
}

static void nvme_rdma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);

static int
nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair;

	if (!qpair) {
		return -1;
	}
	nvme_transport_ctrlr_disconnect_qpair(ctrlr, qpair);
	nvme_rdma_qpair_abort_reqs(qpair, 1);
	nvme_qpair_deinit(qpair);

	rqpair = nvme_rdma_qpair(qpair);

	nvme_rdma_free_reqs(rqpair);
	nvme_rdma_free_rsps(rqpair);
	free(rqpair);

	return 0;
}

static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_rdma_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					    opts->io_queue_requests,
					    opts->delay_cmd_submit);
}

static int
nvme_rdma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	/* do nothing here */
	return 0;
}

static int nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);

static struct spdk_nvme_ctrlr *nvme_rdma_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct nvme_rdma_ctrlr *rctrlr;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_vs_register vs;
	struct ibv_context **contexts;
	struct ibv_device_attr dev_attr;
	int i, flag, rc;

	rctrlr = calloc(1, sizeof(struct nvme_rdma_ctrlr));
	if (rctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	rctrlr->ctrlr.opts = *opts;
	rctrlr->ctrlr.trid = *trid;

	if (opts->transport_retry_count > NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT) {
		SPDK_NOTICELOG("transport_retry_count exceeds max value %d, use max value\n",
			       NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT);
		rctrlr->ctrlr.opts.transport_retry_count = NVME_RDMA_CTRLR_MAX_TRANSPORT_RETRY_COUNT;
	}

	if (opts->transport_ack_timeout > NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) {
		SPDK_NOTICELOG("transport_ack_timeout exceeds max value %d, use max value\n",
			       NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
		rctrlr->ctrlr.opts.transport_ack_timeout = NVME_RDMA_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT;
	}

	contexts = rdma_get_devices(NULL);
	if (contexts == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		free(rctrlr);
		return NULL;
	}

	i = 0;
	rctrlr->max_sge = NVME_RDMA_MAX_SGL_DESCRIPTORS;

	while (contexts[i] != NULL) {
		rc = ibv_query_device(contexts[i], &dev_attr);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
			rdma_free_devices(contexts);
			free(rctrlr);
			return NULL;
		}
		rctrlr->max_sge = spdk_min(rctrlr->max_sge, (uint16_t)dev_attr.max_sge);
		i++;
	}

	rdma_free_devices(contexts);

	rc = nvme_ctrlr_construct(&rctrlr->ctrlr);
	if (rc != 0) {
		free(rctrlr);
		return NULL;
	}

	STAILQ_INIT(&rctrlr->pending_cm_events);
	STAILQ_INIT(&rctrlr->free_cm_events);
	rctrlr->cm_events = calloc(NVME_RDMA_NUM_CM_EVENTS, sizeof(*rctrlr->cm_events));
	if (rctrlr->cm_events == NULL) {
		SPDK_ERRLOG("unable to allocat buffers to hold CM events.\n");
		goto destruct_ctrlr;
	}

	for (i = 0; i < NVME_RDMA_NUM_CM_EVENTS; i++) {
		STAILQ_INSERT_TAIL(&rctrlr->free_cm_events, &rctrlr->cm_events[i], link);
	}

	rctrlr->cm_channel = rdma_create_event_channel();
	if (rctrlr->cm_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed\n");
		goto destruct_ctrlr;
	}

	flag = fcntl(rctrlr->cm_channel->fd, F_GETFL);
	if (fcntl(rctrlr->cm_channel->fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("Cannot set event channel to non blocking\n");
		goto destruct_ctrlr;
	}

	rctrlr->ctrlr.adminq = nvme_rdma_ctrlr_create_qpair(&rctrlr->ctrlr, 0,
			       rctrlr->ctrlr.opts.admin_queue_size, 0,
			       rctrlr->ctrlr.opts.admin_queue_size, false);
	if (!rctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		goto destruct_ctrlr;
	}

	rc = nvme_transport_ctrlr_connect_qpair(&rctrlr->ctrlr, rctrlr->ctrlr.adminq);
	if (rc < 0) {
		SPDK_ERRLOG("failed to connect admin qpair\n");
		goto destruct_ctrlr;
	}

	if (nvme_ctrlr_get_cap(&rctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		goto destruct_ctrlr;
	}

	if (nvme_ctrlr_get_vs(&rctrlr->ctrlr, &vs)) {
		SPDK_ERRLOG("get_vs() failed\n");
		goto destruct_ctrlr;
	}

	if (nvme_ctrlr_add_process(&rctrlr->ctrlr, 0) != 0) {
		SPDK_ERRLOG("nvme_ctrlr_add_process() failed\n");
		goto destruct_ctrlr;
	}

	nvme_ctrlr_init_cap(&rctrlr->ctrlr, &cap, &vs);

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "successfully initialized the nvmf ctrlr\n");
	return &rctrlr->ctrlr;

destruct_ctrlr:
	nvme_ctrlr_destruct(&rctrlr->ctrlr);
	return NULL;
}

static int
nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);
	struct nvme_rdma_cm_event_entry *entry;

	if (ctrlr->adminq) {
		nvme_rdma_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);
	}

	STAILQ_FOREACH(entry, &rctrlr->pending_cm_events, link) {
		rdma_ack_cm_event(entry->evt);
	}

	STAILQ_INIT(&rctrlr->free_cm_events);
	STAILQ_INIT(&rctrlr->pending_cm_events);
	free(rctrlr->cm_events);

	if (rctrlr->cm_channel) {
		rdma_destroy_event_channel(rctrlr->cm_channel);
		rctrlr->cm_channel = NULL;
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	free(rctrlr);

	return 0;
}

static int
nvme_rdma_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			       struct nvme_request *req)
{
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_rdma_req *rdma_req;
	struct ibv_send_wr *wr;

	rqpair = nvme_rdma_qpair(qpair);
	assert(rqpair != NULL);
	assert(req != NULL);

	rdma_req = nvme_rdma_req_get(rqpair);
	if (!rdma_req) {
		/* Inform the upper layer to try again later. */
		return -EAGAIN;
	}

	if (nvme_rdma_req_init(rqpair, req, rdma_req)) {
		SPDK_ERRLOG("nvme_rdma_req_init() failed\n");
		nvme_rdma_req_put(rqpair, rdma_req);
		return -1;
	}

	wr = &rdma_req->send_wr;
	wr->next = NULL;
	nvme_rdma_trace_ibv_sge(wr->sg_list);
	return nvme_rdma_qpair_queue_send_wr(rqpair, wr);
}

static int
nvme_rdma_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	/* Currently, doing nothing here */
	return 0;
}

static void
nvme_rdma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct nvme_request *req;
	struct spdk_nvme_cpl cpl;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);
		req = rdma_req->req;

		nvme_rdma_req_complete(req, &cpl);
		nvme_rdma_req_put(rqpair, rdma_req);
	}
}

static void
nvme_rdma_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = spdk_nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);

		if (nvme_request_check_timeout(rdma_req->req, rdma_req->id, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

static inline int
nvme_rdma_request_ready(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	nvme_rdma_req_complete(rdma_req->req, &rqpair->rsps[rdma_req->rsp_idx]);
	nvme_rdma_req_put(rqpair, rdma_req);
	return nvme_rdma_post_recv(rqpair, rdma_req->rsp_idx);
}

#define MAX_COMPLETIONS_PER_POLL 128

static int
nvme_rdma_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct nvme_rdma_qpair		*rqpair = nvme_rdma_qpair(qpair);
	struct ibv_wc			wc[MAX_COMPLETIONS_PER_POLL];
	int				i, rc = 0, batch_size;
	uint32_t			reaped = 0;
	uint16_t			rsp_idx;
	struct ibv_cq			*cq;
	struct spdk_nvme_rdma_req	*rdma_req;
	struct nvme_rdma_ctrlr		*rctrlr;
	struct spdk_nvme_cpl		*rsp;

	if (max_completions == 0) {
		max_completions = rqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, rqpair->num_entries);
	}

	if (nvme_qpair_is_admin_queue(&rqpair->qpair)) {
		rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
		nvme_rdma_poll_events(rctrlr);
	}
	nvme_rdma_qpair_process_cm_event(rqpair);

	if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE)) {
		goto fail;
	}

	cq = rqpair->cq;

	do {
		batch_size = spdk_min((max_completions - reaped),
				      MAX_COMPLETIONS_PER_POLL);
		rc = ibv_poll_cq(cq, batch_size, wc);
		if (rc < 0) {
			SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
				    errno, spdk_strerror(errno));
			goto fail;
		} else if (rc == 0) {
			/* Ran out of completions */
			break;
		}

		for (i = 0; i < rc; i++) {
			if (wc[i].status) {
				SPDK_ERRLOG("CQ error on Queue Pair %p, Response Index %lu (%d): %s\n",
					    qpair, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));
				goto fail;
			}

			switch (wc[i].opcode) {
			case IBV_WC_RECV:
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "CQ recv completion\n");

				if (wc[i].byte_len < sizeof(struct spdk_nvme_cpl)) {
					SPDK_ERRLOG("recv length %u less than expected response size\n", wc[i].byte_len);
					goto fail;
				}

				assert(wc[i].wr_id < rqpair->num_entries);
				rsp_idx = (uint16_t)wc[i].wr_id;
				rsp = &rqpair->rsps[rsp_idx];
				rdma_req = &rqpair->rdma_reqs[rsp->cid];
				rdma_req->completion_flags |= NVME_RDMA_RECV_COMPLETED;
				rdma_req->rsp_idx = rsp_idx;

				if ((rdma_req->completion_flags & NVME_RDMA_SEND_COMPLETED) != 0) {
					if (spdk_unlikely(nvme_rdma_request_ready(rqpair, rdma_req))) {
						SPDK_ERRLOG("Unable to re-post rx descriptor\n");
						goto fail;
					}
					reaped++;
				}
				break;

			case IBV_WC_SEND:
				rdma_req = (struct spdk_nvme_rdma_req *)wc[i].wr_id;
				rdma_req->completion_flags |= NVME_RDMA_SEND_COMPLETED;

				if ((rdma_req->completion_flags & NVME_RDMA_RECV_COMPLETED) != 0) {
					if (spdk_unlikely(nvme_rdma_request_ready(rqpair, rdma_req))) {
						SPDK_ERRLOG("Unable to re-post rx descriptor\n");
						goto fail;
					}
					reaped++;
				}
				break;

			default:
				SPDK_ERRLOG("Received an unexpected opcode on the CQ: %d\n", wc[i].opcode);
				goto fail;
			}
		}
	} while (reaped < max_completions);

	nvme_rdma_qpair_submit_sends(rqpair);
	nvme_rdma_qpair_submit_recvs(rqpair);

	if (spdk_unlikely(rqpair->qpair.ctrlr->timeout_enabled)) {
		nvme_rdma_qpair_check_timeout(qpair);
	}

	return reaped;

fail:
	/*
	 * Since admin queues take the ctrlr_lock before entering this function,
	 * we can call nvme_transport_ctrlr_disconnect_qpair. For other qpairs we need
	 * to call the generic function which will take the lock for us.
	 */
	if (rc == IBV_WC_RETRY_EXC_ERR) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;
	} else if (qpair->transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_NONE) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_transport_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);
	} else {
		nvme_ctrlr_disconnect_qpair(qpair);
	}
	return -ENXIO;
}

static uint32_t
nvme_rdma_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* max_mr_size by ibv_query_device indicates the largest value that we can
	 * set for a registered memory region.  It is independent from the actual
	 * I/O size and is very likely to be larger than 2 MiB which is the
	 * granularity we currently register memory regions.  Hence return
	 * UINT32_MAX here and let the generic layer use the controller data to
	 * moderate this value.
	 */
	return UINT32_MAX;
}

static uint16_t
nvme_rdma_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_rdma_ctrlr *rctrlr = nvme_rdma_ctrlr(ctrlr);

	return rctrlr->max_sge;
}

static void
nvme_rdma_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct nvme_request *req;
	struct spdk_nvme_cpl cpl;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		if (rdma_req->req->cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;
		}
		assert(rdma_req->req != NULL);
		req = rdma_req->req;

		nvme_rdma_req_complete(req, &cpl);
		nvme_rdma_req_put(rqpair, rdma_req);
	}
}

static struct spdk_nvme_transport_poll_group *
nvme_rdma_poll_group_create(void)
{
	struct nvme_rdma_poll_group *group = calloc(1, sizeof(*group));

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	return &group->group;
}

static int
nvme_rdma_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int
nvme_rdma_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int
nvme_rdma_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			 struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int
nvme_rdma_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			    struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static int64_t
nvme_rdma_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	int32_t local_completions = 0;
	int64_t total_completions = 0;

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		disconnected_qpair_cb(qpair, tgroup->group->ctx);
	}

	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
		local_completions = spdk_nvme_qpair_process_completions(qpair, completions_per_qpair);
		if (local_completions < 0) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
			local_completions = 0;
		}
		total_completions += local_completions;
	}

	return total_completions;
}

static int
nvme_rdma_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	free(tgroup);

	return 0;
}

void
spdk_nvme_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	g_nvme_hooks = *hooks;
}

const struct spdk_nvme_transport_ops rdma_ops = {
	.name = "RDMA",
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.ctrlr_construct = nvme_rdma_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,
	.ctrlr_destruct = nvme_rdma_ctrlr_destruct,
	.ctrlr_enable = nvme_rdma_ctrlr_enable,

	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,

	.ctrlr_get_max_xfer_size = nvme_rdma_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_rdma_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_rdma_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_rdma_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_rdma_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_rdma_ctrlr_disconnect_qpair,

	.qpair_abort_reqs = nvme_rdma_qpair_abort_reqs,
	.qpair_reset = nvme_rdma_qpair_reset,
	.qpair_submit_request = nvme_rdma_qpair_submit_request,
	.qpair_process_completions = nvme_rdma_qpair_process_completions,
	.admin_qpair_abort_aers = nvme_rdma_admin_qpair_abort_aers,

	.poll_group_create = nvme_rdma_poll_group_create,
	.poll_group_connect_qpair = nvme_rdma_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_rdma_poll_group_disconnect_qpair,
	.poll_group_add = nvme_rdma_poll_group_add,
	.poll_group_remove = nvme_rdma_poll_group_remove,
	.poll_group_process_completions = nvme_rdma_poll_group_process_completions,
	.poll_group_destroy = nvme_rdma_poll_group_destroy,

};

SPDK_NVME_TRANSPORT_REGISTER(rdma, &rdma_ops);
