/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/config.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/log.h"

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

/* Timeout for destroying defunct rqpairs */
#define NVMF_RDMA_QPAIR_DESTROY_TIMEOUT_US	4000000

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

#define OBJECT_NVMF_RDMA_IO				0x40

#define TRACE_GROUP_NVMF_RDMA				0x4
#define TRACE_RDMA_REQUEST_STATE_NEW					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x0)
#define TRACE_RDMA_REQUEST_STATE_NEED_BUFFER				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x1)
#define TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x2)
#define TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x3)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x4)
#define TRACE_RDMA_REQUEST_STATE_EXECUTING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x5)
#define TRACE_RDMA_REQUEST_STATE_EXECUTED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x6)
#define TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x7)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x8)
#define TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x9)
#define TRACE_RDMA_REQUEST_STATE_COMPLETING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xA)
#define TRACE_RDMA_REQUEST_STATE_COMPLETED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xB)
#define TRACE_RDMA_QP_CREATE						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xC)
#define TRACE_RDMA_IBV_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xD)
#define TRACE_RDMA_CM_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xE)
#define TRACE_RDMA_QP_STATE_CHANGE					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xF)
#define TRACE_RDMA_QP_DISCONNECT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x10)
#define TRACE_RDMA_QP_DESTROY						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x11)

