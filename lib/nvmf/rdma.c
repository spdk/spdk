/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "spdk/config.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/assert.h"
#include "spdk/log.h"
#include "spdk_internal/rdma.h"

#include "nvmf_internal.h"

#include "spdk_internal/trace_defs.h"

struct spdk_nvme_rdma_hooks g_nvmf_hooks = {};
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma;

/*
 RDMA Connection Resource Defaults
 */
#define NVMF_DEFAULT_TX_SGE		SPDK_NVMF_MAX_SGL_ENTRIES
#define NVMF_DEFAULT_RSP_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

/* The RDMA completion queue size */
#define DEFAULT_NVMF_RDMA_CQ_SIZE	4096
#define MAX_WR_PER_QP(queue_depth)	(queue_depth * 3 + 2)

static int g_spdk_nvmf_ibv_query_mask =
	IBV_QP_STATE |
	IBV_QP_PKEY_INDEX |
	IBV_QP_PORT |
	IBV_QP_ACCESS_FLAGS |
	IBV_QP_AV |
	IBV_QP_PATH_MTU |
	IBV_QP_DEST_QPN |
	IBV_QP_RQ_PSN |
	IBV_QP_MAX_DEST_RD_ATOMIC |
	IBV_QP_MIN_RNR_TIMER |
	IBV_QP_SQ_PSN |
	IBV_QP_TIMEOUT |
	IBV_QP_RETRY_CNT |
	IBV_QP_RNR_RETRY |
	IBV_QP_MAX_QP_RD_ATOMIC;

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
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,

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
	RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,

	/* The request is ready to send a completion */
	RDMA_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request is currently transferring data from the controller to the host. */
	RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,

	/* The request currently has an outstanding completion without an
	 * associated data transfer.
	 */
	RDMA_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	RDMA_REQUEST_STATE_COMPLETED,

	/* Terminator */
	RDMA_REQUEST_NUM_STATES,
};

