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

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/io_channel.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/*
 * AIO backend requires block size aligned data buffers,
 * extra 4KiB aligned data buffer should work for most devices.
 */
#define SHIFT_4KB			12u
#define NVMF_DATA_BUFFER_ALIGNMENT	(1u << SHIFT_4KB)
#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1u)
#define NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME 16

#define NVMF_TCP_CH_LEN 8
#define NVMF_TCP_PSH_LEN_MAX 120
#define NVMF_TCP_DIGEST_LEN 4
#define NVMF_TCP_CPDA_MAX 31
#define NVMF_TCP_ALIGNMENT 4
#define SPDK_CRC32C_INITIAL    0xffffffffUL
#define SPDK_CRC32C_XOR        0xffffffffUL

#define MAKE_DIGEST_WORD(BUF, CRC32C) \
	(   ((*((uint8_t *)(BUF)+0)) = (uint8_t)((uint32_t)(CRC32C) >> 0)), \
	    ((*((uint8_t *)(BUF)+1)) = (uint8_t)((uint32_t)(CRC32C) >> 8)), \
	    ((*((uint8_t *)(BUF)+2)) = (uint8_t)((uint32_t)(CRC32C) >> 16)), \
	    ((*((uint8_t *)(BUF)+3)) = (uint8_t)((uint32_t)(CRC32C) >> 24)))

enum spdk_nvmf_tcp_request_state {

	/* The request is not currently in use */
	TCP_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	TCP_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	TCP_REQUEST_STATE_NEED_BUFFER,

	/* The request is waiting on TCP queue depth availability
	 * to transfer data from the host to the controller.
	 */
	TCP_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER,

	/* The request is currently transferring data from the host to the controller. */
	TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	TCP_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	TCP_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	TCP_REQUEST_STATE_EXECUTED,

	/* The request is waiting on TCP queue depth availability
	 * to transfer data from the controller to the host.
	 */
	TCP_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST,

	/* The request is ready to send a completion */
	TCP_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request currently has a completion outstanding */
	TCP_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	TCP_REQUEST_STATE_COMPLETED,
};

enum spdk_nvmf_tcp_qpair_recv_state {
	TQPAIR_RECV_STATE_INVALID,

	/* New connection waiting for ICReq */
	TQPAIR_RECV_STATE_AWAIT_ICREQ,

	/* Active connection waiting for any PDU type */
	TQPAIR_RECV_STATE_AWAIT_PDU_HDR,

	/* Active connection waiting for CapsuleCmd (already received header) */
	TQPAIR_RECV_STATE_AWAIT_CAPSULE_CMD,
};

enum nvmf_tcp_qpair_state {
	NVMF_TCP_QPAIR_STATE_INVALID = 0,
	NVMF_TCP_QPAIR_STATE_RUNNING = 1,
	NVMF_TCP_QPAIR_STATE_EXITING = 2,
	NVMF_TCP_QPAIR_STATE_EXITED = 3,
};

enum spdk_nvmf_tcp_error_codes {
	SPDK_NVMF_TCP_PDU_SUCCESS	= 0,
	SPDK_NVMF_TCP_CONNECTION_FATAL	= -1,
	SPDK_NVMF_TCP_PDU_FATAL		= -2,
};

struct spdk_nvmf_tcp_qpair;
typedef void (*spdk_nvmf_tcp_qpair_xfer_complete_cb)(struct spdk_nvmf_tcp_qpair *tqpair,
		void *cb_arg);

struct spdk_nvmf_tcp_pdu {
	union {
		uint8_t					raw[128];
		struct spdk_nvme_tcp_common_pdu_hdr	hdr;
		struct spdk_nvme_tcp_ic_req		ic_req;
		struct spdk_nvme_tcp_term_req		term_req;
		struct spdk_nvme_tcp_cmd		capsule_cmd;
		struct spdk_nvme_tcp_h2c_data		h2c_data;
		struct spdk_nvme_tcp_ic_resp		ic_resp;
		struct spdk_nvme_tcp_term_resp		term_resp;
		struct spdk_nvme_tcp_rsp		capsule_resp;
		struct spdk_nvme_tcp_c2h_data		c2h_data;
		struct spdk_nvme_tcp_r2t		r2t;

	} u;

	int						ch_valid_bytes;
	int						psh_valid_bytes;
	int						padding_valid_bytes;
	int						data_valid_bytes;
	int						hdigest_valid_bytes;
	int						ddigest_valid_bytes;

	int						ref;
	uint8_t						header_digest[NVMF_TCP_DIGEST_LEN];
	uint8_t						data_digest[NVMF_TCP_DIGEST_LEN];
	uint8_t						*data;
	struct spdk_nvmf_tcp_qpair			*tqpair;

	uint32_t					writev_offset;
	TAILQ_ENTRY(spdk_nvmf_tcp_pdu)			tailq;
	uint32_t					remaining;

	spdk_nvmf_tcp_qpair_xfer_complete_cb		complete_cb;
	void						*cb_arg;
	struct spdk_nvmf_request			req;
};

struct spdk_nvmf_tcp_request  {
	struct spdk_nvmf_request	req;
	struct spdk_nvmf_tcp_pdu	*r_pdu; // recv pdu
	struct spdk_nvmf_tcp_pdu	*s_pdu; // send_pdu;
	TAILQ_ENTRY(spdk_nvmf_tcp_pdu)	tailq;
};

struct spdk_nvmf_tcp_qpair {
	TAILQ_ENTRY(spdk_nvmf_tcp_qpair)	link;
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_tcp_port		*port;
	struct spdk_sock			*sock;
	struct spdk_poller			*flush_poller;

	enum spdk_nvmf_tcp_qpair_recv_state	recv_state;
	enum  nvmf_tcp_qpair_state		state;

	struct spdk_nvmf_tcp_pdu		*recv_pdu;

	TAILQ_HEAD(, spdk_nvmf_tcp_pdu)		send_queue;

	uint64_t				maxr2t;
	uint8_t					cpda;

	uint32_t				host_hdgst_enable : 1;
	uint32_t				host_ddgst_enable : 1;


	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;

