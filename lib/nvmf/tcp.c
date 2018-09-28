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
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk_internal/log.h"

/*
 * AIO backend requires block size aligned data buffers,
 * extra 4KiB aligned data buffer should work for most devices.
 */
#define SHIFT_4KB			12u
#define NVMF_DATA_BUFFER_ALIGNMENT	(1u << SHIFT_4KB)
#define NVMF_DATA_BUFFER_MASK		(NVMF_DATA_BUFFER_ALIGNMENT - 1LL)
#define NVMF_TCP_MAX_ACCEPT_SOCK_ONE_TIME 16

#define NVMF_TCP_CH_LEN sizeof(struct spdk_nvme_tcp_common_pdu_hdr)
#define NVMF_TCP_PSH_LEN_MAX 120
#define NVMF_TCP_DIGEST_LEN 4
#define NVMF_TCP_CPDA_MAX 31
#define NVMF_TCP_ALIGNMENT 4
#define NVMF_TCP_MAX_ERROR_DATA_SIZE 128

#define NVMF_TCP_TERM_REQ_PDU_MAX_SIZE  152
#define NVMF_TCP_PDU_PDO_MAX_OFFSET	((NVMF_TCP_CPDA_MAX + 1) << 2)
#define NVMF_TCP_PDU_MAX_H2C_DATA_SIZE	131072
#define NVMF_TCP_PDU_MAX_C2H_DATA_SIZE	131072
#define NVMF_TCP_QPAIR_EXIT_TIMEOUT	30
#define NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM  64  /* Maximal c2h_data pdu number for ecah tqpair */

#define MAX_ADDRBUF 64
#define MAX_INITIATOR_ADDR (MAX_ADDRBUF)
#define MAX_TARGET_ADDR (MAX_ADDRBUF)

/* THe following security functions  shoudl put in general */
#define SPDK_CRC32C_XOR        0xffffffffUL

#define MAKE_DIGEST_WORD(BUF, CRC32C) \
	(   ((*((uint8_t *)(BUF)+0)) = (uint8_t)((uint32_t)(CRC32C) >> 0)), \
	    ((*((uint8_t *)(BUF)+1)) = (uint8_t)((uint32_t)(CRC32C) >> 8)), \
	    ((*((uint8_t *)(BUF)+2)) = (uint8_t)((uint32_t)(CRC32C) >> 16)), \
	    ((*((uint8_t *)(BUF)+3)) = (uint8_t)((uint32_t)(CRC32C) >> 24)))

#define MATCH_DIGEST_WORD(BUF, CRC32C) \
	(    ((((uint32_t) *((uint8_t *)(BUF)+0)) << 0)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+1)) << 8)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+2)) << 16)	\
	    | (((uint32_t) *((uint8_t *)(BUF)+3)) << 24))	\
	    == (CRC32C))

#define DGET32(B)								\
	(((  (uint32_t) *((uint8_t *)(B)+0)) << 0)				\
	 | (((uint32_t) *((uint8_t *)(B)+1)) << 8)				\
	 | (((uint32_t) *((uint8_t *)(B)+2)) << 16)				\
	 | (((uint32_t) *((uint8_t *)(B)+3)) << 24))

#define DSET32(B,D)								\
	(((*((uint8_t *)(B)+0)) = (uint8_t)((uint32_t)(D) >> 0)),		\
	 ((*((uint8_t *)(B)+1)) = (uint8_t)((uint32_t)(D) >> 8)),		\
	 ((*((uint8_t *)(B)+2)) = (uint8_t)((uint32_t)(D) >> 16)),		\
	 ((*((uint8_t *)(B)+3)) = (uint8_t)((uint32_t)(D) >> 24)))

#define  LINUX_KERNEL_SUPPORT_NOT_SENDING_RESP_FOR_C2H 0

/* spdk nvmf related structure */
enum spdk_nvmf_tcp_request_state {

	/* The request is not currently in use */
	TCP_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	TCP_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	TCP_REQUEST_STATE_NEED_BUFFER,

	/* The request is pending on r2t slots */
	TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T,

	/* The request is currently transferring data from the host to the controller. */
	TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

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

enum spdk_nvmf_tcp_pdu_recv_state {
	/* Ready to wait to wait PDU */
	TCP_PDU_RECV_STATE_AWAIT_PDU_READY,

	/* Active tqpair waiting for any PDU common header */
	TCP_PDU_RECV_STATE_AWAIT_PDU_CH,

	/* Active tqpair waiting for any PDU specific header */
	TCP_PDU_RECV_STATE_AWAIT_PDU_PSH,

	/* Active tqpair waiting for payload */
	TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD,

	/* Active tqpair in an error state, and it will not accept new pdu */
	TCP_PDU_RECV_STATE_ERROR,
};

enum nvmf_tcp_qpair_state {
	NVMF_TCP_QPAIR_STATE_INVALID = 0,
	NVMF_TCP_QPAIR_STATE_RUNNING = 1,
	NVMF_TCP_QPAIR_STATE_EXITING = 3,
	NVMF_TCP_QPAIR_STATE_EXITED = 4,
};

enum spdk_nvmf_tcp_error_codes {
	SPDK_NVMF_TCP_PDU_IN_PROGRESS	= 0,
	SPDK_NVMF_TCP_CONNECTION_FATAL	= -1,
};

static const char *spdk_nvme_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digiest Error",
	"Data Transfer Out of Range",
	"R2T Limit Exceeded",
	"Unsupported parameter",
};

#define OBJECT_NVMF_TCP_IO				0x50

#define									TRACE_GROUP_NVMF_TCP 0x5
#define TRACE_TCP_REQUEST_STATE_NEW					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0)
#define TRACE_TCP_REQUEST_STATE_NEED_BUFFER				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x1)
#define TRACE_TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x2)
#define TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x3)
#define TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x4)
#define TRACE_TCP_REQUEST_STATE_EXECUTING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x5)
#define TRACE_TCP_REQUEST_STATE_EXECUTED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x6)
#define TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x7)
#define TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x8)
#define TRACE_TCP_REQUEST_STATE_COMPLETED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x9)

