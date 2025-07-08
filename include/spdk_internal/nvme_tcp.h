/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_INTERNAL_NVME_TCP_H
#define SPDK_INTERNAL_NVME_TCP_H

#include "spdk/likely.h"
#include "spdk/sock.h"
#include "spdk/dif.h"
#include "spdk/hexlify.h"
#include "spdk/nvmf_spec.h"
#include "spdk/util.h"
#include "spdk/base64.h"

#include "sgl.h"

#include "openssl/evp.h"
#include "openssl/kdf.h"
#include "openssl/sha.h"

#define SPDK_CRC32C_XOR				0xffffffffUL
#define SPDK_NVME_TCP_DIGEST_LEN		4
#define SPDK_NVME_TCP_DIGEST_ALIGNMENT		4
#define SPDK_NVME_TCP_QPAIR_EXIT_TIMEOUT	30
#define SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR	8
#define SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE	8192u
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

/* The PSK identity comprises of following components:
 * 4-character format specifier "NVMe" +
 * 1-character TLS protocol version indicator +
 * 1-character PSK type indicator, specifying the used PSK +
 * 2-characters hash specifier +
 * NQN of the host (SPDK_NVMF_NQN_MAX_LEN -> 223) +
 * NQN of the subsystem (SPDK_NVMF_NQN_MAX_LEN -> 223) +
 * 2 space character separators +
 * 1 null terminator =
 * 457 characters. */
#define NVMF_PSK_IDENTITY_LEN (SPDK_NVMF_NQN_MAX_LEN + SPDK_NVMF_NQN_MAX_LEN + 11)

/* The maximum size of hkdf_info is defined by RFC 8446, 514B (2 + 256 + 256). */
#define NVME_TCP_HKDF_INFO_MAX_LEN 514

#define PSK_ID_PREFIX "NVMe0R"

enum nvme_tcp_cipher_suite {
	NVME_TCP_CIPHER_AES_128_GCM_SHA256,
	NVME_TCP_CIPHER_AES_256_GCM_SHA384,
};

typedef void (*nvme_tcp_qpair_xfer_complete_cb)(void *cb_arg);

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
	uint32_t					data_digest_crc32;
	uint8_t						data_digest[SPDK_NVME_TCP_DIGEST_LEN];

	uint8_t						ch_valid_bytes;
	uint8_t						psh_valid_bytes;
	uint8_t						psh_len;

	nvme_tcp_qpair_xfer_complete_cb			cb_fn;
	void						*cb_arg;

	/* The sock request ends with a 0 length iovec. Place the actual iovec immediately
	 * after it. There is a static assert below to check if the compiler inserted
	 * any unwanted padding */
	struct spdk_sock_request			sock_req;
	struct iovec					iov[NVME_TCP_MAX_SGL_DESCRIPTORS * 2];

	struct iovec					data_iov[NVME_TCP_MAX_SGL_DESCRIPTORS];
	uint32_t					data_iovcnt;
	uint32_t					data_len;

	uint32_t					rw_offset;
	TAILQ_ENTRY(nvme_tcp_pdu)			tailq;
	uint32_t					remaining;
	uint32_t					padding_len;

	struct spdk_dif_ctx				*dif_ctx;

	void						*req; /* data tied to a tcp request */
	void						*qpair;
	SLIST_ENTRY(nvme_tcp_pdu)			slist;
};
SPDK_STATIC_ASSERT(offsetof(struct nvme_tcp_pdu,
			    sock_req) + sizeof(struct spdk_sock_request) == offsetof(struct nvme_tcp_pdu, iov),
		   "Compiler inserted padding between iov and sock_req");

enum nvme_tcp_pdu_recv_state {
	/* Ready to wait for PDU */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY,

	/* Active tqpair waiting for any PDU common header */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH,

	/* Active tqpair waiting for any PDU specific header */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH,

	/* Active tqpair waiting for a tcp request, only use in target side */
	NVME_TCP_PDU_RECV_STATE_AWAIT_REQ,

	/* Active tqpair waiting for a free buffer to store PDU */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_BUF,