	/* Requests that are not in use */
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	free_reqs;
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	outstanding_reqs;
	struct spdk_nvmf_tcp_request		*reqs;

	/* Mgmt channel */
	struct spdk_io_channel			*mgmt_channel;
	struct spdk_nvmf_tcp_mgmt_channel	*ch;
};

struct spdk_nvmf_tcp_poll_group {
	struct spdk_nvmf_transport_poll_group	group;
	struct spdk_sock_group			*sock_group;
	TAILQ_HEAD(, spdk_nvmf_tcp_qpair)	qpairs;
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

	struct spdk_mempool			*data_buf_pool;
	struct spdk_mempool			*pdu_pool;

	uint16_t				max_queue_depth;
	uint32_t				max_io_size;
	uint32_t				in_capsule_data_size;

	TAILQ_HEAD(, spdk_nvmf_tcp_port)	ports;
};

struct spdk_nvmf_tcp_mgmt_channel {
	/* Requests that are waiting to obtain a data buffer */
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	pending_data_buf_queue;
};

static uint32_t
spdk_nvmf_tcp_pdu_calc_header_digest(struct spdk_nvmf_tcp_pdu *pdu)
{
	uint32_t crc32c;
	uint32_t hlen = pdu->u.hdr.hlen;

	crc32c = SPDK_CRC32C_INITIAL;
	crc32c = spdk_crc32c_update(&pdu->u.raw, hlen, crc32c);
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static uint32_t
spdk_nvmf_tcp_calc_data_digest(void *data, int data_len)
{
	uint32_t crc32c;
	uint32_t mod;

	crc32c = SPDK_CRC32C_INITIAL;
	crc32c = spdk_crc32c_update(data, data_len, crc32c);

	mod = data_len % NVMF_TCP_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = NVMF_TCP_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}

	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}


static struct spdk_nvmf_tcp_pdu *
spdk_get_nvmf_tcp_pdu(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_pdu *pdu;
	struct spdk_nvmf_tcp_transport *ttransport;

	ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);
	assert(ttransport != NULL);
	pdu = spdk_mempool_get(ttransport->pdu_pool);
	if (!pdu) {
		SPDK_ERRLOG("Unable to get PDU\n");
		abort();
	}

	/* we do not want to zero out the last part of the structure reserved for AHS and sense data */
	memset(pdu, 0, sizeof(*pdu));
	pdu->ref = 1;
	pdu->tqpair = tqpair;

	return pdu;
}

static void
spdk_put_nvmf_tcp_pdu(struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	if (!pdu) {
		return;
	}

	assert(pdu->ref > 0);
	assert(pdu->tqpair != NULL);
	ttransport = SPDK_CONTAINEROF(pdu->tqpair->qpair.transport, struct spdk_nvmf_tcp_transport,
				      transport);
	pdu->ref--;
	if (pdu->ref == 0) {
		spdk_mempool_put(ttransport->pdu_pool, pdu);
	}
}

static int
spdk_nvmf_tcp_mgmt_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tcp_mgmt_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->pending_data_buf_queue);
	return 0;
}

static void
spdk_nvmf_tcp_mgmt_channel_destroy(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tcp_mgmt_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->pending_data_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on channel destruction\n");
	}
}

static void
spdk_nvmf_tcp_qpair_destroy(struct spdk_nvmf_tcp_qpair *tqpair)
{
	spdk_poller_unregister(&tqpair->flush_poller);
	spdk_sock_close(&tqpair->sock);

	if (tqpair->mgmt_channel) {
		spdk_put_io_channel(tqpair->mgmt_channel);
	}

	free(tqpair->reqs);
	free(tqpair);
}

#if 0
static int
request_transfer_in(struct spdk_nvmf_request *req)
{
	int				rc;
	struct spdk_nvmf_tcp_request	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct ibv_send_wr		*bad_wr = NULL;

	qpair = req->qpair;
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	assert(req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER);

	tqpair->cur_tcp_rw_depth++;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP READ POSTED. Request: %p Connection: %p\n", req, qpair);
	spdk_trace_record(TRACE_TCP_READ_START, 0, 0, (uintptr_t)req, 0);

	tcp_req->data.wr.opcode = IBV_WR_TCP_READ;
	tcp_req->data.wr.next = NULL;
	rc = ibv_post_send(tqpair->cm_id->qp, &tcp_req->data.wr, &bad_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to transfer data from host to target\n");

		/* Decrement r/w counter back since data transfer
		 * has not started.
		 */
		tqpair->cur_tcp_rw_depth--;
		return -1;
	}

	return 0;
}

static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	int				rc;
	struct spdk_nvmf_tcp_request	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp;
	struct ibv_recv_wr		*bad_recv_wr = NULL;
	struct ibv_send_wr		*send_wr, *bad_send_wr = NULL;

	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	/* Post the capsule to the recv buffer */
	assert(tcp_req->recv != NULL);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP RECV POSTED. Recv: %p Connection: %p\n", tcp_req->recv,
		      tqpair);
	rc = ibv_post_recv(tqpair->cm_id->qp, &tcp_req->recv->wr, &bad_recv_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to re-post rx descriptor\n");
		return rc;
	}
	tcp_req->recv = NULL;

	/* Build the response which consists of an optional
	 * TCP WRITE to transfer data, plus an TCP SEND
	 * containing the response.
	 */
	send_wr = &tcp_req->rsp.wr;

	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP WRITE POSTED. Request: %p Connection: %p\n", req, qpair);
		spdk_trace_record(TRACE_TCP_WRITE_START, 0, 0, (uintptr_t)req, 0);

		tqpair->cur_tcp_rw_depth++;
		tcp_req->data.wr.opcode = IBV_WR_TCP_WRITE;

		tcp_req->data.wr.next = send_wr;
		send_wr = &tcp_req->data.wr;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "TCP SEND POSTED. Request: %p Connection: %p\n", req, qpair);
	spdk_trace_record(TRACE_NVMF_IO_COMPLETE, 0, 0, (uintptr_t)req, 0);

	/* Send the completion */
	rc = ibv_post_send(tqpair->cm_id->qp, send_wr, &bad_send_wr);
	if (rc) {
		SPDK_ERRLOG("Unable to send response capsule\n");

		if (tcp_req->data.wr.opcode == IBV_WR_TCP_WRITE) {
			/* Decrement r/w counter back since data transfer
			 * has not started.
			 */
			tqpair->cur_tcp_rw_depth--;
		}
	}

	return rc;
}
#endif

