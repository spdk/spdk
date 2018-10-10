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

#include "spdk/stdinc.h"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/config.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/*
 RDMA Connection Resource Defaults
 */
#define NVMF_DEFAULT_TX_SGE		1
#define NVMF_DEFAULT_RX_SGE		2
#define NVMF_DEFAULT_DATA_SGE		16

/* The RDMA completion queue size */
#define NVMF_RDMA_CQ_SIZE	4096

/* AIO backend requires block size aligned data buffers,
 * extra 4KiB aligned data buffer should work for most devices.
 */
#define SHIFT_4KB			12
#define NVMF_DATA_BUFFER_ALIGNMENT	(1 << SHIFT_4KB)
#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1)

enum spdk_nvmf_rdma_request_state {
	/* The request is not currently in use */
	RDMA_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	RDMA_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	RDMA_REQUEST_STATE_NEED_BUFFER,

	/* The request is waiting on RDMA queue depth availability
	 * to transfer data between the host and the controller.
	 */
	RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING,

	/* The request is currently transferring data from the host to the controller. */
	RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	RDMA_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	RDMA_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	RDMA_REQUEST_STATE_EXECUTED,

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

#define									TRACE_GROUP_NVMF_RDMA 0x4
#define TRACE_RDMA_REQUEST_STATE_NEW					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x0)
#define TRACE_RDMA_REQUEST_STATE_NEED_BUFFER				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x1)
#define TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x2)
#define TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x3)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x4)
#define TRACE_RDMA_REQUEST_STATE_EXECUTING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x5)
#define TRACE_RDMA_REQUEST_STATE_EXECUTED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x6)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x7)
#define TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x8)
#define TRACE_RDMA_REQUEST_STATE_COMPLETING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x9)
#define TRACE_RDMA_REQUEST_STATE_COMPLETED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xA)
#define TRACE_RDMA_QP_CREATE						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xB)
#define TRACE_RDMA_IBV_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xC)
#define TRACE_RDMA_CM_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xD)
#define TRACE_RDMA_QP_STATE_CHANGE					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xE)
#define TRACE_RDMA_QP_DISCONNECT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xF)
#define TRACE_RDMA_QP_DESTROY						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x10)

SPDK_TRACE_REGISTER_FN(nvmf_trace)
{
	spdk_trace_register_object(OBJECT_NVMF_RDMA_IO, 'r');
	spdk_trace_register_description("RDMA_REQ_NEW", "",
					TRACE_RDMA_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 1, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_NEED_BUFFER", "",
					TRACE_RDMA_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_H_TO_C", "",
					TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_TX_H_TO_C", "",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_EXECUTE", "",
					TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_EXECUTING", "",
					TRACE_RDMA_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_EXECUTED", "",
					TRACE_RDMA_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_COMPLETE", "",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_COMPLETING_CONTROLLER_TO_HOST", "",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_COMPLETING_INCAPSULE", "",
					TRACE_RDMA_REQUEST_STATE_COMPLETING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");
	spdk_trace_register_description("RDMA_REQ_COMPLETED", "",
					TRACE_RDMA_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 1, "cmid:   ");

	spdk_trace_register_description("RDMA_QP_CREATE", "", TRACE_RDMA_QP_CREATE,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("RDMA_IBV_ASYNC_EVENT", "", TRACE_RDMA_IBV_ASYNC_EVENT,
					OWNER_NONE, OBJECT_NONE, 0, 0, "type:   ");
	spdk_trace_register_description("RDMA_CM_ASYNC_EVENT", "", TRACE_RDMA_CM_ASYNC_EVENT,
					OWNER_NONE, OBJECT_NONE, 0, 0, "type:   ");
	spdk_trace_register_description("RDMA_QP_STATE_CHANGE", "", TRACE_RDMA_QP_STATE_CHANGE,
					OWNER_NONE, OBJECT_NONE, 0, 1, "state:  ");
	spdk_trace_register_description("RDMA_QP_DISCONNECT", "", TRACE_RDMA_QP_DISCONNECT,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("RDMA_QP_DESTROY", "", TRACE_RDMA_QP_DESTROY,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
}

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_rdma_recv {
	struct ibv_recv_wr		wr;
	struct ibv_sge			sgl[NVMF_DEFAULT_RX_SGE];

	struct spdk_nvmf_rdma_qpair	*qpair;

	/* In-capsule data buffer */
	uint8_t				*buf;

	TAILQ_ENTRY(spdk_nvmf_rdma_recv) link;
};

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;
	bool					data_from_pool;

	enum spdk_nvmf_rdma_request_state	state;

	struct spdk_nvmf_rdma_recv		*recv;

	struct {
		struct	ibv_send_wr		wr;
		struct	ibv_sge			sgl[NVMF_DEFAULT_TX_SGE];
	} rsp;

	struct {
		struct ibv_send_wr		wr;
		struct ibv_sge			sgl[SPDK_NVMF_MAX_SGL_ENTRIES];
		void				*buffers[SPDK_NVMF_MAX_SGL_ENTRIES];
	} data;

	TAILQ_ENTRY(spdk_nvmf_rdma_request)	link;
	TAILQ_ENTRY(spdk_nvmf_rdma_request)	state_link;
};

struct spdk_nvmf_rdma_qpair {
	struct spdk_nvmf_qpair			qpair;

	struct spdk_nvmf_rdma_port		*port;
	struct spdk_nvmf_rdma_poller		*poller;

	struct rdma_cm_id			*cm_id;
	struct rdma_cm_id			*listen_id;

	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* The maximum number of active RDMA READ and WRITE operations at one time */
	uint16_t				max_rw_depth;

	/* Receives that are waiting for a request object */
	TAILQ_HEAD(, spdk_nvmf_rdma_recv)	incoming_queue;

	/* Queues to track the requests in all states */
	TAILQ_HEAD(, spdk_nvmf_rdma_request)	state_queue[RDMA_REQUEST_NUM_STATES];

	/* Number of requests in each state */
	uint32_t				state_cntr[RDMA_REQUEST_NUM_STATES];

	int                                     max_sge;

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

	TAILQ_ENTRY(spdk_nvmf_rdma_qpair)	link;

	/* Mgmt channel */
	struct spdk_io_channel			*mgmt_channel;
	struct spdk_nvmf_rdma_mgmt_channel	*ch;

	/* IBV queue pair attributes: they are used to manage
	 * qp state and recover from errors.
	 */
	struct ibv_qp_init_attr			ibv_init_attr;
	struct ibv_qp_attr			ibv_attr;

	bool					qpair_disconnected;

	/* Reference counter for how many unprocessed messages
	 * from other threads are currently outstanding. The
	 * qpair cannot be destroyed until this is 0. This is
	 * atomically incremented from any thread, but only
	 * decremented and read from the thread that owns this
	 * qpair.
	 */
	uint32_t				refcnt;
};

struct spdk_nvmf_rdma_poller {
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poll_group	*group;

	struct ibv_cq				*cq;

	TAILQ_HEAD(, spdk_nvmf_rdma_qpair)	qpairs;

	TAILQ_ENTRY(spdk_nvmf_rdma_poller)	link;
};

struct spdk_nvmf_rdma_poll_group {
	struct spdk_nvmf_transport_poll_group	group;

	TAILQ_HEAD(, spdk_nvmf_rdma_poller)	pollers;
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
struct spdk_nvmf_rdma_device {
	struct ibv_device_attr			attr;
	struct ibv_context			*context;

	struct spdk_mem_map			*map;
	struct ibv_pd				*pd;

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

	struct rdma_event_channel	*event_channel;

	struct spdk_mempool		*data_buf_pool;

	pthread_mutex_t			lock;

	/* fields used to poll RDMA/IB events */
	nfds_t			npoll_fds;
	struct pollfd		*poll_fds;

	TAILQ_HEAD(, spdk_nvmf_rdma_device)	devices;
	TAILQ_HEAD(, spdk_nvmf_rdma_port)	ports;
};

struct spdk_nvmf_rdma_mgmt_channel {
	/* Requests that are waiting to obtain a data buffer */
	TAILQ_HEAD(, spdk_nvmf_rdma_request)	pending_data_buf_queue;
};

static inline void
spdk_nvmf_rdma_qpair_inc_refcnt(struct spdk_nvmf_rdma_qpair *rqpair)
{
	__sync_fetch_and_add(&rqpair->refcnt, 1);
}

static inline uint32_t
spdk_nvmf_rdma_qpair_dec_refcnt(struct spdk_nvmf_rdma_qpair *rqpair)
{
	uint32_t old_refcnt, new_refcnt;

	do {
		old_refcnt = rqpair->refcnt;
		assert(old_refcnt > 0);
		new_refcnt = old_refcnt - 1;
	} while (__sync_bool_compare_and_swap(&rqpair->refcnt, old_refcnt, new_refcnt) == false);

	return new_refcnt;
}

/* API to IBV QueuePair */
static const char *str_ibv_qp_state[] = {
	"IBV_QPS_RESET",
	"IBV_QPS_INIT",
	"IBV_QPS_RTR",
	"IBV_QPS_RTS",
	"IBV_QPS_SQD",
	"IBV_QPS_SQE",
	"IBV_QPS_ERR"
};

static enum ibv_qp_state
spdk_nvmf_rdma_update_ibv_state(struct spdk_nvmf_rdma_qpair *rqpair) {
	enum ibv_qp_state old_state, new_state;
	int rc;

	/* All the attributes needed for recovery */
	static int spdk_nvmf_ibv_attr_mask =
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

	old_state = rqpair->ibv_attr.qp_state;
	rc = ibv_query_qp(rqpair->cm_id->qp, &rqpair->ibv_attr,
			  spdk_nvmf_ibv_attr_mask, &rqpair->ibv_init_attr);

	if (rc)
	{
		SPDK_ERRLOG("Failed to get updated RDMA queue pair state!\n");
		assert(false);
	}

	new_state = rqpair->ibv_attr.qp_state;
	if (old_state != new_state)
	{
		spdk_trace_record(TRACE_RDMA_QP_STATE_CHANGE, 0, 0,
				  (uintptr_t)rqpair->cm_id, new_state);
	}
	return new_state;
}

static int
spdk_nvmf_rdma_set_ibv_state(struct spdk_nvmf_rdma_qpair *rqpair,
			     enum ibv_qp_state new_state)
{
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

	switch (new_state) {
	case IBV_QPS_RESET:
	case IBV_QPS_INIT:
	case IBV_QPS_RTR:
	case IBV_QPS_RTS:
	case IBV_QPS_SQD:
	case IBV_QPS_SQE:
	case IBV_QPS_ERR:
		break;
	default:
		SPDK_ERRLOG("QP#%d: bad state requested: %u\n",
			    rqpair->qpair.qid, new_state);
		return -1;
	}
	rqpair->ibv_attr.cur_qp_state = rqpair->ibv_attr.qp_state;
	rqpair->ibv_attr.qp_state = new_state;
	rqpair->ibv_attr.ah_attr.port_num = rqpair->ibv_attr.port_num;

	rc = ibv_modify_qp(rqpair->cm_id->qp, &rqpair->ibv_attr,
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
	SPDK_NOTICELOG("IBV QP#%u changed to: %s\n", rqpair->qpair.qid,
		       str_ibv_qp_state[state]);
	return 0;
}

static void
spdk_nvmf_rdma_request_set_state(struct spdk_nvmf_rdma_request *rdma_req,
				 enum spdk_nvmf_rdma_request_state state)
{
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;

	qpair = rdma_req->req.qpair;
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	TAILQ_REMOVE(&rqpair->state_queue[rdma_req->state], rdma_req, state_link);
	rqpair->state_cntr[rdma_req->state]--;

	rdma_req->state = state;

	TAILQ_INSERT_TAIL(&rqpair->state_queue[rdma_req->state], rdma_req, state_link);
	rqpair->state_cntr[rdma_req->state]++;
}

static int
spdk_nvmf_rdma_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_rdma_mgmt_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->pending_data_buf_queue);
	return 0;
}

static void
spdk_nvmf_rdma_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_rdma_mgmt_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->pending_data_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on channel destruction\n");
	}
}

static int
spdk_nvmf_rdma_cur_rw_depth(struct spdk_nvmf_rdma_qpair *rqpair)
{
	return rqpair->state_cntr[RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER] +
	       rqpair->state_cntr[RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST];
}

static int
spdk_nvmf_rdma_cur_queue_depth(struct spdk_nvmf_rdma_qpair *rqpair)
{
	return rqpair->max_queue_depth -
	       rqpair->state_cntr[RDMA_REQUEST_STATE_FREE];
}

static void
spdk_nvmf_rdma_qpair_destroy(struct spdk_nvmf_rdma_qpair *rqpair)
{
	spdk_trace_record(TRACE_RDMA_QP_DESTROY, 0, 0, (uintptr_t)rqpair->cm_id, 0);

	if (spdk_nvmf_rdma_cur_queue_depth(rqpair)) {
		rqpair->qpair_disconnected = true;
		return;
	}

	if (rqpair->refcnt > 0) {
		return;
	}

	if (rqpair->poller) {
		TAILQ_REMOVE(&rqpair->poller->qpairs, rqpair, link);
	}

	if (rqpair->cmds_mr) {
		ibv_dereg_mr(rqpair->cmds_mr);
	}

	if (rqpair->cpls_mr) {
		ibv_dereg_mr(rqpair->cpls_mr);
	}

	if (rqpair->bufs_mr) {
		ibv_dereg_mr(rqpair->bufs_mr);
	}

	if (rqpair->cm_id) {
		rdma_destroy_qp(rqpair->cm_id);
		rdma_destroy_id(rqpair->cm_id);
	}

	if (rqpair->mgmt_channel) {
		spdk_put_io_channel(rqpair->mgmt_channel);
	}

	/* Free all memory */
	spdk_dma_free(rqpair->cmds);
	spdk_dma_free(rqpair->cpls);
	spdk_dma_free(rqpair->bufs);
	free(rqpair->reqs);
	free(rqpair->recvs);
	free(rqpair);
}

static int
spdk_nvmf_rdma_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	int				rc, i;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_transport      *transport;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
	transport = &rtransport->transport;

	memset(&rqpair->ibv_init_attr, 0, sizeof(struct ibv_qp_init_attr));
	rqpair->ibv_init_attr.qp_context	= rqpair;
	rqpair->ibv_init_attr.qp_type		= IBV_QPT_RC;
	rqpair->ibv_init_attr.send_cq		= rqpair->poller->cq;
	rqpair->ibv_init_attr.recv_cq		= rqpair->poller->cq;
	rqpair->ibv_init_attr.cap.max_send_wr	= rqpair->max_queue_depth *
			2; /* SEND, READ, and WRITE operations */
	rqpair->ibv_init_attr.cap.max_recv_wr	= rqpair->max_queue_depth; /* RECV operations */
	rqpair->ibv_init_attr.cap.max_send_sge	= rqpair->max_sge;
	rqpair->ibv_init_attr.cap.max_recv_sge	= NVMF_DEFAULT_RX_SGE;

	rc = rdma_create_qp(rqpair->cm_id, rqpair->port->device->pd, &rqpair->ibv_init_attr);
	if (rc) {
		SPDK_ERRLOG("rdma_create_qp failed: errno %d: %s\n", errno, spdk_strerror(errno));
		rdma_destroy_id(rqpair->cm_id);
		rqpair->cm_id = NULL;
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}

	spdk_trace_record(TRACE_RDMA_QP_CREATE, 0, 0, (uintptr_t)rqpair->cm_id, 0);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "New RDMA Connection: %p\n", qpair);

