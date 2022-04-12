/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "spdk/accel_engine.h"
#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/nvme_tcp.h"
#include "spdk_internal/sock.h"

#include "nvmf_internal.h"

#include "spdk_internal/trace_defs.h"

#define NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME 16
#define SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY 16
#define SPDK_NVMF_TCP_DEFAULT_SOCK_PRIORITY 0
#define SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM 32
#define SPDK_NVMF_TCP_DEFAULT_SUCCESS_OPTIMIZATION true

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp;

/* spdk nvmf related structure */
enum spdk_nvmf_tcp_req_state {

	/* The request is not currently in use */
	TCP_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	TCP_REQUEST_STATE_NEW = 1,

	/* The request is queued until a data buffer is available. */
	TCP_REQUEST_STATE_NEED_BUFFER = 2,

	/* The request is waiting for zcopy_start to finish */
	TCP_REQUEST_STATE_AWAITING_ZCOPY_START = 3,

	/* The request has received a zero-copy buffer */
	TCP_REQUEST_STATE_ZCOPY_START_COMPLETED = 4,

	/* The request is currently transferring data from the host to the controller. */
	TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER = 5,

	/* The request is waiting for the R2T send acknowledgement. */
	TCP_REQUEST_STATE_AWAITING_R2T_ACK = 6,

	/* The request is ready to execute at the block device */
	TCP_REQUEST_STATE_READY_TO_EXECUTE = 7,

	/* The request is currently executing at the block device */
	TCP_REQUEST_STATE_EXECUTING = 8,

	/* The request is waiting for zcopy buffers to be commited */
	TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT = 9,

	/* The request finished executing at the block device */
	TCP_REQUEST_STATE_EXECUTED = 10,

	/* The request is ready to send a completion */
	TCP_REQUEST_STATE_READY_TO_COMPLETE = 11,

	/* The request is currently transferring final pdus from the controller to the host. */
	TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST = 12,

	/* The request is waiting for zcopy buffers to be released (without committing) */
	TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE = 13,

	/* The request completed and can be marked free. */
	TCP_REQUEST_STATE_COMPLETED = 14,

	/* Terminator */
	TCP_REQUEST_NUM_STATES,
};

static const char *spdk_nvmf_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digiest Error",
	"Data Transfer Out of Range",
	"R2T Limit Exceeded",
	"Unsupported parameter",
};

SPDK_TRACE_REGISTER_FN(nvmf_tcp_trace, "nvmf_tcp", TRACE_GROUP_NVMF_TCP)
{
	spdk_trace_register_object(OBJECT_NVMF_TCP_IO, 'r');
	spdk_trace_register_description("TCP_REQ_NEW",
					TRACE_TCP_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 1,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_NEED_BUFFER",
					TRACE_TCP_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_WAIT_ZCPY_START",
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_START,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_ZCPY_START_CPL",
					TRACE_TCP_REQUEST_STATE_ZCOPY_START_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_TX_H_TO_C",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_RDY_TO_EXECUTE",
					TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_EXECUTING",
					TRACE_TCP_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_WAIT_ZCPY_CMT",
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_COMMIT,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_EXECUTED",
					TRACE_TCP_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_RDY_TO_COMPLETE",
					TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_TRANSFER_C2H",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_AWAIT_ZCPY_RLS",
					TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_RELEASE,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_COMPLETED",
					TRACE_TCP_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_WRITE_START",
					TRACE_TCP_FLUSH_WRITEBUF_START,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_WRITE_DONE",
					TRACE_TCP_FLUSH_WRITEBUF_DONE,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_READ_DONE",
					TRACE_TCP_READ_FROM_SOCKET_DONE,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_REQ_AWAIT_R2T_ACK",
					TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");

	spdk_trace_register_description("TCP_QP_CREATE", TRACE_TCP_QP_CREATE,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_SOCK_INIT", TRACE_TCP_QP_SOCK_INIT,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_STATE_CHANGE", TRACE_TCP_QP_STATE_CHANGE,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "state");
	spdk_trace_register_description("TCP_QP_DISCONNECT", TRACE_TCP_QP_DISCONNECT,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_DESTROY", TRACE_TCP_QP_DESTROY,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("TCP_QP_ABORT_REQ", TRACE_TCP_QP_ABORT_REQ,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_PTR, "qpair");
	spdk_trace_register_description("TCP_QP_RCV_STATE_CHANGE", TRACE_TCP_QP_RCV_STATE_CHANGE,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "state");

	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_START, OBJECT_NVMF_TCP_IO, 1);
	spdk_trace_tpoint_register_relation(TRACE_BDEV_IO_DONE, OBJECT_NVMF_TCP_IO, 0);
}

struct spdk_nvmf_tcp_req  {
	struct spdk_nvmf_request		req;
	struct spdk_nvme_cpl			rsp;
	struct spdk_nvme_cmd			cmd;

	/* A PDU that can be used for sending responses. This is
	 * not the incoming PDU! */
	struct nvme_tcp_pdu			*pdu;

	/* In-capsule data buffer */
	uint8_t					*buf;
	/*
	 * The PDU for a request may be used multiple times in serial over
	 * the request's lifetime. For example, first to send an R2T, then
	 * to send a completion. To catch mistakes where the PDU is used
	 * twice at the same time, add a debug flag here for init/fini.
	 */
	bool					pdu_in_use;
	bool					has_in_capsule_data;

	/* transfer_tag */
	uint16_t				ttag;

	enum spdk_nvmf_tcp_req_state		state;

	/*
	 * h2c_offset is used when we receive the h2c_data PDU.
	 */
	uint32_t				h2c_offset;

	STAILQ_ENTRY(spdk_nvmf_tcp_req)		link;
	TAILQ_ENTRY(spdk_nvmf_tcp_req)		state_link;
};

struct spdk_nvmf_tcp_qpair {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_tcp_poll_group		*group;
	struct spdk_sock			*sock;

	enum nvme_tcp_pdu_recv_state		recv_state;
	enum nvme_tcp_qpair_state		state;

	/* PDU being actively received */
	struct nvme_tcp_pdu			*pdu_in_progress;

	/* Queues to track the requests in all states */
	TAILQ_HEAD(, spdk_nvmf_tcp_req)		tcp_req_working_queue;
	TAILQ_HEAD(, spdk_nvmf_tcp_req)		tcp_req_free_queue;

	/* Number of requests in each state */
	uint32_t				state_cntr[TCP_REQUEST_NUM_STATES];

	uint8_t					cpda;

	bool					host_hdgst_enable;
	bool					host_ddgst_enable;

	/* This is a spare PDU used for sending special management
	 * operations. Primarily, this is used for the initial
	 * connection response and c2h termination request. */
	struct nvme_tcp_pdu			*mgmt_pdu;

	/* Arrays of in-capsule buffers, requests, and pdus.
	 * Each array is 'resource_count' number of elements */
	void					*bufs;
	struct spdk_nvmf_tcp_req		*reqs;
	struct nvme_tcp_pdu			*pdus;
	uint32_t				resource_count;
	uint32_t				recv_buf_size;

	struct spdk_nvmf_tcp_port		*port;

	/* IP address */
	char					initiator_addr[SPDK_NVMF_TRADDR_MAX_LEN];
	char					target_addr[SPDK_NVMF_TRADDR_MAX_LEN];

	/* IP port */
	uint16_t				initiator_port;
	uint16_t				target_port;

	/* Timer used to destroy qpair after detecting transport error issue if initiator does
	 *  not close the connection.
	 */
	struct spdk_poller			*timeout_poller;

	spdk_nvmf_transport_qpair_fini_cb	fini_cb_fn;
	void					*fini_cb_arg;

	TAILQ_ENTRY(spdk_nvmf_tcp_qpair)	link;
};

struct spdk_nvmf_tcp_control_msg {
	STAILQ_ENTRY(spdk_nvmf_tcp_control_msg) link;
};

struct spdk_nvmf_tcp_control_msg_list {
	void *msg_buf;
	STAILQ_HEAD(, spdk_nvmf_tcp_control_msg) free_msgs;
};

struct spdk_nvmf_tcp_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	struct spdk_sock_group			*sock_group;

	TAILQ_HEAD(, spdk_nvmf_tcp_qpair)	qpairs;
	TAILQ_HEAD(, spdk_nvmf_tcp_qpair)	await_req;

	struct spdk_io_channel			*accel_channel;
	struct spdk_nvmf_tcp_control_msg_list	*control_msg_list;

	TAILQ_ENTRY(spdk_nvmf_tcp_poll_group)	link;
};

struct spdk_nvmf_tcp_port {
	const struct spdk_nvme_transport_id	*trid;
	struct spdk_sock			*listen_sock;
	TAILQ_ENTRY(spdk_nvmf_tcp_port)		link;
};

struct tcp_transport_opts {
	bool		c2h_success;
	uint16_t	control_msg_num;
	uint32_t	sock_priority;
};

struct spdk_nvmf_tcp_transport {
	struct spdk_nvmf_transport		transport;
	struct tcp_transport_opts               tcp_opts;

	struct spdk_nvmf_tcp_poll_group		*next_pg;

	struct spdk_poller			*accept_poller;
	pthread_mutex_t				lock;

	TAILQ_HEAD(, spdk_nvmf_tcp_port)	ports;
	TAILQ_HEAD(, spdk_nvmf_tcp_poll_group)	poll_groups;
};

static const struct spdk_json_object_decoder tcp_transport_opts_decoder[] = {
	{
		"c2h_success", offsetof(struct tcp_transport_opts, c2h_success),
		spdk_json_decode_bool, true
	},
	{
		"control_msg_num", offsetof(struct tcp_transport_opts, control_msg_num),
		spdk_json_decode_uint16, true
	},
	{
		"sock_priority", offsetof(struct tcp_transport_opts, sock_priority),
		spdk_json_decode_uint32, true
	},
};

static bool nvmf_tcp_req_process(struct spdk_nvmf_tcp_transport *ttransport,
				 struct spdk_nvmf_tcp_req *tcp_req);
static void nvmf_tcp_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group);

static void _nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
				    struct spdk_nvmf_tcp_req *tcp_req);

static void
nvmf_tcp_req_set_state(struct spdk_nvmf_tcp_req *tcp_req,
		       enum spdk_nvmf_tcp_req_state state)
{
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_tcp_qpair *tqpair;

	qpair = tcp_req->req.qpair;
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair->state_cntr[tcp_req->state] > 0);
	tqpair->state_cntr[tcp_req->state]--;
	tqpair->state_cntr[state]++;

	tcp_req->state = state;
}

static inline struct nvme_tcp_pdu *
nvmf_tcp_req_pdu_init(struct spdk_nvmf_tcp_req *tcp_req)
{
	assert(tcp_req->pdu_in_use == false);

	memset(tcp_req->pdu, 0, sizeof(*tcp_req->pdu));
	tcp_req->pdu->qpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

	return tcp_req->pdu;
}

static struct spdk_nvmf_tcp_req *
nvmf_tcp_req_get(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_req *tcp_req;

	tcp_req = TAILQ_FIRST(&tqpair->tcp_req_free_queue);
	if (!tcp_req) {
		return NULL;
	}

	memset(&tcp_req->rsp, 0, sizeof(tcp_req->rsp));
	tcp_req->h2c_offset = 0;
	tcp_req->has_in_capsule_data = false;
	tcp_req->req.dif_enabled = false;
	tcp_req->req.zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	TAILQ_REMOVE(&tqpair->tcp_req_free_queue, tcp_req, state_link);
	TAILQ_INSERT_TAIL(&tqpair->tcp_req_working_queue, tcp_req, state_link);
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_NEW);
	return tcp_req;
}