#if 0
static void
nvmf_tcp_handle_disconnect(void *ctx)
{
	struct spdk_nvmf_qpair		*qpair = ctx;
	struct spdk_nvmf_ctrlr		*ctrlr;
	struct spdk_nvmf_tcp_qpair	*tqpair;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	ctrlr = qpair->ctrlr;
	if (ctrlr == NULL) {
		/* No ctrlr has been established yet, so destroy
		 * the connection.
		 */
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return;
	}

	spdk_nvmf_ctrlr_disconnect(qpair);
}

static int
nvmf_tcp_disconnect(struct spdk_sock *sock)
{
#if 0
	struct spdk_nvmf_qpair	*qpair;
	struct spdk_io_channel	*ch;

	qpair = evt->id->context;
	if (qpair == NULL) {
		SPDK_ERRLOG("disconnect request: no active connection\n");
		return -1;
	}
	/* ack the disconnect event before tcp_destroy_id */
	tcp_ack_cm_event(evt);

	ch = spdk_io_channel_from_ctx(qpair->group);
	spdk_thread_send_msg(spdk_io_channel_get_thread(ch), nvmf_tcp_handle_disconnect, qpair);
#endif

	return 0;
}

static enum spdk_nvme_data_transfer spdk_nvmf_tcp_request_get_xfer(struct spdk_nvmf_tcp_request
		*tcp_req)
{
	enum spdk_nvme_data_transfer xfer;
	struct spdk_nvme_cmd *cmd = &tcp_req->req.cmd->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;

	/* Figure out data transfer direction */
	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		xfer = spdk_nvme_opc_get_data_transfer(tcp_req->req.cmd->nvmf_cmd.fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(cmd->opc);

		/* Some admin commands are special cases */
		if ((tcp_req->req.qpair->qid == 0) &&
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
#endif


static struct spdk_nvmf_transport *
spdk_nvmf_tcp_create(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_tcp_transport *ttransport;

	ttransport = calloc(1, sizeof(*ttransport));
	if (!ttransport) {
		return NULL;
	}

	pthread_mutex_init(&ttransport->lock, NULL);
	TAILQ_INIT(&ttransport->ports);

	ttransport->transport.tgt = tgt;
	ttransport->transport.ops = &spdk_nvmf_transport_tcp;

	SPDK_NOTICELOG("*** TCP Transport Init ***\n");

	ttransport->max_queue_depth = tgt->opts.max_queue_depth;
	ttransport->max_io_size = tgt->opts.max_io_size;
	ttransport->in_capsule_data_size = tgt->opts.in_capsule_data_size;

	// TODO: create poll group for listen sockets

	ttransport->data_buf_pool = spdk_mempool_create("spdk_nvmf_tcp_data",
				    ttransport->max_queue_depth * 4, /* The 4 is arbitrarily chosen. Needs to be configurable. */
				    ttransport->max_io_size + NVMF_DATA_BUFFER_ALIGNMENT,
				    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				    SPDK_ENV_SOCKET_ID_ANY);

	if (!ttransport->data_buf_pool) {
		SPDK_ERRLOG("Unable to allocate buffer pool for poll group\n");
		free(ttransport);
		return NULL;
	}

	ttransport->pdu_pool = spdk_mempool_create("spdk_nvmf_tcp_pdu",
			       ttransport->max_queue_depth * 4, /* The 4 is arbitrarily chosen. Needs to be configurable. */
			       sizeof(struct spdk_nvmf_tcp_pdu),
			       SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
			       SPDK_ENV_SOCKET_ID_ANY);

	if (!ttransport->pdu_pool) {
		SPDK_ERRLOG("Unable to allocate data pool for poll group\n");
		spdk_mempool_free(ttransport->data_buf_pool);
		free(ttransport);
		return NULL;
	}

	spdk_io_device_register(ttransport, spdk_nvmf_tcp_mgmt_channel_create,
				spdk_nvmf_tcp_mgmt_channel_destroy,
				sizeof(struct spdk_nvmf_tcp_mgmt_channel));

	return &ttransport->transport;
}

static int
spdk_nvmf_tcp_destroy(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_tcp_transport	*ttransport;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	if (spdk_mempool_count(ttransport->data_buf_pool) != (ttransport->max_queue_depth * 4)) {
		SPDK_ERRLOG("transport buffer pool count is %zu but should be %u\n",
			    spdk_mempool_count(ttransport->data_buf_pool),
			    ttransport->max_queue_depth * 4);
	}

	if (spdk_mempool_count(ttransport->pdu_pool) != (ttransport->max_queue_depth * 4)) {
		SPDK_ERRLOG("transport pdu pool count is %zu but should be %u\n",
			    spdk_mempool_count(ttransport->pdu_pool),
			    ttransport->max_queue_depth * 4);
	}


	spdk_mempool_free(ttransport->data_buf_pool);
	spdk_mempool_free(ttransport->pdu_pool);
	spdk_io_device_unregister(ttransport, NULL);
	free(ttransport);
	return 0;
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
	canon_trid->trtype = SPDK_NVMF_TRTYPE_TCP;
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
		     const struct spdk_nvme_transport_id *trid)
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Already listening on %s port %s\n",
			      trid->traddr, trid->trsvcid);
		port->ref++;
		pthread_mutex_unlock(&ttransport->lock);
		return 0;
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	port->ref = 1;

	if (_spdk_nvmf_tcp_canon_listen_trid(&port->trid, trid) != 0) {
		SPDK_ERRLOG("Invalid traddr %s / trsvcid %s\n",
			    trid->traddr, trid->trsvcid);
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	port->listen_sock = spdk_sock_listen(trid->traddr, trsvcid_int);
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

	SPDK_NOTICELOG("*** NVMe/TCP Target Listening on %s port %d ***\n",
		       trid->traddr, trsvcid_int);

	TAILQ_INSERT_TAIL(&ttransport->ports, port, link);
	pthread_mutex_unlock(&ttransport->lock);

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
			// TODO: remove sock from acceptor listen group
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

static int
spdk_nvmf_tcp_build_iovecs(struct spdk_nvmf_tcp_qpair *tqpair, struct iovec *iovec,
			   struct spdk_nvmf_tcp_pdu *pdu)
{

	int iovec_cnt = 0;
	uint32_t crc32c;
	int enable_digest;
	int hlen;
	int data_len;
	int padding_len = 0;
	struct spdk_nvme_tcp_c2h_data *c2h_pdu;

	hlen = pdu->u.hdr.hlen;

	enable_digest = 1;
	if (pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_TERM_RESP) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	/* PDU header */
	iovec[iovec_cnt].iov_base = &pdu->u.raw;
	iovec[iovec_cnt].iov_len = hlen;
	iovec_cnt++;

	/* Header Digest */
	if (enable_digest && tqpair->host_hdgst_enable) {
		crc32c = spdk_nvmf_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD(pdu->header_digest, crc32c);

		iovec[iovec_cnt].iov_base = pdu->header_digest;
		iovec[iovec_cnt].iov_len = NVMF_TCP_DIGEST_LEN;
		iovec_cnt++;
		padding_len -= NVMF_TCP_DIGEST_LEN;
	}

	if (pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_DATA) {
		c2h_pdu = &pdu->u.c2h_data;
		if (c2h_pdu->datal > 0) {
			data_len = c2h_pdu->datal;
			/* Padding */
			padding_len += (tqpair->cpda + 1) * 4 - hlen;
			if (padding_len > 0) {
				iovec[iovec_cnt].iov_base = &pdu->u.raw + hlen;
				iovec[iovec_cnt].iov_len = padding_len;
				iovec_cnt++;
			}

			/* Data Segment */
			iovec[iovec_cnt].iov_base = pdu->data;
			iovec[iovec_cnt].iov_len = c2h_pdu->datal;
			iovec_cnt++;

			/* Data Digest */
			if (enable_digest && tqpair->host_ddgst_enable) {
				crc32c = spdk_nvmf_tcp_calc_data_digest(pdu->data, data_len);
				MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
				iovec[iovec_cnt].iov_base = pdu->data_digest;
				iovec[iovec_cnt].iov_len = NVMF_TCP_DIGEST_LEN;
				iovec_cnt++;
			}
		}
	}

	return iovec_cnt;

}

static int
spdk_nvmf_tcp_qpair_flush_pdus_internal(struct spdk_nvmf_tcp_qpair *tqpair)
{
	const int array_size = 32;
	struct iovec	iovec_array[array_size];
	struct iovec	*iov = iovec_array;
	int iovec_cnt = 0;
	int bytes = 0;
	int total_length = 0;
	uint32_t writev_offset;
	struct spdk_nvmf_tcp_pdu *pdu;
	int pdu_length;
	spdk_nvmf_tcp_qpair_xfer_complete_cb		complete_cb;
	void						*cb_arg;

	pdu = TAILQ_FIRST(&tqpair->send_queue);

	if (pdu == NULL) {
		return 0;
	}

	/*
	 * Build up a list of iovecs for the first few PDUs in the
	 *  tqpair 's send_queue.
	 */
	while (pdu != NULL && ((array_size - iovec_cnt) >= 5)) {
		pdu_length = pdu->u.hdr.length;
		iovec_cnt += spdk_nvmf_tcp_build_iovecs(tqpair,
							&iovec_array[iovec_cnt],
							pdu);
		total_length += pdu_length;
		pdu = TAILQ_NEXT(pdu, tailq);
	}

	/*
	 * Check if the first PDU was partially written out the last time
	 *  this function was called, and if so adjust the iovec array
	 *  accordingly.
	 */
	writev_offset = TAILQ_FIRST(&tqpair->send_queue)->writev_offset;
	total_length -= writev_offset;
	while (writev_offset > 0) {
		if (writev_offset >= iov->iov_len) {
			writev_offset -= iov->iov_len;
			iov++;
			iovec_cnt--;
		} else {
			iov->iov_len -= writev_offset;
			iov->iov_base = (char *)iov->iov_base + writev_offset;
			writev_offset = 0;
		}
	}

	bytes = spdk_sock_writev(tqpair->sock, iov, iovec_cnt);
	if (bytes == -1) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return 1;
		} else {
			SPDK_ERRLOG("spdk_sock_writev() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
			return -1;
		}
	}

	pdu = TAILQ_FIRST(&tqpair->send_queue);

	/*
	 * Free any PDUs that were fully written.  If a PDU was only
	 *  partially written, update its writev_offset so that next
	 *  time only the unwritten portion will be sent to writev().
	 */
	while (bytes > 0) {
		pdu_length = pdu->u.hdr.length;
		pdu_length -= pdu->writev_offset;

		if (bytes >= pdu_length) {
			bytes -= pdu_length;
			complete_cb = pdu->complete_cb;
			cb_arg = pdu->cb_arg;
			TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
			complete_cb(tqpair, cb_arg);
			spdk_put_nvmf_tcp_pdu(pdu);
			pdu = TAILQ_FIRST(&tqpair->send_queue);

		} else {
			pdu->writev_offset += bytes;
			bytes = 0;
		}
	}

	return TAILQ_EMPTY(&tqpair->send_queue) ? 0 : 1;
}

static int
spdk_nvmf_tcp_qpair_flush_pdus(void *_tqpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair = _tqpair;
	int rc;

	if (tqpair->state == NVMF_TCP_QPAIR_STATE_RUNNING) {
		rc = spdk_nvmf_tcp_qpair_flush_pdus_internal(tqpair);
		if (rc == 0 && tqpair->flush_poller != NULL) {
			spdk_poller_unregister(&tqpair->flush_poller);
		} else if (rc == 1 && tqpair->flush_poller == NULL) {
			tqpair->flush_poller = spdk_poller_register(spdk_nvmf_tcp_qpair_flush_pdus,
					       tqpair, 50);
		}
	} else {
		/*
		 * If the tqpair state is not RUNNING, then
		 * keep trying to flush PDUs until our list is
		 * empty - to make sure all data is sent before
		 * closing the connection.
		 */
		do {
			rc = spdk_nvmf_tcp_qpair_flush_pdus_internal(tqpair);
		} while (rc == 1);
	}

	if (rc < 0 && tqpair->state < NVMF_TCP_QPAIR_STATE_RUNNING) {
		/*
		 * If the poller has already started destruction of the tqpair,
		 *  i.e. the socket read failed, then the connection state may already
		 *  be EXITED.  We don't want to set it back to EXITING in that case.
		 */
		tqpair->state = NVMF_TCP_QPAIR_STATE_RUNNING;
	}

	return -1;
}

static void
_spdk_nvmf_tcp_qpair_send(struct spdk_nvmf_tcp_qpair *tqpair,
			  struct spdk_nvmf_tcp_pdu *pdu,
			  spdk_nvmf_tcp_qpair_xfer_complete_cb send_complete_cb,
			  void *cb_arg)
{
	pdu->complete_cb = send_complete_cb;
	pdu->cb_arg = cb_arg;
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	spdk_nvmf_tcp_qpair_flush_pdus(tqpair);
}

static void
_spdk_nvmf_tcp_recv_capsule_cmd_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg)
{

}

static inline void
spdk_iscsi_task_associate_pdu(struct spdk_nvmf_tcp_request *tcp_req, struct spdk_nvmf_tcp_pdu *pdu)
{
	tcp_req->r_pdu = pdu;
	pdu->ref++;
}

static int
spdk_nvmf_tcp_qpair_initialize(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_transport	*ttransport;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				i;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New TCP Connection: %p\n", qpair);

	tqpair->reqs = calloc(tqpair->max_queue_depth, sizeof(*tqpair->reqs));
	if (!tqpair->reqs) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for TCP queue.\n");
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return -1;
	}

	for (i = 0; i < tqpair->max_queue_depth; i++) {
		struct spdk_nvmf_tcp_request *treq = &tqpair->reqs[i];

		treq->req.qpair = &tqpair->qpair;
		/* Allocate it later */
		//treq->req.cmd = &treq->cmd;
		//treq->req.rsp = &treq->rsp;
		TAILQ_INSERT_TAIL(&tqpair->free_reqs, treq, link);
	}

	return 0;
}