	rqpair->reqs = calloc(rqpair->max_queue_depth, sizeof(*rqpair->reqs));
	rqpair->recvs = calloc(rqpair->max_queue_depth, sizeof(*rqpair->recvs));
	rqpair->cmds = spdk_dma_zmalloc(rqpair->max_queue_depth * sizeof(*rqpair->cmds),
					0x1000, NULL);
	rqpair->cpls = spdk_dma_zmalloc(rqpair->max_queue_depth * sizeof(*rqpair->cpls),
					0x1000, NULL);


	if (transport->opts.in_capsule_data_size > 0) {
		rqpair->bufs = spdk_dma_zmalloc(rqpair->max_queue_depth *
						transport->opts.in_capsule_data_size,
						0x1000, NULL);
	}

	if (!rqpair->reqs || !rqpair->recvs || !rqpair->cmds ||
	    !rqpair->cpls || (transport->opts.in_capsule_data_size && !rqpair->bufs)) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for RDMA queue.\n");
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}

	rqpair->cmds_mr = ibv_reg_mr(rqpair->cm_id->pd, rqpair->cmds,
				     rqpair->max_queue_depth * sizeof(*rqpair->cmds),
				     IBV_ACCESS_LOCAL_WRITE);
	rqpair->cpls_mr = ibv_reg_mr(rqpair->cm_id->pd, rqpair->cpls,
				     rqpair->max_queue_depth * sizeof(*rqpair->cpls),
				     0);

	if (transport->opts.in_capsule_data_size) {
		rqpair->bufs_mr = ibv_reg_mr(rqpair->cm_id->pd, rqpair->bufs,
					     rqpair->max_queue_depth *
					     transport->opts.in_capsule_data_size,
					     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	}

	if (!rqpair->cmds_mr || !rqpair->cpls_mr || (transport->opts.in_capsule_data_size &&
			!rqpair->bufs_mr)) {
		SPDK_ERRLOG("Unable to register required memory for RDMA queue.\n");
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Command Array: %p Length: %lx LKey: %x\n",
		      rqpair->cmds, rqpair->max_queue_depth * sizeof(*rqpair->cmds), rqpair->cmds_mr->lkey);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Completion Array: %p Length: %lx LKey: %x\n",
		      rqpair->cpls, rqpair->max_queue_depth * sizeof(*rqpair->cpls), rqpair->cpls_mr->lkey);
	if (rqpair->bufs && rqpair->bufs_mr) {
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "In Capsule Data Array: %p Length: %x LKey: %x\n",
			      rqpair->bufs, rqpair->max_queue_depth *
			      transport->opts.in_capsule_data_size, rqpair->bufs_mr->lkey);
	}

	/* Initialise request state queues and counters of the queue pair */
	for (i = RDMA_REQUEST_STATE_FREE; i < RDMA_REQUEST_NUM_STATES; i++) {
		TAILQ_INIT(&rqpair->state_queue[i]);
		rqpair->state_cntr[i] = 0;
	}

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		struct ibv_recv_wr *bad_wr = NULL;

		rdma_recv = &rqpair->recvs[i];
		rdma_recv->qpair = rqpair;

		/* Set up memory to receive commands */
		if (rqpair->bufs) {
			rdma_recv->buf = (void *)((uintptr_t)rqpair->bufs + (i *
						  transport->opts.in_capsule_data_size));
		}

		rdma_recv->sgl[0].addr = (uintptr_t)&rqpair->cmds[i];
		rdma_recv->sgl[0].length = sizeof(rqpair->cmds[i]);
		rdma_recv->sgl[0].lkey = rqpair->cmds_mr->lkey;
		rdma_recv->wr.num_sge = 1;

		if (rdma_recv->buf && rqpair->bufs_mr) {
			rdma_recv->sgl[1].addr = (uintptr_t)rdma_recv->buf;
			rdma_recv->sgl[1].length = transport->opts.in_capsule_data_size;
			rdma_recv->sgl[1].lkey = rqpair->bufs_mr->lkey;
			rdma_recv->wr.num_sge++;
		}

		rdma_recv->wr.wr_id = (uintptr_t)rdma_recv;
		rdma_recv->wr.sg_list = rdma_recv->sgl;

		rc = ibv_post_recv(rqpair->cm_id->qp, &rdma_recv->wr, &bad_wr);
		if (rc) {
			SPDK_ERRLOG("Unable to post capsule for RDMA RECV\n");
			spdk_nvmf_rdma_qpair_destroy(rqpair);
			return -1;
		}
	}

	for (i = 0; i < rqpair->max_queue_depth; i++) {
		rdma_req = &rqpair->reqs[i];

		rdma_req->req.qpair = &rqpair->qpair;
		rdma_req->req.cmd = NULL;

		/* Set up memory to send responses */
		rdma_req->req.rsp = &rqpair->cpls[i];

		rdma_req->rsp.sgl[0].addr = (uintptr_t)&rqpair->cpls[i];
		rdma_req->rsp.sgl[0].length = sizeof(rqpair->cpls[i]);
		rdma_req->rsp.sgl[0].lkey = rqpair->cpls_mr->lkey;

		rdma_req->rsp.wr.wr_id = (uintptr_t)rdma_req;
		rdma_req->rsp.wr.next = NULL;
		rdma_req->rsp.wr.opcode = IBV_WR_SEND;
		rdma_req->rsp.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->rsp.wr.sg_list = rdma_req->rsp.sgl;
		rdma_req->rsp.wr.num_sge = SPDK_COUNTOF(rdma_req->rsp.sgl);

		/* Set up memory for data buffers */
		rdma_req->data.wr.wr_id = (uint64_t)rdma_req;
		rdma_req->data.wr.next = NULL;
		rdma_req->data.wr.send_flags = IBV_SEND_SIGNALED;
		rdma_req->data.wr.sg_list = rdma_req->data.sgl;
		rdma_req->data.wr.num_sge = SPDK_COUNTOF(rdma_req->data.sgl);

		/* Initialize request state to FREE */
		rdma_req->state = RDMA_REQUEST_STATE_FREE;
		TAILQ_INSERT_TAIL(&rqpair->state_queue[rdma_req->state], rdma_req, state_link);
		rqpair->state_cntr[rdma_req->state]++;
	}

	return 0;
}