SPDK_TRACE_REGISTER_FN(nvmf_trace, "nvmf_rdma", TRACE_GROUP_NVMF_RDMA)
{
	spdk_trace_register_object(OBJECT_NVMF_RDMA_IO, 'r');
	spdk_trace_register_description("RDMA_REQ_NEW", TRACE_RDMA_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 1,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_NEED_BUFFER", TRACE_RDMA_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_C2H",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_H2C",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_TX_H2C",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_EXECUTE",
					TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_EXECUTING",
					TRACE_RDMA_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_EXECUTED",
					TRACE_RDMA_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_COMPL",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_COMPLETING_C2H",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_COMPLETING",
					TRACE_RDMA_REQUEST_STATE_COMPLETING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("RDMA_REQ_COMPLETED",
					TRACE_RDMA_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");

	spdk_trace_register_description("RDMA_QP_CREATE", TRACE_RDMA_QP_CREATE,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("RDMA_IBV_ASYNC_EVENT", TRACE_RDMA_IBV_ASYNC_EVENT,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");
	spdk_trace_register_description("RDMA_CM_ASYNC_EVENT", TRACE_RDMA_CM_ASYNC_EVENT,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "type");
	spdk_trace_register_description("RDMA_QP_STATE_CHANGE", TRACE_RDMA_QP_STATE_CHANGE,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "state");
	spdk_trace_register_description("RDMA_QP_DISCONNECT", TRACE_RDMA_QP_DISCONNECT,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("RDMA_QP_DESTROY", TRACE_RDMA_QP_DESTROY,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
}

enum spdk_nvmf_rdma_wr_type {
	RDMA_WR_TYPE_RECV,
	RDMA_WR_TYPE_SEND,
	RDMA_WR_TYPE_DATA,
};

struct spdk_nvmf_rdma_wr {
	enum spdk_nvmf_rdma_wr_type	type;
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_rdma_recv {
	struct ibv_recv_wr			wr;
	struct ibv_sge				sgl[NVMF_DEFAULT_RX_SGE];

	struct spdk_nvmf_rdma_qpair		*qpair;

	/* In-capsule data buffer */
	uint8_t					*buf;

	struct spdk_nvmf_rdma_wr		rdma_wr;
	uint64_t				receive_tsc;

	STAILQ_ENTRY(spdk_nvmf_rdma_recv)	link;
};

struct spdk_nvmf_rdma_request_data {
	struct spdk_nvmf_rdma_wr	rdma_wr;
	struct ibv_send_wr		wr;
	struct ibv_sge			sgl[SPDK_NVMF_MAX_SGL_ENTRIES];
};

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;

	enum spdk_nvmf_rdma_request_state	state;

	/* Data offset in req.iov */
	uint32_t				offset;

	struct spdk_nvmf_rdma_recv		*recv;

	struct {
		struct spdk_nvmf_rdma_wr	rdma_wr;
		struct	ibv_send_wr		wr;
		struct	ibv_sge			sgl[NVMF_DEFAULT_RSP_SGE];
	} rsp;

	struct spdk_nvmf_rdma_request_data	data;

	uint32_t				iovpos;

	uint32_t				num_outstanding_data_wr;
	uint64_t				receive_tsc;

	STAILQ_ENTRY(spdk_nvmf_rdma_request)	state_link;
};

struct spdk_nvmf_rdma_resource_opts {
	struct spdk_nvmf_rdma_qpair	*qpair;
	/* qp points either to an ibv_qp object or an ibv_srq object depending on the value of shared. */
	void				*qp;
	struct ibv_pd			*pd;
	uint32_t			max_queue_depth;
	uint32_t			in_capsule_data_size;
	bool				shared;
};

struct spdk_nvmf_rdma_resources {
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

	/* Receives that are waiting for a request object */
	STAILQ_HEAD(, spdk_nvmf_rdma_recv)	incoming_queue;

	/* Queue to track free requests */
	STAILQ_HEAD(, spdk_nvmf_rdma_request)	free_queue;
};

typedef void (*spdk_nvmf_rdma_qpair_ibv_event)(struct spdk_nvmf_rdma_qpair *rqpair);

struct spdk_nvmf_rdma_ibv_event_ctx {
	struct spdk_nvmf_rdma_qpair			*rqpair;
	spdk_nvmf_rdma_qpair_ibv_event			cb_fn;
	/* Link to other ibv events associated with this qpair */
	STAILQ_ENTRY(spdk_nvmf_rdma_ibv_event_ctx)	link;
};

struct spdk_nvmf_rdma_qpair {
	struct spdk_nvmf_qpair			qpair;

	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;

	struct spdk_rdma_qp			*rdma_qp;
	struct rdma_cm_id			*cm_id;
	struct spdk_rdma_srq			*srq;
	struct rdma_cm_id			*listen_id;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* The maximum number of active RDMA READ and ATOMIC operations at one time */
	uint16_t				max_read_depth;

	/* The maximum number of RDMA SEND operations at one time */
	uint32_t				max_send_depth;

	/* The current number of outstanding WRs from this qpair's
	 * recv queue. Should not exceed device->attr.max_queue_depth.
	 */
	uint16_t				current_recv_depth;

	/* The current number of active RDMA READ operations */
	uint16_t				current_read_depth;

	/* The current number of posted WRs from this qpair's
	 * send queue. Should not exceed max_send_depth.
	 */
	uint32_t				current_send_depth;

	/* The maximum number of SGEs per WR on the send queue */
	uint32_t				max_send_sge;

	/* The maximum number of SGEs per WR on the recv queue */
	uint32_t				max_recv_sge;

	struct spdk_nvmf_rdma_resources		*resources;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_read_queue;

	STAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_rdma_write_queue;

	/* Number of requests not in the free state */
	uint32_t				qd;

	TAILQ_ENTRY(spdk_nvmf_rdma_qpair)	link;

	STAILQ_ENTRY(spdk_nvmf_rdma_qpair)	recv_link;

	STAILQ_ENTRY(spdk_nvmf_rdma_qpair)	send_link;

	/* IBV queue pair attributes: they are used to manage
	 * qp state and recover from errors.
	 */
	enum ibv_qp_state			ibv_state;

	/*
	 * io_channel which is used to destroy qpair when it is removed from poll group
	 */
	struct spdk_io_channel		*destruct_channel;

	/* List of ibv async events */
	STAILQ_HEAD(, spdk_nvmf_rdma_ibv_event_ctx)	ibv_events;

	/* Lets us know that we have received the last_wqe event. */
	bool					last_wqe_reached;

	/* Indicate that nvmf_rdma_close_qpair is called */
	bool					to_close;
};

struct spdk_nvmf_rdma_poller_stat {
	uint64_t				completions;
	uint64_t				polls;
	uint64_t				idle_polls;
	uint64_t				requests;
	uint64_t				request_latency;
	uint64_t				pending_free_request;
	uint64_t				pending_rdma_read;
	uint64_t				pending_rdma_write;
	struct spdk_rdma_qp_stats		qp_stats;
};

struct spdk_nvmf_rdma_poller {
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poll_group	*group;

	int					num_cqe;
	int					required_num_wr;
	struct ibv_cq				*cq;

	/* The maximum number of I/O outstanding on the shared receive queue at one time */
	uint16_t				max_srq_depth;

	/* Shared receive queue */
	struct spdk_rdma_srq			*srq;

	struct spdk_nvmf_rdma_resources		*resources;
	struct spdk_nvmf_rdma_poller_stat	stat;

	TAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs;

	STAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs_pending_recv;

	STAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs_pending_send;

	TAILQ_ENTRY(spdk_nvmf_rdma_poller)	link;
};

struct spdk_nvmf_rdma_poll_group_stat {
	uint64_t				pending_data_buffer;
};

struct spdk_nvmf_rdma_poll_group {
	struct spdk_nvmf_transport_poll_group		group;
	struct spdk_nvmf_rdma_poll_group_stat		stat;
	TAILQ_HEAD(, spdk_nvmf_rdma_poller)		pollers;
	TAILQ_ENTRY(spdk_nvmf_rdma_poll_group)		link;
};

struct spdk_nvmf_rdma_conn_sched {
	struct spdk_nvmf_rdma_poll_group *next_admin_pg;
	struct spdk_nvmf_rdma_poll_group *next_io_pg;
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
struct spdk_nvmf_rdma_device {
	struct ibv_device_attr			attr;
	struct ibv_context			*context;

	struct spdk_rdma_mem_map		*map;
	struct ibv_pd				*pd;

	int					num_srq;

	TAILQ_ENTRY(spdk_nvmf_rdma_device)	link;
};

struct spdk_nvmf_rdma_port {
	const struct spdk_nvme_transport_id	*trid;
	struct rdma_cm_id			*id;
	struct spdk_nvmf_rdma_device		*device;
	TAILQ_ENTRY(spdk_nvmf_rdma_port)	link;
};

struct rdma_transport_opts {
	int		num_cqe;
	uint32_t	max_srq_depth;
	bool		no_srq;
	bool		no_wr_batching;
	int		acceptor_backlog;
};

struct spdk_nvmf_rdma_transport {
	struct spdk_nvmf_transport	transport;
	struct rdma_transport_opts	rdma_opts;

	struct spdk_nvmf_rdma_conn_sched conn_sched;

	struct rdma_event_channel	*event_channel;

	struct spdk_mempool		*data_wr_pool;

	struct spdk_poller		*accept_poller;
	pthread_mutex_t			lock;

	/* fields used to poll RDMA/IB events */
	nfds_t			npoll_fds;
	struct pollfd		*poll_fds;

	TAILQ_HEAD(, spdk_nvmf_rdma_device)	devices;
	TAILQ_HEAD(, spdk_nvmf_rdma_port)	ports;
	TAILQ_HEAD(, spdk_nvmf_rdma_poll_group)	poll_groups;
};

static const struct spdk_json_object_decoder rdma_transport_opts_decoder[] = {
	{
		"num_cqe", offsetof(struct rdma_transport_opts, num_cqe),
		spdk_json_decode_int32, true
	},
	{
		"max_srq_depth", offsetof(struct rdma_transport_opts, max_srq_depth),
		spdk_json_decode_uint32, true
	},
	{
		"no_srq", offsetof(struct rdma_transport_opts, no_srq),
		spdk_json_decode_bool, true
	},
	{
		"no_wr_batching", offsetof(struct rdma_transport_opts, no_wr_batching),
		spdk_json_decode_bool, true
	},
	{
		"acceptor_backlog", offsetof(struct rdma_transport_opts, acceptor_backlog),
		spdk_json_decode_int32, true
	},
};

static bool
nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
			  struct spdk_nvmf_rdma_request *rdma_req);

static void
_poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller);

static void
_poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller);

static inline int
nvmf_rdma_check_ibv_state(enum ibv_qp_state state)
{
	switch (state) {
	case IBV_QPS_RESET:
	case IBV_QPS_INIT:
	case IBV_QPS_RTR:
	case IBV_QPS_RTS:
	case IBV_QPS_SQD:
	case IBV_QPS_SQE:
	case IBV_QPS_ERR:
		return 0;
	default:
		return -1;
	}
}

static inline enum spdk_nvme_media_error_status_code
nvmf_rdma_dif_error_to_compl_status(uint8_t err_type) {
	enum spdk_nvme_media_error_status_code result;
	switch (err_type)
	{
	case SPDK_DIF_REFTAG_ERROR:
		result = SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_APPTAG_ERROR:
		result = SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR;
		break;
	case SPDK_DIF_GUARD_ERROR:
		result = SPDK_NVME_SC_GUARD_CHECK_ERROR;
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return result;
}

static enum ibv_qp_state
nvmf_rdma_update_ibv_state(struct spdk_nvmf_rdma_qpair *rqpair) {
	enum ibv_qp_state old_state, new_state;
	struct ibv_qp_attr qp_attr;
	struct ibv_qp_init_attr init_attr;
	int rc;

	old_state = rqpair->ibv_state;
	rc = ibv_query_qp(rqpair->rdma_qp->qp, &qp_attr,
			  g_spdk_nvmf_ibv_query_mask, &init_attr);

	if (rc)
	{
		SPDK_ERRLOG("Failed to get updated RDMA queue pair state!\n");
		return IBV_QPS_ERR + 1;
	}

	new_state = qp_attr.qp_state;
	rqpair->ibv_state = new_state;
	qp_attr.ah_attr.port_num = qp_attr.port_num;

	rc = nvmf_rdma_check_ibv_state(new_state);
	if (rc)
	{
		SPDK_ERRLOG("QP#%d: bad state updated: %u, maybe hardware issue\n", rqpair->qpair.qid, new_state);
		/*
		 * IBV_QPS_UNKNOWN undefined if lib version smaller than libibverbs-1.1.8
		 * IBV_QPS_UNKNOWN is the enum element after IBV_QPS_ERR
		 */
		return IBV_QPS_ERR + 1;
	}

	if (old_state != new_state)
	{
		spdk_trace_record(TRACE_RDMA_QP_STATE_CHANGE, 0, 0, (uintptr_t)rqpair, new_state);
	}
	return new_state;
}

static void
nvmf_rdma_request_free_data(struct spdk_nvmf_rdma_request *rdma_req,
			    struct spdk_nvmf_rdma_transport *rtransport)
{
	struct spdk_nvmf_rdma_request_data	*data_wr;
	struct ibv_send_wr			*next_send_wr;
	uint64_t				req_wrid;

	rdma_req->num_outstanding_data_wr = 0;
	data_wr = &rdma_req->data;
	req_wrid = data_wr->wr.wr_id;
	while (data_wr && data_wr->wr.wr_id == req_wrid) {
		memset(data_wr->sgl, 0, sizeof(data_wr->wr.sg_list[0]) * data_wr->wr.num_sge);
		data_wr->wr.num_sge = 0;
		next_send_wr = data_wr->wr.next;
		if (data_wr != &rdma_req->data) {
			data_wr->wr.next = NULL;
			spdk_mempool_put(rtransport->data_wr_pool, data_wr);
		}
		data_wr = (!next_send_wr || next_send_wr == &rdma_req->rsp.wr) ? NULL :
			  SPDK_CONTAINEROF(next_send_wr, struct spdk_nvmf_rdma_request_data, wr);
	}
	rdma_req->data.wr.next = NULL;
	rdma_req->rsp.wr.next = NULL;
}

static void
nvmf_rdma_dump_request(struct spdk_nvmf_rdma_request *req)
{
	SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", req->req.data_from_pool);
	if (req->req.cmd) {
		SPDK_ERRLOG("\t\tRequest opcode: %d\n", req->req.cmd->nvmf_cmd.opcode);
	}
	if (req->recv) {
		SPDK_ERRLOG("\t\tRequest recv wr_id%lu\n", req->recv->wr.wr_id);
	}
}

static void
nvmf_rdma_dump_qpair_contents(struct spdk_nvmf_rdma_qpair *rqpair)
{
	int i;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", rqpair->qpair.qid);
	for (i = 0; i < rqpair->max_queue_depth; i++) {
		if (rqpair->resources->reqs[i].state != RDMA_REQUEST_STATE_FREE) {
			nvmf_rdma_dump_request(&rqpair->resources->reqs[i]);
		}
	}
}

static void
nvmf_rdma_resources_destroy(struct spdk_nvmf_rdma_resources *resources)
{
	if (resources->cmds_mr) {
		ibv_dereg_mr(resources->cmds_mr);
	}

	if (resources->cpls_mr) {
		ibv_dereg_mr(resources->cpls_mr);
	}

	if (resources->bufs_mr) {
		ibv_dereg_mr(resources->bufs_mr);
	}

	spdk_free(resources->cmds);
	spdk_free(resources->cpls);
	spdk_free(resources->bufs);
	free(resources->reqs);
	free(resources->recvs);
	free(resources);
}


static struct spdk_nvmf_rdma_resources *
nvmf_rdma_resources_create(struct spdk_nvmf_rdma_resource_opts *opts)
{
	struct spdk_nvmf_rdma_resources	*resources;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_rdma_qp		*qp = NULL;
	struct spdk_rdma_srq		*srq = NULL;
	struct ibv_recv_wr		*bad_wr = NULL;
	uint32_t			i;
	int				rc = 0;

	resources = calloc(1, sizeof(struct spdk_nvmf_rdma_resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
		return NULL;
	}

	resources->reqs = calloc(opts->max_queue_depth, sizeof(*resources->reqs));
	resources->recvs = calloc(opts->max_queue_depth, sizeof(*resources->recvs));
	resources->cmds = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cmds),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cpls = spdk_zmalloc(opts->max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (opts->in_capsule_data_size > 0) {
		resources->bufs = spdk_zmalloc(opts->max_queue_depth * opts->in_capsule_data_size,
					       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
					       SPDK_MALLOC_DMA);
	}

	if (!resources->reqs || !resources->recvs || !resources->cmds ||
	    !resources->cpls || (opts->in_capsule_data_size && !resources->bufs)) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
		goto cleanup;
	}

	resources->cmds_mr = ibv_reg_mr(opts->pd, resources->cmds,
					opts->max_queue_depth * sizeof(*resources->cmds),
					IBV_ACCESS_LOCAL_WRITE);
	resources->cpls_mr = ibv_reg_mr(opts->pd, resources->cpls,
					opts->max_queue_depth * sizeof(*resources->cpls),
					0);

	if (opts->in_capsule_data_size) {
		resources->bufs_mr = ibv_reg_mr(opts->pd, resources->bufs,
						opts->max_queue_depth *
						opts->in_capsule_data_size,
						IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	}

	if (!resources->cmds_mr || !resources->cpls_mr ||
	    (opts->in_capsule_data_size &&
	     !resources->bufs_mr)) {
		goto cleanup;
	}
	SPDK_DEBUGLOG(rdma, "Command Array: %p Length: %lx LKey: %x\n",
		      resources->cmds, opts->max_queue_depth * sizeof(*resources->cmds),
		      resources->cmds_mr->lkey);
	SPDK_DEBUGLOG(rdma, "Completion Array: %p Length: %lx LKey: %x\n",
		      resources->cpls, opts->max_queue_depth * sizeof(*resources->cpls),
		      resources->cpls_mr->lkey);
	if (resources->bufs && resources->bufs_mr) {
		SPDK_DEBUGLOG(rdma, "In Capsule Data Array: %p Length: %x LKey: %x\n",
			      resources->bufs, opts->max_queue_depth *
			      opts->in_capsule_data_size, resources->bufs_mr->lkey);
	}

	/* Initialize queues */
	STAILQ_INIT(&resources->incoming_queue);
	STAILQ_INIT(&resources->free_queue);

	if (opts->shared) {
		srq = (struct spdk_rdma_srq *)opts->qp;
	} else {
		qp = (struct spdk_rdma_qp *)opts->qp;
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_recv = &resources->recvs[i];
		rdma_recv->qpair = opts->qpair;

		/* Set up memory to receive commands */
		if (resources->bufs) {
			rdma_recv->buf = (void *)((uintptr_t)resources->bufs + (i *
						  opts->in_capsule_data_size));
		}

		rdma_recv->rdma_wr.type = RDMA_WR_TYPE_RECV;

		rdma_recv->sgl[0].addr = (uintptr_t)&resources->cmds[i];
		rdma_recv->sgl[0].length = sizeof(resources->cmds[i]);
		rdma_recv->sgl[0].lkey = resources->cmds_mr->lkey;
		rdma_recv->wr.num_sge = 1;

		if (rdma_recv->buf && resources->bufs_mr) {
			rdma_recv->sgl[1].addr = (uintptr_t)rdma_recv->buf;
			rdma_recv->sgl[1].length = opts->in_capsule_data_size;
			rdma_recv->sgl[1].lkey = resources->bufs_mr->lkey;
			rdma_recv->wr.num_sge++;
		}

		rdma_recv->wr.wr_id = (uintptr_t)&rdma_recv->rdma_wr;
		rdma_recv->wr.sg_list = rdma_recv->sgl;
		if (srq) {
			spdk_rdma_srq_queue_recv_wrs(srq, &rdma_recv->wr);
		} else {
			spdk_rdma_qp_queue_recv_wrs(qp, &rdma_recv->wr);
		}
	}

	for (i = 0; i < opts->max_queue_depth; i++) {
		rdma_req = &resources->reqs[i];

		if (opts->qpair != NULL) {
			rdma_req->req.qpair = &opts->qpair->qpair;
		} else {
			rdma_req->req.qpair = NULL;
		}
		rdma_req->req.cmd = NULL;

		/* Set up memory to send responses */
		rdma_req->req.rsp = &resources->cpls[i];

		rdma_req->rsp.sgl[0].addr = (uintptr_t)&resources->cpls[i];
		rdma_req->rsp.sgl[0].length = sizeof(resources->cpls[i]);
		rdma_req->rsp.sgl[0].lkey = resources->cpls_mr->lkey;

		rdma_req->rsp.rdma_wr.type = RDMA_WR_TYPE_SEND;
		rdma_req->rsp.wr.wr_id = (uintptr_t)&rdma_req->rsp.rdma_wr;
		rdma_req->rsp.wr.next = NULL;
		rdma_req->rsp.wr.opcode = IBV_WR_SEND;
		rdma_req->rsp.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->rsp.wr.sg_list = rdma_req->rsp.sgl;
		rdma_req->rsp.wr.num_sge = SPDK_COUNTOF(rdma_req->rsp.sgl);

		/* Set up memory for data buffers */
		rdma_req->data.rdma_wr.type = RDMA_WR_TYPE_DATA;
		rdma_req->data.wr.wr_id = (uintptr_t)&rdma_req->data.rdma_wr;
		rdma_req->data.wr.next = NULL;
		rdma_req->data.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->data.wr.sg_list = rdma_req->data.sgl;
		rdma_req->data.wr.num_sge = SPDK_COUNTOF(rdma_req->data.sgl);

		/* Initialize request state to FREE */
		rdma_req->state = RDMA_REQUEST_STATE_FREE;
		STAILQ_INSERT_TAIL(&resources->free_queue, rdma_req, state_link);
	}

	if (srq) {
		rc = spdk_rdma_srq_flush_recv_wrs(srq, &bad_wr);
	} else {
		rc = spdk_rdma_qp_flush_recv_wrs(qp, &bad_wr);
	}

	if (rc) {
		goto cleanup;
	}

	return resources;

cleanup:
	nvmf_rdma_resources_destroy(resources);
	return NULL;
}

static void
nvmf_rdma_qpair_clean_ibv_events(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx, *tctx;
	STAILQ_FOREACH_SAFE(ctx, &rqpair->ibv_events, link, tctx) {
		ctx->rqpair = NULL;
		/* Memory allocated for ctx is freed in nvmf_rdma_qpair_process_ibv_event */
		STAILQ_REMOVE(&rqpair->ibv_events, ctx, spdk_nvmf_rdma_ibv_event_ctx, link);
	}
}

static void
nvmf_rdma_qpair_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv, *recv_tmp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	int				rc;

	spdk_trace_record(TRACE_RDMA_QP_DESTROY, 0, 0, (uintptr_t)rqpair);

	if (rqpair->qd != 0) {
		struct spdk_nvmf_qpair *qpair = &rqpair->qpair;
		struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(qpair->transport,
				struct spdk_nvmf_rdma_transport, transport);
		struct spdk_nvmf_rdma_request *req;
		uint32_t i, max_req_count = 0;

		SPDK_WARNLOG("Destroying qpair when queue depth is %d\n", rqpair->qd);

		if (rqpair->srq == NULL) {
			nvmf_rdma_dump_qpair_contents(rqpair);
			max_req_count = rqpair->max_queue_depth;
		} else if (rqpair->poller && rqpair->resources) {
			max_req_count = rqpair->poller->max_srq_depth;
		}

		SPDK_DEBUGLOG(rdma, "Release incomplete requests\n");
		for (i = 0; i < max_req_count; i++) {
			req = &rqpair->resources->reqs[i];
			if (req->req.qpair == qpair && req->state != RDMA_REQUEST_STATE_FREE) {
				/* nvmf_rdma_request_process checks qpair ibv and internal state
				 * and completes a request */
				nvmf_rdma_request_process(rtransport, req);
			}
		}
		assert(rqpair->qd == 0);
	}

	if (rqpair->poller) {
		TAILQ_REMOVE(&rqpair->poller->qpairs, rqpair, link);

		if (rqpair->srq != NULL && rqpair->resources != NULL) {
			/* Drop all received but unprocessed commands for this queue and return them to SRQ */
			STAILQ_FOREACH_SAFE(rdma_recv, &rqpair->resources->incoming_queue, link, recv_tmp) {
				if (rqpair == rdma_recv->qpair) {
					STAILQ_REMOVE(&rqpair->resources->incoming_queue, rdma_recv, spdk_nvmf_rdma_recv, link);
					spdk_rdma_srq_queue_recv_wrs(rqpair->srq, &rdma_recv->wr);
					rc = spdk_rdma_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
					if (rc) {
						SPDK_ERRLOG("Unable to re-post rx descriptor\n");
					}
				}
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->rdma_qp != NULL) {
			spdk_rdma_qp_destroy(rqpair->rdma_qp);
			rqpair->rdma_qp = NULL;
		}
		rdma_destroy_id(rqpair->cm_id);

		if (rqpair->poller != NULL && rqpair->srq == NULL) {
			rqpair->poller->required_num_wr -= MAX_WR_PER_QP(rqpair->max_queue_depth);
		}
	}

	if (rqpair->srq == NULL && rqpair->resources != NULL) {
		nvmf_rdma_resources_destroy(rqpair->resources);
	}

	nvmf_rdma_qpair_clean_ibv_events(rqpair);

	if (rqpair->destruct_channel) {
		spdk_put_io_channel(rqpair->destruct_channel);
		rqpair->destruct_channel = NULL;
	}

	free(rqpair);
}

static int
nvmf_rdma_resize_cq(struct spdk_nvmf_rdma_qpair *rqpair, struct spdk_nvmf_rdma_device *device)
{
	struct spdk_nvmf_rdma_poller	*rpoller;
	int				rc, num_cqe, required_num_wr;

	/* Enlarge CQ size dynamically */
	rpoller = rqpair->poller;
	required_num_wr = rpoller->required_num_wr + MAX_WR_PER_QP(rqpair->max_queue_depth);
	num_cqe = rpoller->num_cqe;
	if (num_cqe < required_num_wr) {
		num_cqe = spdk_max(num_cqe * 2, required_num_wr);
		num_cqe = spdk_min(num_cqe, device->attr.max_cqe);
	}

	if (rpoller->num_cqe != num_cqe) {
		if (device->context->device->transport_type == IBV_TRANSPORT_IWARP) {
			SPDK_ERRLOG("iWARP doesn't support CQ resize. Current capacity %u, required %u\n"
				    "Using CQ of insufficient size may lead to CQ overrun\n", rpoller->num_cqe, num_cqe);
			return -1;
		}
		if (required_num_wr > device->attr.max_cqe) {
			SPDK_ERRLOG("RDMA CQE requirement (%d) exceeds device max_cqe limitation (%d)\n",
				    required_num_wr, device->attr.max_cqe);
			return -1;
		}

		SPDK_DEBUGLOG(rdma, "Resize RDMA CQ from %d to %d\n", rpoller->num_cqe, num_cqe);
		rc = ibv_resize_cq(rpoller->cq, num_cqe);
		if (rc) {
			SPDK_ERRLOG("RDMA CQ resize failed: errno %d: %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		rpoller->num_cqe = num_cqe;
	}

	rpoller->required_num_wr = required_num_wr;
	return 0;
}

static int
nvmf_rdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_rdma_resource_opts	opts;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_rdma_qp_init_attr		qp_init_attr = {};

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->device;

	qp_init_attr.qp_context	= rqpair;
	qp_init_attr.pd		= device->pd;
	qp_init_attr.send_cq	= rqpair->poller->cq;
	qp_init_attr.recv_cq	= rqpair->poller->cq;

	if (rqpair->srq) {
		qp_init_attr.srq		= rqpair->srq->srq;
	} else {
		qp_init_attr.cap.max_recv_wr	= rqpair->max_queue_depth;
	}

	/* SEND, READ, and WRITE operations */
	qp_init_attr.cap.max_send_wr	= (uint32_t)rqpair->max_queue_depth * 2;
	qp_init_attr.cap.max_send_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_TX_SGE);
	qp_init_attr.cap.max_recv_sge	= spdk_min((uint32_t)device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
	qp_init_attr.stats		= &rqpair->poller->stat.qp_stats;

	if (rqpair->srq == NULL && nvmf_rdma_resize_cq(rqpair, device) < 0) {
		SPDK_ERRLOG("Failed to resize the completion queue. Cannot initialize qpair.\n");
		goto error;
	}

	rqpair->rdma_qp = spdk_rdma_qp_create(rqpair->cm_id, &qp_init_attr);
	if (!rqpair->rdma_qp) {
		goto error;
	}

	rqpair->max_send_depth = spdk_min((uint32_t)(rqpair->max_queue_depth * 2),
					  qp_init_attr.cap.max_send_wr);
	rqpair->max_send_sge = spdk_min(NVMF_DEFAULT_TX_SGE, qp_init_attr.cap.max_send_sge);
	rqpair->max_recv_sge = spdk_min(NVMF_DEFAULT_RX_SGE, qp_init_attr.cap.max_recv_sge);
	spdk_trace_record(TRACE_RDMA_QP_CREATE, 0, 0, (uintptr_t)rqpair);
	SPDK_DEBUGLOG(rdma, "New RDMA Connection: %p\n", qpair);

	if (rqpair->poller->srq == NULL) {
		rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
		transport = &rtransport->transport;

		opts.qp = rqpair->rdma_qp;
		opts.pd = rqpair->cm_id->pd;
		opts.qpair = rqpair;
		opts.shared = false;
		opts.max_queue_depth = rqpair->max_queue_depth;
		opts.in_capsule_data_size = transport->opts.in_capsule_data_size;

		rqpair->resources = nvmf_rdma_resources_create(&opts);

		if (!rqpair->resources) {
			SPDK_ERRLOG("Unable to allocate resources for receive queue.\n");
			rdma_destroy_qp(rqpair->cm_id);
			goto error;
		}
	} else {
		rqpair->resources = rqpair->poller->resources;
	}

	rqpair->current_recv_depth = 0;
	STAILQ_INIT(&rqpair->pending_rdma_read_queue);
	STAILQ_INIT(&rqpair->pending_rdma_write_queue);

	return 0;

error:
	rdma_destroy_id(rqpair->cm_id);
	rqpair->cm_id = NULL;
	return -1;
}

/* Append the given recv wr structure to the resource structs outstanding recvs list. */
/* This function accepts either a single wr or the first wr in a linked list. */
static void
nvmf_rdma_qpair_queue_recv_wrs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *first)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	if (rqpair->srq != NULL) {
		spdk_rdma_srq_queue_recv_wrs(rqpair->srq, first);
	} else {
		if (spdk_rdma_qp_queue_recv_wrs(rqpair->rdma_qp, first)) {
			STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_recv, rqpair, recv_link);
		}
	}

	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_recvs(rtransport, rqpair->poller);
	}
}

static int
request_transfer_in(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_transport *rtransport;

	qpair = req->qpair;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	assert(rdma_req != NULL);

	if (spdk_rdma_qp_queue_send_wrs(rqpair->rdma_qp, &rdma_req->data.wr)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	}
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	}

	rqpair->current_read_depth += rdma_req->num_outstanding_data_wr;
	rqpair->current_send_depth += rdma_req->num_outstanding_data_wr;
	return 0;
}

static int
request_transfer_out(struct spdk_nvmf_request *req, int *data_posted)
{
	int				num_outstanding_data_wr = 0;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvme_cpl		*rsp;
	struct ibv_send_wr		*first = NULL;
	struct spdk_nvmf_rdma_transport *rtransport;

	*data_posted = 0;
	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport, transport);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	/* queue the capsule for the recv buffer */
	assert(rdma_req->recv != NULL);

	nvmf_rdma_qpair_queue_recv_wrs(rqpair, &rdma_req->recv->wr);

	rdma_req->recv = NULL;
	assert(rqpair->current_recv_depth > 0);
	rqpair->current_recv_depth--;

	/* Build the response which consists of optional
	 * RDMA WRITEs to transfer data, plus an RDMA SEND
	 * containing the response.
	 */
	first = &rdma_req->rsp.wr;

	if (rsp->status.sc != SPDK_NVME_SC_SUCCESS) {
		/* On failure, data was not read from the controller. So clear the
		 * number of outstanding data WRs to zero.
		 */
		rdma_req->num_outstanding_data_wr = 0;
	} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		first = &rdma_req->data.wr;
		*data_posted = 1;
		num_outstanding_data_wr = rdma_req->num_outstanding_data_wr;
	}
	if (spdk_rdma_qp_queue_send_wrs(rqpair->rdma_qp, first)) {
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	}
	if (rtransport->rdma_opts.no_wr_batching) {
		_poller_submit_sends(rtransport, rqpair->poller);
	}

	/* +1 for the rsp wr */
	rqpair->current_send_depth += num_outstanding_data_wr + 1;

	return 0;
}

