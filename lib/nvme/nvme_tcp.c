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
 * NVMe/TCP transport
 */

#include "nvme_internal.h"

#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/nvmf_spec.h"
#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/util.h"


#define NVME_TCP_TIME_OUT_IN_MS 30000
#define NVME_TCP_RW_BUFFER_SIZE 131072

/*
 * Maximum number of SGL elements.
 * This is chosen to match the current nvme_pcie.c limit.
 */
#define NVME_TCP_MAX_SGL_DESCRIPTORS	(253)

struct nvme_tcp_request;


#define NVME_TCP_CH_LEN sizeof(struct spdk_nvme_tcp_common_pdu_hdr)
#define NVME_TCP_PSH_LEN_MAX 120
#define NVME_TCP_DIGEST_LEN 4
#define NVME_TCP_CPDA_MAX 31
#define NVME_TCP_HPDA_DEFAULT 0
#define NVME_TCP_ALIGNMENT 4
#define NVME_TCP_MAX_R2T_DEFAULT 4

#define NVME_TCP_TERM_REQ_PDU_MAX_SIZE  152
#define NVME_TCP_PDU_PDO_MAX_OFFSET	((NVME_TCP_CPDA_MAX + 1) << 2)
#define NVME_TCP_PDU_H2C_MIN_DATA_SIZE	4096
#define NVME_TCP_QPAIR_EXIT_TIMEOUT	30

/* The following security functions  should  put in general header file */
#define SPDK_CRC32C_INITIAL    0xffffffffUL
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

enum nvme_tcp_error_codes {
	NVME_TCP_PDU_IN_PROGRESS	= 0,
	NVME_TCP_CONNECTION_FATAL	= -1,
	NVME_TCP_PDU_FATAL		= -2,
};

enum nvme_tcp_qpair_state {
	NVME_TCP_QPAIR_STATE_INVALID = 0,
	NVME_TCP_QPAIR_STATE_RUNNING = 1,
	NVME_TCP_QPAIR_STATE_EXITING = 2,
};

typedef void (*nvme_tcp_qpair_xfer_complete_cb)(void *cb_arg);

struct nvme_tcp_pdu {
	union {
		/* to hold error pdu data */
		uint8_t					raw[NVME_TCP_TERM_REQ_PDU_MAX_SIZE];
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
	uint8_t						data_digest[NVME_TCP_DIGEST_LEN];
	uint32_t					padding_valid_bytes;
	uint32_t					ddigest_valid_bytes;

	uint32_t					ch_valid_bytes;
	uint32_t					psh_valid_bytes;
	uint32_t					data_valid_bytes;

	nvme_tcp_qpair_xfer_complete_cb			cb_fn;
	void						*cb_arg;
	int						ref;
	uint8_t						*data;
	uint32_t					data_len;
	bool						data_from_req;
	struct nvme_tcp_qpair				*tqpair;

	struct nvme_tcp_req				*tcp_req; /* data tied to a tcp request */
	uint32_t					writev_offset;
	TAILQ_ENTRY(nvme_tcp_pdu)			tailq;
	uint32_t					remaining;
	int32_t						padding_len;
};

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
};

enum nvme_tcp_pdu_recv_state {
	/* Ready to wait to wait PDU */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY,

	/* Active tqpair waiting for any PDU ch header */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH_HDR,

	/* Active tqpair waiting for any PDU specific header */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_HDR,

	/* Active tqpair waiting for payload */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD,

	/* Active tqpair does not wait for payload */
	NVME_TCP_PDU_RECV_STATE_ERROR,
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;
	struct spdk_sock			*sock;

	TAILQ_HEAD(, nvme_tcp_req)		free_reqs;
	TAILQ_HEAD(, nvme_tcp_req)		outstanding_reqs;
	TAILQ_HEAD(, nvme_tcp_req)		active_r2t_reqs;

	TAILQ_HEAD(, nvme_tcp_pdu)		send_queue;
	uint32_t				in_capsule_data_size;
	struct nvme_tcp_pdu			recv_pdu;
	struct nvme_tcp_pdu			send_pdu; //only for error pdu and init pdu
	enum nvme_tcp_pdu_recv_state		recv_state;

	struct nvme_tcp_req			*tcp_reqs;

	uint16_t				num_entries;

	bool					host_hdgst_enable;
	bool					host_ddgst_enable;

	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint32_t				maxh2cdata;

	int32_t					max_r2t;
	int32_t					pending_r2t;

	/* 0 based value, which is used to guide the padding */
	uint8_t					cpda;

	enum nvme_tcp_qpair_state		state;
};

enum nvme_tcp_req_state {
	NVME_TCP_REQ_FREE,
	NVME_TCP_REQ_ACTIVE,
	NVME_TCP_REQ_ACTIVE_R2T,
};

struct nvme_tcp_req {
	struct nvme_request			*req;
	//struct spdk_nvme_cmd			cmd;
	//struct spdk_nvme_cpl			cpl;

	enum nvme_tcp_req_state			state;
	uint16_t				cid;
	uint16_t				ttag;
	uint32_t				datao;
	bool					in_capsule_data;
	struct nvme_tcp_pdu			send_pdu;
	TAILQ_ENTRY(nvme_tcp_req)		link;
	TAILQ_ENTRY(nvme_tcp_req)		active_r2t_link;
};

static inline struct nvme_tcp_qpair *
nvme_tcp_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_TCP);
	return SPDK_CONTAINEROF(qpair, struct nvme_tcp_qpair, qpair);
}

