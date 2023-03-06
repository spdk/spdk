/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over RDMA transport
 */

#include "spdk/stdinc.h"

#include "spdk/assert.h"
#include "spdk/dma.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/config.h"

#include "nvme_internal.h"
#include "spdk_internal/rdma.h"

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

/* The default size for a shared rdma completion queue. */
#define DEFAULT_NVME_RDMA_CQ_SIZE		4096

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

/*
 * Number of microseconds to wait until the lingering qpair becomes quiet.
 */
#define NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US	1000000ull

/*
 * The max length of keyed SGL data block (3 bytes)
 */
#define NVME_RDMA_MAX_KEYED_SGL_LENGTH ((1u << 24u) - 1)

#define WC_PER_QPAIR(queue_depth)	(queue_depth * 2)

#define NVME_RDMA_POLL_GROUP_CHECK_QPN(_rqpair, qpn)				\
	((_rqpair)->rdma_qp && (_rqpair)->rdma_qp->qp->qp_num == (qpn))	\

struct nvme_rdma_memory_domain {
	TAILQ_ENTRY(nvme_rdma_memory_domain) link;
	uint32_t ref;
	struct ibv_pd *pd;
	struct spdk_memory_domain *domain;
	struct spdk_memory_domain_rdma_ctx rdma_ctx;
};

enum nvme_rdma_wr_type {
	RDMA_WR_TYPE_RECV,
	RDMA_WR_TYPE_SEND,
};

struct nvme_rdma_wr {
	/* Using this instead of the enum allows this struct to only occupy one byte. */
	uint8_t	type;
};

struct spdk_nvmf_cmd {
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_sgl_descriptor sgl[NVME_RDMA_MAX_SGL_DESCRIPTORS];
};

struct spdk_nvme_rdma_hooks g_nvme_hooks = {};

/* STAILQ wrapper for cm events. */
struct nvme_rdma_cm_event_entry {
	struct rdma_cm_event			*evt;
	STAILQ_ENTRY(nvme_rdma_cm_event_entry)	link;
};

/* NVMe RDMA transport extensions for spdk_nvme_ctrlr */
struct nvme_rdma_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;

	uint16_t				max_sge;

	struct rdma_event_channel		*cm_channel;

	STAILQ_HEAD(, nvme_rdma_cm_event_entry)	pending_cm_events;

	STAILQ_HEAD(, nvme_rdma_cm_event_entry)	free_cm_events;

	struct nvme_rdma_cm_event_entry		*cm_events;
};

struct nvme_rdma_poller_stats {
	uint64_t polls;
	uint64_t idle_polls;
	uint64_t queued_requests;
	uint64_t completions;
	struct spdk_rdma_qp_stats rdma_stats;
};

struct nvme_rdma_poll_group;
struct nvme_rdma_rsps;

struct nvme_rdma_poller {
	struct ibv_context		*device;
	struct ibv_cq			*cq;
	struct spdk_rdma_srq		*srq;
	struct nvme_rdma_rsps		*rsps;
	struct ibv_pd			*pd;
	struct spdk_rdma_mem_map	*mr_map;
	uint32_t			refcnt;
	int				required_num_wc;
	int				current_num_wc;
	struct nvme_rdma_poller_stats	stats;
	struct nvme_rdma_poll_group	*group;
	STAILQ_ENTRY(nvme_rdma_poller)	link;
};

struct nvme_rdma_poll_group {
	struct spdk_nvme_transport_poll_group		group;
	STAILQ_HEAD(, nvme_rdma_poller)			pollers;
	uint32_t					num_pollers;
};

enum nvme_rdma_qpair_state {
	NVME_RDMA_QPAIR_STATE_INVALID = 0,
	NVME_RDMA_QPAIR_STATE_STALE_CONN,
	NVME_RDMA_QPAIR_STATE_INITIALIZING,
	NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND,
	NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL,
	NVME_RDMA_QPAIR_STATE_RUNNING,
	NVME_RDMA_QPAIR_STATE_EXITING,
	NVME_RDMA_QPAIR_STATE_LINGERING,
	NVME_RDMA_QPAIR_STATE_EXITED,
};

struct nvme_rdma_qpair;

typedef int (*nvme_rdma_cm_event_cb)(struct nvme_rdma_qpair *rqpair, int ret);

struct nvme_rdma_rsp_opts {
	uint16_t				num_entries;
	struct nvme_rdma_qpair			*rqpair;
	struct spdk_rdma_srq			*srq;
	struct spdk_rdma_mem_map		*mr_map;
};

struct nvme_rdma_rsps {
	/* Parallel arrays of response buffers + response SGLs of size num_entries */
	struct ibv_sge				*rsp_sgls;
	struct spdk_nvme_rdma_rsp		*rsps;

	struct ibv_recv_wr			*rsp_recv_wrs;

	/* Count of outstanding recv objects */
	uint16_t				current_num_recvs;

	uint16_t				num_entries;
};

/* NVMe RDMA qpair extensions for spdk_nvme_qpair */
struct nvme_rdma_qpair {
	struct spdk_nvme_qpair			qpair;

	struct spdk_rdma_qp			*rdma_qp;
	struct rdma_cm_id			*cm_id;
	struct ibv_cq				*cq;
	struct spdk_rdma_srq			*srq;

	struct	spdk_nvme_rdma_req		*rdma_reqs;

	uint32_t				max_send_sge;

	uint32_t				max_recv_sge;

	uint16_t				num_entries;

	bool					delay_cmd_submit;

	uint32_t				num_completions;

	struct nvme_rdma_rsps			*rsps;

	/*
	 * Array of num_entries NVMe commands registered as RDMA message buffers.
	 * Indexed by rdma_req->id.
	 */
	struct spdk_nvmf_cmd			*cmds;

	struct spdk_rdma_mem_map		*mr_map;

	TAILQ_HEAD(, spdk_nvme_rdma_req)	free_reqs;
	TAILQ_HEAD(, spdk_nvme_rdma_req)	outstanding_reqs;

	struct nvme_rdma_memory_domain		*memory_domain;

	/* Count of outstanding send objects */
	uint16_t				current_num_sends;

	/* Placed at the end of the struct since it is not used frequently */
	struct rdma_cm_event			*evt;
	struct nvme_rdma_poller			*poller;

	uint64_t				evt_timeout_ticks;
	nvme_rdma_cm_event_cb			evt_cb;
	enum rdma_cm_event_type			expected_evt_type;

	enum nvme_rdma_qpair_state		state;

	bool					in_connect_poll;

	uint8_t					stale_conn_retry_count;
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
	 * during processing of RDMA_SEND. To complete the request we must know the response
	 * received in RDMA_RECV, so store it in this field */
	struct spdk_nvme_rdma_rsp		*rdma_rsp;

	struct nvme_rdma_wr			rdma_wr;

	struct ibv_send_wr			send_wr;

	struct nvme_request			*req;

	struct ibv_sge				send_sgl[NVME_RDMA_DEFAULT_TX_SGE];

	TAILQ_ENTRY(spdk_nvme_rdma_req)		link;
};

struct spdk_nvme_rdma_rsp {
	struct spdk_nvme_cpl	cpl;
	struct nvme_rdma_qpair	*rqpair;
	struct ibv_recv_wr	*recv_wr;
	struct nvme_rdma_wr	rdma_wr;
};

struct nvme_rdma_memory_translation_ctx {
	void *addr;
	size_t length;
	uint32_t lkey;
	uint32_t rkey;
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

static struct nvme_rdma_poller *nvme_rdma_poll_group_get_poller(struct nvme_rdma_poll_group *group,
		struct ibv_context *device);
static void nvme_rdma_poll_group_put_poller(struct nvme_rdma_poll_group *group,
		struct nvme_rdma_poller *poller);

static TAILQ_HEAD(, nvme_rdma_memory_domain) g_memory_domains = TAILQ_HEAD_INITIALIZER(
			g_memory_domains);
static pthread_mutex_t g_memory_domains_lock = PTHREAD_MUTEX_INITIALIZER;

static struct nvme_rdma_memory_domain *
nvme_rdma_get_memory_domain(struct ibv_pd *pd)
{
	struct nvme_rdma_memory_domain *domain = NULL;
	struct spdk_memory_domain_ctx ctx;
	int rc;

	pthread_mutex_lock(&g_memory_domains_lock);

	TAILQ_FOREACH(domain, &g_memory_domains, link) {
		if (domain->pd == pd) {
			domain->ref++;
			pthread_mutex_unlock(&g_memory_domains_lock);
			return domain;
		}
	}

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		SPDK_ERRLOG("Memory allocation failed\n");
		pthread_mutex_unlock(&g_memory_domains_lock);
		return NULL;
	}

	domain->rdma_ctx.size = sizeof(domain->rdma_ctx);
	domain->rdma_ctx.ibv_pd = pd;
	ctx.size = sizeof(ctx);
	ctx.user_ctx = &domain->rdma_ctx;

	rc = spdk_memory_domain_create(&domain->domain, SPDK_DMA_DEVICE_TYPE_RDMA, &ctx,
				       SPDK_RDMA_DMA_DEVICE);
	if (rc) {
		SPDK_ERRLOG("Failed to create memory domain\n");
		free(domain);
		pthread_mutex_unlock(&g_memory_domains_lock);
		return NULL;
	}

	domain->pd = pd;
	domain->ref = 1;
	TAILQ_INSERT_TAIL(&g_memory_domains, domain, link);

	pthread_mutex_unlock(&g_memory_domains_lock);

	return domain;
}

static void
nvme_rdma_put_memory_domain(struct nvme_rdma_memory_domain *device)
{
	if (!device) {
		return;
	}

	pthread_mutex_lock(&g_memory_domains_lock);

	assert(device->ref > 0);

	device->ref--;

	if (device->ref == 0) {
		spdk_memory_domain_destroy(device->domain);
		TAILQ_REMOVE(&g_memory_domains, device, link);
		free(device);
	}

	pthread_mutex_unlock(&g_memory_domains_lock);
}

static int nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);

static inline struct nvme_rdma_qpair *
nvme_rdma_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_RDMA);
	return SPDK_CONTAINEROF(qpair, struct nvme_rdma_qpair, qpair);
}

static inline struct nvme_rdma_poll_group *
nvme_rdma_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return (SPDK_CONTAINEROF(group, struct nvme_rdma_poll_group, group));
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
	TAILQ_INSERT_HEAD(&rqpair->free_reqs, rdma_req, link);
}