static inline void
nvmf_tcp_req_put(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_req *tcp_req)
{
	assert(!tcp_req->pdu_in_use);

	TAILQ_REMOVE(&tqpair->tcp_req_working_queue, tcp_req, state_link);
	TAILQ_INSERT_TAIL(&tqpair->tcp_req_free_queue, tcp_req, state_link);
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_FREE);
}

static void
nvmf_tcp_request_free(void *cb_arg)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;

	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(nvmf_tcp, "tcp_req=%p will be freed\n", tcp_req);
	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
	nvmf_tcp_req_process(ttransport, tcp_req);
}

static int
nvmf_tcp_req_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req *tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	nvmf_tcp_request_free(tcp_req);

	return 0;
}

static void
nvmf_tcp_drain_state_queue(struct spdk_nvmf_tcp_qpair *tqpair,
			   enum spdk_nvmf_tcp_req_state state)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	assert(state != TCP_REQUEST_STATE_FREE);
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->tcp_req_working_queue, state_link, req_tmp) {
		if (state == tcp_req->state) {
			nvmf_tcp_request_free(tcp_req);
		}
	}
}

static void
nvmf_tcp_cleanup_all_states(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEW);

	/* Wipe the requests waiting for buffer from the global list */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->tcp_req_working_queue, state_link, req_tmp) {
		if (tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER) {
			STAILQ_REMOVE(&tqpair->group->group.pending_buf_queue, &tcp_req->req,
				      spdk_nvmf_request, buf_link);
		}
	}

	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEED_BUFFER);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_EXECUTING);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_AWAITING_R2T_ACK);
}

static void
nvmf_tcp_dump_qpair_req_contents(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int i;
	struct spdk_nvmf_tcp_req *tcp_req;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", tqpair->qpair.qid);
	for (i = 1; i < TCP_REQUEST_NUM_STATES; i++) {
		SPDK_ERRLOG("\tNum of requests in state[%d] = %u\n", i, tqpair->state_cntr[i]);
		TAILQ_FOREACH(tcp_req, &tqpair->tcp_req_working_queue, state_link) {
			if ((int)tcp_req->state == i) {
				SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", tcp_req->req.data_from_pool);
				SPDK_ERRLOG("\t\tRequest opcode: %d\n", tcp_req->req.cmd->nvmf_cmd.opcode);
			}
		}
	}
}

static void
_nvmf_tcp_qpair_destroy(void *_tqpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair = _tqpair;
	spdk_nvmf_transport_qpair_fini_cb cb_fn = tqpair->fini_cb_fn;
	void *cb_arg = tqpair->fini_cb_arg;
	int err = 0;

	spdk_trace_record(TRACE_TCP_QP_DESTROY, 0, 0, (uintptr_t)tqpair);

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	err = spdk_sock_close(&tqpair->sock);
	assert(err == 0);
	nvmf_tcp_cleanup_all_states(tqpair);

	if (tqpair->state_cntr[TCP_REQUEST_STATE_FREE] != tqpair->resource_count) {
		SPDK_ERRLOG("tqpair(%p) free tcp request num is %u but should be %u\n", tqpair,
			    tqpair->state_cntr[TCP_REQUEST_STATE_FREE],
			    tqpair->resource_count);
		err++;
	}

	if (err > 0) {
		nvmf_tcp_dump_qpair_req_contents(tqpair);
	}

	/* The timeout poller might still be registered here if we close the qpair before host
	 * terminates the connection.
	 */
	spdk_poller_unregister(&tqpair->timeout_poller);
	spdk_dma_free(tqpair->pdus);
	free(tqpair->reqs);
	spdk_free(tqpair->bufs);
	free(tqpair);

	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}

	SPDK_DEBUGLOG(nvmf_tcp, "Leave\n");
}

static void
nvmf_tcp_qpair_destroy(struct spdk_nvmf_tcp_qpair *tqpair)
{
	/* Delay the destruction to make sure it isn't performed from the context of a sock
	 * callback.  Otherwise, spdk_sock_close() might not abort pending requests, causing their
	 * completions to be executed after the qpair is freed.  (Note: this fixed issue #2471.)
	 */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_tcp_qpair_destroy, tqpair);
}

static void
nvmf_tcp_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	assert(w != NULL);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);
	spdk_json_write_named_bool(w, "c2h_success", ttransport->tcp_opts.c2h_success);
	spdk_json_write_named_uint32(w, "sock_priority", ttransport->tcp_opts.sock_priority);
}

static int
nvmf_tcp_destroy(struct spdk_nvmf_transport *transport,
		 spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_tcp_transport	*ttransport;

	assert(transport != NULL);
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	spdk_poller_unregister(&ttransport->accept_poller);
	pthread_mutex_destroy(&ttransport->lock);
	free(ttransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
	return 0;
}

static int
nvmf_tcp_accept(void *ctx);

static struct spdk_nvmf_transport *
nvmf_tcp_create(struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	uint32_t sge_count;
	uint32_t min_shared_buffers;

	ttransport = calloc(1, sizeof(*ttransport));
	if (!ttransport) {
		return NULL;
	}

	TAILQ_INIT(&ttransport->ports);
	TAILQ_INIT(&ttransport->poll_groups);

	ttransport->transport.ops = &spdk_nvmf_transport_tcp;

	ttransport->tcp_opts.c2h_success = SPDK_NVMF_TCP_DEFAULT_SUCCESS_OPTIMIZATION;
	ttransport->tcp_opts.sock_priority = SPDK_NVMF_TCP_DEFAULT_SOCK_PRIORITY;
	ttransport->tcp_opts.control_msg_num = SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM;
	if (opts->transport_specific != NULL &&
	    spdk_json_decode_object_relaxed(opts->transport_specific, tcp_transport_opts_decoder,
					    SPDK_COUNTOF(tcp_transport_opts_decoder),
					    &ttransport->tcp_opts)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		free(ttransport);
		return NULL;
	}

	SPDK_NOTICELOG("*** TCP Transport Init ***\n");

	SPDK_INFOLOG(nvmf_tcp, "*** TCP Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d\n"
		     "  num_shared_buffers=%d, c2h_success=%d,\n"
		     "  dif_insert_or_strip=%d, sock_priority=%d\n"
		     "  abort_timeout_sec=%d, control_msg_num=%hu\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     ttransport->tcp_opts.c2h_success,
		     opts->dif_insert_or_strip,
		     ttransport->tcp_opts.sock_priority,
		     opts->abort_timeout_sec,
		     ttransport->tcp_opts.control_msg_num);

	if (ttransport->tcp_opts.sock_priority > SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY) {
		SPDK_ERRLOG("Unsupported socket_priority=%d, the current range is: 0 to %d\n"
			    "you can use man 7 socket to view the range of priority under SO_PRIORITY item\n",
			    ttransport->tcp_opts.sock_priority, SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY);
		free(ttransport);
		return NULL;
	}

	if (ttransport->tcp_opts.control_msg_num == 0 &&
	    opts->in_capsule_data_size < SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE) {
		SPDK_WARNLOG("TCP param control_msg_num can't be 0 if ICD is less than %u bytes. Using default value %u\n",
			     SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE, SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM);
		ttransport->tcp_opts.control_msg_num = SPDK_NVMF_TCP_DEFAULT_CONTROL_MSG_NUM;
	}

	/* I/O unit size cannot be larger than max I/O size */
	if (opts->io_unit_size > opts->max_io_size) {
		opts->io_unit_size = opts->max_io_size;
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > SPDK_NVMF_MAX_SGL_ENTRIES) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		free(ttransport);
		return NULL;
	}

	min_shared_buffers = spdk_env_get_core_count() * opts->buf_cache_size;
	if (min_shared_buffers > opts->num_shared_buffers) {
		SPDK_ERRLOG("There are not enough buffers to satisfy "
			    "per-poll group caches for each thread. (%" PRIu32 ") "
			    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
		SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
		free(ttransport);
		return NULL;
	}

	pthread_mutex_init(&ttransport->lock, NULL);

	ttransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_tcp_accept, &ttransport->transport,
				    opts->acceptor_poll_rate);
	if (!ttransport->accept_poller) {
		pthread_mutex_destroy(&ttransport->lock);
		free(ttransport);
		return NULL;
	}

	return &ttransport->transport;
}

static int
nvmf_tcp_trsvcid_to_int(const char *trsvcid)
{
	unsigned long long ull;
	char *end = NULL;

	ull = strtoull(trsvcid, &end, 10);
	if (end == NULL || end == trsvcid || *end != '\0') {
		return -1;
	}

	/* Valid TCP/IP port numbers are in [0, 65535] */
	if (ull > 65535) {
		return -1;
	}

	return (int)ull;
}

/**
 * Canonicalize a listen address trid.
 */
static int
nvmf_tcp_canon_listen_trid(struct spdk_nvme_transport_id *canon_trid,
			   const struct spdk_nvme_transport_id *trid)
{
	int trsvcid_int;

	trsvcid_int = nvmf_tcp_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		return -EINVAL;
	}

	memset(canon_trid, 0, sizeof(*canon_trid));
	spdk_nvme_trid_populate_transport(canon_trid, SPDK_NVME_TRANSPORT_TCP);
	canon_trid->adrfam = trid->adrfam;
	snprintf(canon_trid->traddr, sizeof(canon_trid->traddr), "%s", trid->traddr);
	snprintf(canon_trid->trsvcid, sizeof(canon_trid->trsvcid), "%d", trsvcid_int);

	return 0;
}

/**
 * Find an existing listening port.
 *
 * Caller must hold ttransport->lock.
 */
static struct spdk_nvmf_tcp_port *
nvmf_tcp_find_port(struct spdk_nvmf_tcp_transport *ttransport,
		   const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_transport_id canon_trid;
	struct spdk_nvmf_tcp_port *port;

	if (nvmf_tcp_canon_listen_trid(&canon_trid, trid) != 0) {
		return NULL;
	}

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		if (spdk_nvme_transport_id_compare(&canon_trid, port->trid) == 0) {
			return port;
		}
	}

	return NULL;
}

static int
nvmf_tcp_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_listen_opts *listen_opts)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	int trsvcid_int;
	uint8_t adrfam;
	struct spdk_sock_opts opts;

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -EINVAL;
	}

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	trsvcid_int = nvmf_tcp_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		SPDK_ERRLOG("Invalid trsvcid '%s'\n", trid->trsvcid);
		return -EINVAL;
	}

	pthread_mutex_lock(&ttransport->lock);
	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	port->trid = trid;
	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.priority = ttransport->tcp_opts.sock_priority;
	port->listen_sock = spdk_sock_listen_ext(trid->traddr, trsvcid_int,
			    NULL, &opts);
	if (port->listen_sock == NULL) {
		SPDK_ERRLOG("spdk_sock_listen(%s, %d) failed: %s (%d)\n",
			    trid->traddr, trsvcid_int,
			    spdk_strerror(errno), errno);
		free(port);
		pthread_mutex_unlock(&ttransport->lock);
		return -errno;
	}

	if (spdk_sock_is_ipv4(port->listen_sock)) {
		adrfam = SPDK_NVMF_ADRFAM_IPV4;
	} else if (spdk_sock_is_ipv6(port->listen_sock)) {
		adrfam = SPDK_NVMF_ADRFAM_IPV6;
	} else {
		SPDK_ERRLOG("Unhandled socket type\n");
		adrfam = 0;
	}

	if (adrfam != trid->adrfam) {
		SPDK_ERRLOG("Socket address family mismatch\n");
		spdk_sock_close(&port->listen_sock);
		free(port);
		pthread_mutex_unlock(&ttransport->lock);
		return -EINVAL;
	}

	SPDK_NOTICELOG("*** NVMe/TCP Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	TAILQ_INSERT_TAIL(&ttransport->ports, port, link);
	pthread_mutex_unlock(&ttransport->lock);
	return 0;
}