	/* Active tqpair waiting for payload */
	NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD,

	/* Active tqpair waiting for all outstanding PDUs to complete */
	NVME_TCP_PDU_RECV_STATE_QUIESCING,

	/* Active tqpair does not wait for payload */
	NVME_TCP_PDU_RECV_STATE_ERROR,
};

enum nvme_tcp_error_codes {
	NVME_TCP_PDU_IN_PROGRESS        = 0,
	NVME_TCP_CONNECTION_FATAL       = -1,
	NVME_TCP_PDU_FATAL              = -2,
};

static const bool g_nvme_tcp_hdgst[] = {
	[SPDK_NVME_TCP_PDU_TYPE_IC_REQ]         = false,
	[SPDK_NVME_TCP_PDU_TYPE_IC_RESP]        = false,
	[SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ]   = false,
	[SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ]   = false,
	[SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD]    = true,
	[SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP]   = true,
	[SPDK_NVME_TCP_PDU_TYPE_H2C_DATA]       = true,
	[SPDK_NVME_TCP_PDU_TYPE_C2H_DATA]       = true,
	[SPDK_NVME_TCP_PDU_TYPE_R2T]            = true
};

static const bool g_nvme_tcp_ddgst[] = {
	[SPDK_NVME_TCP_PDU_TYPE_IC_REQ]         = false,
	[SPDK_NVME_TCP_PDU_TYPE_IC_RESP]        = false,
	[SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ]   = false,
	[SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ]   = false,
	[SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD]    = true,
	[SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP]   = false,
	[SPDK_NVME_TCP_PDU_TYPE_H2C_DATA]       = true,
	[SPDK_NVME_TCP_PDU_TYPE_C2H_DATA]       = true,
	[SPDK_NVME_TCP_PDU_TYPE_R2T]            = false
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

	assert(pdu->data_len != 0);

	if (spdk_likely(!pdu->dif_ctx)) {
		crc32c = spdk_crc32c_iov_update(pdu->data_iov, pdu->data_iovcnt, crc32c);
	} else {
		spdk_dif_update_crc32c_stream(pdu->data_iov, pdu->data_iovcnt,
					      0, pdu->data_len, &crc32c, pdu->dif_ctx);
	}

	mod = pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = SPDK_NVME_TCP_DIGEST_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}
	return crc32c;
}

static inline void
_nvme_tcp_sgl_get_buf(struct spdk_iov_sgl *s, void **_buf, uint32_t *_buf_len)
{
	if (_buf != NULL) {
		*_buf = (uint8_t *)s->iov->iov_base + s->iov_offset;
	}
	if (_buf_len != NULL) {
		*_buf_len = s->iov->iov_len - s->iov_offset;
	}
}

static inline bool
_nvme_tcp_sgl_append_multi(struct spdk_iov_sgl *s, struct iovec *iov, int iovcnt)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (!spdk_iov_sgl_append(s, iov[i].iov_base, iov[i].iov_len)) {
			return false;
		}
	}

	return true;
}

static inline uint32_t
_get_iov_array_size(struct iovec *iov, int iovcnt)
{
	int i;
	uint32_t size = 0;

	for (i = 0; i < iovcnt; i++) {
		size += iov[i].iov_len;
	}

	return size;
}

static inline bool
_nvme_tcp_sgl_append_multi_with_md(struct spdk_iov_sgl *s, struct iovec *iov, int iovcnt,
				   uint32_t data_len, const struct spdk_dif_ctx *dif_ctx)
{
	int rc;
	uint32_t mapped_len = 0;

	if (s->iov_offset >= data_len) {
		s->iov_offset -= _get_iov_array_size(iov, iovcnt);
	} else {
		rc = spdk_dif_set_md_interleave_iovs(s->iov, s->iovcnt, iov, iovcnt,
						     s->iov_offset, data_len - s->iov_offset,
						     &mapped_len, dif_ctx);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to setup iovs for DIF insert/strip.\n");
			return false;
		}

		s->total_size += mapped_len;
		s->iov_offset = 0;
		assert(s->iovcnt >= rc);
		s->iovcnt -= rc;
		s->iov += rc;

		if (s->iovcnt == 0) {
			return false;
		}
	}

	return true;
}