static int
request_transfer_in(struct spdk_nvmf_request *req)
{
	int				rc;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_send_wr		*bad_wr = NULL;

	qpair = req->qpair;
	rdma_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_rdma_request, req);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA READ POSTED. Request: %p Connection: %p\n", req, qpair);

	rdma_req->data.wr.opcode = IBV_WR_RDMA_READ;
	rdma_req->data.wr.next = NULL;
	rc = ibv_post_send(rqpair->cm_id->qp, &rdma_req->data.wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to transfer data from host to target\n");
		return -1;
	}
	return 0;
}

static int
request_transfer_out(struct spdk_nvmf_request *req, int *data_posted)
{
	int				rc;
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvme_cpl		*rsp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	struct ibv_send_wr		*send_wr, *bad_send_wr = NULL;

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

	/* Post the capsule to the recv buffer */
	assert(rdma_req->recv != NULL);
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA RECV POSTED. Recv: %p Connection: %p\n", rdma_req->recv,
		      rqpair);
	rc = ibv_post_recv(rqpair->cm_id->qp, &rdma_req->recv->wr, &bad_recv_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return rc;
	}
	rdma_req->recv = NULL;

	/* Build the response which consists of an optional
	 * RDMA WRITE to transfer data, plus an RDMA SEND
	 * containing the response.
	 */
	send_wr = &rdma_req->rsp.wr;

	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA WRITE POSTED. Request: %p Connection: %p\n", req, qpair);

		rdma_req->data.wr.opcode = IBV_WR_RDMA_WRITE;

		rdma_req->data.wr.next = send_wr;
		*data_posted = 1;
		send_wr = &rdma_req->data.wr;
	}

	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "RDMA SEND POSTED. Request: %p Connection: %p\n", req, qpair);

	/* Send the completion */
	rc = ibv_post_send(rqpair->cm_id->qp, send_wr, &bad_send_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to send response capsule\n");
	}

	return rc;
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
		ctrlr_event_data.initiator_depth = rqpair->max_rw_depth;
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
		  new_qpair_fn cb_fn)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct spdk_nvmf_rdma_qpair	*rqpair = NULL;
	struct spdk_nvmf_rdma_port	*port;
	struct rdma_conn_param		*rdma_param = NULL;
	const struct spdk_nvmf_rdma_request_private_data *private_data = NULL;
	uint16_t			max_queue_depth;
	uint16_t			max_rw_depth;

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
	max_rw_depth = rtransport->transport.opts.max_queue_depth;
	SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Target Max Queue Depth: %d\n",
		      rtransport->transport.opts.max_queue_depth);

	/* Next check the local NIC's hardware limitations */
	SPDK_DEBUGLOG(SPDK_LOG_RDMA,
		      "Local NIC Max Send/Recv Queue Depth: %d Max Read/Write Queue Depth: %d\n",
		      port->device->attr.max_qp_wr, port->device->attr.max_qp_rd_atom);
	max_queue_depth = spdk_min(max_queue_depth, port->device->attr.max_qp_wr);
	max_rw_depth = spdk_min(max_rw_depth, port->device->attr.max_qp_rd_atom);

	/* Next check the remote NIC's hardware limitations */
	SPDK_DEBUGLOG(SPDK_LOG_RDMA,
		      "Host (Initiator) NIC Max Incoming RDMA R/W operations: %d Max Outgoing RDMA R/W operations: %d\n",
		      rdma_param->initiator_depth, rdma_param->responder_resources);
	if (rdma_param->initiator_depth > 0) {
		max_rw_depth = spdk_min(max_rw_depth, rdma_param->initiator_depth);
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
		      max_queue_depth, max_rw_depth);

	rqpair = calloc(1, sizeof(struct spdk_nvmf_rdma_qpair));
	if (rqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_nvmf_rdma_event_reject(event->id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		return -1;
	}

	rqpair->port = port;
	rqpair->max_queue_depth = max_queue_depth;
	rqpair->max_rw_depth = max_rw_depth;
	rqpair->cm_id = event->id;
	rqpair->listen_id = event->listen_id;
	rqpair->qpair.transport = transport;
	rqpair->max_sge = spdk_min(port->device->attr.max_sge, SPDK_NVMF_MAX_SGL_ENTRIES);
	TAILQ_INIT(&rqpair->incoming_queue);
	event->id->context = &rqpair->qpair;

	cb_fn(&rqpair->qpair);

	return 0;
}

