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

#ifndef SPDK_INTERNAL_NVME_TCP_H
#define SPDK_INTERNAL_NVME_TCP_H

#include "spdk/sock.h"

#define SPDK_CRC32C_XOR				0xffffffffUL
#define SPDK_NVME_TCP_DIGEST_LEN		4
#define SPDK_NVME_TCP_DIGEST_ALIGNMENT		4
#define SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT	30

/*
 * Maximum number of SGL elements.
 */
#define NVME_TCP_MAX_SGL_DESCRIPTORS	(16)

#define MAKE_DIGEST_WORD(BUF, CRC32C) \
        (   ((*((uint8_t *)(BUF)+0)) = (uint8_t)((uint32_t)(CRC32C) >> 0)), \
            ((*((uint8_t *)(BUF)+1)) = (uint8_t)((uint32_t)(CRC32C) >> 8)), \
            ((*((uint8_t *)(BUF)+2)) = (uint8_t)((uint32_t)(CRC32C) >> 16)), \
            ((*((uint8_t *)(BUF)+3)) = (uint8_t)((uint32_t)(CRC32C) >> 24)))

#define MATCH_DIGEST_WORD(BUF, CRC32C) \
        (    ((((uint32_t) *((uint8_t *)(BUF)+0)) << 0)         \
            | (((uint32_t) *((uint8_t *)(BUF)+1)) << 8)         \
            | (((uint32_t) *((uint8_t *)(BUF)+2)) << 16)        \
            | (((uint32_t) *((uint8_t *)(BUF)+3)) << 24))       \
            == (CRC32C))

#define DGET32(B)                                                               \
        (((  (uint32_t) *((uint8_t *)(B)+0)) << 0)                              \
         | (((uint32_t) *((uint8_t *)(B)+1)) << 8)                              \
         | (((uint32_t) *((uint8_t *)(B)+2)) << 16)                             \
         | (((uint32_t) *((uint8_t *)(B)+3)) << 24))

#define DSET32(B,D)                                                             \
        (((*((uint8_t *)(B)+0)) = (uint8_t)((uint32_t)(D) >> 0)),               \
         ((*((uint8_t *)(B)+1)) = (uint8_t)((uint32_t)(D) >> 8)),               \
         ((*((uint8_t *)(B)+2)) = (uint8_t)((uint32_t)(D) >> 16)),              \
         ((*((uint8_t *)(B)+3)) = (uint8_t)((uint32_t)(D) >> 24)))

typedef void (*nvme_tcp_qpair_xfer_complete_cb)(void *cb_arg);

struct _iov_ctx {
	struct iovec    *iov;
	int             num_iovs;
	uint32_t        iov_offset;
	int             iovcnt;
	uint32_t        mapped_len;
};

struct nvme_tcp_pdu {
	union {
		/* to hold error pdu data */
		uint8_t					raw[SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE];
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
	bool						ddgst_enable;
	uint8_t						data_digest[SPDK_NVME_TCP_DIGEST_LEN];
	int32_t						padding_valid_bytes;

	uint32_t					ch_valid_bytes;
	uint32_t					psh_valid_bytes;

	nvme_tcp_qpair_xfer_complete_cb			cb_fn;
	void						*cb_arg;
	int						ref;
	struct iovec					data_iov[NVME_TCP_MAX_SGL_DESCRIPTORS];
	uint32_t					data_iovcnt;
	uint32_t					data_len;

	uint32_t					readv_offset;
	uint32_t					writev_offset;
	TAILQ_ENTRY(nvme_tcp_pdu)			tailq;
	uint32_t					remaining;
	uint32_t					padding_len;
	struct _iov_ctx					iov_ctx;

	void						*ctx; /* data tied to a tcp request */
};

enum nvme_tcp_pdu_recv_state {
	/* Ready to wait for PDU */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY,

	/* Active tqpair waiting for any PDU common header */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH,