static inline struct nvme_tcp_ctrlr *
nvme_tcp_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_TCP);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
}

static struct nvme_tcp_req *
nvme_tcp_req_get(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_req *tcp_req;

	tcp_req = TAILQ_FIRST(&tqpair->free_reqs);
	if (tcp_req) {
		assert(tcp_req->state == NVME_TCP_REQ_FREE);
		tcp_req->state = NVME_TCP_REQ_ACTIVE;
		TAILQ_REMOVE(&tqpair->free_reqs, tcp_req, link);
		tcp_req->datao = 0;
		tcp_req->req = NULL;
		memset(&tcp_req->send_pdu, 0, sizeof(tcp_req->send_pdu));
		TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);
	}

	return tcp_req;
}

static void
nvme_tcp_req_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	assert(tcp_req->state != NVME_TCP_REQ_FREE);
	tcp_req->state = NVME_TCP_REQ_FREE;
	TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);
	TAILQ_INSERT_TAIL(&tqpair->free_reqs, tcp_req, link);
}

static int
nvme_tcp_parse_addr(struct sockaddr_storage *sa, int family, const char *addr, const char *service)
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
nvme_tcp_qpair_socket_init(struct nvme_tcp_qpair *tqpair)
{
	/* We do not need to set anything here, will remove this function later */
#if 0
	int buf_size, rc;

	/* set recv buffer size */
	buf_size = 2 * 1024 * 1024;
	rc = spdk_sock_set_recvbuf(tqpair->sock, buf_size);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvbuf failed\n");
		return -1;
	}

	/* set send buffer size */
	rc = spdk_sock_set_sendbuf(tqpair->sock, buf_size);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_sendbuf failed\n");
		return -1;
	}

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(tqpair->sock, 1);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed\n");
		return -1;
	}
#endif
	return 0;
}

static void
nvme_tcp_free_reqs(struct nvme_tcp_qpair *tqpair)
{
	if (!tqpair->tcp_reqs) {
		return;
	}

	free(tqpair->tcp_reqs);
	tqpair->tcp_reqs = NULL;
}

static int
nvme_tcp_alloc_reqs(struct nvme_tcp_qpair *tqpair)
{
	int i;

	tqpair->tcp_reqs = calloc(tqpair->num_entries, sizeof(struct nvme_tcp_req));
	if (tqpair->tcp_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate tcp_reqs\n");
		goto fail;
	}

	TAILQ_INIT(&tqpair->send_queue);
	TAILQ_INIT(&tqpair->free_reqs);
	TAILQ_INIT(&tqpair->outstanding_reqs);
	TAILQ_INIT(&tqpair->active_r2t_reqs);
	for (i = 0; i < tqpair->num_entries; i++) {
		struct nvme_tcp_req	*tcp_req;
		tcp_req = &tqpair->tcp_reqs[i];
		tcp_req->cid = i;
		TAILQ_INSERT_TAIL(&tqpair->free_reqs, tcp_req, link);
	}

	return 0;

fail:
	nvme_tcp_free_reqs(tqpair);
	return -ENOMEM;
}

static int
nvme_tcp_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;

	if (!qpair) {
		return -1;
	}

	nvme_qpair_deinit(qpair);

	tqpair = nvme_tcp_qpair(qpair);

	nvme_tcp_free_reqs(tqpair);

	spdk_sock_close(&tqpair->sock);
	free(tqpair);

	return 0;
}

int
nvme_tcp_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
int
nvme_tcp_ctrlr_scan(const struct spdk_nvme_transport_id *trid,
		    void *cb_ctx,
		    spdk_nvme_probe_cb probe_cb,
		    spdk_nvme_remove_cb remove_cb,
		    bool direct_connect)
{
	struct spdk_nvme_ctrlr_opts discovery_opts;
	struct spdk_nvme_ctrlr *discovery_ctrlr;
	union spdk_nvme_cc_register cc;
	int rc;
	struct nvme_completion_poll_status status;

	if (strcmp(trid->subnqn, SPDK_NVMF_DISCOVERY_NQN) != 0) {
		/* Not a discovery controller - connect directly. */
		rc = nvme_ctrlr_probe(trid, NULL, probe_cb, cb_ctx);
		return rc;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&discovery_opts, sizeof(discovery_opts));
	/* For discovery_ctrlr set the timeout to 0 */
	discovery_opts.keep_alive_timeout_ms = 0;

	discovery_ctrlr = nvme_tcp_ctrlr_construct(trid, &discovery_opts, NULL);
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

	rc = nvme_fabric_ctrlr_discover(discovery_ctrlr, cb_ctx, probe_cb);
	nvme_ctrlr_destruct(discovery_ctrlr);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "leave\n");
	return rc;
}

int
nvme_tcp_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_tcp_ctrlr *tctrlr = nvme_tcp_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_tcp_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	free(tctrlr);

	return 0;
}

int
nvme_tcp_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	return nvme_fabric_ctrlr_set_reg_4(ctrlr, offset, value);
}

int
nvme_tcp_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	return nvme_fabric_ctrlr_set_reg_8(ctrlr, offset, value);
}

int
nvme_tcp_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	return nvme_fabric_ctrlr_get_reg_4(ctrlr, offset, value);
}

int
nvme_tcp_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	return nvme_fabric_ctrlr_get_reg_8(ctrlr, offset, value);
}