static int
nvme_tcp_build_iovs(struct iovec *iov, int iovcnt, struct nvme_tcp_pdu *pdu,
		    bool hdgst_enable, bool ddgst_enable, uint32_t *_mapped_length)
{
	uint32_t hlen;
	uint32_t plen __attribute__((unused));
	struct spdk_iov_sgl sgl;

	if (iovcnt == 0) {
		return 0;
	}

	spdk_iov_sgl_init(&sgl, iov, iovcnt, 0);
	hlen = pdu->hdr.common.hlen;

	/* Header Digest */
	if (g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type] && hdgst_enable) {
		hlen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	plen = hlen;
	if (!pdu->data_len) {
		/* PDU header + possible header digest */
		spdk_iov_sgl_append(&sgl, (uint8_t *)&pdu->hdr.raw, hlen);
		goto end;
	}

	/* Padding */
	if (pdu->padding_len > 0) {
		hlen += pdu->padding_len;
		plen = hlen;
	}

	if (!spdk_iov_sgl_append(&sgl, (uint8_t *)&pdu->hdr.raw, hlen)) {
		goto end;
	}

	/* Data Segment */
	plen += pdu->data_len;
	if (spdk_likely(!pdu->dif_ctx)) {
		if (!_nvme_tcp_sgl_append_multi(&sgl, pdu->data_iov, pdu->data_iovcnt)) {
			goto end;
		}
	} else {
		if (!_nvme_tcp_sgl_append_multi_with_md(&sgl, pdu->data_iov, pdu->data_iovcnt,
							pdu->data_len, pdu->dif_ctx)) {
			goto end;
		}
	}

	/* Data Digest */
	if (g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type] && ddgst_enable) {
		plen += SPDK_NVME_TCP_DIGEST_LEN;
		spdk_iov_sgl_append(&sgl, pdu->data_digest, SPDK_NVME_TCP_DIGEST_LEN);
	}

	assert(plen == pdu->hdr.common.plen);

end:
	if (_mapped_length != NULL) {
		*_mapped_length = sgl.total_size;
	}

	return iovcnt - sgl.iovcnt;
}

