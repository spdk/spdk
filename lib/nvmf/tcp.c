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
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/nvmf_transport.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/assert.h"
#include "spdk_internal/log.h"
#include "spdk_internal/nvme_tcp.h"

#define NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME 16
#define SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY 6
#define SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR 4

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp;

/* spdk nvmf related structure */
enum spdk_nvmf_tcp_req_state {

	/* The request is not currently in use */
	TCP_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	TCP_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	TCP_REQUEST_STATE_NEED_BUFFER,

	/* The request is currently transferring data from the host to the controller. */
	TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is waiting for the R2T send acknowledgement. */
	TCP_REQUEST_STATE_AWAITING_R2T_ACK,

	/* The request is ready to execute at the block device */
	TCP_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	TCP_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	TCP_REQUEST_STATE_EXECUTED,

	/* The request is ready to send a completion */
	TCP_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request is currently transferring final pdus from the controller to the host. */
	TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,

	/* The request completed and can be marked free. */
	TCP_REQUEST_STATE_COMPLETED,

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

#define OBJECT_NVMF_TCP_IO				0x80

#define TRACE_GROUP_NVMF_TCP				0x5
#define TRACE_TCP_REQUEST_STATE_NEW					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0)
#define TRACE_TCP_REQUEST_STATE_NEED_BUFFER				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x1)
#define TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x2)
#define TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x3)
#define TRACE_TCP_REQUEST_STATE_EXECUTING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x4)
#define TRACE_TCP_REQUEST_STATE_EXECUTED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x5)
#define TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x6)
#define TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x7)
#define TRACE_TCP_REQUEST_STATE_COMPLETED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x8)
#define TRACE_TCP_FLUSH_WRITEBUF_START					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x9)
#define TRACE_TCP_FLUSH_WRITEBUF_DONE					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0xA)
#define TRACE_TCP_READ_FROM_SOCKET_DONE					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0xB)
#define TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0xC)

SPDK_TRACE_REGISTER_FN(nvmf_tcp_trace, "nvmf_tcp", TRACE_GROUP_NVMF_TCP)
{
	spdk_trace_register_object(OBJECT_NVMF_TCP_IO, 'r');
	spdk_trace_register_description("TCP_REQ_NEW",
					TRACE_TCP_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 1, 1, "");
	spdk_trace_register_description("TCP_REQ_NEED_BUFFER",
					TRACE_TCP_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_REQ_TX_H_TO_C",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_EXECUTE",
					TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_REQ_EXECUTING",
					TRACE_TCP_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_REQ_EXECUTED",
					TRACE_TCP_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_COMPLETE",
					TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_REQ_TRANSFER_C2H",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_REQ_COMPLETED",
					TRACE_TCP_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
	spdk_trace_register_description("TCP_WRITE_START",
					TRACE_TCP_FLUSH_WRITEBUF_START,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("TCP_WRITE_DONE",
					TRACE_TCP_FLUSH_WRITEBUF_DONE,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("TCP_READ_DONE",
					TRACE_TCP_READ_FROM_SOCKET_DONE,
					OWNER_NONE, OBJECT_NONE, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_AWAIT_R2T_ACK",
					TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 1, "");
}

struct spdk_nvmf_tcp_req  {
	struct spdk_nvmf_request		req;
	struct spdk_nvme_cpl			rsp;
	struct spdk_nvme_cmd			cmd;

	/* A PDU that can be used for sending responses. This is
	 * not the incoming PDU! */
	struct nvme_tcp_pdu			*pdu;

	/*
	 * The PDU for a request may be used multiple times in serial over
	 * the request's lifetime. For example, first to send an R2T, then
	 * to send a completion. To catch mistakes where the PDU is used
	 * twice at the same time, add a debug flag here for init/fini.
	 */
	bool					pdu_in_use;

	/* In-capsule data buffer */
	uint8_t					*buf;

	bool					has_incapsule_data;

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

struct nvme_tcp_pdu_recv_buf {
	char					*buf;
	uint32_t				off;
	uint32_t				size;
	uint32_t				remain_size;
};

struct spdk_nvmf_tcp_qpair {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_tcp_poll_group		*group;
	struct spdk_nvmf_tcp_port		*port;
	struct spdk_sock			*sock;

	enum nvme_tcp_pdu_recv_state		recv_state;
	enum nvme_tcp_qpair_state		state;

	/* PDU being actively received */
	struct nvme_tcp_pdu			pdu_in_progress;
	struct nvme_tcp_pdu_recv_buf		pdu_recv_buf;

	/* This is a spare PDU used for sending special management
	 * operations. Primarily, this is used for the initial
	 * connection response and c2h termination request. */
	struct nvme_tcp_pdu			mgmt_pdu;

	TAILQ_HEAD(, nvme_tcp_pdu)		send_queue;

	/* Arrays of in-capsule buffers, requests, and pdus.
	 * Each array is 'resource_count' number of elements */
	void					*bufs;
	struct spdk_nvmf_tcp_req		*reqs;
	struct nvme_tcp_pdu			*pdus;
	uint32_t				resource_count;

	/* Queues to track the requests in all states */
	TAILQ_HEAD(, spdk_nvmf_tcp_req)		state_queue[TCP_REQUEST_NUM_STATES];
	/* Number of requests in each state */
	uint32_t				state_cntr[TCP_REQUEST_NUM_STATES];

	uint8_t					cpda;

	bool					host_hdgst_enable;
	bool					host_ddgst_enable;

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

	TAILQ_ENTRY(spdk_nvmf_tcp_qpair)	link;
};

struct spdk_nvmf_tcp_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	struct spdk_sock_group			*sock_group;

	TAILQ_HEAD(, spdk_nvmf_tcp_qpair)	qpairs;
	TAILQ_HEAD(, spdk_nvmf_tcp_qpair)	await_req;
};

struct spdk_nvmf_tcp_port {
	struct spdk_nvme_transport_id		trid;
	struct spdk_sock			*listen_sock;
	uint32_t				ref;
	TAILQ_ENTRY(spdk_nvmf_tcp_port)		link;
};

struct spdk_nvmf_tcp_transport {
	struct spdk_nvmf_transport		transport;

	pthread_mutex_t				lock;

	TAILQ_HEAD(, spdk_nvmf_tcp_port)	ports;
};

static bool spdk_nvmf_tcp_req_process(struct spdk_nvmf_tcp_transport *ttransport,
				      struct spdk_nvmf_tcp_req *tcp_req);

static void
spdk_nvmf_tcp_req_set_state(struct spdk_nvmf_tcp_req *tcp_req,
			    enum spdk_nvmf_tcp_req_state state)
{
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_tcp_qpair *tqpair;

	qpair = tcp_req->req.qpair;
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	TAILQ_REMOVE(&tqpair->state_queue[tcp_req->state], tcp_req, state_link);
	assert(tqpair->state_cntr[tcp_req->state] > 0);
	tqpair->state_cntr[tcp_req->state]--;

	TAILQ_INSERT_TAIL(&tqpair->state_queue[state], tcp_req, state_link);
	tqpair->state_cntr[state]++;

	tcp_req->state = state;
}

static inline struct nvme_tcp_pdu *
nvmf_tcp_req_pdu_init(struct spdk_nvmf_tcp_req *tcp_req)
{
	assert(tcp_req->pdu_in_use == false);
	tcp_req->pdu_in_use = true;

	memset(tcp_req->pdu, 0, sizeof(*tcp_req->pdu));
	tcp_req->pdu->qpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	tcp_req->pdu->hdr = &tcp_req->pdu->hdr_mem;

	return tcp_req->pdu;
}

static inline void
nvmf_tcp_req_pdu_fini(struct spdk_nvmf_tcp_req *tcp_req)
{
	tcp_req->pdu_in_use = false;
}

static struct spdk_nvmf_tcp_req *
spdk_nvmf_tcp_req_get(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_req *tcp_req;

	tcp_req = TAILQ_FIRST(&tqpair->state_queue[TCP_REQUEST_STATE_FREE]);
	if (!tcp_req) {
		return NULL;
	}

	memset(&tcp_req->rsp, 0, sizeof(tcp_req->rsp));
	tcp_req->h2c_offset = 0;
	tcp_req->has_incapsule_data = false;
	tcp_req->req.dif.dif_insert_or_strip = false;

	spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_NEW);
	return tcp_req;
}

static void
nvmf_tcp_request_free(struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_transport *ttransport;

	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tcp_req=%p will be freed\n", tcp_req);
	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);
	spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
	spdk_nvmf_tcp_req_process(ttransport, tcp_req);
}

static int
spdk_nvmf_tcp_req_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req *tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	nvmf_tcp_request_free(tcp_req);

	return 0;
}

static void
spdk_nvmf_tcp_drain_state_queue(struct spdk_nvmf_tcp_qpair *tqpair,
				enum spdk_nvmf_tcp_req_state state)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->state_queue[state], state_link, req_tmp) {
		nvmf_tcp_request_free(tcp_req);
	}
}