SPDK_TRACE_REGISTER_FN(nvmf_trace, "nvmf_rdma", TRACE_GROUP_NVMF_RDMA)
{
	spdk_trace_register_object(OBJECT_NVMF_RDMA_IO, 'r');
	spdk_trace_register_description("RDMA_REQ_NEW", TRACE_RDMA_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 1, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_NEED_BUFFER", TRACE_RDMA_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_C2H",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_H2C",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_TX_H2C",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_EXECUTE",
					TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_EXECUTING",
					TRACE_RDMA_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_EXECUTED",
					TRACE_RDMA_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_COMPL",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_COMPLETING_C2H",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_COMPLETING",
					TRACE_RDMA_REQUEST_STATE_COMPLETING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_COMPLETED",
					TRACE_RDMA_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");

	spdk_trace_register_description("RDMA_QP_CREATE", TRACE_RDMA_QP_CREATE,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("RDMA_IBV_ASYNC_EVENT", TRACE_RDMA_IBV_ASYNC_EVENT,
					OWNER_NONE, OBJECT_NONE, 0, 0, "type:   ");
	spdk_trace_register_description("RDMA_CM_ASYNC_EVENT", TRACE_RDMA_CM_ASYNC_EVENT,
					OWNER_NONE, OBJECT_NONE, 0, 0, "type:   ");
	spdk_trace_register_description("RDMA_QP_STATE_CHANGE", TRACE_RDMA_QP_STATE_CHANGE,
					OWNER_NONE, OBJECT_NONE, 0, 1, "state:  ");
	spdk_trace_register_description("RDMA_QP_DISCONNECT", TRACE_RDMA_QP_DISCONNECT,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("RDMA_QP_DESTROY", TRACE_RDMA_QP_DESTROY,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
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

enum spdk_nvmf_rdma_qpair_disconnect_flags {
	RDMA_QP_DISCONNECTING		= 1,
	RDMA_QP_RECV_DRAINED		= 1 << 1,
	RDMA_QP_SEND_DRAINED		= 1 << 2
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

struct spdk_nvmf_send_wr_list {
	struct ibv_send_wr	*first;
	struct ibv_send_wr	*last;
};

struct spdk_nvmf_recv_wr_list {
	struct ibv_recv_wr	*first;
	struct ibv_recv_wr	*last;
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

	/* The list of pending recvs to transfer */
	struct spdk_nvmf_recv_wr_list		recvs_to_post;

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

	struct rdma_cm_id			*cm_id;
	struct ibv_srq				*srq;
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

	/* The list of pending send requests for a transfer */
	struct spdk_nvmf_send_wr_list		sends_to_post;

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

	uint32_t				disconnect_flags;

	/* Poller registered in case the qpair doesn't properly
	 * complete the qpair destruct process and becomes defunct.
	 */

	struct spdk_poller			*destruct_poller;

	/* List of ibv async events */
	STAILQ_HEAD(, spdk_nvmf_rdma_ibv_event_ctx)	ibv_events;

	/* There are several ways a disconnect can start on a qpair
	 * and they are not all mutually exclusive. It is important
	 * that we only initialize one of these paths.
	 */
	bool					disconnect_started;
	/* Lets us know that we have received the last_wqe event. */
	bool					last_wqe_reached;
};

struct spdk_nvmf_rdma_poller_stat {
	uint64_t				completions;
	uint64_t				polls;
	uint64_t				requests;
	uint64_t				request_latency;
	uint64_t				pending_free_request;
	uint64_t				pending_rdma_read;
	uint64_t				pending_rdma_write;
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
	struct ibv_srq				*srq;

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
	/*
	 * buffers which are split across multiple RDMA
	 * memory regions cannot be used by this transport.
	 */
	STAILQ_HEAD(, spdk_nvmf_transport_pg_cache_buf)	retired_bufs;
};

struct spdk_nvmf_rdma_conn_sched {
	struct spdk_nvmf_rdma_poll_group *next_admin_pg;
	struct spdk_nvmf_rdma_poll_group *next_io_pg;
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
struct spdk_nvmf_rdma_device {
	struct ibv_device_attr			attr;
	struct ibv_context			*context;

	struct spdk_mem_map			*map;
	struct ibv_pd				*pd;

	int					num_srq;

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

	struct spdk_nvmf_rdma_conn_sched conn_sched;

	struct rdma_event_channel	*event_channel;

	struct spdk_mempool		*data_wr_pool;

	pthread_mutex_t			lock;

	/* fields used to poll RDMA/IB events */
	nfds_t			npoll_fds;
	struct pollfd		*poll_fds;

	TAILQ_HEAD(, spdk_nvmf_rdma_device)	devices;
	TAILQ_HEAD(, spdk_nvmf_rdma_port)	ports;
	TAILQ_HEAD(, spdk_nvmf_rdma_poll_group)	poll_groups;
};

static inline void
spdk_nvmf_rdma_start_disconnect(struct spdk_nvmf_rdma_qpair *rqpair);

static inline int
spdk_nvmf_rdma_check_ibv_state(enum ibv_qp_state state)
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
spdk_nvmf_rdma_dif_error_to_compl_status(uint8_t err_type) {
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
spdk_nvmf_rdma_update_ibv_state(struct spdk_nvmf_rdma_qpair *rqpair) {
	enum ibv_qp_state old_state, new_state;
	struct ibv_qp_attr qp_attr;
	struct ibv_qp_init_attr init_attr;
	int rc;

	old_state = rqpair->ibv_state;
	rc = ibv_query_qp(rqpair->cm_id->qp, &qp_attr,
			  g_spdk_nvmf_ibv_query_mask, &init_attr);

	if (rc)
	{
		SPDK_ERRLOG("Failed to get updated RDMA queue pair state!\n");
		return IBV_QPS_ERR + 1;
	}

	new_state = qp_attr.qp_state;
	rqpair->ibv_state = new_state;
	qp_attr.ah_attr.port_num = qp_attr.port_num;

	rc = spdk_nvmf_rdma_check_ibv_state(new_state);
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
		spdk_trace_record(TRACE_RDMA_QP_STATE_CHANGE, 0, 0,
				  (uintptr_t)rqpair->cm_id, new_state);
	}
	return new_state;
}

static const char *str_ibv_qp_state[] = {
	"IBV_QPS_RESET",
	"IBV_QPS_INIT",
	"IBV_QPS_RTR",
	"IBV_QPS_RTS",
	"IBV_QPS_SQD",
	"IBV_QPS_SQE",
	"IBV_QPS_ERR",
	"IBV_QPS_UNKNOWN"
};

static int
spdk_nvmf_rdma_set_ibv_state(struct spdk_nvmf_rdma_qpair *rqpair,
			     enum ibv_qp_state new_state)
{
	struct ibv_qp_attr qp_attr;
	struct ibv_qp_init_attr init_attr;
	int rc;
	enum ibv_qp_state state;
	static int attr_mask_rc[] = {
		[IBV_QPS_RESET] = IBV_QP_STATE,
		[IBV_QPS_INIT] = (IBV_QP_STATE |
				  IBV_QP_PKEY_INDEX |
				  IBV_QP_PORT |
				  IBV_QP_ACCESS_FLAGS),
		[IBV_QPS_RTR] = (IBV_QP_STATE |
				 IBV_QP_AV |
				 IBV_QP_PATH_MTU |
				 IBV_QP_DEST_QPN |
				 IBV_QP_RQ_PSN |
				 IBV_QP_MAX_DEST_RD_ATOMIC |
				 IBV_QP_MIN_RNR_TIMER),
		[IBV_QPS_RTS] = (IBV_QP_STATE |
				 IBV_QP_SQ_PSN |
				 IBV_QP_TIMEOUT |
				 IBV_QP_RETRY_CNT |
				 IBV_QP_RNR_RETRY |
				 IBV_QP_MAX_QP_RD_ATOMIC),
		[IBV_QPS_SQD] = IBV_QP_STATE,
		[IBV_QPS_SQE] = IBV_QP_STATE,
		[IBV_QPS_ERR] = IBV_QP_STATE,
	};

	rc = spdk_nvmf_rdma_check_ibv_state(new_state);
	if (rc) {
		SPDK_ERRLOG("QP#%d: bad state requested: %u\n",
			    rqpair->qpair.qid, new_state);
		return rc;
	}

	rc = ibv_query_qp(rqpair->cm_id->qp, &qp_attr,
			  g_spdk_nvmf_ibv_query_mask, &init_attr);

	if (rc) {
		SPDK_ERRLOG("Failed to get updated RDMA queue pair state!\n");
		assert(false);
	}

	qp_attr.cur_qp_state = rqpair->ibv_state;
	qp_attr.qp_state = new_state;

	rc = ibv_modify_qp(rqpair->cm_id->qp, &qp_attr,
			   attr_mask_rc[new_state]);

	if (rc) {
		SPDK_ERRLOG("QP#%d: failed to set state to: %s, %d (%s)\n",
			    rqpair->qpair.qid, str_ibv_qp_state[new_state], errno, strerror(errno));
		return rc;
	}

	state = spdk_nvmf_rdma_update_ibv_state(rqpair);

	if (state != new_state) {
		SPDK_ERRLOG("QP#%d: expected state: %s, actual state: %s\n",
			    rqpair->qpair.qid, str_ibv_qp_state[new_state],
			    str_ibv_qp_state[state]);
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "IBV QP#%u changed to: %s\n", rqpair->qpair.qid,
		      str_ibv_qp_state[state]);
	return 0;
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
			spdk_mempool_put(rtransport->data_wr_pool, data_wr);
		}
		data_wr = (!next_send_wr || next_send_wr == &rdma_req->rsp.wr) ? NULL :
			  SPDK_CONTAINEROF(next_send_wr, struct spdk_nvmf_rdma_request_data, wr);
	}
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
	struct ibv_qp			*qp;
	struct ibv_srq			*srq;
	uint32_t			i;
	int				rc;

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
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Command Array: %p Length: %lx LKey: %x\n",
		      resources->cmds, opts->max_queue_depth * sizeof(*resources->cmds),
		      resources->cmds_mr->lkey);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Completion Array: %p Length: %lx LKey: %x\n",
		      resources->cpls, opts->max_queue_depth * sizeof(*resources->cpls),
		      resources->cpls_mr->lkey);
	if (resources->bufs && resources->bufs_mr) {
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "In Capsule Data Array: %p Length: %x LKey: %x\n",
			      resources->bufs, opts->max_queue_depth *
			      opts->in_capsule_data_size, resources->bufs_mr->lkey);
	}

	/* Initialize queues */
	STAILQ_INIT(&resources->incoming_queue);
	STAILQ_INIT(&resources->free_queue);

	for (i = 0; i < opts->max_queue_depth; i++) {
		struct ibv_recv_wr *bad_wr = NULL;

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
		if (opts->shared) {
			srq = (struct ibv_srq *)opts->qp;
			rc = ibv_post_srq_recv(srq, &rdma_recv->wr, &bad_wr);
		} else {
			qp = (struct ibv_qp *)opts->qp;
			rc = ibv_post_recv(qp, &rdma_recv->wr, &bad_wr);
		}
		if (rc) {
			goto cleanup;
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

	return resources;

cleanup:
	nvmf_rdma_resources_destroy(resources);
	return NULL;
}

static void
spdk_nvmf_rdma_qpair_clean_ibv_events(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx, *tctx;
	STAILQ_FOREACH_SAFE(ctx, &rqpair->ibv_events, link, tctx) {
		ctx->rqpair = NULL;
		/* Memory allocated for ctx is freed in spdk_nvmf_rdma_qpair_process_ibv_event */
		STAILQ_REMOVE(&rqpair->ibv_events, ctx, spdk_nvmf_rdma_ibv_event_ctx, link);
	}
}

static void
spdk_nvmf_rdma_qpair_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv, *recv_tmp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	int				rc;

	spdk_trace_record(TRACE_RDMA_QP_DESTROY, 0, 0, (uintptr_t)rqpair->cm_id, 0);

	spdk_poller_unregister(&rqpair->destruct_poller);

	if (rqpair->qd != 0) {
		if (rqpair->srq == NULL) {
			nvmf_rdma_dump_qpair_contents(rqpair);
		}
		SPDK_WARNLOG("Destroying qpair when queue depth is %d\n", rqpair->qd);
	}

	if (rqpair->poller) {
		TAILQ_REMOVE(&rqpair->poller->qpairs, rqpair, link);

		if (rqpair->srq != NULL && rqpair->resources != NULL) {
			/* Drop all received but unprocessed commands for this queue and return them to SRQ */
			STAILQ_FOREACH_SAFE(rdma_recv, &rqpair->resources->incoming_queue, link, recv_tmp) {
				if (rqpair == rdma_recv->qpair) {
					STAILQ_REMOVE(&rqpair->resources->incoming_queue, rdma_recv, spdk_nvmf_rdma_recv, link);
					rc = ibv_post_srq_recv(rqpair->srq, &rdma_recv->wr, &bad_recv_wr);
					if (rc) {
						SPDK_ERRLOG("Unable to re-post rx descriptor\n");
					}
				}
			}
		}
	}

	if (rqpair->cm_id) {
		if (rqpair->cm_id->qp != NULL) {
			rdma_destroy_qp(rqpair->cm_id);
		}
		rdma_destroy_id(rqpair->cm_id);

		if (rqpair->poller != NULL && rqpair->srq == NULL) {
			rqpair->poller->required_num_wr -= MAX_WR_PER_QP(rqpair->max_queue_depth);
		}
	}

	if (rqpair->srq == NULL && rqpair->resources != NULL) {
		nvmf_rdma_resources_destroy(rqpair->resources);
	}

	spdk_nvmf_rdma_qpair_clean_ibv_events(rqpair);

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
		if (required_num_wr > device->attr.max_cqe) {
			SPDK_ERRLOG("RDMA CQE requirement (%d) exceeds device max_cqe limitation (%d)\n",
				    required_num_wr, device->attr.max_cqe);
			return -1;
		}

		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Resize RDMA CQ from %d to %d\n", rpoller->num_cqe, num_cqe);
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
spdk_nvmf_rdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair		*rqpair;
	int					rc;
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_rdma_resource_opts	opts;
	struct spdk_nvmf_rdma_device		*device;
	struct ibv_qp_init_attr			ibv_init_attr;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->device;

	memset(&ibv_init_attr, 0, sizeof(struct ibv_qp_init_attr));
	ibv_init_attr.qp_context	= rqpair;
	ibv_init_attr.qp_type		= IBV_QPT_RC;
	ibv_init_attr.send_cq		= rqpair->poller->cq;
	ibv_init_attr.recv_cq		= rqpair->poller->cq;

	if (rqpair->srq) {
		ibv_init_attr.srq		= rqpair->srq;
	} else {
		ibv_init_attr.cap.max_recv_wr	= rqpair->max_queue_depth +
						  1; /* RECV operations + dummy drain WR */
	}

	ibv_init_attr.cap.max_send_wr	= rqpair->max_queue_depth *
					  2 + 1; /* SEND, READ, and WRITE operations + dummy drain WR */
	ibv_init_attr.cap.max_send_sge	= spdk_min(device->attr.max_sge, NVMF_DEFAULT_TX_SGE);
	ibv_init_attr.cap.max_recv_sge	= spdk_min(device->attr.max_sge, NVMF_DEFAULT_RX_SGE);

	if (rqpair->srq == NULL && nvmf_rdma_resize_cq(rqpair, device) < 0) {
		SPDK_ERRLOG("Failed to resize the completion queue. Cannot initialize qpair.\n");
		goto error;
	}

	rc = rdma_create_qp(rqpair->cm_id, device->pd, &ibv_init_attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed: errno %d: %s\n", errno, spdk_strerror(errno));
		goto error;
	}

	rqpair->max_send_depth = spdk_min((uint32_t)(rqpair->max_queue_depth * 2 + 1),
					  ibv_init_attr.cap.max_send_wr);
	rqpair->max_send_sge = spdk_min(NVMF_DEFAULT_TX_SGE, ibv_init_attr.cap.max_send_sge);
	rqpair->max_recv_sge = spdk_min(NVMF_DEFAULT_RX_SGE, ibv_init_attr.cap.max_recv_sge);
	spdk_trace_record(TRACE_RDMA_QP_CREATE, 0, 0, (uintptr_t)rqpair->cm_id, 0);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "New RDMA Connection: %p\n", qpair);

	rqpair->sends_to_post.first = NULL;
	rqpair->sends_to_post.last = NULL;

	if (rqpair->poller->srq == NULL) {
		rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
		transport = &rtransport->transport;

		opts.qp = rqpair->cm_id->qp;
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
	struct ibv_recv_wr *last;

	last = first;
	while (last->next != NULL) {
		last = last->next;
	}

	if (rqpair->resources->recvs_to_post.first == NULL) {
		rqpair->resources->recvs_to_post.first = first;
		rqpair->resources->recvs_to_post.last = last;
		if (rqpair->srq == NULL) {
			STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_recv, rqpair, recv_link);
		}
	} else {
		rqpair->resources->recvs_to_post.last->next = first;
		rqpair->resources->recvs_to_post.last = last;
	}
}

/* Append the given send wr structure to the qpair's outstanding sends list. */
/* This function accepts either a single wr or the first wr in a linked list. */
static void
nvmf_rdma_qpair_queue_send_wrs(struct spdk_nvmf_rdma_qpair *rqpair, struct ibv_send_wr *first)
{
	struct ibv_send_wr *last;

	last = first;
	while (last->next != NULL) {
		last = last->next;
	}

	if (rqpair->sends_to_post.first == NULL) {
		rqpair->sends_to_post.first = first;
		rqpair->sends_to_post.last = last;
		STAILQ_INSERT_TAIL(&rqpair->poller->qpairs_pending_send, rqpair, send_link);
	} else {
		rqpair->sends_to_post.last->next = first;
		rqpair->sends_to_post.last = last;
	}
}

static int
request_transfer_in(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;

	qpair = req->qpair;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
	assert(rdma_req != NULL);

	nvmf_rdma_qpair_queue_send_wrs(rqpair, &rdma_req->data.wr);
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

	*data_posted = 0;
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

	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		first = &rdma_req->data.wr;
		*data_posted = 1;
		num_outstanding_data_wr = rdma_req->num_outstanding_data_wr;
	}
	nvmf_rdma_qpair_queue_send_wrs(rqpair, first);
	/* +1 for the rsp wr */
	rqpair->current_send_depth += num_outstanding_data_wr + 1;

	return 0;
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
		  new_qpair_fn cb_fn, void *cb_arg)
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
	max_queue_depth = rtransport->transport.opts.max_queue_depth;
	max_read_depth = rtransport->transport.opts.max_queue_depth;
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Target Max Queue Depth: %d\n",
		      rtransport->transport.opts.max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_DEBUGLOG(SPDK_LOG_RDMA,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      port->device->attr.max_qp_wr, port->device->attr.max_qp_rd_atom);
	max_queue_depth = spdk_min(max_queue_depth, port->device->attr.max_qp_wr);
	max_read_depth = spdk_min(max_read_depth, port->device->attr.max_qp_init_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	SPDK_DEBUGLOG(SPDK_LOG_RDMA,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	if (rdma_param->initiator_depth > 0) {
		max_read_depth = spdk_min(max_read_depth, rdma_param->initiator_depth);
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
		      max_queue_depth, max_read_depth);

	rqpair = calloc(1, sizeof(struct spdk_nvmf_rdma_qpair));
	if (rqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
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

	cb_fn(&rqpair->qpair, cb_arg);

	return 0;
}

static int
spdk_nvmf_rdma_mem_notify(void *cb_ctx, struct spdk_mem_map *map,
			  enum spdk_mem_map_notify_action action,
			  void *vaddr, size_t size)
{
	struct ibv_pd *pd = cb_ctx;
	struct ibv_mr *mr;
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (!g_nvmf_hooks.get_rkey) {
			mr = ibv_reg_mr(pd, vaddr, size,
					IBV_ACCESS_LOCAL_WRITE |
					IBV_ACCESS_REMOTE_READ |
					IBV_ACCESS_REMOTE_WRITE);
			if (mr == NULL) {
				SPDK_ERRLOG("ibv_reg_mr() failed\n");
				return -1;
			} else {
				rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)mr);
			}
		} else {
			rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, size,
							  g_nvmf_hooks.get_rkey(pd, vaddr, size));
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		if (!g_nvmf_hooks.get_rkey) {
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
spdk_nvmf_rdma_check_contiguous_entries(uint64_t addr_1, uint64_t addr_2)
{
	/* Two contiguous mappings will point to the same address which is the start of the RDMA MR. */
	return addr_1 == addr_2;
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

/* This function is used in the rare case that we have a buffer split over multiple memory regions. */
static int
nvmf_rdma_replace_buffer(struct spdk_nvmf_rdma_poll_group *rgroup, void **buf)
{
	struct spdk_nvmf_transport_poll_group	*group = &rgroup->group;
	struct spdk_nvmf_transport		*transport = group->transport;
	struct spdk_nvmf_transport_pg_cache_buf	*old_buf;
	void					*new_buf;

	if (!(STAILQ_EMPTY(&group->buf_cache))) {
		group->buf_cache_count--;
		new_buf = STAILQ_FIRST(&group->buf_cache);
		STAILQ_REMOVE_HEAD(&group->buf_cache, link);
		assert(*buf != NULL);
	} else {
		new_buf = spdk_mempool_get(transport->data_buf_pool);
	}

	if (*buf == NULL) {
		return -ENOMEM;
	}

	old_buf = *buf;
	STAILQ_INSERT_HEAD(&rgroup->retired_bufs, old_buf, link);
	*buf = new_buf;
	return 0;
}

static bool
nvmf_rdma_get_lkey(struct spdk_nvmf_rdma_device *device, struct iovec *iov,
		   uint32_t *_lkey)
{
	uint64_t	translation_len;
	uint32_t	lkey;

	translation_len = iov->iov_len;

	if (!g_nvmf_hooks.get_rkey) {
		lkey = ((struct ibv_mr *)spdk_mem_map_translate(device->map,
				(uint64_t)iov->iov_base, &translation_len))->lkey;
	} else {
		lkey = spdk_mem_map_translate(device->map,
					      (uint64_t)iov->iov_base, &translation_len);
	}

	if (spdk_unlikely(translation_len < iov->iov_len)) {
		return false;
	}

	*_lkey = lkey;
	return true;
}

static bool
nvmf_rdma_fill_wr_sge(struct spdk_nvmf_rdma_device *device,
		      struct iovec *iov, struct ibv_send_wr **_wr,
		      uint32_t *_remaining_data_block, uint32_t *_offset,
		      uint32_t *_num_extra_wrs,
		      const struct spdk_dif_ctx *dif_ctx)
{
	struct ibv_send_wr *wr = *_wr;
	struct ibv_sge	*sg_ele = &wr->sg_list[wr->num_sge];
	uint32_t	lkey = 0;
	uint32_t	remaining, data_block_size, md_size, sge_len;

	if (spdk_unlikely(!nvmf_rdma_get_lkey(device, iov, &lkey))) {
		/* This is a very rare case that can occur when using DPDK version < 19.05 */
		SPDK_ERRLOG("Data buffer split over multiple RDMA Memory Regions. Removing it from circulation.\n");
		return false;
	}

	if (spdk_likely(!dif_ctx)) {
		sg_ele->lkey = lkey;
		sg_ele->addr = (uintptr_t)(iov->iov_base);
		sg_ele->length = iov->iov_len;
		wr->num_sge++;
	} else {
		remaining = iov->iov_len - *_offset;
		data_block_size = dif_ctx->block_size - dif_ctx->md_size;
		md_size = dif_ctx->md_size;

		while (remaining) {
			if (wr->num_sge >= SPDK_NVMF_MAX_SGL_ENTRIES) {
				if (*_num_extra_wrs > 0 && wr->next) {
					*_wr = wr->next;
					wr = *_wr;
					wr->num_sge = 0;
					sg_ele = &wr->sg_list[wr->num_sge];
					(*_num_extra_wrs)--;
				} else {
					break;
				}
			}
			sg_ele->lkey = lkey;
			sg_ele->addr = (uintptr_t)((char *)iov->iov_base + *_offset);
			sge_len = spdk_min(remaining, *_remaining_data_block);
			sg_ele->length = sge_len;
			remaining -= sge_len;
			*_remaining_data_block -= sge_len;
			*_offset += sge_len;

			sg_ele++;
			wr->num_sge++;

			if (*_remaining_data_block == 0) {
				/* skip metadata */
				*_offset += md_size;
				/* Metadata that do not fit this IO buffer will be included in the next IO buffer */
				remaining -= spdk_min(remaining, md_size);
				*_remaining_data_block = data_block_size;
			}

			if (remaining == 0) {
				/* By subtracting the size of the last IOV from the offset, we ensure that we skip
				   the remaining metadata bits at the beginning of the next buffer */
				*_offset -= iov->iov_len;
			}
		}
	}

	return true;
}

static int
nvmf_rdma_fill_wr_sgl(struct spdk_nvmf_rdma_poll_group *rgroup,
		      struct spdk_nvmf_rdma_device *device,
		      struct spdk_nvmf_rdma_request *rdma_req,
		      struct ibv_send_wr *wr,
		      uint32_t length,
		      uint32_t num_extra_wrs)
{
	struct spdk_nvmf_request *req = &rdma_req->req;
	struct spdk_dif_ctx *dif_ctx = NULL;
	uint32_t remaining_data_block = 0;
	uint32_t offset = 0;

	if (spdk_unlikely(rdma_req->req.dif.dif_insert_or_strip)) {
		dif_ctx = &rdma_req->req.dif.dif_ctx;
		remaining_data_block = dif_ctx->block_size - dif_ctx->md_size;
	}

	wr->num_sge = 0;

	while (length && (num_extra_wrs || wr->num_sge < SPDK_NVMF_MAX_SGL_ENTRIES)) {
		while (spdk_unlikely(!nvmf_rdma_fill_wr_sge(device, &req->iov[rdma_req->iovpos], &wr,
				     &remaining_data_block, &offset, &num_extra_wrs, dif_ctx))) {
			if (nvmf_rdma_replace_buffer(rgroup, &req->buffers[rdma_req->iovpos]) == -ENOMEM) {
				return -ENOMEM;
			}
			req->iov[rdma_req->iovpos].iov_base = (void *)((uintptr_t)(req->buffers[rdma_req->iovpos] +
							      NVMF_DATA_BUFFER_MASK) &
							      ~NVMF_DATA_BUFFER_MASK);
		}

		length -= req->iov[rdma_req->iovpos].iov_len;
		rdma_req->iovpos++;
	}

	if (length) {
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
spdk_nvmf_rdma_request_fill_iovs(struct spdk_nvmf_rdma_transport *rtransport,
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

	if (spdk_unlikely(req->dif.dif_insert_or_strip)) {
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
	uint32_t				lengths[SPDK_NVMF_MAX_SGL_ENTRIES];
	uint32_t				i;
	int					rc;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	inline_segment = &req->cmd->nvme_cmd.dptr.sgl1;
	assert(inline_segment->generic.type == SPDK_NVME_SGL_TYPE_LAST_SEGMENT);
	assert(inline_segment->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET);

	num_sgl_descriptors = inline_segment->unkeyed.length / sizeof(struct spdk_nvme_sgl_descriptor);
	assert(num_sgl_descriptors <= SPDK_NVMF_MAX_SGL_ENTRIES);

	if (nvmf_request_alloc_wrs(rtransport, rdma_req, num_sgl_descriptors - 1) != 0) {
		return -ENOMEM;
	}

	desc = (struct spdk_nvme_sgl_descriptor *)rdma_req->recv->buf + inline_segment->address;
	for (i = 0; i < num_sgl_descriptors; i++) {
		if (spdk_likely(!req->dif.dif_insert_or_strip)) {
			lengths[i] = desc->keyed.length;
		} else {
			req->dif.orig_length += desc->keyed.length;
			lengths[i] = spdk_dif_get_length_with_md(desc->keyed.length, &req->dif.dif_ctx);
			req->dif.elba_length += lengths[i];
		}
		desc++;
	}

	rc = spdk_nvmf_request_get_buffers_multi(req, &rgroup->group, &rtransport->transport,
			lengths, num_sgl_descriptors);
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

		current_wr->num_sge = 0;

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
spdk_nvmf_rdma_request_parse_sgl(struct spdk_nvmf_rdma_transport *rtransport,
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

		if (spdk_unlikely(req->dif.dif_insert_or_strip)) {
			req->dif.orig_length = length;
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		rc = spdk_nvmf_rdma_request_fill_iovs(rtransport, device, rdma_req, length);
		if (spdk_unlikely(rc < 0)) {
			if (rc == -EINVAL) {
				SPDK_ERRLOG("SGL length exceeds the max I/O size\n");
				rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				return -1;
			}
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		}

		/* backward compatible */
		req->data = req->iov[0].iov_base;

		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = rtransport->transport.opts.in_capsule_data_size;

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
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		} else if (rc == -EINVAL) {
			SPDK_ERRLOG("Multi SGL element request length exceeds the max I/O size\n");
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		/* backward compatible */
		req->data = req->iov[0].iov_base;

		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      req->iovcnt);

		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static void
nvmf_rdma_request_free(struct spdk_nvmf_rdma_request *rdma_req,
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
	rdma_req->rsp.wr.next = NULL;
	rdma_req->data.wr.next = NULL;
	memset(&rdma_req->req.dif, 0, sizeof(rdma_req->req.dif));
	rqpair->qd--;

	STAILQ_INSERT_HEAD(&rqpair->resources->free_queue, rdma_req, state_link);
	rdma_req->state = RDMA_REQUEST_STATE_FREE;
}

static bool
spdk_nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
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

		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Request %p entering state %d\n", rdma_req, prev_state);

		switch (rdma_req->state) {
		case RDMA_REQUEST_STATE_FREE:
			/* Some external code must kick a request into RDMA_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEW, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
			rdma_recv = rdma_req->recv;

			/* The first element of the SGL is the NVMe command */
			rdma_req->req.cmd = (union nvmf_h2c_msg *)rdma_recv->sgl[0].addr;
			memset(rdma_req->req.rsp, 0, sizeof(*rdma_req->req.rsp));

			if (rqpair->ibv_state == IBV_QPS_ERR  || rqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
				rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
				break;
			}

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&rdma_req->req, &rdma_req->req.dif.dif_ctx))) {
				rdma_req->req.dif.dif_insert_or_strip = true;
			}

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
			rdma_req->rsp.wr.opcode = IBV_WR_SEND;
			rdma_req->rsp.wr.imm_data = 0;
#endif

			/* The next state transition depends on the data transfer needs of this request. */
			rdma_req->req.xfer = spdk_nvmf_req_get_xfer(&rdma_req->req);

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
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);

			assert(rdma_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (&rdma_req->req != STAILQ_FIRST(&rgroup->group.pending_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = spdk_nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
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
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);

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
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);

			if (spdk_unlikely(rdma_req->req.dif.dif_insert_or_strip)) {
				if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
					/* generate DIF for write operation */
					num_blocks = SPDK_CEIL_DIV(rdma_req->req.dif.elba_length, rdma_req->req.dif.dif_ctx.block_size);
					assert(num_blocks > 0);

					rc = spdk_dif_generate(rdma_req->req.iov, rdma_req->req.iovcnt,
							       num_blocks, &rdma_req->req.dif.dif_ctx);
					if (rc != 0) {
						SPDK_ERRLOG("DIF generation failed\n");
						rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
						spdk_nvmf_rdma_start_disconnect(rqpair);
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
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_EXECUTED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
			if (rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				STAILQ_INSERT_TAIL(&rqpair->pending_rdma_write_queue, rdma_req, state_link);
				rdma_req->state = RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING;
			} else {
				rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
			}
			if (spdk_unlikely(rdma_req->req.dif.dif_insert_or_strip)) {
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
						rsp->status.sc = spdk_nvmf_rdma_dif_error_to_compl_status(error_blk.err_type);
						rdma_req->state = RDMA_REQUEST_STATE_READY_TO_COMPLETE;
						STAILQ_REMOVE(&rqpair->pending_rdma_write_queue, rdma_req, spdk_nvmf_rdma_request, state_link);
					}
				}
			}
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);

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
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
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
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
			/* Some external code must kick a request into RDMA_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case RDMA_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_COMPLETED, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);

			rqpair->poller->stat.request_latency += spdk_get_ticks() - rdma_req->receive_tsc;
			nvmf_rdma_request_free(rdma_req, rtransport);
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

static void
spdk_nvmf_rdma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_RDMA_MIN_IO_BUFFER_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_RDMA_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_RDMA_DEFAULT_BUFFER_CACHE_SIZE;
	opts->max_srq_depth =		SPDK_NVMF_RDMA_DEFAULT_SRQ_DEPTH;
	opts->no_srq =			SPDK_NVMF_RDMA_DEFAULT_NO_SRQ;
	opts->dif_insert_or_strip =	SPDK_NVMF_RDMA_DIF_INSERT_OR_STRIP;
}

const struct spdk_mem_map_ops g_nvmf_rdma_map_ops = {
	.notify_cb = spdk_nvmf_rdma_mem_notify,
	.are_contiguous = spdk_nvmf_rdma_check_contiguous_entries
};

static int spdk_nvmf_rdma_destroy(struct spdk_nvmf_transport *transport);

static struct spdk_nvmf_transport *
spdk_nvmf_rdma_create(struct spdk_nvmf_transport_opts *opts)
{
	int rc;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device	*device, *tmp;
	struct ibv_context		**contexts;
	uint32_t			i;
	int				flag;
	uint32_t			sge_count;
	uint32_t			min_shared_buffers;
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

	SPDK_INFOLOG(SPDK_LOG_RDMA, "*** RDMA Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, max_srq_depth=%d, no_srq=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     opts->max_srq_depth,
		     opts->no_srq);

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	if (opts->num_shared_buffers < (SPDK_NVMF_MAX_SGL_ENTRIES * 2)) {
		SPDK_ERRLOG("The number of shared data buffers (%d) is less than"
			    "the minimum number required to guarantee that forward progress can be made (%d)\n",
			    opts->num_shared_buffers, (SPDK_NVMF_MAX_SGL_ENTRIES * 2));
		spdk_nvmf_rdma_destroy(&rtransport->transport);
		return NULL;
	}

	min_shared_buffers = spdk_thread_get_count() * opts->buf_cache_size;
	if (min_shared_buffers > opts->num_shared_buffers) {
		SPDK_ERRLOG("There are not enough buffers to satisfy"
			    "per-poll group caches for each thread. (%" PRIu32 ")"
			    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
		SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
		spdk_nvmf_rdma_destroy(&rtransport->transport);
		return NULL;
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > NVMF_DEFAULT_TX_SGE) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		spdk_nvmf_rdma_destroy(&rtransport->transport);
		return NULL;
	}

	rtransport->event_channel = rdma_create_event_channel();
	if (rtransport->event_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed, %s\n", spdk_strerror(errno));
		spdk_nvmf_rdma_destroy(&rtransport->transport);
		return NULL;
	}

	flag = fcntl(rtransport->event_channel->fd, F_GETFL);
	if (fcntl(rtransport->event_channel->fd, F_SETFL, flag | O_NONBLOCK) < 0) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%s)\n",
			    rtransport->event_channel->fd, spdk_strerror(errno));
		spdk_nvmf_rdma_destroy(&rtransport->transport);
		return NULL;
	}

	rtransport->data_wr_pool = spdk_mempool_create("spdk_nvmf_rdma_wr_data",
				   opts->max_queue_depth * SPDK_NVMF_MAX_SGL_ENTRIES,
				   sizeof(struct spdk_nvmf_rdma_request_data),
				   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_SOCKET_ID_ANY);
	if (!rtransport->data_wr_pool) {
		SPDK_ERRLOG("Unable to allocate work request pool for poll group\n");
		spdk_nvmf_rdma_destroy(&rtransport->transport);
		return NULL;
	}

	contexts = rdma_get_devices(NULL);
	if (contexts == NULL) {
		SPDK_ERRLOG("rdma_get_devices() failed: %s (%d)\n", spdk_strerror(errno), errno);
		spdk_nvmf_rdma_destroy(&rtransport->transport);
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
		if (device->attr.vendor_id == 0) {
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

		device->map = spdk_mem_map_alloc(0, &g_nvmf_rdma_map_ops, device->pd);
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
		spdk_nvmf_rdma_destroy(&rtransport->transport);
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
		spdk_nvmf_rdma_destroy(&rtransport->transport);
		return NULL;
	}

	rtransport->poll_fds[i].fd = rtransport->event_channel->fd;
	rtransport->poll_fds[i++].events = POLLIN;

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		rtransport->poll_fds[i].fd = device->context->async_fd;
		rtransport->poll_fds[i++].events = POLLIN;
	}

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

	if (rtransport->poll_fds != NULL) {
		free(rtransport->poll_fds);
	}

	if (rtransport->event_channel != NULL) {
		rdma_destroy_event_channel(rtransport->event_channel);
	}

	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, device_tmp) {
		TAILQ_REMOVE(&rtransport->devices, device, link);
		if (device->map) {
			spdk_mem_map_free(&device->map);
		}
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

	pthread_mutex_destroy(&rtransport->lock);
	free(rtransport);

	return 0;
}

static int
spdk_nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
			       struct spdk_nvme_transport_id *trid,
			       bool peer);

static int
spdk_nvmf_rdma_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid,
		      spdk_nvmf_tgt_listen_done_fn cb_fn,
		      void *cb_arg)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_port	*port;
	struct addrinfo			*res;
	struct addrinfo			hints;
	int				family;
	int				rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	assert(rtransport->event_channel != NULL);

	pthread_mutex_lock(&rtransport->lock);
	TAILQ_FOREACH(port, &rtransport->ports, link) {
		if (spdk_nvme_transport_id_compare(&port->trid, trid) == 0) {
			goto success;
		}
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		pthread_mutex_unlock(&rtransport->lock);
		return -ENOMEM;
	}

	/* Selectively copy the trid. Things like NQN don't matter here - that
	 * mapping is enforced elsewhere.
	 */
	spdk_nvme_trid_populate_transport(&port->trid, SPDK_NVME_TRANSPORT_RDMA);
	port->trid.adrfam = trid->adrfam;
	snprintf(port->trid.traddr, sizeof(port->trid.traddr), "%s", trid->traddr);
	snprintf(port->trid.trsvcid, sizeof(port->trid.trsvcid), "%s", trid->trsvcid);

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
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	rc = getaddrinfo(port->trid.traddr, port->trid.trsvcid, &hints, &res);
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

	SPDK_NOTICELOG("*** NVMe/RDMA Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);

success:
	port->ref++;
	pthread_mutex_unlock(&rtransport->lock);
	if (cb_fn != NULL) {
		cb_fn(cb_arg, 0);
	}
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
	spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_RDMA);
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
spdk_nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
				     struct spdk_nvmf_rdma_qpair *rqpair, bool drain)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;
	struct spdk_nvmf_rdma_resources *resources;

	/* We process I/O in the data transfer pending queue at the highest priority. RDMA reads first */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_read_queue, state_link, req_tmp) {
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* Then RDMA writes since reads have stronger restrictions than writes */
	STAILQ_FOREACH_SAFE(rdma_req, &rqpair->pending_rdma_write_queue, state_link, req_tmp) {
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
			break;
		}
	}

	/* The second highest priority is I/O waiting on memory buffers. */
	STAILQ_FOREACH_SAFE(req, &rqpair->poller->group->group.pending_buf_queue, buf_link, tmp) {
		rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false && drain == false) {
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
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
	if (!STAILQ_EMPTY(&resources->incoming_queue) && STAILQ_EMPTY(&resources->free_queue)) {
		rqpair->poller->stat.pending_free_request++;
	}
}

static void
_nvmf_rdma_qpair_disconnect(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;

	spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
}

static void
_nvmf_rdma_try_disconnect(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;
	struct spdk_nvmf_poll_group *group;

	/* Read the group out of the qpair. This is normally set and accessed only from
	 * the thread that created the group. Here, we're not on that thread necessarily.
	 * The data member qpair->group begins it's life as NULL and then is assigned to
	 * a pointer and never changes. So fortunately reading this and checking for
	 * non-NULL is thread safe in the x86_64 memory model. */
	group = qpair->group;

	if (group == NULL) {
		/* The qpair hasn't been assigned to a group yet, so we can't
		 * process a disconnect. Send a message to ourself and try again. */
		spdk_thread_send_msg(spdk_get_thread(), _nvmf_rdma_try_disconnect, qpair);
		return;
	}

	spdk_thread_send_msg(group->thread, _nvmf_rdma_qpair_disconnect, qpair);
}

static inline void
spdk_nvmf_rdma_start_disconnect(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (!__atomic_test_and_set(&rqpair->disconnect_started, __ATOMIC_RELAXED)) {
		_nvmf_rdma_try_disconnect(&rqpair->qpair);
	}
}

static void nvmf_rdma_destroy_drained_qpair(void *ctx)
{
	struct spdk_nvmf_rdma_qpair *rqpair = ctx;
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	/* In non SRQ path, we will reach rqpair->max_queue_depth. In SRQ path, we will get the last_wqe event. */
	if (rqpair->current_send_depth != 0) {
		return;
	}

	if (rqpair->srq == NULL && rqpair->current_recv_depth != rqpair->max_queue_depth) {
		return;
	}

	if (rqpair->srq != NULL && rqpair->last_wqe_reached == false) {
		return;
	}

	spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair, true);

	/* Qpair will be destroyed after nvmf layer closes this qpair */
	if (rqpair->qpair.state != SPDK_NVMF_QPAIR_ERROR) {
		return;
	}

	spdk_nvmf_rdma_qpair_destroy(rqpair);
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

	spdk_trace_record(TRACE_RDMA_QP_DISCONNECT, 0, 0, (uintptr_t)rqpair->cm_id, 0);

	spdk_nvmf_rdma_start_disconnect(rqpair);

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

static bool
nvmf_rdma_handle_cm_event_addr_change(struct spdk_nvmf_transport *transport,
				      struct rdma_cm_event *event)
{
	struct spdk_nvme_transport_id		trid;
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*rpoller;
	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_transport		*rtransport;
	uint32_t				ref, i;
	bool					event_acked = false;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	TAILQ_FOREACH(port, &rtransport->ports, link) {
		if (port->id == event->id) {
			SPDK_ERRLOG("ADDR_CHANGE: IP %s:%s migrated\n", port->trid.traddr, port->trid.trsvcid);
			rdma_ack_cm_event(event);
			event_acked = true;
			trid = port->trid;
			ref = port->ref;
			break;
		}
	}
	if (event_acked) {
		TAILQ_FOREACH(rgroup, &rtransport->poll_groups, link) {
			TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
				TAILQ_FOREACH(rqpair, &rpoller->qpairs, link) {
					if (rqpair->listen_id == port->id) {
						spdk_nvmf_rdma_start_disconnect(rqpair);
					}
				}
			}
		}

		for (i = 0; i < ref; i++) {
			spdk_nvmf_rdma_stop_listen(transport, &trid);
		}
		while (ref > 0) {
			spdk_nvmf_rdma_listen(transport, &trid, NULL, NULL);
			ref--;
		}
	}
	return event_acked;
}

static void
spdk_nvmf_process_cm_event(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn, void *cb_arg)
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
		if (rc == 0) {
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Acceptor Event: %s\n", CM_EVENT_STR[event->event]);

			spdk_trace_record(TRACE_RDMA_CM_ASYNC_EVENT, 0, 0, 0, event->event);

			switch (event->event) {
			case RDMA_CM_EVENT_ADDR_RESOLVED:
			case RDMA_CM_EVENT_ADDR_ERROR:
			case RDMA_CM_EVENT_ROUTE_RESOLVED:
			case RDMA_CM_EVENT_ROUTE_ERROR:
				/* No action required. The target never attempts to resolve routes. */
				break;
			case RDMA_CM_EVENT_CONNECT_REQUEST:
				rc = nvmf_rdma_connect(transport, event, cb_fn, cb_arg);
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
			case RDMA_CM_EVENT_DEVICE_REMOVAL:
				rc = nvmf_rdma_disconnect(event);
				if (rc < 0) {
					SPDK_ERRLOG("Unable to process disconnect event. rc: %d\n", rc);
					break;
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
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("Acceptor Event Error: %s\n", spdk_strerror(errno));
			}
			break;
		}
	}
}

static void
nvmf_rdma_handle_qp_fatal(struct spdk_nvmf_rdma_qpair *rqpair)
{
	spdk_nvmf_rdma_update_ibv_state(rqpair);
	spdk_nvmf_rdma_start_disconnect(rqpair);
}

static void
nvmf_rdma_handle_last_wqe_reached(struct spdk_nvmf_rdma_qpair *rqpair)
{
	rqpair->last_wqe_reached = true;
	nvmf_rdma_destroy_drained_qpair(rqpair);
}

static void
nvmf_rdma_handle_sq_drained(struct spdk_nvmf_rdma_qpair *rqpair)
{
	spdk_nvmf_rdma_start_disconnect(rqpair);
}

static void
spdk_nvmf_rdma_qpair_process_ibv_event(void *ctx)
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
spdk_nvmf_rdma_send_qpair_async_event(struct spdk_nvmf_rdma_qpair *rqpair,
				      spdk_nvmf_rdma_qpair_ibv_event fn)
{
	struct spdk_nvmf_rdma_ibv_event_ctx *ctx;

	if (!rqpair->qpair.group) {
		return EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return ENOMEM;
	}

	ctx->rqpair = rqpair;
	ctx->cb_fn = fn;
	STAILQ_INSERT_TAIL(&rqpair->ibv_events, ctx, link);

	return spdk_thread_send_msg(rqpair->qpair.group->thread, spdk_nvmf_rdma_qpair_process_ibv_event,
				    ctx);
}

static void
spdk_nvmf_process_ib_event(struct spdk_nvmf_rdma_device *device)
{
	int				rc;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct ibv_async_event		event;

	rc = ibv_get_async_event(device->context, &event);

	if (rc) {
		SPDK_ERRLOG("Failed to get async_event (%d): %s\n",
			    errno, spdk_strerror(errno));
		return;
	}

	switch (event.event_type) {
	case IBV_EVENT_QP_FATAL:
		rqpair = event.element.qp->qp_context;
		SPDK_ERRLOG("Fatal event received for rqpair %p\n", rqpair);
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair->cm_id, event.event_type);
		if (spdk_nvmf_rdma_send_qpair_async_event(rqpair, nvmf_rdma_handle_qp_fatal)) {
			SPDK_ERRLOG("Failed to send QP_FATAL event for rqpair %p\n", rqpair);
			nvmf_rdma_handle_qp_fatal(rqpair);
		}
		break;
	case IBV_EVENT_QP_LAST_WQE_REACHED:
		/* This event only occurs for shared receive queues. */
		rqpair = event.element.qp->qp_context;
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Last WQE reached event received for rqpair %p\n", rqpair);
		if (spdk_nvmf_rdma_send_qpair_async_event(rqpair, nvmf_rdma_handle_last_wqe_reached)) {
			SPDK_ERRLOG("Failed to send LAST_WQE_REACHED event for rqpair %p\n", rqpair);
			rqpair->last_wqe_reached = true;
		}
		break;
	case IBV_EVENT_SQ_DRAINED:
		/* This event occurs frequently in both error and non-error states.
		 * Check if the qpair is in an error state before sending a message. */
		rqpair = event.element.qp->qp_context;
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Last sq drained event received for rqpair %p\n", rqpair);
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair->cm_id, event.event_type);
		if (spdk_nvmf_rdma_update_ibv_state(rqpair) == IBV_QPS_ERR) {
			if (spdk_nvmf_rdma_send_qpair_async_event(rqpair, nvmf_rdma_handle_sq_drained)) {
				SPDK_ERRLOG("Failed to send SQ_DRAINED event for rqpair %p\n", rqpair);
				nvmf_rdma_handle_sq_drained(rqpair);
			}
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
				  (uintptr_t)rqpair->cm_id, event.event_type);
		spdk_nvmf_rdma_update_ibv_state(rqpair);
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
}

static void
spdk_nvmf_rdma_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn, void *cb_arg)
{
	int	nfds, i = 0;
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_device *device, *tmp;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);
	nfds = poll(rtransport->poll_fds, rtransport->npoll_fds, 0);

	if (nfds <= 0) {
		return;
	}

	/* The first poll descriptor is RDMA CM event */
	if (rtransport->poll_fds[i++].revents & POLLIN) {
		spdk_nvmf_process_cm_event(transport, cb_fn, cb_arg);
		nfds--;
	}

	if (nfds == 0) {
		return;
	}

	/* Second and subsequent poll descriptors are IB async events */
	TAILQ_FOREACH_SAFE(device, &rtransport->devices, link, tmp) {
		if (rtransport->poll_fds[i++].revents & POLLIN) {
			spdk_nvmf_process_ib_event(device);
			nfds--;
		}
	}
	/* check all flagged fd's have been served */
	assert(nfds == 0);
}