static void
nvmf_tcp_stop_listen(struct spdk_nvmf_transport *transport,
		     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(nvmf_tcp, "Removing listen address %s port %s\n",
		      trid->traddr, trid->trsvcid);

	pthread_mutex_lock(&ttransport->lock);
	port = nvmf_tcp_find_port(ttransport, trid);
	if (port) {
		TAILQ_REMOVE(&ttransport->ports, port, link);
		spdk_sock_close(&port->listen_sock);
		free(port);
	}

	pthread_mutex_unlock(&ttransport->lock);
}

static void nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
		enum nvme_tcp_pdu_recv_state state);

static void
nvmf_tcp_qpair_set_state(struct spdk_nvmf_tcp_qpair *tqpair, enum nvme_tcp_qpair_state state)
{
	tqpair->state = state;
	spdk_trace_record(TRACE_TCP_QP_STATE_CHANGE, 0, 0, (uintptr_t)tqpair, tqpair->state);
}

static void
nvmf_tcp_qpair_disconnect(struct spdk_nvmf_tcp_qpair *tqpair)
{
	SPDK_DEBUGLOG(nvmf_tcp, "Disconnecting qpair %p\n", tqpair);

	spdk_trace_record(TRACE_TCP_QP_DISCONNECT, 0, 0, (uintptr_t)tqpair);

	if (tqpair->state <= NVME_TCP_QPAIR_STATE_RUNNING) {
		nvmf_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_EXITING);
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
		spdk_poller_unregister(&tqpair->timeout_poller);

		/* This will end up calling nvmf_tcp_close_qpair */
		spdk_nvmf_qpair_disconnect(&tqpair->qpair, NULL, NULL);
	}
}

static void
_mgmt_pdu_write_done(void *_tqpair, int err)
{
	struct spdk_nvmf_tcp_qpair *tqpair = _tqpair;
	struct nvme_tcp_pdu *pdu = tqpair->mgmt_pdu;

	if (spdk_unlikely(err != 0)) {
		nvmf_tcp_qpair_disconnect(tqpair);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(pdu->cb_arg);
}

static void
_req_pdu_write_done(void *req, int err)
{
	struct spdk_nvmf_tcp_req *tcp_req = req;
	struct nvme_tcp_pdu *pdu = tcp_req->pdu;
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;

	assert(tcp_req->pdu_in_use);
	tcp_req->pdu_in_use = false;

	/* If the request is in a completed state, we're waiting for write completion to free it */
	if (spdk_unlikely(tcp_req->state == TCP_REQUEST_STATE_COMPLETED)) {
		nvmf_tcp_request_free(tcp_req);
		return;
	}

	if (spdk_unlikely(err != 0)) {
		nvmf_tcp_qpair_disconnect(tqpair);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(pdu->cb_arg);
}

static void
_pdu_write_done(struct nvme_tcp_pdu *pdu, int err)
{
	pdu->sock_req.cb_fn(pdu->sock_req.cb_arg, err);
}

static void
_tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	uint32_t mapped_length = 0;
	ssize_t rc;
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;

	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       tqpair->host_hdgst_enable, tqpair->host_ddgst_enable,
			       &mapped_length);
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ) {
		rc = spdk_sock_writev(tqpair->sock, pdu->iov, pdu->sock_req.iovcnt);
		if (rc == mapped_length) {
			_pdu_write_done(pdu, 0);
		} else {
			SPDK_ERRLOG("IC_RESP or TERM_REQ could not write to socket.\n");
			_pdu_write_done(pdu, -1);
		}
	} else {
		spdk_sock_writev_async(tqpair->sock, &pdu->sock_req);
	}
}

static void
data_crc32_accel_done(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;

	if (spdk_unlikely(status)) {
		SPDK_ERRLOG("Failed to compute the data digest for pdu =%p\n", pdu);
		_pdu_write_done(pdu, status);
		return;
	}

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	MAKE_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);

	_tcp_write_pdu(pdu);
}

static void
pdu_data_crc32_compute(struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_qpair *tqpair = pdu->qpair;
	uint32_t crc32c;

	/* Data Digest */
	if (pdu->data_len > 0 && g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type] && tqpair->host_ddgst_enable) {
		/* Only suport this limitated case for the first step */
		if (spdk_likely(!pdu->dif_ctx && (pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT == 0)
				&& tqpair->group)) {
			spdk_accel_submit_crc32cv(tqpair->group->accel_channel, &pdu->data_digest_crc32,
						  pdu->data_iov, pdu->data_iovcnt, 0, data_crc32_accel_done, pdu);
			return;
		}

		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
	}

	_tcp_write_pdu(pdu);
}

static void
nvmf_tcp_qpair_write_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			 struct nvme_tcp_pdu *pdu,
			 nvme_tcp_qpair_xfer_complete_cb cb_fn,
			 void *cb_arg)
{
	int hlen;
	uint32_t crc32c;

	assert(tqpair->pdu_in_progress != pdu);

	hlen = pdu->hdr.common.hlen;
	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;

	pdu->iov[0].iov_base = &pdu->hdr.raw;
	pdu->iov[0].iov_len = hlen;

	/* Header Digest */
	if (g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type] && tqpair->host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr.raw + hlen, crc32c);
	}

	/* Data Digest */
	pdu_data_crc32_compute(pdu);
}

static void
nvmf_tcp_qpair_write_mgmt_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			      nvme_tcp_qpair_xfer_complete_cb cb_fn,
			      void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = tqpair->mgmt_pdu;

	pdu->sock_req.cb_fn = _mgmt_pdu_write_done;
	pdu->sock_req.cb_arg = tqpair;

	nvmf_tcp_qpair_write_pdu(tqpair, pdu, cb_fn, cb_arg);
}

static void
nvmf_tcp_qpair_write_req_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			     struct spdk_nvmf_tcp_req *tcp_req,
			     nvme_tcp_qpair_xfer_complete_cb cb_fn,
			     void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = tcp_req->pdu;

	pdu->sock_req.cb_fn = _req_pdu_write_done;
	pdu->sock_req.cb_arg = tcp_req;

	assert(!tcp_req->pdu_in_use);
	tcp_req->pdu_in_use = true;

	nvmf_tcp_qpair_write_pdu(tqpair, pdu, cb_fn, cb_arg);
}

static int
nvmf_tcp_qpair_init_mem_resource(struct spdk_nvmf_tcp_qpair *tqpair)
{
	uint32_t i;
	struct spdk_nvmf_transport_opts *opts;
	uint32_t in_capsule_data_size;

	opts = &tqpair->qpair.transport->opts;

	in_capsule_data_size = opts->in_capsule_data_size;
	if (opts->dif_insert_or_strip) {
		in_capsule_data_size = SPDK_BDEV_BUF_SIZE_WITH_MD(in_capsule_data_size);
	}

	tqpair->resource_count = opts->max_queue_depth;

	tqpair->reqs = calloc(tqpair->resource_count, sizeof(*tqpair->reqs));
	if (!tqpair->reqs) {
		SPDK_ERRLOG("Unable to allocate reqs on tqpair=%p\n", tqpair);
		return -1;
	}

	if (in_capsule_data_size) {
		tqpair->bufs = spdk_zmalloc(tqpair->resource_count * in_capsule_data_size, 0x1000,
					    NULL, SPDK_ENV_LCORE_ID_ANY,
					    SPDK_MALLOC_DMA);
		if (!tqpair->bufs) {
			SPDK_ERRLOG("Unable to allocate bufs on tqpair=%p.\n", tqpair);
			return -1;
		}
	}

	/* Add additional 2 members, which will be used for mgmt_pdu and pdu_in_progress owned by the tqpair */
	tqpair->pdus = spdk_dma_zmalloc((tqpair->resource_count + 2) * sizeof(*tqpair->pdus), 0x1000, NULL);
	if (!tqpair->pdus) {
		SPDK_ERRLOG("Unable to allocate pdu pool on tqpair =%p.\n", tqpair);
		return -1;
	}

	for (i = 0; i < tqpair->resource_count; i++) {
		struct spdk_nvmf_tcp_req *tcp_req = &tqpair->reqs[i];

		tcp_req->ttag = i + 1;
		tcp_req->req.qpair = &tqpair->qpair;

		tcp_req->pdu = &tqpair->pdus[i];
		tcp_req->pdu->qpair = tqpair;

		/* Set up memory to receive commands */
		if (tqpair->bufs) {
			tcp_req->buf = (void *)((uintptr_t)tqpair->bufs + (i * in_capsule_data_size));
		}

		/* Set the cmdn and rsp */
		tcp_req->req.rsp = (union nvmf_c2h_msg *)&tcp_req->rsp;
		tcp_req->req.cmd = (union nvmf_h2c_msg *)&tcp_req->cmd;

		/* Initialize request state to FREE */
		tcp_req->state = TCP_REQUEST_STATE_FREE;
		TAILQ_INSERT_TAIL(&tqpair->tcp_req_free_queue, tcp_req, state_link);
		tqpair->state_cntr[TCP_REQUEST_STATE_FREE]++;
	}

	tqpair->mgmt_pdu = &tqpair->pdus[i];
	tqpair->mgmt_pdu->qpair = tqpair;
	tqpair->pdu_in_progress = &tqpair->pdus[i + 1];

	tqpair->recv_buf_size = (in_capsule_data_size + sizeof(struct spdk_nvme_tcp_cmd) + 2 *
				 SPDK_NVME_TCP_DIGEST_LEN) * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;

	return 0;
}

static int
nvmf_tcp_qpair_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	SPDK_DEBUGLOG(nvmf_tcp, "New TCP Connection: %p\n", qpair);

	spdk_trace_record(TRACE_TCP_QP_CREATE, 0, 0, (uintptr_t)tqpair);

	/* Initialise request state queues of the qpair */
	TAILQ_INIT(&tqpair->tcp_req_free_queue);
	TAILQ_INIT(&tqpair->tcp_req_working_queue);

	tqpair->host_hdgst_enable = true;
	tqpair->host_ddgst_enable = true;

	return 0;
}

static int
nvmf_tcp_qpair_sock_init(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int rc;

	spdk_trace_record(TRACE_TCP_QP_SOCK_INIT, 0, 0, (uintptr_t)tqpair);

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(tqpair->sock, sizeof(struct spdk_nvme_tcp_common_pdu_hdr));
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed\n");
		return rc;
	}

	return 0;
}

static void
nvmf_tcp_handle_connect(struct spdk_nvmf_transport *transport,
			struct spdk_nvmf_tcp_port *port,
			struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair;
	int rc;

	SPDK_DEBUGLOG(nvmf_tcp, "New connection accepted on %s port %s\n",
		      port->trid->traddr, port->trid->trsvcid);

	tqpair = calloc(1, sizeof(struct spdk_nvmf_tcp_qpair));
	if (tqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_sock_close(&sock);
		return;
	}

	tqpair->sock = sock;
	tqpair->state_cntr[TCP_REQUEST_STATE_FREE] = 0;
	tqpair->port = port;
	tqpair->qpair.transport = transport;

	rc = spdk_sock_getaddr(tqpair->sock, tqpair->target_addr,
			       sizeof(tqpair->target_addr), &tqpair->target_port,
			       tqpair->initiator_addr, sizeof(tqpair->initiator_addr),
			       &tqpair->initiator_port);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed of tqpair=%p\n", tqpair);
		nvmf_tcp_qpair_destroy(tqpair);
		return;
	}

	spdk_nvmf_tgt_new_qpair(transport->tgt, &tqpair->qpair);
}