static void
nvme_rdma_req_complete(struct spdk_nvme_rdma_req *rdma_req,
		       struct spdk_nvme_cpl *rsp,
		       bool print_on_error)
{
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_qpair *qpair;
	bool error, print_error;

	assert(req != NULL);

	qpair = req->qpair;
	rqpair = nvme_rdma_qpair(qpair);

	error = spdk_nvme_cpl_is_error(rsp);
	print_error = error && print_on_error && !qpair->ctrlr->opts.disable_error_logging;

	if (print_error) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
	}

	if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
		spdk_nvme_qpair_print_completion(qpair, rsp);
	}

	TAILQ_REMOVE(&rqpair->outstanding_reqs, rdma_req, link);

	nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, rsp);
	nvme_free_request(req);
	nvme_rdma_req_put(rqpair, rdma_req);
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
		case RDMA_CM_EVENT_CONNECT_ERROR:
			break;
		case RDMA_CM_EVENT_UNREACHABLE:
		case RDMA_CM_EVENT_REJECTED:
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:
			rc = spdk_rdma_qp_complete_connect(rqpair->rdma_qp);
		/* fall through */
		case RDMA_CM_EVENT_ESTABLISHED:
			accept_data = (struct spdk_nvmf_rdma_accept_private_data *)event->param.conn.private_data;
			if (accept_data == NULL) {
				rc = -1;
			} else {
				SPDK_DEBUGLOG(nvme, "Requested queue depth %d. Target receive queue depth %d.\n",
					      rqpair->num_entries + 1, accept_data->crqsize);
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
		event_qpair = entry->evt->id->context;
		if (event_qpair->evt == NULL) {
			event_qpair->evt = entry->evt;
			STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
			STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);
		}
	}

	while (rdma_get_cm_event(channel, &event) == 0) {
		event_qpair = event->id->context;
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

	/* rdma_get_cm_event() returns -1 on error. If an error occurs, errno
	 * will be set to indicate the failure reason. So return negated errno here.
	 */
	return -errno;
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
		} else if (reaped_evt->event == RDMA_CM_EVENT_CONNECT_RESPONSE) {
			/*
			 *  If we are using a qpair which is not created using rdma cm API
			 *  then we will receive RDMA_CM_EVENT_CONNECT_RESPONSE instead of
			 *  RDMA_CM_EVENT_ESTABLISHED.
			 */
			return 0;
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
nvme_rdma_process_event_start(struct nvme_rdma_qpair *rqpair,
			      enum rdma_cm_event_type evt,
			      nvme_rdma_cm_event_cb evt_cb)
{
	int	rc;

	assert(evt_cb != NULL);

	if (rqpair->evt != NULL) {
		rc = nvme_rdma_qpair_process_cm_event(rqpair);
		if (rc) {
			return rc;
		}
	}

	rqpair->expected_evt_type = evt;
	rqpair->evt_cb = evt_cb;
	rqpair->evt_timeout_ticks = (NVME_RDMA_QPAIR_CM_EVENT_TIMEOUT_US * spdk_get_ticks_hz()) /
				    SPDK_SEC_TO_USEC + spdk_get_ticks();

	return 0;
}

static int
nvme_rdma_process_event_poll(struct nvme_rdma_qpair *rqpair)
{
	struct nvme_rdma_ctrlr	*rctrlr;
	int	rc = 0, rc2;

	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	assert(rctrlr != NULL);

	if (!rqpair->evt && spdk_get_ticks() < rqpair->evt_timeout_ticks) {
		rc = nvme_rdma_poll_events(rctrlr);
		if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
			return rc;
		}
	}

	if (rqpair->evt == NULL) {
		rc = -EADDRNOTAVAIL;
		goto exit;
	}

	rc = nvme_rdma_validate_cm_event(rqpair->expected_evt_type, rqpair->evt);

	rc2 = nvme_rdma_qpair_process_cm_event(rqpair);
	/* bad message takes precedence over the other error codes from processing the event. */
	rc = rc == 0 ? rc2 : rc;

exit:
	assert(rqpair->evt_cb != NULL);
	return rqpair->evt_cb(rqpair, rc);
}

static int
nvme_rdma_resize_cq(struct nvme_rdma_qpair *rqpair, struct nvme_rdma_poller *poller)
{
	int	current_num_wc, required_num_wc;

	required_num_wc = poller->required_num_wc + WC_PER_QPAIR(rqpair->num_entries);
	current_num_wc = poller->current_num_wc;
	if (current_num_wc < required_num_wc) {
		current_num_wc = spdk_max(current_num_wc * 2, required_num_wc);
	}

	if (poller->current_num_wc != current_num_wc) {
		SPDK_DEBUGLOG(nvme, "Resize RDMA CQ from %d to %d\n", poller->current_num_wc,
			      current_num_wc);
		if (ibv_resize_cq(poller->cq, current_num_wc)) {
			SPDK_ERRLOG("RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		poller->current_num_wc = current_num_wc;
	}

	poller->required_num_wc = required_num_wc;
	return 0;
}

static int
nvme_rdma_qpair_set_poller(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair          *rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_poll_group     *group = nvme_rdma_poll_group(qpair->poll_group);
	struct nvme_rdma_poller         *poller;

	assert(rqpair->cq == NULL);

	poller = nvme_rdma_poll_group_get_poller(group, rqpair->cm_id->verbs);
	if (!poller) {
		SPDK_ERRLOG("Unable to find a cq for qpair %p on poll group %p\n", qpair, qpair->poll_group);
		return -EINVAL;
	}

	if (!poller->srq) {
		if (nvme_rdma_resize_cq(rqpair, poller)) {
			nvme_rdma_poll_group_put_poller(group, poller);
			return -EPROTO;
		}
	}

	rqpair->cq = poller->cq;
	rqpair->srq = poller->srq;
	if (rqpair->srq) {
		rqpair->rsps = poller->rsps;
	}
	rqpair->poller = poller;
	return 0;
}

static int
nvme_rdma_qpair_init(struct nvme_rdma_qpair *rqpair)
{
	int			rc;
	struct spdk_rdma_qp_init_attr	attr = {};
	struct ibv_device_attr	dev_attr;
	struct nvme_rdma_ctrlr	*rctrlr;

	rc = ibv_query_device(rqpair->cm_id->verbs, &dev_attr);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		return -1;
	}

	if (rqpair->qpair.poll_group) {
		assert(!rqpair->cq);
		rc = nvme_rdma_qpair_set_poller(&rqpair->qpair);
		if (rc) {
			SPDK_ERRLOG("Unable to activate the rdmaqpair.\n");
			return -1;
		}
		assert(rqpair->cq);
	} else {
		rqpair->cq = ibv_create_cq(rqpair->cm_id->verbs, rqpair->num_entries * 2, rqpair, NULL, 0);
		if (!rqpair->cq) {
			SPDK_ERRLOG("Unable to create completion queue: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;
		}
	}

	rctrlr = nvme_rdma_ctrlr(rqpair->qpair.ctrlr);
	if (g_nvme_hooks.get_ibv_pd) {
		attr.pd = g_nvme_hooks.get_ibv_pd(&rctrlr->ctrlr.trid, rqpair->cm_id->verbs);
	} else {
		attr.pd = spdk_rdma_get_pd(rqpair->cm_id->verbs);
	}

	attr.stats =		rqpair->poller ? &rqpair->poller->stats.rdma_stats : NULL;
	attr.send_cq		= rqpair->cq;
	attr.recv_cq		= rqpair->cq;
	attr.cap.max_send_wr	= rqpair->num_entries; /* SEND operations */
	if (rqpair->srq) {
		attr.srq	= rqpair->srq->srq;
	} else {
		attr.cap.max_recv_wr = rqpair->num_entries; /* RECV operations */
	}
	attr.cap.max_send_sge	= spdk_min(NVME_RDMA_DEFAULT_TX_SGE, dev_attr.max_sge);
	attr.cap.max_recv_sge	= spdk_min(NVME_RDMA_DEFAULT_RX_SGE, dev_attr.max_sge);

	rqpair->rdma_qp = spdk_rdma_qp_create(rqpair->cm_id, &attr);

	if (!rqpair->rdma_qp) {
		return -1;
	}

	rqpair->memory_domain = nvme_rdma_get_memory_domain(rqpair->rdma_qp->qp->pd);
	if (!rqpair->memory_domain) {
		SPDK_ERRLOG("Failed to get memory domain\n");
		return -1;
	}

	/* ibv_create_qp will change the values in attr.cap. Make sure we store the proper value. */
	rqpair->max_send_sge = spdk_min(NVME_RDMA_DEFAULT_TX_SGE, attr.cap.max_send_sge);
	rqpair->max_recv_sge = spdk_min(NVME_RDMA_DEFAULT_RX_SGE, attr.cap.max_recv_sge);
	rqpair->current_num_sends = 0;

	rqpair->cm_id->context = rqpair;

	return 0;
}

static void
nvme_rdma_reset_failed_sends(struct nvme_rdma_qpair *rqpair,
			     struct ibv_send_wr *bad_send_wr, int rc)
{
	SPDK_ERRLOG("Failed to post WRs on send queue, errno %d (%s), bad_wr %p\n",
		    rc, spdk_strerror(rc), bad_send_wr);
	while (bad_send_wr != NULL) {
		assert(rqpair->current_num_sends > 0);
		rqpair->current_num_sends--;
		bad_send_wr = bad_send_wr->next;
	}
}

static void
nvme_rdma_reset_failed_recvs(struct nvme_rdma_rsps *rsps,
			     struct ibv_recv_wr *bad_recv_wr, int rc)
{
	SPDK_ERRLOG("Failed to post WRs on receive queue, errno %d (%s), bad_wr %p\n",
		    rc, spdk_strerror(rc), bad_recv_wr);
	while (bad_recv_wr != NULL) {
		assert(rsps->current_num_recvs > 0);
		rsps->current_num_recvs--;
		bad_recv_wr = bad_recv_wr->next;
	}
}

static inline int
nvme_rdma_qpair_submit_sends(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_send_wr *bad_send_wr = NULL;
	int rc;

	rc = spdk_rdma_qp_flush_send_wrs(rqpair->rdma_qp, &bad_send_wr);

	if (spdk_unlikely(rc)) {
		nvme_rdma_reset_failed_sends(rqpair, bad_send_wr, rc);
	}

	return rc;
}

static inline int
nvme_rdma_qpair_submit_recvs(struct nvme_rdma_qpair *rqpair)
{
	struct ibv_recv_wr *bad_recv_wr;
	int rc = 0;

	rc = spdk_rdma_qp_flush_recv_wrs(rqpair->rdma_qp, &bad_recv_wr);
	if (spdk_unlikely(rc)) {
		nvme_rdma_reset_failed_recvs(rqpair->rsps, bad_recv_wr, rc);
	}

	return rc;
}

static inline int
nvme_rdma_poller_submit_recvs(struct nvme_rdma_poller *poller)
{
	struct ibv_recv_wr *bad_recv_wr;
	int rc;

	rc = spdk_rdma_srq_flush_recv_wrs(poller->srq, &bad_recv_wr);
	if (spdk_unlikely(rc)) {
		nvme_rdma_reset_failed_recvs(poller->rsps, bad_recv_wr, rc);
	}

	return rc;
}

#define nvme_rdma_trace_ibv_sge(sg_list) \
	if (sg_list) { \
		SPDK_DEBUGLOG(nvme, "local addr %p length 0x%x lkey 0x%x\n", \
			      (void *)(sg_list)->addr, (sg_list)->length, (sg_list)->lkey); \
	}

static void
nvme_rdma_free_rsps(struct nvme_rdma_rsps *rsps)
{
	if (!rsps) {
		return;
	}

	spdk_free(rsps->rsps);
	spdk_free(rsps->rsp_sgls);
	spdk_free(rsps->rsp_recv_wrs);
	spdk_free(rsps);
}

static struct nvme_rdma_rsps *
nvme_rdma_create_rsps(struct nvme_rdma_rsp_opts *opts)
{
	struct nvme_rdma_rsps *rsps;
	struct spdk_rdma_memory_translation translation;
	uint16_t i;
	int rc;

	rsps = spdk_zmalloc(sizeof(*rsps), 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!rsps) {
		SPDK_ERRLOG("Failed to allocate rsps object\n");
		return NULL;
	}

	rsps->rsp_sgls = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsp_sgls), 0, NULL,
				      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!rsps->rsp_sgls) {
		SPDK_ERRLOG("Failed to allocate rsp_sgls\n");
		goto fail;
	}

	rsps->rsp_recv_wrs = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsp_recv_wrs), 0, NULL,
					  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!rsps->rsp_recv_wrs) {
		SPDK_ERRLOG("Failed to allocate rsp_recv_wrs\n");
		goto fail;
	}

	rsps->rsps = spdk_zmalloc(opts->num_entries * sizeof(*rsps->rsps), 0, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!rsps->rsps) {
		SPDK_ERRLOG("can not allocate rdma rsps\n");
		goto fail;
	}

	for (i = 0; i < opts->num_entries; i++) {
		struct ibv_sge *rsp_sgl = &rsps->rsp_sgls[i];
		struct spdk_nvme_rdma_rsp *rsp = &rsps->rsps[i];
		struct ibv_recv_wr *recv_wr = &rsps->rsp_recv_wrs[i];

		rsp->rqpair = opts->rqpair;
		rsp->rdma_wr.type = RDMA_WR_TYPE_RECV;
		rsp->recv_wr = recv_wr;
		rsp_sgl->addr = (uint64_t)rsp;
		rsp_sgl->length = sizeof(struct spdk_nvme_cpl);
		rc = spdk_rdma_get_translation(opts->mr_map, rsp, sizeof(*rsp), &translation);
		if (rc) {
			goto fail;
		}
		rsp_sgl->lkey = spdk_rdma_memory_translation_get_lkey(&translation);

		recv_wr->wr_id = (uint64_t)&rsp->rdma_wr;
		recv_wr->next = NULL;
		recv_wr->sg_list = rsp_sgl;
		recv_wr->num_sge = 1;

		nvme_rdma_trace_ibv_sge(recv_wr->sg_list);

		if (opts->rqpair) {
			spdk_rdma_qp_queue_recv_wrs(opts->rqpair->rdma_qp, recv_wr);
		} else {
			spdk_rdma_srq_queue_recv_wrs(opts->srq, recv_wr);
		}
	}

	rsps->num_entries = opts->num_entries;
	rsps->current_num_recvs = opts->num_entries;

	return rsps;