static void
spdk_nvmf_tcp_cleanup_all_states(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_req *tcp_req, *req_tmp;

	assert(TAILQ_EMPTY(&tqpair->send_queue));

	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEW);

	/* Wipe the requests waiting for buffer from the global list */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->state_queue[TCP_REQUEST_STATE_NEED_BUFFER], state_link,
			   req_tmp) {
		STAILQ_REMOVE(&tqpair->group->group.pending_buf_queue, &tcp_req->req,
			      spdk_nvmf_request, buf_link);
	}

	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEED_BUFFER);
	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_EXECUTING);
	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_AWAITING_R2T_ACK);
}

static void
nvmf_tcp_dump_qpair_req_contents(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int i;
	struct spdk_nvmf_tcp_req *tcp_req;

	SPDK_ERRLOG("Dumping contents of queue pair (QID %d)\n", tqpair->qpair.qid);
	for (i = 1; i < TCP_REQUEST_NUM_STATES; i++) {
		SPDK_ERRLOG("\tNum of requests in state[%d] = %u\n", i, tqpair->state_cntr[i]);
		TAILQ_FOREACH(tcp_req, &tqpair->state_queue[i], state_link) {
			SPDK_ERRLOG("\t\tRequest Data From Pool: %d\n", tcp_req->req.data_from_pool);
			SPDK_ERRLOG("\t\tRequest opcode: %d\n", tcp_req->req.cmd->nvmf_cmd.opcode);
		}
	}
}

static void
spdk_nvmf_tcp_qpair_destroy(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int err = 0;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

	err = spdk_sock_close(&tqpair->sock);
	assert(err == 0);
	spdk_nvmf_tcp_cleanup_all_states(tqpair);

	if (tqpair->state_cntr[TCP_REQUEST_STATE_FREE] != tqpair->resource_count) {
		SPDK_ERRLOG("tqpair(%p) free tcp request num is %u but should be %u\n", tqpair,
			    tqpair->state_cntr[TCP_REQUEST_STATE_FREE],
			    tqpair->resource_count);
		err++;
	}

	if (err > 0) {
		nvmf_tcp_dump_qpair_req_contents(tqpair);
	}

	spdk_dma_free(tqpair->pdus);
	free(tqpair->reqs);
	spdk_free(tqpair->bufs);
	free(tqpair->pdu_recv_buf.buf);
	free(tqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Leave\n");
}

static int
spdk_nvmf_tcp_destroy(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_tcp_transport	*ttransport;

	assert(transport != NULL);
	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	pthread_mutex_destroy(&ttransport->lock);
	free(ttransport);
	return 0;
}

static struct spdk_nvmf_transport *
spdk_nvmf_tcp_create(struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	uint32_t sge_count;
	uint32_t min_shared_buffers;

	ttransport = calloc(1, sizeof(*ttransport));
	if (!ttransport) {
		return NULL;
	}

	TAILQ_INIT(&ttransport->ports);

	ttransport->transport.ops = &spdk_nvmf_transport_tcp;

	SPDK_NOTICELOG("*** TCP Transport Init ***\n");

	SPDK_INFOLOG(SPDK_LOG_NVMF_TCP, "*** TCP Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d\n"
		     "  num_shared_buffers=%d, c2h_success=%d,\n"
		     "  dif_insert_or_strip=%d, sock_priority=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     opts->c2h_success,
		     opts->dif_insert_or_strip,
		     opts->sock_priority);

	if (opts->sock_priority > SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY) {
		SPDK_ERRLOG("Unsupported socket_priority=%d, the current range is: 0 to %d\n"
			    "you can use man 7 socket to view the range of priority under SO_PRIORITY item\n",
			    opts->sock_priority, SPDK_NVMF_TCP_DEFAULT_MAX_SOCK_PRIORITY);
		free(ttransport);
		return NULL;
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

	min_shared_buffers = spdk_thread_get_count() * opts->buf_cache_size;
	if (min_shared_buffers > opts->num_shared_buffers) {
		SPDK_ERRLOG("There are not enough buffers to satisfy"
			    "per-poll group caches for each thread. (%" PRIu32 ")"
			    "supplied. (%" PRIu32 ") required\n", opts->num_shared_buffers, min_shared_buffers);
		SPDK_ERRLOG("Please specify a larger number of shared buffers\n");
		spdk_nvmf_tcp_destroy(&ttransport->transport);
		return NULL;
	}

	pthread_mutex_init(&ttransport->lock, NULL);

	return &ttransport->transport;
}

static int
_spdk_nvmf_tcp_trsvcid_to_int(const char *trsvcid)
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
_spdk_nvmf_tcp_canon_listen_trid(struct spdk_nvme_transport_id *canon_trid,
				 const struct spdk_nvme_transport_id *trid)
{
	int trsvcid_int;

	trsvcid_int = _spdk_nvmf_tcp_trsvcid_to_int(trid->trsvcid);
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
_spdk_nvmf_tcp_find_port(struct spdk_nvmf_tcp_transport *ttransport,
			 const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvme_transport_id canon_trid;
	struct spdk_nvmf_tcp_port *port;

	if (_spdk_nvmf_tcp_canon_listen_trid(&canon_trid, trid) != 0) {
		return NULL;
	}

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		if (spdk_nvme_transport_id_compare(&canon_trid, &port->trid) == 0) {
			return port;
		}
	}

	return NULL;
}

static int
spdk_nvmf_tcp_listen(struct spdk_nvmf_transport *transport,
		     const struct spdk_nvme_transport_id *trid,
		     spdk_nvmf_tgt_listen_done_fn cb_fn,
		     void *cb_arg)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	int trsvcid_int;
	uint8_t adrfam;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	trsvcid_int = _spdk_nvmf_tcp_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		SPDK_ERRLOG("Invalid trsvcid '%s'\n", trid->trsvcid);
		return -EINVAL;
	}

	pthread_mutex_lock(&ttransport->lock);
	port = _spdk_nvmf_tcp_find_port(ttransport, trid);
	if (port) {
		goto success;
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	if (_spdk_nvmf_tcp_canon_listen_trid(&port->trid, trid) != 0) {
		SPDK_ERRLOG("Invalid traddr %s / trsvcid %s\n",
			    trid->traddr, trid->trsvcid);
		free(port);
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	port->listen_sock = spdk_sock_listen(trid->traddr, trsvcid_int, NULL);
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

success:
	port->ref++;
	pthread_mutex_unlock(&ttransport->lock);
	cb_fn(cb_arg, 0);
	return 0;
}

static int
spdk_nvmf_tcp_stop_listen(struct spdk_nvmf_transport *transport,
			  const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;
	int rc;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Removing listen address %s port %s\n",
		      trid->traddr, trid->trsvcid);

	pthread_mutex_lock(&ttransport->lock);
	port = _spdk_nvmf_tcp_find_port(ttransport, trid);
	if (port) {
		assert(port->ref > 0);
		port->ref--;
		if (port->ref == 0) {
			TAILQ_REMOVE(&ttransport->ports, port, link);
			spdk_sock_close(&port->listen_sock);
			free(port);
		}
		rc = 0;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Port not found\n");
		rc = -ENOENT;
	}
	pthread_mutex_unlock(&ttransport->lock);

	return rc;
}

static void spdk_nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
		enum nvme_tcp_pdu_recv_state state);