static int
nvmf_rdma_event_accept(struct rdma_cm_id *id, struct spdk_nvmf_rdma_qpair *rqpair)
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
		ctrlr_event_data.initiator_depth = rqpair->max_read_depth;
	}

	/* Configure infinite retries for the initiator side qpair.
	 * When using a shared receive queue on the target side,
	 * we need to pass this value to the initiator to prevent the
	 * initiator side NIC from completing SEND requests back to the
	 * initiator with status rnr_retry_count_exceeded. */
	if (rqpair->srq != NULL) {
		ctrlr_event_data.rnr_retry_count = 0x7;
	}

	/* When qpair is created without use of rdma cm API, an additional
	 * information must be provided to initiator in the connection response:
	 * whether qpair is using SRQ and its qp_num
	 * Fields below are ignored by rdma cm if qpair has been
	 * created using rdma cm API. */
	ctrlr_event_data.srq = rqpair->srq ? 1 : 0;
	ctrlr_event_data.qp_num = rqpair->rdma_qp->qp->qp_num;

	rc = spdk_rdma_qp_accept(rqpair->rdma_qp, &ctrlr_event_data);
	if (rc) {
		SPDK_ERRLOG("Error %d on spdk_rdma_qp_accept\n", errno);
	} else {
		SPDK_DEBUGLOG(rdma, "Sent back the accept\n");
	}

	return rc;
}

static void
nvmf_rdma_event_reject(struct rdma_cm_id *id, enum spdk_nvmf_rdma_transport_error error)
{
	struct spdk_nvmf_rdma_reject_private_data	rej_data;

	rej_data.recfmt = 0;
	rej_data.sts = error;

	rdma_reject(id, &rej_data, sizeof(rej_data));
}