fail:
	nvme_rdma_free_rsps(rsps);
	return NULL;
}

static void
nvme_rdma_free_reqs(struct nvme_rdma_qpair *rqpair)
{
	if (!rqpair->rdma_reqs) {
		return;
	}

	spdk_free(rqpair->cmds);
	rqpair->cmds = NULL;

	spdk_free(rqpair->rdma_reqs);
	rqpair->rdma_reqs = NULL;
}

static int
nvme_rdma_create_reqs(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_rdma_memory_translation translation;
	uint16_t i;
	int rc;

	assert(!rqpair->rdma_reqs);
	rqpair->rdma_reqs = spdk_zmalloc(rqpair->num_entries * sizeof(struct spdk_nvme_rdma_req), 0, NULL,
					 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (rqpair->rdma_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate rdma_reqs\n");
		goto fail;
	}

	assert(!rqpair->cmds);
	rqpair->cmds = spdk_zmalloc(rqpair->num_entries * sizeof(*rqpair->cmds), 0, NULL,
				    SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
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
		rdma_req->rdma_wr.type = RDMA_WR_TYPE_SEND;
		cmd = &rqpair->cmds[i];

		rdma_req->id = i;

		rc = spdk_rdma_get_translation(rqpair->mr_map, cmd, sizeof(*cmd), &translation);
		if (rc) {
			goto fail;
		}
		rdma_req->send_sgl[0].lkey = spdk_rdma_memory_translation_get_lkey(&translation);

		/* The first RDMA sgl element will always point
		 * at this data structure. Depending on whether
		 * an NVMe-oF SGL is required, the length of
		 * this element may change. */
		rdma_req->send_sgl[0].addr = (uint64_t)cmd;
		rdma_req->send_wr.wr_id = (uint64_t)&rdma_req->rdma_wr;
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

static int nvme_rdma_connect(struct nvme_rdma_qpair *rqpair);

static int
nvme_rdma_route_resolved(struct nvme_rdma_qpair *rqpair, int ret)
{
	if (ret) {
		SPDK_ERRLOG("RDMA route resolution error\n");
		return -1;
	}

	ret = nvme_rdma_qpair_init(rqpair);
	if (ret < 0) {
		SPDK_ERRLOG("nvme_rdma_qpair_init() failed\n");
		return -1;
	}

	return nvme_rdma_connect(rqpair);
}

static int
nvme_rdma_addr_resolved(struct nvme_rdma_qpair *rqpair, int ret)
{
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
		SPDK_DEBUGLOG(nvme, "transport_ack_timeout is not supported\n");
#endif
	}

	if (rqpair->qpair.ctrlr->opts.transport_tos != SPDK_NVME_TRANSPORT_TOS_DISABLED) {
#ifdef SPDK_CONFIG_RDMA_SET_TOS
		uint8_t tos = rqpair->qpair.ctrlr->opts.transport_tos;
		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_TOS, &tos, sizeof(tos));
		if (ret) {
			SPDK_NOTICELOG("Can't apply RDMA_OPTION_ID_TOS %u, ret %d\n", tos, ret);
		}
#else
		SPDK_DEBUGLOG(nvme, "transport_tos is not supported\n");
#endif
	}

	ret = rdma_resolve_route(rqpair->cm_id, NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		SPDK_ERRLOG("rdma_resolve_route\n");
		return ret;
	}

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ROUTE_RESOLVED,
					     nvme_rdma_route_resolved);
}

static int
nvme_rdma_resolve_addr(struct nvme_rdma_qpair *rqpair,
		       struct sockaddr *src_addr,
		       struct sockaddr *dst_addr)
{
	int ret;

	if (src_addr) {
		int reuse = 1;

		ret = rdma_set_option(rqpair->cm_id, RDMA_OPTION_ID, RDMA_OPTION_ID_REUSEADDR,
				      &reuse, sizeof(reuse));
		if (ret) {
			SPDK_NOTICELOG("Can't apply RDMA_OPTION_ID_REUSEADDR %d, ret %d\n",
				       reuse, ret);
			/* It is likely that rdma_resolve_addr() returns -EADDRINUSE, but
			 * we may missing something. We rely on rdma_resolve_addr().
			 */
		}
	}

	ret = rdma_resolve_addr(rqpair->cm_id, src_addr, dst_addr,
				NVME_RDMA_TIME_OUT_IN_MS);
	if (ret) {
		SPDK_ERRLOG("rdma_resolve_addr, %d\n", errno);
		return ret;
	}

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ADDR_RESOLVED,
					     nvme_rdma_addr_resolved);
}

static int nvme_rdma_stale_conn_retry(struct nvme_rdma_qpair *rqpair);

static int
nvme_rdma_connect_established(struct nvme_rdma_qpair *rqpair, int ret)
{
	struct nvme_rdma_rsp_opts opts = {};

	if (ret == -ESTALE) {
		return nvme_rdma_stale_conn_retry(rqpair);
	} else if (ret) {
		SPDK_ERRLOG("RDMA connect error %d\n", ret);
		return ret;
	}

	assert(!rqpair->mr_map);
	rqpair->mr_map = spdk_rdma_create_mem_map(rqpair->rdma_qp->qp->pd, &g_nvme_hooks,
			 SPDK_RDMA_MEMORY_MAP_ROLE_INITIATOR);
	if (!rqpair->mr_map) {
		SPDK_ERRLOG("Unable to register RDMA memory translation map\n");
		return -1;
	}

	ret = nvme_rdma_create_reqs(rqpair);
	SPDK_DEBUGLOG(nvme, "rc =%d\n", ret);
	if (ret) {
		SPDK_ERRLOG("Unable to create rqpair RDMA requests\n");
		return -1;
	}
	SPDK_DEBUGLOG(nvme, "RDMA requests created\n");

	if (!rqpair->srq) {
		opts.num_entries = rqpair->num_entries;
		opts.rqpair = rqpair;
		opts.srq = NULL;
		opts.mr_map = rqpair->mr_map;

		assert(!rqpair->rsps);
		rqpair->rsps = nvme_rdma_create_rsps(&opts);
		if (!rqpair->rsps) {
			SPDK_ERRLOG("Unable to create rqpair RDMA responses\n");
			return -1;
		}
		SPDK_DEBUGLOG(nvme, "RDMA responses created\n");

		ret = nvme_rdma_qpair_submit_recvs(rqpair);
		SPDK_DEBUGLOG(nvme, "rc =%d\n", ret);
		if (ret) {
			SPDK_ERRLOG("Unable to submit rqpair RDMA responses\n");
			return -1;
		}
		SPDK_DEBUGLOG(nvme, "RDMA responses submitted\n");
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND;

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

	ret = ibv_query_device(rqpair->cm_id->verbs, &attr);
	if (ret != 0) {
		SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
		return ret;
	}

	param.responder_resources = attr.max_qp_rd_atom;

	ctrlr = rqpair->qpair.ctrlr;
	if (!ctrlr) {
		return -1;
	}

	request_data.qid = rqpair->qpair.id;
	request_data.hrqsize = rqpair->num_entries + 1;
	request_data.hsqsize = rqpair->num_entries;
	request_data.cntlid = ctrlr->cntlid;

	param.private_data = &request_data;
	param.private_data_len = sizeof(request_data);
	param.retry_count = ctrlr->opts.transport_retry_count;
	param.rnr_retry_count = 7;

	/* Fields below are ignored by rdma cm if qpair has been
	 * created using rdma cm API. */
	param.srq = 0;
	param.qp_num = rqpair->rdma_qp->qp->qp_num;

	ret = rdma_connect(rqpair->cm_id, &param);
	if (ret) {
		SPDK_ERRLOG("nvme rdma connect error\n");
		return ret;
	}

	return nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_ESTABLISHED,
					     nvme_rdma_connect_established);
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
		return -(abs(ret));
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n", (size_t)res->ai_addrlen);
		ret = -EINVAL;
	} else {
		memcpy(sa, res->ai_addr, res->ai_addrlen);
	}

	freeaddrinfo(res);
	return ret;
}