static uint32_t
nvme_tcp_pdu_calc_header_digest(struct nvme_tcp_pdu *pdu)
{
	uint32_t crc32c;
	uint32_t hlen = pdu->hdr.common.hlen;

	crc32c = SPDK_CRC32C_INITIAL;
	crc32c = spdk_crc32c_update(&pdu->hdr.raw, hlen, crc32c);
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static uint32_t
nvme_tcp_pdu_calc_data_digest(struct nvme_tcp_pdu *pdu)
{
	uint32_t crc32c;

	uint32_t mod;

	crc32c = SPDK_CRC32C_INITIAL;
	crc32c = spdk_crc32c_update(pdu->data, pdu->data_len, crc32c);

	mod = pdu->data_len % NVME_TCP_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = NVME_TCP_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}

	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}


static int
spdk_nvme_tcp_build_iovecs(struct nvme_tcp_qpair *tqpair, struct iovec *iovec,
			   struct nvme_tcp_pdu *pdu)
{

	int iovec_cnt = 0;
	uint32_t crc32c;
	int enable_digest;
	int hlen;

	hlen = pdu->hdr.common.hlen;
	enable_digest = 1;
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	/* Header Digest */
	if (enable_digest && tqpair->host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr.raw + hlen, crc32c);
		hlen += NVME_TCP_DIGEST_LEN;
	}

	/* PDU header + possible header digest */
	iovec[iovec_cnt].iov_base = &pdu->hdr.raw;
	iovec[iovec_cnt].iov_len = hlen;
	iovec_cnt++;

	if (!pdu->data_len) {
		return iovec_cnt;
	}

	/* Padding  */
	if (pdu->padding_len > 0) {
		iovec[iovec_cnt].iov_base = &pdu->hdr.raw + hlen;
		iovec[iovec_cnt].iov_len = pdu->padding_len;
		iovec_cnt++;
	}

	/* Data Segment */
	iovec[iovec_cnt].iov_base = pdu->data;
	iovec[iovec_cnt].iov_len = pdu->data_len;
	iovec_cnt++;

	/* Data Digest */
	if (enable_digest && tqpair->host_ddgst_enable) {
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
		iovec[iovec_cnt].iov_base = pdu->data_digest;
		iovec[iovec_cnt].iov_len = NVME_TCP_DIGEST_LEN;
		iovec_cnt++;
	}

	return iovec_cnt;
}

static int
nvme_tcp_qpair_process_send_queue(struct nvme_tcp_qpair *tqpair)
{
	const int array_size = 32;
	struct iovec	iovec_array[array_size];
	struct iovec	*iov = iovec_array;
	int iovec_cnt = 0;
	int bytes = 0;
	int total_length = 0;
	uint32_t writev_offset;
	struct nvme_tcp_pdu *pdu;
	int pdu_length;

	pdu = TAILQ_FIRST(&tqpair->send_queue);

	if (pdu == NULL) {
		return 0;
	}

	/*
	 * Build up a list of iovecs for the first few PDUs in the
	 *  tqpair 's send_queue.
	 */
	while (pdu != NULL && ((array_size - iovec_cnt) >= 4)) {
		pdu_length = pdu->hdr.common.plen;
		iovec_cnt += spdk_nvme_tcp_build_iovecs(tqpair,
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
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "bytes=%d are out\n", bytes);
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
		pdu_length = pdu->hdr.common.plen;
		pdu_length -= pdu->writev_offset;

		if (bytes >= pdu_length) {
			bytes -= pdu_length;
			TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
			assert(pdu->cb_fn != NULL);
			pdu->cb_fn(pdu->cb_arg);
			pdu = TAILQ_FIRST(&tqpair->send_queue);

		} else {
			pdu->writev_offset += bytes;
			bytes = 0;
		}
	}

	return TAILQ_EMPTY(&tqpair->send_queue) ? 0 : 1;

}

static int
nvme_tcp_qpair_write_pdu(struct nvme_tcp_qpair *tqpair,
			 struct nvme_tcp_pdu *pdu,
			 nvme_tcp_qpair_xfer_complete_cb cb_fn,
			 void *cb_arg)
{
	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	return 0;
	/* here is a debate, whether we need to send out the pdu immediately */
	//return nvme_tcp_qpair_process_send_queue(tqpair);
}


/*
 * Build SGL describing empty payload.
 */
static int
nvme_tcp_build_null_request(struct nvme_request *req)
{
	struct spdk_nvme_sgl_descriptor *nvme_sgl;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

	nvme_sgl = &req->cmd.dptr.sgl1;
	nvme_sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	nvme_sgl->unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	nvme_sgl->unkeyed.length = 0;
	nvme_sgl->address = 0;

	return 0;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
static int
nvme_tcp_build_contig_request(struct nvme_tcp_qpair *tqpair, struct nvme_request *req)
{
	void *payload = req->payload.contig_or_cb_arg + req->payload_offset;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;
	req->cmd.dptr.sgl1.address = (uint64_t)payload;

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static int
nvme_tcp_build_sgl_request(struct nvme_tcp_qpair *tqpair, struct nvme_request *req)
{
	int rc;
	void *virt_addr;
	uint32_t length;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	/* TODO: for now, we only support a single SGL entry */
	rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &virt_addr, &length);
	if (rc) {
		return -1;
	}

	if (length < req->payload_size) {
		SPDK_ERRLOG("multi-element SGL currently not supported for TCP now\n");
		return -1;
	}

	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;
	req->cmd.dptr.sgl1.address = (uint64_t)virt_addr;

	return 0;
}


static int
nvme_tcp_req_init(struct nvme_tcp_qpair *tqpair, struct nvme_request *req,
		  struct nvme_tcp_req *tcp_req)
{
	int rc = 0;
	enum spdk_nvme_data_transfer xfer;

	tcp_req->req = req;
	req->cmd.cid = tcp_req->cid;

	/* Here I have questions, we should use unkeyed right? */
	if (req->payload_size == 0) {
		rc = nvme_tcp_build_null_request(req);
	} else if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_tcp_build_contig_request(tqpair, req);
	} else if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL) {
		rc = nvme_tcp_build_sgl_request(tqpair, req);
	} else {
		rc = -1;
	}

	if (rc) {
		return rc;
	}

	xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);

	if (tqpair->in_capsule_data_size != 0 && xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
	    req->payload_size <= tqpair->in_capsule_data_size) {
		req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
		tcp_req->in_capsule_data = true;
	}

	return 0;

}