static int
nvme_tcp_build_payload_iovs(struct iovec *iov, int iovcnt, struct nvme_tcp_pdu *pdu,
			    bool ddgst_enable, uint32_t *_mapped_length)
{
	struct spdk_iov_sgl sgl;

	if (iovcnt == 0) {
		return 0;
	}

	spdk_iov_sgl_init(&sgl, iov, iovcnt, pdu->rw_offset);

	if (spdk_likely(!pdu->dif_ctx)) {
		if (!_nvme_tcp_sgl_append_multi(&sgl, pdu->data_iov, pdu->data_iovcnt)) {
			goto end;
		}
	} else {
		if (!_nvme_tcp_sgl_append_multi_with_md(&sgl, pdu->data_iov, pdu->data_iovcnt,
							pdu->data_len, pdu->dif_ctx)) {
			goto end;
		}
	}

	/* Data Digest */
	if (ddgst_enable) {
		spdk_iov_sgl_append(&sgl, pdu->data_digest, SPDK_NVME_TCP_DIGEST_LEN);
	}

end:
	if (_mapped_length != NULL) {
		*_mapped_length = sgl.total_size;
	}
	return iovcnt - sgl.iovcnt;
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
		if (errno != ECONNRESET) {
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
		if (errno != ECONNRESET) {
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
	struct iovec iov[NVME_TCP_MAX_SGL_DESCRIPTORS + 1];
	int iovcnt;

	iovcnt = nvme_tcp_build_payload_iovs(iov, NVME_TCP_MAX_SGL_DESCRIPTORS + 1, pdu,
					     pdu->ddgst_enable, NULL);
	assert(iovcnt >= 0);

	return nvme_tcp_readv_data(sock, iov, iovcnt);
}

static void
_nvme_tcp_pdu_set_data(struct nvme_tcp_pdu *pdu, void *data, uint32_t data_len)
{
	pdu->data_iov[0].iov_base = data;
	pdu->data_iov[0].iov_len = data_len;
	pdu->data_iovcnt = 1;
}

static void
nvme_tcp_pdu_set_data(struct nvme_tcp_pdu *pdu, void *data, uint32_t data_len)
{
	_nvme_tcp_pdu_set_data(pdu, data, data_len);
	pdu->data_len = data_len;
}

static void
nvme_tcp_pdu_set_data_buf(struct nvme_tcp_pdu *pdu,
			  struct iovec *iov, int iovcnt,
			  uint32_t data_offset, uint32_t data_len)
{
	uint32_t buf_offset, buf_len, remain_len, len;
	uint8_t *buf;
	struct spdk_iov_sgl pdu_sgl, buf_sgl;

	pdu->data_len = data_len;

	if (spdk_likely(!pdu->dif_ctx)) {
		buf_offset = data_offset;
		buf_len = data_len;
	} else {
		spdk_dif_ctx_set_data_offset(pdu->dif_ctx, data_offset);
		spdk_dif_get_range_with_md(data_offset, data_len,
					   &buf_offset, &buf_len, pdu->dif_ctx);
	}

	if (iovcnt == 1) {
		_nvme_tcp_pdu_set_data(pdu, (void *)((uint64_t)iov[0].iov_base + buf_offset), buf_len);
	} else {
		spdk_iov_sgl_init(&pdu_sgl, pdu->data_iov, NVME_TCP_MAX_SGL_DESCRIPTORS, 0);
		spdk_iov_sgl_init(&buf_sgl, iov, iovcnt, 0);

		spdk_iov_sgl_advance(&buf_sgl, buf_offset);
		remain_len = buf_len;

		while (remain_len > 0) {
			_nvme_tcp_sgl_get_buf(&buf_sgl, (void *)&buf, &len);
			len = spdk_min(len, remain_len);

			spdk_iov_sgl_advance(&buf_sgl, len);
			remain_len -= len;

			if (!spdk_iov_sgl_append(&pdu_sgl, buf, len)) {
				break;
			}
		}

		assert(remain_len == 0);
		assert(pdu_sgl.total_size == buf_len);

		pdu->data_iovcnt = NVME_TCP_MAX_SGL_DESCRIPTORS - pdu_sgl.iovcnt;
	}
}

static void
nvme_tcp_pdu_calc_psh_len(struct nvme_tcp_pdu *pdu, bool hdgst_enable)
{
	uint8_t psh_len, pdo, padding_len;

	psh_len = pdu->hdr.common.hlen;

	if (g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type] && hdgst_enable) {
		pdu->has_hdgst = true;
		psh_len += SPDK_NVME_TCP_DIGEST_LEN;
	}
	if (pdu->hdr.common.plen > psh_len) {
		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
		case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
		case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
			pdo = pdu->hdr.common.pdo;
			padding_len = pdo - psh_len;
			if (padding_len > 0) {
				psh_len = pdo;
			}
			break;
		default:
			/* There is no padding for other PDU types */
			break;
		}
	}

	psh_len -= sizeof(struct spdk_nvme_tcp_common_pdu_hdr);
	pdu->psh_len = psh_len;
}

static inline int
nvme_tcp_generate_psk_identity(char *out_id, size_t out_id_len, const char *hostnqn,
			       const char *subnqn, enum nvme_tcp_cipher_suite tls_cipher_suite)
{
	int rc;

	assert(out_id != NULL);

	if (out_id_len < strlen(PSK_ID_PREFIX) + strlen(hostnqn) + strlen(subnqn) + 5) {
		SPDK_ERRLOG("Out buffer too small!\n");
		return -1;
	}

	if (tls_cipher_suite == NVME_TCP_CIPHER_AES_128_GCM_SHA256) {
		rc = snprintf(out_id, out_id_len, "%s%s %s %s", PSK_ID_PREFIX, "01",
			      hostnqn, subnqn);
	} else if (tls_cipher_suite == NVME_TCP_CIPHER_AES_256_GCM_SHA384) {
		rc = snprintf(out_id, out_id_len, "%s%s %s %s", PSK_ID_PREFIX, "02",
			      hostnqn, subnqn);
	} else {
		SPDK_ERRLOG("Unknown cipher suite requested!\n");
		return -EOPNOTSUPP;
	}

	if (rc < 0) {
		SPDK_ERRLOG("Could not generate PSK identity\n");
		return -1;
	}

	return 0;
}