static void
spdk_nvmf_tcp_qpair_disconnect(struct spdk_nvmf_tcp_qpair *tqpair)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Disconnecting qpair %p\n", tqpair);

	if (tqpair->state <= NVME_TCP_QPAIR_STATE_RUNNING) {
		tqpair->state = NVME_TCP_QPAIR_STATE_EXITING;
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
		spdk_poller_unregister(&tqpair->timeout_poller);

		/* This will end up calling spdk_nvmf_tcp_close_qpair */
		spdk_nvmf_qpair_disconnect(&tqpair->qpair, NULL, NULL);
	}
}

static void
_pdu_write_done(void *_pdu, int err)
{
	struct nvme_tcp_pdu			*pdu = _pdu;
	struct spdk_nvmf_tcp_qpair		*tqpair = pdu->qpair;

	TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);

	if (err != 0) {
		spdk_nvmf_tcp_qpair_disconnect(tqpair);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(pdu->cb_arg);
}

static void
spdk_nvmf_tcp_qpair_write_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			      struct nvme_tcp_pdu *pdu,
			      nvme_tcp_qpair_xfer_complete_cb cb_fn,
			      void *cb_arg)
{
	int enable_digest;
	int hlen;
	uint32_t crc32c;
	uint32_t mapped_length = 0;
	ssize_t rc;

	assert(&tqpair->pdu_in_progress != pdu);

	hlen = pdu->hdr->common.hlen;
	enable_digest = 1;
	if (pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	/* Header Digest */
	if (enable_digest && tqpair->host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr->raw + hlen, crc32c);
	}

	/* Data Digest */
	if (pdu->data_len > 0 && enable_digest && tqpair->host_ddgst_enable) {
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
	}

	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;

	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       tqpair->host_hdgst_enable, tqpair->host_ddgst_enable,
			       &mapped_length);
	pdu->sock_req.cb_fn = _pdu_write_done;
	pdu->sock_req.cb_arg = pdu;
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	if (pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ) {
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

static int
spdk_nvmf_tcp_qpair_init_mem_resource(struct spdk_nvmf_tcp_qpair *tqpair)
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

	tqpair->mgmt_pdu.qpair = tqpair;
	tqpair->mgmt_pdu.hdr = &tqpair->mgmt_pdu.hdr_mem;

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

	tqpair->pdus = spdk_dma_malloc(tqpair->resource_count * sizeof(*tqpair->pdus), 0x1000, NULL);
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
		tcp_req->pdu->hdr = &tcp_req->pdu->hdr_mem;

		/* Set up memory to receive commands */
		if (tqpair->bufs) {
			tcp_req->buf = (void *)((uintptr_t)tqpair->bufs + (i * in_capsule_data_size));
		}

		/* Set the cmdn and rsp */
		tcp_req->req.rsp = (union nvmf_c2h_msg *)&tcp_req->rsp;
		tcp_req->req.cmd = (union nvmf_h2c_msg *)&tcp_req->cmd;

		/* Initialize request state to FREE */
		tcp_req->state = TCP_REQUEST_STATE_FREE;
		TAILQ_INSERT_TAIL(&tqpair->state_queue[tcp_req->state], tcp_req, state_link);
		tqpair->state_cntr[TCP_REQUEST_STATE_FREE]++;
	}

	tqpair->pdu_recv_buf.size = (in_capsule_data_size + sizeof(struct spdk_nvme_tcp_cmd) + 2 *
				     SPDK_NVME_TCP_DIGEST_LEN) * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	tqpair->pdu_recv_buf.buf = calloc(1, tqpair->pdu_recv_buf.size);
	if (!tqpair->pdu_recv_buf.buf) {
		SPDK_ERRLOG("Unable to allocate the pdu recv buf on tqpair=%p with size=%d\n", tqpair,
			    tqpair->pdu_recv_buf.size);
		return -1;
	}
	tqpair->pdu_in_progress.hdr = (union nvme_tcp_pdu_hdr *)tqpair->pdu_recv_buf.buf;

	return 0;
}

static int
spdk_nvmf_tcp_qpair_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair;
	int i;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New TCP Connection: %p\n", qpair);

	TAILQ_INIT(&tqpair->send_queue);

	/* Initialise request state queues of the qpair */
	for (i = TCP_REQUEST_STATE_FREE; i < TCP_REQUEST_NUM_STATES; i++) {
		TAILQ_INIT(&tqpair->state_queue[i]);
	}

	tqpair->host_hdgst_enable = true;
	tqpair->host_ddgst_enable = true;
	return 0;
}

static int
spdk_nvmf_tcp_qpair_sock_init(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int rc;

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(tqpair->sock, sizeof(struct spdk_nvme_tcp_c2h_data_hdr));
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed\n");
		return rc;
	}

	return 0;
}

static void
_spdk_nvmf_tcp_handle_connect(struct spdk_nvmf_transport *transport,
			      struct spdk_nvmf_tcp_port *port,
			      struct spdk_sock *sock,
			      new_qpair_fn cb_fn, void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New connection accepted on %s port %s\n",
		      port->trid.traddr, port->trid.trsvcid);

	if (transport->opts.sock_priority) {
		rc = spdk_sock_set_priority(sock, transport->opts.sock_priority);
		if (rc) {
			SPDK_ERRLOG("Failed to set the priority of the socket\n");
			spdk_sock_close(&sock);
			return;
		}
	}

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
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return;
	}

	cb_fn(&tqpair->qpair, cb_arg);
}

static void
spdk_nvmf_tcp_port_accept(struct spdk_nvmf_transport *transport, struct spdk_nvmf_tcp_port *port,
			  new_qpair_fn cb_fn, void *cb_arg)
{
	struct spdk_sock *sock;
	int i;

	for (i = 0; i < NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(port->listen_sock);
		if (sock == NULL) {
			break;
		}
		_spdk_nvmf_tcp_handle_connect(transport, port, sock, cb_fn, cb_arg);
	}
}

static void
spdk_nvmf_tcp_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn, void *cb_arg)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		spdk_nvmf_tcp_port_accept(transport, port, cb_fn, cb_arg);
	}
}