static void
_nvmf_rdma_disconnect(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;
	struct spdk_nvmf_rdma_qpair *rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	spdk_nvmf_rdma_qpair_dec_refcnt(rqpair);

	spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
}

static void
_nvmf_rdma_disconnect_retry(void *ctx)
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
		spdk_thread_send_msg(spdk_get_thread(), _nvmf_rdma_disconnect_retry, qpair);
		return;
	}

	spdk_thread_send_msg(group->thread, _nvmf_rdma_disconnect, qpair);
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

	spdk_nvmf_rdma_update_ibv_state(rqpair);
	spdk_nvmf_rdma_qpair_inc_refcnt(rqpair);

	_nvmf_rdma_disconnect_retry(qpair);

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
spdk_nvmf_process_cm_event(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_rdma_transport *rtransport;
	struct rdma_cm_event		*event;
	int				rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	if (rtransport->event_channel == NULL) {
		return;
	}

	while (1) {
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
				rc = nvmf_rdma_connect(transport, event, cb_fn);
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
				/* Not utilizing this event */
				break;
			case RDMA_CM_EVENT_TIMEWAIT_EXIT:
				/* For now, do nothing. The target never re-uses queue pairs. */
				break;
			default:
				SPDK_ERRLOG("Unexpected Acceptor Event [%d]\n", event->event);
				break;
			}

			rdma_ack_cm_event(event);
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("Acceptor Event Error: %s\n", spdk_strerror(errno));
			}
			break;
		}
	}
}

static int
spdk_nvmf_rdma_mem_notify(void *cb_ctx, struct spdk_mem_map *map,
			  enum spdk_mem_map_notify_action action,
			  void *vaddr, size_t size)
{
	struct spdk_nvmf_rdma_device *device = cb_ctx;
	struct ibv_pd *pd = device->pd;
	struct ibv_mr *mr;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		mr = ibv_reg_mr(pd, vaddr, size,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
		if (mr == NULL) {
			SPDK_ERRLOG("ibv_reg_mr() failed\n");
			return -1;
		} else {
			spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)mr);
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		mr = (struct ibv_mr *)spdk_mem_map_translate(map, (uint64_t)vaddr, NULL);
		spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);
		if (mr) {
			ibv_dereg_mr(mr);
		}
		break;
	}

	return 0;
}

typedef enum spdk_nvme_data_transfer spdk_nvme_data_transfer_t;

static spdk_nvme_data_transfer_t
spdk_nvmf_rdma_request_get_xfer(struct spdk_nvmf_rdma_request *rdma_req)
{
	enum spdk_nvme_data_transfer xfer;
	struct spdk_nvme_cmd *cmd = &rdma_req->req.cmd->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;

#ifdef SPDK_CONFIG_RDMA_SEND_WITH_INVAL
	rdma_req->rsp.wr.opcode = IBV_WR_SEND;
	rdma_req->rsp.wr.imm_data = 0;
#endif

	/* Figure out data transfer direction */
	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		xfer = spdk_nvme_opc_get_data_transfer(rdma_req->req.cmd->nvmf_cmd.fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

		/* Some admin commands are special cases */
		if ((rdma_req->req.qpair->qid == 0) &&
		    ((cmd->opc == SPDK_NVME_OPC_GET_FEATURES) ||
		     (cmd->opc == SPDK_NVME_OPC_SET_FEATURES))) {
			switch (cmd->cdw10 & 0xff) {
			case SPDK_NVME_FEAT_LBA_RANGE_TYPE:
			case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION:
			case SPDK_NVME_FEAT_HOST_IDENTIFIER:
				break;
			default:
				xfer = SPDK_NVME_DATA_NONE;
			}
		}
	}

	if (xfer == SPDK_NVME_DATA_NONE) {
		return xfer;
	}

	/* Even for commands that may transfer data, they could have specified 0 length.
	 * We want those to show up with xfer SPDK_NVME_DATA_NONE.
	 */
	switch (sgl->generic.type) {
	case SPDK_NVME_SGL_TYPE_DATA_BLOCK:
	case SPDK_NVME_SGL_TYPE_BIT_BUCKET:
	case SPDK_NVME_SGL_TYPE_SEGMENT:
	case SPDK_NVME_SGL_TYPE_LAST_SEGMENT:
	case SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK:
		if (sgl->unkeyed.length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	case SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK:
		if (sgl->keyed.length == 0) {
			xfer = SPDK_NVME_DATA_NONE;
		}
		break;
	}

	return xfer;
}

static int
spdk_nvmf_rdma_request_fill_iovs(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_device *device,
				 struct spdk_nvmf_rdma_request *rdma_req)
{
	void		*buf = NULL;
	uint32_t	length = rdma_req->req.length;
	uint32_t	i = 0;

	rdma_req->req.iovcnt = 0;
	while (length) {
		buf = spdk_mempool_get(rtransport->data_buf_pool);
		if (!buf) {
			goto nomem;
		}

		rdma_req->req.iov[i].iov_base = (void *)((uintptr_t)(buf + NVMF_DATA_BUFFER_MASK) &
						~NVMF_DATA_BUFFER_MASK);
		rdma_req->req.iov[i].iov_len  = spdk_min(length, rtransport->transport.opts.io_unit_size);
		rdma_req->req.iovcnt++;
		rdma_req->data.buffers[i] = buf;
		rdma_req->data.wr.sg_list[i].addr = (uintptr_t)(rdma_req->req.iov[i].iov_base);
		rdma_req->data.wr.sg_list[i].length = rdma_req->req.iov[i].iov_len;
		rdma_req->data.wr.sg_list[i].lkey = ((struct ibv_mr *)spdk_mem_map_translate(device->map,
						     (uint64_t)buf, NULL))->lkey;

		length -= rdma_req->req.iov[i].iov_len;
		i++;
	}

	rdma_req->data_from_pool = true;

	return 0;

nomem:
	while (i) {
		i--;
		spdk_mempool_put(rtransport->data_buf_pool, rdma_req->req.iov[i].iov_base);
		rdma_req->req.iov[i].iov_base = NULL;
		rdma_req->req.iov[i].iov_len = 0;

		rdma_req->data.wr.sg_list[i].addr = 0;
		rdma_req->data.wr.sg_list[i].length = 0;
		rdma_req->data.wr.sg_list[i].lkey = 0;
	}
	rdma_req->req.iovcnt = 0;
	return -ENOMEM;
}

static int
spdk_nvmf_rdma_request_parse_sgl(struct spdk_nvmf_rdma_transport *rtransport,
				 struct spdk_nvmf_rdma_device *device,
				 struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;

	cmd = &rdma_req->req.cmd->nvme_cmd;
	rsp = &rdma_req->req.rsp->nvme_cpl;
	sgl = &cmd->dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
	    (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS ||
	     sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY)) {
		if (sgl->keyed.length > rtransport->transport.opts.max_io_size) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    sgl->keyed.length, rtransport->transport.opts.max_io_size);
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
		rdma_req->req.length = sgl->keyed.length;

		if (spdk_nvmf_rdma_request_fill_iovs(rtransport, device, rdma_req) < 0) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(SPDK_LOG_RDMA, "No available large data buffers. Queueing request %p\n", rdma_req);
			return 0;
		}

		/* backward compatible */
		rdma_req->req.data = rdma_req->req.iov[0].iov_base;

		/* rdma wr specifics */
		rdma_req->data.wr.num_sge = rdma_req->req.iovcnt;
		rdma_req->data.wr.wr.rdma.rkey = sgl->keyed.key;
		rdma_req->data.wr.wr.rdma.remote_addr = sgl->address;

		SPDK_DEBUGLOG(SPDK_LOG_RDMA, "Request %p took %d buffer/s from central pool\n", rdma_req,
			      rdma_req->req.iovcnt);

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

		rdma_req->req.data = rdma_req->recv->buf + offset;
		rdma_req->data_from_pool = false;
		rdma_req->req.length = sgl->unkeyed.length;

		rdma_req->req.iov[0].iov_base = rdma_req->req.data;
		rdma_req->req.iov[0].iov_len = rdma_req->req.length;
		rdma_req->req.iovcnt = 1;

		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static bool