static uint32_t
nvmf_tcp_port_accept(struct spdk_nvmf_transport *transport, struct spdk_nvmf_tcp_port *port)
{
	struct spdk_sock *sock;
	uint32_t count = 0;
	int i;

	for (i = 0; i < NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(port->listen_sock);
		if (sock == NULL) {
			break;
		}
		count++;
		nvmf_tcp_handle_connect(transport, port, sock);
	}

	return count;
}

static int
nvmf_tcp_accept(void *ctx)
{
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	uint32_t count = 0;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		count += nvmf_tcp_port_accept(transport, port);
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_tcp_discover(struct spdk_nvmf_transport *transport,
		  struct spdk_nvme_transport_id *trid,
		  struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_TCP;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.tcp.sectype = SPDK_NVME_TCP_SECURITY_NONE;
}

static struct spdk_nvmf_tcp_control_msg_list *
nvmf_tcp_control_msg_list_create(uint16_t num_messages)
{
	struct spdk_nvmf_tcp_control_msg_list *list;
	struct spdk_nvmf_tcp_control_msg *msg;
	uint16_t i;

	list = calloc(1, sizeof(*list));
	if (!list) {
		SPDK_ERRLOG("Failed to allocate memory for list structure\n");
		return NULL;
	}

	list->msg_buf = spdk_zmalloc(num_messages * SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE,
				     NVMF_DATA_BUFFER_ALIGNMENT, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!list->msg_buf) {
		SPDK_ERRLOG("Failed to allocate memory for control message buffers\n");
		free(list);
		return NULL;
	}

	STAILQ_INIT(&list->free_msgs);

	for (i = 0; i < num_messages; i++) {
		msg = (struct spdk_nvmf_tcp_control_msg *)((char *)list->msg_buf + i *
				SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE);
		STAILQ_INSERT_TAIL(&list->free_msgs, msg, link);
	}

	return list;
}

static void
nvmf_tcp_control_msg_list_free(struct spdk_nvmf_tcp_control_msg_list *list)
{
	if (!list) {
		return;
	}

	spdk_free(list->msg_buf);
	free(list);
}

static struct spdk_nvmf_transport_poll_group *
nvmf_tcp_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	struct spdk_nvmf_tcp_poll_group *tgroup;

	tgroup = calloc(1, sizeof(*tgroup));
	if (!tgroup) {
		return NULL;
	}

	tgroup->sock_group = spdk_sock_group_create(&tgroup->group);
	if (!tgroup->sock_group) {
		goto cleanup;
	}

	TAILQ_INIT(&tgroup->qpairs);
	TAILQ_INIT(&tgroup->await_req);

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	if (transport->opts.in_capsule_data_size < SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE) {
		SPDK_DEBUGLOG(nvmf_tcp, "ICD %u is less than min required for admin/fabric commands (%u). "
			      "Creating control messages list\n", transport->opts.in_capsule_data_size,
			      SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE);
		tgroup->control_msg_list = nvmf_tcp_control_msg_list_create(ttransport->tcp_opts.control_msg_num);
		if (!tgroup->control_msg_list) {
			goto cleanup;
		}
	}

	tgroup->accel_channel = spdk_accel_engine_get_io_channel();
	if (spdk_unlikely(!tgroup->accel_channel)) {
		SPDK_ERRLOG("Cannot create accel_channel for tgroup=%p\n", tgroup);
		goto cleanup;
	}

	pthread_mutex_lock(&ttransport->lock);
	TAILQ_INSERT_TAIL(&ttransport->poll_groups, tgroup, link);
	if (ttransport->next_pg == NULL) {
		ttransport->next_pg = tgroup;
	}
	pthread_mutex_unlock(&ttransport->lock);

	return &tgroup->group;

cleanup:
	nvmf_tcp_poll_group_destroy(&tgroup->group);
	return NULL;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_tcp_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_transport_poll_group *result;
	struct spdk_nvmf_tcp_poll_group **pg;
	struct spdk_nvmf_tcp_qpair *tqpair;
	struct spdk_sock_group *group = NULL;
	int rc;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	rc = spdk_sock_get_optimal_sock_group(tqpair->sock, &group);
	if (!rc && group != NULL) {
		return spdk_sock_group_get_ctx(group);
	}

	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);

	pthread_mutex_lock(&ttransport->lock);

	if (TAILQ_EMPTY(&ttransport->poll_groups)) {
		pthread_mutex_unlock(&ttransport->lock);
		return NULL;
	}

	pg = &ttransport->next_pg;
	assert(*pg != NULL);

	result = &(*pg)->group;

	*pg = TAILQ_NEXT(*pg, link);
	if (*pg == NULL) {
		*pg = TAILQ_FIRST(&ttransport->poll_groups);
	}

	pthread_mutex_unlock(&ttransport->lock);
	return result;
}

static void
nvmf_tcp_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup, *next_tgroup;
	struct spdk_nvmf_tcp_transport *ttransport;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	spdk_sock_group_close(&tgroup->sock_group);
	if (tgroup->control_msg_list) {
		nvmf_tcp_control_msg_list_free(tgroup->control_msg_list);
	}

	if (tgroup->accel_channel) {
		spdk_put_io_channel(tgroup->accel_channel);
	}

	ttransport = SPDK_CONTAINEROF(tgroup->group.transport, struct spdk_nvmf_tcp_transport, transport);

	pthread_mutex_lock(&ttransport->lock);
	next_tgroup = TAILQ_NEXT(tgroup, link);
	TAILQ_REMOVE(&ttransport->poll_groups, tgroup, link);
	if (next_tgroup == NULL) {
		next_tgroup = TAILQ_FIRST(&ttransport->poll_groups);
	}
	if (ttransport->next_pg == tgroup) {
		ttransport->next_pg = next_tgroup;
	}
	pthread_mutex_unlock(&ttransport->lock);

	free(tgroup);
}

static void
nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
			      enum nvme_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ) {
		/* When leaving the await req state, move the qpair to the main list */
		TAILQ_REMOVE(&tqpair->group->await_req, tqpair, link);
		TAILQ_INSERT_TAIL(&tqpair->group->qpairs, tqpair, link);
	}

	SPDK_DEBUGLOG(nvmf_tcp, "tqpair(%p) recv state=%d\n", tqpair, state);
	tqpair->recv_state = state;

	spdk_trace_record(TRACE_TCP_QP_RCV_STATE_CHANGE, 0, 0, (uintptr_t)tqpair, tqpair->recv_state);

	switch (state) {
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
		break;
	case NVME_TCP_PDU_RECV_STATE_AWAIT_REQ:
		TAILQ_REMOVE(&tqpair->group->qpairs, tqpair, link);
		TAILQ_INSERT_TAIL(&tqpair->group->await_req, tqpair, link);
		break;
	case NVME_TCP_PDU_RECV_STATE_ERROR:
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
		memset(tqpair->pdu_in_progress, 0, sizeof(*(tqpair->pdu_in_progress)));
		break;
	default:
		SPDK_ERRLOG("The state(%d) is invalid\n", state);
		abort();
		break;
	}
}

static int
nvmf_tcp_qpair_handle_timeout(void *ctx)
{
	struct spdk_nvmf_tcp_qpair *tqpair = ctx;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);

	SPDK_ERRLOG("No pdu coming for tqpair=%p within %d seconds\n", tqpair,
		    SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT);

	nvmf_tcp_qpair_disconnect(tqpair);
	return SPDK_POLLER_BUSY;
}

static void
nvmf_tcp_send_c2h_term_req_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = (struct spdk_nvmf_tcp_qpair *)cb_arg;

	if (!tqpair->timeout_poller) {
		tqpair->timeout_poller = SPDK_POLLER_REGISTER(nvmf_tcp_qpair_handle_timeout, tqpair,
					 SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT * 1000000);
	}
}

static void
nvmf_tcp_send_c2h_term_req(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
			   enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req;
	uint32_t c2h_term_req_hdr_len = sizeof(*c2h_term_req);
	uint32_t copy_len;

	rsp_pdu = tqpair->mgmt_pdu;

	c2h_term_req = &rsp_pdu->hdr.term_req;
	c2h_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ;
	c2h_term_req->common.hlen = c2h_term_req_hdr_len;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&c2h_term_req->fei, error_offset);
	}

	copy_len = spdk_min(pdu->hdr.common.hlen, SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)rsp_pdu->hdr.raw + c2h_term_req_hdr_len, pdu->hdr.raw, copy_len);
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr.raw + c2h_term_req_hdr_len, copy_len);

	/* Contain the header of the wrong received pdu */
	c2h_term_req->common.plen = c2h_term_req->common.hlen + copy_len;
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
	nvmf_tcp_qpair_write_mgmt_pdu(tqpair, nvmf_tcp_send_c2h_term_req_complete, tqpair);
}

static void
nvmf_tcp_capsule_cmd_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
				struct spdk_nvmf_tcp_qpair *tqpair,
				struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;

	assert(pdu->psh_valid_bytes == pdu->psh_len);
	assert(pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD);

	tcp_req = nvmf_tcp_req_get(tqpair);
	if (!tcp_req) {
		/* Directly return and make the allocation retry again.  This can happen if we're
		 * using asynchronous writes to send the response to the host or when releasing
		 * zero-copy buffers after a response has been sent.  In both cases, the host might
		 * receive the response before we've finished processing the request and is free to
		 * send another one.
		 */
		if (tqpair->state_cntr[TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST] > 0 ||
		    tqpair->state_cntr[TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE] > 0) {
			return;
		}

		/* The host sent more commands than the maximum queue depth. */
		SPDK_ERRLOG("Cannot allocate tcp_req on tqpair=%p\n", tqpair);
		nvmf_tcp_qpair_disconnect(tqpair);
		return;
	}

	pdu->req = tcp_req;
	assert(tcp_req->state == TCP_REQUEST_STATE_NEW);
	nvmf_tcp_req_process(ttransport, tcp_req);
}

static void
nvmf_tcp_capsule_cmd_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
				    struct spdk_nvmf_tcp_qpair *tqpair,
				    struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	struct spdk_nvme_cpl *rsp;

	capsule_cmd = &pdu->hdr.capsule_cmd;
	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	/* Zero-copy requests don't support ICD */
	assert(!spdk_nvmf_request_using_zcopy(&tcp_req->req));

	if (capsule_cmd->common.pdo > SPDK_NVME_TCP_PDU_PDO_MAX_OFFSET) {
		SPDK_ERRLOG("Expected ICReq capsule_cmd pdu offset <= %d, got %c\n",
			    SPDK_NVME_TCP_PDU_PDO_MAX_OFFSET, capsule_cmd->common.pdo);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo);
		goto err;
	}

	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	rsp = &tcp_req->req.rsp->nvme_cpl;
	if (spdk_unlikely(rsp->status.sc == SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR)) {
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
	} else {
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
	}

	nvmf_tcp_req_process(ttransport, tcp_req);

	return;
err:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static int
nvmf_tcp_find_req_in_state(struct spdk_nvmf_tcp_qpair *tqpair,
			   enum spdk_nvmf_tcp_req_state state,
			   uint16_t cid, uint16_t tag,
			   struct spdk_nvmf_tcp_req **req)
{
	struct spdk_nvmf_tcp_req *tcp_req = NULL;