static void
spdk_nvmf_rdma_discover(struct spdk_nvmf_transport *transport,
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
spdk_nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_rdma_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_device		*device;
	struct ibv_srq_init_attr		srq_init_attr;
	struct spdk_nvmf_rdma_resource_opts	opts;
	int					num_cqe;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	rgroup = calloc(1, sizeof(*rgroup));
	if (!rgroup) {
		return NULL;
	}

	TAILQ_INIT(&rgroup->pollers);
	STAILQ_INIT(&rgroup->retired_bufs);

	pthread_mutex_lock(&rtransport->lock);
	TAILQ_FOREACH(device, &rtransport->devices, link) {
		poller = calloc(1, sizeof(*poller));
		if (!poller) {
			SPDK_ERRLOG("Unable to allocate memory for new RDMA poller\n");
			spdk_nvmf_rdma_poll_group_destroy(&rgroup->group);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}

		poller->device = device;
		poller->group = rgroup;

		TAILQ_INIT(&poller->qpairs);
		STAILQ_INIT(&poller->qpairs_pending_send);
		STAILQ_INIT(&poller->qpairs_pending_recv);

		TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);
		if (transport->opts.no_srq == false && device->num_srq < device->attr.max_srq) {
			poller->max_srq_depth = transport->opts.max_srq_depth;

			device->num_srq++;
			memset(&srq_init_attr, 0, sizeof(struct ibv_srq_init_attr));
			srq_init_attr.attr.max_wr = poller->max_srq_depth;
			srq_init_attr.attr.max_sge = spdk_min(device->attr.max_sge, NVMF_DEFAULT_RX_SGE);
			poller->srq = ibv_create_srq(device->pd, &srq_init_attr);
			if (!poller->srq) {
				SPDK_ERRLOG("Unable to create shared receive queue, errno %d\n", errno);
				spdk_nvmf_rdma_poll_group_destroy(&rgroup->group);
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
				spdk_nvmf_rdma_poll_group_destroy(&rgroup->group);
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
			num_cqe = DEFAULT_NVMF_RDMA_CQ_SIZE;
		}

		poller->cq = ibv_create_cq(device->context, num_cqe, poller, NULL, 0);
		if (!poller->cq) {
			SPDK_ERRLOG("Unable to create completion queue\n");
			spdk_nvmf_rdma_poll_group_destroy(&rgroup->group);
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
spdk_nvmf_rdma_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
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
spdk_nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup, *next_rgroup;
	struct spdk_nvmf_rdma_poller		*poller, *tmp;
	struct spdk_nvmf_rdma_qpair		*qpair, *tmp_qpair;
	struct spdk_nvmf_transport_pg_cache_buf	*buf, *tmp_buf;
	struct spdk_nvmf_rdma_transport		*rtransport;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	if (!rgroup) {
		return;
	}

	/* free all retired buffers back to the transport so we don't short the mempool. */
	STAILQ_FOREACH_SAFE(buf, &rgroup->retired_bufs, link, tmp_buf) {
		STAILQ_REMOVE(&rgroup->retired_bufs, buf, spdk_nvmf_transport_pg_cache_buf, link);
		assert(group->transport != NULL);
		spdk_mempool_put(group->transport->data_buf_pool, buf);
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		TAILQ_REMOVE(&rgroup->pollers, poller, link);

		TAILQ_FOREACH_SAFE(qpair, &poller->qpairs, link, tmp_qpair) {
			spdk_nvmf_rdma_qpair_destroy(qpair);
		}

		if (poller->srq) {
			nvmf_rdma_resources_destroy(poller->resources);
			ibv_destroy_srq(poller->srq);
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Destroyed RDMA shared queue %p\n", poller->srq);
		}

		if (poller->cq) {
			ibv_destroy_cq(poller->cq);
		}

		free(poller);
	}

	if (rgroup->group.transport == NULL) {
		/* Transport can be NULL when spdk_nvmf_rdma_poll_group_create()
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
spdk_nvmf_rdma_qpair_reject_connection(struct spdk_nvmf_rdma_qpair *rqpair)
{
	if (rqpair->cm_id != NULL) {
		spdk_nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
	}
	spdk_nvmf_rdma_qpair_destroy(rqpair);
}

static int
spdk_nvmf_rdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
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

	rc = spdk_nvmf_rdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		return -1;
	}

	rc = spdk_nvmf_rdma_event_accept(rqpair->cm_id, rqpair);
	if (rc) {
		/* Try to reject, but we probably can't */
		spdk_nvmf_rdma_qpair_reject_connection(rqpair);
		return -1;
	}

	spdk_nvmf_rdma_update_ibv_state(rqpair);

	return 0;
}

static int
spdk_nvmf_rdma_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_rdma_request	*rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	struct spdk_nvmf_rdma_transport	*rtransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_rdma_transport, transport);

	nvmf_rdma_request_free(rdma_req, rtransport);
	return 0;
}