SPDK_TRACE_REGISTER_FN(nvmf_tcp_trace)
{
	spdk_trace_register_object(OBJECT_NVMF_TCP_IO, 'r');
	spdk_trace_register_description("TCP_REQ_NEW", "",
					TRACE_TCP_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 1, 0, "");
	spdk_trace_register_description("TCP_REQ_NEED_BUFFER", "",
					TRACE_TCP_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_TX_PENDING_R2T", "",
					TRACE_TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_TX_H_TO_C", "",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_EXECUTE", "",
					TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_EXECUTING", "",
					TRACE_TCP_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_EXECUTED", "",
					TRACE_TCP_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_RDY_TO_COMPLETE", "",
					TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_COMPLETING_INCAPSULE", "",
					TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
	spdk_trace_register_description("TCP_REQ_COMPLETED", "",
					TRACE_TCP_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_TCP_IO, 0, 0, "");
}

struct spdk_nvmf_tcp_qpair;
struct spdk_nvmf_tcp_request;
typedef void (*spdk_nvmf_tcp_qpair_xfer_complete_cb)(void *cb_arg);

struct spdk_nvmf_tcp_pdu {
	union {
		/* to hold error pdu data */
		uint8_t					raw[NVMF_TCP_TERM_REQ_PDU_MAX_SIZE];
		struct spdk_nvme_tcp_common_pdu_hdr	common;
		struct spdk_nvme_tcp_ic_req		ic_req;
		struct spdk_nvme_tcp_term_req_hdr	term_req;
		struct spdk_nvme_tcp_cmd		capsule_cmd;
		struct spdk_nvme_tcp_h2c_data_hdr	h2c_data;
		struct spdk_nvme_tcp_ic_resp		ic_resp;
		struct spdk_nvme_tcp_rsp		capsule_resp;
		struct spdk_nvme_tcp_c2h_data_hdr	c2h_data;
		struct spdk_nvme_tcp_r2t_hdr		r2t;

	} hdr;

	bool						has_hdgst;
	uint8_t						data_digest[NVMF_TCP_DIGEST_LEN];
	int32_t						padding_valid_bytes;
	int32_t						ddigest_valid_bytes;

	uint32_t					ch_valid_bytes;
	uint32_t					psh_valid_bytes;
	bool						hd_is_read;
	uint32_t					data_valid_bytes;

	spdk_nvmf_tcp_qpair_xfer_complete_cb		cb_fn;
	void						*cb_arg;
	int						ref;
	void						*data;
	uint32_t					data_len;
	struct spdk_nvmf_tcp_qpair			*tqpair;

	struct spdk_nvmf_tcp_request			*tcp_req; /* data tied to a tcp request */
	uint32_t					writev_offset;
	TAILQ_ENTRY(spdk_nvmf_tcp_pdu)			tailq;
	uint32_t					remaining;
	uint32_t					padding_len;
};

struct spdk_nvmf_tcp_request  {
	struct spdk_nvmf_request		req;
	struct spdk_nvme_cpl			rsp;
	struct spdk_nvme_cmd			cmd;

	/* In-capsule data buffer */
	uint8_t					*buf;

	bool					data_from_pool;
	void					*buffers[SPDK_NVMF_MAX_SGL_ENTRIES];

	/* transfer_tag */
	uint16_t				ttag;

	/*
	 * next_expected_r2t_offset is used when we receive the h2c_data PDU.
	 */
	uint32_t				next_expected_r2t_offset;

	/*
	 * c2h_data_offset is used when we send the c2h_data PDU.
	 */
	uint32_t				c2h_data_offset;
	uint32_t				c2h_data_pdu_num;

	enum spdk_nvmf_tcp_request_state	state;

	TAILQ_ENTRY(spdk_nvmf_tcp_request)	link;
	TAILQ_ENTRY(spdk_nvmf_tcp_request)	state_link;
};

struct spdk_nvmf_tcp_qpair {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_tcp_port		*port;
	struct spdk_sock			*sock;
	struct spdk_poller			*flush_poller;

	enum spdk_nvmf_tcp_pdu_recv_state	recv_state;
	enum nvmf_tcp_qpair_state		state;

	struct spdk_nvmf_tcp_pdu		pdu_in_progress;

	TAILQ_HEAD(, spdk_nvmf_tcp_pdu)		send_queue;
	TAILQ_HEAD(, spdk_nvmf_tcp_pdu)		free_queue;

	struct spdk_nvmf_tcp_pdu		*pdu;
	struct spdk_nvmf_tcp_pdu		*pdu_pool;
	uint16_t				free_pdu_num;

	/* Queues to track the requests in all states */
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	state_queue[TCP_REQUEST_NUM_STATES];
	/* Number of requests in each state */
	int32_t					state_cntr[TCP_REQUEST_NUM_STATES];

	uint32_t				maxr2t;
	uint32_t				pending_r2t;
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	queued_r2t_tcp_req;
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	queued_c2h_data_tcp_req;

	uint8_t					cpda;

	/* Array of size "max_queue_depth * InCapsuleDataSize" containing
	 * buffers to be used for in capsule data.
	 */
	void					*buf;
	void					*bufs;
	struct spdk_nvmf_tcp_request		*req;
	struct spdk_nvmf_tcp_request		*reqs;

	bool					host_hdgst_enable;
	bool					host_ddgst_enable;


	/* The maximum number of I/O outstanding on this connection at one time */
	uint16_t				max_queue_depth;


	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint32_t				maxh2cdata;

	/* Timer used to destroy qpair after detecting transport error issue if initiator does
	 *  not close the connection.
	 */
	uint64_t				last_pdu_time;
	int					timeout;

	/* Mgmt channel */
	struct spdk_io_channel			*mgmt_channel;
	struct spdk_nvmf_tcp_mgmt_channel	*ch;

	uint32_t				c2h_data_pdu_cnt;

	/* IP address */
	char initiator_addr[MAX_INITIATOR_ADDR];
	char target_addr[MAX_TARGET_ADDR];

	/* qpair->group is freed early, this should be a temporal fix */
	struct spdk_nvmf_tcp_poll_group		*tgroup;
	TAILQ_ENTRY(spdk_nvmf_tcp_qpair)	link;
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

	uint16_t				max_queue_depth;
	uint32_t				max_io_size;
	uint32_t				io_unit_size;
	uint32_t				in_capsule_data_size;

	TAILQ_HEAD(, spdk_nvmf_tcp_port)	ports;
};

struct spdk_nvmf_tcp_mgmt_channel {
	/* Requests that are waiting to obtain a data buffer */
	TAILQ_HEAD(, spdk_nvmf_tcp_request)	pending_data_buf_queue;
};

static bool spdk_nvmf_tcp_request_process(struct spdk_nvmf_tcp_transport *ttransport,
		struct spdk_nvmf_tcp_request *tcp_req);
static void spdk_nvmf_tcp_handle_pending_c2h_data_queue(struct spdk_nvmf_tcp_qpair *tqpair);

static uint32_t
spdk_nvmf_tcp_pdu_calc_header_digest(struct spdk_nvmf_tcp_pdu *pdu)
{
	uint32_t crc32c;
	uint32_t hlen = pdu->hdr.common.hlen;

	crc32c = spdk_crc32_ieee_update(&pdu->hdr.raw, hlen, ~0);
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static uint32_t
spdk_nvmf_tcp_pdu_calc_data_digest(struct spdk_nvmf_tcp_pdu *pdu)
{
	uint32_t crc32c;
	uint32_t mod;

	assert(pdu->data != NULL);
	assert(pdu->data_len != 0);

	crc32c = spdk_crc32_ieee_update(pdu->data, pdu->data_len, ~0);

	mod = pdu->data_len % NVMF_TCP_ALIGNMENT;
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

static void
spdk_nvmf_tcp_request_set_state(struct spdk_nvmf_tcp_request *tcp_req,
				enum spdk_nvmf_tcp_request_state state)
{
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_tcp_qpair *tqpair;

	qpair = tcp_req->req.qpair;
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	TAILQ_REMOVE(&tqpair->state_queue[tcp_req->state], tcp_req, state_link);
	tqpair->state_cntr[tcp_req->state]--;
	assert(tqpair->state_cntr[tcp_req->state] >= 0);

	TAILQ_INSERT_TAIL(&tqpair->state_queue[state], tcp_req, state_link);
	tqpair->state_cntr[state]++;

	tcp_req->state = state;
}

static struct spdk_nvmf_tcp_pdu *
spdk_nvmf_tcp_pdu_get(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_pdu *pdu;

	pdu = TAILQ_FIRST(&tqpair->free_queue);
	if (!pdu) {
		SPDK_ERRLOG("Unable to get PDU for tqpair=%p\n", tqpair);
		abort();
		return NULL;
	}

	tqpair->free_pdu_num--;
	TAILQ_REMOVE(&tqpair->free_queue, pdu, tailq);
	memset(pdu, 0, sizeof(*pdu));
	pdu->ref = 1;
	pdu->tqpair = tqpair;

	return pdu;
}

static void
spdk_nvmf_tcp_pdu_put(struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_qpair *tqpair;
	if (!pdu) {
		return;
	}

	assert(pdu->ref > 0);
	assert(pdu->tqpair != NULL);
	tqpair = pdu->tqpair;

	pdu->ref--;
	if (pdu->ref == 0) {
		tqpair->free_pdu_num++;
		TAILQ_INSERT_HEAD(&tqpair->free_queue, pdu, tailq);
	}
}

static struct spdk_nvmf_tcp_request *
spdk_nvmf_tcp_req_get(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_request *tcp_req;

	tcp_req = TAILQ_FIRST(&tqpair->state_queue[TCP_REQUEST_STATE_FREE]);
	if (!tcp_req) {
		SPDK_ERRLOG("Cannot allocate tcp_req on tqpair=%p\n", tqpair);
		return NULL;
	}

	memset(&tcp_req->cmd, 0, sizeof(tcp_req->cmd));
	memset(&tcp_req->rsp, 0, sizeof(tcp_req->rsp));
	tcp_req->next_expected_r2t_offset = 0;
	tcp_req->c2h_data_offset = 0;

	spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_NEW);
	return tcp_req;
}

static void
nvmf_tcp_request_free(struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvmf_tcp_transport *ttransport;

	if (!tcp_req) {
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tcp_req=%p will be freed\n", tcp_req);
	ttransport = SPDK_CONTAINEROF(tcp_req->req.qpair->transport,
				      struct spdk_nvmf_tcp_transport, transport);
	spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_COMPLETED);
	spdk_nvmf_tcp_request_process(ttransport, tcp_req);
}

static int
spdk_nvmf_tcp_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_request *tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);

	nvmf_tcp_request_free(tcp_req);

	return 0;
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
spdk_nvmf_tcp_drain_state_queue(struct spdk_nvmf_tcp_qpair *tqpair,
				enum spdk_nvmf_tcp_request_state state)
{
	struct spdk_nvmf_tcp_request *tcp_req, *req_tmp;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->state_queue[state], state_link, req_tmp) {
		nvmf_tcp_request_free(tcp_req);
	}
}

static void
spdk_nvmf_tcp_cleanup_all_states(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_request *tcp_req, *req_tmp;
	struct spdk_nvmf_tcp_pdu *pdu, *tmp_pdu;

	/* For the requests in TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,
	 * they can be refreed via the pdu in the send_queue
	  */
	TAILQ_FOREACH_SAFE(pdu, &tqpair->send_queue, tailq, tmp_pdu) {
		TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
		assert(pdu->cb_fn != NULL);
		pdu->cb_fn(pdu->cb_arg);
		spdk_nvmf_tcp_pdu_put(pdu);
	}

	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEW);

	/* Wipe the requests waiting for R2t  */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->state_queue[TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T], link,
			   req_tmp) {
		TAILQ_REMOVE(&tqpair->queued_r2t_tcp_req, tcp_req, link);
	}
	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T);

	/* Wipe the requests waiting for buffer from the global list */
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->state_queue[TCP_REQUEST_STATE_NEED_BUFFER], link, req_tmp) {
		TAILQ_REMOVE(&tqpair->ch->pending_data_buf_queue, tcp_req, link);
	}

	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_NEED_BUFFER);
	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_EXECUTING);

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->queued_c2h_data_tcp_req, link, req_tmp) {
		TAILQ_REMOVE(&tqpair->queued_c2h_data_tcp_req, tcp_req, link);
	}

	spdk_nvmf_tcp_drain_state_queue(tqpair, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
}