enum nvme_tcp_hash_algorithm {
	NVME_TCP_HASH_ALGORITHM_NONE,
	NVME_TCP_HASH_ALGORITHM_SHA256,
	NVME_TCP_HASH_ALGORITHM_SHA384,
};

static inline int
nvme_tcp_derive_retained_psk(const uint8_t *psk_in, uint64_t psk_in_size, const char *hostnqn,
			     uint8_t *psk_out, uint64_t psk_out_len, enum nvme_tcp_hash_algorithm psk_retained_hash)
{
	EVP_PKEY_CTX *ctx;
	uint64_t digest_len;
	uint8_t hkdf_info[NVME_TCP_HKDF_INFO_MAX_LEN] = {};
	const char *label = "tls13 HostNQN";
	size_t pos, labellen, nqnlen;
	const EVP_MD *hash;
	int rc, hkdf_info_size;

	labellen = strlen(label);
	nqnlen = strlen(hostnqn);
	assert(nqnlen <= SPDK_NVMF_NQN_MAX_LEN);

	*(uint16_t *)&hkdf_info[0] = htons(psk_in_size);
	pos = sizeof(uint16_t);
	hkdf_info[pos] = (uint8_t)labellen;
	pos += sizeof(uint8_t);
	memcpy(&hkdf_info[pos], label, labellen);
	pos += labellen;
	hkdf_info[pos] = (uint8_t)nqnlen;
	pos += sizeof(uint8_t);
	memcpy(&hkdf_info[pos], hostnqn, nqnlen);
	pos += nqnlen;
	hkdf_info_size = pos;

	switch (psk_retained_hash) {
	case NVME_TCP_HASH_ALGORITHM_SHA256:
		digest_len = SHA256_DIGEST_LENGTH;
		hash = EVP_sha256();
		break;
	case NVME_TCP_HASH_ALGORITHM_SHA384:
		digest_len = SHA384_DIGEST_LENGTH;
		hash = EVP_sha384();
		break;
	default:
		SPDK_ERRLOG("Unknown PSK hash requested!\n");
		return -EOPNOTSUPP;
	}

	if (digest_len > psk_out_len) {
		SPDK_ERRLOG("Insufficient buffer size for out key!\n");
		return -EINVAL;
	}

	ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	if (!ctx) {
		SPDK_ERRLOG("Unable to initialize EVP_PKEY_CTX!\n");
		return -ENOMEM;
	}

	/* EVP_PKEY_* functions returns 1 as a success code and 0 or negative on failure. */
	if (EVP_PKEY_derive_init(ctx) != 1) {
		SPDK_ERRLOG("Unable to initialize key derivation ctx for HKDF!\n");
		rc = -ENOMEM;
		goto end;
	}
	if (EVP_PKEY_CTX_set_hkdf_md(ctx, hash) != 1) {
		SPDK_ERRLOG("Unable to set hash for HKDF!\n");
		rc = -EOPNOTSUPP;
		goto end;
	}
	if (EVP_PKEY_CTX_set1_hkdf_key(ctx, psk_in, psk_in_size) != 1) {
		SPDK_ERRLOG("Unable to set PSK key for HKDF!\n");
		rc = -ENOBUFS;
		goto end;
	}

	if (EVP_PKEY_CTX_add1_hkdf_info(ctx, hkdf_info, hkdf_info_size) != 1) {
		SPDK_ERRLOG("Unable to set info label for HKDF!\n");
		rc = -ENOBUFS;
		goto end;
	}
	if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, "", 0) != 1) {
		SPDK_ERRLOG("Unable to set salt for HKDF!\n");
		rc = -EINVAL;
		goto end;
	}
	if (EVP_PKEY_derive(ctx, psk_out, &digest_len) != 1) {
		SPDK_ERRLOG("Unable to derive the PSK key!\n");
		rc = -EINVAL;
		goto end;
	}

	rc = digest_len;