static void
spdk_nvmf_tcp_discover(struct spdk_nvmf_transport *transport,
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

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_tcp_poll_group_create(struct spdk_nvmf_transport *transport)
{
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

	return &tgroup->group;

cleanup:
	free(tgroup);
	return NULL;
}

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_tcp_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair;
	struct spdk_sock_group *group = NULL;
	int rc;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	rc = spdk_sock_get_optimal_sock_group(tqpair->sock, &group);
	if (!rc && group != NULL) {
		return spdk_sock_group_get_ctx(group);
	}

	return NULL;
}

static void
spdk_nvmf_tcp_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	spdk_sock_group_close(&tgroup->sock_group);

	free(tgroup);
}

static inline void
spdk_nvmf_tcp_reset_pdu_in_process(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu_recv_buf *pdu_recv_buf = &tqpair->pdu_recv_buf;
	char *dst, *src;

	if (spdk_unlikely((pdu_recv_buf->off + sizeof(union nvme_tcp_pdu_hdr)) >
			  pdu_recv_buf->size)) {
		if (pdu_recv_buf->remain_size) {
			dst = pdu_recv_buf->buf;
			src = (char *)((void *)pdu_recv_buf->buf + pdu_recv_buf->off);

			/* purpose: to avoid overlap copy, so do not use memcpy if there is overlap case */
			memmove(dst, src, pdu_recv_buf->remain_size);
		}
		tqpair->pdu_recv_buf.off = 0;
	} else if (!pdu_recv_buf->remain_size) {
		tqpair->pdu_recv_buf.off = 0;
	}

	tqpair->pdu_in_progress.hdr = (union nvme_tcp_pdu_hdr *)((void *)pdu_recv_buf->buf +
				      pdu_recv_buf->off);
}

static void
spdk_nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tqpair(%p) recv state=%d\n", tqpair, state);
	tqpair->recv_state = state;

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
		memset(&tqpair->pdu_in_progress, 0, sizeof(tqpair->pdu_in_progress));
		spdk_nvmf_tcp_reset_pdu_in_process(tqpair);
		break;
	default:
		SPDK_ERRLOG("The state(%d) is invalid\n", state);
		abort();
		break;
	}
}

static int
spdk_nvmf_tcp_qpair_handle_timeout(void *ctx)
{
	struct spdk_nvmf_tcp_qpair *tqpair = ctx;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_ERROR);

	SPDK_ERRLOG("No pdu coming for tqpair=%p within %d seconds\n", tqpair,
		    SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT);

	spdk_nvmf_tcp_qpair_disconnect(tqpair);
	return 0;
}

static void
spdk_nvmf_tcp_send_c2h_term_req_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = (struct spdk_nvmf_tcp_qpair *)cb_arg;

	if (!tqpair->timeout_poller) {
		tqpair->timeout_poller = spdk_poller_register(spdk_nvmf_tcp_qpair_handle_timeout, tqpair,
					 SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT * 1000000);
	}
}

static void
spdk_nvmf_tcp_send_c2h_term_req(struct spdk_nvmf_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req;
	uint32_t c2h_term_req_hdr_len = sizeof(*c2h_term_req);
	uint32_t copy_len;

	rsp_pdu = &tqpair->mgmt_pdu;

	c2h_term_req = &rsp_pdu->hdr->term_req;
	c2h_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ;
	c2h_term_req->common.hlen = c2h_term_req_hdr_len;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&c2h_term_req->fei, error_offset);
	}

	copy_len = spdk_min(pdu->hdr->common.hlen, SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)rsp_pdu->hdr->raw + c2h_term_req_hdr_len, pdu->hdr->raw, copy_len);
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr->raw + c2h_term_req_hdr_len, copy_len);

	/* Contain the header of the wrong received pdu */
	c2h_term_req->common.plen = c2h_term_req->common.hlen + copy_len;
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_send_c2h_term_req_complete, tqpair);
}

static void
spdk_nvmf_tcp_capsule_cmd_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
				     struct spdk_nvmf_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;

	assert(pdu->psh_valid_bytes == pdu->psh_len);
	assert(pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD);

	tcp_req = spdk_nvmf_tcp_req_get(tqpair);
	if (!tcp_req) {
		/* Directly return and make the allocation retry again */
		if (tqpair->state_cntr[TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST] > 0) {
			return;
		}

		/* The host sent more commands than the maximum queue depth. */
		SPDK_ERRLOG("Cannot allocate tcp_req on tqpair=%p\n", tqpair);
		spdk_nvmf_tcp_qpair_disconnect(tqpair);
		return;
	}

	pdu->req = tcp_req;
	assert(tcp_req->state == TCP_REQUEST_STATE_NEW);
	spdk_nvmf_tcp_req_process(ttransport, tcp_req);
}

static void
spdk_nvmf_tcp_capsule_cmd_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
		struct spdk_nvmf_tcp_qpair *tqpair,
		struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	capsule_cmd = &pdu->hdr->capsule_cmd;
	tcp_req = pdu->req;
	assert(tcp_req != NULL);
	if (capsule_cmd->common.pdo > SPDK_NVME_TCP_PDU_PDO_MAX_OFFSET) {
		SPDK_ERRLOG("Expected ICReq capsule_cmd pdu offset <= %d, got %c\n",
			    SPDK_NVME_TCP_PDU_PDO_MAX_OFFSET, capsule_cmd->common.pdo);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo);
		goto err;
	}

	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
	spdk_nvmf_tcp_req_process(ttransport, tcp_req);

	return;
err:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static int
nvmf_tcp_find_req_in_state(struct spdk_nvmf_tcp_qpair *tqpair,
			   enum spdk_nvmf_tcp_req_state state,
			   uint16_t cid, uint16_t tag,
			   struct spdk_nvmf_tcp_req **req)
{
	struct spdk_nvmf_tcp_req *tcp_req = NULL;

	TAILQ_FOREACH(tcp_req, &tqpair->state_queue[state], state_link) {
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
spdk_nvmf_tcp_h2c_data_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
				  struct spdk_nvmf_tcp_qpair *tqpair,
				  struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes = 0;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	int rc;

	h2c_data = &pdu->hdr->h2c_data;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tqpair=%p, r2t_info: datao=%u, datal=%u, cccid=%u, ttag=%u\n",
		      tqpair, h2c_data->datao, h2c_data->datal, h2c_data->cccid, h2c_data->ttag);

	rc = nvmf_tcp_find_req_in_state(tqpair, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					h2c_data->cccid, h2c_data->ttag, &tcp_req);
	if (rc == 0 && tcp_req == NULL) {
		rc = nvmf_tcp_find_req_in_state(tqpair, TCP_REQUEST_STATE_AWAITING_R2T_ACK, h2c_data->cccid,
						h2c_data->ttag, &tcp_req);
	}

	if (!tcp_req) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tcp_req is not found for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER;
		if (rc == 0) {
			error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, cccid);
		} else {
			error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, ttag);
		}
		goto err;
	}

	if (tcp_req->h2c_offset != h2c_data->datao) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
			      "tcp_req(%p), tqpair=%p, expected data offset %u, but data offset is %u\n",
			      tcp_req, tqpair, tcp_req->h2c_offset, h2c_data->datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	if ((h2c_data->datao + h2c_data->datal) > tcp_req->req.length) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
			      "tcp_req(%p), tqpair=%p,  (datao=%u + datal=%u) execeeds requested length=%u\n",
			      tcp_req, tqpair, h2c_data->datao, h2c_data->datal, tcp_req->req.length);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	pdu->req = tcp_req;

	if (spdk_unlikely(tcp_req->req.dif.dif_insert_or_strip)) {
		pdu->dif_ctx = &tcp_req->req.dif.dif_ctx;
	}

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
				  h2c_data->datao, h2c_data->datal);
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

err:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
spdk_nvmf_tcp_pdu_cmd_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;
	nvmf_tcp_request_free(tcp_req);
}

static void
spdk_nvmf_tcp_send_capsule_resp_pdu(struct spdk_nvmf_tcp_req *tcp_req,
				    struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_rsp *capsule_resp;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter, tqpair=%p\n", tqpair);

	rsp_pdu = nvmf_tcp_req_pdu_init(tcp_req);
	assert(rsp_pdu != NULL);

	capsule_resp = &rsp_pdu->hdr->capsule_resp;
	capsule_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP;
	capsule_resp->common.plen = capsule_resp->common.hlen = sizeof(*capsule_resp);
	capsule_resp->rccqe = tcp_req->req.rsp->nvme_cpl;
	if (tqpair->host_hdgst_enable) {
		capsule_resp->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		capsule_resp->common.plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_pdu_cmd_complete, tcp_req);
}

static void
spdk_nvmf_tcp_pdu_c2h_data_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair != NULL);
	if (tqpair->qpair.transport->opts.c2h_success) {
		nvmf_tcp_request_free(tcp_req);
	} else {
		nvmf_tcp_req_pdu_fini(tcp_req);
		spdk_nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}
}

static void
spdk_nvmf_tcp_r2t_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_req *tcp_req = cb_arg;
	struct spdk_nvmf_tcp_transport *ttransport;

	nvmf_tcp_req_pdu_fini(tcp_req);

	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);

	spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

	if (tcp_req->h2c_offset == tcp_req->req.length) {
		spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		spdk_nvmf_tcp_req_process(ttransport, tcp_req);
	}
}

static void
spdk_nvmf_tcp_send_r2t_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			   struct spdk_nvmf_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_r2t_hdr *r2t;

	rsp_pdu = nvmf_tcp_req_pdu_init(tcp_req);
	assert(rsp_pdu != NULL);

	r2t = &rsp_pdu->hdr->r2t;
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

	spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_AWAITING_R2T_ACK);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
		      "tcp_req(%p) on tqpair(%p), r2t_info: cccid=%u, ttag=%u, r2to=%u, r2tl=%u\n",
		      tcp_req, tqpair, r2t->cccid, r2t->ttag, r2t->r2to, r2t->r2tl);
	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_r2t_complete, tcp_req);
}

static void
spdk_nvmf_tcp_h2c_data_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
				      struct spdk_nvmf_tcp_qpair *tqpair,
				      struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_req *tcp_req;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

	tcp_req->h2c_offset += pdu->data_len;

	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	/* Wait for all of the data to arrive AND for the initial R2T PDU send to be
	 * acknowledged before moving on. */
	if (tcp_req->h2c_offset == tcp_req->req.length &&
	    tcp_req->state == TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER) {
		spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		spdk_nvmf_tcp_req_process(ttransport, tcp_req);
	}
}

static void
spdk_nvmf_tcp_h2c_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *h2c_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", h2c_term_req,
		    spdk_nvmf_tcp_term_req_fes_str[h2c_term_req->fes]);
	if ((h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "The offset from the start of the PDU header is %u\n",
			      DGET32(h2c_term_req->fei));
	}
}

static void
spdk_nvmf_tcp_h2c_term_req_hdr_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				      struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr->term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;


	if (h2c_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		SPDK_ERRLOG("Fatal Error Stauts(FES) is unknown for h2c_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr->raw + h2c_term_req->common.hlen,
			      h2c_term_req->common.plen - h2c_term_req->common.hlen);
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
spdk_nvmf_tcp_h2c_term_req_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
		struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr->term_req;

	spdk_nvmf_tcp_h2c_term_req_dump(h2c_term_req);
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
}

static void
spdk_nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				 struct spdk_nvmf_tcp_transport *ttransport)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	pdu = &tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");
	/* check data digest if need */
	if (pdu->ddgst_enable) {
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("Data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr->common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		spdk_nvmf_tcp_capsule_cmd_payload_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		spdk_nvmf_tcp_h2c_data_payload_handle(ttransport, tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		spdk_nvmf_tcp_h2c_term_req_payload_handle(tqpair, pdu);
		break;

	default:
		/* The code should not go to here */
		SPDK_ERRLOG("The code should not go to here\n");
		break;
	}
}

static void
spdk_nvmf_tcp_send_icresp_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_qpair *tqpair = cb_arg;

	tqpair->state = NVME_TCP_QPAIR_STATE_RUNNING;
}

static void
spdk_nvmf_tcp_icreq_handle(struct spdk_nvmf_tcp_transport *ttransport,
			   struct spdk_nvmf_tcp_qpair *tqpair,
			   struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_req *ic_req = &pdu->hdr->ic_req;
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
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "maxr2t =%u\n", (ic_req->maxr2t + 1u));

	tqpair->host_hdgst_enable = ic_req->dgst.bits.hdgst_enable ? true : false;
	if (!tqpair->host_hdgst_enable) {
		tqpair->pdu_recv_buf.size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	}
	tqpair->host_ddgst_enable = ic_req->dgst.bits.ddgst_enable ? true : false;
	if (!tqpair->host_ddgst_enable) {
		tqpair->pdu_recv_buf.size -= SPDK_NVME_TCP_DIGEST_LEN * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR;
	}

	tqpair->cpda = spdk_min(ic_req->hpda, SPDK_NVME_TCP_CPDA_MAX);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "cpda of tqpair=(%p) is : %u\n", tqpair, tqpair->cpda);

	rsp_pdu = &tqpair->mgmt_pdu;

	ic_resp = &rsp_pdu->hdr->ic_resp;
	ic_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	ic_resp->common.hlen = ic_resp->common.plen =  sizeof(*ic_resp);
	ic_resp->pfv = 0;
	ic_resp->cpda = tqpair->cpda;
	ic_resp->maxh2cdata = ttransport->transport.opts.max_io_size;
	ic_resp->dgst.bits.hdgst_enable = tqpair->host_hdgst_enable ? 1 : 0;
	ic_resp->dgst.bits.ddgst_enable = tqpair->host_ddgst_enable ? 1 : 0;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	tqpair->state = NVME_TCP_QPAIR_STATE_INITIALIZING;
	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_send_icresp_complete, tqpair);
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	return;
end:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
spdk_nvmf_tcp_pdu_psh_handle(struct spdk_nvmf_tcp_qpair *tqpair,
			     struct spdk_nvmf_tcp_transport *ttransport)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = &tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "pdu type of tqpair(%p) is %d\n", tqpair,
		      pdu->hdr->common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Compare the header of pdu=%p on tqpair=%p\n", pdu, tqpair);
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr->raw + pdu->hdr->common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("Header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr->common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_REQ:
		spdk_nvmf_tcp_icreq_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_REQ);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		spdk_nvmf_tcp_h2c_data_hdr_handle(ttransport, tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		spdk_nvmf_tcp_h2c_term_req_hdr_handle(tqpair, pdu);
		break;

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->pdu_in_progress.hdr->common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
		break;
	}
}