spdk_nvmf_rdma_request_process(struct spdk_nvmf_rdma_transport *rtransport,
			       struct spdk_nvmf_rdma_request *rdma_req)
{
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvme_cpl		*rsp = &rdma_req->req.rsp->nvme_cpl;
	int				rc;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	enum spdk_nvmf_rdma_request_state prev_state;
	bool				progress = false;
	int				data_posted;
	int				cur_rdma_rw_depth;

	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	device = rqpair->port->device;

	assert(rdma_req->state != RDMA_REQUEST_STATE_FREE);

	/* If the queue pair is in an error state, force the request to the completed state
	 * to release resources. */
	if (rqpair->ibv_attr.qp_state == IBV_QPS_ERR || rqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
		if (rdma_req->state == RDMA_REQUEST_STATE_NEED_BUFFER) {
			TAILQ_REMOVE(&rqpair->ch->pending_data_buf_queue, rdma_req, link);
		}
		spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
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

			TAILQ_REMOVE(&rqpair->incoming_queue, rdma_recv, link);

			if (rqpair->ibv_attr.qp_state == IBV_QPS_ERR) {
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
				break;
			}

			/* The next state transition depends on the data transfer needs of this request. */
			rdma_req->req.xfer = spdk_nvmf_rdma_request_get_xfer(rdma_req);

			/* If no data to transfer, ready to execute. */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_NONE) {
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_READY_TO_EXECUTE);
				break;
			}

			spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_NEED_BUFFER);
			TAILQ_INSERT_TAIL(&rqpair->ch->pending_data_buf_queue, rdma_req, link);
			break;
		case RDMA_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEED_BUFFER, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);

			assert(rdma_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (rdma_req != TAILQ_FIRST(&rqpair->ch->pending_data_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = spdk_nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
			if (rc < 0) {
				TAILQ_REMOVE(&rqpair->ch->pending_data_buf_queue, rdma_req, link);
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_READY_TO_COMPLETE);
				break;
			}

			if (!rdma_req->req.data) {
				/* No buffers available. */
				break;
			}

			TAILQ_REMOVE(&rqpair->ch->pending_data_buf_queue, rdma_req, link);

			/* If data is transferring from host to controller and the data didn't
			 * arrive using in capsule data, we need to do a transfer from the host.
			 */
			if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER && rdma_req->data_from_pool) {
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING);
				break;
			}

			spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_READY_TO_EXECUTE);
			break;
		case RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);

			if (rdma_req != TAILQ_FIRST(&rqpair->state_queue[RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING])) {
				/* This request needs to wait in line to perform RDMA */
				break;
			}
			cur_rdma_rw_depth = spdk_nvmf_rdma_cur_rw_depth(rqpair);

			if (cur_rdma_rw_depth >= rqpair->max_rw_depth) {
				/* R/W queue is full, need to wait */
				break;
			}

			if (rdma_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				rc = request_transfer_in(&rdma_req->req);
				if (!rc) {
					spdk_nvmf_rdma_request_set_state(rdma_req,
									 RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
				} else {
					rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					spdk_nvmf_rdma_request_set_state(rdma_req,
									 RDMA_REQUEST_STATE_READY_TO_COMPLETE);
				}
			} else if (rdma_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				/* The data transfer will be kicked off from
				 * RDMA_REQUEST_STATE_READY_TO_COMPLETE state.
				 */
				spdk_nvmf_rdma_request_set_state(rdma_req,
								 RDMA_REQUEST_STATE_READY_TO_COMPLETE);
			} else {
				SPDK_ERRLOG("Cannot perform data transfer, unknown state: %u\n",
					    rdma_req->req.xfer);
				assert(0);
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
			spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_EXECUTING);
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
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING);
			} else {
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_READY_TO_COMPLETE);
			}
			break;
		case RDMA_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE, 0, 0,
					  (uintptr_t)rdma_req, (uintptr_t)rqpair->cm_id);
			rc = request_transfer_out(&rdma_req->req, &data_posted);
			assert(rc == 0); /* No good way to handle this currently */
			if (rc) {
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
			} else {
				spdk_nvmf_rdma_request_set_state(rdma_req,
								 data_posted ?
								 RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST :
								 RDMA_REQUEST_STATE_COMPLETING);
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

			if (rdma_req->data_from_pool) {
				/* Put the buffer/s back in the pool */
				for (uint32_t i = 0; i < rdma_req->req.iovcnt; i++) {
					spdk_mempool_put(rtransport->data_buf_pool, rdma_req->data.buffers[i]);
					rdma_req->req.iov[i].iov_base = NULL;
					rdma_req->data.buffers[i] = NULL;
				}
				rdma_req->data_from_pool = false;
			}
			rdma_req->req.length = 0;
			rdma_req->req.iovcnt = 0;
			rdma_req->req.data = NULL;
			spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_FREE);
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
#define SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR 64
#define SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_RDMA_DEFAULT_IO_BUFFER_SIZE 131072

static void
spdk_nvmf_rdma_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =      SPDK_NVMF_RDMA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr = SPDK_NVMF_RDMA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size = SPDK_NVMF_RDMA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =          SPDK_NVMF_RDMA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =         SPDK_NVMF_RDMA_DEFAULT_IO_BUFFER_SIZE;
	opts->max_aq_depth =         SPDK_NVMF_RDMA_DEFAULT_AQ_DEPTH;
}

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

	const struct spdk_mem_map_ops nvmf_rdma_map_ops = {
		.notify_cb = spdk_nvmf_rdma_mem_notify,
		.are_contiguous = NULL
	};

	rtransport = calloc(1, sizeof(*rtransport));
	if (!rtransport) {
		return NULL;
	}

	if (pthread_mutex_init(&rtransport->lock, NULL)) {
		SPDK_ERRLOG("pthread_mutex_init() failed\n");
		free(rtransport);
		return NULL;
	}

	spdk_io_device_register(rtransport, spdk_nvmf_rdma_mgmt_channel_create,
				spdk_nvmf_rdma_mgmt_channel_destroy,
				sizeof(struct spdk_nvmf_rdma_mgmt_channel),
				"rdma_transport");

	TAILQ_INIT(&rtransport->devices);
	TAILQ_INIT(&rtransport->ports);

	rtransport->transport.ops = &spdk_nvmf_transport_rdma;

	SPDK_INFOLOG(SPDK_LOG_RDMA, "*** RDMA Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth);

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > SPDK_NVMF_MAX_SGL_ENTRIES) {
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

	rtransport->data_buf_pool = spdk_mempool_create("spdk_nvmf_rdma",
				    opts->max_queue_depth * 4, /* The 4 is arbitrarily chosen. Needs to be configurable. */
				    opts->max_io_size + NVMF_DATA_BUFFER_ALIGNMENT,
				    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!rtransport->data_buf_pool) {
		SPDK_ERRLOG("Unable to allocate buffer pool for poll group\n");
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

		device->pd = ibv_alloc_pd(device->context);
		if (!device->pd) {
			SPDK_ERRLOG("Unable to allocate protection domain.\n");
			free(device);
			rc = -1;
			break;
		}

		device->map = spdk_mem_map_alloc(0, &nvmf_rdma_map_ops, device);
		if (!device->map) {
			SPDK_ERRLOG("Unable to allocate memory map for new poll group\n");
			ibv_dealloc_pd(device->pd);
			free(device);
			rc = -1;
			break;
		}

		TAILQ_INSERT_TAIL(&rtransport->devices, device, link);
		i++;
	}
	rdma_free_devices(contexts);

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
			ibv_dealloc_pd(device->pd);
		}
		free(device);
	}

	if (rtransport->data_buf_pool != NULL) {
		if (spdk_mempool_count(rtransport->data_buf_pool) !=
		    (transport->opts.max_queue_depth * 4)) {
			SPDK_ERRLOG("transport buffer pool count is %zu but should be %u\n",
				    spdk_mempool_count(rtransport->data_buf_pool),
				    transport->opts.max_queue_depth * 4);
		}
	}

	spdk_mempool_free(rtransport->data_buf_pool);
	spdk_io_device_unregister(rtransport, NULL);
	pthread_mutex_destroy(&rtransport->lock);
	free(rtransport);

	return 0;
}