static int
spdk_nvmf_rdma_request_complete(struct spdk_nvmf_request *req)
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

	spdk_nvmf_rdma_request_process(rtransport, rdma_req);

	return 0;
}

static int
spdk_nvmf_rdma_destroy_defunct_qpair(void *ctx)
{
	struct spdk_nvmf_rdma_qpair	*rqpair = ctx;
	struct spdk_nvmf_rdma_transport *rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
			struct spdk_nvmf_rdma_transport, transport);

	SPDK_INFOLOG(SPDK_LOG_RDMA, "QP#%d hasn't been drained as expected, manually destroy it\n",
		     rqpair->qpair.qid);

	spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair, true);
	spdk_nvmf_rdma_qpair_destroy(rqpair);

	return 0;
}

static void
spdk_nvmf_rdma_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	if (rqpair->disconnect_flags & RDMA_QP_DISCONNECTING) {
		return;
	}

	rqpair->disconnect_flags |= RDMA_QP_DISCONNECTING;

	/* This happens only when the qpair is disconnected before
	 * it is added to the poll group. Since there is no poll group,
	 * the RDMA qp has not been initialized yet and the RDMA CM
	 * event has not yet been acknowledged, so we need to reject it.
	 */
	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		spdk_nvmf_rdma_qpair_reject_connection(rqpair);
		return;
	}

	if (rqpair->ibv_state != IBV_QPS_ERR) {
		spdk_nvmf_rdma_set_ibv_state(rqpair, IBV_QPS_ERR);
	}

	rqpair->destruct_poller = spdk_poller_register(spdk_nvmf_rdma_destroy_defunct_qpair, (void *)rqpair,
				  NVMF_RDMA_QPAIR_DESTROY_TIMEOUT_US);
}