static void
nvme_tcp_qpair_cmd_send_complete(void *cb_arg)
{
}

/* This function should be fixed */
static void
nvme_tcp_pdu_set_data_buf(struct nvme_tcp_pdu *pdu,
			  struct nvme_tcp_req *tcp_req)
{
	/* Here is the tricky, we should consider different NVME data command type: SGL with continue or
	scatter data, now we only consider continous data, which is not corroct, shoud be fixed */

	pdu->data = (void *)tcp_req->req->cmd.dptr.sgl1.address + tcp_req->datao;
}

static int
nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
				struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t plen = 0, alignment;
	uint8_t pdo;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	pdu = &tcp_req->send_pdu;

	capsule_cmd = &pdu->hdr.capsule_cmd;
	capsule_cmd->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	plen = capsule_cmd->common.hlen = sizeof(*capsule_cmd);
	capsule_cmd->ccsqe = tcp_req->req->cmd;


	SPDK_DEBUGLOG(SPDK_LOG_NVME, "capsule_cmd cid=%u on tqpair(%p)\n", tcp_req->req->cmd.cid, tqpair);

	if (tqpair->host_hdgst_enable) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Header digest is enabled for capsule command on tcp_req=%p\n",
			      tcp_req);
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += NVME_TCP_DIGEST_LEN;
	}

	if ((tcp_req->req->payload_size == 0) || !tcp_req->in_capsule_data) {
		goto end;
	}

	pdo = plen;
	pdu->padding_len = 0;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			pdu->padding_len = alignment - plen;
			pdo = alignment;
			plen = alignment;
		}
	}

	capsule_cmd->common.pdo = pdo;
	plen += tcp_req->req->payload_size;
	if (tqpair->host_ddgst_enable) {
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += NVME_TCP_DIGEST_LEN;
	}

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req);
	pdu->data_len = tcp_req->req->payload_size;
	tcp_req->datao = 0;

end:
	capsule_cmd->common.plen = plen;
	return nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_qpair_cmd_send_complete, NULL);

}

int
nvme_tcp_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			      struct nvme_request *req)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req *tcp_req;

	tqpair = nvme_tcp_qpair(qpair);
	assert(tqpair != NULL);
	assert(req != NULL);

	tcp_req = nvme_tcp_req_get(tqpair);
	if (!tcp_req) {
		/*
		 * No tcp_req is available.  Queue the request to be processed later.
		 */
		STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
		return 0;
	}

	if (nvme_tcp_req_init(tqpair, req, tcp_req)) {
		SPDK_ERRLOG("nvme_tcp_req_init() failed\n");
		nvme_tcp_req_put(tqpair, tcp_req);
		return -1;
	}

	req->timed_out = false;
	if (spdk_unlikely(tqpair->qpair.ctrlr->timeout_enabled)) {
		req->submit_tick = spdk_get_ticks();
	} else {
		req->submit_tick = 0;
	}

	return nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);
}

int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return nvme_tcp_qpair_destroy(qpair);
}

int
nvme_tcp_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	// TODO
	return -1;
	//return nvme_tcp_qpair_connect(nvme_tcp_qpair(qpair));
}

int
nvme_tcp_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_tcp_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_tcp_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_tcp_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static void
nvme_tcp_qpair_set_recv_state(struct nvme_tcp_qpair *tqpair,
			      enum nvme_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	tqpair->recv_state = state;
	switch (state) {
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
	case NVME_TCP_PDU_RECV_STATE_ERROR:
		memset(&tqpair->recv_pdu, 0, sizeof(struct nvme_tcp_pdu));
		break;
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH_HDR:
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_HDR:
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
	default:
		break;
	}
}

static int
nvme_tcp_qpair_read_data(struct nvme_tcp_qpair *tqpair, int bytes,
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
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "spdk_sock_recv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	return NVME_TCP_CONNECTION_FATAL;
}

static void
nvme_tcp_qpair_send_h2c_term_req_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	tqpair->state = NVME_TCP_QPAIR_STATE_EXITING;
}


static void
nvme_tcp_qpair_send_h2c_term_req(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req;
	uint32_t h2c_term_req_hdr_len = sizeof(*h2c_term_req);

	rsp_pdu = &tqpair->send_pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	h2c_term_req = &rsp_pdu->hdr.term_req;
	h2c_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ;
	h2c_term_req->common.hlen = h2c_term_req_hdr_len;
	/* It should contain the header len of the received pdu */
	h2c_term_req->common.plen = h2c_term_req->common.hlen + pdu->hdr.common.hlen;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PRAMATER)) {
		DSET32(&h2c_term_req->fei, error_offset);
	}

	/* copy the error code into the buffer */
	rsp_pdu->data = (uint8_t *)rsp_pdu->hdr.raw + h2c_term_req_hdr_len;
	memcpy((uint8_t *)rsp_pdu->data, pdu->hdr.raw, sizeof(pdu->hdr.common.plen));

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_send_h2c_term_req_complete, NULL);

}