	TAILQ_FOREACH(tcp_req, &tqpair->tcp_req_working_queue, state_link) {
		if (tcp_req->state != state) {
			continue;
		}

		if (tcp_req->req.cmd->nvme_cmd.cid != cid) {
			continue;
		}

		if (tcp_req->ttag == tag) {
			*req = tcp_req;
			return 0;
		}

		*req = NULL;
		return -1;
	}

	/* Didn't find it, but not an error */
	*req = NULL;
	return 0;
}

static void
nvmf_tcp_h2c_data_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
			     struct spdk_nvmf_tcp_qpair *tqpair,
			     struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes = 0;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	int rc;

	h2c_data = &pdu->hdr.h2c_data;

	SPDK_DEBUGLOG(nvmf_tcp, "tqpair=%p, r2t_info: datao=%u, datal=%u, cccid=%u, ttag=%u\n",
		      tqpair, h2c_data->datao, h2c_data->datal, h2c_data->cccid, h2c_data->ttag);

	rc = nvmf_tcp_find_req_in_state(tqpair, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					h2c_data->cccid, h2c_data->ttag, &tcp_req);
	if (rc == 0 && tcp_req == NULL) {
		rc = nvmf_tcp_find_req_in_state(tqpair, TCP_REQUEST_STATE_AWAITING_R2T_ACK, h2c_data->cccid,
						h2c_data->ttag, &tcp_req);
	}

	if (!tcp_req) {
		SPDK_DEBUGLOG(nvmf_tcp, "tcp_req is not found for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER;
		if (rc == 0) {
			error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, cccid);
		} else {
			error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, ttag);
		}
		goto err;
	}

	if (tcp_req->h2c_offset != h2c_data->datao) {
		SPDK_DEBUGLOG(nvmf_tcp,
			      "tcp_req(%p), tqpair=%p, expected data offset %u, but data offset is %u\n",
			      tcp_req, tqpair, tcp_req->h2c_offset, h2c_data->datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	if ((h2c_data->datao + h2c_data->datal) > tcp_req->req.length) {
		SPDK_DEBUGLOG(nvmf_tcp,
			      "tcp_req(%p), tqpair=%p,  (datao=%u + datal=%u) exceeds requested length=%u\n",
			      tcp_req, tqpair, h2c_data->datao, h2c_data->datal, tcp_req->req.length);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	pdu->req = tcp_req;

	if (spdk_unlikely(tcp_req->req.dif_enabled)) {
		pdu->dif_ctx = &tcp_req->req.dif.dif_ctx;
	}

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
				  h2c_data->datao, h2c_data->datal);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

err:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvmf_tcp_send_capsule_resp_pdu(struct spdk_nvmf_tcp_req *tcp_req,
			       struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_rsp *capsule_resp;

	SPDK_DEBUGLOG(nvmf_tcp, "enter, tqpair=%p\n", tqpair);

	rsp_pdu = nvmf_tcp_req_pdu_init(tcp_req);
	assert(rsp_pdu != NULL);

	capsule_resp = &rsp_pdu->hdr.capsule_resp;
	capsule_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP;
	capsule_resp->common.plen = capsule_resp->common.hlen = sizeof(*capsule_resp);
	capsule_resp->rccqe = tcp_req->req.rsp->nvme_cpl;
	if (tqpair->host_hdgst_enable) {
		capsule_resp->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		capsule_resp->common.plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	nvmf_tcp_qpair_write_req_pdu(tqpair, tcp_req, nvmf_tcp_request_free, tcp_req);
}

static void
nvmf_tcp_pdu_c2h_data_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair != NULL);

	if (spdk_unlikely(tcp_req->pdu->rw_offset < tcp_req->req.length)) {
		SPDK_DEBUGLOG(nvmf_tcp, "sending another C2H part, offset %u length %u\n", tcp_req->pdu->rw_offset,
			      tcp_req->req.length);
		_nvmf_tcp_send_c2h_data(tqpair, tcp_req);
		return;
	}

	if (tcp_req->pdu->hdr.c2h_data.common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
		nvmf_tcp_request_free(tcp_req);
	} else {
		nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}
}

static void
nvmf_tcp_r2t_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;
	struct spdk_nvmf_tcp_transport *ttransport;

	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);

	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

	if (tcp_req->h2c_offset == tcp_req->req.length) {
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		nvmf_tcp_req_process(ttransport, tcp_req);
	}
}

static void
nvmf_tcp_send_r2t_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
		      struct spdk_nvmf_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_r2t_hdr *r2t;

	rsp_pdu = nvmf_tcp_req_pdu_init(tcp_req);
	assert(rsp_pdu != NULL);

	r2t = &rsp_pdu->hdr.r2t;
	r2t->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_R2T;
	r2t->common.plen = r2t->common.hlen = sizeof(*r2t);

	if (tqpair->host_hdgst_enable) {
		r2t->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		r2t->common.plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	r2t->cccid = tcp_req->req.cmd->nvme_cmd.cid;
	r2t->ttag = tcp_req->ttag;
	r2t->r2to = tcp_req->h2c_offset;
	r2t->r2tl = tcp_req->req.length;

	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_R2T_ACK);

	SPDK_DEBUGLOG(nvmf_tcp,
		      "tcp_req(%p) on tqpair(%p), r2t_info: cccid=%u, ttag=%u, r2to=%u, r2tl=%u\n",
		      tcp_req, tqpair, r2t->cccid, r2t->ttag, r2t->r2to, r2t->r2tl);
	nvmf_tcp_qpair_write_req_pdu(tqpair, tcp_req, nvmf_tcp_r2t_complete, tcp_req);
}

static void
nvmf_tcp_h2c_data_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
				 struct spdk_nvmf_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvme_cpl *rsp;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	tcp_req->h2c_offset += pdu->data_len;

	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	/* Wait for all of the data to arrive AND for the initial R2T PDU send to be
	 * acknowledged before moving on. */
	if (tcp_req->h2c_offset == tcp_req->req.length &&
	    tcp_req->state == TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER) {
		/* After receiving all the h2c data, we need to check whether there is
		 * transient transport error */
		rsp = &tcp_req->req.rsp->nvme_cpl;
		if (spdk_unlikely(rsp->status.sc == SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR)) {
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
		} else {
			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		}
		nvmf_tcp_req_process(ttransport, tcp_req);
	}
}

static void
nvmf_tcp_h2c_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *h2c_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", h2c_term_req,
		    spdk_nvmf_tcp_term_req_fes_str[h2c_term_req->fes]);
	if ((h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		SPDK_DEBUGLOG(nvmf_tcp, "The offset from the start of the PDU header is %u\n",
			      DGET32(h2c_term_req->fei));
	}
}

static void
nvmf_tcp_h2c_term_req_hdr_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	if (h2c_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		SPDK_ERRLOG("Fatal Error Status(FES) is unknown for h2c_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr.raw + h2c_term_req->common.hlen,
			      h2c_term_req->common.plen - h2c_term_req->common.hlen);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvmf_tcp_h2c_term_req_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;

	nvmf_tcp_h2c_term_req_dump(h2c_term_req);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
}

static void
_nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
			     struct spdk_nvmf_tcp_transport *ttransport)
{
	struct nvme_tcp_pdu *pdu = tqpair->pdu_in_progress;

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		nvmf_tcp_capsule_cmd_payload_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		nvmf_tcp_h2c_data_payload_handle(ttransport, tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		nvmf_tcp_h2c_term_req_payload_handle(tqpair, pdu);
		break;

	default:
		/* The code should not go to here */
		SPDK_ERRLOG("The code should not go to here\n");
		break;
	}
}

static void
nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
			    struct spdk_nvmf_tcp_transport *ttransport)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t crc32c;
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvme_cpl *rsp;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	pdu = tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");
	/* check data digest if need */
	if (pdu->ddgst_enable) {
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("Data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			tcp_req = pdu->req;
			assert(tcp_req != NULL);
			rsp = &tcp_req->req.rsp->nvme_cpl;
			rsp->status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
		}
	}

	_nvmf_tcp_pdu_payload_handle(tqpair, ttransport);
}

static void
nvmf_tcp_send_icresp_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = cb_arg;

	nvmf_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_RUNNING);
}

static void
nvmf_tcp_icreq_handle(struct spdk_nvmf_tcp_transport *ttransport,
		      struct spdk_nvmf_tcp_qpair *tqpair,
		      struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_req *ic_req = &pdu->hdr.ic_req;
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_ic_resp *ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	/* Only PFV 0 is defined currently */
	if (ic_req->pfv != 0) {
		SPDK_ERRLOG("Expected ICReq PFV %u, got %u\n", 0u, ic_req->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_req, pfv);
		goto end;
	}

	/* MAXR2T is 0's based */
	SPDK_DEBUGLOG(nvmf_tcp, "maxr2t =%u\n", (ic_req->maxr2t + 1u));

	tqpair->host_hdgst_enable = ic_req->dgst.bits.hdgst_enable ? true : false;
	if (!tqpair->host_hdgst_enable) {
		tqpair->recv_buf_size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	}

	tqpair->host_ddgst_enable = ic_req->dgst.bits.ddgst_enable ? true : false;
	if (!tqpair->host_ddgst_enable) {
		tqpair->recv_buf_size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	}

	tqpair->recv_buf_size = spdk_max(tqpair->recv_buf_size, MIN_SOCK_PIPE_SIZE);
	/* Now that we know whether digests are enabled, properly size the receive buffer */
	if (spdk_sock_set_recvbuf(tqpair->sock, tqpair->recv_buf_size) < 0) {
		SPDK_WARNLOG("Unable to allocate enough memory for receive buffer on tqpair=%p with size=%d\n",
			     tqpair,
			     tqpair->recv_buf_size);
		/* Not fatal. */
	}

	tqpair->cpda = spdk_min(ic_req->hpda, SPDK_NVME_TCP_CPDA_MAX);
	SPDK_DEBUGLOG(nvmf_tcp, "cpda of tqpair=(%p) is : %u\n", tqpair, tqpair->cpda);

	rsp_pdu = tqpair->mgmt_pdu;

	ic_resp = &rsp_pdu->hdr.ic_resp;
	ic_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	ic_resp->common.hlen = ic_resp->common.plen =  sizeof(*ic_resp);
	ic_resp->pfv = 0;
	ic_resp->cpda = tqpair->cpda;
	ic_resp->maxh2cdata = ttransport->transport.opts.max_io_size;
	ic_resp->dgst.bits.hdgst_enable = tqpair->host_hdgst_enable ? 1 : 0;
	ic_resp->dgst.bits.ddgst_enable = tqpair->host_ddgst_enable ? 1 : 0;

	SPDK_DEBUGLOG(nvmf_tcp, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(nvmf_tcp, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	nvmf_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_INITIALIZING);
	nvmf_tcp_qpair_write_mgmt_pdu(tqpair, nvmf_tcp_send_icresp_complete, tqpair);
	nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	return;
end:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvmf_tcp_pdu_psh_handle(struct spdk_nvmf_tcp_qpair *tqpair,
			struct spdk_nvmf_tcp_transport *ttransport)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(nvmf_tcp, "pdu type of tqpair(%p) is %d\n", tqpair,
		      pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		SPDK_DEBUGLOG(nvmf_tcp, "Compare the header of pdu=%p on tqpair=%p\n", pdu, tqpair);
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("Header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_REQ:
		nvmf_tcp_icreq_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_REQ);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		nvmf_tcp_h2c_data_hdr_handle(ttransport, tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		nvmf_tcp_h2c_term_req_hdr_handle(tqpair, pdu);
		break;

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->pdu_in_progress->hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
		break;
	}
}

static void
nvmf_tcp_pdu_ch_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint8_t expected_hlen, pdo;
	bool plen_error = false, pdo_error = false;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
	pdu = tqpair->pdu_in_progress;

	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received ICreq PDU, and reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_req);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
			SPDK_ERRLOG("The TCP/IP connection is not negotiated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
			expected_hlen = sizeof(struct spdk_nvme_tcp_cmd);
			pdo = pdu->hdr.common.pdo;
			if ((tqpair->cpda != 0) && (pdo % ((tqpair->cpda + 1) << 2) != 0)) {
				pdo_error = true;
				break;
			}

			if (pdu->hdr.common.plen < expected_hlen) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_h2c_data_hdr);
			pdo = pdu->hdr.common.pdo;
			if ((tqpair->cpda != 0) && (pdo % ((tqpair->cpda + 1) << 2) != 0)) {
				pdo_error = true;
				break;
			}
			if (pdu->hdr.common.plen < expected_hlen) {
				plen_error = true;
			}
			break;

		case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr.common.plen <= expected_hlen) ||
			    (pdu->hdr.common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", pdu->hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr.common.hlen != expected_hlen) {
		SPDK_ERRLOG("PDU type=0x%02x, Expected ICReq header length %u, got %u on tqpair=%p\n",
			    pdu->hdr.common.pdu_type,
			    expected_hlen, pdu->hdr.common.hlen, tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;
	} else if (pdo_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo);
	} else if (plen_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(tqpair->pdu_in_progress, tqpair->host_hdgst_enable);
		return;
	}