static struct spdk_nvmf_rdma_qpair *
get_rdma_qpair_from_wc(struct spdk_nvmf_rdma_poller *rpoller, struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_qpair *rqpair;
	/* @todo: improve QP search */
	TAILQ_FOREACH(rqpair, &rpoller->qpairs, link) {
		if (wc->qp_num == rqpair->cm_id->qp->qp_num) {
			return rqpair;
		}
	}
	SPDK_ERRLOG("Didn't find QP with qp_num %u\n", wc->qp_num);
	return NULL;
}

#ifdef DEBUG
static int
spdk_nvmf_rdma_req_is_completing(struct spdk_nvmf_rdma_request *rdma_req)
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
		spdk_nvmf_rdma_start_disconnect(rdma_recv->qpair);
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
	spdk_nvmf_rdma_start_disconnect(rqpair);
}

static void
_poller_submit_recvs(struct spdk_nvmf_rdma_transport *rtransport,
		     struct spdk_nvmf_rdma_poller *rpoller)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_recv_wr		*bad_recv_wr;
	int				rc;

	if (rpoller->srq) {
		if (rpoller->resources->recvs_to_post.first != NULL) {
			rc = ibv_post_srq_recv(rpoller->srq, rpoller->resources->recvs_to_post.first, &bad_recv_wr);
			if (rc) {
				_poller_reset_failed_recvs(rpoller, bad_recv_wr, rc);
			}
			rpoller->resources->recvs_to_post.first = NULL;
			rpoller->resources->recvs_to_post.last = NULL;
		}
	} else {
		while (!STAILQ_EMPTY(&rpoller->qpairs_pending_recv)) {
			rqpair = STAILQ_FIRST(&rpoller->qpairs_pending_recv);
			assert(rqpair->resources->recvs_to_post.first != NULL);
			rc = ibv_post_recv(rqpair->cm_id->qp, rqpair->resources->recvs_to_post.first, &bad_recv_wr);
			if (rc) {
				_qp_reset_failed_recvs(rqpair, bad_recv_wr, rc);
			}
			rqpair->resources->recvs_to_post.first = NULL;
			rqpair->resources->recvs_to_post.last = NULL;
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

		spdk_nvmf_rdma_request_process(rtransport, cur_rdma_req);
		prev_rdma_req = cur_rdma_req;
	}

	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_ACTIVE) {
		/* Disconnect the connection. */
		spdk_nvmf_rdma_start_disconnect(rqpair);
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
		assert(rqpair->sends_to_post.first != NULL);
		rc = ibv_post_send(rqpair->cm_id->qp, rqpair->sends_to_post.first, &bad_wr);

		/* bad wr always points to the first wr that failed. */
		if (rc) {
			_qp_reset_failed_sends(rtransport, rqpair, bad_wr, rc);
		}
		rqpair->sends_to_post.first = NULL;
		rqpair->sends_to_post.last = NULL;
		STAILQ_REMOVE_HEAD(&rpoller->qpairs_pending_send, send_link);
	}
}