static int
nvme_rdma_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
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

	SPDK_DEBUGLOG(nvme, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	SPDK_DEBUGLOG(nvme, "trsvcid is %s\n", ctrlr->trid.trsvcid);
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
				    (struct sockaddr *)&dst_addr);
	if (rc < 0) {
		SPDK_ERRLOG("nvme_rdma_resolve_addr() failed\n");
		return -1;
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_INITIALIZING;

	return 0;
}

static int
nvme_rdma_stale_conn_reconnect(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (spdk_get_ticks() < rqpair->evt_timeout_ticks) {
		return -EAGAIN;
	}

	return nvme_rdma_ctrlr_connect_qpair(qpair->ctrlr, qpair);
}

static int
nvme_rdma_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	if (rqpair->in_connect_poll) {
		return -EAGAIN;
	}

	rqpair->in_connect_poll = true;

	switch (rqpair->state) {
	case NVME_RDMA_QPAIR_STATE_INVALID:
		rc = -EAGAIN;
		break;

	case NVME_RDMA_QPAIR_STATE_INITIALIZING:
	case NVME_RDMA_QPAIR_STATE_EXITING:
		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		}

		rc = nvme_rdma_process_event_poll(rqpair);

		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		}

		if (rc == 0) {
			rc = -EAGAIN;
		}
		rqpair->in_connect_poll = false;

		return rc;

	case NVME_RDMA_QPAIR_STATE_STALE_CONN:
		rc = nvme_rdma_stale_conn_reconnect(rqpair);
		if (rc == 0) {
			rc = -EAGAIN;
		}
		break;
	case NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_SEND:
		rc = nvme_fabric_qpair_connect_async(qpair, rqpair->num_entries + 1);
		if (rc == 0) {
			rqpair->state = NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL;
			rc = -EAGAIN;
		} else {
			SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_RDMA_QPAIR_STATE_FABRIC_CONNECT_POLL:
		rc = nvme_fabric_qpair_connect_poll(qpair);
		if (rc == 0) {
			rqpair->state = NVME_RDMA_QPAIR_STATE_RUNNING;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		} else if (rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to poll NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_RDMA_QPAIR_STATE_RUNNING:
		rc = 0;
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	rqpair->in_connect_poll = false;

	return rc;
}

static inline int
nvme_rdma_get_memory_translation(struct nvme_request *req, struct nvme_rdma_qpair *rqpair,
				 struct nvme_rdma_memory_translation_ctx *_ctx)
{
	struct spdk_memory_domain_translation_ctx ctx;
	struct spdk_memory_domain_translation_result dma_translation = {.iov_count = 0};
	struct spdk_rdma_memory_translation rdma_translation;
	int rc;

	assert(req);
	assert(rqpair);
	assert(_ctx);

	if (req->payload.opts && req->payload.opts->memory_domain) {
		ctx.size = sizeof(struct spdk_memory_domain_translation_ctx);
		ctx.rdma.ibv_qp = rqpair->rdma_qp->qp;
		dma_translation.size = sizeof(struct spdk_memory_domain_translation_result);

		rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
						       req->payload.opts->memory_domain_ctx,
						       rqpair->memory_domain->domain, &ctx, _ctx->addr,
						       _ctx->length, &dma_translation);
		if (spdk_unlikely(rc) || dma_translation.iov_count != 1) {
			SPDK_ERRLOG("DMA memory translation failed, rc %d, iov count %u\n", rc, dma_translation.iov_count);
			return rc;
		}

		_ctx->lkey = dma_translation.rdma.lkey;
		_ctx->rkey = dma_translation.rdma.rkey;
		_ctx->addr = dma_translation.iov.iov_base;
		_ctx->length = dma_translation.iov.iov_len;
	} else {
		rc = spdk_rdma_get_translation(rqpair->mr_map, _ctx->addr, _ctx->length, &rdma_translation);
		if (spdk_unlikely(rc)) {
			SPDK_ERRLOG("RDMA memory translation failed, rc %d\n", rc);
			return rc;
		}
		if (rdma_translation.translation_type == SPDK_RDMA_TRANSLATION_MR) {
			_ctx->lkey = rdma_translation.mr_or_key.mr->lkey;
			_ctx->rkey = rdma_translation.mr_or_key.mr->rkey;
		} else {
			_ctx->lkey = _ctx->rkey = (uint32_t)rdma_translation.mr_or_key.key;
		}
	}

	return 0;
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

/*
 * Build inline SGL describing contiguous payload buffer.
 */
static int
nvme_rdma_build_contig_inline_request(struct nvme_rdma_qpair *rqpair,
				      struct spdk_nvme_rdma_req *rdma_req)
{
	struct nvme_request *req = rdma_req->req;
	struct nvme_rdma_memory_translation_ctx ctx = {
		.addr = req->payload.contig_or_cb_arg + req->payload_offset,
		.length = req->payload_size
	};
	int rc;

	assert(ctx.length != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
	if (spdk_unlikely(rc)) {
		return -1;
	}

	rdma_req->send_sgl[1].lkey = ctx.lkey;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	rdma_req->send_sgl[1].addr = (uint64_t)ctx.addr;
	rdma_req->send_sgl[1].length = (uint32_t)ctx.length;

	/* The RDMA SGL contains two elements. The first describes
	 * the NVMe command and the second describes the data
	 * payload. */
	rdma_req->send_wr.num_sge = 2;

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)ctx.length;
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
	struct nvme_rdma_memory_translation_ctx ctx = {
		.addr = req->payload.contig_or_cb_arg + req->payload_offset,
		.length = req->payload_size
	};
	int rc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	if (spdk_unlikely(req->payload_size > NVME_RDMA_MAX_KEYED_SGL_LENGTH)) {
		SPDK_ERRLOG("SGL length %u exceeds max keyed SGL block size %u\n",
			    req->payload_size, NVME_RDMA_MAX_KEYED_SGL_LENGTH);
		return -1;
	}

	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
	if (spdk_unlikely(rc)) {
		return -1;
	}

	req->cmd.dptr.sgl1.keyed.key = ctx.rkey;

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
	req->cmd.dptr.sgl1.keyed.length = (uint32_t)ctx.length;
	req->cmd.dptr.sgl1.address = (uint64_t)ctx.addr;

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
	struct nvme_rdma_memory_translation_ctx ctx;
	uint32_t remaining_size;
	uint32_t sge_length;
	int rc, max_num_sgl, num_sgl_desc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	max_num_sgl = req->qpair->ctrlr->max_sges;

	remaining_size = req->payload_size;
	num_sgl_desc = 0;
	do {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &ctx.addr, &sge_length);
		if (rc) {
			return -1;
		}

		sge_length = spdk_min(remaining_size, sge_length);

		if (spdk_unlikely(sge_length > NVME_RDMA_MAX_KEYED_SGL_LENGTH)) {
			SPDK_ERRLOG("SGL length %u exceeds max keyed SGL block size %u\n",
				    sge_length, NVME_RDMA_MAX_KEYED_SGL_LENGTH);
			return -1;
		}
		ctx.length = sge_length;
		rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
		if (spdk_unlikely(rc)) {
			return -1;
		}

		cmd->sgl[num_sgl_desc].keyed.key = ctx.rkey;
		cmd->sgl[num_sgl_desc].keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
		cmd->sgl[num_sgl_desc].keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
		cmd->sgl[num_sgl_desc].keyed.length = (uint32_t)ctx.length;
		cmd->sgl[num_sgl_desc].address = (uint64_t)ctx.addr;

		remaining_size -= ctx.length;
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
		uint32_t descriptors_size = sizeof(struct spdk_nvme_sgl_descriptor) * num_sgl_desc;

		if (spdk_unlikely(descriptors_size > rqpair->qpair.ctrlr->ioccsz_bytes)) {
			SPDK_ERRLOG("Size of SGL descriptors (%u) exceeds ICD (%u)\n",
				    descriptors_size, rqpair->qpair.ctrlr->ioccsz_bytes);
			return -1;
		}
		rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd) + descriptors_size;

		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
		req->cmd.dptr.sgl1.unkeyed.length = descriptors_size;
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
	struct nvme_rdma_memory_translation_ctx ctx;
	uint32_t length;
	int rc;

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &ctx.addr, &length);
	if (rc) {
		return -1;
	}

	if (length < req->payload_size) {
		SPDK_DEBUGLOG(nvme, "Inline SGL request split so sending separately.\n");
		return nvme_rdma_build_sgl_request(rqpair, rdma_req);
	}

	if (length > req->payload_size) {
		length = req->payload_size;
	}

	ctx.length = length;
	rc = nvme_rdma_get_memory_translation(req, rqpair, &ctx);
	if (spdk_unlikely(rc)) {
		return -1;
	}

	rdma_req->send_sgl[1].addr = (uint64_t)ctx.addr;
	rdma_req->send_sgl[1].length = (uint32_t)ctx.length;
	rdma_req->send_sgl[1].lkey = ctx.lkey;

	rdma_req->send_wr.num_sge = 2;

	/* The first element of this SGL is pointing at an
	 * spdk_nvmf_cmd object. For this particular command,
	 * we only need the first 64 bytes corresponding to
	 * the NVMe command. */
	rdma_req->send_sgl[0].length = sizeof(struct spdk_nvme_cmd);

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
	req->cmd.dptr.sgl1.unkeyed.length = (uint32_t)ctx.length;
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
		rdma_req->req = NULL;
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
			     bool delay_cmd_submit,
			     bool async)
{
	struct nvme_rdma_qpair *rqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {
		SPDK_ERRLOG("Failed to create qpair with size %u. Minimum queue size is %d.\n",
			    qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}

	rqpair = spdk_zmalloc(sizeof(struct nvme_rdma_qpair), 0, NULL, SPDK_ENV_SOCKET_ID_ANY,
			      SPDK_MALLOC_DMA);
	if (!rqpair) {
		SPDK_ERRLOG("failed to get create rqpair\n");
		return NULL;
	}

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	rqpair->num_entries = qsize - 1;
	rqpair->delay_cmd_submit = delay_cmd_submit;
	rqpair->state = NVME_RDMA_QPAIR_STATE_INVALID;
	qpair = &rqpair->qpair;
	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests, async);
	if (rc != 0) {
		spdk_free(rqpair);
		return NULL;
	}

	return qpair;
}