	/* Active tqpair waiting for any PDU specific header */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH,

	/* Active tqpair waiting for payload */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD,

	/* Active tqpair does not wait for payload */
	NVME_TCP_PDU_RECV_STATE_ERROR,
};

enum nvme_tcp_error_codes {
	NVME_TCP_PDU_IN_PROGRESS        = 0,
	NVME_TCP_CONNECTION_FATAL       = -1,
	NVME_TCP_PDU_FATAL              = -2,
};

enum nvme_tcp_qpair_state {
	NVME_TCP_QPAIR_STATE_INVALID = 0,
	NVME_TCP_QPAIR_STATE_RUNNING = 1,
	NVME_TCP_QPAIR_STATE_EXITING = 2,
	NVME_TCP_QPAIR_STATE_EXITED = 3,
};

static uint32_t
nvme_tcp_pdu_calc_header_digest(struct nvme_tcp_pdu *pdu)
{
	uint32_t crc32c;
	uint32_t hlen = pdu->hdr.common.hlen;

	crc32c = spdk_crc32c_update(&pdu->hdr.raw, hlen, ~0);
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static uint32_t
nvme_tcp_pdu_calc_data_digest(struct nvme_tcp_pdu *pdu)
{
	uint32_t crc32c = SPDK_CRC32C_XOR;
	uint32_t mod;
	uint32_t i;

	assert(pdu->data_len != 0);

	for (i = 0; i < pdu->data_iovcnt; i++) {
		assert(pdu->data_iov[i].iov_base != NULL);
		assert(pdu->data_iov[i].iov_len != 0);
		crc32c = spdk_crc32c_update(pdu->data_iov[i].iov_base, pdu->data_iov[i].iov_len, crc32c);
	}

	mod = pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = SPDK_NVME_TCP_DIGEST_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static inline void
_iov_ctx_init(struct _iov_ctx *ctx, struct iovec *iovs, int num_iovs,
	      uint32_t iov_offset)
{
	ctx->iov = iovs;
	ctx->num_iovs = num_iovs;
	ctx->iov_offset = iov_offset;
	ctx->iovcnt = 0;
	ctx->mapped_len = 0;
}

static inline bool
_iov_ctx_set_iov(struct _iov_ctx *ctx, uint8_t *data, uint32_t data_len)
{
	if (ctx->iov_offset >= data_len) {
		ctx->iov_offset -= data_len;
	} else {
		ctx->iov->iov_base = data + ctx->iov_offset;
		ctx->iov->iov_len = data_len - ctx->iov_offset;
		ctx->mapped_len += data_len - ctx->iov_offset;
		ctx->iov_offset = 0;
		ctx->iov++;
		ctx->iovcnt++;
		if (ctx->iovcnt == ctx->num_iovs) {
			return false;
		}
	}

	return true;
}

static int
nvme_tcp_build_iovecs(struct iovec *iovec, int num_iovs, struct nvme_tcp_pdu *pdu,
		      bool hdgst_enable, bool ddgst_enable, uint32_t *_mapped_length)
{
	int enable_digest;
	uint32_t hlen, plen, i;
	struct _iov_ctx *ctx;

	if (num_iovs == 0) {
		return 0;
	}

	ctx = &pdu->iov_ctx;
	_iov_ctx_init(ctx, iovec, num_iovs, pdu->writev_offset);
	hlen = pdu->hdr.common.hlen;
	enable_digest = 1;
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ ||
	    pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	/* Header Digest */
	if (enable_digest && hdgst_enable) {
		hlen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	plen = hlen;
	if (!pdu->data_len) {
		/* PDU header + possible header digest */
		_iov_ctx_set_iov(ctx, (uint8_t *)&pdu->hdr.raw, hlen);
		goto end;
	}

	/* Padding */
	if (pdu->padding_len > 0) {
		hlen += pdu->padding_len;
		plen = hlen;
	}

	if (!_iov_ctx_set_iov(ctx, (uint8_t *)&pdu->hdr.raw, hlen)) {
		goto end;
	}

	/* Data Segment */
	plen += pdu->data_len;
	for (i = 0; i < pdu->data_iovcnt; i++) {
		if (!_iov_ctx_set_iov(ctx, pdu->data_iov[i].iov_base, pdu->data_iov[i].iov_len)) {
			goto end;
		}
	}

	/* Data Digest */
	if (enable_digest && ddgst_enable) {
		plen += SPDK_NVME_TCP_DIGEST_LEN;
		_iov_ctx_set_iov(ctx, pdu->data_digest, SPDK_NVME_TCP_DIGEST_LEN);
	}

end:
	if (_mapped_length != NULL) {
		*_mapped_length = ctx->mapped_len;
	}

	/* check the plen for the first time constructing iov */
	if (!pdu->writev_offset) {
		assert(plen == pdu->hdr.common.plen);
	}

	return ctx->iovcnt;
}

static int
nvme_tcp_build_payload_iovecs(struct iovec *iovec, int num_iovs, struct nvme_tcp_pdu *pdu,
			      bool ddgst_enable, uint32_t *_mapped_length)
{
	struct _iov_ctx *ctx;
	uint32_t i;

	if (num_iovs == 0) {
		return 0;
	}

	ctx = &pdu->iov_ctx;
	_iov_ctx_init(ctx, iovec, num_iovs, pdu->readv_offset);

	for (i = 0; i < pdu->data_iovcnt; i++) {
		if (!_iov_ctx_set_iov(ctx, pdu->data_iov[i].iov_base, pdu->data_iov[i].iov_len)) {
			goto end;
		}
	}

	/* Data Digest */
	if (ddgst_enable) {
		_iov_ctx_set_iov(ctx, pdu->data_digest, SPDK_NVME_TCP_DIGEST_LEN);
	}

end:
	if (_mapped_length != NULL) {
		*_mapped_length = ctx->mapped_len;
	}
	return ctx->iovcnt;
}

static int
nvme_tcp_read_data(struct spdk_sock *sock, int bytes,
		   void *buf)
{
	int ret;

	ret = spdk_sock_recv(sock, buf, bytes);

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

	/* connection closed */
	return NVME_TCP_CONNECTION_FATAL;
}

static int
nvme_tcp_readv_data(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	int ret;

	assert(sock != NULL);
	if (iov == NULL || iovcnt == 0) {
		return 0;
	}

	if (iovcnt == 1) {
		return nvme_tcp_read_data(sock, iov->iov_len, iov->iov_base);
	}

	ret = spdk_sock_readv(sock, iov, iovcnt);

	if (ret > 0) {
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno == ECONNRESET) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "spdk_sock_readv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_readv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	/* connection closed */
	return NVME_TCP_CONNECTION_FATAL;
}


static int
nvme_tcp_read_payload_data(struct spdk_sock *sock, struct nvme_tcp_pdu *pdu)
{
	struct iovec iovec_array[NVME_TCP_MAX_SGL_DESCRIPTORS + 1];
	struct iovec *iov = iovec_array;
	int iovec_cnt;

	iovec_cnt = nvme_tcp_build_payload_iovecs(iovec_array, NVME_TCP_MAX_SGL_DESCRIPTORS + 1, pdu,
			pdu->ddgst_enable, NULL);
	assert(iovec_cnt >= 0);

	return nvme_tcp_readv_data(sock, iov, iovec_cnt);
}

static void
nvme_tcp_pdu_set_data(struct nvme_tcp_pdu *pdu, void *data, uint32_t data_len)
{
	pdu->data_iov[0].iov_base = data;
	pdu->data_iov[0].iov_len = pdu->data_len = data_len;
	pdu->data_iovcnt = 1;
}

#endif /* SPDK_INTERNAL_NVME_TCP_H */