static void
_spdk_nvmf_tcp_handle_connect(struct spdk_nvmf_transport *transport,
			      struct spdk_nvmf_tcp_port *port,
			      struct spdk_sock *sock, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_qpair *tqpair;
	int rc;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New connection accepted on %s port %s\n",
		      port->trid.traddr, port->trid.trsvcid);

	tqpair = calloc(1, sizeof(struct spdk_nvmf_tcp_qpair));
	if (tqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_sock_close(&sock);
		return;
	}

	tqpair->sock = sock;
	tqpair->port = port;
	tqpair->max_queue_depth = ttransport->max_queue_depth;
	tqpair->qpair.transport = transport;
	TAILQ_INIT(&tqpair->send_queue);
	TAILQ_INIT(&tqpair->free_reqs);
	TAILQ_INIT(&tqpair->oustanding_reqs);

	rc = spdk_nvmf_tcp_qpair_initialize(&tqpair->qpair);

	if (rc < 0) {
		return;
	}

	tqpair->state = NVMF_TCP_QPAIR_STATE_INVALID;
	cb_fn(&tqpair->qpair);
}

static void
spdk_nvmf_tcp_port_accept(struct spdk_nvmf_transport *transport, struct spdk_nvmf_tcp_port *port,
			  new_qpair_fn cb_fn)
{
	struct spdk_sock *sock;
	int i;

	for (i = 0; i < NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(port->listen_sock);
		if (sock) {
			_spdk_nvmf_tcp_handle_connect(transport, port, sock, cb_fn);
		}
	}
}