static int
spdk_nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
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
				assert(spdk_nvmf_rdma_req_is_completing(rdma_req));
			} else {
				SPDK_ERRLOG("data=%p length=%u\n", rdma_req->req.data, rdma_req->req.length);
			}

			rdma_req->state = RDMA_REQUEST_STATE_COMPLETED;
			/* +1 for the response wr */
			rqpair->current_send_depth -= rdma_req->num_outstanding_data_wr + 1;
			rdma_req->num_outstanding_data_wr = 0;

			spdk_nvmf_rdma_request_process(rtransport, rdma_req);
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
					rc = ibv_post_srq_recv(rpoller->srq,
							       &rdma_recv->wr,
							       &bad_wr);
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
					spdk_nvmf_rdma_start_disconnect(rqpair);
					break;
				}
			}

			rdma_recv->wr.next = NULL;
			rqpair->current_recv_depth++;
			rdma_recv->receive_tsc = poll_tsc;
			rpoller->stat.requests++;
			STAILQ_INSERT_TAIL(&rqpair->resources->incoming_queue, rdma_recv, link);
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
					spdk_nvmf_rdma_request_process(rtransport, rdma_req);
				}
			} else {
				/* If the data transfer fails still force the queue into the error state,
				 * if we were performing an RDMA_READ, we need to force the request into a
				 * completed state since it wasn't linked to a send. However, in the RDMA_WRITE
				 * case, we should wait for the SEND to complete. */
				SPDK_ERRLOG("data=%p length=%u\n", rdma_req->req.data, rdma_req->req.length);
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
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "CQ error on CQ %p, Request 0x%lu (%d): %s\n",
				      rpoller->cq, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));

			error = true;

			if (rqpair->qpair.state == SPDK_NVMF_QPAIR_ACTIVE) {
				/* Disconnect the connection. */
				spdk_nvmf_rdma_start_disconnect(rqpair);
			} else {
				nvmf_rdma_destroy_drained_qpair(rqpair);
			}
			continue;
		}

		spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair, false);

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