err:
	nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static int
nvmf_tcp_pdu_payload_insert_dif(struct nvme_tcp_pdu *pdu, uint32_t read_offset,
				int read_len)
{
	int rc;

	rc = spdk_dif_generate_stream(pdu->data_iov, pdu->data_iovcnt,
				      read_offset, read_len, pdu->dif_ctx);
	if (rc != 0) {
		SPDK_ERRLOG("DIF generate failed\n");
	}

	return rc;
}

static int
nvmf_tcp_sock_process(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	enum nvme_tcp_pdu_recv_state prev_state;
	uint32_t data_len;
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport,
			struct spdk_nvmf_tcp_transport, transport);

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tqpair->recv_state;
		SPDK_DEBUGLOG(nvmf_tcp, "tqpair(%p) recv pdu entering state %d\n", tqpair, prev_state);

		pdu = tqpair->pdu_in_progress;
		switch (tqpair->recv_state) {
		/* Wait for the common header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			if (spdk_unlikely(tqpair->state == NVME_TCP_QPAIR_STATE_INITIALIZING)) {
				return rc;
			}

			rc = nvme_tcp_read_data(tqpair->sock,
						sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes,
						(void *)&pdu->hdr.common + pdu->ch_valid_bytes);
			if (rc < 0) {
				SPDK_DEBUGLOG(nvmf_tcp, "will disconnect tqpair=%p\n", tqpair);
				return NVME_TCP_PDU_FATAL;
			} else if (rc > 0) {
				pdu->ch_valid_bytes += rc;
				spdk_trace_record(TRACE_TCP_READ_FROM_SOCKET_DONE, 0, rc, 0, tqpair);
				if (spdk_likely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY)) {
					nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
				}
			}

			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* The command header of this PDU has now been read from the socket. */
			nvmf_tcp_pdu_ch_handle(tqpair);
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			rc = nvme_tcp_read_data(tqpair->sock,
						pdu->psh_len - pdu->psh_valid_bytes,
						(void *)&pdu->hdr.raw + sizeof(struct spdk_nvme_tcp_common_pdu_hdr) + pdu->psh_valid_bytes);
			if (rc < 0) {
				return NVME_TCP_PDU_FATAL;
			} else if (rc > 0) {
				spdk_trace_record(TRACE_TCP_READ_FROM_SOCKET_DONE, 0, rc, 0, tqpair);
				pdu->psh_valid_bytes += rc;
			}

			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
			nvmf_tcp_pdu_psh_handle(tqpair, ttransport);
			break;
		/* Wait for the req slot */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_REQ:
			nvmf_tcp_capsule_cmd_hdr_handle(ttransport, tqpair, pdu);
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			/* data digest */
			if (spdk_unlikely((pdu->hdr.common.pdu_type != SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ) &&
					  tqpair->host_ddgst_enable)) {
				data_len += SPDK_NVME_TCP_DIGEST_LEN;
				pdu->ddgst_enable = true;
			}

			rc = nvme_tcp_read_payload_data(tqpair->sock, pdu);
			if (rc < 0) {
				return NVME_TCP_PDU_FATAL;
			}
			pdu->rw_offset += rc;

			if (spdk_unlikely(pdu->dif_ctx != NULL)) {
				rc = nvmf_tcp_pdu_payload_insert_dif(pdu, pdu->rw_offset - rc, rc);
				if (rc != 0) {
					return NVME_TCP_PDU_FATAL;
				}
			}

			if (pdu->rw_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All of this PDU has now been read from the socket. */
			nvmf_tcp_pdu_payload_handle(tqpair, ttransport);
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			if (!spdk_sock_is_connected(tqpair->sock)) {
				return NVME_TCP_PDU_FATAL;
			}
			break;
		default:
			assert(0);
			SPDK_ERRLOG("code should not come to here");
			break;
		}
	} while (tqpair->recv_state != prev_state);

	return rc;
}

static inline void *
nvmf_tcp_control_msg_get(struct spdk_nvmf_tcp_control_msg_list *list)
{
	struct spdk_nvmf_tcp_control_msg *msg;

	assert(list);

	msg = STAILQ_FIRST(&list->free_msgs);
	if (!msg) {
		SPDK_DEBUGLOG(nvmf_tcp, "Out of control messages\n");
		return NULL;
	}
	STAILQ_REMOVE_HEAD(&list->free_msgs, link);
	return msg;
}

static inline void
nvmf_tcp_control_msg_put(struct spdk_nvmf_tcp_control_msg_list *list, void *_msg)
{
	struct spdk_nvmf_tcp_control_msg *msg = _msg;

	assert(list);
	STAILQ_INSERT_HEAD(&list->free_msgs, msg, link);
}

static int
nvmf_tcp_req_parse_sgl(struct spdk_nvmf_tcp_req *tcp_req,
		       struct spdk_nvmf_transport *transport,
		       struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_request		*req = &tcp_req->req;
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;
	struct spdk_nvmf_tcp_poll_group		*tgroup;
	uint32_t				length;

	cmd = &req->cmd->nvme_cmd;
	rsp = &req->rsp->nvme_cpl;
	sgl = &cmd->dptr.sgl1;

	length = sgl->unkeyed.length;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK &&
	    sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_TRANSPORT) {
		if (length > transport->opts.max_io_size) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    length, transport->opts.max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		/* fill request length and populate iovs */
		req->length = length;

		SPDK_DEBUGLOG(nvmf_tcp, "Data requested length= 0x%x\n", length);

		if (spdk_unlikely(req->dif_enabled)) {
			req->dif.orig_length = length;
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		if (nvmf_ctrlr_use_zcopy(req)) {
			SPDK_DEBUGLOG(nvmf_tcp, "Using zero-copy to execute request %p\n", tcp_req);
			req->data_from_pool = false;
			return 0;
		}

		if (spdk_nvmf_request_get_buffers(req, group, transport, length)) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(nvmf_tcp, "No available large data buffers. Queueing request %p\n",
				      tcp_req);
			return 0;
		}

		/* backward compatible */
		req->data = req->iov[0].iov_base;

		SPDK_DEBUGLOG(nvmf_tcp, "Request %p took %d buffer/s from central pool, and data=%p\n",
			      tcp_req, req->iovcnt, req->data);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = transport->opts.in_capsule_data_size;
		assert(tcp_req->has_in_capsule_data);

		SPDK_DEBUGLOG(nvmf_tcp, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, length);

		if (offset > max_len) {
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
				    offset, max_len);
			rsp->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
			return -1;
		}
		max_len -= (uint32_t)offset;

		if (spdk_unlikely(length > max_len)) {
			/* According to the SPEC we should support ICD up to 8192 bytes for admin and fabric commands */
			if (length <= SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE &&
			    (cmd->opc == SPDK_NVME_OPC_FABRIC || req->qpair->qid == 0)) {

				/* Get a buffer from dedicated list */
				SPDK_DEBUGLOG(nvmf_tcp, "Getting a buffer from control msg list\n");
				tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
				assert(tgroup->control_msg_list);
				req->data = nvmf_tcp_control_msg_get(tgroup->control_msg_list);
				if (!req->data) {
					/* No available buffers. Queue this request up. */
					SPDK_DEBUGLOG(nvmf_tcp, "No available ICD buffers. Queueing request %p\n", tcp_req);
					return 0;
				}
			} else {
				SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
					    length, max_len);
				rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				return -1;
			}
		} else {
			req->data = tcp_req->buf;
		}

		req->length = length;
		req->data_from_pool = false;

		if (spdk_unlikely(req->dif_enabled)) {
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		req->iov[0].iov_base = req->data;
		req->iov[0].iov_len = length;
		req->iovcnt = 1;

		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static inline enum spdk_nvme_media_error_status_code
nvmf_tcp_dif_error_to_compl_status(uint8_t err_type) {
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
		break;
	}

	return result;
}

static void
_nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
			struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(
				tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint32_t plen, pdo, alignment;
	int rc;

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	rsp_pdu = tcp_req->pdu;
	assert(rsp_pdu != NULL);

	c2h_data = &rsp_pdu->hdr.c2h_data;
	c2h_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_DATA;
	plen = c2h_data->common.hlen = sizeof(*c2h_data);

	if (tqpair->host_hdgst_enable) {
		plen += SPDK_NVME_TCP_DIGEST_LEN;
		c2h_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	}

	/* set the psh */
	c2h_data->cccid = tcp_req->req.cmd->nvme_cmd.cid;
	c2h_data->datal = tcp_req->req.length - tcp_req->pdu->rw_offset;
	c2h_data->datao = tcp_req->pdu->rw_offset;

	/* set the padding */
	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (plen % alignment != 0) {
			pdo = (plen + alignment) / alignment * alignment;
			rsp_pdu->padding_len = pdo - plen;
			plen = pdo;
		}
	}

	c2h_data->common.pdo = pdo;
	plen += c2h_data->datal;
	if (tqpair->host_ddgst_enable) {
		c2h_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	c2h_data->common.plen = plen;

	if (spdk_unlikely(tcp_req->req.dif_enabled)) {
		rsp_pdu->dif_ctx = &tcp_req->req.dif.dif_ctx;
	}

	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
				  c2h_data->datao, c2h_data->datal);


	c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU;
	/* Need to send the capsule response if response is not all 0 */
	if (ttransport->tcp_opts.c2h_success &&
	    tcp_req->rsp.cdw0 == 0 && tcp_req->rsp.cdw1 == 0) {
		c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS;
	}

	if (spdk_unlikely(tcp_req->req.dif_enabled)) {
		struct spdk_nvme_cpl *rsp = &tcp_req->req.rsp->nvme_cpl;
		struct spdk_dif_error err_blk = {};
		uint32_t mapped_length = 0;
		uint32_t available_iovs = SPDK_COUNTOF(rsp_pdu->iov);
		uint32_t ddgst_len = 0;

		if (tqpair->host_ddgst_enable) {
			/* Data digest consumes additional iov entry */
			available_iovs--;
			/* plen needs to be updated since nvme_tcp_build_iovs compares expected and actual plen */
			ddgst_len = SPDK_NVME_TCP_DIGEST_LEN;
			c2h_data->common.plen -= ddgst_len;
		}
		/* Temp call to estimate if data can be described by limited number of iovs.
		 * iov vector will be rebuilt in nvmf_tcp_qpair_write_pdu */
		nvme_tcp_build_iovs(rsp_pdu->iov, available_iovs, rsp_pdu, tqpair->host_hdgst_enable,
				    false, &mapped_length);

		if (mapped_length != c2h_data->common.plen) {
			c2h_data->datal = mapped_length - (c2h_data->common.plen - c2h_data->datal);
			SPDK_DEBUGLOG(nvmf_tcp,
				      "Part C2H, data_len %u (of %u), PDU len %u, updated PDU len %u, offset %u\n",
				      c2h_data->datal, tcp_req->req.length, c2h_data->common.plen, mapped_length, rsp_pdu->rw_offset);
			c2h_data->common.plen = mapped_length;

			/* Rebuild pdu->data_iov since data length is changed */
			nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->req.iov, tcp_req->req.iovcnt, c2h_data->datao,
						  c2h_data->datal);

			c2h_data->common.flags &= ~(SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU |
						    SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS);
		}

		c2h_data->common.plen += ddgst_len;

		assert(rsp_pdu->rw_offset <= tcp_req->req.length);

		rc = spdk_dif_verify_stream(rsp_pdu->data_iov, rsp_pdu->data_iovcnt,
					    0, rsp_pdu->data_len, rsp_pdu->dif_ctx, &err_blk);
		if (rc != 0) {
			SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
				    err_blk.err_type, err_blk.err_offset);
			rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
			rsp->status.sc = nvmf_tcp_dif_error_to_compl_status(err_blk.err_type);
			nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
			return;
		}
	}

	rsp_pdu->rw_offset += c2h_data->datal;
	nvmf_tcp_qpair_write_req_pdu(tqpair, tcp_req, nvmf_tcp_pdu_c2h_data_complete, tcp_req);
}