static int
nvmf_rdma_connect(struct spdk_nvmf_transport *transport, struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct spdk_nvmf_rdma_port	*port;
	struct rdma_conn_param		*rdma_param = NULL;
	const struct spdk_nvmf_rdma_request_private_data *private_data = NULL;
	uint16_t			max_queue_depth;
	uint16_t			max_read_depth;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	assert(event->id != NULL); /* Impossible. Can't even reject the connection. */
	assert(event->id->verbs != NULL); /* Impossible. No way to handle this. */

	rdma_param = &event->param.conn;
	if (rdma_param->private_data == NULL ||
	    rdma_param->private_data_len < sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		SPDK_ERRLOG("connect request: no private data provided\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH);
		return -1;
	}

	private_data = rdma_param->private_data;
	if (private_data->recfmt != 0) {
		SPDK_ERRLOG("Received RDMA private data with RECFMT != 0\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_INVALID_RECFMT);
		return -1;
	}

	SPDK_DEBUGLOG(rdma, "Connect Recv on fabric intf name %s, dev_name %s\n",
		      event->id->verbs->device->name, event->id->verbs->device->dev_name);

	port = event->listen_id->context;
	SPDK_DEBUGLOG(rdma, "Listen Id was %p with verbs %p. ListenAddr: %p\n",
		      event->listen_id, event->listen_id->verbs, port);

	/* Figure out the supported queue depth. This is a multi-step process
	 * that takes into account hardware maximums, host provided values,
	 * and our target's internal memory limits */

	SPDK_DEBUGLOG(rdma, "Calculating Queue Depth\n");

	/* Start with the maximum queue depth allowed by the target */
	max_queue_depth = rtransport->transport.opts.max_queue_depth;
	max_read_depth = rtransport->transport.opts.max_queue_depth;
	SPDK_DEBUGLOG(rdma, "Target Max Queue Depth: %d\n",
		      rtransport->transport.opts.max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      port->device->attr.max_qp_wr, port->device->attr.max_qp_rd_atom);
	max_queue_depth = spdk_min(max_queue_depth, port->device->attr.max_qp_wr);
	max_read_depth = spdk_min(max_read_depth, port->device->attr.max_qp_init_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	SPDK_DEBUGLOG(rdma,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	if (rdma_param->initiator_depth > 0) {
		max_read_depth = spdk_min(max_read_depth, rdma_param->initiator_depth);
	}

	/* Finally check for the host software requested values, which are
	 * optional. */
	if (rdma_param->private_data != NULL &&
	    rdma_param->private_data_len >= sizeof(struct spdk_nvmf_rdma_request_private_data)) {
		SPDK_DEBUGLOG(rdma, "Host Receive Queue Size: %d\n", private_data->hrqsize);
		SPDK_DEBUGLOG(rdma, "Host Send Queue Size: %d\n", private_data->hsqsize);
		max_queue_depth = spdk_min(max_queue_depth, private_data->hrqsize);
		max_queue_depth = spdk_min(max_queue_depth, private_data->hsqsize + 1);
	}

	SPDK_DEBUGLOG(rdma, "Final Negotiated Queue Depth: %d R/W Depth: %d\n",
		      max_queue_depth, max_read_depth);

	rqpair = calloc(1, sizeof(struct spdk_nvmf_rdma_qpair));
	if (rqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		return -1;
	}

	rqpair->device = port->device;
	rqpair->max_queue_depth = max_queue_depth;
	rqpair->max_read_depth = max_read_depth;
	rqpair->cm_id = event->id;
	rqpair->listen_id = event->listen_id;
	rqpair->qpair.transport = transport;
	STAILQ_INIT(&rqpair->ibv_events);
	/* use qid from the private data to determine the qpair type
	   qid will be set to the appropriate value when the controller is created */
	rqpair->qpair.qid = private_data->qid;

	event->id->context = &rqpair->qpair;

	spdk_nvmf_tgt_new_qpair(transport->tgt, &rqpair->qpair);

	return 0;
}

static inline void
nvmf_rdma_setup_wr(struct ibv_send_wr *wr, struct ibv_send_wr *next,
		   enum spdk_nvme_data_transfer xfer)
{
	if (xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		wr->opcode = IBV_WR_RDMA_WRITE;
		wr->send_flags = 0;
		wr->next = next;
	} else if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		wr->opcode = IBV_WR_RDMA_READ;
		wr->send_flags = IBV_SEND_SIGNALED;
		wr->next = NULL;
	} else {
		assert(0);
	}
}

static int
nvmf_request_alloc_wrs(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_request *rdma_req,
		       uint32_t num_sgl_descriptors)
{
	struct spdk_nvmf_rdma_request_data	*work_requests[SPDK_NVMF_MAX_SGL_ENTRIES];
	struct spdk_nvmf_rdma_request_data	*current_data_wr;
	uint32_t				i;

	if (num_sgl_descriptors > SPDK_NVMF_MAX_SGL_ENTRIES) {
		SPDK_ERRLOG("Requested too much entries (%u), the limit is %u\n",
			    num_sgl_descriptors, SPDK_NVMF_MAX_SGL_ENTRIES);
		return -EINVAL;
	}

	if (spdk_mempool_get_bulk(rtransport->data_wr_pool, (void **)work_requests, num_sgl_descriptors)) {
		return -ENOMEM;
	}

	current_data_wr = &rdma_req->data;

	for (i = 0; i < num_sgl_descriptors; i++) {
		nvmf_rdma_setup_wr(&current_data_wr->wr, &work_requests[i]->wr, rdma_req->req.xfer);
		current_data_wr->wr.next = &work_requests[i]->wr;
		current_data_wr = work_requests[i];
		current_data_wr->wr.sg_list = current_data_wr->sgl;
		current_data_wr->wr.wr_id = rdma_req->data.wr.wr_id;
	}

	nvmf_rdma_setup_wr(&current_data_wr->wr, &rdma_req->rsp.wr, rdma_req->req.xfer);

	return 0;
}

static inline void
nvmf_rdma_setup_request(struct spdk_nvmf_rdma_request *rdma_req)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;

	wr->wr.rdma.rkey = sgl->keyed.key;
	wr->wr.rdma.remote_addr = sgl->address;
	nvmf_rdma_setup_wr(wr, &rdma_req->rsp.wr, rdma_req->req.xfer);
}

static inline void
nvmf_rdma_update_remote_addr(struct spdk_nvmf_rdma_request *rdma_req, uint32_t num_wrs)
{
	struct ibv_send_wr		*wr = &rdma_req->data.wr;
	struct spdk_nvme_sgl_descriptor	*sgl = &rdma_req->req.cmd->nvme_cmd.dptr.sgl1;
	uint32_t			i;
	int				j;
	uint64_t			remote_addr_offset = 0;

	for (i = 0; i < num_wrs; ++i) {
		wr->wr.rdma.rkey = sgl->keyed.key;
		wr->wr.rdma.remote_addr = sgl->address + remote_addr_offset;
		for (j = 0; j < wr->num_sge; ++j) {
			remote_addr_offset += wr->sg_list[j].length;
		}
		wr = wr->next;
	}
}

static int
nvmf_rdma_fill_wr_sgl(struct spdk_nvmf_rdma_poll_group *rgroup,
		      struct spdk_nvmf_rdma_device *device,
		      struct spdk_nvmf_rdma_request *rdma_req,
		      struct ibv_send_wr *wr,
		      uint32_t total_length,
		      uint32_t num_extra_wrs)
{
	struct spdk_rdma_memory_translation mem_translation;
	struct spdk_dif_ctx *dif_ctx = NULL;
	struct ibv_sge	*sg_ele;
	struct iovec *iov;
	uint32_t remaining_data_block = 0;
	uint32_t lkey, remaining;
	int rc;

	if (spdk_unlikely(rdma_req->req.dif_enabled)) {
		dif_ctx = &rdma_req->req.dif.dif_ctx;
		remaining_data_block = dif_ctx->block_size - dif_ctx->md_size;
	}

	wr->num_sge = 0;

	while (total_length && (num_extra_wrs || wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES)) {
		iov = &rdma_req->req.iov[rdma_req->iovpos];
		rc = spdk_rdma_get_translation(device->map, iov->iov_base, iov->iov_len, &mem_translation);
		if (spdk_unlikely(rc)) {
			return false;
		}

		lkey = spdk_rdma_memory_translation_get_lkey(&mem_translation);
		sg_ele = &wr->sg_list[wr->num_sge];
		remaining = spdk_min((uint32_t)iov->iov_len - rdma_req->offset, total_length);

		if (spdk_likely(!dif_ctx)) {
			sg_ele->lkey = lkey;
			sg_ele->addr = (uintptr_t)iov->iov_base + rdma_req->offset;
			sg_ele->length = remaining;
			SPDK_DEBUGLOG(rdma, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele, sg_ele->addr,
				      sg_ele->length);
			rdma_req->offset += sg_ele->length;
			total_length -= sg_ele->length;
			wr->num_sge++;

			if (rdma_req->offset == iov->iov_len) {
				rdma_req->offset = 0;
				rdma_req->iovpos++;
			}
		} else {
			uint32_t data_block_size = dif_ctx->block_size - dif_ctx->md_size;
			uint32_t md_size = dif_ctx->md_size;
			uint32_t sge_len;

			while (remaining) {
				if (wr->num_sge >= SPDK_NVMF_MAX_SGL_ENTRIES) {
					if (num_extra_wrs > 0 && wr->next) {
						wr = wr->next;
						wr->num_sge = 0;
						sg_ele = &wr->sg_list[wr->num_sge];
						num_extra_wrs--;
					} else {
						break;
					}
				}
				sg_ele->lkey = lkey;
				sg_ele->addr = (uintptr_t)((char *)iov->iov_base + rdma_req->offset);
				sge_len = spdk_min(remaining, remaining_data_block);
				sg_ele->length = sge_len;
				SPDK_DEBUGLOG(rdma, "sge[%d] %p addr 0x%"PRIx64", len %u\n", wr->num_sge, sg_ele, sg_ele->addr,
					      sg_ele->length);
				remaining -= sge_len;
				remaining_data_block -= sge_len;
				rdma_req->offset += sge_len;
				total_length -= sge_len;

				sg_ele++;
				wr->num_sge++;

				if (remaining_data_block == 0) {
					/* skip metadata */
					rdma_req->offset += md_size;
					total_length -= md_size;
					/* Metadata that do not fit this IO buffer will be included in the next IO buffer */
					remaining -= spdk_min(remaining, md_size);
					remaining_data_block = data_block_size;
				}

				if (remaining == 0) {
					/* By subtracting the size of the last IOV from the offset, we ensure that we skip
					   the remaining metadata bits at the beginning of the next buffer */
					rdma_req->offset -= spdk_min(iov->iov_len, rdma_req->offset);
					rdma_req->iovpos++;
				}
			}
		}
	}

	if (total_length) {
		SPDK_ERRLOG("Not enough SG entries to hold data buffer\n");
		return -EINVAL;
	}

	return 0;
}

static inline uint32_t
nvmf_rdma_calc_num_wrs(uint32_t length, uint32_t io_unit_size, uint32_t block_size)
{
	/* estimate the number of SG entries and WRs needed to process the request */
	uint32_t num_sge = 0;
	uint32_t i;
	uint32_t num_buffers = SPDK_CEIL_DIV(length, io_unit_size);

	for (i = 0; i < num_buffers && length > 0; i++) {
		uint32_t buffer_len = spdk_min(length, io_unit_size);
		uint32_t num_sge_in_block = SPDK_CEIL_DIV(buffer_len, block_size);

		if (num_sge_in_block * block_size > buffer_len) {
			++num_sge_in_block;
		}
		num_sge += num_sge_in_block;
		length -= buffer_len;
	}
	return SPDK_CEIL_DIV(num_sge, SPDK_NVMF_MAX_SGL_ENTRIES);
}

static int
nvmf_rdma_request_fill_iovs(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req,
			    uint32_t length)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct ibv_send_wr			*wr = &rdma_req->data.wr;
	int					rc;
	uint32_t				num_wrs = 1;

	rqpair = SPDK_CONTAINEROF(req->qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	/* rdma wr specifics */
	nvmf_rdma_setup_request(rdma_req);

	rc = spdk_nvmf_request_get_buffers(req, &rgroup->group, &rtransport->transport,
					   length);
	if (rc != 0) {
		return rc;
	}

	assert(req->iovcnt <= rqpair->max_send_sge);

	rdma_req->iovpos = 0;

	if (spdk_unlikely(req->dif_enabled)) {
		num_wrs = nvmf_rdma_calc_num_wrs(length, rtransport->transport.opts.io_unit_size,
						 req->dif.dif_ctx.block_size);
		if (num_wrs > 1) {
			rc = nvmf_request_alloc_wrs(rtransport, rdma_req, num_wrs - 1);
			if (rc != 0) {
				goto err_exit;
			}
		}
	}

	rc = nvmf_rdma_fill_wr_sgl(rgroup, device, rdma_req, wr, length, num_wrs - 1);
	if (spdk_unlikely(rc != 0)) {
		goto err_exit;
	}

	if (spdk_unlikely(num_wrs > 1)) {
		nvmf_rdma_update_remote_addr(rdma_req, num_wrs);
	}

	/* set the number of outstanding data WRs for this request. */
	rdma_req->num_outstanding_data_wr = num_wrs;

	return rc;

err_exit:
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	req->iovcnt = 0;
	return rc;
}

static int
nvmf_rdma_request_fill_iovs_multi_sgl(struct spdk_nvmf_rdma_transport *rtransport,
				      struct spdk_nvmf_rdma_device *device,
				      struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct ibv_send_wr			*current_wr;
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct spdk_nvme_sgl_descriptor		*inline_segment, *desc;
	uint32_t				num_sgl_descriptors;
	uint32_t				lengths[SPDK_NVMF_MAX_SGL_ENTRIES], total_length = 0;
	uint32_t				i;
	int					rc;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	inline_segment = &req->cmd->nvme_cmd.dptr.sgl1;
	assert(inline_segment->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	assert(inline_segment->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);

	num_sgl_descriptors = inline_segment->unkeyed.length / sizeof(struct spdk_nvme_sgl_descriptor);
	assert(num_sgl_descriptors <= SPDK_NVMF_MAX_SGL_ENTRIES);

	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		if (spdk_likely(!req->dif_enabled)) {
			lengths[i] = desc->keyed.length;
		} else {
			req->dif.orig_length += desc->keyed.length;
			lengths[i] = spdk_dif_get_length_with_md(desc->keyed.length, &req->dif.dif_ctx);
			req->dif.elba_length += lengths[i];
		}
		total_length += lengths[i];
		desc++;
	}

	if (total_length > rtransport->transport.opts.max_io_size) {
		SPDK_ERRLOG("Multi SGL length 0x%x exceeds max io size 0x%x\n",
			    total_length, rtransport->transport.opts.max_io_size);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return -EINVAL;
	}

	if (nvmf_request_alloc_wrs(rtransport, rdma_req, num_sgl_descriptors - 1) != 0) {
		return -ENOMEM;
	}

	rc = spdk_nvmf_request_get_buffers(req, &rgroup->group, &rtransport->transport, total_length);
	if (rc != 0) {
		nvmf_rdma_request_free_data(rdma_req, rtransport);
		return rc;
	}

	/* The first WR must always be the embedded data WR. This is how we unwind them later. */
	current_wr = &rdma_req->data.wr;
	assert(current_wr != NULL);

	req->length = 0;
	rdma_req->iovpos = 0;
	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		/* The descriptors must be keyed data block descriptors with an address, not an offset. */
		if (spdk_unlikely(desc->generic.type != SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK ||
				  desc->keyed.subtype != SPDK_NVME_SGL_SUBTYPE_ADDRESS)) {
			rc = -EINVAL;
			goto err_exit;
		}

		rc = nvmf_rdma_fill_wr_sgl(rgroup, device, rdma_req, current_wr, lengths[i], 0);
		if (rc != 0) {
			rc = -ENOMEM;
			goto err_exit;
		}

		req->length += desc->keyed.length;
		current_wr->wr.rdma.rkey = desc->keyed.key;
		current_wr->wr.rdma.remote_addr = desc->address;
		current_wr = current_wr->next;
		desc++;
	}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	/* Go back to the last descriptor in the list. */
	desc--;
	if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
		if (desc->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
			rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
			rdma_req->rsp.wr.imm_data = desc->keyed.key;
		}
	}
#endif

	rdma_req->num_outstanding_data_wr = num_sgl_descriptors;

	return 0;

err_exit:
	spdk_nvmf_request_free_buffers(req, &rgroup->group, &rtransport->transport);
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	return rc;
}

static int
nvmf_rdma_request_parse_sgl(struct spdk_nvmf_rdma_transport *rtransport,
			    struct spdk_nvmf_rdma_device *device,
			    struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_request		*req = &rdma_req->req;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;
	int					rc;
	uint32_t				length;

	rsp = &req->rsp->nvme_cpl;
	sgl = &req->cmd->nvme_cmd.dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {

		length = sgl->keyed.length;
		if (length > rtransport->transport.opts.max_io_size) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    length, rtransport->transport.opts.max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}
#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
		if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
			if (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY) {
				rdma_req->rsp.wr.opcode = IBV_WR_SEND_WITH_INV;
				rdma_req->rsp.wr.imm_data = sgl->keyed.key;
			}
		}
#endif

		/* fill request length and populate iovs */
		req->length = length;

		if (spdk_unlikely(req->dif_enabled)) {
			req->dif.orig_length = length;
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		rc = nvmf_rdma_request_fill_iovs(rtransport, device, rdma_req, length);
		if (spdk_unlikely(rc < 0)) {
			if (rc == -EINVAL) {
				SPDK_ERRLOG("SGL length exceeds the max I/O size\n");
				rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				return -1;
			}
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(rdma, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		}

		/* backward compatible */
		req->data = req->iov[0].iov_base;

		SPDK_DEBUGLOG(rdma, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = rtransport->transport.opts.in_capsule_data_size;

		SPDK_DEBUGLOG(nvmf, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
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

		rdma_req->num_outstanding_data_wr = 0;
		req->data = rdma_req->recv->buf + offset;
		req->data_from_pool = false;
		req->length = sgl->unkeyed.length;

		req->iov[0].iov_base = req->data;
		req->iov[0].iov_len = req->length;
		req->iovcnt = 1;

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {

		rc = nvmf_rdma_request_fill_iovs_multi_sgl(rtransport, device, rdma_req);
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(rdma, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		} else if (rc == -EINVAL) {
			SPDK_ERRLOG("Multi SGL element request length exceeds the max I/O size\n");
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		/* backward compatible */
		req->data = req->iov[0].iov_base;

		SPDK_DEBUGLOG(rdma, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static void
_nvmf_rdma_request_free(struct spdk_nvmf_rdma_request *rdma_req,
			struct spdk_nvmf_rdma_transport	*rtransport)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	if (rdma_req->req.data_from_pool) {
		rgroup = rqpair->poller->group;

		spdk_nvmf_request_free_buffers(&rdma_req->req, &rgroup->group, &rtransport->transport);
	}
	nvmf_rdma_request_free_data(rdma_req, rtransport);
	rdma_req->req.length = 0;
	rdma_req->req.iovcnt = 0;
	rdma_req->req.data = NULL;
	rdma_req->offset = 0;
	memset(&rdma_req->req.dif, 0, sizeof(rdma_req->req.dif));
	rqpair->qd--;

	STAILQ_INSERT_HEAD(&rqpair->resources->free_queue, rdma_req, state_link);
	rdma_req->state = RDMA_REQUEST_STATE_FREE;
}

bool
nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
			  struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvme_cpl		*rsp = &rdma_req->req.rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	enum spdk_nvmf_rdma_request_state prev_state;
	bool				progress = false;
	int				data_posted;
	uint32_t			num_blocks;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->device;
	rgroup = rqpair->poller->group;

	assert(rdma_req->state != RDMA_REQUEST_STATE_FREE);

	/* If the queue pair is in an error state, force the request to the completed state
	 * to release resources. */
	if (rqpair->ibv_state == IBV_QPS_ERR || rqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
		if (rdma_req->state == RDMA_REQUEST_STATE_NEED_BUFFER) {
			STAILQ_REMOVE(&rgroup->group.pending_buf_queue, &rdma_req->req, spdk_nvmf_request, buf_link);
		} else if (rdma_req->state == RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING) {
			STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
		} else if (rdma_req->state == RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING) {
			STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
		}
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	}

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = rdma_req->state;

		SPDK_DEBUGLOG(rdma, "Request %p entering state %d\n", rdma_req, prev_state);

		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEW, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			rdma_recv = rdma_req->recv;

			/* The first element of the SGL is the NVMe command */
			rdma_req->req.cmd = (union nvmf_h2c_msg *)rdma_recv->sgl[0].addr;
			memset(rdma_req->req.rsp, 0, sizeof(*rdma_req->req.rsp));

			if (rqpair->ibv_state == IBV_QPS_ERR  || rqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
				break;
			}

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&rdma_req->req, &rdma_req->req.dif.dif_ctx))) {
				rdma_req->req.dif_enabled = true;
			}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
			rdma_req->rsp.wr.opcode = IBV_WR_SEND;
			rdma_req->rsp.wr.imm_data = 0;
#endif

			/* The next state transition depends on the data transfer needs of this request. */
			rdma_req->req.xfer = spdk_nvmf_req_get_xfer(&rdma_req->req);

			if (spdk_unlikely(rdma_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
				SPDK_DEBUGLOG(rdma, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", rdma_req);
				break;
			}

			/* If no data to transfer, ready to execute. */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_NONE) {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_NEED_BUFFER;
			STAILQ_INSERT_TAIL(&rgroup->group.pending_buf_queue, &rdma_req->req, buf_link);
			break;
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEED_BUFFER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			assert(rdma_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (&rdma_req->req != STAILQ_FIRST(&rgroup->group.pending_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
			if (rc < 0) {
				STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
				break;
			}

			if (!rdma_req->req.data) {
				/* No buffers available. */
				rgroup->stat.pending_data_buffer++;
				break;
			}

			STAILQ_REMOVE_HEAD(&rgroup->group.pending_buf_queue, buf_link);

			/* If data is transferring from host to controller and the data didn't
			 * arrive using in capsule data, we need to do a transfer from the host.
			 */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
			    rdma_req->req.data_from_pool) {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_read_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING;
				break;
			}

			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_read_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			if (rqpair->current_send_depth + rdma_req->num_outstanding_data_wr > rqpair->max_send_depth
			    || rqpair->current_read_depth + rdma_req->num_outstanding_data_wr > rqpair->max_read_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish. */
				rqpair->poller->stat.pending_rdma_read++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_read_queue, state_link);

			rc = request_transfer_in(&rdma_req->req);
			if (!rc) {
				rdma_req->state = RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER;
			} else {
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (spdk_unlikely(rdma_req->req.dif_enabled)) {
				if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
					/* generate DIF for write operation */
					num_blocks = SPDK_CEIL_DIV(rdma_req->req.dif.elba_length, rdma_req->req.dif.dif_ctx.block_size);
					assert(num_blocks > 0);

					rc = spdk_dif_generate(rdma_req->req.iov, rdma_req->req.iovcnt,
							       num_blocks, &rdma_req->req.dif.dif_ctx);
					if (rc != 0) {
						SPDK_ERRLOG("DIF generation failed\n");
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
						break;
					}
				}

				assert(rdma_req->req.dif.elba_length >= rdma_req->req.length);
				/* set extended length before IO operation */
				rdma_req->req.length = rdma_req->req.dif.elba_length;
			}

			rdma_req->state = RDMA_REQUEST_STATE_EXECUTING;
			spdk_nvmf_request_exec(&rdma_req->req);
			break;
		case RDMA_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_EXECUTING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_EXECUTED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
			    rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_write_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING;
			} else {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			}
			if (spdk_unlikely(rdma_req->req.dif_enabled)) {
				/* restore the original length */
				rdma_req->req.length = rdma_req->req.dif.orig_length;

				if (rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
					struct spdk_dif_error error_blk;

					num_blocks = SPDK_CEIL_DIV(rdma_req->req.dif.elba_length, rdma_req->req.dif.dif_ctx.block_size);

					rc = spdk_dif_verify(rdma_req->req.iov, rdma_req->req.iovcnt, num_blocks,
							     &rdma_req->req.dif.dif_ctx, &error_blk);
					if (rc) {
						struct spdk_nvme_cpl *rsp = &rdma_req->req.rsp->nvme_cpl;

						SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n", error_blk.err_type,
							    error_blk.err_offset);
						rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
						rsp->status.sc = nvmf_rdma_dif_error_to_compl_status(error_blk.err_type);
						rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
						STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
					}
				}
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			if (rdma_req != STAILQ_FIRST(&rqpair->pending_rdma_write_queue)) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			if ((rqpair->current_send_depth + rdma_req->num_outstanding_data_wr + 1) >
			    rqpair->max_send_depth) {
				/* We can only have so many WRs outstanding. we have to wait until some finish.
				 * +1 since each request has an additional wr in the resp. */
				rqpair->poller->stat.pending_rdma_write++;
				break;
			}

			/* We have already verified that this request is the head of the queue. */
			STAILQ_REMOVE_HEAD(&rqpair->pending_rdma_write_queue, state_link);

			/* The data transfer will be kicked off from
			 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
			 */
			rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			rc = request_transfer_out(&rdma_req->req, &data_posted);
			assert(rc == 0); /* No good way to handle this currently */
			if (rc) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			} else {
				rdma_req->state = data_posted ? RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST :
						  RDMA_REQUEST_STATE_COMPLETING;
			}
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair);

			rqpair->poller->stat.request_latency += spdk_get_ticks() - rdma_req->receive_tsc;
			_nvmf_rdma_request_free(rdma_req, rtransport);
			break;
		case RDMA_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		if (rdma_req->state != prev_state) {
			progress = true;
		}
	} while (rdma_req->state != prev_state);

	return progress;
}

/* Public API callbacks begin here */

#define SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR 128
#define SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE (SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE / SPDK_NVMF_MAX_SGL_ENTRIES)
#define SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS 4095
#define SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE 32
#define SPDK_NVMF_RDMA_DEFAULT_NO_SRQ false
#define SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG 100
#define SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC 1
#define SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING false

static void
nvmf_rdma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_RDMA_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->transport_specific =      NULL;
}

static int nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
			     spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg);