end:
	EVP_PKEY_CTX_free(ctx);
	return rc;
}

static inline int
nvme_tcp_derive_tls_psk(const uint8_t *psk_in, uint64_t psk_in_size, const char *psk_identity,
			uint8_t *psk_out, uint64_t psk_out_size, enum nvme_tcp_cipher_suite tls_cipher_suite)
{
	EVP_PKEY_CTX *ctx;
	uint64_t digest_len = 0;
	char hkdf_info[NVME_TCP_HKDF_INFO_MAX_LEN] = {};
	const char *label = "tls13 nvme-tls-psk";
	size_t pos, labellen, idlen;
	const EVP_MD *hash;
	int rc, hkdf_info_size;

	if (tls_cipher_suite == NVME_TCP_CIPHER_AES_128_GCM_SHA256) {
		digest_len = SHA256_DIGEST_LENGTH;
		hash = EVP_sha256();
	} else if (tls_cipher_suite == NVME_TCP_CIPHER_AES_256_GCM_SHA384) {
		digest_len = SHA384_DIGEST_LENGTH;
		hash = EVP_sha384();
	} else {
		SPDK_ERRLOG("Unknown cipher suite requested!\n");
		return -EOPNOTSUPP;
	}

	labellen = strlen(label);
	idlen = strlen(psk_identity);
	if (idlen > UINT8_MAX) {
		SPDK_ERRLOG("Invalid PSK ID: too long\n");
		return -1;
	}

	*(uint16_t *)&hkdf_info[0] = htons(psk_in_size);
	pos = sizeof(uint16_t);
	hkdf_info[pos] = (uint8_t)labellen;
	pos += sizeof(uint8_t);
	memcpy(&hkdf_info[pos], label, labellen);
	pos += labellen;
	hkdf_info[pos] = (uint8_t)idlen;
	pos += sizeof(uint8_t);
	memcpy(&hkdf_info[pos], psk_identity, idlen);
	pos += idlen;
	hkdf_info_size = pos;

	if (digest_len > psk_out_size) {
		SPDK_ERRLOG("Insufficient buffer size for out key!\n");
		return -1;
	}

	ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	if (!ctx) {
		SPDK_ERRLOG("Unable to initialize EVP_PKEY_CTX!\n");
		return -1;
	}

	if (EVP_PKEY_derive_init(ctx) != 1) {
		SPDK_ERRLOG("Unable to initialize key derivation ctx for HKDF!\n");
		rc = -ENOMEM;
		goto end;
	}
	if (EVP_PKEY_CTX_set_hkdf_md(ctx, hash) != 1) {
		SPDK_ERRLOG("Unable to set hash method for HKDF!\n");
		rc = -EOPNOTSUPP;
		goto end;
	}
	if (EVP_PKEY_CTX_set1_hkdf_key(ctx, psk_in, psk_in_size) != 1) {
		SPDK_ERRLOG("Unable to set PSK key for HKDF!\n");
		rc = -ENOBUFS;
		goto end;
	}
	if (EVP_PKEY_CTX_add1_hkdf_info(ctx, hkdf_info, hkdf_info_size) != 1) {
		SPDK_ERRLOG("Unable to set info label for HKDF!\n");
		rc = -ENOBUFS;
		goto end;
	}
	if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, "", 0) != 1) {
		SPDK_ERRLOG("Unable to set salt for HKDF!\n");
		rc = -EINVAL;
		goto end;
	}
	if (EVP_PKEY_derive(ctx, psk_out, &digest_len) != 1) {
		SPDK_ERRLOG("Unable to derive the PSK key!\n");
		rc = -EINVAL;
		goto end;
	}

	rc = digest_len;

end:
	EVP_PKEY_CTX_free(ctx);
	return rc;
}