static void
spdk_nvmf_tcp_qpair_destroy(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_poll_group *tgroup;
	int                             rc;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");
	tgroup = tqpair->tgroup;

	if (tgroup) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "remove tqpair=%p from the tgroup=%p\n", tqpair, tgroup);
		TAILQ_REMOVE(&tgroup->qpairs, tqpair, link);
		rc = spdk_sock_group_remove_sock(tgroup->sock_group, tqpair->sock);
		if (rc != 0) {
			SPDK_ERRLOG("Could not remove sock from sock_group: %s (%d)\n",
				    spdk_strerror(errno), errno);
		}
	}

	spdk_poller_unregister(&tqpair->flush_poller);
	spdk_sock_close(&tqpair->sock);
	spdk_nvmf_tcp_cleanup_all_states(tqpair);
	if (tqpair->mgmt_channel) {
		spdk_put_io_channel(tqpair->mgmt_channel);
	}

	if (tqpair->free_pdu_num != (tqpair->max_queue_depth + NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM)) {
		SPDK_ERRLOG("tqpair(%p) free pdu pool num is %u but should be %u\n", tqpair,
			    tqpair->free_pdu_num,
			    (tqpair->max_queue_depth + NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM));
	}

	if (tqpair->state_cntr[TCP_REQUEST_STATE_FREE] != tqpair->max_queue_depth) {
		SPDK_ERRLOG("tqpair(%p) free tcp request num is %u but should be %u\n", tqpair,
			    tqpair->state_cntr[TCP_REQUEST_STATE_FREE],
			    tqpair->max_queue_depth);
	}

	if (tqpair->c2h_data_pdu_cnt != 0) {
		SPDK_ERRLOG("tqpair(%p) free c2h_data_pdu cnt is %u but should be 0\n", tqpair,
			    tqpair->c2h_data_pdu_cnt);
	}

	free(tqpair->pdu);
	free(tqpair->pdu_pool);
	free(tqpair->req);
	free(tqpair->reqs);
	spdk_dma_free(tqpair->buf);
	spdk_dma_free(tqpair->bufs);
	free(tqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Leave\n");
}

static struct spdk_nvmf_transport *
spdk_nvmf_tcp_create(struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	uint32_t			sge_count;

	ttransport = calloc(1, sizeof(*ttransport));
	if (!ttransport) {
		return NULL;
	}

	pthread_mutex_init(&ttransport->lock, NULL);
	TAILQ_INIT(&ttransport->ports);

	ttransport->transport.ops = &spdk_nvmf_transport_tcp;

	SPDK_NOTICELOG("*** TCP Transport Init ***\n");

	ttransport->max_queue_depth = opts->max_queue_depth;
	ttransport->max_io_size = opts->max_io_size;
	ttransport->in_capsule_data_size = opts->in_capsule_data_size;
	ttransport->io_unit_size = opts->io_unit_size;

	/* I/O unit size cannot be larger than max I/O size */
	if (ttransport->io_unit_size > ttransport->max_io_size) {
		ttransport->io_unit_size = ttransport->max_io_size;
	}

	sge_count = ttransport->max_io_size / ttransport->io_unit_size;
	if (sge_count > SPDK_NVMF_MAX_SGL_ENTRIES) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", ttransport->io_unit_size);
		free(ttransport);
		return NULL;
	}

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

	spdk_io_device_register(ttransport, spdk_nvmf_tcp_mgmt_channel_create,
				spdk_nvmf_tcp_mgmt_channel_destroy,
				sizeof(struct spdk_nvmf_tcp_mgmt_channel), "tcp_transport");

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

	spdk_mempool_free(ttransport->data_buf_pool);
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
	canon_trid->trtype = SPDK_NVME_TRANSPORT_TCP;
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
		free(port);
		pthread_mutex_unlock(&ttransport->lock);
		return -ENOMEM;
	}

	port->ref = 1;

	if (_spdk_nvmf_tcp_canon_listen_trid(&port->trid, trid) != 0) {
		SPDK_ERRLOG("Invalid traddr %s / trsvcid %s\n",
			    trid->traddr, trid->trsvcid);
		free(port);
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

	hlen = pdu->hdr.common.hlen;
	enable_digest = 1;
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	/* Header Digest */
	if (enable_digest && tqpair->host_hdgst_enable) {
		crc32c = spdk_nvmf_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr.raw + hlen, crc32c);
		hlen += NVMF_TCP_DIGEST_LEN;
	}

	/* PDU header + possible digest */
	iovec[iovec_cnt].iov_base = &pdu->hdr.raw;
	iovec[iovec_cnt].iov_len = hlen;
	iovec_cnt++;

	if (!pdu->data_len || !pdu->data) {
		return iovec_cnt;
	}

	/* Padding */
	if (pdu->padding_len > 0) {
		iovec[iovec_cnt - 1].iov_len += pdu->padding_len;
	}

	/* Data Segment */
	iovec[iovec_cnt].iov_base = pdu->data;
	iovec[iovec_cnt].iov_len = pdu->data_len;
	iovec_cnt++;

	/* Data Digest */
	if (enable_digest && tqpair->host_ddgst_enable) {
		crc32c = spdk_nvmf_tcp_pdu_calc_data_digest(pdu);
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
		iovec[iovec_cnt].iov_base = pdu->data_digest;
		iovec[iovec_cnt].iov_len = NVMF_TCP_DIGEST_LEN;
		iovec_cnt++;
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
	uint32_t writev_offset;
	struct spdk_nvmf_tcp_pdu *pdu;
	int pdu_length;

	pdu = TAILQ_FIRST(&tqpair->send_queue);

	if (pdu == NULL) {
		return 0;
	}

	/*
	 * Build up a list of iovecs for the first few PDUs in the
	 *  tqpair 's send_queue.
	 */
	while (pdu != NULL && ((array_size - iovec_cnt) >= 3)) {
		iovec_cnt += spdk_nvmf_tcp_build_iovecs(tqpair,
							&iovec_array[iovec_cnt],
							pdu);
		pdu = TAILQ_NEXT(pdu, tailq);
	}

	/*
	 * Check if the first PDU was partially written out the last time
	 *  this function was called, and if so adjust the iovec array
	 *  accordingly.
	 */
	writev_offset = TAILQ_FIRST(&tqpair->send_queue)->writev_offset;
	while ((writev_offset > 0) && (iovec_cnt > 0)) {
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
		pdu_length = pdu->hdr.common.plen - pdu->writev_offset;
		if (bytes >= pdu_length) {
			bytes -= pdu_length;
			TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
			assert(pdu->cb_fn != NULL);
			pdu->cb_fn(pdu->cb_arg);
			spdk_nvmf_tcp_pdu_put(pdu);
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
spdk_nvmf_tcp_qpair_write_pdu(struct spdk_nvmf_tcp_qpair *tqpair,
			      struct spdk_nvmf_tcp_pdu *pdu,
			      spdk_nvmf_tcp_qpair_xfer_complete_cb cb_fn,
			      void *cb_arg)
{
	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	spdk_nvmf_tcp_qpair_flush_pdus(tqpair);

}

static int
spdk_nvmf_tcp_qpair_init_mem_resource(struct spdk_nvmf_tcp_qpair *tqpair, uint16_t size)
{
	int i;
	struct spdk_nvmf_tcp_request *tcp_req;
	struct spdk_nvmf_tcp_transport *ttransport;
	ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);

	if (!tqpair->qpair.sq_head_max) {
		tqpair->req = calloc(1, sizeof(*tqpair->req));
		if (!tqpair->req) {
			SPDK_ERRLOG("Unable to allocate req on tqpair=%p.\n", tqpair);
			return -1;
		}

		if (ttransport->in_capsule_data_size) {
			tqpair->buf = spdk_dma_zmalloc(ttransport->in_capsule_data_size, 0x1000, NULL);
			if (!tqpair->buf) {
				SPDK_ERRLOG("Unable to allocate buf on tqpair=%p.\n", tqpair);
				return -1;
			}
		}

		tcp_req = tqpair->req;
		tcp_req->ttag = 0;
		tcp_req->req.qpair = &tqpair->qpair;

		/* Set up memory to receive commands */
		if (tqpair->buf) {
			tcp_req->buf = tqpair->buf;
		}

		/* Set the cmdn and rsp */
		tcp_req->req.rsp = (union nvmf_c2h_msg *)&tcp_req->rsp;
		tcp_req->req.cmd = (union nvmf_h2c_msg *)&tcp_req->cmd;

		/* Initialize request state to FREE */
		tcp_req->state = TCP_REQUEST_STATE_FREE;
		TAILQ_INSERT_TAIL(&tqpair->state_queue[tcp_req->state], tcp_req, state_link);

		tqpair->pdu = calloc(NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM + 1, sizeof(*tqpair->pdu));
		if (!tqpair->pdu) {
			SPDK_ERRLOG("Unable to allocate pdu on tqpair=%p.\n", tqpair);
			return -1;
		}

		for (i = 0; i < 1 + NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM; i++) {
			TAILQ_INSERT_TAIL(&tqpair->free_queue, &tqpair->pdu[i], tailq);
		}

	} else {
		tqpair->reqs = calloc(size, sizeof(*tqpair->reqs));
		if (!tqpair->reqs) {
			SPDK_ERRLOG("Unable to allocate reqs on tqpair=%p\n", tqpair);
			return -1;
		}

		if (ttransport->in_capsule_data_size) {
			tqpair->bufs = spdk_dma_zmalloc(size * ttransport->in_capsule_data_size,
							0x1000, NULL);
			if (!tqpair->bufs) {
				SPDK_ERRLOG("Unable to allocate bufs on tqpair=%p.\n", tqpair);
				return -1;
			}
		}

		for (i = 0; i < size; i++) {
			struct spdk_nvmf_tcp_request *tcp_req = &tqpair->reqs[i];

			tcp_req->ttag = i + 1;
			tcp_req->req.qpair = &tqpair->qpair;

			/* Set up memory to receive commands */
			if (tqpair->bufs) {
				tcp_req->buf = (void *)((uintptr_t)tqpair->bufs + (i * ttransport->in_capsule_data_size));
			}

			/* Set the cmdn and rsp */
			tcp_req->req.rsp = (union nvmf_c2h_msg *)&tcp_req->rsp;
			tcp_req->req.cmd = (union nvmf_h2c_msg *)&tcp_req->cmd;

			/* Initialize request state to FREE */
			tcp_req->state = TCP_REQUEST_STATE_FREE;
			TAILQ_INSERT_TAIL(&tqpair->state_queue[tcp_req->state], tcp_req, state_link);
		}

		tqpair->pdu_pool = calloc(size, sizeof(*tqpair->pdu_pool));
		if (!tqpair->pdu_pool) {
			SPDK_ERRLOG("Unable to allocate pdu pool on tqpair =%p.\n", tqpair);
			return -1;
		}

		for (i = 0; i < size; i++) {
			TAILQ_INSERT_TAIL(&tqpair->free_queue, &tqpair->pdu_pool[i], tailq);
		}
	}

	return 0;
}

static int
spdk_nvmf_tcp_qpair_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_qpair *tqpair;
	int i;

	ttransport = SPDK_CONTAINEROF(qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New TCP Connection: %p\n", qpair);

	TAILQ_INIT(&tqpair->send_queue);
	TAILQ_INIT(&tqpair->free_queue);
	TAILQ_INIT(&tqpair->queued_r2t_tcp_req);
	TAILQ_INIT(&tqpair->queued_c2h_data_tcp_req);

	/* Initialise request state queues of the qpair */
	for (i = TCP_REQUEST_STATE_FREE; i < TCP_REQUEST_NUM_STATES; i++) {
		TAILQ_INIT(&tqpair->state_queue[i]);
	}

	tqpair->host_hdgst_enable = true;
	tqpair->host_ddgst_enable = true;

	tqpair->mgmt_channel = spdk_get_io_channel(ttransport);
	if (!tqpair->mgmt_channel) {
		return -1;
	}
	tqpair->ch = spdk_io_channel_get_ctx(tqpair->mgmt_channel);
	assert(tqpair->ch != NULL);

	return 0;
}

static int
spdk_nvmf_tcp_qpair_sock_init(struct spdk_nvmf_tcp_qpair *tqpair)
{

	int rc;
	int buf_size;

	/* set recv buffer size */
	buf_size = 2 * 1024 * 1024;
	rc = spdk_sock_set_recvbuf(tqpair->sock, buf_size);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvbuf failed\n");
		return rc;
	}

#if 0
	/* disable it here, it seems not necessary */
	/* set send buffer size */
	rc = spdk_sock_set_sendbuf(tqpair->sock, buf_size);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_sendbuf failed\n");
		return rc;
	}
#endif
	/* set low water mark */
	rc = spdk_sock_set_recvlowat(tqpair->sock, 1);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed\n");
		return rc;
	}

	return 0;
}