static void
spdk_nvmf_tcp_pdu_ch_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint8_t expected_hlen, pdo;
	bool plen_error = false, pdo_error = false;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
	pdu = &tqpair->pdu_in_progress;

	if (pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received ICreq PDU, and reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_req);
		if (pdu->hdr->common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
			SPDK_ERRLOG("The TCP/IP connection is not negotitated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr->common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
			expected_hlen = sizeof(struct spdk_nvme_tcp_cmd);
			pdo = pdu->hdr->common.pdo;
			if ((tqpair->cpda != 0) && (pdo != ((tqpair->cpda + 1) << 2))) {
				pdo_error = true;
				break;
			}

			if (pdu->hdr->common.plen < expected_hlen) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_h2c_data_hdr);
			pdo = pdu->hdr->common.pdo;
			if ((tqpair->cpda != 0) && (pdo != ((tqpair->cpda + 1) << 2))) {
				pdo_error = true;
				break;
			}
			if (pdu->hdr->common.plen < expected_hlen) {
				plen_error = true;
			}
			break;

		case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr->common.plen <= expected_hlen) ||
			    (pdu->hdr->common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", pdu->hdr->common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr->common.hlen != expected_hlen) {
		SPDK_ERRLOG("PDU type=0x%02x, Expected ICReq header length %u, got %u on tqpair=%p\n",
			    pdu->hdr->common.pdu_type,
			    expected_hlen, pdu->hdr->common.hlen, tqpair);
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
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(&tqpair->pdu_in_progress, tqpair->host_hdgst_enable);
		return;
	}
err:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
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
nvme_tcp_recv_buf_read(struct spdk_sock *sock, struct nvme_tcp_pdu_recv_buf *pdu_recv_buf)
{
	int rc;

	rc = nvme_tcp_read_data(sock, pdu_recv_buf->size - pdu_recv_buf->off,
				(void *)pdu_recv_buf->buf + pdu_recv_buf->off);
	if (rc < 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "will disconnect sock=%p\n", sock);
	} else if (rc > 0) {
		pdu_recv_buf->remain_size = rc;
		spdk_trace_record(TRACE_TCP_READ_FROM_SOCKET_DONE, 0, rc, 0, 0);
	}

	return rc;
}

static uint32_t
nvme_tcp_read_data_from_pdu_recv_buf(struct nvme_tcp_pdu_recv_buf *pdu_recv_buf,
				     uint32_t expected_size,
				     char *dst)
{
	uint32_t size;

	assert(pdu_recv_buf->remain_size > 0);
	size = spdk_min(expected_size, pdu_recv_buf->remain_size);
	if (dst) {
		memcpy(dst, (void *)pdu_recv_buf->buf + pdu_recv_buf->off, size);
	}
	pdu_recv_buf->off += size;
	pdu_recv_buf->remain_size -= size;


	return size;
}

static int
nvme_tcp_read_payload_data_from_pdu_recv_buf(struct nvme_tcp_pdu_recv_buf *pdu_recv_buf,
		struct nvme_tcp_pdu *pdu)
{
	struct iovec iov[NVME_TCP_MAX_SGL_DESCRIPTORS + 1];
	int iovcnt, i;
	uint32_t size = 0;
	void *dst;

	assert(pdu_recv_buf->remain_size > 0);
	iovcnt = nvme_tcp_build_payload_iovs(iov, NVME_TCP_MAX_SGL_DESCRIPTORS + 1, pdu,
					     pdu->ddgst_enable, NULL);
	assert(iovcnt >= 0);
	for (i = 0; i < iovcnt; i++) {
		if (!pdu_recv_buf->remain_size) {
			break;
		}

		dst = NULL;
		if (pdu->hdr->common.pdu_type != SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ) {
			dst = iov[i].iov_base;
		}
		size += nvme_tcp_read_data_from_pdu_recv_buf(pdu_recv_buf, iov[i].iov_len, dst);
	}

	return size;
}

static int
spdk_nvmf_tcp_sock_process(struct spdk_nvmf_tcp_qpair *tqpair)
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tqpair(%p) recv pdu entering state %d\n", tqpair, prev_state);

		pdu = &tqpair->pdu_in_progress;
		switch (tqpair->recv_state) {
		/* Wait for the common header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			if (spdk_unlikely(tqpair->state == NVME_TCP_QPAIR_STATE_INITIALIZING)) {
				return rc;
			}

			if (!tqpair->pdu_recv_buf.remain_size) {
				rc = nvme_tcp_recv_buf_read(tqpair->sock, &tqpair->pdu_recv_buf);
				if (rc <= 0) {
					return rc;
				}
			}
			rc = nvme_tcp_read_data_from_pdu_recv_buf(&tqpair->pdu_recv_buf,
					sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes,
					NULL);
			pdu->ch_valid_bytes += rc;
			if (spdk_likely(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY)) {
				spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
			}

			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* The command header of this PDU has now been read from the socket. */
			spdk_nvmf_tcp_pdu_ch_handle(tqpair);
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			if (!tqpair->pdu_recv_buf.remain_size) {
				rc = nvme_tcp_recv_buf_read(tqpair->sock, &tqpair->pdu_recv_buf);
				if (rc <= 0) {
					return rc;
				}
			}

			rc = nvme_tcp_read_data_from_pdu_recv_buf(&tqpair->pdu_recv_buf,
					pdu->psh_len - pdu->psh_valid_bytes,
					NULL);
			pdu->psh_valid_bytes += rc;
			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
			spdk_nvmf_tcp_pdu_psh_handle(tqpair, ttransport);
			break;
		/* Wait for the req slot */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_REQ:
			spdk_nvmf_tcp_capsule_cmd_hdr_handle(ttransport, tqpair, pdu);
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			/* data digest */
			if (spdk_unlikely((pdu->hdr->common.pdu_type != SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ) &&
					  tqpair->host_ddgst_enable)) {
				data_len += SPDK_NVME_TCP_DIGEST_LEN;
				pdu->ddgst_enable = true;
			}

			if (tqpair->pdu_recv_buf.remain_size) {
				rc = nvme_tcp_read_payload_data_from_pdu_recv_buf(&tqpair->pdu_recv_buf, pdu);
				pdu->readv_offset += rc;
			}

			if (pdu->readv_offset < data_len) {
				rc = nvme_tcp_read_payload_data(tqpair->sock, pdu);
				if (rc < 0) {
					return NVME_TCP_PDU_IN_PROGRESS;
				}
				pdu->readv_offset += rc;
			}

			if (spdk_unlikely(pdu->dif_ctx != NULL)) {
				rc = nvmf_tcp_pdu_payload_insert_dif(pdu, pdu->readv_offset - rc, rc);
				if (rc != 0) {
					return NVME_TCP_PDU_FATAL;
				}
			}

			if (pdu->readv_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All of this PDU has now been read from the socket. */
			spdk_nvmf_tcp_pdu_payload_handle(tqpair, ttransport);
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

static int
spdk_nvmf_tcp_req_parse_sgl(struct spdk_nvmf_tcp_req *tcp_req,
			    struct spdk_nvmf_transport *transport,
			    struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_request		*req = &tcp_req->req;
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;
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

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Data requested length= 0x%x\n", length);

		if (spdk_unlikely(req->dif.dif_insert_or_strip)) {
			req->dif.orig_length = length;
			length = spdk_dif_get_length_with_md(length, &req->dif.dif_ctx);
			req->dif.elba_length = length;
		}

		if (spdk_nvmf_request_get_buffers(req, group, transport, length)) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "No available large data buffers. Queueing request %p\n",
				      tcp_req);
			return 0;
		}

		/* backward compatible */
		req->data = req->iov[0].iov_base;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Request %p took %d buffer/s from central pool, and data=%p\n",
			      tcp_req, req->iovcnt, req->data);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = transport->opts.in_capsule_data_size;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
			      offset, length);

		if (offset > max_len) {
			SPDK_ERRLOG("In-capsule offset 0x%" PRIx64 " exceeds capsule length 0x%x\n",
				    offset, max_len);
			rsp->status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
			return -1;
		}
		max_len -= (uint32_t)offset;

		if (length > max_len) {
			SPDK_ERRLOG("In-capsule data length 0x%x exceeds capsule length 0x%x\n",
				    length, max_len);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		req->data = tcp_req->buf + offset;
		req->data_from_pool = false;
		req->length = length;

		if (spdk_unlikely(req->dif.dif_insert_or_strip)) {
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
spdk_nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
			    struct spdk_nvmf_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint32_t plen, pdo, alignment;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

	rsp_pdu = nvmf_tcp_req_pdu_init(tcp_req);
	assert(rsp_pdu != NULL);

	c2h_data = &rsp_pdu->hdr->c2h_data;
	c2h_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_DATA;
	plen = c2h_data->common.hlen = sizeof(*c2h_data);

	if (tqpair->host_hdgst_enable) {
		plen += SPDK_NVME_TCP_DIGEST_LEN;
		c2h_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	}

	/* set the psh */
	c2h_data->cccid = tcp_req->req.cmd->nvme_cmd.cid;
	c2h_data->datal = tcp_req->req.length;
	c2h_data->datao = 0;

	/* set the padding */
	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			rsp_pdu->padding_len = alignment - plen;
			pdo = plen = alignment;
		}
	}

	c2h_data->common.pdo = pdo;
	plen += c2h_data->datal;
	if (tqpair->host_ddgst_enable) {
		c2h_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	c2h_data->common.plen = plen;

	if (spdk_unlikely(tcp_req->req.dif.dif_insert_or_strip)) {
		rsp_pdu->dif_ctx = &tcp_req->req.dif.dif_ctx;
	}

	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
				  c2h_data->datao, c2h_data->datal);

	if (spdk_unlikely(tcp_req->req.dif.dif_insert_or_strip)) {
		struct spdk_nvme_cpl *rsp = &tcp_req->req.rsp->nvme_cpl;
		struct spdk_dif_error err_blk = {};

		rc = spdk_dif_verify_stream(rsp_pdu->data_iov, rsp_pdu->data_iovcnt,
					    0, rsp_pdu->data_len, rsp_pdu->dif_ctx, &err_blk);
		if (rc != 0) {
			SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
				    err_blk.err_type, err_blk.err_offset);
			rsp->status.sct = SPDK_NVME_SCT_MEDIA_ERROR;
			rsp->status.sc = nvmf_tcp_dif_error_to_compl_status(err_blk.err_type);
			nvmf_tcp_req_pdu_fini(tcp_req);
			spdk_nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
			return;
		}
	}

	c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU;
	if (tqpair->qpair.transport->opts.c2h_success) {
		c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS;
	}

	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_pdu_c2h_data_complete, tcp_req);
}