static void
nvme_tcp_pdu_ch_handle(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint32_t expected_hlen, hd_len = 0;
	bool plen_error = false;

	pdu = &tqpair->recv_pdu;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "pdu type = %d\n", pdu->hdr.common.pdu_type);
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP) {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received IC_RESP PDU, and we should reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_PDU_SEQUENCE;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
			SPDK_ERRLOG("The TCP/IP tqpair connection is not negotitated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_PDU_SEQUENCE;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
			expected_hlen = sizeof(struct spdk_nvme_tcp_rsp);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_c2h_data_hdr);
			if (pdu->hdr.common.plen < pdu->hdr.common.pdo) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr.common.plen <= expected_hlen) ||
			    (pdu->hdr.common.plen > NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_R2T:
			expected_hlen = sizeof(struct spdk_nvme_tcp_r2t_hdr);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu.hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr.common.hlen != expected_hlen) {
		SPDK_ERRLOG("Expected PDU header length %u, got %u\n",
			    expected_hlen, pdu->hdr.common.hlen);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;

	} else if (plen_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_HDR);
		return;
	}
err:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static struct nvme_tcp_req *
get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid)
{
	assert(tqpair != NULL);
	if ((cid >= tqpair->num_entries) || (tqpair->tcp_reqs[cid].state == NVME_TCP_REQ_FREE)) {
		return NULL;
	}

	return &tqpair->tcp_reqs[cid];
}

static void
nvme_tcp_req_complete(struct nvme_request *req,
		      struct spdk_nvme_cpl *rsp)
{
	nvme_complete_request(req, rsp);
	nvme_free_request(req);
}

static void
nvme_tcp_free_and_handle_queued_req(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request *req;

	if (!STAILQ_EMPTY(&qpair->queued_req) && !qpair->ctrlr->is_resetting) {
		req = STAILQ_FIRST(&qpair->queued_req);
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
		nvme_qpair_submit_request(qpair, req);
	}
}

static void
nvme_tcp_c2h_data_payload_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu, uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	struct spdk_nvme_cpl cpl = {};

	tcp_req = pdu->tcp_req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	c2h_data = &pdu->hdr.c2h_data;
	tcp_req->datao += pdu->data_len;

	if (c2h_data->common.flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
		if (tcp_req->datao == tcp_req->req->payload_size) {
			cpl.status.p = 0;
		} else {
			cpl.status.p = 1;
		}

		cpl.cid = tcp_req->cid;
		cpl.sqid = 0;
		nvme_tcp_req_complete(tcp_req->req, &cpl);
		nvme_tcp_req_put(tqpair, tcp_req);
		(*reaped)++;
		nvme_tcp_free_and_handle_queued_req(&tqpair->qpair);
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
}

static const char *spdk_nvme_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digiest Error",
	"Data Transfer Out of Range",
	"R2T Limit Exceeded",
	"Unsupported parameter",
};

static void
nvme_tcp_c2h_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *c2h_term_req)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Error info of pdu(%p): %s\n", c2h_term_req,
		      spdk_nvme_tcp_term_req_fes_str[c2h_term_req->fes]);
	if ((c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PRAMATER)) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "The offset from the start of the PDU header is %u\n",
			      DGET32(c2h_term_req->fei));
	}
	/* we may also need to dump some other info here */
}


static void
nvme_tcp_c2h_term_req_payload_handle(struct nvme_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	nvme_tcp_c2h_term_req_dump(&pdu->hdr.term_req);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
}

static void
nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair,
			    uint32_t *reaped)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	pdu = &tqpair->recv_pdu;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");

	/* check data digest if need */
	if (pdu->ddigest_valid_bytes) {
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HDGST;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;
		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_payload_handle(tqpair, pdu, reaped);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_payload_handle(tqpair, pdu);
		break;

	default:
		/* The code should not go to here */
		SPDK_ERRLOG("The code should not go to here\n");
		break;
	}
}


static void
nvme_tcp_send_icreq_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Complete the icreq send for tqpair=%p\n", tqpair);
}