static void
_spdk_nvmf_tcp_handle_connect(struct spdk_nvmf_transport *transport,
			      struct spdk_nvmf_tcp_port *port,
			      struct spdk_sock *sock, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "New connection accepted on %s port %s\n",
		      port->trid.traddr, port->trid.trsvcid);

	tqpair = calloc(1, sizeof(struct spdk_nvmf_tcp_qpair));
	if (tqpair == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_sock_close(&sock);
		return;
	}

	tqpair->sock = sock;
	tqpair->max_queue_depth = 1;
	tqpair->free_pdu_num = tqpair->max_queue_depth + NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM;
	tqpair->state_cntr[TCP_REQUEST_STATE_FREE] = tqpair->max_queue_depth;
	tqpair->port = port;
	tqpair->qpair.transport = transport;


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

static void
spdk_nvmf_tcp_qpair_set_recv_state(struct spdk_nvmf_tcp_qpair *tqpair,
				   enum spdk_nvmf_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tqpair(%p) recv state=%d\n", tqpair, state);
	tqpair->recv_state = state;
	switch (state) {
	case TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
	case TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
	case TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
		break;
	case TCP_PDU_RECV_STATE_ERROR:
	case TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
		memset(&tqpair->pdu_in_progress, 0, sizeof(tqpair->pdu_in_progress));
		break;
	default:
		SPDK_ERRLOG("The state(%d) is invalid\n", state);
		abort();
		break;
	}
}

static void
spdk_nvmf_tcp_send_c2h_term_req_complete(void *cb_arg)
{
}

static void
spdk_nvmf_tcp_send_c2h_term_req(struct spdk_nvmf_tcp_qpair *tqpair, struct spdk_nvmf_tcp_pdu *pdu,
				enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct spdk_nvmf_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req;
	uint32_t c2h_term_req_hdr_len = sizeof(*c2h_term_req);

	rsp_pdu = spdk_nvmf_tcp_pdu_get(tqpair);
	if (!rsp_pdu) {
		tqpair->state = NVMF_TCP_QPAIR_STATE_EXITING;
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
		return;
	}

	c2h_term_req = &rsp_pdu->hdr.term_req;
	c2h_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ;
	c2h_term_req->common.hlen = c2h_term_req_hdr_len;
	/* It should contain the header the received pdu */
	c2h_term_req->common.plen = c2h_term_req->common.hlen + pdu->hdr.common.hlen;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PRAMATER)) {
		DSET32(&c2h_term_req->fei, error_offset);
	}

	/* copy the error code into the buffer */
	rsp_pdu->data = (uint8_t *)rsp_pdu->hdr.raw + c2h_term_req_hdr_len;
	if (pdu->hdr.common.plen <= NVMF_TCP_MAX_ERROR_DATA_SIZE) {
		memcpy((uint8_t *)rsp_pdu->data, pdu->hdr.raw, pdu->hdr.common.plen);
	} else {
		memcpy((uint8_t *)rsp_pdu->data, pdu->hdr.raw,  NVMF_TCP_MAX_ERROR_DATA_SIZE);
	}

	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_send_c2h_term_req_complete, tqpair);
}

static void
spdk_nvmf_tcp_capsule_cmd_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
				     struct spdk_nvmf_tcp_qpair *tqpair,
				     struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_request *tcp_req;

	tcp_req = spdk_nvmf_tcp_req_get(tqpair);
	if (!tcp_req) {
		SPDK_ERRLOG("Cannot allocate tcp_req\n");
		tqpair->state = NVMF_TCP_QPAIR_STATE_EXITING;
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
		return;
	}

	pdu->tcp_req = tcp_req;
	spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_NEW);
	spdk_nvmf_tcp_request_process(ttransport, tcp_req);
	return;
}

static void
spdk_nvmf_tcp_capsule_cmd_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
		struct spdk_nvmf_tcp_qpair *tqpair,
		struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_request *tcp_req;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	capsule_cmd = &pdu->hdr.capsule_cmd;
	tcp_req = pdu->tcp_req;
	assert(tcp_req != NULL);
	if (capsule_cmd->common.pdo > NVMF_TCP_PDU_PDO_MAX_OFFSET) {
		SPDK_ERRLOG("Expected ICReq capsule_cmd pdu offset <= %d, got %c\n",
			    NVMF_TCP_PDU_PDO_MAX_OFFSET, capsule_cmd->common.pdo);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdo);
		goto err;
	}

	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
	spdk_nvmf_tcp_request_process(ttransport, tcp_req);

	return;
err:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
spdk_nvmf_tcp_h2c_data_hdr_handle(struct spdk_nvmf_tcp_transport *ttransport,
				  struct spdk_nvmf_tcp_qpair *tqpair,
				  struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_request *tcp_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes = 0;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	uint32_t iov_index;
	bool ttag_offset_error = false;

	h2c_data = &pdu->hdr.h2c_data;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tqpair=%p, r2t_info: datao=%u, datal=%u, cccid=%u, ttag=%u\n",
		      tqpair, h2c_data->datao, h2c_data->datal, h2c_data->cccid, h2c_data->ttag);

	/* According to the information in the pdu to find the req */
	TAILQ_FOREACH(tcp_req, &tqpair->state_queue[TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER],
		      state_link) {
		if ((tcp_req->req.cmd->nvme_cmd.cid == h2c_data->cccid) && (tcp_req->ttag == h2c_data->ttag)) {
			break;
		}

		if (!ttag_offset_error && (tcp_req->req.cmd->nvme_cmd.cid == h2c_data->cccid)) {
			ttag_offset_error = true;
		}
	}

	if (!tcp_req) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tcp_req is not found for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PRAMATER;
		if (!ttag_offset_error) {
			error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, cccid);
		} else {
			error_offset = offsetof(struct spdk_nvme_tcp_h2c_data_hdr, ttag);
		}
		goto err;
	}

	if (tcp_req->next_expected_r2t_offset != h2c_data->datao) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
			      "tcp_req(%p), tqpair=%p,  expected_r2t_offset=%u, but data offset =%u\n",
			      tcp_req, tqpair, tcp_req->next_expected_r2t_offset, h2c_data->datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	if (h2c_data->datal > tqpair->maxh2cdata) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tcp_req(%p), tqpair=%p,  datao=%u execeeds maxh2cdata size=%u\n",
			      tcp_req, tqpair, h2c_data->datao, tqpair->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_TRANSFER_OUT_OF_RANGE;
		goto err;
	}

	if ((h2c_data->datao + h2c_data->datal) > tcp_req->req.length) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
			      "tcp_req(%p), tqpair=%p,  (datao=%u + datal=%u) execeeds requested length=%u\n",
			      tcp_req, tqpair, h2c_data->datao, h2c_data->datal, tcp_req->req.length);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_TRANSFER_LIMIT_EXCEEDED;
		goto err;
	}

	pdu->tcp_req = tcp_req;
	pdu->data_len = h2c_data->datal;
	iov_index = pdu->hdr.h2c_data.datao / ttransport->io_unit_size;
	pdu->data = tcp_req->req.iov[iov_index].iov_base + (pdu->hdr.h2c_data.datao %
			ttransport->io_unit_size);
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

err:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static void
spdk_nvmf_tcp_pdu_cmd_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_request *tcp_req = cb_arg;
	nvmf_tcp_request_free(tcp_req);
}

static void
spdk_nvmf_tcp_pdu_c2h_data_complete(void *cb_arg)
{
	struct spdk_nvmf_tcp_request *tcp_req = cb_arg;
	struct spdk_nvmf_tcp_qpair *tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair,
					     struct spdk_nvmf_tcp_qpair, qpair);

	assert(tqpair != NULL);

	tcp_req->c2h_data_pdu_num--;
	if (!tcp_req->c2h_data_pdu_num) {
		nvmf_tcp_request_free(tcp_req);
	}

	tqpair->c2h_data_pdu_cnt--;
	spdk_nvmf_tcp_handle_pending_c2h_data_queue(tqpair);
}

static void
spdk_nvmf_tcp_send_r2t_pdu(struct spdk_nvmf_tcp_transport *ttransport,
			   struct spdk_nvmf_tcp_qpair *tqpair,
			   struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvmf_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_r2t_hdr *r2t;

	rsp_pdu = spdk_nvmf_tcp_pdu_get(tqpair);
	if (!rsp_pdu) {
		tqpair->state = NVMF_TCP_QPAIR_STATE_EXITING;
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
		return;
	}

	r2t = &rsp_pdu->hdr.r2t;
	r2t->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_R2T;
	r2t->common.plen = r2t->common.hlen = sizeof(*r2t);

	if (tqpair->host_hdgst_enable) {
		r2t->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		r2t->common.plen += NVMF_TCP_DIGEST_LEN;
	}

	r2t->cccid = tcp_req->req.cmd->nvme_cmd.cid;
	r2t->ttag = tcp_req->ttag;
	r2t->r2to = tcp_req->next_expected_r2t_offset;
	r2t->r2tl = spdk_min(tqpair->maxh2cdata, tcp_req->req.length - tcp_req->next_expected_r2t_offset);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
		      "tcp_req(%p) on tqpair(%p), r2t_info: cccid=%u, ttag=%u, r2to=%u, r2tl=%u\n",
		      tcp_req, tqpair, r2t->cccid, r2t->ttag, r2t->r2to, r2t->r2tl);
	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_pdu_cmd_complete, NULL);

}

static void
spdk_nvmf_tcp_h2c_data_payload_handle(struct spdk_nvmf_tcp_transport *ttransport,
				      struct spdk_nvmf_tcp_qpair *tqpair,
				      struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvmf_tcp_request *tcp_req;

	tcp_req = pdu->tcp_req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

	tcp_req->next_expected_r2t_offset += pdu->data_len;
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	if (tcp_req->next_expected_r2t_offset == tcp_req->req.length) {
		assert(tqpair->pending_r2t > 0);
		tqpair->pending_r2t--;
		assert(tqpair->pending_r2t < tqpair->maxr2t);
		spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
		spdk_nvmf_tcp_request_process(ttransport, tcp_req);

		/* fetch next */
		tcp_req = TAILQ_FIRST(&tqpair->queued_r2t_tcp_req);
		if (tcp_req) {
			spdk_nvmf_tcp_request_process(ttransport, tcp_req);
		}
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Send r2t pdu for tcp_req=%p on tqpair=%p\n", tcp_req, tqpair);
		spdk_nvmf_tcp_send_r2t_pdu(ttransport, tqpair, tcp_req);
	}
}

static void
spdk_nvmf_tcp_h2c_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *h2c_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", h2c_term_req,
		    spdk_nvme_tcp_term_req_fes_str[h2c_term_req->fes]);
	if ((h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (h2c_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PRAMATER)) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "The offset from the start of the PDU header is %u\n",
			      DGET32(h2c_term_req->fei));
	}
	/* we may also need to dump some other info here */
}