static inline bool
nvmf_rdma_is_rxe_device(struct spdk_nvmf_rdma_device *device)
{
	return device->attr.vendor_id == SPDK_RDMA_RXE_VENDOR_ID_OLD ||
	       device->attr.vendor_id == SPDK_RDMA_RXE_VENDOR_ID_NEW;
}

static int
nvmf_rdma_accept(void *ctx);

static struct spdk_nvmf_transport *
nvmf_rdma_create(struct spdk_nvmf_transport_opts *opts)
{
	int rc;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device	*device, *tmp;
	struct ibv_context		**contexts;
	uint32_t			i;
	int				flag;
	uint32_t			sge_count;
	uint32_t			min_shared_buffers;
	uint32_t			min_in_capsule_data_size;
	int				max_device_sge = SPDK_NVMF_MAX_SGL_ENTRIES;
	pthread_mutexattr_t		attr;

	rtransport = calloc(1, sizeof(*rtransport));
	if (!rtransport) {
		return NULL;
	}

	if (pthread_mutexattr_init(&attr)) {
		SPDK_ERRLOG("pthread_mutexattr_init() failed\n");
		free(rtransport);
		return NULL;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		SPDK_ERRLOG("pthread_mutexattr_settype() failed\n");
		pthread_mutexattr_destroy(&attr);
		free(rtransport);
		return NULL;
	}

	if (pthread_mutex_init(&rtransport->lock, &attr)) {
		SPDK_ERRLOG("pthread_mutex_init() failed\n");
		pthread_mutexattr_destroy(&attr);
		free(rtransport);
		return NULL;
	}

	pthread_mutexattr_destroy(&attr);

	TAILQ_INIT(&rtransport->devices);
	TAILQ_INIT(&rtransport->ports);
	TAILQ_INIT(&rtransport->poll_groups);

	rtransport->transport.ops = &spdk_nvmf_transport_rdma;
	rtransport->rdma_opts.num_cqe = DEFAULT_NVMF_RDMA_CQ_SIZE;
	rtransport->rdma_opts.max_srq_depth = SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH;
	rtransport->rdma_opts.no_srq = SPDK_NVMF_RDMA_DEFAULT_NO_SRQ;
	rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	rtransport->rdma_opts.no_wr_batching = SPDK_NVMF_RDMA_DEFAULT_NO_WR_BATCHING;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, rdma_transport_opts_decoder,
					    SPDK_COUNTOF(rdma_transport_opts_decoder),
					    &rtransport->rdma_opts)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	SPDK_INFOLOG(rdma, "*** RDMA Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, num_cqe=%d, max_srq_depth=%d, no_srq=%d,"
		     "  acceptor_backlog=%d, no_wr_batching=%d abort_timeout_sec=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     rtransport->rdma_opts.num_cqe,
		     rtransport->rdma_opts.max_srq_depth,
		     rtransport->rdma_opts.no_srq,
		     rtransport->rdma_opts.acceptor_backlog,
		     rtransport->rdma_opts.no_wr_batching,
		     opts->abort_timeout_sec);

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	if (rtransport->rdma_opts.acceptor_backlog <= 0) {
		SPDK_ERRLOG("The acceptor backlog cannot be less than 1, setting to the default value of (%d).\n",
			    SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG);
		rtransport->rdma_opts.acceptor_backlog = SPDK_NVMF_RDMA_ACCEPTOR_BACKLOG;
	}

	if (opts->num_shared_buffers < (SPDK_NVMF_MAX_SGL_ENTRIES * 2)) {
		SPDK_ERRLOG("The number of shared data buffers (%d) is less than"
			    "the minimum number required to guarantee that forward progress can be made (%d)\n",
			    opts->num_shared_buffers, (SPDK_NVMF_MAX_SGL_ENTRIES * 2));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	min_shared_buffers = spdk_env_get_core_count() * opts->buf_cache_size;
	if (min_shared_buffers > opts->num_shared_buffers) {
		SPDK_ERRLOG("There are not enough buffers to satisfy"
			    "per-poll group caches for each thread. (%" PRIu32 ")"
			    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
		SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > NVMF_DEFAULT_TX_SGE) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	min_in_capsule_data_size = sizeof(struct spdk_nvme_sgl_descriptor) * SPDK_NVMF_MAX_SGL_ENTRIES;
	if (opts->in_capsule_data_size < min_in_capsule_data_size) {
		SPDK_WARNLOG("In capsule data size is set to %u, this is minimum size required to support msdbd=16\n",
			     min_in_capsule_data_size);
		opts->in_capsule_data_size = min_in_capsule_data_size;
	}

	rtransport->event_channel = rdma_create_event_channel();
	if (rtransport->event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed, %s\n", spdk_strerror(errno));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	flag = fcntl(rtransport->event_channel->fd, F_GETFL);
	if (fcntl(rtransport->event_channel->fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%s)\n",
			    rtransport->event_channel->fd, spdk_strerror(errno));
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rtransport->data_wr_pool = spdk_mempool_create("spdk_nvmf_rdma_wr_data",
				   opts->max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES,
				   sizeof(struct spdk_nvmf_rdma_request_data),
				   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_SOCKET_ID_ANY);
	if (!rtransport->data_wr_pool) {
		SPDK_ERRLOG("Unable to allocate work request pool for poll group\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	contexts = rdma_get_devices(NULL);
	if (contexts == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

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

		max_device_sge = spdk_min(max_device_sge, device->attr.max_sge);

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
		if ((device->attr.device_cap_flags & IBV_DEVICE_MEM_MGT_EXTENSIONS) == 0) {
			SPDK_WARNLOG("The libibverbs on this system supports SEND_WITH_INVALIDATE,");
			SPDK_WARNLOG("but the device with vendor ID %u does not.\n", device->attr.vendor_id);
		}

		/**
		 * The vendor ID is assigned by the IEEE and an ID of 0 implies Soft-RoCE.
		 * The Soft-RoCE RXE driver does not currently support send with invalidate,
		 * but incorrectly reports that it does. There are changes making their way
		 * through the kernel now that will enable this feature. When they are merged,
		 * we can conditionally enable this feature.
		 *
		 * TODO: enable this for versions of the kernel rxe driver that support it.
		 */
		if (nvmf_rdma_is_rxe_device(device)) {
			device->attr.device_cap_flags &= ~(IBV_DEVICE_MEM_MGT_EXTENSIONS);
		}
#endif

		/* set up device context async ev fd as NON_BLOCKING */
		flag = fcntl(device->context->async_fd, F_GETFL);
		rc = fcntl(device->context->async_fd, F_SETFL, flag | O_NONBLOCK);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to set context async fd to NONBLOCK.\n");
			free(device);
			break;
		}

		TAILQ_INSERT_TAIL(&rtransport->devices, device, link);
		i++;

		if (g_nvmf_hooks.get_ibv_pd) {
			device->pd = g_nvmf_hooks.get_ibv_pd(NULL, device->context);
		} else {
			device->pd = ibv_alloc_pd(device->context);
		}

		if (!device->pd) {
			SPDK_ERRLOG("Unable to allocate protection domain.\n");
			rc = -ENOMEM;
			break;
		}

		assert(device->map == NULL);

		device->map = spdk_rdma_create_mem_map(device->pd, &g_nvmf_hooks, SPDK_RDMA_MEMORY_MAP_ROLE_TARGET);
		if (!device->map) {
			SPDK_ERRLOG("Unable to allocate memory map for listen address\n");
			rc = -ENOMEM;
			break;
		}

		assert(device->map != NULL);
		assert(device->pd != NULL);
	}
	rdma_free_devices(contexts);

	if (opts->io_unit_size * max_device_sge < opts->max_io_size) {
		/* divide and round up. */
		opts->io_unit_size = (opts->max_io_size + max_device_sge - 1) / max_device_sge;

		/* round up to the nearest 4k. */
		opts->io_unit_size = (opts->io_unit_size + NVMF_DATA_BUFFER_ALIGNMENT - 1) & ~NVMF_DATA_BUFFER_MASK;

		opts->io_unit_size = spdk_max(opts->io_unit_size, SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE);
		SPDK_NOTICELOG("Adjusting the io unit size to fit the device's maximum I/O size. New I/O unit size %u\n",
			       opts->io_unit_size);
	}

	if (rc < 0) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	/* Set up poll descriptor array to monitor events from RDMA and IB
	 * in a single poll syscall
	 */
	rtransport->npoll_fds = i + 1;
	i = 0;
	rtransport->poll_fds = calloc(rtransport->npoll_fds, sizeof(struct pollfd));
	if (rtransport->poll_fds == NULL) {
		SPDK_ERRLOG("poll_fds allocation failed\n");
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	rtransport->poll_fds[i].fd = rtransport->event_channel->fd;
	rtransport->poll_fds[i++].events = POLLIN;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		rtransport->poll_fds[i].fd = device->context->async_fd;
		rtransport->poll_fds[i++].events = POLLIN;
	}

	rtransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_rdma_accept, &rtransport->transport,
				    opts->acceptor_poll_rate);
	if (!rtransport->accept_poller) {
		nvmf_rdma_destroy(&rtransport->transport, NULL, NULL);
		return NULL;
	}

	return &rtransport->transport;
}

static void
nvmf_rdma_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	assert(w != NULL);

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	spdk_json_write_named_uint32(w, "max_srq_depth", rtransport->rdma_opts.max_srq_depth);
	spdk_json_write_named_bool(w, "no_srq", rtransport->rdma_opts.no_srq);
	if (rtransport->rdma_opts.no_srq == true) {
		spdk_json_write_named_int32(w, "num_cqe", rtransport->rdma_opts.num_cqe);
	}
	spdk_json_write_named_int32(w, "acceptor_backlog", rtransport->rdma_opts.acceptor_backlog);
	spdk_json_write_named_bool(w, "no_wr_batching", rtransport->rdma_opts.no_wr_batching);
}

static int
nvmf_rdma_destroy(struct spdk_nvmf_transport *transport,
		  spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
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

	if (rtransport->poll_fds != NULL) {
		free(rtransport->poll_fds);
	}

	if (rtransport->event_channel != NULL) {
		rdma_destroy_event_channel(rtransport->event_channel);
	}

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		TAILQ_REMOVE(&rtransport->devices, device, link);
		spdk_rdma_free_mem_map(&device->map);
		if (device->pd) {
			if (!g_nvmf_hooks.get_ibv_pd) {
				ibv_dealloc_pd(device->pd);
			}
		}
		free(device);
	}

	if (rtransport->data_wr_pool != NULL) {
		if (spdk_mempool_count(rtransport->data_wr_pool) !=
		    (transport->opts.max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES)) {
			SPDK_ERRLOG("transport wr pool count is %zu but should be %u\n",
				    spdk_mempool_count(rtransport->data_wr_pool),
				    transport->opts.max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES);
		}
	}

	spdk_mempool_free(rtransport->data_wr_pool);

	spdk_poller_unregister(&rtransport->accept_poller);
	pthread_mutex_destroy(&rtransport->lock);
	free(rtransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
	return 0;
}

static int
nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
			  struct spdk_nvme_transport_id *trid,
			  bool peer);