static void
spdk_nvmf_tcp_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_port *port;

	ttransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_tcp_transport, transport);

	TAILQ_FOREACH(port, &ttransport->ports, link) {
		spdk_nvmf_tcp_port_accept(transport, port, cb_fn);
	}
}

static void
spdk_nvmf_tcp_discover(struct spdk_nvmf_transport *transport,
		       struct spdk_nvme_transport_id *trid,
		       struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = SPDK_NVMF_TRTYPE_TCP;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	entry->tsas.tcp.tcp_secsoc = SPDK_NVME_TCP_SECURITY_NONE;
	entry->tsas.tcp.tcp_clist = 0;
	memset(entry->tsas.tcp.tcp_cipher, 0, sizeof(entry->tsas.tcp.tcp_cipher));
}

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_tcp_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;

	tgroup = calloc(1, sizeof(*tgroup));
	if (!tgroup) {
		return NULL;
	}

	tgroup->sock_group = spdk_sock_group_create();
	if (!tgroup->sock_group) {
		goto cleanup;
	}

	TAILQ_INIT(&tgroup->qpairs);
	return &tgroup->group;

cleanup:
	free(tgroup);
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

static int
spdk_nvmf_tcp_qpair_read_data(struct spdk_nvmf_tcp_qpair *tqpair, int bytes,
			      void *buf)
{
	int ret;

	if (bytes == 0) {
		return 0;
	}

	ret = spdk_sock_recv(tqpair->sock, buf, bytes);

	if (ret > 0) {
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno == ECONNRESET) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "spdk_sock_recv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	return SPDK_NVMF_TCP_CONNECTION_FATAL;
}