static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_req	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

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
	spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS && req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		spdk_nvmf_tcp_send_c2h_data(tqpair, tcp_req);
	} else {
		spdk_nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}

	return 0;
}

static void
spdk_nvmf_tcp_set_incapsule_data(struct spdk_nvmf_tcp_qpair *tqpair,
				 struct spdk_nvmf_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t plen = 0;

	pdu = &tqpair->pdu_in_progress;
	plen = pdu->hdr->common.hlen;

	if (tqpair->host_hdgst_enable) {
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (pdu->hdr->common.plen != plen) {
		tcp_req->has_incapsule_data = true;
	}
}

static bool
spdk_nvmf_tcp_req_process(struct spdk_nvmf_tcp_transport *ttransport,
			  struct spdk_nvmf_tcp_req *tcp_req)
{
	struct spdk_nvmf_tcp_qpair		*tqpair;
	int					rc;
	enum spdk_nvmf_tcp_req_state		prev_state;
	bool					progress = false;
	struct spdk_nvmf_transport		*transport = &ttransport->transport;
	struct spdk_nvmf_transport_poll_group	*group;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	group = &tqpair->group->group;
	assert(tcp_req->state != TCP_REQUEST_STATE_FREE);

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tcp_req->state;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Request %p entering state %d on tqpair=%p\n", tcp_req, prev_state,
			      tqpair);

		switch (tcp_req->state) {
		case TCP_REQUEST_STATE_FREE:
			/* Some external code must kick a request into TCP_REQUEST_STATE_NEW
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_NEW:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEW, 0, 0, (uintptr_t)tcp_req, 0);

			/* copy the cmd from the receive pdu */
			tcp_req->cmd = tqpair->pdu_in_progress.hdr->capsule_cmd.ccsqe;

			if (spdk_unlikely(spdk_nvmf_request_get_dif_ctx(&tcp_req->req, &tcp_req->req.dif.dif_ctx))) {
				tcp_req->req.dif.dif_insert_or_strip = true;
				tqpair->pdu_in_progress.dif_ctx = &tcp_req->req.dif.dif_ctx;
			}

			/* The next state transition depends on the data transfer needs of this request. */
			tcp_req->req.xfer = spdk_nvmf_req_get_xfer(&tcp_req->req);

			/* If no data to transfer, ready to execute. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_NONE) {
				/* Reset the tqpair receving pdu state */
				spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
				break;
			}

			spdk_nvmf_tcp_set_incapsule_data(tqpair, tcp_req);

			if (!tcp_req->has_incapsule_data) {
				spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			}

			spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_NEED_BUFFER);
			STAILQ_INSERT_TAIL(&group->pending_buf_queue, &tcp_req->req, buf_link);
			break;
		case TCP_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEED_BUFFER, 0, 0, (uintptr_t)tcp_req, 0);

			assert(tcp_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (!tcp_req->has_incapsule_data && (&tcp_req->req != STAILQ_FIRST(&group->pending_buf_queue))) {
				SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
					      "Not the first element to wait for the buf for tcp_req(%p) on tqpair=%p\n",
					      tcp_req, tqpair);
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = spdk_nvmf_tcp_req_parse_sgl(tcp_req, transport, group);
			if (rc < 0) {
				STAILQ_REMOVE_HEAD(&group->pending_buf_queue, buf_link);
				/* Reset the tqpair receving pdu state */
				spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
				spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				break;
			}

			if (!tcp_req->req.data) {
				SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "No buffer allocated for tcp_req(%p) on tqpair(%p\n)",
					      tcp_req, tqpair);
				/* No buffers available. */
				break;
			}

			STAILQ_REMOVE(&group->pending_buf_queue, &tcp_req->req, spdk_nvmf_request, buf_link);

			/* If data is transferring from host to controller, we need to do a transfer from the host. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				if (tcp_req->req.data_from_pool) {
					SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Sending R2T for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
					spdk_nvmf_tcp_send_r2t_pdu(tqpair, tcp_req);
				} else {
					struct nvme_tcp_pdu *pdu;

					spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);

					pdu = &tqpair->pdu_in_progress;
					SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Not need to send r2t for tcp_req(%p) on tqpair=%p\n", tcp_req,
						      tqpair);
					/* No need to send r2t, contained in the capsuled data */
					nvme_tcp_pdu_set_data_buf(pdu, tcp_req->req.iov, tcp_req->req.iovcnt,
								  0, tcp_req->req.length);
					spdk_nvmf_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
				}
				break;
			}

			spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
			break;
		case TCP_REQUEST_STATE_AWAITING_R2T_ACK:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK, 0, 0, (uintptr_t)tcp_req, 0);
			/* The R2T completion or the h2c data incoming will kick it out of this state. */
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:

			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)tcp_req, 0);
			/* Some external code must kick a request into TCP_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE, 0, 0, (uintptr_t)tcp_req, 0);

			if (spdk_unlikely(tcp_req->req.dif.dif_insert_or_strip)) {
				assert(tcp_req->req.dif.elba_length >= tcp_req->req.length);
				tcp_req->req.length = tcp_req->req.dif.elba_length;
			}

			spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTING);
			spdk_nvmf_request_exec(&tcp_req->req);
			break;
		case TCP_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTING, 0, 0, (uintptr_t)tcp_req, 0);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTED, 0, 0, (uintptr_t)tcp_req, 0);

			if (spdk_unlikely(tcp_req->req.dif.dif_insert_or_strip)) {
				tcp_req->req.length = tcp_req->req.dif.orig_length;
			}

			spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
			break;
		case TCP_REQUEST_STATE_READY_TO_COMPLETE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE, 0, 0, (uintptr_t)tcp_req, 0);
			rc = request_transfer_out(&tcp_req->req);
			assert(rc == 0); /* No good way to handle this currently */
			break;
		case TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST, 0, 0,
					  (uintptr_t)tcp_req,
					  0);
			/* Some external code must kick a request into TCP_REQUEST_STATE_COMPLETED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_COMPLETED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_COMPLETED, 0, 0, (uintptr_t)tcp_req, 0);
			if (tcp_req->req.data_from_pool) {
				spdk_nvmf_request_free_buffers(&tcp_req->req, group, transport);
			}
			tcp_req->req.length = 0;
			tcp_req->req.iovcnt = 0;
			tcp_req->req.data = NULL;

			nvmf_tcp_req_pdu_fini(tcp_req);

			spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_FREE);
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
spdk_nvmf_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;
	int rc;

	assert(tqpair != NULL);
	rc = spdk_nvmf_tcp_sock_process(tqpair);

	/* If there was a new socket error, disconnect */
	if (rc < 0) {
		spdk_nvmf_tcp_qpair_disconnect(tqpair);
	}
}