static int
spdk_nvmf_rdma_trid_from_cm_id(struct rdma_cm_id *id,
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
spdk_nvmf_rdma_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return spdk_nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, true);
}

static int
spdk_nvmf_rdma_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return spdk_nvmf_rdma_trid_from_cm_id(rqpair->cm_id, trid, false);
}

static int
spdk_nvmf_rdma_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	return spdk_nvmf_rdma_trid_from_cm_id(rqpair->listen_id, trid, false);
}

void
spdk_nvmf_rdma_init_hooks(struct spdk_nvme_rdma_hooks *hooks)
{
	g_nvmf_hooks = *hooks;
}

static int
spdk_nvmf_rdma_poll_group_get_stat(struct spdk_nvmf_tgt *tgt,
				   struct spdk_nvmf_transport_poll_group_stat **stat)
{
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_transport_poll_group *tgroup;
	struct spdk_nvmf_rdma_poll_group *rgroup;
	struct spdk_nvmf_rdma_poller *rpoller;
	struct spdk_nvmf_rdma_device_stat *device_stat;
	uint64_t num_devices = 0;

	if (tgt == NULL || stat == NULL) {
		return -EINVAL;
	}

	ch = spdk_get_io_channel(tgt);
	group = spdk_io_channel_get_ctx(ch);;
	spdk_put_io_channel(ch);
	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (SPDK_NVME_TRANSPORT_RDMA == tgroup->transport->ops->type) {
			*stat = calloc(1, sizeof(struct spdk_nvmf_transport_poll_group_stat));
			if (!*stat) {
				SPDK_ERRLOG("Failed to allocate memory for NVMf RDMA statistics\n");
				return -ENOMEM;
			}
			(*stat)->trtype = SPDK_NVME_TRANSPORT_RDMA;

			rgroup = SPDK_CONTAINEROF(tgroup, struct spdk_nvmf_rdma_poll_group, group);
			/* Count devices to allocate enough memory */
			TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
				++num_devices;
			}
			(*stat)->rdma.devices = calloc(num_devices, sizeof(struct spdk_nvmf_rdma_device_stat));
			if (!(*stat)->rdma.devices) {
				SPDK_ERRLOG("Failed to allocate NVMf RDMA devices statistics\n");
				free(*stat);
				return -ENOMEM;
			}