static int
spdk_nvmf_tcp_read_pdu(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_pdu **_pdu)
{
	int rc;
	struct spdk_nvmf_tcp_pdu *pdu;
	int psh_len, total_pdu_len, data_len, hlen, padding_len, pdo;
	struct spdk_nvmf_tcp_transport *ttransport;
	ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);
	if (tqpair->recv_pdu == NULL) {
		tqpair->recv_pdu = spdk_get_nvmf_tcp_pdu(tqpair);
		if (tqpair->recv_pdu == NULL) {
			return SPDK_NVMF_TCP_CONNECTION_FATAL;
		}
	}

	pdu = tqpair->recv_pdu;

	/* common header */
	if (pdu->ch_valid_bytes < NVMF_TCP_CH_LEN) {
		rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
						   NVMF_TCP_CH_LEN - pdu->ch_valid_bytes,
						   (uint8_t *)&pdu->u.hdr + pdu->ch_valid_bytes);
		if (rc < 0) {
			*_pdu = NULL;
			spdk_put_nvmf_tcp_pdu(pdu);
			tqpair->recv_pdu = NULL;
			return rc;
		}
		pdu->ch_valid_bytes += rc;
		if (pdu->ch_valid_bytes < NVMF_TCP_CH_LEN) {
			*_pdu = NULL;
			return SPDK_NVMF_TCP_PDU_SUCCESS;
		}
	}

	/* pdu specific header_len */
	hlen = pdu->u.hdr.hlen;
	psh_len = hlen - pdu->u.hdr.hlen;
	assert(psh_len <= NVMF_TCP_PSH_LEN_MAX);
	if (pdu->psh_valid_bytes < psh_len) {
		rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
						   psh_len - pdu->psh_valid_bytes,
						   (uint8_t *)&pdu->u.hdr + pdu->psh_valid_bytes + NVMF_TCP_CH_LEN);
		if (rc < 0) {
			*_pdu = NULL;
			spdk_put_nvmf_tcp_pdu(pdu);
			tqpair->recv_pdu = NULL;
			return rc;
		}

		pdu->psh_valid_bytes += rc;
		if (pdu->psh_valid_bytes < psh_len) {
			*_pdu = NULL;
			return SPDK_NVMF_TCP_PDU_SUCCESS;
		}
	}

	/* total_pdu_len */
	total_pdu_len = pdu->u.hdr.length;
	if (total_pdu_len == hlen) {
		goto end;
	}

	data_len = total_pdu_len = hlen;
	assert(data_len > 0);

	if (pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_TERM_REQ) {
		if (pdu->data_valid_bytes < data_len) {
			pdu->data = spdk_mempool_get(ttransport->data_buf_pool);
			if (!pdu->data) {
				*_pdu = NULL;
				return SPDK_NVMF_TCP_PDU_SUCCESS;
			}
			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   data_len - pdu->data_valid_bytes,
							   (uint8_t *)pdu->data + pdu->data_valid_bytes);
			if (rc < 0) {
				*_pdu = NULL;
				spdk_put_nvmf_tcp_pdu(pdu);
				tqpair->recv_pdu = NULL;
				return rc;
			}

			pdu->data_valid_bytes += rc;
			if (pdu->data_valid_bytes < data_len) {
				*_pdu = NULL;
				return SPDK_NVMF_TCP_PDU_SUCCESS;
			}
		}
	} else {
		/* header digst */
		if (tqpair->host_ddgst_enable && pdu->hdigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   NVMF_TCP_DIGEST_LEN - pdu->hdigest_valid_bytes,
							   pdu->header_digest + pdu->hdigest_valid_bytes);
			if (rc < 0) {
				*_pdu = NULL;
				spdk_put_nvmf_tcp_pdu(pdu);
				tqpair->recv_pdu = NULL;
				return rc;
			}

			pdu->hdigest_valid_bytes += rc;
			if (pdu->hdigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
				*_pdu = NULL;
				return SPDK_NVMF_TCP_PDU_SUCCESS;
			}
		}

		pdo = pdu->u.hdr.pdo;
		padding_len = pdo - hlen;
		if (tqpair->host_ddgst_enable) {
			padding_len -= NVMF_TCP_DIGEST_LEN;
		}

		if (padding_len > 0 && pdu->padding_valid_bytes < padding_len) {
			/* reuse the space */
			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   padding_len - pdu->padding_valid_bytes,
							   (uint8_t *)pdu + hlen + pdu->padding_valid_bytes);
			if (rc < 0) {
				*_pdu = NULL;
				spdk_put_nvmf_tcp_pdu(pdu);
				tqpair->recv_pdu = NULL;
				return rc;
			}

			pdu->padding_valid_bytes += rc;
			if (pdu->padding_valid_bytes < padding_len) {
				*_pdu = NULL;
				return SPDK_NVMF_TCP_PDU_SUCCESS;
			}
		}

		data_len = total_pdu_len - pdo;

		if (tqpair->host_ddgst_enable) {
			data_len -= NVMF_TCP_DIGEST_LEN;
		}

		/* data len */
		if (pdu->data_valid_bytes < data_len) {
			pdu->data = spdk_mempool_get(ttransport->data_buf_pool);
			if (!pdu->data) {
				*_pdu = NULL;
				return SPDK_NVMF_TCP_PDU_SUCCESS;
			}
			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   data_len - pdu->data_valid_bytes,
							   (uint8_t *)pdu->data + pdu->data_valid_bytes);
			if (rc < 0) {
				*_pdu = NULL;
				spdk_put_nvmf_tcp_pdu(pdu);
				tqpair->recv_pdu = NULL;
				return rc;
			}

			pdu->data_valid_bytes += rc;
			if (pdu->data_valid_bytes < data_len) {
				*_pdu = NULL;
				return SPDK_NVMF_TCP_PDU_SUCCESS;
			}
		}

		/* data digest */
		if (tqpair->host_ddgst_enable && pdu->ddigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   NVMF_TCP_DIGEST_LEN - pdu->ddigest_valid_bytes,
							   pdu->data_digest + pdu->ddigest_valid_bytes);
			if (rc < 0) {
				*_pdu = NULL;
				spdk_put_nvmf_tcp_pdu(pdu);
				tqpair->recv_pdu = NULL;
				return rc;
			}

			pdu->ddigest_valid_bytes += rc;
			if (pdu->ddigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
				*_pdu = NULL;
				return SPDK_NVMF_TCP_PDU_SUCCESS;
			}
		}
	}