static void
nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair,
		       struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_resp *ic_resp = &pdu->hdr.ic_resp;
	uint32_t error_offset = 0, maxh2cdata;
	enum spdk_nvme_tcp_term_req_fes fes;

	/* Only PFV 0 is defined currently */
	if (ic_resp->pfv != 0) {
		SPDK_ERRLOG("Expected ICResp PFV %u, got %u\n", 0u, ic_resp->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, pfv);
		goto end;
	}

	maxh2cdata = DGET32(ic_resp->maxh2cdata);
	if (maxh2cdata < NVME_TCP_PDU_H2C_MIN_DATA_SIZE) {
		SPDK_ERRLOG("Expected ICResp maxh2cdata >=%u, got %u\n", NVME_TCP_PDU_H2C_MIN_DATA_SIZE,
			    maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, maxh2cdata);
		goto end;
	}
	tqpair->maxh2cdata = maxh2cdata;

	if (ic_resp->cpda > NVME_TCP_CPDA_MAX) {
		SPDK_ERRLOG("Expected ICResp cpda <=%u, got %u\n", NVME_TCP_CPDA_MAX, ic_resp->cpda);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, cpda);
		goto end;
	}
	tqpair->cpda = ic_resp->cpda;

	tqpair->host_hdgst_enable = ic_resp->dgst.bits.hdgst_enable ? true : false;
	tqpair->host_ddgst_enable = ic_resp->dgst.bits.ddgst_enable ? true : false;
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	tqpair->state = NVME_TCP_QPAIR_STATE_RUNNING;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_capsule_resp_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_rsp *capsule_resp = &pdu->hdr.capsule_resp;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	cid = capsule_resp->rccqe.cid;

	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (!tcp_req) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "no tcp_req is found with cid=%u", cid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_rsp, rccqe);
		goto end;

	}

	assert(tcp_req->req != NULL);
	assert(tcp_req->state == NVME_TCP_REQ_ACTIVE);
	nvme_tcp_req_complete(tcp_req->req, &capsule_resp->rccqe);
	nvme_tcp_req_put(tqpair, tcp_req);
	(*reaped)++;
	nvme_tcp_free_and_handle_queued_req(&tqpair->qpair);

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "complete tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_c2h_term_req_hdr_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;


	if (c2h_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PRAMATER) {
		SPDK_ERRLOG("Fatal Error Stauts(FES) is unknown for c2h_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	pdu->data = (uint8_t *)pdu->hdr.raw + c2h_term_req->common.hlen;
	pdu->data_len = c2h_term_req->common.plen - c2h_term_req->common.hlen;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_c2h_data_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data = &pdu->hdr.c2h_data;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "c2h_data info on tqpair(%p): datao=%u, datal=%u, cccid=%d\n",
		      tqpair, c2h_data->datao, c2h_data->datal, c2h_data->cccid);
	tcp_req = get_nvme_active_req_by_cid(tqpair, c2h_data->cccid);
	if (!tcp_req) {
		SPDK_ERRLOG("no tcp_req found for c2hdata cid=%d\n", c2h_data->cccid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, cccid);
		goto end;

	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "tcp_req(%p) on tqpair(%p): datao=%u, payload_size=%u\n",
		      tcp_req, tqpair, tcp_req->datao, tcp_req->req->payload_size);

	if (c2h_data->datal > tcp_req->req->payload_size) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_TRANSFER_OUT_OF_RANGE;
		goto end;
	}

	if (tcp_req->datao != c2h_data->datao) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datao);
		goto end;
	}


	if ((c2h_data->datao + c2h_data->datal) > tcp_req->req->payload_size) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datal);
		goto end;

	}

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req);
	pdu->data_len = c2h_data->datal;
	pdu->tcp_req = tcp_req;

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_r2t_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_r2t_hdr *r2t = &pdu->hdr.r2t;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	uint32_t plen, pdo, alignment;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	cid = r2t->cccid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (!tcp_req) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Cannot find tcp_req for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, cccid);
		goto end;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "r2t info: r2to=%u, r2tl=%u for tqpair=%p\n", r2t->r2to, r2t->r2tl,
		      tqpair);

	if (tcp_req->state != NVME_TCP_REQ_ACTIVE_R2T) {
		if (tqpair->pending_r2t >= tqpair->max_r2t) {
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_PDU_SEQUENCE;
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "Invalid R2T: it exceed the R2T maixmal=%u\n", tqpair->max_r2t);
			goto end;
		}
		tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;
		tqpair->pending_r2t++;
	}

	if (tcp_req->datao != r2t->r2to) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2to);
		goto end;

	}

	if (r2t->r2tl > tqpair->maxh2cdata) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2tl);
		goto end;

	}

	if ((r2t->r2tl + r2t->r2to) > tcp_req->req->payload_size) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2tl);
		goto end;

	}

	rsp_pdu = &tcp_req->send_pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req);
	h2c_data = &rsp_pdu->hdr.h2c_data;

	h2c_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	plen = h2c_data->common.hlen = sizeof(*h2c_data);
	h2c_data->cccid = cid;
	h2c_data->ttag = r2t->ttag;
	h2c_data->datao = r2t->r2to;
	h2c_data->datal = r2t->r2tl;
	rsp_pdu->data_len = h2c_data->datal;

	if (tqpair->host_hdgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += NVME_TCP_DIGEST_LEN;
	}

	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			rsp_pdu->padding_len = alignment - plen;
			pdo = plen = alignment;
		}
	}

	h2c_data->common.pdo = pdo;
	plen += h2c_data->datal;
	if (tqpair->host_ddgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += NVME_TCP_DIGEST_LEN;
	}

	h2c_data->common.plen = plen;

	tcp_req->datao += h2c_data->datal;
	if ((r2t->r2tl + r2t->r2to) == tcp_req->req->payload_size) {
		tqpair->pending_r2t--;
		assert(tqpair->pending_r2t >= 0);
		tcp_req->state = NVME_TCP_REQ_ACTIVE; //reset to active state.
		h2c_data->common.flags |= SPDK_NVME_TCP_H2C_DATA_FLAGS_LAST_PDU;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "h2c_data info: datao=%u, datal=%u, pdu_len=%u for tqpair=%p\n",
		      h2c_data->datao, h2c_data->datal, h2c_data->common.plen, tqpair);

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_cmd_send_complete, NULL);
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;

}