static int
spdk_nvmf_rdma_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_rdma_transport	*rtransport;
	struct spdk_nvmf_rdma_device	*device;
	struct spdk_nvmf_rdma_port	*port_tmp, *port;
	struct addrinfo			*res;
	struct addrinfo			hints;
	int				family;
	int				rc;

	rtransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_rdma_transport, transport);

	port = calloc(1, sizeof(*port));
	if (!port) {
		return -ENOMEM;
	}

	/* Selectively copy the trid. Things like NQN don't matter here - that
	 * mapping is enforced elsewhere.
	 */
	port->trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	port->trid.adrfam = trid->adrfam;
	snprintf(port->trid.traddr, sizeof(port->trid.traddr), "%s", trid->traddr);
	snprintf(port->trid.trsvcid, sizeof(port->trid.trsvcid), "%s", trid->trsvcid);

	pthread_mutex_lock(&rtransport->lock);
	assert(rtransport->event_channel != NULL);
	TAILQ_FOREACH(port_tmp, &rtransport->ports, link) {
		if (spdk_nvme_transport_id_compare(&port_tmp->trid, &port->trid) == 0) {
			port_tmp->ref++;
			free(port);
			/* Already listening at this address */
			pthread_mutex_unlock(&rtransport->lock);
			return 0;
		}
	}

	rc = rdma_create_id(rtransport->event_channel, &port->id, port, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		free(port);
		pthread_mutex_unlock(&rtransport->lock);
		return rc;
	}

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

	SPDK_INFOLOG(SPDK_LOG_RDMA, "*** NVMf Target Listening on %s port %d ***\n",
		     port->trid.traddr, ntohs(rdma_get_src_port(port->id)));

	port->ref = 1;

	TAILQ_INSERT_TAIL(&rtransport->ports, port, link);
	pthread_mutex_unlock(&rtransport->lock);

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
	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
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

static bool
spdk_nvmf_rdma_qpair_is_idle(struct spdk_nvmf_qpair *qpair)
{
	int cur_queue_depth, cur_rdma_rw_depth;
	struct spdk_nvmf_rdma_qpair *rqpair;

	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);
	cur_queue_depth = spdk_nvmf_rdma_cur_queue_depth(rqpair);
	cur_rdma_rw_depth = spdk_nvmf_rdma_cur_rw_depth(rqpair);

	if (cur_queue_depth == 0 && cur_rdma_rw_depth == 0) {
		return true;
	}
	return false;
}

static void
spdk_nvmf_rdma_qpair_process_pending(struct spdk_nvmf_rdma_transport *rtransport,
				     struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_recv	*rdma_recv, *recv_tmp;
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;

	/* We process I/O in the data transfer pending queue at the highest priority. */
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->state_queue[RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING],
			   state_link, req_tmp) {
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}

	/* The second highest priority is I/O waiting on memory buffers. */
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->ch->pending_data_buf_queue, link,
			   req_tmp) {
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}

	if (rqpair->qpair_disconnected) {
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return;
	}

	/* Do not process newly received commands if qp is in ERROR state,
	 * wait till the recovery is complete.
	 */
	if (rqpair->ibv_attr.qp_state == IBV_QPS_ERR) {
		return;
	}

	/* The lowest priority is processing newly received commands */
	TAILQ_FOREACH_SAFE(rdma_recv, &rqpair->incoming_queue, link, recv_tmp) {
		if (TAILQ_EMPTY(&rqpair->state_queue[RDMA_REQUEST_STATE_FREE])) {
			break;
		}

		rdma_req = TAILQ_FIRST(&rqpair->state_queue[RDMA_REQUEST_STATE_FREE]);
		rdma_req->recv = rdma_recv;
		spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_NEW);
		if (spdk_nvmf_rdma_request_process(rtransport, rdma_req) == false) {
			break;
		}
	}
}

static void
spdk_nvmf_rdma_drain_state_queue(struct spdk_nvmf_rdma_qpair *rqpair,
				 enum spdk_nvmf_rdma_request_state state)
{
	struct spdk_nvmf_rdma_request *rdma_req, *req_tmp;
	struct spdk_nvmf_rdma_transport *rtransport;

	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->state_queue[state], state_link, req_tmp) {
		rtransport = SPDK_CONTAINEROF(rdma_req->req.qpair->transport,
					      struct spdk_nvmf_rdma_transport, transport);
		spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
		spdk_nvmf_rdma_request_process(rtransport, rdma_req);
	}
}

static void
spdk_nvmf_rdma_qpair_recover(struct spdk_nvmf_rdma_qpair *rqpair)
{
	enum ibv_qp_state state, next_state;
	int recovered;
	struct spdk_nvmf_rdma_transport *rtransport;

	if (!spdk_nvmf_rdma_qpair_is_idle(&rqpair->qpair)) {
		/* There must be outstanding requests down to media.
		 * If so, wait till they're complete.
		 */
		assert(!TAILQ_EMPTY(&rqpair->qpair.outstanding));
		return;
	}

	state = rqpair->ibv_attr.qp_state;
	next_state = state;

	SPDK_NOTICELOG("RDMA qpair %u is in state: %s\n",
		       rqpair->qpair.qid,
		       str_ibv_qp_state[state]);

	if (!(state == IBV_QPS_ERR || state == IBV_QPS_RESET)) {
		SPDK_ERRLOG("Can't recover RDMA qpair %u from the state: %s\n",
			    rqpair->qpair.qid,
			    str_ibv_qp_state[state]);
		spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
		return;
	}

	recovered = 0;
	while (!recovered) {
		switch (state) {
		case IBV_QPS_ERR:
			next_state = IBV_QPS_RESET;
			break;
		case IBV_QPS_RESET:
			next_state = IBV_QPS_INIT;
			break;
		case IBV_QPS_INIT:
			next_state = IBV_QPS_RTR;
			break;
		case IBV_QPS_RTR:
			next_state = IBV_QPS_RTS;
			break;
		case IBV_QPS_RTS:
			recovered = 1;
			break;
		default:
			SPDK_ERRLOG("RDMA qpair %u unexpected state for recovery: %u\n",
				    rqpair->qpair.qid, state);
			goto error;
		}
		/* Do not transition into same state */
		if (next_state == state) {
			break;
		}

		if (spdk_nvmf_rdma_set_ibv_state(rqpair, next_state)) {
			goto error;
		}

		state = next_state;
	}

	rtransport = SPDK_CONTAINEROF(rqpair->qpair.transport,
				      struct spdk_nvmf_rdma_transport,
				      transport);

	spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);

	return;
error:
	SPDK_NOTICELOG("RDMA qpair %u: recovery failed, disconnecting...\n",
		       rqpair->qpair.qid);
	spdk_nvmf_qpair_disconnect(&rqpair->qpair, NULL, NULL);
}