end:
	*_pdu = pdu;
	return 1;
}

static void assocate

static void
_spdk_nvmf_tcp_send_icresp_complete(struct spdk_nvmf_tcp_qpair *tqpair, void *cb_arg)
{
	tqpair->state = NVMF_TCP_QPAIR_STATE_RUNNING;
}

static int
spdk_nvmf_tcp_icreq_handle(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_req *ic_req = &pdu->u.ic_req;
	struct spdk_nvmf_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_ic_resp *ic_resp;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Got PDU header\n");

	if (ic_req->hdr.length != sizeof(struct spdk_nvme_tcp_ic_req)) {
		SPDK_ERRLOG("Expected ICReq PDU length %zu, got %u\n",
			    sizeof(struct spdk_nvme_tcp_ic_req), ic_req->hdr.length);
		return SPDK_NVMF_TCP_CONNECTION_FATAL;
	}

	/* Only PFV 0 is defined currently */
	if (ic_req->pfv != 0) {
		SPDK_ERRLOG("Expected ICReq PFV %u, got %u\n", 0u, ic_req->pfv);
		return SPDK_NVMF_TCP_CONNECTION_FATAL;
	}

	/* MAXR2T is 0's based */
	tqpair->maxr2t = ic_req->maxr2t + 1ull;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "maxr2t = %" PRIu64 "\n", tqpair->maxr2t);

	tqpair->host_hdgst_enable = ic_req->dgst.bits.hdgst_enable;
	tqpair->host_ddgst_enable = ic_req->dgst.bits.ddgst_enable;
	tqpair->cpda = spdk_min(ic_req->hpda, NVMF_TCP_CPDA_MAX);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	rsp_pdu = spdk_get_nvmf_tcp_pdu(tqpair);
	if (rsp_pdu == NULL) {
		return SPDK_NVMF_TCP_CONNECTION_FATAL;
	}

	ic_resp = &rsp_pdu->u.ic_resp;
	ic_resp->hdr.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	ic_resp->hdr.length = sizeof(*ic_resp);
	ic_resp->pfv = 0;
	ic_resp->cpda = tqpair->cpda;
	ic_resp->maxh2cdata = 4096; // TODO - new nvmf_tgt config option? bdev large buffer size?
	ic_resp->dgst.bits.hdgst_enable = 1; // TODO: nvmf_tgt option
	ic_resp->dgst.bits.ddgst_enable = 1; // TODO: nvmf_tgt option

	_spdk_nvmf_tcp_qpair_send(tqpair, pdu, _spdk_nvmf_tcp_send_icresp_complete, ic_resp);

	return 0;
}

struct spdk_nvmf_tcp_request *
spdk_nvmf_tcp_req_get(struct spdk_nvmf_tcp_qpair *tqpair)
{	
	struct spdk_nvmf_tcp_request *tcp_req;
	
	tcp_req = TAILQ_FIRST(&tqpair->free_reqs);
	if (tcp_req) {
		TAILQ_REMOVE(&tqpair->free_reqs, tcp_req, link);
		TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);
	}

	return tcp_req;
}

static void
spdk_nvmf_tcp_req_put(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_request *tcp_req)
{
	TAILQ_REMOVE(&tqpair->free_reqs, tcp_req, link);
	TAILQ_INSERT_HEAD(&tqpair->outstanding_reqs, tcp_req, link);
}

static int
spdk_nvmf_tcp_capsule_handle(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_pdu *pdu)
{	
	struct spdk_nvmf_tcp_request *tcp_req;

	tcp_req = 


	rdma_recv = rdma_req->recv;

	/* The first element of the SGL is the NVMe command */
	rdma_req->req.cmd = (union nvmf_h2c_msg *)rdma_recv->sgl[0].addr;
	memset(rdma_req->req.rsp, 0, sizeof(*rdma_req->req.rsp));

	TAILQ_REMOVE(&rqpair->incoming_queue, rdma_recv, link);

	if (rqpair->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
		spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
		break;
	}

	if (rqpair->ibv_attr.qp_state == IBV_QPS_ERR) {
		spdk_nvmf_rdma_request_set_state(rdma_req, RDMA_REQUEST_STATE_COMPLETED);
		break;
	}

	return 0;
}

static int
spdk_nvmf_tcp_execute(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_pdu *pdu)
{
	int rc = 0;

	if (tqpair->state != NVMF_TCP_QPAIR_STATE_RUNNING &&
	    pdu->u.hdr.pdu_type != SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		return SPDK_NVMF_TCP_CONNECTION_FATAL;
	}

	switch (pdu->u.hdr.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_REQ:
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_INVALID) {
			return SPDK_NVMF_TCP_CONNECTION_FATAL;
		}
		rc = spdk_nvmf_tcp_icreq_handle(tqpair, pdu);

		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		rc = spdk_nvmf_tcp_capsule_handle(tqpair, pdu);
		
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		break;
	case SPDK_NVME_TCP_PDU_TYPE_TERM_REQ:

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->u.hdr.pdu_type);
		break;
	}

	return rc;
}

#define GET_NVMF_PDU_LOOP_COUNT	16

static int
spdk_nvmf_tcp_qpair_recv(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_pdu *pdu;
	int i, rc;

	/* Read new PDUs from network */
	for (i = 0; i < GET_NVMF_PDU_LOOP_COUNT; i++) {
		rc = spdk_nvmf_tcp_read_pdu(tqpair, &pdu);
		if (rc == 0) {
			break;
		} else if (rc == SPDK_NVMF_TCP_CONNECTION_FATAL) {
			return rc;
		}

		rc = spdk_nvmf_tcp_execute(tqpair, pdu);
		spdk_put_nvmf_tcp_pdu(pdu);
		if (rc != 0) {
			SPDK_ERRLOG("spdk_nvmf_tcp_execute() fatal error on tpqair=%p\n", tqpair);
			return rc;
		}
	}

	return i;
}