static void
nvme_rdma_qpair_destroy(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;
	struct nvme_rdma_ctrlr *rctrlr;
	struct nvme_rdma_cm_event_entry *entry, *tmp;

	spdk_rdma_free_mem_map(&rqpair->mr_map);

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
			if (entry->evt->id->context == rqpair) {
				STAILQ_REMOVE(&rctrlr->pending_cm_events, entry, nvme_rdma_cm_event_entry, link);
				rdma_ack_cm_event(entry->evt);
				STAILQ_INSERT_HEAD(&rctrlr->free_cm_events, entry, link);
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp) {
			spdk_rdma_put_pd(rqpair->rdma_qp->qp->pd);
			spdk_rdma_qp_destroy(rqpair->rdma_qp);
			rqpair->rdma_qp = NULL;
		}

		rdma_destroy_id(rqpair->cm_id);
		rqpair->cm_id = NULL;
	}

	if (rqpair->poller) {
		struct nvme_rdma_poll_group     *group;

		assert(qpair->poll_group);
		group = nvme_rdma_poll_group(qpair->poll_group);

		nvme_rdma_poll_group_put_poller(group, rqpair->poller);

		rqpair->poller = NULL;
		rqpair->cq = NULL;
		if (rqpair->srq) {
			rqpair->srq = NULL;
			rqpair->rsps = NULL;
		}
	} else if (rqpair->cq) {
		ibv_destroy_cq(rqpair->cq);
		rqpair->cq = NULL;
	}

	nvme_rdma_free_reqs(rqpair);
	nvme_rdma_free_rsps(rqpair->rsps);
	rqpair->rsps = NULL;
}

static void nvme_rdma_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);

static int
nvme_rdma_qpair_disconnected(struct nvme_rdma_qpair *rqpair, int ret)
{
	nvme_rdma_qpair_abort_reqs(&rqpair->qpair, 0);

	if (ret) {
		SPDK_DEBUGLOG(nvme, "Target did not respond to qpair disconnect.\n");
		goto quiet;
	}

	if (rqpair->poller == NULL) {
		/* If poller is not used, cq is not shared or already destroyed.
		 * So complete disconnecting qpair immediately.
		 */
		goto quiet;
	}

	if (rqpair->rsps == NULL) {
		goto quiet;
	}

	if (rqpair->current_num_sends != 0 ||
	    (!rqpair->srq && rqpair->rsps->current_num_recvs != 0)) {
		rqpair->state = NVME_RDMA_QPAIR_STATE_LINGERING;
		rqpair->evt_timeout_ticks = (NVME_RDMA_DISCONNECTED_QPAIR_TIMEOUT_US * spdk_get_ticks_hz()) /
					    SPDK_SEC_TO_USEC + spdk_get_ticks();

		return -EAGAIN;
	}

quiet:
	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITED;

	nvme_rdma_qpair_destroy(rqpair);
	nvme_transport_ctrlr_disconnect_qpair_done(&rqpair->qpair);

	return 0;
}

static int
nvme_rdma_qpair_wait_until_quiet(struct nvme_rdma_qpair *rqpair)
{
	if (spdk_get_ticks() < rqpair->evt_timeout_ticks &&
	    (rqpair->current_num_sends != 0 ||
	     (!rqpair->srq && rqpair->rsps->current_num_recvs != 0))) {
		return -EAGAIN;
	}

	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITED;

	nvme_rdma_qpair_destroy(rqpair);
	nvme_transport_ctrlr_disconnect_qpair_done(&rqpair->qpair);

	return 0;
}

static void
_nvme_rdma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				  nvme_rdma_cm_event_cb disconnected_qpair_cb)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	assert(disconnected_qpair_cb != NULL);

	rqpair->state = NVME_RDMA_QPAIR_STATE_EXITING;

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp) {
			rc = spdk_rdma_qp_disconnect(rqpair->rdma_qp);
			if ((qpair->ctrlr != NULL) && (rc == 0)) {
				rc = nvme_rdma_process_event_start(rqpair, RDMA_CM_EVENT_DISCONNECTED,
								   disconnected_qpair_cb);
				if (rc == 0) {
					return;
				}
			}
		}
	}

	disconnected_qpair_cb(rqpair, 0);
}

static int
nvme_rdma_ctrlr_disconnect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	int rc;

	switch (rqpair->state) {
	case NVME_RDMA_QPAIR_STATE_EXITING:
		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
		}

		rc = nvme_rdma_process_event_poll(rqpair);

		if (!nvme_qpair_is_admin_queue(qpair)) {
			nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		}
		break;

	case NVME_RDMA_QPAIR_STATE_LINGERING:
		rc = nvme_rdma_qpair_wait_until_quiet(rqpair);
		break;
	case NVME_RDMA_QPAIR_STATE_EXITED:
		rc = 0;
		break;

	default:
		assert(false);
		rc = -EAGAIN;
		break;
	}

	return rc;
}

static void
nvme_rdma_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc;

	_nvme_rdma_ctrlr_disconnect_qpair(ctrlr, qpair, nvme_rdma_qpair_disconnected);

	/* If the async mode is disabled, poll the qpair until it is actually disconnected.
	 * It is ensured that poll_group_process_completions() calls disconnected_qpair_cb
	 * for any disconnected qpair. Hence, we do not have to check if the qpair is in
	 * a poll group or not.
	 */
	if (qpair->async) {
		return;
	}

	while (1) {
		rc = nvme_rdma_ctrlr_disconnect_qpair_poll(ctrlr, qpair);
		if (rc != -EAGAIN) {
			break;
		}
	}
}

static int
nvme_rdma_stale_conn_disconnected(struct nvme_rdma_qpair *rqpair, int ret)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (ret) {
		SPDK_DEBUGLOG(nvme, "Target did not respond to qpair disconnect.\n");
	}

	nvme_rdma_qpair_destroy(rqpair);

	qpair->last_transport_failure_reason = qpair->transport_failure_reason;
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_NONE;

	rqpair->state = NVME_RDMA_QPAIR_STATE_STALE_CONN;
	rqpair->evt_timeout_ticks = (NVME_RDMA_STALE_CONN_RETRY_DELAY_US * spdk_get_ticks_hz()) /
				    SPDK_SEC_TO_USEC + spdk_get_ticks();

	return 0;
}

static int
nvme_rdma_stale_conn_retry(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_qpair *qpair = &rqpair->qpair;

	if (rqpair->stale_conn_retry_count >= NVME_RDMA_STALE_CONN_RETRY_MAX) {
		SPDK_ERRLOG("Retry failed %d times, give up stale connection to qpair (cntlid:%u, qid:%u).\n",
			    NVME_RDMA_STALE_CONN_RETRY_MAX, qpair->ctrlr->cntlid, qpair->id);
		return -ESTALE;
	}

	rqpair->stale_conn_retry_count++;

	SPDK_NOTICELOG("%d times, retry stale connection to qpair (cntlid:%u, qid:%u).\n",
		       rqpair->stale_conn_retry_count, qpair->ctrlr->cntlid, qpair->id);

	_nvme_rdma_ctrlr_disconnect_qpair(qpair->ctrlr, qpair, nvme_rdma_stale_conn_disconnected);

	return 0;
}

static int
nvme_rdma_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair;

	assert(qpair != NULL);
	rqpair = nvme_rdma_qpair(qpair);

	if (rqpair->state != NVME_RDMA_QPAIR_STATE_EXITED) {
		int rc __attribute__((unused));

		/* qpair was removed from the poll group while the disconnect is not finished.
		 * Destroy rdma resources forcefully. */
		rc = nvme_rdma_qpair_disconnected(rqpair, 0);
		assert(rc == 0);
	}

	nvme_rdma_qpair_abort_reqs(qpair, 0);
	nvme_qpair_deinit(qpair);

	nvme_rdma_put_memory_domain(rqpair->memory_domain);

	spdk_free(rqpair);

	return 0;
}

static struct spdk_nvme_qpair *
nvme_rdma_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_rdma_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					    opts->io_queue_requests,
					    opts->delay_cmd_submit,
					    opts->async_mode);
}

static int
nvme_rdma_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	/* do nothing here */
	return 0;
}

static int nvme_rdma_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr);

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