/* Clean up only the states that can be aborted at any time */
static void
_spdk_nvmf_rdma_qp_cleanup_safe_states(struct spdk_nvmf_rdma_qpair *rqpair)
{
	struct spdk_nvmf_rdma_request	*rdma_req, *req_tmp;

	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_NEW);
	TAILQ_FOREACH_SAFE(rdma_req, &rqpair->state_queue[RDMA_REQUEST_STATE_NEED_BUFFER], link, req_tmp) {
		TAILQ_REMOVE(&rqpair->ch->pending_data_buf_queue, rdma_req, link);
	}
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_NEED_BUFFER);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_DATA_TRANSFER_PENDING);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_READY_TO_EXECUTE);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_EXECUTED);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_READY_TO_COMPLETE);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_COMPLETED);
}

/* This cleans up all memory. It is only safe to use if the rest of the software stack
 * has been shut down */
static void
_spdk_nvmf_rdma_qp_cleanup_all_states(struct spdk_nvmf_rdma_qpair *rqpair)
{
	_spdk_nvmf_rdma_qp_cleanup_safe_states(rqpair);

	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_EXECUTING);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_COMPLETING);
}

static void
_spdk_nvmf_rdma_qp_error(void *arg)
{
	struct spdk_nvmf_rdma_qpair	*rqpair = arg;
	enum ibv_qp_state		state;

	spdk_nvmf_rdma_qpair_dec_refcnt(rqpair);

	state = rqpair->ibv_attr.qp_state;
	if (state != IBV_QPS_ERR) {
		/* Error was already recovered */
		return;
	}

	if (spdk_nvmf_qpair_is_admin_queue(&rqpair->qpair)) {
		spdk_nvmf_ctrlr_abort_aer(rqpair->qpair.ctrlr);
	}

	_spdk_nvmf_rdma_qp_cleanup_safe_states(rqpair);

	/* Attempt recovery. This will exit without recovering if I/O requests
	 * are still outstanding */
	spdk_nvmf_rdma_qpair_recover(rqpair);
}

static void
_spdk_nvmf_rdma_qp_last_wqe(void *arg)
{
	struct spdk_nvmf_rdma_qpair	*rqpair = arg;
	enum ibv_qp_state		state;

	spdk_nvmf_rdma_qpair_dec_refcnt(rqpair);

	state = rqpair->ibv_attr.qp_state;
	if (state != IBV_QPS_ERR) {
		/* Error was already recovered */
		return;
	}

	/* Clear out the states that are safe to clear any time, plus the
	 * RDMA data transfer states. */
	_spdk_nvmf_rdma_qp_cleanup_safe_states(rqpair);

	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	spdk_nvmf_rdma_drain_state_queue(rqpair, RDMA_REQUEST_STATE_COMPLETING);

	spdk_nvmf_rdma_qpair_recover(rqpair);
}

static void
spdk_nvmf_process_ib_event(struct spdk_nvmf_rdma_device *device)
{
	int				rc;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	struct ibv_async_event		event;
	enum ibv_qp_state		state;

	rc = ibv_get_async_event(device->context, &event);

	if (rc) {
		SPDK_ERRLOG("Failed to get async_event (%d): %s\n",
			    errno, spdk_strerror(errno));
		return;
	}

	SPDK_NOTICELOG("Async event: %s\n",
		       ibv_event_type_str(event.event_type));

	switch (event.event_type) {
	case IBV_EVENT_QP_FATAL:
		rqpair = event.element.qp->qp_context;
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair->cm_id, event.event_type);
		spdk_nvmf_rdma_update_ibv_state(rqpair);
		spdk_nvmf_rdma_qpair_inc_refcnt(rqpair);
		spdk_thread_send_msg(rqpair->qpair.group->thread, _spdk_nvmf_rdma_qp_error, rqpair);
		break;
	case IBV_EVENT_QP_LAST_WQE_REACHED:
		rqpair = event.element.qp->qp_context;
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair->cm_id, event.event_type);
		spdk_nvmf_rdma_update_ibv_state(rqpair);
		spdk_nvmf_rdma_qpair_inc_refcnt(rqpair);
		spdk_thread_send_msg(rqpair->qpair.group->thread, _spdk_nvmf_rdma_qp_last_wqe, rqpair);
		break;
	case IBV_EVENT_SQ_DRAINED:
		/* This event occurs frequently in both error and non-error states.
		 * Check if the qpair is in an error state before sending a message.
		 * Note that we're not on the correct thread to access the qpair, but
		 * the operations that the below calls make all happen to be thread
		 * safe. */
		rqpair = event.element.qp->qp_context;
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0,
				  (uintptr_t)rqpair->cm_id, event.event_type);
		state = spdk_nvmf_rdma_update_ibv_state(rqpair);
		if (state == IBV_QPS_ERR) {
			spdk_nvmf_rdma_qpair_inc_refcnt(rqpair);
			spdk_thread_send_msg(rqpair->qpair.group->thread, _spdk_nvmf_rdma_qp_error, rqpair);
		}
		break;
	case IBV_EVENT_QP_REQ_ERR:
	case IBV_EVENT_QP_ACCESS_ERR:
	case IBV_EVENT_COMM_EST:
	case IBV_EVENT_PATH_MIG:
	case IBV_EVENT_PATH_MIG_ERR:
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
		spdk_trace_record(TRACE_RDMA_IBV_ASYNC_EVENT, 0, 0, 0, event.event_type);
		break;
	}
	ibv_ack_async_event(&event);
}

static void
spdk_nvmf_rdma_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn)
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
		spdk_nvmf_process_cm_event(transport, cb_fn);
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
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.rdma.rdma_qptype = SPDK_NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED;
	entry->tsas.rdma.rdma_prtype = SPDK_NVMF_RDMA_PRTYPE_NONE;
	entry->tsas.rdma.rdma_cms = SPDK_NVMF_RDMA_CMS_RDMA_CM;
}

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_rdma_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller;
	struct spdk_nvmf_rdma_device		*device;

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
			free(rgroup);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}

		poller->device = device;
		poller->group = rgroup;

		TAILQ_INIT(&poller->qpairs);

		poller->cq = ibv_create_cq(device->context, NVMF_RDMA_CQ_SIZE, poller, NULL, 0);
		if (!poller->cq) {
			SPDK_ERRLOG("Unable to create completion queue\n");
			free(poller);
			free(rgroup);
			pthread_mutex_unlock(&rtransport->lock);
			return NULL;
		}

		TAILQ_INSERT_TAIL(&rgroup->pollers, poller, link);
	}

	pthread_mutex_unlock(&rtransport->lock);
	return &rgroup->group;
}

static void
spdk_nvmf_rdma_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_poller		*poller, *tmp;
	struct spdk_nvmf_rdma_qpair		*qpair, *tmp_qpair;

	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);

	if (!rgroup) {
		return;
	}

	TAILQ_FOREACH_SAFE(poller, &rgroup->pollers, link, tmp) {
		TAILQ_REMOVE(&rgroup->pollers, poller, link);

		if (poller->cq) {
			ibv_destroy_cq(poller->cq);
		}
		TAILQ_FOREACH_SAFE(qpair, &poller->qpairs, link, tmp_qpair) {
			_spdk_nvmf_rdma_qp_cleanup_all_states(qpair);
			spdk_nvmf_rdma_qpair_destroy(qpair);
		}

		free(poller);
	}

	free(rgroup);
}