static void
spdk_nvmf_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;
	int rc;

	rc = spdk_nvmf_tcp_qpair_recv(tqpair);
	if (rc < 0) {
		// SHOULD do some work to recyle the tqpair
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

	TAILQ_INSERT_TAIL(&tgroup->qpairs, tqpair, link);

	return 0;
}

static int
spdk_nvmf_tcp_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_transport	*ttransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_tcp_transport, transport);
	struct spdk_nvmf_tcp_request	*tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);

	return 0;
}

static void
spdk_nvmf_tcp_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_poll_group	*tgroup;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	int				rc;

	tgroup = SPDK_CONTAINEROF(qpair->group, struct spdk_nvmf_tcp_poll_group, group);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	if (tgroup) {
		TAILQ_REMOVE(&tgroup->qpairs, tqpair, link);

		rc = spdk_sock_group_remove_sock(tgroup->sock_group, tqpair->sock);
		if (rc != 0) {
			SPDK_ERRLOG("Could not remove sock from sock_group: %s (%d)\n",
				    spdk_strerror(errno), errno);
		}
	}
	
	spdk_nvmf_tcp_qpair_destroy(SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair));
}

#if 0
static void
spdk_nvmf_tcp_qpair_process_pending(struct spdk_nvmf_tcp_transport *ttransport,
				    struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_recv	*tcp_recv, *recv_tmp;
	struct spdk_nvmf_tcp_request	*tcp_req, *req_tmp;

	/* We process I/O in the pending_tcp_rw queue at the highest priority. */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->pending_tcp_rw_queue, link, req_tmp) {
		if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
			break;
		}
	}

	/* The second highest priority is I/O waiting on memory buffers. */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->ch->pending_data_buf_queue, link, req_tmp) {
		if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
			break;
		}
	}

	/* The lowest priority is processing newly received commands */
	TAILQ_FOREACH_SAFE(tcp_recv, &tqpair->incoming_queue, link, recv_tmp) {
		tcp_req = TAILQ_FIRST(&tqpair->free_reqs);
		if (tcp_req == NULL) {
			/* Need to wait for more SEND completions */
			break;
		}

		tcp_req->recv = tcp_recv;
		tcp_req->state = TCP_REQUEST_STATE_NEW;
		if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
			break;
		}
	}
}
#endif


#if 0
static int
spdk_nvmf_tcp_poller_poll(struct spdk_nvmf_tcp_transport *ttransport,
			  struct spdk_nvmf_tcp_poller *rpoller)
{
	struct ibv_wc wc[32];
	struct spdk_nvmf_tcp_request	*tcp_req;
	struct spdk_nvmf_tcp_recv	*tcp_recv;
	struct spdk_nvmf_tcp_qpair	*tqpair;
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
		if (wc[i].status) {
			SPDK_ERRLOG("CQ error on CQ %p, Request 0x%lu (%d): %s\n",
				    rpoller->cq, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));
			error = true;
			continue;
		}

		switch (wc[i].opcode) {
		case IBV_WC_SEND:
			tcp_req = get_tcp_req_from_wc(&wc[i]);
			tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

			assert(tcp_req->state == TCP_REQUEST_STATE_COMPLETING);
			tcp_req->state = TCP_REQUEST_STATE_COMPLETED;

			spdk_nvmf_tcp_request_process(ttransport, tcp_req);

			count++;

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
			break;

		case IBV_WC_TCP_WRITE:
			tcp_req = get_tcp_req_from_wc(&wc[i]);
			tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

			tqpair->cur_tcp_rw_depth--;

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
			break;

		case IBV_WC_TCP_READ:
			tcp_req = get_tcp_req_from_wc(&wc[i]);
			tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);

			assert(tcp_req->state == TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
			tqpair->cur_tcp_rw_depth--;
			tcp_req->state = TCP_REQUEST_STATE_READY_TO_EXECUTE;

			spdk_nvmf_tcp_request_process(ttransport, tcp_req);

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
			break;

		case IBV_WC_RECV:
			tcp_recv = get_tcp_recv_from_wc(&wc[i]);
			tqpair = tcp_recv->qpair;

			TAILQ_INSERT_TAIL(&tqpair->incoming_queue, tcp_recv, link);

			/* Try to process other queued requests */
			spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);
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
#endif

static int
spdk_nvmf_tcp_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;
	struct spdk_nvmf_tcp_qpair *tqpair, *tmp;
	int rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_tcp_poll_group, group);

	if (spdk_unlikely(TAILQ_EMPTY(&tgroup->qpairs))) {
		return 0;
	}

	rc = spdk_sock_group_poll(tgroup->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", tgroup->sock_group);
		return rc;
	}

	TAILQ_FOREACH_SAFE(tqpair, &tgroup->qpairs, link, tmp) {
		if (tqpair->state == NVMF_TCP_QPAIR_STATE_EXITING) {
			spdk_nvmf_qpair_disconnect(&tqpair->qpair, NULL, NULL);
		}
	}

	return 0;
}

static bool
spdk_nvmf_tcp_qpair_is_idle(struct spdk_nvmf_qpair *qpair)
{
	return false;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_tcp = {
	.type = SPDK_NVME_TRANSPORT_TCP,
	.create = spdk_nvmf_tcp_create,
	.destroy = spdk_nvmf_tcp_destroy,

	.listen = spdk_nvmf_tcp_listen,
	.stop_listen = spdk_nvmf_tcp_stop_listen,
	.accept = spdk_nvmf_tcp_accept,

	.listener_discover = spdk_nvmf_tcp_discover,

	.poll_group_create = spdk_nvmf_tcp_poll_group_create,
	.poll_group_destroy = spdk_nvmf_tcp_poll_group_destroy,
	.poll_group_add = spdk_nvmf_tcp_poll_group_add,
	.poll_group_poll = spdk_nvmf_tcp_poll_group_poll,

	.req_complete = spdk_nvmf_tcp_request_complete,

	.qpair_fini = spdk_nvmf_tcp_close_qpair,
	.qpair_is_idle = spdk_nvmf_tcp_qpair_is_idle,
};

SPDK_LOG_REGISTER_COMPONENT("nvmf_tcp", SPDK_LOG_NVMF_TCP)