static void
nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
		       struct spdk_nvmf_tcp_req *tcp_req)
{
	nvmf_tcp_req_pdu_init(tcp_req);
	_nvmf_tcp_send_c2h_data(tqpair, tcp_req);
}

static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp;

	SPDK_DEBUGLOG(nvmf_tcp, "enter\n");

	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS && req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		nvmf_tcp_send_c2h_data(tqpair, tcp_req);
	} else {
		nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}

	return 0;
}

static void
nvmf_tcp_set_in_capsule_data(struct spdk_nvmf_tcp_qpair *tqpair,
			     struct spdk_nvmf_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t plen = 0;

	pdu = tqpair->pdu_in_progress;
	plen = pdu->hdr.common.hlen;

	if (tqpair->host_hdgst_enable) {
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (pdu->hdr.common.plen != plen) {
		tcp_req->has_in_capsule_data = true;
	}
}

static bool
nvmf_tcp_req_process(struct spdk_nvmf_tcp_transport *ttransport,
		     struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_qpair		*tqpair;
	int					rc;
	enum spdk_nvmf_tcp_req_state		prev_state;
	bool					progress = false;
	struct spdk_nvmf_transport		*transport = &ttransport->transport;
	struct spdk_nvmf_transport_poll_group	*group;
	struct spdk_nvmf_tcp_poll_group		*tgroup;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	group = &tqpair->group->group;
	assert(tcp_req->state != TCP_REQUEST_STATE_FREE);

	/* If the qpair is not active, we need to abort the outstanding requests. */
	if (tqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
		if (tcp_req->state == TCP_REQUEST_STATE_NEED_BUFFER) {
			STAILQ_REMOVE(&group->pending_buf_queue, &tcp_req->req, spdk_nvmf_request, buf_link);
		}
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
	}

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tcp_req->state;

		SPDK_DEBUGLOG(nvmf_tcp, "Request %p entering state %d on tqpair=%p\n", tcp_req, prev_state,
			      tqpair);

		switch (tcp_req->state) {
		case TCP_REQUEST_STATE_FREE:
			/* Some external code must kick a request into TCP_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEW, 0, 0, (uintptr_t)tcp_req, tqpair);

			/* copy the cmd from the receive pdu */
			tcp_req->cmd = tqpair->pdu_in_progress->hdr.capsule_cmd.ccsqe;

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&tcp_req->req, &tcp_req->req.dif.dif_ctx))) {
				tcp_req->req.dif_enabled = true;
				tqpair->pdu_in_progress->dif_ctx = &tcp_req->req.dif.dif_ctx;
			}

			/* The next state transition depends on the data transfer needs of this request. */
			tcp_req->req.xfer = spdk_nvmf_req_get_xfer(&tcp_req->req);

			if (spdk_unlikely(tcp_req->req.xfer == SPDK_NVME_DATA_BIDIRECTIONAL)) {
				tcp_req->req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
				tcp_req->req.rsp->nvme_cpl.status.sc  = SPDK_NVME_SC_INVALID_OPCODE;
				tcp_req->req.rsp->nvme_cpl.cid = tcp_req->req.cmd->nvme_cmd.cid;
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				SPDK_DEBUGLOG(nvmf_tcp, "Request %p: invalid xfer type (BIDIRECTIONAL)\n", tcp_req);
				break;
			}

			/* If no data to transfer, ready to execute. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_NONE) {
				/* Reset the tqpair receiving pdu state */
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
				break;
			}

			nvmf_tcp_set_in_capsule_data(tqpair, tcp_req);

			if (!tcp_req->has_in_capsule_data) {
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_NEED_BUFFER);
			STAILQ_INSERT_TAIL(&group->pending_buf_queue, &tcp_req->req, buf_link);
			break;
		case TCP_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEED_BUFFER, 0, 0, (uintptr_t)tcp_req, tqpair);

			assert(tcp_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (!tcp_req->has_in_capsule_data && (&tcp_req->req != STAILQ_FIRST(&group->pending_buf_queue))) {
				SPDK_DEBUGLOG(nvmf_tcp,
					      "Not the first element to wait for the buf for tcp_req(%p) on tqpair=%p\n",
					      tcp_req, tqpair);
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = nvmf_tcp_req_parse_sgl(tcp_req, transport, group);
			if (rc < 0) {
				STAILQ_REMOVE_HEAD(&group->pending_buf_queue, buf_link);
				/* Reset the tqpair receiving pdu state */
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				tcp_req->req.rsp->nvme_cpl.cid = tcp_req->req.cmd->nvme_cmd.cid;
				nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				break;
			}

			/* Get a zcopy buffer if the request can be serviced through zcopy */
			if (spdk_nvmf_request_using_zcopy(&tcp_req->req)) {
				if (spdk_unlikely(tcp_req->req.dif_enabled)) {
					assert(tcp_req->req.dif.elba_length >= tcp_req->req.length);
					tcp_req->req.length = tcp_req->req.dif.elba_length;
				}

				STAILQ_REMOVE(&group->pending_buf_queue, &tcp_req->req, spdk_nvmf_request, buf_link);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_START);
				spdk_nvmf_request_zcopy_start(&tcp_req->req);
				break;
			}

			if (!tcp_req->req.data) {
				SPDK_DEBUGLOG(nvmf_tcp, "No buffer allocated for tcp_req(%p) on tqpair(%p\n)",
					      tcp_req, tqpair);
				/* No buffers available. */
				break;
			}

			STAILQ_REMOVE(&group->pending_buf_queue, &tcp_req->req, spdk_nvmf_request, buf_link);

			/* If data is transferring from host to controller, we need to do a transfer from the host. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				if (tcp_req->req.data_from_pool) {
					SPDK_DEBUGLOG(nvmf_tcp, "Sending R2T for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
					nvmf_tcp_send_r2t_pdu(tqpair, tcp_req);
				} else {
					struct nvme_tcp_pdu *pdu;

					nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

					pdu = tqpair->pdu_in_progress;
					SPDK_DEBUGLOG(nvmf_tcp, "Not need to send r2t for tcp_req(%p) on tqpair=%p\n", tcp_req,
						      tqpair);
					/* No need to send r2t, contained in the capsuled data */
					nvme_tcp_pdu_set_data_buf(pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
								  0, tcp_req->req.length);
					nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
				}
				break;
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_START, 0, 0,
					  (uintptr_t)tcp_req, tqpair);
			/* Some external code must kick a request into  TCP_REQUEST_STATE_ZCOPY_START_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_ZCOPY_START_COMPLETED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_ZCOPY_START_COMPLETED, 0, 0,
					  (uintptr_t)tcp_req, tqpair);
			if (spdk_unlikely(spdk_nvme_cpl_is_error(&tcp_req->req.rsp->nvme_cpl))) {
				SPDK_DEBUGLOG(nvmf_tcp, "Zero-copy start failed for tcp_req(%p) on tqpair=%p\n",
					      tcp_req, tqpair);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				break;
			}
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				SPDK_DEBUGLOG(nvmf_tcp, "Sending R2T for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
				nvmf_tcp_send_r2t_pdu(tqpair, tcp_req);
			} else {
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTED);
			}
			break;
		case TCP_REQUEST_STATE_AWAITING_R2T_ACK:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK, 0, 0, (uintptr_t)tcp_req,
					  tqpair);
			/* The R2T completion or the h2c data incoming will kick it out of this state. */
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:

			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)tcp_req, tqpair);
			/* Some external code must kick a request into TCP_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE, 0, 0, (uintptr_t)tcp_req,
					  tqpair);

			if (spdk_unlikely(tcp_req->req.dif_enabled)) {
				assert(tcp_req->req.dif.elba_length >= tcp_req->req.length);
				tcp_req->req.length = tcp_req->req.dif.elba_length;
			}

			if (!spdk_nvmf_request_using_zcopy(&tcp_req->req)) {
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTING);
				spdk_nvmf_request_exec(&tcp_req->req);
			} else {
				/* For zero-copy, only requests with data coming from host to the
				 * controller can end up here. */
				assert(tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT);
				spdk_nvmf_request_zcopy_end(&tcp_req->req, true);
			}
			break;
		case TCP_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTING, 0, 0, (uintptr_t)tcp_req, tqpair);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_COMMIT, 0, 0,
					  (uintptr_t)tcp_req, tqpair);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTED, 0, 0, (uintptr_t)tcp_req, tqpair);

			if (spdk_unlikely(tcp_req->req.dif_enabled)) {
				tcp_req->req.length = tcp_req->req.dif.orig_length;
			}

			nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
			break;
		case TCP_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE, 0, 0, (uintptr_t)tcp_req,
					  tqpair);
			rc = request_transfer_out(&tcp_req->req);
			assert(rc == 0); /* No good way to handle this currently */
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, 0, 0,
					  (uintptr_t)tcp_req, tqpair);
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_RELEASE, 0, 0,
					  (uintptr_t)tcp_req, tqpair);
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_COMPLETED, 0, 0, (uintptr_t)tcp_req, tqpair);
			/* If there's an outstanding PDU sent to the host, the request is completed
			 * due to the qpair being disconnected.  We must delay the completion until
			 * that write is done to avoid freeing the request twice. */
			if (spdk_unlikely(tcp_req->pdu_in_use)) {
				SPDK_DEBUGLOG(nvmf_tcp, "Delaying completion due to outstanding "
					      "write on req=%p\n", tcp_req);
				/* This can only happen for zcopy requests */
				assert(spdk_nvmf_request_using_zcopy(&tcp_req->req));
				assert(tqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE);
				break;
			}

			if (tcp_req->req.data_from_pool) {
				spdk_nvmf_request_free_buffers(&tcp_req->req, group, transport);
			} else if (spdk_unlikely(tcp_req->has_in_capsule_data &&
						 (tcp_req->cmd.opc == SPDK_NVME_OPC_FABRIC ||
						  tqpair->qpair.qid == 0) && tcp_req->req.length > transport->opts.in_capsule_data_size)) {
				tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
				assert(tgroup->control_msg_list);
				SPDK_DEBUGLOG(nvmf_tcp, "Put buf to control msg list\n");
				nvmf_tcp_control_msg_put(tgroup->control_msg_list, tcp_req->req.data);
			} else if (tcp_req->req.zcopy_bdev_io != NULL) {
				/* If the request has an unreleased zcopy bdev_io, it's either a
				 * read or a failed write */
				assert(spdk_nvmf_request_using_zcopy(&tcp_req->req));
				assert(tcp_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST ||
				       spdk_nvme_cpl_is_error(&tcp_req->req.rsp->nvme_cpl));
				nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE);
				spdk_nvmf_request_zcopy_end(&tcp_req->req, false);
				break;
			}
			tcp_req->req.length = 0;
			tcp_req->req.iovcnt = 0;
			tcp_req->req.data = NULL;

			nvmf_tcp_req_put(tqpair, tcp_req);
			break;
		case TCP_REQUEST_NUM_STATES:
		default:
			assert(0);
			break;
		}

		if (tcp_req->state != prev_state) {
			progress = true;
		}
	} while (tcp_req->state != prev_state);

	return progress;
}