static void
nvme_tcp_pdu_psh_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_HDR);
	pdu = &tqpair->recv_pdu;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter: pdu type =%u\n", pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HDGST;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_RESP:
		nvme_tcp_icresp_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
		nvme_tcp_capsule_resp_hdr_handle(tqpair, pdu, reaped);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_hdr_handle(tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_hdr_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_R2T:
		nvme_tcp_r2t_hdr_handle(tqpair, pdu);
		break;

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu.hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
		break;
	}

}

static int
nvme_tcp_read_pdu(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t psh_len, data_len, hlen, padding_len;
	enum nvme_tcp_pdu_recv_state prev_state;

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tqpair->recv_state;
		//SPDK_DEBUGLOG(SPDK_LOG_NVME, "tqpair%p recv pdu entering state %d\n", tqpair, prev_state);
		switch (tqpair->recv_state) {
		/* If in a new state */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH_HDR);
			break;
		/* common header */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH_HDR:
			pdu = &tqpair->recv_pdu;
			if (pdu->ch_valid_bytes < NVME_TCP_CH_LEN) {
				rc = nvme_tcp_qpair_read_data(tqpair,
							      NVME_TCP_CH_LEN - pdu->ch_valid_bytes,
							      (uint8_t *)&pdu->hdr.common + pdu->ch_valid_bytes);
				if (rc < 0) {
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
					break;
				}
				pdu->ch_valid_bytes += rc;
				if (pdu->ch_valid_bytes < NVME_TCP_CH_LEN) {
					return NVME_TCP_PDU_IN_PROGRESS;
				}
			}

			/* The command header of this PDU has now been read from the socket. */
			nvme_tcp_pdu_ch_handle(tqpair);
			rc = 0;
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_HDR:
			pdu = &tqpair->recv_pdu;
			hlen = pdu->hdr.common.hlen;
			psh_len = hlen - NVME_TCP_CH_LEN;

			/* The following pdus can have digest  */
			if (((pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP) ||
			     (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_DATA) ||
			     (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_R2T)) &&
			    tqpair->host_hdgst_enable) {
				psh_len += NVME_TCP_DIGEST_LEN;
				pdu->has_hdgst = true;
			}

			if (pdu->psh_valid_bytes < psh_len) {
				rc = nvme_tcp_qpair_read_data(tqpair,
							      psh_len - pdu->psh_valid_bytes,
							      (uint8_t *)&pdu->hdr.raw + NVME_TCP_CH_LEN + pdu->psh_valid_bytes);
				if (rc < 0) {
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
					break;
				}

				pdu->psh_valid_bytes += rc;
				if (pdu->psh_valid_bytes < psh_len) {
					return NVME_TCP_PDU_IN_PROGRESS;
				}
			}

			/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
			nvme_tcp_pdu_psh_handle(tqpair, reaped);
			rc = 0;
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			pdu = &tqpair->recv_pdu;
			/* check whether the data is valid, if not we just return */
			if (!pdu->data) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ) {
				if (pdu->data_valid_bytes < data_len) {
					rc = nvme_tcp_qpair_read_data(tqpair,
								      data_len - pdu->data_valid_bytes,
								      (uint8_t *)pdu->data + pdu->data_valid_bytes);
					if (rc < 0) {
						nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
						break;
					}

					pdu->data_valid_bytes += rc;
					if (pdu->data_valid_bytes < data_len) {
						return NVME_TCP_PDU_IN_PROGRESS;
					}
				}

			} else  {
				padding_len = pdu->hdr.common.pdo - pdu->hdr.common.hlen;
				if ((padding_len > 0) && tqpair->host_hdgst_enable) {
					padding_len -= NVME_TCP_DIGEST_LEN;
				}

				if (padding_len > 0 && pdu->padding_valid_bytes < padding_len) {
					/* reuse the space in raw */
					rc = nvme_tcp_qpair_read_data(tqpair,
								      padding_len - pdu->padding_valid_bytes,
								      (uint8_t *)pdu->hdr.raw + hlen + pdu->padding_valid_bytes);
					if (rc < 0) {
						nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
						break;
					}

					pdu->padding_valid_bytes += rc;
					if (pdu->padding_valid_bytes < padding_len) {
						return NVME_TCP_PDU_IN_PROGRESS;
					}
				}

				/* data len */
				if (pdu->data_valid_bytes < data_len) {
					rc = nvme_tcp_qpair_read_data(tqpair,
								      data_len - pdu->data_valid_bytes,
								      (uint8_t *)pdu->data + pdu->data_valid_bytes);
					if (rc < 0) {
						nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
						break;
					}

					pdu->data_valid_bytes += rc;
					if (pdu->data_valid_bytes < data_len) {
						return NVME_TCP_PDU_IN_PROGRESS;
					}
				}

				/* data digest */
				if (tqpair->host_ddgst_enable && pdu->ddigest_valid_bytes < NVME_TCP_DIGEST_LEN) {
					rc = nvme_tcp_qpair_read_data(tqpair,
								      NVME_TCP_DIGEST_LEN - pdu->ddigest_valid_bytes,
								      pdu->data_digest + pdu->ddigest_valid_bytes);
					if (rc < 0) {
						nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
						break;
					}

					pdu->ddigest_valid_bytes += rc;
					if (pdu->ddigest_valid_bytes < NVME_TCP_DIGEST_LEN) {
						return NVME_TCP_PDU_IN_PROGRESS;
					}
				}
			}

			/* All of this PDU has now been read from the socket. */
			nvme_tcp_pdu_payload_handle(tqpair, reaped);
			rc = 1;
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			rc = NVME_TCP_PDU_FATAL;
			break;
		default:
			assert(0);
			break;
		}
	} while (prev_state != tqpair->recv_state);

	return rc;
}