static int
spdk_nvmf_rdma_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_transport		*rtransport;
	struct spdk_nvmf_rdma_poll_group	*rgroup;
	struct spdk_nvmf_rdma_qpair		*rqpair;
	struct spdk_nvmf_rdma_device		*device;
	struct spdk_nvmf_rdma_poller		*poller;
	int					rc;

	rtransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_rdma_transport, transport);
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	device = rqpair->port->device;

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

	rc = spdk_nvmf_rdma_qpair_initialize(qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to initialize nvmf_rdma_qpair with qpair=%p\n", qpair);
		return -1;
	}

	rqpair->mgmt_channel = spdk_get_io_channel(rtransport);
	if (!rqpair->mgmt_channel) {
		spdk_nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		spdk_nvmf_rdma_qpair_destroy(rqpair);
		return -1;
	}

	rqpair->ch = spdk_io_channel_get_ctx(rqpair->mgmt_channel);
	assert(rqpair->ch != NULL);

	rc = spdk_nvmf_rdma_event_accept(rqpair->cm_id, rqpair);
	if (rc) {
		/* Try to reject, but we probably can't */
		spdk_nvmf_rdma_event_reject(rqpair->cm_id, SPDK_NVMF_RDMA_ERROR_NO_RESOURCES);
		spdk_nvmf_rdma_qpair_destroy(rqpair);
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

	if (rdma_req->data_from_pool) {
		/* Put the buffer/s back in the pool */
		for (uint32_t i = 0; i < rdma_req->req.iovcnt; i++) {
			spdk_mempool_put(rtransport->data_buf_pool, rdma_req->data.buffers[i]);
			rdma_req->req.iov[i].iov_base = NULL;
			rdma_req->data.buffers[i] = NULL;
		}
		rdma_req->data_from_pool = false;
	}
	rdma_req->req.length = 0;
	rdma_req->req.iovcnt = 0;
	rdma_req->req.data = NULL;
	spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_FREE);
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

	if (rqpair->ibv_attr.qp_state != IBV_QPS_ERR) {
		/* The connection is alive, so process the request as normal */
		spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_EXECUTED);
	} else {
		/* The connection is dead. Move the request directly to the completed state. */
		spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
	}

	spdk_nvmf_rdma_request_process(rtransport, rdma_req);

	if (rqpair->qpair.state == SPDK_NVMF_QPAIR_ACTIVE && rqpair->ibv_attr.qp_state == IBV_QPS_ERR) {
		/* If the NVMe-oF layer thinks the connection is active, but the RDMA layer thinks
		 * the connection is dead, perform error recovery. */
		spdk_nvmf_rdma_qpair_recover(rqpair);
	}

	return 0;
}

static void
spdk_nvmf_rdma_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_rdma_qpair *rqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_rdma_qpair, qpair);

	spdk_nvmf_rdma_qpair_destroy(rqpair);
}

static struct spdk_nvmf_rdma_request *
get_rdma_req_from_wc(struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_request *rdma_req;

	rdma_req = (struct spdk_nvmf_rdma_request *)wc->wr_id;
	assert(rdma_req != NULL);

#ifdef DEBUG
	struct spdk_nvmf_rdma_qpair *rqpair;
	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

	assert(rdma_req - rqpair->reqs >= 0);
	assert(rdma_req - rqpair->reqs < (ptrdiff_t)rqpair->max_queue_depth);
#endif

	return rdma_req;
}

static struct spdk_nvmf_rdma_recv *
get_rdma_recv_from_wc(struct ibv_wc *wc)
{
	struct spdk_nvmf_rdma_recv *rdma_recv;

	assert(wc->byte_len >= sizeof(struct spdk_nvmf_capsule_cmd));

	rdma_recv = (struct spdk_nvmf_rdma_recv *)wc->wr_id;
	assert(rdma_recv != NULL);

#ifdef DEBUG
	struct spdk_nvmf_rdma_qpair *rqpair = rdma_recv->qpair;

	assert(rdma_recv - rqpair->recvs >= 0);
	assert(rdma_recv - rqpair->recvs < (ptrdiff_t)rqpair->max_queue_depth);
#endif

	return rdma_recv;
}

#ifdef DEBUG
static int
spdk_nvmf_rdma_req_is_completing(struct spdk_nvmf_rdma_request *rdma_req)
{
	return rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST ||
	       rdma_req->state == RDMA_REQUEST_STATE_COMPLETING;
}
#endif

static int
spdk_nvmf_rdma_poller_poll(struct spdk_nvmf_rdma_transport *rtransport,
			   struct spdk_nvmf_rdma_poller *rpoller)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_rdma_request	*rdma_req;
	struct spdk_nvmf_rdma_recv	*rdma_recv;
	struct spdk_nvmf_rdma_qpair	*rqpair;
	int reaped, i;
	int count = 0;
	bool error = false;

	/* Poll for completing operations. */
	reaped = ibv_poll_cq(rpoller->cq, 32, wc);
	if (reaped < 0) {
		SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
			    errno, spdk_strerror(errno));
		return -1;
	}

	for (i = 0; i < reaped; i++) {
		/* Handle error conditions */
		if (wc[i].status) {
			SPDK_WARNLOG("CQ error on CQ %p, Request 0x%lu (%d): %s\n",
				     rpoller->cq, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));
			error = true;

			switch (wc[i].opcode) {
			case IBV_WC_SEND:
				rdma_req = get_rdma_req_from_wc(&wc[i]);
				rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

				/* We're going to attempt an error recovery, so force the request into
				 * the completed state. */
				spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
				spdk_nvmf_rdma_request_process(rtransport, rdma_req);
				break;
			case IBV_WC_RECV:
				rdma_recv = get_rdma_recv_from_wc(&wc[i]);
				rqpair = rdma_recv->qpair;

				/* Dump this into the incoming queue. This gets cleaned up when
				 * the queue pair disconnects or recovers. */
				TAILQ_INSERT_TAIL(&rqpair->incoming_queue, rdma_recv, link);
				break;
			case IBV_WC_RDMA_WRITE:
			case IBV_WC_RDMA_READ:
				/* If the data transfer fails still force the queue into the error state,
				 * but the rdma_req objects should only be manipulated in response to
				 * SEND and RECV operations. */
				rdma_req = get_rdma_req_from_wc(&wc[i]);
				rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
				break;
			default:
				SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
				continue;
			}

			/* Set the qpair to the error state. This will initiate a recovery. */
			spdk_nvmf_rdma_set_ibv_state(rqpair, IBV_QPS_ERR);
			continue;
		}

		switch (wc[i].opcode) {
		case IBV_WC_SEND:
			rdma_req = get_rdma_req_from_wc(&wc[i]);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			assert(spdk_nvmf_rdma_req_is_completing(rdma_req));

			spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
			spdk_nvmf_rdma_request_process(rtransport, rdma_req);

			count++;

			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
			break;

		case IBV_WC_RDMA_WRITE:
			rdma_req = get_rdma_req_from_wc(&wc[i]);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
			break;

		case IBV_WC_RDMA_READ:
			rdma_req = get_rdma_req_from_wc(&wc[i]);
			rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);

			assert(rdma_req->state == RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
			spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_READY_TO_EXECUTE);
			spdk_nvmf_rdma_request_process(rtransport, rdma_req);

			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
			break;

		case IBV_WC_RECV:
			rdma_recv = get_rdma_recv_from_wc(&wc[i]);
			rqpair = rdma_recv->qpair;

			TAILQ_INSERT_TAIL(&rqpair->incoming_queue, rdma_recv, link);
			/* Try to process other queued requests */
			spdk_nvmf_rdma_qpair_process_pending(rtransport, rqpair);
			break;

		default:
			SPDK_ERRLOG("Received an unknown opcode on the CQ: %d\n", wc[i].opcode);
			continue;
		}
	}

	if (error == true) {
		return -1;
	}

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

	trid->trtype = SPDK_NVME_TRANSPORT_RDMA;

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

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_rdma = {
	.type = SPDK_NVME_TRANSPORT_RDMA,
	.opts_init = spdk_nvmf_rdma_opts_init,
	.create = spdk_nvmf_rdma_create,
	.destroy = spdk_nvmf_rdma_destroy,

	.listen = spdk_nvmf_rdma_listen,
	.stop_listen = spdk_nvmf_rdma_stop_listen,
	.accept = spdk_nvmf_rdma_accept,

	.listener_discover = spdk_nvmf_rdma_discover,

	.poll_group_create = spdk_nvmf_rdma_poll_group_create,
	.poll_group_destroy = spdk_nvmf_rdma_poll_group_destroy,
	.poll_group_add = spdk_nvmf_rdma_poll_group_add,
	.poll_group_poll = spdk_nvmf_rdma_poll_group_poll,

	.req_free = spdk_nvmf_rdma_request_free,
	.req_complete = spdk_nvmf_rdma_request_complete,

	.qpair_fini = spdk_nvmf_rdma_close_qpair,
	.qpair_is_idle = spdk_nvmf_rdma_qpair_is_idle,
	.qpair_get_peer_trid = spdk_nvmf_rdma_qpair_get_peer_trid,
	.qpair_get_local_trid = spdk_nvmf_rdma_qpair_get_local_trid,
	.qpair_get_listen_trid = spdk_nvmf_rdma_qpair_get_listen_trid,

};

SPDK_LOG_REGISTER_COMPONENT("rdma", SPDK_LOG_RDMA)