static int
nvmf_rdma_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvmf_listen_opts *listen_opts)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_port	*port;
	struct addrinfo			*res;
	struct addrinfo			hints;
	int				family;
	int				rc;

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -EINVAL;
	}

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	assert(rtransport->event_channel != NULL);

	pthread_mutex_lock(&rtransport->lock);
	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		pthread_mutex_unlock(&rtransport->lock);
		return -ENOMEM;
	}

	port->trid = trid;

	switch (trid->adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", trid->adrfam);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return -EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	rc = getaddrinfo(trid->traddr, trid->trsvcid, &hints, &res);
	if (rc) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(rc), rc);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return -EINVAL;
	}

	rc = rdma_create_id(rtransport->event_channel, &port->id, port, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		freeaddrinfo(res);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return rc;
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

	if (!port->id->verbs) {
		SPDK_ERRLOG("ibv_context is null\n");
		rdma_destroy_id(port->id);
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return -1;
	}

	rc = rdma_listen(port->id, rtransport->rdma_opts.acceptor_backlog);
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

	SPDK_NOTICELOG("*** NVMe/RDMA Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);
	pthread_mutex_unlock(&rtransport->lock);
	return 0;
}

static void
nvmf_rdma_stop_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_port *port, *tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	pthread_mutex_lock(&rtransport->lock);
	TAILQ_FOREACH_SAFE(port, &rtransport->ports, link, tmp) {
		if (spdk_nvme_transport_id_compare(port->trid, trid) == 0) {
			TAILQ_REMOVE(&rtransport->ports, port, link);
			rdma_destroy_id(port->id);
			free(port);
			break;
		}
	}

	pthread_mutex_unlock(&rtransport->lock);
}

static void
nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
				struct spdk_nvmf_rdma_qpair *rqpair, bool drain)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;
	struct spdk_nvmf_rdma_resources *resources;

	/* We process I/O in the data transfer pending queue at the highest priority. RDMA reads first */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_read_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then RDMA writes since reads have stronger restrictions than writes */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_write_queue, state_link, req_tmp) {
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then we handle request waiting on memory buffers. */
	STAILQ_FOREACH_SAFE(req, &rqpair->poller->group->group.pending_buf_queue, buf_link, tmp) {
		rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	resources = rqpair->resources;
	while (!STAILQ_EMPTY(&resources->free_queue) && !STAILQ_EMPTY(&resources->incoming_queue)) {
		rdma_req = STAILQ_FIRST(&resources->free_queue);
		STAILQ_REMOVE_HEAD(&resources->free_queue, state_link);
		rdma_req->recv = STAILQ_FIRST(&resources->incoming_queue);
		STAILQ_REMOVE_HEAD(&resources->incoming_queue, link);

		if (rqpair->srq != NULL) {
			rdma_req->req.qpair = &rdma_req->recv->qpair->qpair;
			rdma_req->recv->qpair->qd++;
		} else {
			rqpair->qd++;
		}

		rdma_req->receive_tsc = rdma_req->recv->receive_tsc;
		rdma_req->state = RDMA_REQUEST_STATE_NEW;
		if (nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
	if (!STAILQ_EMPTY(&resources->incoming_queue) && STAILQ_EMPTY(&resources->free_queue)) {
		rqpair->poller->stat.pending_free_request++;
	}
}

static inline bool
nvmf_rdma_can_ignore_last_wqe_reached(struct spdk_nvmf_rdma_device *device)
{
	/* iWARP transport and SoftRoCE driver don't support LAST_WQE_REACHED ibv async event */
	return nvmf_rdma_is_rxe_device(device) ||
	       device->context->device->transport_type == IBV_TRANSPORT_IWARP;
}

static void
nvmf_rdma_destroy_drained_qpair(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	nvmf_rdma_qpair_process_pending(rtransport, rqpair, true);

	/* nvmr_rdma_close_qpair is not called */
	if (!rqpair->to_close) {
		return;
	}

	/* In non SRQ path, we will reach rqpair->max_queue_depth. In SRQ path, we will get the last_wqe event. */
	if (rqpair->current_send_depth != 0) {
		return;
	}

	if (rqpair->srq == NULL && rqpair->current_recv_depth != rqpair->max_queue_depth) {
		return;
	}

	if (rqpair->srq != NULL && rqpair->last_wqe_reached == false &&
	    !nvmf_rdma_can_ignore_last_wqe_reached(rqpair->device)) {
		return;
	}

	assert(rqpair->qpair.state == SPDK_NVMF_QPAIR_ERROR);

	nvmf_rdma_qpair_destroy(rqpair);
}

static int
nvmf_rdma_disconnect(struct rdma_cm_event *evt)
{
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;

	if (evt->id == NULL) {
		SPDK_ERRLOG("disconnect request: missing cm_id\n");
		return -1;
	}

	qpair = evt->id->context;
	if (qpair == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	spdk_trace_record(TRACE_RDMA_QP_DISCONNECT, 0, 0, (uintptr_t)rqpair);

	spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);

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

static void
nvmf_rdma_disconnect_qpairs_on_port(struct spdk_nvmf_rdma_transport *rtransport,
				    struct spdk_nvmf_rdma_port *port)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_rdma_qpair		*rqpair;

	TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
		TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
			TAILQ_FOREACH(rqpair, &rpoller->qpairs, link) {
				if (rqpair->listen_id == port->id) {
					spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
				}
			}
		}
	}
}

static bool
nvmf_rdma_handle_cm_event_addr_change(struct spdk_nvmf_transport *transport,
				      struct rdma_cm_event *event)
{
	const struct spdk_nvme_transport_id	*trid;
	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_transport		*rtransport;
	bool					event_acked = false;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	TAILQ_FOREACH(port, &rtransport->ports, link) {
		if (port->id == event->id) {
			SPDK_ERRLOG("ADDR_CHANGE: IP %s:%s migrated\n", port->trid->traddr, port->trid->trsvcid);
			rdma_ack_cm_event(event);
			event_acked = true;
			trid = port->trid;
			break;
		}
	}

	if (event_acked) {
		nvmf_rdma_disconnect_qpairs_on_port(rtransport, port);

		nvmf_rdma_stop_listen(transport, trid);
		nvmf_rdma_listen(transport, trid, NULL);
	}

	return event_acked;
}

static void
nvmf_rdma_handle_cm_event_port_removal(struct spdk_nvmf_transport *transport,
				       struct rdma_cm_event *event)
{
	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_transport		*rtransport;

	port = event->id->context;
	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	SPDK_NOTICELOG("Port %s:%s is being removed\n", port->trid->traddr, port->trid->trsvcid);

	nvmf_rdma_disconnect_qpairs_on_port(rtransport, port);

	rdma_ack_cm_event(event);

	while (spdk_nvmf_transport_stop_listen(transport, port->trid) == 0) {
		;
	}
}

static void
nvmf_process_cm_event(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct rdma_cm_event		*event;
	int				rc;
	bool				event_acked;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (rtransport->event_channel == NULL) {
		return;
	}

	while (1) {
		event_acked = false;
		rc = rdma_get_cm_event(rtransport->event_channel, &event);
		if (rc) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("Acceptor Event Error: %s\n", spdk_strerror(errno));
			}
			break;
		}

		SPDK_DEBUGLOG(rdma, "Acceptor Event: %s\n", CM_EVENT_STR[event->event]);

		spdk_trace_record(TRACE_RDMA_CM_ASYNC_EVENT, 0, 0, 0, event->event);

		switch (event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
		case RDMA_CM_EVENT_ROUTE_ERROR:
			/* No action required. The target never attempts to resolve routes. */
			break;
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			rc = nvmf_rdma_connect(transport, event);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process connect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_CONNECT_RESPONSE:
			/* The target never initiates a new connection. So this will not occur. */
			break;
		case RDMA_CM_EVENT_CONNECT_ERROR:
			/* Can this happen? The docs say it can, but not sure what causes it. */
			break;
		case RDMA_CM_EVENT_UNREACHABLE:
		case RDMA_CM_EVENT_REJECTED:
			/* These only occur on the client side. */
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			/* TODO: Should we be waiting for this event anywhere? */
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
			rc = nvmf_rdma_disconnect(event);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
				break;
			}
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			/* In case of device removal, kernel IB part triggers IBV_EVENT_DEVICE_FATAL
			 * which triggers RDMA_CM_EVENT_DEVICE_REMOVAL on all cma_id’s.
			 * Once these events are sent to SPDK, we should release all IB resources and
			 * don't make attempts to call any ibv_query/modify/create functions. We can only call
			 * ibv_destroy* functions to release user space memory allocated by IB. All kernel
			 * resources are already cleaned. */
			if (event->id->qp) {
				/* If rdma_cm event has a valid `qp` pointer then the event refers to the
				 * corresponding qpair. Otherwise the event refers to a listening device */
				rc = nvmf_rdma_disconnect(event);
				if (rc < 0) {
					SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
					break;
				}
			} else {
				nvmf_rdma_handle_cm_event_port_removal(transport, event);
				event_acked = true;
			}
			break;
		case RDMA_CM_EVENT_MULTICAST_JOIN:
		case RDMA_CM_EVENT_MULTICAST_ERROR:
			/* Multicast is not used */
			break;
		case RDMA_CM_EVENT_ADDR_CHANGE:
			event_acked = nvmf_rdma_handle_cm_event_addr_change(transport, event);
			break;
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			/* For now, do nothing. The target never re-uses queue pairs. */
			break;
		default:
			SPDK_ERRLOG("Unexpected Acceptor Event [%d]\n", event->event);
			break;
		}
		if (!event_acked) {
			rdma_ack_cm_event(event);
		}
	}
}

static void
nvmf_rdma_handle_last_wqe_reached(struct spdk_nvmf_rdma_qpair *rqpair)
{
	rqpair->last_wqe_reached = true;
	nvmf_rdma_destroy_drained_qpair(rqpair);
}

static void
nvmf_rdma_qpair_process_ibv_event(void *ctx)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *event_ctx = ctx;

	if (event_ctx->rqpair) {
		STAILQ_REMOVE(&event_ctx->rqpair->ibv_events, event_ctx, spdk_nvmf_rdma_ibv_event_ctx, link);
		if (event_ctx->cb_fn) {
			event_ctx->cb_fn(event_ctx->rqpair);
		}
	}
	free(event_ctx);
}

static int
nvmf_rdma_send_qpair_async_event(struct spdk_nvmf_rdma_qpair *rqpair,
				 spdk_nvmf_rdma_qpair_ibv_event fn)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;
	struct spdk_thread *thr = NULL;
	int rc;

	if (rqpair->qpair.group) {
		thr = rqpair->qpair.group->thread;
	} else if (rqpair->destruct_channel) {
		thr = spdk_io_channel_get_thread(rqpair->destruct_channel);
	}

	if (!thr) {
		SPDK_DEBUGLOG(rdma, "rqpair %p has no thread\n", rqpair);
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->rqpair = rqpair;
	ctx->cb_fn = fn;
	STAILQ_INSERT_TAIL(&rqpair->ibv_events, ctx, link);

	rc = spdk_thread_send_msg(thr, nvmf_rdma_qpair_process_ibv_event, ctx);
	if (rc) {
		STAILQ_REMOVE(&rqpair->ibv_events, ctx, spdk_nvmf_rdma_ibv_event_ctx, link);
		free(ctx);
	}

	return rc;
}

static int
nvmf_process_ib_event(struct spdk_nvmf_rdma_device *device)
{
	int				rc;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct ibv_async_event		event;

	rc = ibv_get_async_event(device->context, &event);

	if (rc) {
		/* In non-blocking mode -1 means there are no events available */
		return rc;
	}

	switch (event.event_type) {
	case IBV_EVENT_QP_FATAL:
		rqpair = event.element.qp->qp_context;
		SPDK_ERRLOG("Fatal event received for rqpair %p\n", rqpair);
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair, event.event_type);
		nvmf_rdma_update_ibv_state(rqpair);
		spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
		break;
	case IBV_EVENT_QP_LAST_WQE_REACHED:
		/* This event only occurs for shared receive queues. */
		rqpair = event.element.qp->qp_context;
		SPDK_DEBUGLOG(rdma, "Last WQE reached event received for rqpair %p\n", rqpair);
		rc = nvmf_rdma_send_qpair_async_event(rqpair, nvmf_rdma_handle_last_wqe_reached);
		if (rc) {
			SPDK_WARNLOG("Failed to send LAST_WQE_REACHED event. rqpair %p, err %d\n", rqpair, rc);
			rqpair->last_wqe_reached = true;
		}
		break;
	case IBV_EVENT_SQ_DRAINED:
		/* This event occurs frequently in both error and non-error states.
		 * Check if the qpair is in an error state before sending a message. */
		rqpair = event.element.qp->qp_context;
		SPDK_DEBUGLOG(rdma, "Last sq drained event received for rqpair %p\n", rqpair);
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair, event.event_type);
		if (nvmf_rdma_update_ibv_state(rqpair) == IBV_QPS_ERR) {
			spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
		}
		break;
	case IBV_EVENT_QP_REQ_ERR:
	case IBV_EVENT_QP_ACCESS_ERR:
	case IBV_EVENT_COMM_EST:
	case IBV_EVENT_PATH_MIG:
	case IBV_EVENT_PATH_MIG_ERR:
		SPDK_NOTICELOG("Async event: %s\n",
			       ibv_event_type_str(event.event_type));
		rqpair = event.element.qp->qp_context;
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair, event.event_type);
		nvmf_rdma_update_ibv_state(rqpair);
		break;
	case IBV_EVENT_CQ_ERR:
	case IBV_EVENT_DEVICE_FATAL:
	case IBV_EVENT_PORT_ACTIVE:
	case IBV_EVENT_PORT_ERR:
	case IBV_EVENT_LID_CHANGE:
	case IBV_EVENT_PKEY_CHANGE:
	case IBV_EVENT_SM_CHANGE:
	case IBV_EVENT_SRQ_ERR:
	case IBV_EVENT_SRQ_LIMIT_REACHED:
	case IBV_EVENT_CLIENT_REREGISTER:
	case IBV_EVENT_GID_CHANGE:
	default:
		SPDK_NOTICELOG("Async event: %s\n",
			       ibv_event_type_str(event.event_type));
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0, 0, event.event_type);
		break;
	}
	ibv_ack_async_event(&event);

	return 0;
}

static void
nvmf_process_ib_events(struct spdk_nvmf_rdma_device *device, uint32_t max_events)
{
	int rc = 0;
	uint32_t i = 0;

	for (i = 0; i < max_events; i++) {
		rc = nvmf_process_ib_event(device);
		if (rc) {
			break;
		}
	}

	SPDK_DEBUGLOG(rdma, "Device %s: %u events processed\n", device->context->device->name, i);
}

static int
nvmf_rdma_accept(void *ctx)
{
	int	nfds, i = 0;
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device *device, *tmp;
	uint32_t count;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	count = nfds = poll(rtransport->poll_fds, rtransport->npoll_fds, 0);

	if (nfds <= 0) {
		return SPDK_POLLER_IDLE;
	}

	/* The first poll descriptor is RDMA CM event */
	if (rtransport->poll_fds[i++].revents & POLLIN) {
		nvmf_process_cm_event(transport);
		nfds--;
	}

	if (nfds == 0) {
		return SPDK_POLLER_BUSY;
	}

	/* Second and subsequent poll descriptors are IB async events */
	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		if (rtransport->poll_fds[i++].revents & POLLIN) {
			nvmf_process_ib_events(device, 32);
			nfds--;
		}
	}
	/* check all flagged fd's have been served */
	assert(nfds == 0);

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_rdma_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		     struct spdk_nvmf_ctrlr_data *cdata)
{
	cdata->nvmf_specific.msdbd = SPDK_NVMF_MAX_SGL_ENTRIES;

	/* Disable in-capsule data transfer for RDMA controller when dif_insert_or_strip is enabled
	since in-capsule data only works with NVME drives that support SGL memory layout */
	if (transport->opts.dif_insert_or_strip) {
		cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	}

	if (cdata->nvmf_specific.ioccsz > ((sizeof(struct spdk_nvme_cmd) + 0x1000) / 16)) {
		SPDK_WARNLOG("RDMA is configured to support up to 16 SGL entries while in capsule"
			     " data is greater than 4KiB.\n");
		SPDK_WARNLOG("When used in conjunction with the NVMe-oF initiator from the Linux "
			     "kernel between versions 5.4 and 5.12 data corruption may occur for "
			     "writes that are not a multiple of 4KiB in size.\n");
	}
}