int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	uint32_t reaped;
	int rc;

	rc = nvme_tcp_qpair_process_send_queue(tqpair);
	if (rc) {
		return 0;
	}

	if (max_completions == 0) {
		max_completions = tqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, tqpair->num_entries);
	}

	reaped = 0;
	do {
		rc = nvme_tcp_read_pdu(tqpair, &reaped);
		if (rc < 0) {
			SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
				    errno, spdk_strerror(errno));
			/* this is only for debug, should be removed later  */
			exit(0);
			return -1;
		} else if (rc == 0) {
			/* Ran out of completions */
			break;
		}

	} while (reaped < max_completions);

	return reaped;
}

static int
nvme_tcp_qpair_icreq_send(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_tcp_ic_req *ic_req;
	struct nvme_tcp_pdu *pdu;

	pdu = &tqpair->send_pdu;
	memset(&tqpair->send_pdu, 0, sizeof(tqpair->send_pdu));
	ic_req = &pdu->hdr.ic_req;

	ic_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	ic_req->common.hlen = ic_req->common.plen = sizeof(*ic_req);

	ic_req->pfv = 0;

	DSET32(&ic_req->maxr2t, NVME_TCP_MAX_R2T_DEFAULT - 1);
	ic_req->hpda = NVME_TCP_HPDA_DEFAULT;

	/* Currently, always enable it here for debuging */
	ic_req->dgst.bits.hdgst_enable = 1;
	ic_req->dgst.bits.ddgst_enable = 1;

	nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_send_icreq_complete, tqpair);

	while (tqpair->state == NVME_TCP_QPAIR_STATE_INVALID) {
		nvme_tcp_qpair_process_completions(&tqpair->qpair, 0);
	}

	if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Failed to construct the tqpair=%p via correct icresp\n", tqpair);
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Succesfully construct the tqpair=%p via correct icresp\n", tqpair);

	return 0;
}

static int
nvme_tcp_qpair_connect(struct nvme_tcp_qpair *tqpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr;
	int family;

	ctrlr = tqpair->qpair.ctrlr;

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
	rc = nvme_tcp_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_tcp_parse_addr() failed\n");
		return -1;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_tcp_parse_addr(&src_addr, family, ctrlr->opts.src_addr, ctrlr->opts.src_svcid);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_tcp_parse_addr() failed\n");
			return -1;
		}
	}

	tqpair->sock = spdk_sock_connect(ctrlr->trid.traddr, atoi(ctrlr->trid.trsvcid));
	if (!tqpair->sock) {
		SPDK_ERRLOG("sock connection error of tqpair=%p with addr=%s, port=%d\n",
			    tqpair, ctrlr->trid.traddr, atoi(ctrlr->trid.trsvcid));
		return -1;
	}

	rc = nvme_tcp_qpair_socket_init(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to init the socket of tqpair=%p\n", tqpair);
		return -1;
	}

	tqpair->max_r2t = NVME_TCP_MAX_R2T_DEFAULT;
	rc = nvme_tcp_alloc_reqs(tqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate tqpair tcp requests\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "TCP requests allocated\n");

	rc = nvme_tcp_qpair_icreq_send(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to connect the tqpair\n");
		return -1;
	}

	rc = nvme_fabric_qpair_connect(&tqpair->qpair, tqpair->num_entries);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		return -1;
	}

	return 0;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			    uint16_t qid, uint32_t qsize,
			    enum spdk_nvme_qprio qprio,
			    uint32_t num_requests)
{
	struct nvme_tcp_qpair *tqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	tqpair = calloc(1, sizeof(struct nvme_tcp_qpair));
	if (!tqpair) {
		SPDK_ERRLOG("failed to get create tqpair\n");
		return NULL;
	}

	tqpair->num_entries = qsize;

	qpair = &tqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests);
	if (rc != 0) {
		return NULL;
	}

	rc = nvme_tcp_qpair_connect(tqpair);
	if (rc < 0) {
		nvme_tcp_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			       const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_tcp_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests);
}

struct spdk_nvme_ctrlr *nvme_tcp_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct nvme_tcp_ctrlr *tctrlr;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_vs_register vs;
	int rc;

	tctrlr = calloc(1, sizeof(*tctrlr));
	if (tctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	tctrlr->ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_TCP;
	tctrlr->ctrlr.opts = *opts;
	tctrlr->ctrlr.trid = *trid;

	rc = nvme_ctrlr_construct(&tctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	tctrlr->ctrlr.adminq = nvme_tcp_ctrlr_create_qpair(&tctrlr->ctrlr, 0,
			       SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES, 0, SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES);
	if (!tctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		return NULL;
	}

	if (nvme_ctrlr_get_cap(&tctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	if (nvme_ctrlr_get_vs(&tctrlr->ctrlr, &vs)) {
		SPDK_ERRLOG("get_vs() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	nvme_ctrlr_init_cap(&tctrlr->ctrlr, &cap, &vs);

	return &tctrlr->ctrlr;
}

uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	// /* Todo, which should get from the NVMF target */
	return NVME_TCP_RW_BUFFER_SIZE;
}

uint16_t
nvme_tcp_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
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
nvme_tcp_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	return NULL;
}

int
nvme_tcp_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	return 0;
}