static void
spdk_nvmf_tcp_h2c_term_req_hdr_handle(struct spdk_nvmf_tcp_qpair *tqpair,
				      struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;


	if (h2c_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PRAMATER) {
		SPDK_ERRLOG("Fatal Error Stauts(FES) is unknown for h2c_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	pdu->data = (uint8_t *)pdu->hdr.raw + h2c_term_req->common.hlen;
	pdu->data_len = h2c_term_req->common.plen - h2c_term_req->common.hlen;
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
spdk_nvmf_tcp_h2c_term_req_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair,
		struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req = &pdu->hdr.term_req;

	spdk_nvmf_tcp_h2c_term_req_dump(h2c_term_req);
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
	return;
}

static void
spdk_nvmf_tcp_pdu_payload_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int rc = 0;
	struct spdk_nvmf_tcp_pdu *pdu;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	struct spdk_nvmf_tcp_transport *ttransport;

	assert(tqpair->recv_state == TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	pdu = &tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");
	/* check data digest if need */
	if (pdu->ddigest_valid_bytes) {
		crc32c = spdk_nvmf_tcp_pdu_calc_data_digest(pdu);
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("Data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HDGST;
			spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);
	switch (pdu->hdr.common.pdu_type) {
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

	tqpair->state = NVMF_TCP_QPAIR_STATE_RUNNING;
}

static void
spdk_nvmf_tcp_icreq_handle(struct spdk_nvmf_tcp_transport *ttransport,
			   struct spdk_nvmf_tcp_qpair *tqpair,
			   struct spdk_nvmf_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_req *ic_req = &pdu->hdr.ic_req;
	struct spdk_nvmf_tcp_pdu *rsp_pdu;
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
	tqpair->maxr2t = ic_req->maxr2t + 1ull;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "maxr2t =%u\n", tqpair->maxr2t);

	tqpair->host_hdgst_enable = ic_req->dgst.bits.hdgst_enable ? true : false;
	tqpair->host_ddgst_enable = ic_req->dgst.bits.ddgst_enable ? true : false;

	tqpair->cpda = spdk_min(ic_req->hpda, NVMF_TCP_CPDA_MAX);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "cpda of tqpair=(%p) is : %u\n", tqpair, tqpair->cpda);

	rsp_pdu = spdk_nvmf_tcp_pdu_get(tqpair);
	if (!rsp_pdu) {
		tqpair->state = NVMF_TCP_QPAIR_STATE_EXITING;
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
		return;
	}

	ic_resp = &rsp_pdu->hdr.ic_resp;
	ic_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_RESP;
	ic_resp->common.hlen = ic_resp->common.plen =  sizeof(*ic_resp);
	ic_resp->pfv = 0;
	ic_resp->cpda = tqpair->cpda;
	tqpair->maxh2cdata = spdk_min(NVMF_TCP_PDU_MAX_H2C_DATA_SIZE, ttransport->io_unit_size);
	ic_resp->maxh2cdata = tqpair->maxh2cdata;
	ic_resp->dgst.bits.hdgst_enable = tqpair->host_hdgst_enable ? 1 : 0;
	ic_resp->dgst.bits.ddgst_enable = tqpair->host_ddgst_enable ? 1 : 0;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_send_icresp_complete, tqpair);
	spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	return;
end:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
spdk_nvmf_tcp_pdu_psh_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	struct spdk_nvmf_tcp_transport *ttransport;

	assert(tqpair->recv_state == TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = &tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "pdu type of tqpair(%p) is %d\n", tqpair,
		      pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Compare the header of pdu=%p on tqpair=%p\n", pdu, tqpair);
		crc32c = spdk_nvmf_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("Header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HDGST;
			spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);
	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_REQ:
		spdk_nvmf_tcp_icreq_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		spdk_nvmf_tcp_capsule_cmd_hdr_handle(ttransport, tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		spdk_nvmf_tcp_h2c_data_hdr_handle(ttransport, tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ:
		spdk_nvmf_tcp_h2c_term_req_hdr_handle(tqpair, pdu);
		break;

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->pdu_in_progress.hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
		break;
	}

}

static void
spdk_nvmf_tcp_pdu_ch_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint8_t expected_hlen, pdo;
	bool plen_error = false, pdo_error = false;

	assert(tqpair->recv_state == TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
	pdu = &tqpair->pdu_in_progress;

	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received ICreq PDU, and reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_PDU_SEQUENCE;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_req);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_RUNNING) {
			SPDK_ERRLOG("The TCP/IP connection is not negotitated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_PDU_SEQUENCE;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
			expected_hlen = sizeof(struct spdk_nvme_tcp_cmd);
			pdo = pdu->hdr.common.pdo;
			if ((tqpair->cpda != 0) && (pdo != ((tqpair->cpda + 1) << 2))) {
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
			if ((tqpair->cpda != 0) && (pdo != ((tqpair->cpda + 1) << 2))) {
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
			    (pdu->hdr.common.plen > NVMF_TCP_TERM_REQ_PDU_MAX_SIZE)) {
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
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		return;
	}
err:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static int
spdk_nvmf_tcp_sock_process(struct spdk_nvmf_tcp_qpair *tqpair)
{
	int rc = 0;
	struct spdk_nvmf_tcp_pdu *pdu;
	enum spdk_nvmf_tcp_pdu_recv_state prev_state;
	uint32_t data_len;
	uint8_t psh_len, pdo, hlen;
	int8_t  padding_len;

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tqpair->recv_state;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tqpair(%p) recv pdu entering state %d\n", tqpair, prev_state);

		switch (tqpair->recv_state) {
		/* If in a new state */
		case TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
			break;
		/* Wait for the common header  */
		case TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			pdu = &tqpair->pdu_in_progress;
			/* common header */
			if (pdu->ch_valid_bytes < NVMF_TCP_CH_LEN) {
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
								   NVMF_TCP_CH_LEN - pdu->ch_valid_bytes,
								   (void *)&pdu->hdr.common + pdu->ch_valid_bytes);
				if (rc < 0) {
					SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "will disconnect tqpair=%p\n", tqpair);
					spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
					break;
				}
				pdu->ch_valid_bytes += rc;
				if (pdu->ch_valid_bytes < NVMF_TCP_CH_LEN) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}

			tqpair->last_pdu_time = spdk_get_ticks();
			/* The command header of this PDU has now been read from the socket. */
			spdk_nvmf_tcp_pdu_ch_handle(tqpair);
			break;
		/* Wait for the pdu specific header  */
		case TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			pdu = &tqpair->pdu_in_progress;
			if (pdu->hd_is_read) {
				break;
			}

			psh_len = hlen = pdu->hdr.common.hlen;
			/* Only capsule_cmd and h2c_data has header digest */
			if (((pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD) ||
			     (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_DATA)) &&
			    tqpair->host_hdgst_enable) {
				pdu->has_hdgst = true;
				psh_len += NVMF_TCP_DIGEST_LEN;
				if (pdu->hdr.common.plen > psh_len) {
					pdo = pdu->hdr.common.pdo;
					padding_len = pdo - psh_len;
					SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "padding length is =%d for pdu=%p on tqpair=%p\n", padding_len,
						      pdu, tqpair);
					if (padding_len > 0) {
						psh_len = pdo;
					}
				}
			}

			psh_len -= NVMF_TCP_CH_LEN;
			/* The following will read psh + hdgest (if possbile) + padding (if posssible) */
			if (pdu->psh_valid_bytes < psh_len) {
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
								   psh_len - pdu->psh_valid_bytes,
								   (void *)&pdu->hdr.raw + NVMF_TCP_CH_LEN + pdu->psh_valid_bytes);
				if (rc < 0) {
					spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
					break;
				}

				pdu->psh_valid_bytes += rc;
				if (pdu->psh_valid_bytes < psh_len) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}

			pdu->hd_is_read = true;

			/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
			spdk_nvmf_tcp_pdu_psh_handle(tqpair);
			break;
		case TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			pdu = &tqpair->pdu_in_progress;

			/* check whether the data is valid, if not we just return */
			if (!pdu->data) {
				return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			/* data len */
			if (pdu->data_valid_bytes < data_len) {
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair, data_len - pdu->data_valid_bytes,
								   (void *)pdu->data + pdu->data_valid_bytes);
				if (rc < 0) {
					spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
					break;
				}

				pdu->data_valid_bytes += rc;
				if (pdu->data_valid_bytes < data_len) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}

			/* data digest */
			if ((pdu->hdr.common.pdu_type != SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ) &&
			    tqpair->host_ddgst_enable && (pdu->ddigest_valid_bytes < NVMF_TCP_DIGEST_LEN)) {
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
								   NVMF_TCP_DIGEST_LEN - pdu->ddigest_valid_bytes,
								   pdu->data_digest + pdu->ddigest_valid_bytes);
				if (rc < 0) {
					spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
					break;
				}

				pdu->ddigest_valid_bytes += rc;
				if (pdu->ddigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}

			/* All of this PDU has now been read from the socket. */
			spdk_nvmf_tcp_pdu_payload_handle(tqpair);
			break;
		case TCP_PDU_RECV_STATE_ERROR:
			break;
		default:
			assert(0);
			SPDK_ERRLOG("code should not come to here");
			break;
		}
	} while (tqpair->recv_state != prev_state);

	return rc;
}

static enum spdk_nvme_data_transfer
spdk_nvmf_tcp_request_get_xfer(struct spdk_nvmf_tcp_request *tcp_req) {
	enum spdk_nvme_data_transfer xfer;
	struct spdk_nvme_cmd *cmd = &tcp_req->req.cmd->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;

	/* Figure out data transfer direction */
	if (cmd->opc == SPDK_NVME_OPC_FABRIC)
	{
		xfer = spdk_nvme_opc_get_data_transfer(tcp_req->req.cmd->nvmf_cmd.fctype);
	} else
	{
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

	if (xfer == SPDK_NVME_DATA_NONE)
	{
		return xfer;
	}

	/* Even for commands that may transfer data, they could have specified 0 length.
	 * We want those to show up with xfer SPDK_NVME_DATA_NONE.
	 */
	switch (sgl->generic.type)
	{
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
spdk_nvmf_tcp_request_fill_iovs(struct spdk_nvmf_tcp_transport *ttransport,
				struct spdk_nvmf_tcp_request *tcp_req)
{
	void		*buf = NULL;
	uint32_t	length = tcp_req->req.length;
	uint32_t	i = 0;

	tcp_req->req.iovcnt = 0;
	while (length) {
		buf = spdk_mempool_get(ttransport->data_buf_pool);
		if (!buf) {
			goto nomem;
		}

		tcp_req->req.iov[i].iov_base = (void *)((uintptr_t)(buf + NVMF_DATA_BUFFER_MASK) &
							~NVMF_DATA_BUFFER_MASK);
		tcp_req->req.iov[i].iov_len  = spdk_min(length, ttransport->io_unit_size);
		tcp_req->req.iovcnt++;
		tcp_req->buffers[i] = buf;
		length -= tcp_req->req.iov[i].iov_len;
		i++;
	}

	tcp_req->data_from_pool = true;

	return 0;

nomem:
	while (i) {
		i--;
		spdk_mempool_put(ttransport->data_buf_pool, tcp_req->req.iov[i].iov_base);
		tcp_req->req.iov[i].iov_base = NULL;
		tcp_req->req.iov[i].iov_len = 0;

	}
	tcp_req->req.iovcnt = 0;
	return -ENOMEM;
}

static int
spdk_nvmf_tcp_request_parse_sgl(struct spdk_nvmf_tcp_transport *ttransport,
				struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvme_cmd			*cmd;
	struct spdk_nvme_cpl			*rsp;
	struct spdk_nvme_sgl_descriptor		*sgl;

	cmd = &tcp_req->req.cmd->nvme_cmd;
	rsp = &tcp_req->req.rsp->nvme_cpl;
	sgl = &cmd->dptr.sgl1;

	if (sgl->generic.type == SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK &&
	    sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_TRANSPORT) {
		if (sgl->unkeyed.length > ttransport->max_io_size) {
			SPDK_ERRLOG("SGL length 0x%x exceeds max io size 0x%x\n",
				    sgl->unkeyed.length, ttransport->max_io_size);
			rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			return -1;
		}

		/* fill request length and populate iovs */
		tcp_req->req.length = sgl->unkeyed.length;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Data requested length= 0x%x\n",
			      sgl->unkeyed.length);

		if (spdk_nvmf_tcp_request_fill_iovs(ttransport, tcp_req) < 0) {
			/* No available buffers. Queue this request up. */
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "No available large data buffers. Queueing request %p\n", tcp_req);
			return 0;
		}

		/* backward compatible */
		tcp_req->req.data = tcp_req->req.iov[0].iov_base;


		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Request %p took %d buffer/s from central pool, and data=%p\n",
			      tcp_req,
			      tcp_req->req.iovcnt, tcp_req->req.data);

		return 0;
	} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK &&
		   sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
		uint64_t offset = sgl->address;
		uint32_t max_len = ttransport->in_capsule_data_size;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "In-capsule data: offset 0x%" PRIx64 ", length 0x%x\n",
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

		tcp_req->req.data = tcp_req->buf + offset;
		tcp_req->data_from_pool = false;
		tcp_req->req.length = sgl->unkeyed.length;

		tcp_req->req.iov[0].iov_base = tcp_req->req.data;
		tcp_req->req.iov[0].iov_len = tcp_req->req.length;
		tcp_req->req.iovcnt = 1;

		return 0;
	}

	SPDK_ERRLOG("Invalid NVMf I/O Command SGL:  Type 0x%x, Subtype 0x%x\n",
		    sgl->generic.type, sgl->generic.subtype);
	rsp->status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	return -1;
}