static inline int
nvme_tcp_parse_interchange_psk(const char *psk_in, uint8_t *psk_out, size_t psk_out_size,
			       uint64_t *psk_out_decoded_size, uint8_t *hash)
{
	const char *delim = ":";
	char psk_cpy[SPDK_TLS_PSK_MAX_LEN] = {};
	char *sp = NULL;
	uint8_t psk_base64_decoded[SPDK_TLS_PSK_MAX_LEN] = {};
	uint64_t psk_configured_size = 0;
	uint32_t crc32_calc, crc32;
	char *psk_base64;
	uint64_t psk_base64_decoded_size = 0;
	int rc;

	/* Verify PSK format. */
	if (sscanf(psk_in, "NVMeTLSkey-1:%02hhx:", hash) != 1 || psk_in[strlen(psk_in) - 1] != delim[0]) {
		SPDK_ERRLOG("Invalid format of PSK interchange!\n");
		return -EINVAL;
	}

	if (strlen(psk_in) >= SPDK_TLS_PSK_MAX_LEN) {
		SPDK_ERRLOG("PSK interchange exceeds maximum %d characters!\n", SPDK_TLS_PSK_MAX_LEN);
		return -EINVAL;
	}
	if (*hash != NVME_TCP_HASH_ALGORITHM_NONE && *hash != NVME_TCP_HASH_ALGORITHM_SHA256 &&
	    *hash != NVME_TCP_HASH_ALGORITHM_SHA384) {
		SPDK_ERRLOG("Invalid PSK length!\n");
		return -EINVAL;
	}

	/* Check provided hash function string. */
	memcpy(psk_cpy, psk_in, strlen(psk_in));
	strtok_r(psk_cpy, delim, &sp);
	strtok_r(NULL, delim, &sp);

	psk_base64 = strtok_r(NULL, delim, &sp);
	if (psk_base64 == NULL) {
		SPDK_ERRLOG("Could not get base64 string from PSK interchange!\n");
		return -EINVAL;
	}

	rc = spdk_base64_decode(psk_base64_decoded, &psk_base64_decoded_size, psk_base64);
	if (rc) {
		SPDK_ERRLOG("Could not decode base64 PSK!\n");
		return -EINVAL;
	}

	switch (*hash) {
	case NVME_TCP_HASH_ALGORITHM_SHA256:
		psk_configured_size = SHA256_DIGEST_LENGTH;
		break;
	case NVME_TCP_HASH_ALGORITHM_SHA384:
		psk_configured_size = SHA384_DIGEST_LENGTH;
		break;
	case NVME_TCP_HASH_ALGORITHM_NONE:
		if (psk_base64_decoded_size == SHA256_DIGEST_LENGTH + SPDK_CRC32_SIZE_BYTES) {
			psk_configured_size = SHA256_DIGEST_LENGTH;
		} else if (psk_base64_decoded_size == SHA384_DIGEST_LENGTH + SPDK_CRC32_SIZE_BYTES) {
			psk_configured_size = SHA384_DIGEST_LENGTH;
		}
		break;
	default:
		SPDK_ERRLOG("Invalid key: unsupported key hash\n");
		assert(0);
		return -EINVAL;
	}
	if (psk_base64_decoded_size != psk_configured_size + SPDK_CRC32_SIZE_BYTES) {
		SPDK_ERRLOG("Invalid key: unsupported key length\n");
		return -EINVAL;
	}

	crc32 = from_le32(&psk_base64_decoded[psk_configured_size]);

	crc32_calc = spdk_crc32_ieee_update(psk_base64_decoded, psk_configured_size, ~0);
	crc32_calc = ~crc32_calc;

	if (crc32 != crc32_calc) {
		SPDK_ERRLOG("CRC-32 checksums do not match!\n");
		return -EINVAL;
	}

	if (psk_configured_size > psk_out_size) {
		SPDK_ERRLOG("Insufficient buffer size: %lu for configured PSK of size: %lu!\n",
			    psk_out_size, psk_configured_size);
		return -ENOBUFS;
	}
	memcpy(psk_out, psk_base64_decoded, psk_configured_size);
	*psk_out_decoded_size = psk_configured_size;

	return rc;
}

#endif /* SPDK_INTERNAL_NVME_TCP_H */