static spdk_nvme_ctrlr_t *
nvme_rdma_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts,
			  void *devhandle)
{
	struct nvme_rdma_ctrlr *rctrlr;
	struct ibv_context **contexts;
	struct ibv_device_attr dev_attr;
	int i, flag, rc;

	rctrlr = spdk_zmalloc(sizeof(struct nvme_rdma_ctrlr), 0, NULL, SPDK_ENV_SOCKET_ID_ANY,
			      SPDK_MALLOC_DMA);
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
		spdk_free(rctrlr);
		return NULL;
	}

	i = 0;
	rctrlr->max_sge = NVME_RDMA_MAX_SGL_DESCRIPTORS;

	while (contexts[i] != NULL) {
		rc = ibv_query_device(contexts[i], &dev_attr);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to query RDMA device attributes.\n");
			rdma_free_devices(contexts);
			spdk_free(rctrlr);
			return NULL;
		}
		rctrlr->max_sge = spdk_min(rctrlr->max_sge, (uint16_t)dev_attr.max_sge);
		i++;
	}

	rdma_free_devices(contexts);

	rc = nvme_ctrlr_construct(&rctrlr->ctrlr);
	if (rc != 0) {
		spdk_free(rctrlr);
		return NULL;
	}

	STAILQ_INIT(&rctrlr->pending_cm_events);
	STAILQ_INIT(&rctrlr->free_cm_events);
	rctrlr->cm_events = spdk_zmalloc(NVME_RDMA_NUM_CM_EVENTS * sizeof(*rctrlr->cm_events), 0, NULL,
					 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (rctrlr->cm_events == NULL) {
		SPDK_ERRLOG("unable to allocate buffers to hold CM events.\n");
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
			       rctrlr->ctrlr.opts.admin_queue_size, false, true);
	if (!rctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		goto destruct_ctrlr;
	}

	if (nvme_ctrlr_add_process(&rctrlr->ctrlr, 0) != 0) {
		SPDK_ERRLOG("nvme_ctrlr_add_process() failed\n");
		goto destruct_ctrlr;
	}

	SPDK_DEBUGLOG(nvme, "successfully initialized the nvmf ctrlr\n");
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
	spdk_free(rctrlr->cm_events);

	if (rctrlr->cm_channel) {
		rdma_destroy_event_channel(rctrlr->cm_channel);
		rctrlr->cm_channel = NULL;
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	spdk_free(rctrlr);

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
	if (spdk_unlikely(!rdma_req)) {
		if (rqpair->poller) {
			rqpair->poller->stats.queued_requests++;
		}
		/* Inform the upper layer to try again later. */
		return -EAGAIN;
	}

	if (nvme_rdma_req_init(rqpair, req, rdma_req)) {
		SPDK_ERRLOG("nvme_rdma_req_init() failed\n");
		TAILQ_REMOVE(&rqpair->outstanding_reqs, rdma_req, link);
		nvme_rdma_req_put(rqpair, rdma_req);
		return -1;
	}

	assert(rqpair->current_num_sends < rqpair->num_entries);
	rqpair->current_num_sends++;

	wr = &rdma_req->send_wr;
	wr->next = NULL;
	nvme_rdma_trace_ibv_sge(wr->sg_list);

	spdk_rdma_qp_queue_send_wrs(rqpair->rdma_qp, wr);

	if (!rqpair->delay_cmd_submit) {
		return nvme_rdma_qpair_submit_sends(rqpair);
	}

	return 0;
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
	struct spdk_nvme_cpl cpl;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);

	cpl.sqid = qpair->id;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	/*
	 * We cannot abort requests at the RDMA layer without
	 * unregistering them. If we do, we can still get error
	 * free completions on the shared completion queue.
	 */
	if (nvme_qpair_get_state(qpair) > NVME_QPAIR_DISCONNECTING &&
	    nvme_qpair_get_state(qpair) != NVME_QPAIR_DESTROYING) {
		nvme_ctrlr_disconnect_qpair(qpair);
	}

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		nvme_rdma_req_complete(rdma_req, &cpl, true);
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
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
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

static inline void
nvme_rdma_request_ready(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	struct spdk_nvme_rdma_rsp *rdma_rsp = rdma_req->rdma_rsp;
	struct ibv_recv_wr *recv_wr = rdma_rsp->recv_wr;

	nvme_rdma_req_complete(rdma_req, &rdma_rsp->cpl, true);

	assert(rqpair->rsps->current_num_recvs < rqpair->rsps->num_entries);
	rqpair->rsps->current_num_recvs++;

	recv_wr->next = NULL;
	nvme_rdma_trace_ibv_sge(recv_wr->sg_list);

	if (!rqpair->srq) {
		spdk_rdma_qp_queue_recv_wrs(rqpair->rdma_qp, recv_wr);
	} else {
		spdk_rdma_srq_queue_recv_wrs(rqpair->srq, recv_wr);
	}
}

#define MAX_COMPLETIONS_PER_POLL 128

static void
nvme_rdma_fail_qpair(struct spdk_nvme_qpair *qpair, int failure_reason)
{
	if (failure_reason == IBV_WC_RETRY_EXC_ERR) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_REMOTE;
	} else if (qpair->transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_NONE) {
		qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
	}

	nvme_ctrlr_disconnect_qpair(qpair);
}

static struct nvme_rdma_qpair *
get_rdma_qpair_from_wc(struct nvme_rdma_poll_group *group, struct ibv_wc *wc)
{
	struct spdk_nvme_qpair *qpair;
	struct nvme_rdma_qpair *rqpair;

	STAILQ_FOREACH(qpair, &group->group.connected_qpairs, poll_group_stailq) {
		rqpair = nvme_rdma_qpair(qpair);
		if (NVME_RDMA_POLL_GROUP_CHECK_QPN(rqpair, wc->qp_num)) {
			return rqpair;
		}
	}

	STAILQ_FOREACH(qpair, &group->group.disconnected_qpairs, poll_group_stailq) {
		rqpair = nvme_rdma_qpair(qpair);
		if (NVME_RDMA_POLL_GROUP_CHECK_QPN(rqpair, wc->qp_num)) {
			return rqpair;
		}
	}

	return NULL;
}

static inline void
nvme_rdma_log_wc_status(struct nvme_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	struct nvme_rdma_wr *rdma_wr = (struct nvme_rdma_wr *)wc->wr_id;

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		/* If qpair is in ERR state, we will receive completions for all posted and not completed
		 * Work Requests with IBV_WC_WR_FLUSH_ERR status. Don't log an error in that case */
		SPDK_DEBUGLOG(nvme, "WC error, qid %u, qp state %d, request 0x%lu type %d, status: (%d): %s\n",
			      rqpair->qpair.id, rqpair->qpair.state, wc->wr_id, rdma_wr->type, wc->status,
			      ibv_wc_status_str(wc->status));
	} else {
		SPDK_ERRLOG("WC error, qid %u, qp state %d, request 0x%lu type %d, status: (%d): %s\n",
			    rqpair->qpair.id, rqpair->qpair.state, wc->wr_id, rdma_wr->type, wc->status,
			    ibv_wc_status_str(wc->status));
	}
}

static inline int
nvme_rdma_process_recv_completion(struct nvme_rdma_poller *poller, struct ibv_wc *wc,
				  struct nvme_rdma_wr *rdma_wr)
{
	struct nvme_rdma_qpair		*rqpair;
	struct spdk_nvme_rdma_req	*rdma_req;
	struct spdk_nvme_rdma_rsp	*rdma_rsp;

	rdma_rsp = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvme_rdma_rsp, rdma_wr);

	if (poller && poller->srq) {
		rqpair = get_rdma_qpair_from_wc(poller->group, wc);
		if (spdk_unlikely(!rqpair)) {
			/* Since we do not handle the LAST_WQE_REACHED event, we do not know when
			 * a Receive Queue in a QP, that is associated with an SRQ, is flushed.
			 * We may get a WC for a already destroyed QP.
			 *
			 * However, for the SRQ, this is not any error. Hence, just re-post the
			 * receive request to the SRQ to reuse for other QPs, and return 0.
			 */
			spdk_rdma_srq_queue_recv_wrs(poller->srq, rdma_rsp->recv_wr);
			return 0;
		}
	} else {
		rqpair = rdma_rsp->rqpair;
		if (spdk_unlikely(!rqpair)) {
			/* TODO: Fix forceful QP destroy when it is not async mode.
			 * CQ itself did not cause any error. Hence, return 0 for now.
			 */
			SPDK_WARNLOG("QP might be already destroyed.\n");
			return 0;
		}
	}


	assert(rqpair->rsps->current_num_recvs > 0);
	rqpair->rsps->current_num_recvs--;

	if (wc->status) {
		nvme_rdma_log_wc_status(rqpair, wc);
		goto err_wc;
	}

	SPDK_DEBUGLOG(nvme, "CQ recv completion\n");

	if (wc->byte_len < sizeof(struct spdk_nvme_cpl)) {
		SPDK_ERRLOG("recv length %u less than expected response size\n", wc->byte_len);
		goto err_wc;
	}
	rdma_req = &rqpair->rdma_reqs[rdma_rsp->cpl.cid];
	rdma_req->completion_flags |= NVME_RDMA_RECV_COMPLETED;
	rdma_req->rdma_rsp = rdma_rsp;

	if ((rdma_req->completion_flags & NVME_RDMA_SEND_COMPLETED) == 0) {
		return 0;
	}

	nvme_rdma_request_ready(rqpair, rdma_req);

	if (!rqpair->delay_cmd_submit) {
		if (spdk_unlikely(nvme_rdma_qpair_submit_recvs(rqpair))) {
			SPDK_ERRLOG("Unable to re-post rx descriptor\n");
			nvme_rdma_fail_qpair(&rqpair->qpair, 0);
			return -ENXIO;
		}
	}

	rqpair->num_completions++;
	return 1;

err_wc:
	nvme_rdma_fail_qpair(&rqpair->qpair, 0);
	if (poller && poller->srq) {
		spdk_rdma_srq_queue_recv_wrs(poller->srq, rdma_rsp->recv_wr);
	}
	return -ENXIO;
}

static inline int
nvme_rdma_process_send_completion(struct nvme_rdma_poller *poller,
				  struct nvme_rdma_qpair *rdma_qpair,
				  struct ibv_wc *wc, struct nvme_rdma_wr *rdma_wr)
{
	struct nvme_rdma_qpair		*rqpair;
	struct spdk_nvme_rdma_req	*rdma_req;

	rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvme_rdma_req, rdma_wr);

	/* If we are flushing I/O */
	if (wc->status) {
		rqpair = rdma_req->req ? nvme_rdma_qpair(rdma_req->req->qpair) : NULL;
		if (!rqpair) {
			rqpair = rdma_qpair != NULL ? rdma_qpair : get_rdma_qpair_from_wc(poller->group, wc);
		}
		if (!rqpair) {
			/* When poll_group is used, several qpairs share the same CQ and it is possible to
			 * receive a completion with error (e.g. IBV_WC_WR_FLUSH_ERR) for already disconnected qpair
			 * That happens due to qpair is destroyed while there are submitted but not completed send/receive
			 * Work Requests */
			assert(poller);
			return 0;
		}
		assert(rqpair->current_num_sends > 0);
		rqpair->current_num_sends--;
		nvme_rdma_log_wc_status(rqpair, wc);
		nvme_rdma_fail_qpair(&rqpair->qpair, 0);
		if (rdma_req->rdma_rsp && poller && poller->srq) {
			spdk_rdma_srq_queue_recv_wrs(poller->srq, rdma_req->rdma_rsp->recv_wr);
		}
		return -ENXIO;
	}

	/* We do not support Soft Roce anymore. Other than Soft Roce's bug, we should not
	 * receive a completion without error status after qpair is disconnected/destroyed.
	 */
	assert(rdma_req->req != NULL);

	rqpair = nvme_rdma_qpair(rdma_req->req->qpair);
	rdma_req->completion_flags |= NVME_RDMA_SEND_COMPLETED;
	assert(rqpair->current_num_sends > 0);
	rqpair->current_num_sends--;

	if ((rdma_req->completion_flags & NVME_RDMA_RECV_COMPLETED) == 0) {
		return 0;
	}

	nvme_rdma_request_ready(rqpair, rdma_req);

	if (!rqpair->delay_cmd_submit) {
		if (spdk_unlikely(nvme_rdma_qpair_submit_recvs(rqpair))) {
			SPDK_ERRLOG("Unable to re-post rx descriptor\n");
			nvme_rdma_fail_qpair(&rqpair->qpair, 0);
			return -ENXIO;
		}
	}

	rqpair->num_completions++;
	return 1;
}

static int
nvme_rdma_cq_process_completions(struct ibv_cq *cq, uint32_t batch_size,
				 struct nvme_rdma_poller *poller,
				 struct nvme_rdma_qpair *rdma_qpair,
				 uint64_t *rdma_completions)
{
	struct ibv_wc			wc[MAX_COMPLETIONS_PER_POLL];
	struct nvme_rdma_wr		*rdma_wr;
	uint32_t			reaped = 0;
	int				completion_rc = 0;
	int				rc, _rc, i;

	rc = ibv_poll_cq(cq, batch_size, wc);
	if (rc < 0) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, spdk_strerror(errno));
		return -ECANCELED;
	} else if (rc == 0) {
		return 0;
	}

	for (i = 0; i < rc; i++) {
		rdma_wr = (struct nvme_rdma_wr *)wc[i].wr_id;
		switch (rdma_wr->type) {
		case RDMA_WR_TYPE_RECV:
			_rc = nvme_rdma_process_recv_completion(poller, &wc[i], rdma_wr);
			break;

		case RDMA_WR_TYPE_SEND:
			_rc = nvme_rdma_process_send_completion(poller, rdma_qpair, &wc[i], rdma_wr);
			break;

		default:
			SPDK_ERRLOG("Received an unexpected opcode on the CQ: %d\n", rdma_wr->type);
			return -ECANCELED;
		}
		if (spdk_likely(_rc >= 0)) {
			reaped += _rc;
		} else {
			completion_rc = _rc;
		}
	}

	*rdma_completions += rc;

	if (completion_rc) {
		return completion_rc;
	}

	return reaped;
}

static void
dummy_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{

}

static int
nvme_rdma_qpair_process_completions(struct spdk_nvme_qpair *qpair,
				    uint32_t max_completions)
{
	struct nvme_rdma_qpair		*rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_ctrlr		*rctrlr = nvme_rdma_ctrlr(qpair->ctrlr);
	int				rc = 0, batch_size;
	struct ibv_cq			*cq;
	uint64_t			rdma_completions = 0;

	/*
	 * This is used during the connection phase. It's possible that we are still reaping error completions
	 * from other qpairs so we need to call the poll group function. Also, it's more correct since the cq
	 * is shared.
	 */
	if (qpair->poll_group != NULL) {
		return spdk_nvme_poll_group_process_completions(qpair->poll_group->group, max_completions,
				dummy_disconnected_qpair_cb);
	}

	if (max_completions == 0) {
		max_completions = rqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, rqpair->num_entries);
	}

	switch (nvme_qpair_get_state(qpair)) {
	case NVME_QPAIR_CONNECTING:
		rc = nvme_rdma_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc == 0) {
			/* Once the connection is completed, we can submit queued requests */
			nvme_qpair_resubmit_requests(qpair, rqpair->num_entries);
		} else if (rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to connect rqpair=%p\n", rqpair);
			goto failed;
		} else if (rqpair->state <= NVME_RDMA_QPAIR_STATE_INITIALIZING) {
			return 0;
		}
		break;

	case NVME_QPAIR_DISCONNECTING:
		nvme_rdma_ctrlr_disconnect_qpair_poll(qpair->ctrlr, qpair);
		return -ENXIO;

	default:
		if (nvme_qpair_is_admin_queue(qpair)) {
			nvme_rdma_poll_events(rctrlr);
		}
		nvme_rdma_qpair_process_cm_event(rqpair);
		break;
	}

	if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE)) {
		goto failed;
	}

	cq = rqpair->cq;

	rqpair->num_completions = 0;
	do {
		batch_size = spdk_min((max_completions - rqpair->num_completions), MAX_COMPLETIONS_PER_POLL);
		rc = nvme_rdma_cq_process_completions(cq, batch_size, NULL, rqpair, &rdma_completions);

		if (rc == 0) {
			break;
			/* Handle the case where we fail to poll the cq. */
		} else if (rc == -ECANCELED) {
			goto failed;
		} else if (rc == -ENXIO) {
			return rc;
		}
	} while (rqpair->num_completions < max_completions);

	if (spdk_unlikely(nvme_rdma_qpair_submit_sends(rqpair) ||
			  nvme_rdma_qpair_submit_recvs(rqpair))) {
		goto failed;
	}

	if (spdk_unlikely(qpair->ctrlr->timeout_enabled)) {
		nvme_rdma_qpair_check_timeout(qpair);
	}

	return rqpair->num_completions;

failed:
	nvme_rdma_fail_qpair(qpair, 0);
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
	uint32_t max_sge = rctrlr->max_sge;
	uint32_t max_in_capsule_sge = (ctrlr->cdata.nvmf_specific.ioccsz * 16 -
				       sizeof(struct spdk_nvme_cmd)) /
				      sizeof(struct spdk_nvme_sgl_descriptor);

	/* Max SGE is limited by capsule size */
	max_sge = spdk_min(max_sge, max_in_capsule_sge);
	/* Max SGE may be limited by MSDBD */
	if (ctrlr->cdata.nvmf_specific.msdbd != 0) {
		max_sge = spdk_min(max_sge, ctrlr->cdata.nvmf_specific.msdbd);
	}

	/* Max SGE can't be less than 1 */
	max_sge = spdk_max(1, max_sge);
	return max_sge;
}

static int
nvme_rdma_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				 int (*iter_fn)(struct nvme_request *req, void *arg),
				 void *arg)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);

		rc = iter_fn(rdma_req->req, arg);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static void
nvme_rdma_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_rdma_req *rdma_req, *tmp;
	struct spdk_nvme_cpl cpl;
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->outstanding_reqs, link, tmp) {
		assert(rdma_req->req != NULL);

		if (rdma_req->req->cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;
		}

		nvme_rdma_req_complete(rdma_req, &cpl, false);
	}
}

static void
nvme_rdma_poller_destroy(struct nvme_rdma_poller *poller)
{
	if (poller->cq) {
		ibv_destroy_cq(poller->cq);
	}
	if (poller->rsps) {
		nvme_rdma_free_rsps(poller->rsps);
	}
	if (poller->srq) {
		spdk_rdma_srq_destroy(poller->srq);
	}
	if (poller->mr_map) {
		spdk_rdma_free_mem_map(&poller->mr_map);
	}
	if (poller->pd) {
		spdk_rdma_put_pd(poller->pd);
	}
	free(poller);
}

static struct nvme_rdma_poller *
nvme_rdma_poller_create(struct nvme_rdma_poll_group *group, struct ibv_context *ctx)
{
	struct nvme_rdma_poller *poller;
	struct ibv_device_attr dev_attr;
	struct spdk_rdma_srq_init_attr srq_init_attr = {};
	struct nvme_rdma_rsp_opts opts;
	int num_cqe;
	int rc;

	poller = calloc(1, sizeof(*poller));
	if (poller == NULL) {
		SPDK_ERRLOG("Unable to allocate poller.\n");
		return NULL;
	}

	poller->group = group;
	poller->device = ctx;

	if (g_spdk_nvme_transport_opts.rdma_srq_size != 0) {
		rc = ibv_query_device(ctx, &dev_attr);
		if (rc) {
			SPDK_ERRLOG("Unable to query RDMA device.\n");
			goto fail;
		}

		poller->pd = spdk_rdma_get_pd(ctx);
		if (poller->pd == NULL) {
			SPDK_ERRLOG("Unable to get PD.\n");
			goto fail;
		}

		poller->mr_map = spdk_rdma_create_mem_map(poller->pd, &g_nvme_hooks,
				 SPDK_RDMA_MEMORY_MAP_ROLE_INITIATOR);
		if (poller->mr_map == NULL) {
			SPDK_ERRLOG("Unable to create memory map.\n");
			goto fail;
		}

		srq_init_attr.stats = &poller->stats.rdma_stats.recv;
		srq_init_attr.pd = poller->pd;
		srq_init_attr.srq_init_attr.attr.max_wr = spdk_min((uint32_t)dev_attr.max_srq_wr,
				g_spdk_nvme_transport_opts.rdma_srq_size);
		srq_init_attr.srq_init_attr.attr.max_sge = spdk_min(dev_attr.max_sge,
				NVME_RDMA_DEFAULT_RX_SGE);

		poller->srq = spdk_rdma_srq_create(&srq_init_attr);
		if (poller->srq == NULL) {
			SPDK_ERRLOG("Unable to create SRQ.\n");
			goto fail;
		}

		opts.num_entries = g_spdk_nvme_transport_opts.rdma_srq_size;
		opts.rqpair = NULL;
		opts.srq = poller->srq;
		opts.mr_map = poller->mr_map;

		poller->rsps = nvme_rdma_create_rsps(&opts);
		if (poller->rsps == NULL) {
			SPDK_ERRLOG("Unable to create poller RDMA responses.\n");
			goto fail;
		}

		rc = nvme_rdma_poller_submit_recvs(poller);
		if (rc) {
			SPDK_ERRLOG("Unable to submit poller RDMA responses.\n");
			goto fail;
		}

		/*
		 * When using an srq, fix the size of the completion queue at startup.
		 * The initiator sends only send and recv WRs. Hence, the multiplier is 2.
		 * (The target sends also data WRs. Hence, the multiplier is 3.)
		 */
		num_cqe = g_spdk_nvme_transport_opts.rdma_srq_size * 2;
	} else {
		num_cqe = DEFAULT_NVME_RDMA_CQ_SIZE;
	}

	poller->cq = ibv_create_cq(poller->device, num_cqe, group, NULL, 0);

	if (poller->cq == NULL) {
		SPDK_ERRLOG("Unable to create CQ, errno %d.\n", errno);
		goto fail;
	}

	STAILQ_INSERT_HEAD(&group->pollers, poller, link);
	group->num_pollers++;
	poller->current_num_wc = num_cqe;
	poller->required_num_wc = 0;
	return poller;

fail:
	nvme_rdma_poller_destroy(poller);
	return NULL;
}