static void
nvmf_rdma_discover(struct spdk_nvmf_transport *transport,
		   struct spdk_nvme_transport_id *trid,
		   struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_RDMA;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.rdma.rdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	entry->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	entry->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static void
nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_rdma_srq_init_attr		srq_init_attr;
	struct spdk_nvmf_rdma_resource_opts	opts;
	int					num_cqe;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rgroup = calloc(1, sizeof(*rgroup));
	if (!rgroup) {
		return NULL;
	}

	TAILQ_INIT(&rgroup->pollers);

	pthread_mutex_lock(&rtransport->lock);
	TAILQ_FOREACH(device, &rtransport->devices, link) {
		poller = calloc(1, sizeof(*poller));
		if (!poller) {
			SPDK_ERRLOG("Unable to allocate memory for new RDMA poller\n");
			nvmf_rdma_poll_group_destroy(&rgroup->group);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}

		poller->device = device;
		poller->group = rgroup;

		TAILQ_INIT(&poller->qpairs);
		STAILQ_INIT(&poller->qpairs_pending_send);
		STAILQ_INIT(&poller->qpairs_pending_recv);

		TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);
		if (rtransport->rdma_opts.no_srq == false && device->num_srq < device->attr.max_srq) {
			if ((int)rtransport->rdma_opts.max_srq_depth > device->attr.max_srq_wr) {
				SPDK_WARNLOG("Requested SRQ depth %u, max supported by dev %s is %d\n",
					     rtransport->rdma_opts.max_srq_depth, device->context->device->name, device->attr.max_srq_wr);
			}
			poller->max_srq_depth = spdk_min((int)rtransport->rdma_opts.max_srq_depth, device->attr.max_srq_wr);

			device->num_srq++;
			memset(&srq_init_attr, 0, sizeof(srq_init_attr));
			srq_init_attr.pd = device->pd;
			srq_init_attr.stats = &poller->stat.qp_stats.recv;
			srq_init_attr.srq_init_attr.attr.max_wr = poller->max_srq_depth;
			srq_init_attr.srq_init_attr.attr.max_sge = spdk_min(device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
			poller->srq = spdk_rdma_srq_create(&srq_init_attr);
			if (!poller->srq) {
				SPDK_ERRLOG("Unable to create shared receive queue, errno %d\n", errno);
				nvmf_rdma_poll_group_destroy(&rgroup->group);
				pthread_mutex_unlock(&rtransport->lock);
				return NULL;
			}

			opts.qp = poller->srq;
			opts.pd = device->pd;
			opts.qpair = NULL;
			opts.shared = true;
			opts.max_queue_depth = poller->max_srq_depth;
			opts.in_capsule_data_size = transport->opts.in_capsule_data_size;

			poller->resources = nvmf_rdma_resources_create(&opts);
			if (!poller->resources) {
				SPDK_ERRLOG("Unable to allocate resources for shared receive queue.\n");
				nvmf_rdma_poll_group_destroy(&rgroup->group);
				pthread_mutex_unlock(&rtransport->lock);
				return NULL;
			}
		}

		/*
		 * When using an srq, we can limit the completion queue at startup.
		 * The following formula represents the calculation:
		 * num_cqe = num_recv + num_data_wr + num_send_wr.
		 * where num_recv=num_data_wr=and num_send_wr=poller->max_srq_depth
		 */
		if (poller->srq) {
			num_cqe = poller->max_srq_depth * 3;
		} else {
			num_cqe = rtransport->rdma_opts.num_cqe;
		}

		poller->cq = ibv_create_cq(device->context, num_cqe, poller, NULL, 0);
		if (!poller->cq) {
			SPDK_ERRLOG("Unable to create completion queue\n");
			nvmf_rdma_poll_group_destroy(&rgroup->group);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}
		poller->num_cqe = num_cqe;
	}

	TAILQ_INSERT_TAIL(&rtransport->poll_groups, rgroup, link);
	if (rtransport->conn_sched.next_admin_pg == NULL) {
		rtransport->conn_sched.next_admin_pg = rgroup;
		rtransport->conn_sched.next_io_pg = rgroup;
	}

	pthread_mutex_unlock(&rtransport->lock);
	return &rgroup->group;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_rdma_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group **pg;
	struct spdk_nvmf_transport_poll_group *result;

	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);

	pthread_mutex_lock(&rtransport->lock);

	if (TAILQ_EMPTY(&rtransport->poll_groups)) {
		pthread_mutex_unlock(&rtransport->lock);
		return NULL;
	}

	if (qpair->qid == 0) {
		pg = &rtransport->conn_sched.next_admin_pg;
	} else {
		pg = &rtransport->conn_sched.next_io_pg;
	}

	assert(*pg != NULL);

	result = &(*pg)->group;

	*pg = TAILQ_NEXT(*pg, link);
	if (*pg == NULL) {
		*pg = TAILQ_FIRST(&rtransport->poll_groups);
	}

	pthread_mutex_unlock(&rtransport->lock);

	return result;
}

static void
nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup, *next_rgroup;
	struct spdk_nvmf_rdma_poller		*poller, *tmp;
	struct spdk_nvmf_rdma_qpair		*qpair, *tmp_qpair;
	struct spdk_nvmf_rdma_transport		*rtransport;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	if (!rgroup) {
		return;
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		TAILQ_REMOVE(&rgroup->pollers, poller, link);

		TAILQ_FOREACH_SAFE(qpair, &poller->qpairs, link, tmp_qpair) {
			nvmf_rdma_qpair_destroy(qpair);
		}

		if (poller->srq) {
			if (poller->resources) {
				nvmf_rdma_resources_destroy(poller->resources);
			}
			spdk_rdma_srq_destroy(poller->srq);
			SPDK_DEBUGLOG(rdma, "Destroyed RDMA shared queue %p\n", poller->srq);
		}

		if (poller->cq) {
			ibv_destroy_cq(poller->cq);
		}

		free(poller);
	}

	if (rgroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_rdma_poll_group_create()
		 * calls this function directly in a failure path. */
		free(rgroup);
		return;
	}

	rtransport = SPDK_CONTAINEROF(rgroup->group.transport, struct spdk_nvmf_rdma_transport, transport);

	pthread_mutex_lock(&rtransport->lock);
	next_rgroup = TAILQ_NEXT(rgroup, link);
	TAILQ_REMOVE(&rtransport->poll_groups, rgroup, link);
	if (next_rgroup == NULL) {
		next_rgroup = TAILQ_FIRST(&rtransport->poll_groups);
	}
	if (rtransport->conn_sched.next_admin_pg == rgroup) {
		rtransport->conn_sched.next_admin_pg = next_rgroup;
	}
	if (rtransport->conn_sched.next_io_pg == rgroup) {
		rtransport->conn_sched.next_io_pg = next_rgroup;
	}
	pthread_mutex_unlock(&rtransport->lock);

	free(rgroup);
}

static void
nvmf_rdma_qpair_reject_connection(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (rqpair->cm_id != NULL) {
		nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
	}
}

static int
nvmf_rdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;
	int					rc;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	device = rqpair->device;

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
	rqpair->srq = rqpair->poller->srq;

	rc = nvmf_rdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		return -1;
	}

	rc = nvmf_rdma_event_accept(rqpair->cm_id, rqpair);
	if (rc) {
		/* Try to reject, but we probably can't */
		nvmf_rdma_qpair_reject_connection(rqpair);
		return -1;
	}

	nvmf_rdma_update_ibv_state(rqpair);

	return 0;
}

static int
nvmf_rdma_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	assert(group->transport->tgt != NULL);

	rqpair->destruct_channel = spdk_get_io_channel(group->transport->tgt);

	if (!rqpair->destruct_channel) {
		SPDK_WARNLOG("failed to get io_channel, qpair %p\n", qpair);
		return 0;
	}

	/* Sanity check that we get io_channel on the correct thread */
	if (qpair->group) {
		assert(qpair->group->thread == spdk_io_channel_get_thread(rqpair->destruct_channel));
	}

	return 0;
}

static int
nvmf_rdma_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair,
					      struct spdk_nvmf_rdma_qpair, qpair);

	/*
	 * AER requests are freed when a qpair is destroyed. The recv corresponding to that request
	 * needs to be returned to the shared receive queue or the poll group will eventually be
	 * starved of RECV structures.
	 */
	if (rqpair->srq && rdma_req->recv) {
		int rc;
		struct ibv_recv_wr *bad_recv_wr;

		spdk_rdma_srq_queue_recv_wrs(rqpair->srq, &rdma_req->recv->wr);
		rc = spdk_rdma_srq_flush_recv_wrs(rqpair->srq, &bad_recv_wr);
		if (rc) {
			SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		}
	}

	_nvmf_rdma_request_free(rdma_req, rtransport);
	return 0;
}

static int
nvmf_rdma_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req,
			struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_qpair     *rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair,
			struct spdk_nvmf_rdma_qpair, qpair);

	if (rqpair->ibv_state != IBV_QPS_ERR) {
		/* The connection is alive, so process the request as normal */
		rdma_req->state = RDMA_REQUEST_STATE_EXECUTED;
	} else {
		/* The connection is dead. Move the request directly to the completed state. */
		rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
	}

	nvmf_rdma_request_process(rtransport, rdma_req);

	return 0;
}

static void
nvmf_rdma_close_qpair(struct spdk_nvmf_qpair *qpair,
		      spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	rqpair->to_close = true;

	/* This happens only when the qpair is disconnected before
	 * it is added to the poll group. Since there is no poll group,
	 * the RDMA qp has not been initialized yet and the RDMA CM
	 * event has not yet been acknowledged, so we need to reject it.
	 */
	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		nvmf_rdma_qpair_reject_connection(rqpair);
		nvmf_rdma_qpair_destroy(rqpair);
		return;
	}

	if (rqpair->rdma_qp) {
		spdk_rdma_qp_disconnect(rqpair->rdma_qp);
	}

	nvmf_rdma_destroy_drained_qpair(rqpair);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static struct spdk_nvmf_rdma_qpair *
get_rdma_qpair_from_wc(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_qpair *rqpair;
	/* @todo: improve QP search */
	TAILQ_FOREACH(rqpair, &rpoller->qpairs, link) {
		if (wc->qp_num == rqpair->rdma_qp->qp->qp_num) {
			return rqpair;
		}
	}
	SPDK_ERRLOG("Didn't find QP with qp_num %u\n", wc->qp_num);
	return NULL;
}

#ifdef DEBUG
static int
nvmf_rdma_req_is_completing(struct spdk_nvmf_rdma_request *rdma_req)
{
	return rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST ||
	       rdma_req->state == RDMA_REQUEST_STATE_COMPLETING;
}
#endif

static void
_poller_reset_failed_recvs(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_recv_wr *bad_recv_wr,
			   int rc)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;

	SPDK_ERRLOG("Failed to post a recv for the poller %p with errno %d\n", rpoller, -rc);
	while (bad_recv_wr != NULL) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_recv_wr->wr_id;
		rdma_recv = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);

		rdma_recv->qpair->current_recv_depth++;
		bad_recv_wr = bad_recv_wr->next;
		SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rdma_recv->qpair, -rc);
		spdk_nvmf_qpair_disconnect(&rdma_recv->qpair->qpair, NULL, NULL);
	}
}

static void
_qp_reset_failed_recvs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_recv_wr *bad_recv_wr, int rc)
{
	SPDK_ERRLOG("Failed to post a recv for the qpair %p with errno %d\n", rqpair, -rc);
	while (bad_recv_wr != NULL) {
		bad_recv_wr = bad_recv_wr->next;
		rqpair->current_recv_depth++;
	}
	spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
}

static void
_poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_recv_wr		*bad_recv_wr;
	int				rc;

	if (rpoller->srq) {
		rc = spdk_rdma_srq_flush_recv_wrs(rpoller->srq, &bad_recv_wr);
		if (rc) {
			_poller_reset_failed_recvs(rpoller, bad_recv_wr, rc);
		}
	} else {
		while (!STAILQ_EMPTY(&rpoller->qpairs_pending_recv)) {
			rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_recv);
			rc = spdk_rdma_qp_flush_recv_wrs(rqpair->rdma_qp, &bad_recv_wr);
			if (rc) {
				_qp_reset_failed_recvs(rqpair, bad_recv_wr, rc);
			}
			STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_recv, recv_link);
		}
	}
}

static void
_qp_reset_failed_sends(struct spdk_nvmf_rdma_transport *rtransport,
		       struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_send_wr *bad_wr, int rc)
{
	struct spdk_nvmf_rdma_wr	*bad_rdma_wr;
	struct spdk_nvmf_rdma_request	*prev_rdma_req = NULL, *cur_rdma_req = NULL;

	SPDK_ERRLOG("Failed to post a send for the qpair %p with errno %d\n", rqpair, -rc);
	for (; bad_wr != NULL; bad_wr = bad_wr->next) {
		bad_rdma_wr = (struct spdk_nvmf_rdma_wr *)bad_wr->wr_id;
		assert(rqpair->current_send_depth > 0);
		rqpair->current_send_depth--;
		switch (bad_rdma_wr->type) {
		case RDMA_WR_TYPE_DATA:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, data.rdma_wr);
			if (bad_wr->opcode == IBV_WR_RDMA_READ) {
				assert(rqpair->current_read_depth > 0);
				rqpair->current_read_depth--;
			}
			break;
		case RDMA_WR_TYPE_SEND:
			cur_rdma_req = SPDK_CONTAINEROF(bad_rdma_wr, struct spdk_nvmf_rdma_request, rsp.rdma_wr);
			break;
		default:
			SPDK_ERRLOG("Found a RECV in the list of pending SEND requests for qpair %p\n", rqpair);
			prev_rdma_req = cur_rdma_req;
			continue;
		}

		if (prev_rdma_req == cur_rdma_req) {
			/* this request was handled by an earlier wr. i.e. we were performing an nvme read. */
			/* We only have to check against prev_wr since each requests wrs are contiguous in this list. */
			continue;
		}

		switch (cur_rdma_req->state) {
		case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			cur_rdma_req->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			cur_rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			break;
		case RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
		case RDMA_REQUEST_STATE_COMPLETING:
			cur_rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			break;
		default:
			SPDK_ERRLOG("Found a request in a bad state %d when draining pending SEND requests for qpair %p\n",
				    cur_rdma_req->state, rqpair);
			continue;
		}

		nvmf_rdma_request_process(rtransport, cur_rdma_req);
		prev_rdma_req = cur_rdma_req;
	}

	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_ACTIVE) {
		/* Disconnect the connection. */
		spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
	}

}