static void
spdk_nvmf_tcp_send_capsule_resp_pdu(struct spdk_nvmf_tcp_request *tcp_req,
				    struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_rsp *capsule_resp;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter, tqpair=%p\n", tqpair);
	rsp_pdu = spdk_nvmf_tcp_pdu_get(tqpair);
	if (!rsp_pdu) {
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
		tqpair->state = NVMF_TCP_QPAIR_STATE_EXITING;
		return;
	}

	capsule_resp = &rsp_pdu->hdr.capsule_resp;
	capsule_resp->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP;
	capsule_resp->common.plen = capsule_resp->common.hlen = sizeof(*capsule_resp);
	capsule_resp->rccqe = tcp_req->req.rsp->nvme_cpl;  //set the nvme cpl info.
	if (tqpair->host_hdgst_enable) {
		capsule_resp->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		capsule_resp->common.plen += NVMF_TCP_DIGEST_LEN;
	}

	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_pdu_cmd_complete, tcp_req);

}

static void
spdk_nvmf_tcp_send_c2h_data(struct spdk_nvmf_tcp_qpair *tqpair,
			    struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvmf_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint32_t plen, pdo, alignment, offset, iov_index;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

	/* always use the first iov_len, which is correct */
	iov_index = tcp_req->c2h_data_offset / tcp_req->req.iov[0].iov_len;
	offset = tcp_req->c2h_data_offset % tcp_req->req.iov[0].iov_len;

	rsp_pdu = spdk_nvmf_tcp_pdu_get(tqpair);
	assert(rsp_pdu != NULL);

	c2h_data = &rsp_pdu->hdr.c2h_data;
	c2h_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_C2H_DATA;
	plen = c2h_data->common.hlen = sizeof(*c2h_data);

	if (tqpair->host_hdgst_enable) {
		plen += NVMF_TCP_DIGEST_LEN;
		c2h_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
	}

	/* set the psh */
	c2h_data->cccid = tcp_req->req.cmd->nvme_cmd.cid;
	c2h_data->datal = spdk_min(NVMF_TCP_PDU_MAX_C2H_DATA_SIZE,
				   (tcp_req->req.iov[iov_index].iov_len - offset));
	c2h_data->datao = tcp_req->c2h_data_offset;

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
		plen += NVMF_TCP_DIGEST_LEN;
	}

	c2h_data->common.plen = plen;

	//set the buf
	rsp_pdu->data = tcp_req->req.iov[iov_index].iov_base + offset;
	rsp_pdu->data_len = c2h_data->datal;

	tcp_req->c2h_data_offset += c2h_data->datal;
	if (iov_index == (tcp_req->req.iovcnt - 1) && (tcp_req->c2h_data_offset == tcp_req->req.length)) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Last pdu for tcp_req=%p on tqpair=%p\n", tcp_req, tqpair);
		c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU;
		/* The linux kernel does not support this yet */
#if LINUX_KERNEL_SUPPORT_NOT_SENDING_RESP_FOR_C2H
		c2h_data->common.flags |= SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS;
#endif
		TAILQ_REMOVE(&tqpair->queued_c2h_data_tcp_req, tcp_req, link);
	}

	tqpair->c2h_data_pdu_cnt += 1;
	spdk_nvmf_tcp_qpair_write_pdu(tqpair, rsp_pdu, spdk_nvmf_tcp_pdu_c2h_data_complete, tcp_req);

#if !LINUX_KERNEL_SUPPORT_NOT_SENDING_RESP_FOR_C2H
	spdk_nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
#endif
}

static int
spdk_nvmf_tcp_calc_c2h_data_pdu_num(struct spdk_nvmf_tcp_request *tcp_req)
{
	uint32_t i, iov_cnt, pdu_num = 0;

	iov_cnt = tcp_req->req.iovcnt;
	for (i = 0; i < iov_cnt; i++) {
		pdu_num += (tcp_req->req.iov[i].iov_len + NVMF_TCP_PDU_MAX_C2H_DATA_SIZE - 1) /
			   NVMF_TCP_PDU_MAX_C2H_DATA_SIZE;
	}

	return pdu_num;
}

static void
spdk_nvmf_tcp_handle_pending_c2h_data_queue(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_request *tcp_req;

	while (!TAILQ_EMPTY(&tqpair->queued_c2h_data_tcp_req) &&
	       (tqpair->c2h_data_pdu_cnt < NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM)) {
		tcp_req = TAILQ_FIRST(&tqpair->queued_c2h_data_tcp_req);
		spdk_nvmf_tcp_send_c2h_data(tqpair, tcp_req);
	}
}

static void
spdk_nvmf_tcp_queue_c2h_data(struct spdk_nvmf_tcp_request *tcp_req,
			     struct spdk_nvmf_tcp_qpair *tqpair)
{
	tcp_req->c2h_data_pdu_num = spdk_nvmf_tcp_calc_c2h_data_pdu_num(tcp_req);

	assert(tcp_req->c2h_data_pdu_num < NVMF_TCP_QPAIR_MAX_C2H_PDU_NUM);

	TAILQ_INSERT_TAIL(&tqpair->queued_c2h_data_tcp_req, tcp_req, link);
	spdk_nvmf_tcp_handle_pending_c2h_data_queue(tqpair);
}

static int
request_transfer_out(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_request	*tcp_req;
	struct spdk_nvmf_qpair		*qpair;
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

	qpair = req->qpair;
	rsp = &req->rsp->nvme_cpl;
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);

	/* Advance our sq_head pointer */
	if (qpair->sq_head == qpair->sq_head_max) {
		qpair->sq_head = 0;
	} else {
		qpair->sq_head++;
	}
	rsp->sqhd = qpair->sq_head;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
	spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST);
	if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
	    req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		spdk_nvmf_tcp_queue_c2h_data(tcp_req, tqpair);
	} else {
		spdk_nvmf_tcp_send_capsule_resp_pdu(tcp_req, tqpair);
	}

	return 0;
}

static void
spdk_nvmf_tcp_pdu_set_buf_from_req(struct spdk_nvmf_tcp_transport *ttransport,
				   struct spdk_nvmf_tcp_qpair *tqpair,
				   struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvmf_tcp_pdu *pdu;
	uint32_t plen = 0;

	pdu = &tqpair->pdu_in_progress;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "tqpair=%p\n", tqpair);
	assert(pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD);

	plen = pdu->hdr.common.hlen;
	if (tqpair->host_hdgst_enable) {
		plen += NVMF_TCP_DIGEST_LEN;
	}

	/* need to send r2t for data */
	if (pdu->hdr.common.plen == plen) {
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Will send r2t for tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
		tcp_req->next_expected_r2t_offset = 0;
		TAILQ_INSERT_TAIL(&tqpair->queued_r2t_tcp_req, tcp_req, link);
		spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T);

	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "Not need to send r2t for tcp_req(%p) on tqpair=%p\n", tcp_req,
			      tqpair);
		/* No need to send r2t, contained in the capsuled data */
		pdu->data = tcp_req->req.data;
		pdu->data_len = tcp_req->req.length;
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
		spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
	}

}

static bool
spdk_nvmf_tcp_request_process(struct spdk_nvmf_tcp_transport *ttransport,
			      struct spdk_nvmf_tcp_request *tcp_req)
{
	struct spdk_nvmf_tcp_qpair	*tqpair;
	struct spdk_nvme_cpl		*rsp = &tcp_req->req.rsp->nvme_cpl;
	int				rc;
	enum spdk_nvmf_tcp_request_state prev_state;
	bool				progress = false;

	tqpair = SPDK_CONTAINEROF(tcp_req->req.qpair, struct spdk_nvmf_tcp_qpair, qpair);
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
			tcp_req->cmd = tqpair->pdu_in_progress.hdr.capsule_cmd.ccsqe;

			/* The next state transition depends on the data transfer needs of this request. */
			tcp_req->req.xfer = spdk_nvmf_tcp_request_get_xfer(tcp_req);