			(*stat)->rdma.pending_data_buffer = rgroup->stat.pending_data_buffer;
			(*stat)->rdma.num_devices = num_devices;
			num_devices = 0;
			TAILQ_FOREACH(rpoller, &rgroup->pollers, link) {
				device_stat = &(*stat)->rdma.devices[num_devices++];
				device_stat->name = ibv_get_device_name(rpoller->device->context->device);
				device_stat->polls = rpoller->stat.polls;
				device_stat->completions = rpoller->stat.completions;
				device_stat->requests = rpoller->stat.requests;
				device_stat->request_latency = rpoller->stat.request_latency;
				device_stat->pending_free_request = rpoller->stat.pending_free_request;
				device_stat->pending_rdma_read = rpoller->stat.pending_rdma_read;
				device_stat->pending_rdma_write = rpoller->stat.pending_rdma_write;
			}
			return 0;
		}
	}
	return -ENOENT;
}

static void
spdk_nvmf_rdma_poll_group_free_stat(struct spdk_nvmf_transport_poll_group_stat *stat)
{
	if (stat) {
		free(stat->rdma.devices);
	}
	free(stat);
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.name = "RDMA",
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.opts_init = spdk_nvmf_rdma_opts_init,
	.create = spdk_nvmf_rdma_create,
	.destroy = spdk_nvmf_rdma_destroy,

	.listen = spdk_nvmf_rdma_listen,
	.stop_listen = spdk_nvmf_rdma_stop_listen,
	.accept = spdk_nvmf_rdma_accept,

	.listener_discover = spdk_nvmf_rdma_discover,

	.poll_group_create = spdk_nvmf_rdma_poll_group_create,
	.get_optimal_poll_group = spdk_nvmf_rdma_get_optimal_poll_group,
	.poll_group_destroy = spdk_nvmf_rdma_poll_group_destroy,
	.poll_group_add = spdk_nvmf_rdma_poll_group_add,
	.poll_group_poll = spdk_nvmf_rdma_poll_group_poll,

	.req_free = spdk_nvmf_rdma_request_free,
	.req_complete = spdk_nvmf_rdma_request_complete,

	.qpair_fini = spdk_nvmf_rdma_close_qpair,
	.qpair_get_peer_trid = spdk_nvmf_rdma_qpair_get_peer_trid,
	.qpair_get_local_trid = spdk_nvmf_rdma_qpair_get_local_trid,
	.qpair_get_listen_trid = spdk_nvmf_rdma_qpair_get_listen_trid,

	.poll_group_get_stat = spdk_nvmf_rdma_poll_group_get_stat,
	.poll_group_free_stat = spdk_nvmf_rdma_poll_group_free_stat,
};

SPDK_NVMF_TRANSPORT_REGISTER(rdma, &spdk_nvmf_transport_rdma);
SPDK_LOG_REGISTER_COMPONENT("rdma", SPDK_LOG_RDMA)