static void
_poller_submit_sends(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_send_wr		*bad_wr = NULL;
	int				rc;

	while (!STAILQ_EMPTY(&rpoller->qpairs_pending_send)) {
		rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_send);
		rc = spdk_rdma_qp_flush_send_wrs(rqpair->rdma_qp, &bad_wr);

		/* bad wr always points to the first wr that failed. */
		if (rc) {
			_qp_reset_failed_sends(rtransport, rqpair, bad_wr, rc);
		}
		STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_send, send_link);
	}
}

static const char *
nvmf_rdma_wr_type_str(enum spdk_nvmf_rdma_wr_type wr_type)
{
	switch (wr_type) {
	case RDMA_WR_TYPE_RECV:
		return "RECV";
	case RDMA_WR_TYPE_SEND:
		return "SEND";
	case RDMA_WR_TYPE_DATA:
		return "DATA";
	default:
		SPDK_ERRLOG("Unknown WR type %d\n", wr_type);
		SPDK_UNREACHABLE();
	}
}

static inline void
nvmf_rdma_log_wc_status(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_wc *wc)
{
	enum spdk_nvmf_rdma_wr_type wr_type = ((struct spdk_nvmf_rdma_wr *)wc->wr_id)->type;

	if (wc->status == IBV_WC_WR_FLUSH_ERR) {
		/* If qpair is in ERR state, we will receive completions for all posted and not completed
		 * Work Requests with IBV_WC_WR_FLUSH_ERR status. Don't log an error in that case */
		SPDK_DEBUGLOG(rdma,
			      "Error on CQ %p, (qp state %d ibv_state %d) request 0x%lu, type %s, status: (%d): %s\n",
			      rqpair->poller->cq, rqpair->qpair.state, rqpair->ibv_state, wc->wr_id,
			      nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	} else {
		SPDK_ERRLOG("Error on CQ %p, (qp state %d ibv_state %d) request 0x%lu, type %s, status: (%d): %s\n",
			    rqpair->poller->cq, rqpair->qpair.state, rqpair->ibv_state, wc->wr_id,
			    nvmf_rdma_wr_type_str(wr_type), wc->status, ibv_wc_status_str(wc->status));
	}
}

static int
nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
		      struct spdk_nvmf_rdma_poller *rpoller)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_rdma_wr	*rdma_wr;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	int reaped, i;
	int count = 0;
	bool error = false;
	uint64_t poll_tsc = spdk_get_ticks();

	/* Poll for completing operations. */
	reaped = ibv_poll_cq(rpoller->cq, 32, wc);
	if (reaped < 0) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, spdk_strerror(errno));
		return -1;
	} else if (reaped == 0) {
		rpoller->stat.idle_polls++;
	}

	rpoller->stat.polls++;
	rpoller->stat.completions += reaped;

	for (i = 0; i < reaped; i++) {

		rdma_wr = (struct spdk_nvmf_rdma_wr *)wc[i].wr_id;

		switch (rdma_wr->type) {
		case RDMA_WR_TYPE_SEND:
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, rsp.rdma_wr);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			if (!wc[i].status) {
				count++;
				assert(wc[i].opcode == IBV_WC_SEND);
				assert(nvmf_rdma_req_is_completing(rdma_req));
			}

			rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			/* RDMA_WRITE operation completed. +1 since it was chained with rsp WR */
			rqpair->current_send_depth -= rdma_req->num_outstanding_data_wr + 1;
			rdma_req->num_outstanding_data_wr = 0;

			nvmf_rdma_request_process(rtransport, rdma_req);
			break;
		case RDMA_WR_TYPE_RECV:
			/* rdma_recv->qpair will be invalid if using an SRQ.  In that case we have to get the qpair from the wc. */
			rdma_recv = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_recv, rdma_wr);
			if (rpoller->srq != NULL) {
				rdma_recv->qpair = get_rdma_qpair_from_wc(rpoller, &wc[i]);
				/* It is possible that there are still some completions for destroyed QP
				 * associated with SRQ. We just ignore these late completions and re-post
				 * receive WRs back to SRQ.
				 */
				if (spdk_unlikely(NULL == rdma_recv->qpair)) {
					struct ibv_recv_wr *bad_wr;
					int rc;

					rdma_recv->wr.next = NULL;
					spdk_rdma_srq_queue_recv_wrs(rpoller->srq, &rdma_recv->wr);
					rc = spdk_rdma_srq_flush_recv_wrs(rpoller->srq, &bad_wr);
					if (rc) {
						SPDK_ERRLOG("Failed to re-post recv WR to SRQ, err %d\n", rc);
					}
					continue;
				}
			}
			rqpair = rdma_recv->qpair;

			assert(rqpair != NULL);
			if (!wc[i].status) {
				assert(wc[i].opcode == IBV_WC_RECV);
				if (rqpair->current_recv_depth >= rqpair->max_queue_depth) {
					spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
					break;
				}
			}

			rdma_recv->wr.next = NULL;
			rqpair->current_recv_depth++;
			rdma_recv->receive_tsc = poll_tsc;
			rpoller->stat.requests++;
			STAILQ_INSERT_HEAD(&rqpair->resources->incoming_queue, rdma_recv, link);
			break;
		case RDMA_WR_TYPE_DATA:
			rdma_req = SPDK_CONTAINEROF(rdma_wr, struct spdk_nvmf_rdma_request, data.rdma_wr);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			assert(rdma_req->num_outstanding_data_wr > 0);

			rqpair->current_send_depth--;
			rdma_req->num_outstanding_data_wr--;
			if (!wc[i].status) {
				assert(wc[i].opcode == IBV_WC_RDMA_READ);
				rqpair->current_read_depth--;
				/* wait for all outstanding reads associated with the same rdma_req to complete before proceeding. */
				if (rdma_req->num_outstanding_data_wr == 0) {
					rdma_req->state = RDMA_REQUEST_STATE_READY_TO_EXECUTE;
					nvmf_rdma_request_process(rtransport, rdma_req);
				}
			} else {
				/* If the data transfer fails still force the queue into the error state,
				 * if we were performing an RDMA_READ, we need to force the request into a
				 * completed state since it wasn't linked to a send. However, in the RDMA_WRITE
				 * case, we should wait for the SEND to complete. */
				if (rdma_req->data.wr.opcode == IBV_WR_RDMA_READ) {
					rqpair->current_read_depth--;
					if (rdma_req->num_outstanding_data_wr == 0) {
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
					}
				}
			}
			break;
		default:
			SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
			continue;
		}

		/* Handle error conditions */
		if (wc[i].status) {
			nvmf_rdma_update_ibv_state(rqpair);
			nvmf_rdma_log_wc_status(rqpair, &wc[i]);

			error = true;

			if (rqpair->qpair.state == SPDK_NVMF_QPAIR_ACTIVE) {
				/* Disconnect the connection. */
				spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
			} else {
				nvmf_rdma_destroy_drained_qpair(rqpair);
			}
			continue;
		}

		nvmf_rdma_qpair_process_pending(rtransport, rqpair, false);

		if (rqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
			nvmf_rdma_destroy_drained_qpair(rqpair);
		}
	}

	if (error == true) {
		return -1;
	}

	/* submit outstanding work requests. */
	_poller_submit_recvs(rtransport, rpoller);
	_poller_submit_sends(rtransport, rpoller);

	return count;
}

static int
nvmf_rdma_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller	*rpoller;
	int				count, rc;

	rtransport = SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_rdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	count = 0;
	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		rc = nvmf_rdma_poller_poll(rtransport, rpoller);
		if (rc < 0) {
			return rc;
		}
		count += rc;
	}

	return count;
}

static int
nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
			  struct spdk_nvme_transport_id *trid,
			  bool peer)
{
	struct sockaddr *saddr;
	uint16_t port;

	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_RDMA);

	if (peer) {
		saddr = rdma_get_peer_addr(id);
	} else {
		saddr = rdma_get_local_addr(id);
	}
	switch (saddr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *saddr_in = (struct sockaddr_in *)saddr;

		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
		inet_ntop(AF_INET, &saddr_in->sin_addr,
			  trid->traddr, sizeof(trid->traddr));
		if (peer) {
			port = ntohs(rdma_get_dst_port(id));
		} else {
			port = ntohs(rdma_get_src_port(id));
		}
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *saddr_in = (struct sockaddr_in6 *)saddr;
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;
		inet_ntop(AF_INET6, &saddr_in->sin6_addr,
			  trid->traddr, sizeof(trid->traddr));
		if (peer) {
			port = ntohs(rdma_get_dst_port(id));
		} else {
			port = ntohs(rdma_get_src_port(id));
		}
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", port);
		break;
	}
	default:
		return -1;

	}

	return 0;
}

static int
nvmf_rdma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, true);
}

static int
nvmf_rdma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, false);
}

static int
nvmf_rdma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return nvmf_rdma_trid_from_cm_id(rqpair->listen_id, trid, false);
}

void
spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	g_nvmf_hooks = *hooks;
}

static void
nvmf_rdma_request_set_abort_status(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_rdma_request *rdma_req_to_abort)
{
	rdma_req_to_abort->req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	rdma_req_to_abort->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;

	rdma_req_to_abort->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;

	req->rsp->nvme_cpl.cdw0 &= ~1U;	/* Command was successfully aborted. */
}

static int
_nvmf_rdma_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = SPDK_CONTAINEROF(
				req->req_to_abort, struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(req->req_to_abort->qpair,
					      struct spdk_nvmf_rdma_qpair, qpair);
	int rc;

	spdk_poller_unregister(&req->poller);

	switch (rdma_req_to_abort->state) {
	case RDMA_REQUEST_STATE_EXECUTING:
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	case RDMA_REQUEST_STATE_NEED_BUFFER:
		STAILQ_REMOVE(&rqpair->poller->group->group.pending_buf_queue,
			      &rdma_req_to_abort->req, spdk_nvmf_request, buf_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING:
		STAILQ_REMOVE(&rqpair->pending_rdma_read_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort);
		break;

	case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
		STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req_to_abort,
			      spdk_nvmf_rdma_request, state_link);

		nvmf_rdma_request_set_abort_status(req, rdma_req_to_abort);
		break;

	case RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		if (spdk_get_ticks() < req->timeout_tsc) {
			req->poller = SPDK_POLLER_REGISTER(_nvmf_rdma_qpair_abort_request, req, 0);
			return SPDK_POLLER_BUSY;
		}
		break;

	default:
		break;
	}

	spdk_nvmf_request_complete(req);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_rdma_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_qpair *rqpair;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_transport *transport;
	uint16_t cid;
	uint32_t i, max_req_count;
	struct spdk_nvmf_rdma_request *rdma_req_to_abort = NULL, *rdma_req;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
	transport = &rtransport->transport;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;
	max_req_count = rqpair->srq == NULL ? rqpair->max_queue_depth : rqpair->poller->max_srq_depth;

	for (i = 0; i < max_req_count; i++) {
		rdma_req = &rqpair->resources->reqs[i];
		/* When SRQ == NULL, rqpair has its own requests and req.qpair pointer always points to the qpair
		 * When SRQ != NULL all rqpairs share common requests and qpair pointer is assigned when we start to
		 * process a request. So in both cases all requests which are not in FREE state have valid qpair ptr */
		if (rdma_req->state != RDMA_REQUEST_STATE_FREE && rdma_req->req.cmd->nvme_cmd.cid == cid &&
		    rdma_req->req.qpair == qpair) {
			rdma_req_to_abort = rdma_req;
			break;
		}
	}

	if (rdma_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &rdma_req_to_abort->req;
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	req->poller = NULL;

	_nvmf_rdma_qpair_abort_request(req);
}

static void
nvmf_rdma_poll_group_dump_stat(struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller *rpoller;

	assert(w != NULL);

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	spdk_json_write_named_uint64(w, "pending_data_buffer", rgroup->stat.pending_data_buffer);

	spdk_json_write_named_array_begin(w, "devices");

	TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "name",
					     ibv_get_device_name(rpoller->device->context->device));
		spdk_json_write_named_uint64(w, "polls",
					     rpoller->stat.polls);
		spdk_json_write_named_uint64(w, "idle_polls",
					     rpoller->stat.idle_polls);
		spdk_json_write_named_uint64(w, "completions",
					     rpoller->stat.completions);
		spdk_json_write_named_uint64(w, "requests",
					     rpoller->stat.requests);
		spdk_json_write_named_uint64(w, "request_latency",
					     rpoller->stat.request_latency);
		spdk_json_write_named_uint64(w, "pending_free_request",
					     rpoller->stat.pending_free_request);
		spdk_json_write_named_uint64(w, "pending_rdma_read",
					     rpoller->stat.pending_rdma_read);
		spdk_json_write_named_uint64(w, "pending_rdma_write",
					     rpoller->stat.pending_rdma_write);
		spdk_json_write_named_uint64(w, "total_send_wrs",
					     rpoller->stat.qp_stats.send.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "send_doorbell_updates",
					     rpoller->stat.qp_stats.send.doorbell_updates);
		spdk_json_write_named_uint64(w, "total_recv_wrs",
					     rpoller->stat.qp_stats.recv.num_submitted_wrs);
		spdk_json_write_named_uint64(w, "recv_doorbell_updates",
					     rpoller->stat.qp_stats.recv.doorbell_updates);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.name = "RDMA",
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.opts_init = nvmf_rdma_opts_init,
	.create = nvmf_rdma_create,
	.dump_opts = nvmf_rdma_dump_opts,
	.destroy = nvmf_rdma_destroy,

	.listen = nvmf_rdma_listen,
	.stop_listen = nvmf_rdma_stop_listen,
	.cdata_init = nvmf_rdma_cdata_init,

	.listener_discover = nvmf_rdma_discover,

	.poll_group_create = nvmf_rdma_poll_group_create,
	.get_optimal_poll_group = nvmf_rdma_get_optimal_poll_group,
	.poll_group_destroy = nvmf_rdma_poll_group_destroy,
	.poll_group_add = nvmf_rdma_poll_group_add,
	.poll_group_remove = nvmf_rdma_poll_group_remove,
	.poll_group_poll = nvmf_rdma_poll_group_poll,

	.req_free = nvmf_rdma_request_free,
	.req_complete = nvmf_rdma_request_complete,

	.qpair_fini = nvmf_rdma_close_qpair,
	.qpair_get_peer_trid = nvmf_rdma_qpair_get_peer_trid,
	.qpair_get_local_trid = nvmf_rdma_qpair_get_local_trid,
	.qpair_get_listen_trid = nvmf_rdma_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_rdma_qpair_abort_request,

	.poll_group_dump_stat = nvmf_rdma_poll_group_dump_stat,
};

SPDK_NVMF_TRANSPORT_REGISTER(rdma, &spdk_nvmf_transport_rdma);
SPDK_LOG_REGISTER_COMPONENT(rdma)