static void
nvmf_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;
	int rc;

	assert(tqpair != NULL);
	rc = nvmf_tcp_sock_process(tqpair);

	/* If there was a new socket error, disconnect */
	if (rc < 0) {
		nvmf_tcp_qpair_disconnect(tqpair);
	}
}

static int
nvmf_tcp_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	rc =  nvmf_tcp_qpair_sock_init(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Cannot set sock opt for tqpair=%p\n", tqpair);
		return -1;
	}

	rc = nvmf_tcp_qpair_init(&tqpair->qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init tqpair=%p\n", tqpair);
		return -1;
	}

	rc = nvmf_tcp_qpair_init_mem_resource(tqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init memory resource info for tqpair=%p\n", tqpair);
		return -1;
	}

	rc = spdk_sock_group_add_sock(tgroup->sock_group, tqpair->sock,
				      nvmf_tcp_sock_cb, tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Could not add sock to sock_group: %s (%d)\n",
			    spdk_strerror(errno), errno);
		return -1;
	}

	tqpair->group = tgroup;
	nvmf_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_INVALID);
	TAILQ_INSERT_TAIL(&tgroup->qpairs, tqpair, link);

	return 0;
}

static int
nvmf_tcp_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			   struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair		*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair->group == tgroup);

	SPDK_DEBUGLOG(nvmf_tcp, "remove tqpair=%p from the tgroup=%p\n", tqpair, tgroup);
	if (tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_REQ) {
		TAILQ_REMOVE(&tgroup->await_req, tqpair, link);
	} else {
		TAILQ_REMOVE(&tgroup->qpairs, tqpair, link);
	}

	rc = spdk_sock_group_remove_sock(tgroup->sock_group, tqpair->sock);
	if (rc != 0) {
		SPDK_ERRLOG("Could not remove sock from sock_group: %s (%d)\n",
			    spdk_strerror(errno), errno);
	}

	return rc;
}

static int
nvmf_tcp_req_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_req *tcp_req;

	ttransport = SPDK_CONTAINEROF(req->qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	switch (tcp_req->state) {
	case TCP_REQUEST_STATE_EXECUTING:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTED);
		break;
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_ZCOPY_START_COMPLETED);
		break;
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_RELEASE:
		nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
		break;
	default:
		assert(0 && "Unexpected request state");
		break;
	}

	nvmf_tcp_req_process(ttransport, tcp_req);

	return 0;
}

static void
nvmf_tcp_close_qpair(struct spdk_nvmf_qpair *qpair,
		     spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	SPDK_DEBUGLOG(nvmf_tcp, "Qpair: %p\n", qpair);

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair->fini_cb_fn == NULL);
	tqpair->fini_cb_fn = cb_fn;
	tqpair->fini_cb_arg = cb_arg;

	nvmf_tcp_qpair_set_state(tqpair, NVME_TCP_QPAIR_STATE_EXITED);
	nvmf_tcp_qpair_destroy(tqpair);
}

static int
nvmf_tcp_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;
	int rc;
	struct spdk_nvmf_request *req, *req_tmp;
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvmf_tcp_qpair *tqpair, *tqpair_tmp;
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(group->transport,
			struct spdk_nvmf_tcp_transport, transport);

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);

	if (spdk_unlikely(TAILQ_EMPTY(&tgroup->qpairs) && TAILQ_EMPTY(&tgroup->await_req))) {
		return 0;
	}

	STAILQ_FOREACH_SAFE(req, &group->pending_buf_queue, buf_link, req_tmp) {
		tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);
		if (nvmf_tcp_req_process(ttransport, tcp_req) == false) {
			break;
		}
	}

	rc = spdk_sock_group_poll(tgroup->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", tgroup->sock_group);
	}

	TAILQ_FOREACH_SAFE(tqpair, &tgroup->await_req, link, tqpair_tmp) {
		nvmf_tcp_sock_process(tqpair);
	}

	return rc;
}

static int
nvmf_tcp_qpair_get_trid(struct spdk_nvmf_qpair *qpair,
			struct spdk_nvme_transport_id *trid, bool peer)
{
	struct spdk_nvmf_tcp_qpair     *tqpair;
	uint16_t			port;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	spdk_nvme_trid_populate_transport(trid, SPDK_NVME_TRANSPORT_TCP);

	if (peer) {
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", tqpair->initiator_addr);
		port = tqpair->initiator_port;
	} else {
		snprintf(trid->traddr, sizeof(trid->traddr), "%s", tqpair->target_addr);
		port = tqpair->target_port;
	}

	if (spdk_sock_is_ipv4(tqpair->sock)) {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
	} else if (spdk_sock_is_ipv6(tqpair->sock)) {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;
	} else {
		return -1;
	}

	snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%d", port);
	return 0;
}

static int
nvmf_tcp_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	return nvmf_tcp_qpair_get_trid(qpair, trid, 0);
}

static int
nvmf_tcp_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvme_transport_id *trid)
{
	return nvmf_tcp_qpair_get_trid(qpair, trid, 1);
}

static int
nvmf_tcp_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	return nvmf_tcp_qpair_get_trid(qpair, trid, 0);
}

static void
nvmf_tcp_req_set_abort_status(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_tcp_req *tcp_req_to_abort)
{
	tcp_req_to_abort->req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	tcp_req_to_abort->req.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
	tcp_req_to_abort->req.rsp->nvme_cpl.cid = tcp_req_to_abort->req.cmd->nvme_cmd.cid;

	nvmf_tcp_req_set_state(tcp_req_to_abort, TCP_REQUEST_STATE_READY_TO_COMPLETE);

	req->rsp->nvme_cpl.cdw0 &= ~1U; /* Command was successfully aborted. */
}

static int
_nvmf_tcp_qpair_abort_request(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_tcp_req *tcp_req_to_abort = SPDK_CONTAINEROF(req->req_to_abort,
			struct spdk_nvmf_tcp_req, req);
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(req->req_to_abort->qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);
	struct spdk_nvmf_tcp_transport *ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport,
			struct spdk_nvmf_tcp_transport, transport);
	int rc;

	spdk_poller_unregister(&req->poller);

	switch (tcp_req_to_abort->state) {
	case TCP_REQUEST_STATE_EXECUTING:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_START:
	case TCP_REQUEST_STATE_AWAITING_ZCOPY_COMMIT:
		rc = nvmf_ctrlr_abort_request(req);
		if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS) {
			return SPDK_POLLER_BUSY;
		}
		break;

	case TCP_REQUEST_STATE_NEED_BUFFER:
		STAILQ_REMOVE(&tqpair->group->group.pending_buf_queue,
			      &tcp_req_to_abort->req, spdk_nvmf_request, buf_link);

		nvmf_tcp_req_set_abort_status(req, tcp_req_to_abort);
		nvmf_tcp_req_process(ttransport, tcp_req_to_abort);
		break;

	case TCP_REQUEST_STATE_AWAITING_R2T_ACK:
	case TCP_REQUEST_STATE_ZCOPY_START_COMPLETED:
		nvmf_tcp_req_set_abort_status(req, tcp_req_to_abort);
		break;

	case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
		if (spdk_get_ticks() < req->timeout_tsc) {
			req->poller = SPDK_POLLER_REGISTER(_nvmf_tcp_qpair_abort_request, req, 0);
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
nvmf_tcp_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_qpair *tqpair;
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_transport *transport;
	uint16_t cid;
	uint32_t i;
	struct spdk_nvmf_tcp_req *tcp_req_to_abort = NULL;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	transport = &ttransport->transport;

	cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;

	for (i = 0; i < tqpair->resource_count; i++) {
		if (tqpair->reqs[i].state != TCP_REQUEST_STATE_FREE &&
		    tqpair->reqs[i].req.cmd->nvme_cmd.cid == cid) {
			tcp_req_to_abort = &tqpair->reqs[i];
			break;
		}
	}

	spdk_trace_record(TRACE_TCP_QP_ABORT_REQ, 0, 0, (uintptr_t)req, tqpair);

	if (tcp_req_to_abort == NULL) {
		spdk_nvmf_request_complete(req);
		return;
	}

	req->req_to_abort = &tcp_req_to_abort->req;
	req->timeout_tsc = spdk_get_ticks() +
			   transport->opts.abort_timeout_sec * spdk_get_ticks_hz();
	req->poller = NULL;

	_nvmf_tcp_qpair_abort_request(req);
}

#define SPDK_NVMF_TCP_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_TCP_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR 128
#define SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE 131072
#define SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS 511
#define SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE 32
#define SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_TCP_DEFAULT_ABORT_TIMEOUT_SEC 1

static void
nvmf_tcp_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_TCP_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_TCP_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_TCP_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->transport_specific =      NULL;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp = {
	.name = "TCP",
	.type = SPDK_NVME_TRANSPORT_TCP,
	.opts_init = nvmf_tcp_opts_init,
	.create = nvmf_tcp_create,
	.dump_opts = nvmf_tcp_dump_opts,
	.destroy = nvmf_tcp_destroy,

	.listen = nvmf_tcp_listen,
	.stop_listen = nvmf_tcp_stop_listen,

	.listener_discover = nvmf_tcp_discover,

	.poll_group_create = nvmf_tcp_poll_group_create,
	.get_optimal_poll_group = nvmf_tcp_get_optimal_poll_group,
	.poll_group_destroy = nvmf_tcp_poll_group_destroy,
	.poll_group_add = nvmf_tcp_poll_group_add,
	.poll_group_remove = nvmf_tcp_poll_group_remove,
	.poll_group_poll = nvmf_tcp_poll_group_poll,

	.req_free = nvmf_tcp_req_free,
	.req_complete = nvmf_tcp_req_complete,

	.qpair_fini = nvmf_tcp_close_qpair,
	.qpair_get_local_trid = nvmf_tcp_qpair_get_local_trid,
	.qpair_get_peer_trid = nvmf_tcp_qpair_get_peer_trid,
	.qpair_get_listen_trid = nvmf_tcp_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_tcp_qpair_abort_request,
};

SPDK_NVMF_TRANSPORT_REGISTER(tcp, &spdk_nvmf_transport_tcp);
SPDK_LOG_REGISTER_COMPONENT(nvmf_tcp)