static void
nvme_rdma_poll_group_free_pollers(struct nvme_rdma_poll_group *group)
{
	struct nvme_rdma_poller	*poller, *tmp_poller;

	STAILQ_FOREACH_SAFE(poller, &group->pollers, link, tmp_poller) {
		assert(poller->refcnt == 0);
		if (poller->refcnt) {
			SPDK_WARNLOG("Destroying poller with non-zero ref count: poller %p, refcnt %d\n",
				     poller, poller->refcnt);
		}

		STAILQ_REMOVE(&group->pollers, poller, nvme_rdma_poller, link);
		nvme_rdma_poller_destroy(poller);
	}
}

static struct nvme_rdma_poller *
nvme_rdma_poll_group_get_poller(struct nvme_rdma_poll_group *group, struct ibv_context *device)
{
	struct nvme_rdma_poller *poller = NULL;

	STAILQ_FOREACH(poller, &group->pollers, link) {
		if (poller->device == device) {
			break;
		}
	}

	if (!poller) {
		poller = nvme_rdma_poller_create(group, device);
		if (!poller) {
			SPDK_ERRLOG("Failed to create a poller for device %p\n", device);
			return NULL;
		}
	}

	poller->refcnt++;
	return poller;
}

static void
nvme_rdma_poll_group_put_poller(struct nvme_rdma_poll_group *group, struct nvme_rdma_poller *poller)
{
	assert(poller->refcnt > 0);
	if (--poller->refcnt == 0) {
		STAILQ_REMOVE(&group->pollers, poller, nvme_rdma_poller, link);
		group->num_pollers--;
		nvme_rdma_poller_destroy(poller);
	}
}

static struct spdk_nvme_transport_poll_group *
nvme_rdma_poll_group_create(void)
{
	struct nvme_rdma_poll_group	*group;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	STAILQ_INIT(&group->pollers);
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
	struct nvme_rdma_qpair		*rqpair = nvme_rdma_qpair(qpair);
	struct nvme_rdma_poll_group	*group = nvme_rdma_poll_group(tgroup);

	assert(qpair->poll_group_tailq_head == &tgroup->disconnected_qpairs);

	if (rqpair->poller) {
		nvme_rdma_poll_group_put_poller(group, rqpair->poller);

		rqpair->poller = NULL;
		rqpair->cq = NULL;
	}

	return 0;
}

static int64_t
nvme_rdma_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct spdk_nvme_qpair			*qpair, *tmp_qpair;
	struct nvme_rdma_qpair			*rqpair;
	struct nvme_rdma_poll_group		*group;
	struct nvme_rdma_poller			*poller;
	int					num_qpairs = 0, batch_size, rc, rc2 = 0;
	int64_t					total_completions = 0;
	uint64_t				completions_allowed = 0;
	uint64_t				completions_per_poller = 0;
	uint64_t				poller_completions = 0;
	uint64_t				rdma_completions;

	if (completions_per_qpair == 0) {
		completions_per_qpair = MAX_COMPLETIONS_PER_POLL;
	}

	group = nvme_rdma_poll_group(tgroup);
	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		rc = nvme_rdma_ctrlr_disconnect_qpair_poll(qpair->ctrlr, qpair);
		if (rc == 0) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
		}
	}

	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
		rqpair = nvme_rdma_qpair(qpair);
		rqpair->num_completions = 0;

		if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
			rc = nvme_rdma_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
			if (rc == 0) {
				/* Once the connection is completed, we can submit queued requests */
				nvme_qpair_resubmit_requests(qpair, rqpair->num_entries);
			} else if (rc != -EAGAIN) {
				SPDK_ERRLOG("Failed to connect rqpair=%p\n", rqpair);
				nvme_rdma_fail_qpair(qpair, 0);
				continue;
			}
		} else {
			nvme_rdma_qpair_process_cm_event(rqpair);
		}

		if (spdk_unlikely(qpair->transport_failure_reason != SPDK_NVME_QPAIR_FAILURE_NONE)) {
			rc2 = -ENXIO;
			nvme_rdma_fail_qpair(qpair, 0);
			continue;
		}
		num_qpairs++;
	}

	completions_allowed = completions_per_qpair * num_qpairs;
	if (group->num_pollers) {
		completions_per_poller = spdk_max(completions_allowed / group->num_pollers, 1);
	}

	STAILQ_FOREACH(poller, &group->pollers, link) {
		poller_completions = 0;
		rdma_completions = 0;
		do {
			poller->stats.polls++;
			batch_size = spdk_min((completions_per_poller - poller_completions), MAX_COMPLETIONS_PER_POLL);
			rc = nvme_rdma_cq_process_completions(poller->cq, batch_size, poller, NULL, &rdma_completions);
			if (rc <= 0) {
				if (rc == -ECANCELED) {
					return -EIO;
				} else if (rc == 0) {
					poller->stats.idle_polls++;
				}
				break;
			}

			poller_completions += rc;
		} while (poller_completions < completions_per_poller);
		total_completions += poller_completions;
		poller->stats.completions += rdma_completions;
		if (poller->srq) {
			nvme_rdma_poller_submit_recvs(poller);
		}
	}

	STAILQ_FOREACH_SAFE(qpair, &tgroup->connected_qpairs, poll_group_stailq, tmp_qpair) {
		rqpair = nvme_rdma_qpair(qpair);

		if (spdk_unlikely(rqpair->state <= NVME_RDMA_QPAIR_STATE_INITIALIZING)) {
			continue;
		}

		if (spdk_unlikely(qpair->ctrlr->timeout_enabled)) {
			nvme_rdma_qpair_check_timeout(qpair);
		}

		nvme_rdma_qpair_submit_sends(rqpair);
		if (!rqpair->srq) {
			nvme_rdma_qpair_submit_recvs(rqpair);
		}
		if (rqpair->num_completions > 0) {
			nvme_qpair_resubmit_requests(qpair, rqpair->num_completions);
		}
	}

	return rc2 != 0 ? rc2 : total_completions;
}

static int
nvme_rdma_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	struct nvme_rdma_poll_group	*group = nvme_rdma_poll_group(tgroup);

	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	nvme_rdma_poll_group_free_pollers(group);
	free(group);

	return 0;
}

static int
nvme_rdma_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_rdma_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;
	struct spdk_nvme_rdma_device_stat *device_stat;
	struct nvme_rdma_poller *poller;
	uint32_t i = 0;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	group = nvme_rdma_poll_group(tgroup);
	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for RDMA stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_RDMA;
	stats->rdma.num_devices = group->num_pollers;

	if (stats->rdma.num_devices == 0) {
		*_stats = stats;
		return 0;
	}

	stats->rdma.device_stats = calloc(stats->rdma.num_devices, sizeof(*stats->rdma.device_stats));
	if (!stats->rdma.device_stats) {
		SPDK_ERRLOG("Can't allocate memory for RDMA device stats\n");
		free(stats);
		return -ENOMEM;
	}

	STAILQ_FOREACH(poller, &group->pollers, link) {
		device_stat = &stats->rdma.device_stats[i];
		device_stat->name = poller->device->device->name;
		device_stat->polls = poller->stats.polls;
		device_stat->idle_polls = poller->stats.idle_polls;
		device_stat->completions = poller->stats.completions;
		device_stat->queued_requests = poller->stats.queued_requests;
		device_stat->total_send_wrs = poller->stats.rdma_stats.send.num_submitted_wrs;
		device_stat->send_doorbell_updates = poller->stats.rdma_stats.send.doorbell_updates;
		device_stat->total_recv_wrs = poller->stats.rdma_stats.recv.num_submitted_wrs;
		device_stat->recv_doorbell_updates = poller->stats.rdma_stats.recv.doorbell_updates;
		i++;
	}

	*_stats = stats;

	return 0;
}

static void
nvme_rdma_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
				struct spdk_nvme_transport_poll_group_stat *stats)
{
	if (stats) {
		free(stats->rdma.device_stats);
	}
	free(stats);
}

static int
nvme_rdma_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_memory_domain **domains, int array_size)
{
	struct nvme_rdma_qpair *rqpair = nvme_rdma_qpair(ctrlr->adminq);

	if (domains && array_size > 0) {
		domains[0] = rqpair->memory_domain->domain;
	}

	return 1;
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
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

	.ctrlr_get_max_xfer_size = nvme_rdma_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_rdma_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_rdma_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_rdma_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_rdma_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_rdma_ctrlr_disconnect_qpair,

	.ctrlr_get_memory_domains = nvme_rdma_ctrlr_get_memory_domains,

	.qpair_abort_reqs = nvme_rdma_qpair_abort_reqs,
	.qpair_reset = nvme_rdma_qpair_reset,
	.qpair_submit_request = nvme_rdma_qpair_submit_request,
	.qpair_process_completions = nvme_rdma_qpair_process_completions,
	.qpair_iterate_requests = nvme_rdma_qpair_iterate_requests,
	.admin_qpair_abort_aers = nvme_rdma_admin_qpair_abort_aers,

	.poll_group_create = nvme_rdma_poll_group_create,
	.poll_group_connect_qpair = nvme_rdma_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_rdma_poll_group_disconnect_qpair,
	.poll_group_add = nvme_rdma_poll_group_add,
	.poll_group_remove = nvme_rdma_poll_group_remove,
	.poll_group_process_completions = nvme_rdma_poll_group_process_completions,
	.poll_group_destroy = nvme_rdma_poll_group_destroy,
	.poll_group_get_stats = nvme_rdma_poll_group_get_stats,
	.poll_group_free_stats = nvme_rdma_poll_group_free_stats,
};

SPDK_NVME_TRANSPORT_REGISTER(rdma, &rdma_ops);