			/* If no data to transfer, ready to execute. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_NONE) {
				/* Reset the tqpair receving pdu state */
				spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
				break;
			}

			spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_NEED_BUFFER);
			TAILQ_INSERT_TAIL(&tqpair->ch->pending_data_buf_queue, tcp_req, link);
			break;
		case TCP_REQUEST_STATE_NEED_BUFFER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_NEED_BUFFER, 0, 0, (uintptr_t)tcp_req, 0);

			assert(tcp_req->req.xfer != SPDK_NVME_DATA_NONE);

			if (tcp_req != TAILQ_FIRST(&tqpair->ch->pending_data_buf_queue)) {
				SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP,
					      "Not the first element to wait for the buf for tcp_req(%p) on tqpair=%p\n",
					      tcp_req, tqpair);
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = spdk_nvmf_tcp_request_parse_sgl(ttransport, tcp_req);
			if (rc < 0) {
				TAILQ_REMOVE(&tqpair->ch->pending_data_buf_queue, tcp_req, link);
				rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				/* Reset the tqpair receving pdu state */
				spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
				spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
				break;
			}

			if (!tcp_req->req.data) {
				SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "No buffer allocated for tcp_req(%p) on tqpair(%p\n)",
					      tcp_req, tqpair);
				/* No buffers available. */
				break;
			}

			TAILQ_REMOVE(&tqpair->ch->pending_data_buf_queue, tcp_req, link);

			/* If data is transferring from host to controller, we need to do a transfer from the host. */
			if (tcp_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				spdk_nvmf_tcp_pdu_set_buf_from_req(ttransport, tqpair, tcp_req);
				break;
			}

			spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_EXECUTE);
			break;
		case TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T:
			spdk_trace_record(TCP_REQUEST_STATE_DATA_PENDING_FOR_R2T, 0, 0,
					  (uintptr_t)tcp_req, 0);

			if (tqpair->pending_r2t < tqpair->maxr2t) {
				tqpair->pending_r2t++;
				TAILQ_REMOVE(&tqpair->queued_r2t_tcp_req, tcp_req, link);
				spdk_nvmf_tcp_send_r2t_pdu(ttransport, tqpair, tcp_req);
				spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER);
			}

			break;

		case TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER, 0, 0,
					  (uintptr_t)tcp_req, 0);
			/* Some external code must kick a request into TCP_REQUEST_STATE_READY_TO_EXECUTE
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_READY_TO_EXECUTE:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE, 0, 0, (uintptr_t)tcp_req, 0);
			spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTING);
			spdk_nvmf_request_exec(&tcp_req->req);
			break;
		case TCP_REQUEST_STATE_EXECUTING:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTING, 0, 0, (uintptr_t)tcp_req, 0);
			/* Some external code must kick a request into TCP_REQUEST_STATE_EXECUTED
			 * to escape this state. */
			break;
		case TCP_REQUEST_STATE_EXECUTED:
			spdk_trace_record(TRACE_TCP_REQUEST_STATE_EXECUTED, 0, 0, (uintptr_t)tcp_req, 0);
			spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_READY_TO_COMPLETE);
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
			if (tcp_req->data_from_pool) {
				/* Put the buffer/s back in the pool */
				for (uint32_t i = 0; i < tcp_req->req.iovcnt; i++) {
					spdk_mempool_put(ttransport->data_buf_pool, tcp_req->buffers[i]);
					tcp_req->req.iov[i].iov_base = NULL;
					tcp_req->buffers[i] = NULL;
				}
				tcp_req->data_from_pool = false;
			}
			tcp_req->req.length = 0;
			tcp_req->req.iovcnt = 0;
			tcp_req->req.data = NULL;
			spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_FREE);
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
spdk_nvmf_tcp_qpair_process_pending(struct spdk_nvmf_tcp_transport *ttransport,
				    struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_request *tcp_req, *req_tmp;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->queued_r2t_tcp_req, link, req_tmp) {
		if (tqpair->pending_r2t < tqpair->maxr2t) {
			if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
				break;
			}
		}
	}

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->ch->pending_data_buf_queue, link, req_tmp) {
		if (spdk_nvmf_tcp_request_process(ttransport, tcp_req) == false) {
			break;
		}
	}
}

static void
spdk_nvmf_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_tcp_qpair *tqpair = arg;
	struct spdk_nvmf_tcp_transport *ttransport;
	int rc;

	assert(tqpair != NULL);

	if (tqpair->recv_state == TCP_PDU_RECV_STATE_ERROR) {
		return;
	}

	ttransport = SPDK_CONTAINEROF(tqpair->qpair.transport, struct spdk_nvmf_tcp_transport, transport);
	spdk_nvmf_tcp_qpair_process_pending(ttransport, tqpair);

	rc = spdk_nvmf_tcp_sock_process(tqpair);
	if (rc < 0) {
		tqpair->state = NVMF_TCP_QPAIR_STATE_EXITING;
		spdk_nvmf_tcp_qpair_flush_pdus(tqpair);
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
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return -1;
	}

	rc = spdk_sock_getaddr(tqpair->sock, tqpair->target_addr,
			       sizeof(tqpair->target_addr),
			       tqpair->initiator_addr, sizeof(tqpair->initiator_addr));
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed\n");
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return -1;
	}

	rc =  spdk_nvmf_tcp_qpair_sock_init(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Cannot set sock opt for tqpair=%p\n", tqpair);
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return -1;
	}

	rc = spdk_nvmf_tcp_qpair_init(&tqpair->qpair);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init tqpair=%p\n", tqpair);
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return -1;
	}

	rc = spdk_nvmf_tcp_qpair_init_mem_resource(tqpair, 1);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot init memory resource info for tqpair=%p\n", tqpair);
		spdk_nvmf_tcp_qpair_destroy(tqpair);
		return -1;
	}

	tqpair->state = NVMF_TCP_QPAIR_STATE_INVALID;
	tqpair->timeout = NVMF_TCP_QPAIR_EXIT_TIMEOUT;
	tqpair->last_pdu_time = spdk_get_ticks();

	tqpair->tgroup = tgroup;
	TAILQ_INSERT_TAIL(&tgroup->qpairs, tqpair, link);

	return 0;
}

static int
spdk_nvmf_tcp_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_tcp_transport *ttransport;
	struct spdk_nvmf_tcp_request *tcp_req;

	ttransport = SPDK_CONTAINEROF(req->qpair->transport, struct spdk_nvmf_tcp_transport, transport);
	tcp_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_tcp_request, req);

	spdk_nvmf_tcp_request_set_state(tcp_req, TCP_REQUEST_STATE_EXECUTED);
	spdk_nvmf_tcp_request_process(ttransport, tcp_req);

	return 0;
}

static void
spdk_nvmf_tcp_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "enter\n");

	spdk_nvmf_tcp_qpair_destroy(SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair));
}

static void
spdk_nvmf_tcp_qpair_handle_timout(struct spdk_nvmf_tcp_qpair *tqpair)
{
	uint64_t	tsc;

	if ((tqpair->state == NVMF_TCP_QPAIR_STATE_EXITING) ||
	    (tqpair->state == NVMF_TCP_QPAIR_STATE_EXITED)) {
		return;
	}

	/* Currently, we did not have keep alive support, so make sure that we should have the generic support later */
	if (tqpair->recv_state != TCP_PDU_RECV_STATE_ERROR) {
		return;
	}

	/* Check for interval expiration */
	tsc = spdk_get_ticks();
	if ((tsc - tqpair->last_pdu_time) > (tqpair->timeout  * spdk_get_ticks_hz())) {
		SPDK_ERRLOG("No pdu coming for tqpair=%p within %d seconds\n", tqpair, tqpair->timeout);
		tqpair->state = NVMF_TCP_QPAIR_STATE_EXITING;
	}
}

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
		spdk_nvmf_tcp_qpair_handle_timout(tqpair);
		if (tqpair->state == NVMF_TCP_QPAIR_STATE_EXITING) {
			/* to prevent the state is set again */
			tqpair->state = NVMF_TCP_QPAIR_STATE_EXITED;
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "will disconect the tqpair=%p\n", tqpair);
			spdk_nvmf_qpair_disconnect(&tqpair->qpair, NULL, NULL);
		}
	}

	return 0;
}

static bool
spdk_nvmf_tcp_qpair_is_idle(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair *tqpair;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	if (tqpair->state_cntr[TCP_REQUEST_STATE_FREE] == tqpair->max_queue_depth) {
		return true;
	}

	return false;
}


static int
spdk_nvmf_tcp_qpair_get_trid(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvme_transport_id *trid, bool peer)
{
	struct spdk_nvmf_tcp_qpair     *tqpair;
	struct sockaddr *saddr;

	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);
	trid->trtype = SPDK_NVME_TRANSPORT_TCP;

	if (peer) {
		saddr = (struct sockaddr *)tqpair->initiator_addr;
	} else {

		saddr = (struct sockaddr *)tqpair->target_addr;
	}

	switch (saddr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *saddr_in = (struct sockaddr_in *)saddr;

		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
		inet_ntop(AF_INET, &saddr_in->sin_addr,
			  trid->traddr, sizeof(trid->traddr));
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", saddr_in->sin_port);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *saddr_in = (struct sockaddr_in6 *)saddr;
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV6;
		inet_ntop(AF_INET6, &saddr_in->sin6_addr,
			  trid->traddr, sizeof(trid->traddr));
		snprintf(trid->trsvcid, sizeof(trid->trsvcid), "%u", saddr_in->sin6_port);
		break;
	}
	default:
		return -1;

	}

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
spdk_nvmf_tcp_qpair_set_sq_size(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_tcp_qpair     *tqpair;
	int rc;
	tqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_tcp_qpair, qpair);

	rc = spdk_nvmf_tcp_qpair_init_mem_resource(tqpair, tqpair->qpair.sq_head_max);
	if (!rc) {
		tqpair->max_queue_depth += tqpair->qpair.sq_head_max;
		tqpair->free_pdu_num += tqpair->qpair.sq_head_max;
		tqpair->state_cntr[TCP_REQUEST_STATE_FREE] += tqpair->qpair.sq_head_max;
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "The queue depth=%u for tqpair=%p\n",
			      tqpair->max_queue_depth, tqpair);
	}

	return rc;

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

	.req_free = spdk_nvmf_tcp_request_free,
	.req_complete = spdk_nvmf_tcp_request_complete,

	.qpair_fini = spdk_nvmf_tcp_close_qpair,
	.qpair_is_idle = spdk_nvmf_tcp_qpair_is_idle,
	.qpair_get_local_trid = spdk_nvmf_tcp_qpair_get_local_trid,
	.qpair_get_peer_trid = spdk_nvmf_tcp_qpair_get_peer_trid,
	.qpair_set_sqsize = spdk_nvmf_tcp_qpair_set_sq_size,
};

SPDK_LOG_REGISTER_COMPONENT("nvmf_tcp", SPDK_LOG_NVMF_TCP)