static int
spdk_nvmf_tcp_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			     struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	rc = spdk_sock_group_add_sock(tgroup->sock_group, tqpair->sock,
				      spdk_nvmf_tcp_sock_cb, tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Could not add sock to sock_group: %s (%d)\n",
			    spdk_strerror(errno), errno);
		return -1;
	}

	rc =  spdk_nvmf_tcp_qpair_sock_init(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Cannot set sock opt for tqpair=%p\n", tqpair);
		return -1;
	}

	rc = spdk_nvmf_tcp_qpair_init(&tqpair->qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init tqpair=%p\n", tqpair);
		return -1;
	}

	rc = spdk_nvmf_tcp_qpair_init_mem_resource(tqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init memory resource info for tqpair=%p\n", tqpair);
		return -1;
	}

	tqpair->group = tgroup;
	tqpair->state = NVME_TCP_QPAIR_STATE_INVALID;
	TAILQ_INSERT_TAIL(&tgroup->qpairs, tqpair, link);

	return 0;
}

static int
spdk_nvmf_tcp_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair		*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair->group == tgroup);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "remove tqpair=%p from the tgroup=%p\n", tqpair, tgroup);
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
spdk_nvmf_tcp_req_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_req *tcp_req;

	ttransport = SPDK_CONTAINEROF(req->qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_req, req);

	spdk_nvmf_tcp_req_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTED);
	spdk_nvmf_tcp_req_process(ttransport, tcp_req);

	return 0;
}

static void
spdk_nvmf_tcp_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Qpair: %p\n", qpair);

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	tqpair->state = NVME_TCP_QPAIR_STATE_EXITED;
	spdk_nvmf_tcp_qpair_destroy(tqpair);
}

static int
spdk_nvmf_tcp_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
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
		if (spdk_nvmf_tcp_req_process(ttransport, tcp_req) == false) {
			break;
		}
	}

	rc = spdk_sock_group_poll(tgroup->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", tgroup->sock_group);
	}

	TAILQ_FOREACH_SAFE(tqpair, &tgroup->await_req, link, tqpair_tmp) {
		spdk_nvmf_tcp_sock_process(tqpair);
	}

	return rc;
}

static int
spdk_nvmf_tcp_qpair_get_trid(struct spdk_nvmf_qpair *qpair,
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
spdk_nvmf_tcp_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	return spdk_nvmf_tcp_qpair_get_trid(qpair, trid, 0);
}

static int
spdk_nvmf_tcp_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				  struct spdk_nvme_transport_id *trid)
{
	return spdk_nvmf_tcp_qpair_get_trid(qpair, trid, 1);
}

static int
spdk_nvmf_tcp_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	return spdk_nvmf_tcp_qpair_get_trid(qpair, trid, 0);
}

#define SPDK_NVMF_TCP_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_TCP_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR 128
#define SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE 131072
#define SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS 511
#define SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE 32
#define SPDK_NVMF_TCP_DEFAULT_SUCCESS_OPTIMIZATION true
#define SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_TCP_DEFAULT_SOCK_PRIORITY 0

static void
spdk_nvmf_tcp_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_TCP_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_TCP_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_TCP_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_TCP_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_TCP_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_TCP_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_TCP_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_TCP_DEFAULT_BUFFER_CACHE_SIZE;
	opts->c2h_success =		SPDK_NVMF_TCP_DEFAULT_SUCCESS_OPTIMIZATION;
	opts->dif_insert_or_strip =	SPDK_NVMF_TCP_DEFAULT_DIF_INSERT_OR_STRIP;
	opts->sock_priority =		SPDK_NVMF_TCP_DEFAULT_SOCK_PRIORITY;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp = {
	.name = "TCP",
	.type = SPDK_NVME_TRANSPORT_TCP,
	.opts_init = spdk_nvmf_tcp_opts_init,
	.create = spdk_nvmf_tcp_create,
	.destroy = spdk_nvmf_tcp_destroy,

	.listen = spdk_nvmf_tcp_listen,
	.stop_listen = spdk_nvmf_tcp_stop_listen,
	.accept = spdk_nvmf_tcp_accept,

	.listener_discover = spdk_nvmf_tcp_discover,

	.poll_group_create = spdk_nvmf_tcp_poll_group_create,
	.get_optimal_poll_group = spdk_nvmf_tcp_get_optimal_poll_group,
	.poll_group_destroy = spdk_nvmf_tcp_poll_group_destroy,
	.poll_group_add = spdk_nvmf_tcp_poll_group_add,
	.poll_group_remove = spdk_nvmf_tcp_poll_group_remove,
	.poll_group_poll = spdk_nvmf_tcp_poll_group_poll,

	.req_free = spdk_nvmf_tcp_req_free,
	.req_complete = spdk_nvmf_tcp_req_complete,

	.qpair_fini = spdk_nvmf_tcp_close_qpair,
	.qpair_get_local_trid = spdk_nvmf_tcp_qpair_get_local_trid,
	.qpair_get_peer_trid = spdk_nvmf_tcp_qpair_get_peer_trid,
	.qpair_get_listen_trid = spdk_nvmf_tcp_qpair_get_listen_trid,
};

SPDK_NVMF_TRANSPORT_REGISTER(tcp, &spdk_nvmf_transport_tcp);
SPDK_LOG_REGISTER_COMPONENT("nvmf_tcp", SPDK_LOG_NVMF_TCP)
