/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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
#include "spdk/env.h"
#include "spdk/trace.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "spdk/net.h"

#include "iscsi/md5.h"
#include "iscsi/iscsi.h"
#include "iscsi/param.h"
#include "iscsi/tgt_node.h"
#include "iscsi/task.h"
#include "iscsi/conn.h"
#include "spdk/scsi.h"
#include "spdk/bdev.h"
#include "iscsi/portal_grp.h"
#include "iscsi/acceptor.h"

#include "spdk_internal/log.h"

#define MAX_TMPBUF 1024

#define SPDK_CRC32C_INITIAL    0xffffffffUL
#define SPDK_CRC32C_XOR        0xffffffffUL

#ifdef __FreeBSD__
#define HAVE_SRANDOMDEV 1
#define HAVE_ARC4RANDOM 1
#endif

struct spdk_iscsi_globals g_spdk_iscsi = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.portal_head = TAILQ_HEAD_INITIALIZER(g_spdk_iscsi.portal_head),
	.pg_head = TAILQ_HEAD_INITIALIZER(g_spdk_iscsi.pg_head),
	.ig_head = TAILQ_HEAD_INITIALIZER(g_spdk_iscsi.ig_head),
	.target_head = TAILQ_HEAD_INITIALIZER(g_spdk_iscsi.target_head),
	.auth_group_head = TAILQ_HEAD_INITIALIZER(g_spdk_iscsi.auth_group_head),
};

/* random value generation */
static void spdk_gen_random(uint8_t *buf, size_t len);
#ifndef HAVE_SRANDOMDEV
static void srandomdev(void);
#endif /* HAVE_SRANDOMDEV */
#ifndef HAVE_ARC4RANDOM
//static uint32_t arc4random(void);
#endif /* HAVE_ARC4RANDOM */

/* convert from/to bin/hex */
static int spdk_bin2hex(char *buf, size_t len, const uint8_t *data, size_t data_len);
static int spdk_hex2bin(uint8_t *data, size_t data_len, const char *str);

static int spdk_add_transfer_task(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_task *task);

static int spdk_iscsi_send_r2t(struct spdk_iscsi_conn *conn,
			       struct spdk_iscsi_task *task, int offset,
			       int len, uint32_t transfer_tag, uint32_t *R2TSN);
static int spdk_iscsi_send_r2t_recovery(struct spdk_iscsi_conn *conn,
					struct spdk_iscsi_task *r2t_task, uint32_t r2t_sn,
					bool send_new_r2tsn);

static int spdk_create_iscsi_sess(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_tgt_node *target, enum session_type session_type);
static int spdk_append_iscsi_sess(struct spdk_iscsi_conn *conn,
				  const char *initiator_port_name, uint16_t tsih, uint16_t cid);

static void spdk_remove_acked_pdu(struct spdk_iscsi_conn *conn, uint32_t ExpStatSN);

static int spdk_iscsi_reject(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
			     int reason);

#define DMIN32(A,B) ((uint32_t) ((uint32_t)(A) > (uint32_t)(B) ? (uint32_t)(B) : (uint32_t)(A)))
#define DMIN64(A,B) ((uint64_t) ((A) > (B) ? (B) : (A)))

#define MATCH_DIGEST_WORD(BUF, CRC32C) \
	(    ((((uint32_t) *((uint8_t *)(BUF)+0)) << 0)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+1)) << 8)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+2)) << 16)	\
	    | (((uint32_t) *((uint8_t *)(BUF)+3)) << 24))	\
	    == (CRC32C))

#define MAKE_DIGEST_WORD(BUF, CRC32C) \
	(   ((*((uint8_t *)(BUF)+0)) = (uint8_t)((uint32_t)(CRC32C) >> 0)), \
	    ((*((uint8_t *)(BUF)+1)) = (uint8_t)((uint32_t)(CRC32C) >> 8)), \
	    ((*((uint8_t *)(BUF)+2)) = (uint8_t)((uint32_t)(CRC32C) >> 16)), \
	    ((*((uint8_t *)(BUF)+3)) = (uint8_t)((uint32_t)(CRC32C) >> 24)))

#if 0
static int
spdk_match_digest_word(const uint8_t *buf, uint32_t crc32c)
{
	uint32_t l;

	l = (buf[0] & 0xffU) << 0;
	l |= (buf[1] & 0xffU) << 8;
	l |= (buf[2] & 0xffU) << 16;
	l |= (buf[3] & 0xffU) << 24;
	return (l == crc32c);
}

static uint8_t *
spdk_make_digest_word(uint8_t *buf, size_t len, uint32_t crc32c)
{
	if (len < ISCSI_DIGEST_LEN) {
		return NULL;
	}

	buf[0] = (crc32c >> 0) & 0xffU;
	buf[1] = (crc32c >> 8) & 0xffU;
	buf[2] = (crc32c >> 16) & 0xffU;
	buf[3] = (crc32c >> 24) & 0xffU;
	return buf;
}
#endif

#ifndef HAVE_SRANDOMDEV
static void
srandomdev(void)
{
	unsigned long seed;
	time_t now;
	pid_t pid;

	pid = getpid();
	now = time(NULL);
	seed = pid ^ now;
	srandom(seed);
}
#endif /* HAVE_SRANDOMDEV */

#ifndef HAVE_ARC4RANDOM
static int spdk_arc4random_initialized = 0;

static uint32_t
arc4random(void)
{
	uint32_t r;
	uint32_t r1, r2;

	if (!spdk_arc4random_initialized) {
		srandomdev();
		spdk_arc4random_initialized = 1;
	}
	r1 = (uint32_t)(random() & 0xffff);
	r2 = (uint32_t)(random() & 0xffff);
	r = (r1 << 16) | r2;
	return r;
}
#endif /* HAVE_ARC4RANDOM */

static void
spdk_gen_random(uint8_t *buf, size_t len)
{
#ifdef USE_RANDOM
	long l;
	size_t idx;

	srandomdev();
	for (idx = 0; idx < len; idx++) {
		l = random();
		buf[idx] = (uint8_t) l;
	}
#else
	uint32_t r;
	size_t idx;

	for (idx = 0; idx < len; idx++) {
		r = arc4random();
		buf[idx] = (uint8_t) r;
	}
#endif /* USE_RANDOM */
}

static uint64_t
spdk_iscsi_get_isid(const uint8_t isid[6])
{
	return (uint64_t)isid[0] << 40 |
	       (uint64_t)isid[1] << 32 |
	       (uint64_t)isid[2] << 24 |
	       (uint64_t)isid[3] << 16 |
	       (uint64_t)isid[4] << 8 |
	       (uint64_t)isid[5];
}

static int
spdk_bin2hex(char *buf, size_t len, const uint8_t *data, size_t data_len)
{
	const char *digits = "0123456789ABCDEF";
	size_t total = 0;
	size_t idx;

	if (len < 3) {
		return -1;
	}
	buf[total] = '0';
	total++;
	buf[total] = 'x';
	total++;
	buf[total] = '\0';

	for (idx = 0; idx < data_len; idx++) {
		if (total + 3 > len) {
			buf[total] = '\0';
			return - 1;
		}
		buf[total] = digits[(data[idx] >> 4) & 0x0fU];
		total++;
		buf[total] = digits[data[idx] & 0x0fU];
		total++;
	}
	buf[total] = '\0';
	return total;
}

static int
spdk_hex2bin(uint8_t *data, size_t data_len, const char *str)
{
	const char *digits = "0123456789ABCDEF";
	const char *dp;
	const char *p;
	size_t total = 0;
	int n0, n1;

	p = str;
	if (p[0] != '0' && (p[1] != 'x' && p[1] != 'X')) {
		return -1;
	}
	p += 2;

	while (p[0] != '\0' && p[1] != '\0') {
		if (total >= data_len) {
			return -1;
		}
		dp = strchr(digits, toupper((int) p[0]));
		if (dp == NULL) {
			return -1;
		}
		n0 = (int)(dp - digits);
		dp = strchr(digits, toupper((int) p[1]));
		if (dp == NULL) {
			return -1;
		}
		n1 = (int)(dp - digits);

		data[total] = (uint8_t)(((n0 & 0x0fU) << 4) | (n1 & 0x0fU));
		total++;
		p += 2;
	}
	return total;
}

static int
spdk_islun2lun(uint64_t islun)
{
	uint64_t fmt_lun;
	uint64_t method;
	int lun_i;

	fmt_lun = islun;
	method = (fmt_lun >> 62) & 0x03U;
	fmt_lun = fmt_lun >> 48;
	if (method == 0x00U) {
		lun_i = (int)(fmt_lun & 0x00ffU);
	} else if (method == 0x01U) {
		lun_i = (int)(fmt_lun & 0x3fffU);
	} else {
		lun_i = 0xffffU;
	}
	return lun_i;
}

static uint32_t
spdk_iscsi_pdu_calc_header_digest(struct spdk_iscsi_pdu *pdu)
{
	uint32_t crc32c;
	uint32_t ahs_len_bytes = pdu->bhs.total_ahs_len * 4;

	crc32c = SPDK_CRC32C_INITIAL;
	crc32c = spdk_crc32c_update(&pdu->bhs, ISCSI_BHS_LEN, crc32c);

	if (ahs_len_bytes) {
		crc32c = spdk_crc32c_update(pdu->ahs, ahs_len_bytes, crc32c);
	}

	/* BHS and AHS are always 4-byte multiples in length, so no padding is necessary. */
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

static uint32_t
spdk_iscsi_pdu_calc_data_digest(struct spdk_iscsi_pdu *pdu)
{
	uint32_t data_len = DGET24(pdu->bhs.data_segment_len);
	uint32_t crc32c;
	uint32_t mod;

	crc32c = SPDK_CRC32C_INITIAL;
	crc32c = spdk_crc32c_update(pdu->data, data_len, crc32c);

	mod = data_len % ISCSI_ALIGNMENT;
	if (mod != 0) {
		uint32_t pad_length = ISCSI_ALIGNMENT - mod;
		uint8_t pad[3] = {0, 0, 0};

		assert(pad_length > 0);
		assert(pad_length <= sizeof(pad));
		crc32c = spdk_crc32c_update(pad, pad_length, crc32c);
	}

	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	return crc32c;
}

int
spdk_iscsi_read_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu **_pdu)
{
	struct spdk_iscsi_pdu *pdu;
	struct spdk_mempool *pool;
	uint32_t crc32c;
	int ahs_len;
	int data_len;
	int max_segment_len;
	int rc;

	if (conn->pdu_in_progress == NULL) {
		conn->pdu_in_progress = spdk_get_pdu();
		if (conn->pdu_in_progress == NULL) {
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	pdu = conn->pdu_in_progress;

	if (pdu->bhs_valid_bytes < ISCSI_BHS_LEN) {
		rc = spdk_iscsi_conn_read_data(conn,
					       ISCSI_BHS_LEN - pdu->bhs_valid_bytes,
					       (uint8_t *)&pdu->bhs + pdu->bhs_valid_bytes);
		if (rc < 0) {
			*_pdu = NULL;
			spdk_put_pdu(pdu);
			conn->pdu_in_progress = NULL;
			return rc;
		}
		pdu->bhs_valid_bytes += rc;
		if (pdu->bhs_valid_bytes < ISCSI_BHS_LEN) {
			*_pdu = NULL;
			return SPDK_SUCCESS;
		}
	}

	data_len = ISCSI_ALIGN(DGET24(pdu->bhs.data_segment_len));

	/* AHS */
	ahs_len = pdu->bhs.total_ahs_len * 4;
	assert(ahs_len <= ISCSI_AHS_LEN);
	if (pdu->ahs_valid_bytes < ahs_len) {
		rc = spdk_iscsi_conn_read_data(conn,
					       ahs_len - pdu->ahs_valid_bytes,
					       pdu->ahs + pdu->ahs_valid_bytes);
		if (rc < 0) {
			*_pdu = NULL;
			spdk_put_pdu(pdu);
			conn->pdu_in_progress = NULL;
			return rc;
		}

		pdu->ahs_valid_bytes += rc;
		if (pdu->ahs_valid_bytes < ahs_len) {
			*_pdu = NULL;
			return SPDK_SUCCESS;
		}
	}

	/* Header Digest */
	if (conn->header_digest &&
	    pdu->hdigest_valid_bytes < ISCSI_DIGEST_LEN) {
		rc = spdk_iscsi_conn_read_data(conn,
					       ISCSI_DIGEST_LEN - pdu->hdigest_valid_bytes,
					       pdu->header_digest + pdu->hdigest_valid_bytes);
		if (rc < 0) {
			*_pdu = NULL;
			spdk_put_pdu(pdu);
			conn->pdu_in_progress = NULL;
			return rc;
		}

		pdu->hdigest_valid_bytes += rc;
		if (pdu->hdigest_valid_bytes < ISCSI_DIGEST_LEN) {
			*_pdu = NULL;
			return SPDK_SUCCESS;
		}
	}

	/* copy the actual data into local buffer */
	if (pdu->data_valid_bytes < data_len) {
		if (pdu->data_buf == NULL) {
			if (data_len <= spdk_get_immediate_data_buffer_size()) {
				pool = g_spdk_iscsi.pdu_immediate_data_pool;
			} else if (data_len <= spdk_get_data_out_buffer_size()) {
				pool = g_spdk_iscsi.pdu_data_out_pool;
			} else {
				SPDK_ERRLOG("Data(%d) > MaxSegment(%d)\n",
					    data_len, spdk_get_data_out_buffer_size());
				*_pdu = NULL;
				spdk_put_pdu(pdu);
				conn->pdu_in_progress = NULL;
				return SPDK_ISCSI_CONNECTION_FATAL;
			}
			pdu->mobj = spdk_mempool_get(pool);
			if (pdu->mobj == NULL) {
				*_pdu = NULL;
				return SPDK_SUCCESS;
			}
			pdu->data_buf = pdu->mobj->buf;
		}

		rc = spdk_iscsi_conn_read_data(conn,
					       data_len - pdu->data_valid_bytes,
					       pdu->data_buf + pdu->data_valid_bytes);
		if (rc < 0) {
			*_pdu = NULL;
			spdk_put_pdu(pdu);
			conn->pdu_in_progress = NULL;
			return rc;
		}

		pdu->data_valid_bytes += rc;
		if (pdu->data_valid_bytes < data_len) {
			*_pdu = NULL;
			return SPDK_SUCCESS;
		}
	}

	/* copy out the data digest */
	if (conn->data_digest && data_len != 0 &&
	    pdu->ddigest_valid_bytes < ISCSI_DIGEST_LEN) {
		rc = spdk_iscsi_conn_read_data(conn,
					       ISCSI_DIGEST_LEN - pdu->ddigest_valid_bytes,
					       pdu->data_digest + pdu->ddigest_valid_bytes);
		if (rc < 0) {
			*_pdu = NULL;
			spdk_put_pdu(pdu);
			conn->pdu_in_progress = NULL;
			return rc;
		}

		pdu->ddigest_valid_bytes += rc;
		if (pdu->ddigest_valid_bytes < ISCSI_DIGEST_LEN) {
			*_pdu = NULL;
			return SPDK_SUCCESS;
		}
	}

	/* All data for this PDU has now been read from the socket. */
	conn->pdu_in_progress = NULL;

	spdk_trace_record(TRACE_ISCSI_READ_PDU, conn->id, pdu->data_valid_bytes,
			  (uintptr_t)pdu, pdu->bhs.opcode);

	/* Data Segment */
	if (data_len != 0) {
		/*
		 * Determine the maximum segment length expected for this PDU.
		 *  This will be used to make sure the initiator did not send
		 *  us too much immediate data.
		 *
		 * This value is specified separately by the initiator and target,
		 *  and not negotiated.  So we can use the #define safely here,
		 *  since the value is not dependent on the initiator's maximum
		 *  segment lengths (FirstBurstLength/MaxRecvDataSegmentLength),
		 *  and SPDK currently does not allow configuration of these values
		 *  at runtime.
		 */
		if (conn->sess == NULL) {
			/*
			 * If the connection does not yet have a session, then
			 *  login is not complete and we use the 8KB default
			 *  FirstBurstLength as our maximum data segment length
			 *  value.
			 */
			max_segment_len = DEFAULT_FIRSTBURSTLENGTH;
		} else if (pdu->bhs.opcode == ISCSI_OP_SCSI_DATAOUT) {
			max_segment_len = spdk_get_data_out_buffer_size();
		} else if (pdu->bhs.opcode == ISCSI_OP_NOPOUT) {
			max_segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
		} else {
			max_segment_len = spdk_get_immediate_data_buffer_size();
		}
		if (data_len > max_segment_len) {
			SPDK_ERRLOG("Data(%d) > MaxSegment(%d)\n", data_len, max_segment_len);
			rc = spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
			spdk_put_pdu(pdu);

			/*
			 * If spdk_iscsi_reject() was not able to reject the PDU,
			 * treat it as a fatal connection error.  Otherwise,
			 * return SUCCESS here so that the caller will continue
			 * to attempt to read PDUs.
			 */
			rc = (rc < 0) ? SPDK_ISCSI_CONNECTION_FATAL : SPDK_SUCCESS;
			return rc;
		}

		pdu->data = pdu->data_buf;
		pdu->data_from_mempool = true;
		pdu->data_segment_len = data_len;
	}

	/* check digest */
	if (conn->header_digest) {
		crc32c = spdk_iscsi_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD(pdu->header_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("header digest error (%s)\n", conn->initiator_name);
			spdk_put_pdu(pdu);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}
	if (conn->data_digest && data_len != 0) {
		crc32c = spdk_iscsi_pdu_calc_data_digest(pdu);
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("data digest error (%s)\n", conn->initiator_name);
			spdk_put_pdu(pdu);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	*_pdu = pdu;
	return 1;
}

int
spdk_iscsi_build_iovecs(struct spdk_iscsi_conn *conn, struct iovec *iovec,
			struct spdk_iscsi_pdu *pdu)
{
	int iovec_cnt = 0;
	uint32_t crc32c;
	int enable_digest;
	int total_ahs_len;
	int data_len;

	total_ahs_len = pdu->bhs.total_ahs_len;
	data_len = DGET24(pdu->bhs.data_segment_len);

	enable_digest = 1;
	if (pdu->bhs.opcode == ISCSI_OP_LOGIN_RSP) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	/* BHS */
	iovec[iovec_cnt].iov_base = &pdu->bhs;
	iovec[iovec_cnt].iov_len = ISCSI_BHS_LEN;
	iovec_cnt++;

	/* AHS */
	if (total_ahs_len > 0) {
		iovec[iovec_cnt].iov_base = pdu->ahs;
		iovec[iovec_cnt].iov_len = 4 * total_ahs_len;
		iovec_cnt++;
	}

	/* Header Digest */
	if (enable_digest && conn->header_digest) {
		crc32c = spdk_iscsi_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD(pdu->header_digest, crc32c);

		iovec[iovec_cnt].iov_base = pdu->header_digest;
		iovec[iovec_cnt].iov_len = ISCSI_DIGEST_LEN;
		iovec_cnt++;
	}

	/* Data Segment */
	if (data_len > 0) {
		iovec[iovec_cnt].iov_base = pdu->data;
		iovec[iovec_cnt].iov_len = ISCSI_ALIGN(data_len);
		iovec_cnt++;
	}

	/* Data Digest */
	if (enable_digest && conn->data_digest && data_len != 0) {
		crc32c = spdk_iscsi_pdu_calc_data_digest(pdu);
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);

		iovec[iovec_cnt].iov_base = pdu->data_digest;
		iovec[iovec_cnt].iov_len = ISCSI_DIGEST_LEN;
		iovec_cnt++;
	}

	return iovec_cnt;
}

static int
spdk_iscsi_append_text(struct spdk_iscsi_conn *conn __attribute__((__unused__)),
		       const char *key, const char *val, uint8_t *data,
		       int alloc_len, int data_len)
{
	int total;
	int len;

	total = data_len;
	if (alloc_len < 1) {
		return 0;
	}
	if (total > alloc_len) {
		total = alloc_len;
		data[total - 1] = '\0';
		return total;
	}

	if (alloc_len - total < 1) {
		SPDK_ERRLOG("data space small %d\n", alloc_len);
		return total;
	}
	len = snprintf((char *) data + total, alloc_len - total, "%s=%s", key, val);
	total += len + 1;

	return total;
}

static int
spdk_iscsi_append_param(struct spdk_iscsi_conn *conn, const char *key,
			uint8_t *data, int alloc_len, int data_len)
{
	struct iscsi_param *param;
	int rc;

	param = spdk_iscsi_param_find(conn->params, key);
	if (param == NULL) {
		param = spdk_iscsi_param_find(conn->sess->params, key);
		if (param == NULL) {
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "no key %.64s\n", key);
			return data_len;
		}
	}
	rc = spdk_iscsi_append_text(conn, param->key, param->val, data,
				    alloc_len, data_len);
	return rc;
}

static int
spdk_iscsi_get_authinfo(struct spdk_iscsi_conn *conn, const char *authuser)
{
	int ag_tag;
	int rc;

	if (conn->sess->target != NULL) {
		ag_tag = conn->sess->target->chap_group;
	} else {
		ag_tag = -1;
	}
	if (ag_tag < 0) {
		ag_tag = g_spdk_iscsi.chap_group;
	}
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "ag_tag=%d\n", ag_tag);

	rc = spdk_iscsi_chap_get_authinfo(&conn->auth, authuser, ag_tag);
	if (rc < 0) {
		SPDK_ERRLOG("chap_get_authinfo() failed\n");
		return -1;
	}
	return 0;
}

static int
spdk_iscsi_auth_params(struct spdk_iscsi_conn *conn,
		       struct iscsi_param *params, const char *method, uint8_t *data,
		       int alloc_len, int data_len)
{
	char *in_val;
	char *in_next;
	char *new_val;
	const char *val;
	const char *user;
	const char *response;
	const char *challenge;
	int total;
	int rc;

	if (conn == NULL || params == NULL || method == NULL) {
		return -1;
	}
	if (strcasecmp(method, "CHAP") == 0) {
		/* method OK */
	} else {
		SPDK_ERRLOG("unsupported AuthMethod %.64s\n", method);
		return -1;
	}

	total = data_len;
	if (alloc_len < 1) {
		return 0;
	}
	if (total > alloc_len) {
		total = alloc_len;
		data[total - 1] = '\0';
		return total;
	}

	/* for temporary store */
	in_val = malloc(ISCSI_TEXT_MAX_VAL_LEN + 1);
	if (!in_val) {
		SPDK_ERRLOG("malloc() failed for temporary store\n");
		return -ENOMEM;
	}

	/* CHAP method (RFC1994) */
	if ((val = spdk_iscsi_param_get_val(params, "CHAP_A")) != NULL) {
		if (conn->auth.chap_phase != ISCSI_CHAP_PHASE_WAIT_A) {
			SPDK_ERRLOG("CHAP sequence error\n");
			goto error_return;
		}

		/* CHAP_A is LIST type */
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", val);
		in_next = in_val;
		while ((new_val = spdk_strsepq(&in_next, ",")) != NULL) {
			if (strcasecmp(new_val, "5") == 0) {
				/* CHAP with MD5 */
				break;
			}
		}
		if (new_val == NULL) {
			snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", "Reject");
			new_val = in_val;
			spdk_iscsi_append_text(conn, "CHAP_A", new_val,
					       data, alloc_len, total);
			goto error_return;
		}
		/* selected algorithm is 5 (MD5) */
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "got CHAP_A=%s\n", new_val);
		total = spdk_iscsi_append_text(conn, "CHAP_A", new_val,
					       data, alloc_len, total);

		/* Identifier is one octet */
		spdk_gen_random(conn->auth.chap_id, 1);
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN, "%d",
			 (int) conn->auth.chap_id[0]);
		total = spdk_iscsi_append_text(conn, "CHAP_I", in_val,
					       data, alloc_len, total);

		/* Challenge Value is a variable stream of octets */
		/* (binary length MUST not exceed 1024 bytes) */
		conn->auth.chap_challenge_len = ISCSI_CHAP_CHALLENGE_LEN;
		spdk_gen_random(conn->auth.chap_challenge,
				conn->auth.chap_challenge_len);
		spdk_bin2hex(in_val, ISCSI_TEXT_MAX_VAL_LEN,
			     conn->auth.chap_challenge,
			     conn->auth.chap_challenge_len);
		total = spdk_iscsi_append_text(conn, "CHAP_C", in_val,
					       data, alloc_len, total);

		conn->auth.chap_phase = ISCSI_CHAP_PHASE_WAIT_NR;
	} else if ((val = spdk_iscsi_param_get_val(params, "CHAP_N")) != NULL) {
		uint8_t resmd5[SPDK_MD5DIGEST_LEN];
		uint8_t tgtmd5[SPDK_MD5DIGEST_LEN];
		struct spdk_md5ctx md5ctx;

		user = val;
		if (conn->auth.chap_phase != ISCSI_CHAP_PHASE_WAIT_NR) {
			SPDK_ERRLOG("CHAP sequence error\n");
			goto error_return;
		}

		response = spdk_iscsi_param_get_val(params, "CHAP_R");
		if (response == NULL) {
			SPDK_ERRLOG("no response\n");
			goto error_return;
		}
		rc = spdk_hex2bin(resmd5, SPDK_MD5DIGEST_LEN, response);
		if (rc < 0 || rc != SPDK_MD5DIGEST_LEN) {
			SPDK_ERRLOG("response format error\n");
			goto error_return;
		}
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "got CHAP_N/CHAP_R\n");

		rc = spdk_iscsi_get_authinfo(conn, val);
		if (rc < 0) {
			//SPDK_ERRLOG("auth user or secret is missing\n");
			SPDK_ERRLOG("iscsi_get_authinfo() failed\n");
			goto error_return;
		}
		if (conn->auth.user[0] == '\0' || conn->auth.secret[0] == '\0') {
			//SPDK_ERRLOG("auth user or secret is missing\n");
			SPDK_ERRLOG("auth failed (user %.64s)\n", user);
			goto error_return;
		}

		spdk_md5init(&md5ctx);
		/* Identifier */
		spdk_md5update(&md5ctx, conn->auth.chap_id, 1);
		/* followed by secret */
		spdk_md5update(&md5ctx, conn->auth.secret,
			       strlen(conn->auth.secret));
		/* followed by Challenge Value */
		spdk_md5update(&md5ctx, conn->auth.chap_challenge,
			       conn->auth.chap_challenge_len);
		/* tgtmd5 is expecting Response Value */
		spdk_md5final(tgtmd5, &md5ctx);

		spdk_bin2hex(in_val, ISCSI_TEXT_MAX_VAL_LEN,
			     tgtmd5, SPDK_MD5DIGEST_LEN);

#if 0
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "tgtmd5=%s, resmd5=%s\n", in_val, response);
		spdk_dump("tgtmd5", tgtmd5, SPDK_MD5DIGEST_LEN);
		spdk_dump("resmd5", resmd5, SPDK_MD5DIGEST_LEN);
#endif

		/* compare MD5 digest */
		if (memcmp(tgtmd5, resmd5, SPDK_MD5DIGEST_LEN) != 0) {
			/* not match */
			//SPDK_ERRLOG("auth user or secret is missing\n");
			SPDK_ERRLOG("auth failed (user %.64s)\n", user);
			goto error_return;
		}
		/* OK initiator's secret */
		conn->authenticated = 1;

		/* mutual CHAP? */
		val = spdk_iscsi_param_get_val(params, "CHAP_I");
		if (val != NULL) {
			conn->auth.chap_mid[0] = (uint8_t) strtol(val, NULL, 10);
			challenge = spdk_iscsi_param_get_val(params, "CHAP_C");
			if (challenge == NULL) {
				SPDK_ERRLOG("CHAP sequence error\n");
				goto error_return;
			}
			rc = spdk_hex2bin(conn->auth.chap_mchallenge,
					  ISCSI_CHAP_CHALLENGE_LEN,
					  challenge);
			if (rc < 0) {
				SPDK_ERRLOG("challenge format error\n");
				goto error_return;
			}
			conn->auth.chap_mchallenge_len = rc;
#if 0
			spdk_dump("MChallenge", conn->auth.chap_mchallenge,
				  conn->auth.chap_mchallenge_len);
#endif
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "got CHAP_I/CHAP_C\n");

			if (conn->auth.muser[0] == '\0' || conn->auth.msecret[0] == '\0') {
				//SPDK_ERRLOG("mutual auth user or secret is missing\n");
				SPDK_ERRLOG("auth failed (user %.64s)\n", user);
				goto error_return;
			}

			spdk_md5init(&md5ctx);
			/* Identifier */
			spdk_md5update(&md5ctx, conn->auth.chap_mid, 1);
			/* followed by secret */
			spdk_md5update(&md5ctx, conn->auth.msecret,
				       strlen(conn->auth.msecret));
			/* followed by Challenge Value */
			spdk_md5update(&md5ctx, conn->auth.chap_mchallenge,
				       conn->auth.chap_mchallenge_len);
			/* tgtmd5 is Response Value */
			spdk_md5final(tgtmd5, &md5ctx);

			spdk_bin2hex(in_val, ISCSI_TEXT_MAX_VAL_LEN,
				     tgtmd5, SPDK_MD5DIGEST_LEN);

			total = spdk_iscsi_append_text(conn, "CHAP_N",
						       conn->auth.muser, data, alloc_len, total);
			total = spdk_iscsi_append_text(conn, "CHAP_R",
						       in_val, data, alloc_len, total);
		} else {
			/* not mutual */
			if (conn->req_mutual) {
				SPDK_ERRLOG("required mutual CHAP\n");
				goto error_return;
			}
		}

		conn->auth.chap_phase = ISCSI_CHAP_PHASE_END;
	} else {
		/* not found CHAP keys */
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "start CHAP\n");
		conn->auth.chap_phase = ISCSI_CHAP_PHASE_WAIT_A;
	}

	free(in_val);
	return total;

error_return:
	conn->auth.chap_phase = ISCSI_CHAP_PHASE_WAIT_A;
	free(in_val);
	return -1;
}

static int
spdk_iscsi_reject(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
		  int reason)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_reject *rsph;
	uint8_t *data;
	int total_ahs_len;
	int data_len;
	int alloc_len;

	total_ahs_len = pdu->bhs.total_ahs_len;
	data_len = 0;
	alloc_len = ISCSI_BHS_LEN + (4 * total_ahs_len);

	if (conn->header_digest) {
		alloc_len += ISCSI_DIGEST_LEN;
	}

	data = calloc(1, alloc_len);
	if (!data) {
		SPDK_ERRLOG("calloc() failed for data segment\n");
		return -ENOMEM;
	}

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Reject PDU reason=%d\n", reason);

	if (conn->sess != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
			      conn->StatSN, conn->sess->ExpCmdSN,
			      conn->sess->MaxCmdSN);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "StatSN=%u\n", conn->StatSN);
	}

	memcpy(data, &pdu->bhs, ISCSI_BHS_LEN);
	data_len += ISCSI_BHS_LEN;

	if (total_ahs_len != 0) {
		memcpy(data + data_len, pdu->ahs, (4 * total_ahs_len));
		data_len += (4 * total_ahs_len);
	}

	if (conn->header_digest) {
		memcpy(data + data_len, pdu->header_digest, ISCSI_DIGEST_LEN);
		data_len += ISCSI_DIGEST_LEN;
	}

	rsp_pdu = spdk_get_pdu();
	if (rsp_pdu == NULL) {
		free(data);
		return -ENOMEM;
	}

	rsph = (struct iscsi_bhs_reject *)&rsp_pdu->bhs;
	rsp_pdu->data = data;
	rsph->opcode = ISCSI_OP_REJECT;
	rsph->flags |= 0x80;	/* bit 0 is default to 1 */
	rsph->reason = reason;
	DSET24(rsph->data_segment_len, data_len);

	rsph->ffffffff = 0xffffffffU;
	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;

	if (conn->sess != NULL) {
		to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
		to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);
	} else {
		to_be32(&rsph->exp_cmd_sn, 1);
		to_be32(&rsph->max_cmd_sn, 1);
	}

	SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "PDU", (void *)&rsp_pdu->bhs, ISCSI_BHS_LEN);

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);

	return 0;
}

static int
spdk_iscsi_check_values(struct spdk_iscsi_conn *conn)
{
	if (conn->sess->FirstBurstLength > conn->sess->MaxBurstLength) {
		SPDK_ERRLOG("FirstBurstLength(%d) > MaxBurstLength(%d)\n",
			    conn->sess->FirstBurstLength,
			    conn->sess->MaxBurstLength);
		return -1;
	}
	if (conn->sess->FirstBurstLength > g_spdk_iscsi.FirstBurstLength) {
		SPDK_ERRLOG("FirstBurstLength(%d) > iSCSI target restriction(%d)\n",
			    conn->sess->FirstBurstLength, g_spdk_iscsi.FirstBurstLength);
		return -1;
	}
	if (conn->sess->MaxBurstLength > 0x00ffffff) {
		SPDK_ERRLOG("MaxBurstLength(%d) > 0x00ffffff\n",
			    conn->sess->MaxBurstLength);
		return -1;
	}

	if (conn->MaxRecvDataSegmentLength < 512) {
		SPDK_ERRLOG("MaxRecvDataSegmentLength(%d) < 512\n",
			    conn->MaxRecvDataSegmentLength);
		return -1;
	}
	if (conn->MaxRecvDataSegmentLength > 0x00ffffff) {
		SPDK_ERRLOG("MaxRecvDataSegmentLength(%d) > 0x00ffffff\n",
			    conn->MaxRecvDataSegmentLength);
		return -1;
	}
	return 0;
}

/*
 * The response function of spdk_iscsi_op_login
 * return:
 * 0:success;
 * -1:error;
 */
static int
spdk_iscsi_op_login_response(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_pdu *rsp_pdu, struct iscsi_param *params)
{
	struct iscsi_bhs_login_rsp *rsph;
	int rc;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	rsph->version_max = ISCSI_VERSION;
	rsph->version_act = ISCSI_VERSION;
	DSET24(rsph->data_segment_len, rsp_pdu->data_segment_len);

	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;

	if (conn->sess != NULL) {
		to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
		to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);
	} else {
		to_be32(&rsph->exp_cmd_sn, rsp_pdu->cmd_sn);
		to_be32(&rsph->max_cmd_sn, rsp_pdu->cmd_sn);
	}

	SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "PDU", (uint8_t *)rsph, ISCSI_BHS_LEN);
	SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "DATA", rsp_pdu->data, rsp_pdu->data_segment_len);

	/* Set T/CSG/NSG to reserved if login error. */
	if (rsph->status_class != 0) {
		rsph->flags &= ~ISCSI_LOGIN_TRANSIT;
		rsph->flags &= ~ISCSI_LOGIN_CURRENT_STAGE_MASK;
		rsph->flags &= ~ISCSI_LOGIN_NEXT_STAGE_MASK;
	}
	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);

	/* after send PDU digest on/off */
	if (conn->full_feature) {
		/* update internal variables */
		rc = spdk_iscsi_copy_param2var(conn);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_copy_param2var() failed\n");
			spdk_iscsi_param_free(params);
			return -1;
		}
		/* check value */
		rc = spdk_iscsi_check_values(conn);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_check_values() failed\n");
			spdk_iscsi_param_free(params);
			return -1;
		}
	}

	spdk_iscsi_param_free(params);
	return 0;
}

/*
 * This function is used to del the original param and update it with new
 * value
 * return:
 * 0: success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_update_param(struct spdk_iscsi_conn *conn,
				 const char *key, const char *value,
				 const char *list)
{
	int rc = 0;
	struct iscsi_param *new_param, *orig_param;
	int index;

	orig_param = spdk_iscsi_param_find(conn->params, key);
	if (orig_param == NULL) {
		SPDK_ERRLOG("orig_param %s not found\n", key);
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}

	index = orig_param->state_index;
	rc = spdk_iscsi_param_del(&conn->params, key);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_del(%s) failed\n", key);
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	rc = spdk_iscsi_param_add(&conn->params, key, value, list, ISPT_LIST);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_add() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	new_param = spdk_iscsi_param_find(conn->params, key);
	if (new_param == NULL) {
		SPDK_ERRLOG("spdk_iscsi_param_find() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	new_param->state_index = index;
	return rc;
}

/*
 * The function which is used to handle the part of session discovery
 * return:
 * 0, success;
 * otherwise: error;
 */
static int
spdk_iscsi_op_login_session_discovery_chap(struct spdk_iscsi_conn *conn)
{
	int rc = 0;

	if (g_spdk_iscsi.disable_chap) {
		conn->req_auth = 0;
		rc = spdk_iscsi_op_login_update_param(conn, "AuthMethod", "None", "None");
		if (rc < 0) {
			return rc;
		}
	} else if (g_spdk_iscsi.require_chap) {
		conn->req_auth = 1;
		rc = spdk_iscsi_op_login_update_param(conn, "AuthMethod", "CHAP", "CHAP");
		if (rc < 0) {
			return rc;
		}
	}
	if (g_spdk_iscsi.mutual_chap) {
		conn->req_mutual = 1;
	}

	return rc;
}

/*
 * This function is used to update the param related with chap
 * return:
 * 0: success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_negotiate_chap_param(struct spdk_iscsi_conn *conn,
		struct spdk_iscsi_pdu *rsp_pdu,
		struct spdk_iscsi_tgt_node *target)
{
	int rc;

	if (target->disable_chap) {
		conn->req_auth = 0;
		rc = spdk_iscsi_op_login_update_param(conn, "AuthMethod", "None", "None");
		if (rc < 0) {
			return rc;
		}
	} else if (target->require_chap) {
		conn->req_auth = 1;
		rc = spdk_iscsi_op_login_update_param(conn, "AuthMethod", "CHAP", "CHAP");
		if (rc < 0) {
			return rc;
		}
	}

	if (target->mutual_chap) {
		conn->req_mutual = 1;
	}

	if (target->header_digest) {
		/*
		 * User specified header digests, so update the list of
		 *  HeaderDigest values to remove "None" so that only
		 *  initiators who support CRC32C can connect.
		 */
		rc = spdk_iscsi_op_login_update_param(conn, "HeaderDigest", "CRC32C", "CRC32C");
		if (rc < 0) {
			return rc;
		}
	}

	if (target->data_digest) {
		/*
		 * User specified data digests, so update the list of
		 *  DataDigest values to remove "None" so that only
		 *  initiators who support CRC32C can connect.
		 */
		rc = spdk_iscsi_op_login_update_param(conn, "DataDigest", "CRC32C", "CRC32C");
		if (rc < 0) {
			return rc;
		}
	}

	return 0;
}

/*
 * This function use to check the session
 * return:
 * 0, success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_check_session(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_pdu *rsp_pdu,
				  char *initiator_port_name, int cid)

{
	int rc = 0;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	/* check existing session */
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "isid=%"PRIx64", tsih=%u, cid=%u\n",
		      spdk_iscsi_get_isid(rsph->isid), from_be16(&rsph->tsih), cid);
	if (rsph->tsih != 0) {
		/* multiple connections */
		rc = spdk_append_iscsi_sess(conn, initiator_port_name,
					    from_be16(&rsph->tsih), cid);
		if (rc < 0) {
			SPDK_ERRLOG("isid=%"PRIx64", tsih=%u, cid=%u:"
				    "spdk_append_iscsi_sess() failed\n",
				    spdk_iscsi_get_isid(rsph->isid), from_be16(&rsph->tsih),
				    cid);
			/* Can't include in session */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = ISCSI_LOGIN_CONN_ADD_FAIL;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
	} else if (!g_spdk_iscsi.AllowDuplicateIsid) {
		/* new session, drop old sess by the initiator */
		spdk_iscsi_drop_conns(conn, initiator_port_name, 0 /* drop old */);
	}

	return rc;
}

/*
 * This function is used to check the target info
 * return:
 * 0: success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_check_target(struct spdk_iscsi_conn *conn,
				 struct spdk_iscsi_pdu *rsp_pdu,
				 const char *target_name,
				 struct spdk_iscsi_tgt_node **target)
{
	bool result;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	*target = spdk_iscsi_find_tgt_node(target_name);
	if (*target == NULL) {
		SPDK_WARNLOG("target %s not found\n", target_name);
		/* Not found */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_TARGET_NOT_FOUND;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	result = spdk_iscsi_tgt_node_access(conn, *target,
					    conn->initiator_name,
					    conn->initiator_addr);
	if (!result) {
		SPDK_ERRLOG("access denied\n");
		/* Not found */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_TARGET_NOT_FOUND;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	return 0;
}

/*
 * The function which is used to handle the part of normal login session
 * return:
 * 0, success;
 * SPDK_ISCSI_LOGIN_ERROR_PARAMETER, parameter error;
 */
static int
spdk_iscsi_op_login_session_normal(struct spdk_iscsi_conn *conn,
				   struct spdk_iscsi_pdu *rsp_pdu,
				   char *initiator_port_name,
				   struct iscsi_param *params,
				   struct spdk_iscsi_tgt_node **target,
				   int cid)
{
	const char *target_name;
	const char *target_short_name;
	struct iscsi_bhs_login_rsp *rsph;
	int rc = 0;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	target_name = spdk_iscsi_param_get_val(params, "TargetName");

	if (target_name == NULL) {
		SPDK_ERRLOG("TargetName is empty\n");
		/* Missing parameter */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	memset(conn->target_short_name, 0, MAX_TARGET_NAME);
	target_short_name = strstr(target_name, ":");
	if (target_short_name != NULL) {
		target_short_name++; /* Advance past the ':' */
		if (strlen(target_short_name) >= MAX_TARGET_NAME) {
			SPDK_ERRLOG("Target Short Name (%s) is more than %u characters\n",
				    target_short_name, MAX_TARGET_NAME);
			return rc;
		}
		snprintf(conn->target_short_name, MAX_TARGET_NAME, "%s",
			 target_short_name);
	}

	pthread_mutex_lock(&g_spdk_iscsi.mutex);
	rc = spdk_iscsi_op_login_check_target(conn, rsp_pdu, target_name, target);
	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

	if (rc < 0) {
		return rc;
	}

	conn->target = *target;
	conn->dev = (*target)->dev;
	conn->target_port = spdk_scsi_dev_find_port_by_id((*target)->dev,
			    conn->portal->group->tag);

	rc = spdk_iscsi_op_login_check_session(conn, rsp_pdu,
					       initiator_port_name, cid);
	if (rc < 0) {
		return rc;
	}

	/* force target flags */
	pthread_mutex_lock(&((*target)->mutex));
	rc = spdk_iscsi_op_login_negotiate_chap_param(conn, rsp_pdu, *target);
	pthread_mutex_unlock(&((*target)->mutex));

	return rc;
}

/*
 * This function is used to judge the session type
 * return
 * 0: success
 * otherwise, error
 */
static int
spdk_iscsi_op_login_session_type(struct spdk_iscsi_conn *conn,
				 struct spdk_iscsi_pdu *rsp_pdu,
				 enum session_type *session_type,
				 struct iscsi_param *params)
{
	const char *session_type_str;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	session_type_str = spdk_iscsi_param_get_val(params, "SessionType");
	if (session_type_str == NULL) {
		if (rsph->tsih != 0) {
			*session_type = SESSION_TYPE_NORMAL;
		} else {
			SPDK_ERRLOG("SessionType is empty\n");
			/* Missing parameter */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
	} else {
		if (strcasecmp(session_type_str, "Discovery") == 0) {
			*session_type = SESSION_TYPE_DISCOVERY;
		} else if (strcasecmp(session_type_str, "Normal") == 0) {
			*session_type = SESSION_TYPE_NORMAL;
		} else {
			*session_type = SESSION_TYPE_INVALID;
			SPDK_ERRLOG("SessionType is invalid\n");
			/* Missing parameter */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
	}
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Session Type: %s\n", session_type_str);

	return 0;
}
/*
 * This function is used to initialize the port info
 * return
 * 0: success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_initialize_port(struct spdk_iscsi_conn *conn,
				    struct spdk_iscsi_pdu *rsp_pdu,
				    char *initiator_port_name,
				    uint32_t name_length,
				    struct iscsi_param *params)
{
	const char *val;
	struct iscsi_bhs_login_rsp *rsph;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	/* Initiator Name and Port */
	val = spdk_iscsi_param_get_val(params, "InitiatorName");
	if (val == NULL) {
		SPDK_ERRLOG("InitiatorName is empty\n");
		/* Missing parameter */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	snprintf(conn->initiator_name, sizeof(conn->initiator_name), "%s", val);
	snprintf(initiator_port_name, name_length,
		 "%s,i,0x%12.12" PRIx64, val, spdk_iscsi_get_isid(rsph->isid));
	spdk_strlwr(conn->initiator_name);
	spdk_strlwr(initiator_port_name);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Initiator name: %s\n", conn->initiator_name);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Initiator port: %s\n", initiator_port_name);

	return 0;
}

/*
 * This function is used to set the info in the connection data structure
 * return
 * 0: success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_set_conn_info(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_pdu *rsp_pdu,
				  char *initiator_port_name,
				  enum session_type session_type,
				  struct spdk_iscsi_tgt_node *target, int cid)
{
	int rc = 0;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	conn->authenticated = 0;
	conn->auth.chap_phase = ISCSI_CHAP_PHASE_WAIT_A;
	conn->cid = cid;

	if (conn->sess == NULL) {
		/* new session */
		rc = spdk_create_iscsi_sess(conn, target, session_type);
		if (rc < 0) {
			SPDK_ERRLOG("create_sess() failed\n");
			rsph->status_class = ISCSI_CLASS_TARGET_ERROR;
			rsph->status_detail = ISCSI_LOGIN_STATUS_NO_RESOURCES;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}

		/* initialize parameters */
		conn->StatSN = from_be32(&rsph->stat_sn);
		conn->sess->initiator_port = spdk_scsi_port_create(spdk_iscsi_get_isid(rsph->isid),
					     0, initiator_port_name);
		conn->sess->isid = spdk_iscsi_get_isid(rsph->isid);
		conn->sess->target = target;

		/* Discovery sessions will not have a target. */
		if (target != NULL) {
			conn->sess->queue_depth = target->queue_depth;
		} else {
			/*
			 * Assume discovery sessions have an effective command
			 *  windows size of 1.
			 */
			conn->sess->queue_depth = 1;
		}
		conn->sess->ExpCmdSN = rsp_pdu->cmd_sn;
		conn->sess->MaxCmdSN = rsp_pdu->cmd_sn + conn->sess->queue_depth - 1;
	}

	conn->initiator_port = conn->sess->initiator_port;

	return 0;
}

/*
 * This function is used to set the target info
 * return
 * 0: success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_set_target_info(struct spdk_iscsi_conn *conn,
				    struct spdk_iscsi_pdu *rsp_pdu,
				    enum session_type session_type,
				    int alloc_len,
				    struct spdk_iscsi_tgt_node *target)
{
	char buf[MAX_TMPBUF];
	const char *val;
	int rc = 0;
	struct spdk_iscsi_portal *portal = conn->portal;

	/* declarative parameters */
	if (target != NULL) {
		pthread_mutex_lock(&target->mutex);
		if (target->alias != NULL) {
			snprintf(buf, sizeof buf, "%s", target->alias);
		} else {
			snprintf(buf, sizeof buf, "%s", "");
		}
		pthread_mutex_unlock(&target->mutex);
		rc = spdk_iscsi_param_set(conn->sess->params, "TargetAlias", buf);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_param_set() failed\n");
			return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
		}
	}
	snprintf(buf, sizeof buf, "%s:%s,%d", portal->host, portal->port,
		 portal->group->tag);
	rc = spdk_iscsi_param_set(conn->sess->params, "TargetAddress", buf);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	snprintf(buf, sizeof buf, "%d", portal->group->tag);
	rc = spdk_iscsi_param_set(conn->sess->params, "TargetPortalGroupTag", buf);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}

	/* write in response */
	if (target != NULL) {
		val = spdk_iscsi_param_get_val(conn->sess->params, "TargetAlias");
		if (val != NULL && strlen(val) != 0) {
			rsp_pdu->data_segment_len = spdk_iscsi_append_param(conn,
						    "TargetAlias",
						    rsp_pdu->data,
						    alloc_len,
						    rsp_pdu->data_segment_len);
		}
		if (session_type == SESSION_TYPE_DISCOVERY) {
			rsp_pdu->data_segment_len = spdk_iscsi_append_param(conn,
						    "TargetAddress",
						    rsp_pdu->data,
						    alloc_len,
						    rsp_pdu->data_segment_len);
		}
		rsp_pdu->data_segment_len = spdk_iscsi_append_param(conn,
					    "TargetPortalGroupTag",
					    rsp_pdu->data,
					    alloc_len,
					    rsp_pdu->data_segment_len);
	}

	return rc;
}

/*
 * This function is used to handle the login of iscsi initiator when there is
 * no session
 * return:
 * 0, success;
 * SPDK_ISCSI_LOGIN_ERROR_PARAMETER, parameter error;
 * SPDK_ISCSI_LOGIN_ERROR_RESPONSE,  used to notify the login fail.
 */
static int
spdk_iscsi_op_login_phase_none(struct spdk_iscsi_conn *conn,
			       struct spdk_iscsi_pdu *rsp_pdu,
			       struct iscsi_param *params,
			       int alloc_len, int cid)
{
	enum session_type session_type;
	char initiator_port_name[MAX_INITIATOR_NAME];
	struct iscsi_bhs_login_rsp *rsph;
	struct spdk_iscsi_tgt_node *target = NULL;
	int rc = 0;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	conn->target = NULL;
	conn->dev = NULL;

	rc = spdk_iscsi_op_login_initialize_port(conn, rsp_pdu,
			initiator_port_name, MAX_INITIATOR_NAME, params);
	if (rc < 0) {
		return rc;
	}

	rc = spdk_iscsi_op_login_session_type(conn, rsp_pdu, &session_type,
					      params);
	if (rc < 0) {
		return rc;
	}

	/* Target Name and Port */
	if (session_type == SESSION_TYPE_NORMAL) {
		rc = spdk_iscsi_op_login_session_normal(conn, rsp_pdu,
							initiator_port_name,
							params, &target, cid);
		if (rc < 0) {
			return rc;
		}

	} else if (session_type == SESSION_TYPE_DISCOVERY) {
		target = NULL;
		rsph->tsih = 0;

		/* force target flags */
		pthread_mutex_lock(&g_spdk_iscsi.mutex);
		rc = spdk_iscsi_op_login_session_discovery_chap(conn);
		pthread_mutex_unlock(&g_spdk_iscsi.mutex);
		if (rc < 0) {
			return rc;
		}
	} else {
		SPDK_ERRLOG("unknown session type\n");
		/* Missing parameter */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	rc = spdk_iscsi_op_login_set_conn_info(conn, rsp_pdu, initiator_port_name,
					       session_type, target, cid);
	if (rc < 0) {
		return rc;
	}

	/* limit conns on discovery session */
	if (session_type == SESSION_TYPE_DISCOVERY) {
		conn->sess->MaxConnections = 1;
		rc = spdk_iscsi_param_set_int(conn->sess->params,
					      "MaxConnections",
					      conn->sess->MaxConnections);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_param_set_int() failed\n");
			return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
		}
	}

	rc = spdk_iscsi_op_login_set_target_info(conn, rsp_pdu, session_type,
			alloc_len, target);
	if (rc < 0) {
		return rc;
	}

	return rc;
}

/*
 * The function which is used to initialize the internal response data
 * structure of iscsi login function.
 * return:
 * 0, success;
 * otherwise, error;
 */
static int
spdk_iscsi_op_login_rsp_init(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_pdu *pdu, struct spdk_iscsi_pdu *rsp_pdu,
			     struct iscsi_param **params, int *alloc_len, int *cid)
{

	struct iscsi_bhs_login_req *reqh;
	struct iscsi_bhs_login_rsp *rsph;
	int rc;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	rsph->opcode = ISCSI_OP_LOGIN_RSP;
	rsph->status_class = ISCSI_CLASS_SUCCESS;
	rsph->status_detail = ISCSI_LOGIN_ACCEPT;
	rsp_pdu->data_segment_len = 0;

	/* Default MaxRecvDataSegmentLength - RFC3720(12.12) */
	if (conn->MaxRecvDataSegmentLength < 8192) {
		*alloc_len = 8192;
	} else {
		*alloc_len = conn->MaxRecvDataSegmentLength;
	}

	rsp_pdu->data = calloc(1, *alloc_len);
	if (!rsp_pdu->data) {
		SPDK_ERRLOG("calloc() failed for data segment\n");
		return -ENOMEM;
	}

	reqh = (struct iscsi_bhs_login_req *)&pdu->bhs;
	rsph->flags |= (reqh->flags & ISCSI_LOGIN_TRANSIT);
	rsph->flags |= (reqh->flags & ISCSI_LOGIN_CONTINUE);
	rsph->flags |= (reqh->flags & ISCSI_LOGIN_CURRENT_STAGE_MASK);
	if (ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags)) {
		rsph->flags |= (reqh->flags & ISCSI_LOGIN_NEXT_STAGE_MASK);
	}

	/* We don't need to convert from network byte order. Just store it */
	memcpy(&rsph->isid, reqh->isid, 6);
	rsph->tsih = reqh->tsih;
	rsph->itt = reqh->itt;
	rsp_pdu->cmd_sn = from_be32(&reqh->cmd_sn);
	*cid = from_be16(&reqh->cid);

	if (rsph->tsih) {
		rsph->stat_sn = reqh->exp_stat_sn;
	}

	SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "PDU", (uint8_t *)&pdu->bhs, ISCSI_BHS_LEN);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
		      "T=%d, C=%d, CSG=%d, NSG=%d, Min=%d, Max=%d, ITT=%x\n",
		      ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags),
		      ISCSI_BHS_LOGIN_GET_CBIT(rsph->flags),
		      ISCSI_BHS_LOGIN_GET_CSG(rsph->flags),
		      ISCSI_BHS_LOGIN_GET_NSG(rsph->flags),
		      reqh->version_min, reqh->version_max, from_be32(&rsph->itt));

	if (conn->sess != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "CmdSN=%u, ExpStatSN=%u, StatSN=%u, ExpCmdSN=%u,"
			      "MaxCmdSN=%u\n", rsp_pdu->cmd_sn,
			      from_be32(&rsph->stat_sn), conn->StatSN,
			      conn->sess->ExpCmdSN,
			      conn->sess->MaxCmdSN);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "CmdSN=%u, ExpStatSN=%u, StatSN=%u\n",
			      rsp_pdu->cmd_sn, from_be32(&rsph->stat_sn),
			      conn->StatSN);
	}

	if (ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags) &&
	    ISCSI_BHS_LOGIN_GET_CBIT(rsph->flags)) {
		SPDK_ERRLOG("transit error\n");
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	/* make sure reqh->version_max < ISCSI_VERSION */
	if (reqh->version_min > ISCSI_VERSION) {
		SPDK_ERRLOG("unsupported version %d/%d\n", reqh->version_min,
			    reqh->version_max);
		/* Unsupported version */
		/* set all reserved flag to zero */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_UNSUPPORTED_VERSION;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	if ((ISCSI_BHS_LOGIN_GET_NSG(rsph->flags) == ISCSI_NSG_RESERVED_CODE) &&
	    ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags)) {
		/* set NSG to zero */
		rsph->flags &= ~ISCSI_LOGIN_NEXT_STAGE_MASK;
		/* also set other bits to zero */
		rsph->flags &= ~ISCSI_LOGIN_TRANSIT;
		rsph->flags &= ~ISCSI_LOGIN_CURRENT_STAGE_MASK;
		SPDK_ERRLOG("Received reserved NSG code: %d\n", ISCSI_NSG_RESERVED_CODE);
		/* Initiator error */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	/* store incoming parameters */
	rc = spdk_iscsi_parse_params(params, pdu->data,
				     pdu->data_segment_len, ISCSI_BHS_LOGIN_GET_CBIT(reqh->flags),
				     &conn->partial_text_parameter);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_parse_params() failed\n");
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	return 0;
}

/*
 * This function is used to set the csg bit case in rsp
 * return:
 * 0, success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_rsp_handle_csg_bit(struct spdk_iscsi_conn *conn,
				       struct spdk_iscsi_pdu *rsp_pdu,
				       struct iscsi_param *params, int alloc_len)
{
	const char *auth_method;
	int rc;
	struct iscsi_bhs_login_rsp *rsph;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	switch (ISCSI_BHS_LOGIN_GET_CSG(rsph->flags)) {
	case ISCSI_SECURITY_NEGOTIATION_PHASE:
		/* SecurityNegotiation */
		auth_method = spdk_iscsi_param_get_val(conn->params, "AuthMethod");
		if (auth_method == NULL) {
			SPDK_ERRLOG("AuthMethod is empty\n");
			/* Missing parameter */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
		if (strcasecmp(auth_method, "None") == 0) {
			conn->authenticated = 1;
		} else {
			rc = spdk_iscsi_auth_params(conn, params, auth_method,
						    rsp_pdu->data, alloc_len,
						    rsp_pdu->data_segment_len);
			if (rc < 0) {
				SPDK_ERRLOG("iscsi_auth_params() failed\n");
				/* Authentication failure */
				rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
				rsph->status_detail = ISCSI_LOGIN_AUTHENT_FAIL;
				return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
			}
			rsp_pdu->data_segment_len = rc;
			if (conn->authenticated == 0) {
				/* not complete */
				rsph->flags &= ~ISCSI_LOGIN_TRANSIT;
			} else {
				if (conn->auth.chap_phase != ISCSI_CHAP_PHASE_END) {
					SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "CHAP phase not complete");
				}
			}

			SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "Negotiated Auth Params",
				       rsp_pdu->data, rsp_pdu->data_segment_len);
		}
		break;

	case ISCSI_OPERATIONAL_NEGOTIATION_PHASE:
		/* LoginOperationalNegotiation */
		if (conn->state == ISCSI_CONN_STATE_INVALID) {
			if (conn->req_auth) {
				/* Authentication failure */
				rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
				rsph->status_detail = ISCSI_LOGIN_AUTHENT_FAIL;
				return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
			} else {
				/* AuthMethod=None */
				conn->authenticated = 1;
			}
		}
		if (conn->authenticated == 0) {
			SPDK_ERRLOG("authentication error\n");
			/* Authentication failure */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = ISCSI_LOGIN_AUTHENT_FAIL;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
		break;

	case ISCSI_FULL_FEATURE_PHASE:
		/* FullFeaturePhase */
		SPDK_ERRLOG("XXX Login in FullFeaturePhase\n");
		/* Initiator error */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;

	default:
		SPDK_ERRLOG("unknown stage\n");
		/* Initiator error */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	return 0;
}

/* This function is used to notify the session info
 * return
 * 0: success
 * otherwise: error
 */
static int
spdk_iscsi_op_login_notify_session_info(struct spdk_iscsi_conn *conn,
					struct spdk_iscsi_pdu *rsp_pdu)
{
	struct spdk_iscsi_portal *portal = conn->portal;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	if (conn->sess->session_type == SESSION_TYPE_NORMAL) {
		/* normal session */
		SPDK_NOTICELOG("Login from %s (%s) on %s tgt_node%d"
			       " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			       " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			       conn->initiator_name, conn->initiator_addr,
			       conn->target->name, conn->target->num,
			       portal->host, portal->port, portal->group->tag,
			       conn->sess->isid, conn->sess->tsih, conn->cid,
			       (spdk_iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
				? "on" : "off"),
			       (spdk_iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
				? "on" : "off"));
	} else if (conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		/* discovery session */
		SPDK_NOTICELOG("Login(discovery) from %s (%s) on"
			       " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			       " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			       conn->initiator_name, conn->initiator_addr,
			       portal->host, portal->port, portal->group->tag,
			       conn->sess->isid, conn->sess->tsih, conn->cid,
			       (spdk_iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
				? "on" : "off"),
			       (spdk_iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
				? "on" : "off"));
	} else {
		SPDK_ERRLOG("unknown session type\n");
		/* Initiator error */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	return 0;
}

/*
 * This function is to handle the tbit cases
 * return
 * 0: success
 * otherwise error
 */
static int
spdk_iscsi_op_login_rsp_handle_t_bit(struct spdk_iscsi_conn *conn,
				     struct spdk_iscsi_pdu *rsp_pdu)
{
	int rc;
	struct iscsi_bhs_login_rsp *rsph;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	switch (ISCSI_BHS_LOGIN_GET_NSG(rsph->flags)) {
	case ISCSI_SECURITY_NEGOTIATION_PHASE:
		/* SecurityNegotiation */
		conn->login_phase = ISCSI_SECURITY_NEGOTIATION_PHASE;
		break;

	case ISCSI_OPERATIONAL_NEGOTIATION_PHASE:
		/* LoginOperationalNegotiation */
		conn->login_phase = ISCSI_OPERATIONAL_NEGOTIATION_PHASE;
		break;

	case ISCSI_FULL_FEATURE_PHASE:
		/* FullFeaturePhase */
		conn->login_phase = ISCSI_FULL_FEATURE_PHASE;
		to_be16(&rsph->tsih, conn->sess->tsih);

		rc = spdk_iscsi_op_login_notify_session_info(conn, rsp_pdu);
		if (rc < 0) {
			return rc;
		}

		conn->full_feature = 1;
		spdk_iscsi_conn_migration(conn);
		break;

	default:
		SPDK_ERRLOG("unknown stage\n");
		/* Initiator error */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	return 0;
}

/*
 * This function is used to set the values of the internal data structure used
 * by spdk_iscsi_op_login function
 * return:
 * 0, used to notify the a successful login
 * SPDK_ISCSI_LOGIN_ERROR_RESPONSE,  used to notify a failure login.
 */
static int
spdk_iscsi_op_login_rsp_handle(struct spdk_iscsi_conn *conn,
			       struct spdk_iscsi_pdu *rsp_pdu, struct iscsi_param **params,
			       int alloc_len)
{
	int rc;
	struct iscsi_bhs_login_rsp *rsph;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	/* negotiate parameters */
	rc = spdk_iscsi_negotiate_params(conn, params, rsp_pdu->data, alloc_len,
					 rsp_pdu->data_segment_len);
	if (rc < 0) {
		/*
		 * spdk_iscsi_negotiate_params just returns -1 on failure,
		 *  so translate this into meaningful response codes and
		 *  return values.
		 */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	rsp_pdu->data_segment_len = rc;
	SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "Negotiated Params", rsp_pdu->data, rc);

	/* handle the CSG bit case */
	rc = spdk_iscsi_op_login_rsp_handle_csg_bit(conn, rsp_pdu, *params,
			alloc_len);
	if (rc < 0) {
		return rc;
	}

	/* handle the T bit case */
	if (ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags)) {
		rc = spdk_iscsi_op_login_rsp_handle_t_bit(conn, rsp_pdu);
	}

	return rc;
}

static int
spdk_iscsi_op_login(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int rc;
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_param *params = NULL;
	struct iscsi_param **params_p = &params;
	int alloc_len;
	int cid;

	if (conn->full_feature && conn->sess != NULL &&
	    conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	rsp_pdu = spdk_get_pdu();
	if (rsp_pdu == NULL) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	rc = spdk_iscsi_op_login_rsp_init(conn, pdu, rsp_pdu, params_p,
					  &alloc_len, &cid);
	if (rc == SPDK_ISCSI_LOGIN_ERROR_RESPONSE || rc == SPDK_ISCSI_LOGIN_ERROR_PARAMETER) {
		spdk_iscsi_op_login_response(conn, rsp_pdu, *params_p);
		return rc;
	}

	/* For other values, we need to directly return */
	if (rc < 0) {
		spdk_put_pdu(rsp_pdu);
		return rc;
	}

	if (conn->state == ISCSI_CONN_STATE_INVALID) {
		rc = spdk_iscsi_op_login_phase_none(conn, rsp_pdu, *params_p,
						    alloc_len, cid);
		if (rc == SPDK_ISCSI_LOGIN_ERROR_RESPONSE || rc == SPDK_ISCSI_LOGIN_ERROR_PARAMETER) {
			spdk_iscsi_op_login_response(conn, rsp_pdu, *params_p);
			return rc;
		}
	}

	rc = spdk_iscsi_op_login_rsp_handle(conn, rsp_pdu, params_p, alloc_len);
	if (rc == SPDK_ISCSI_LOGIN_ERROR_RESPONSE) {
		spdk_iscsi_op_login_response(conn, rsp_pdu, *params_p);
		return rc;
	}

	rc = spdk_iscsi_op_login_response(conn, rsp_pdu, *params_p);
	if (rc == 0) {
		conn->state = ISCSI_CONN_STATE_RUNNING;
	} else {
		SPDK_ERRLOG("login error - connection will be destroyed\n");
	}

	return rc;
}

static int
spdk_iscsi_op_text(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct iscsi_param *params = NULL;
	struct iscsi_param **params_p = &params;
	struct spdk_iscsi_pdu *rsp_pdu;
	uint8_t *data;
	uint64_t lun;
	uint32_t task_tag;
	uint32_t CmdSN;
	uint32_t ExpStatSN;
	const char *val;
	int F_bit, C_bit;
	int data_len;
	int alloc_len;
	int rc;
	struct iscsi_bhs_text_req *reqh;
	struct iscsi_bhs_text_resp *rsph;

	data_len = 0;
	alloc_len = conn->MaxRecvDataSegmentLength;

	reqh = (struct iscsi_bhs_text_req *)&pdu->bhs;

	F_bit = !!(reqh->flags & ISCSI_FLAG_FINAL);
	C_bit = !!(reqh->flags & ISCSI_TEXT_CONTINUE);
	lun = from_be64(&reqh->lun);
	task_tag = from_be32(&reqh->itt);
	CmdSN = from_be32(&reqh->cmd_sn);
	pdu->cmd_sn = CmdSN;
	ExpStatSN = from_be32(&reqh->exp_stat_sn);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "I=%d, F=%d, C=%d, ITT=%x, TTT=%x\n",
		      reqh->immediate, F_bit, C_bit, task_tag, from_be32(&reqh->ttt));

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
		      "CmdSN=%u, ExpStatSN=%u, StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      CmdSN, ExpStatSN, conn->StatSN, conn->sess->ExpCmdSN,
		      conn->sess->MaxCmdSN);

	if (ExpStatSN != conn->StatSN) {
#if 0
		SPDK_ERRLOG("StatSN(%u) error\n", ExpStatSN);
		return -1;
#else
		/* StarPort have a bug */
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "StatSN(%u) rewound\n", ExpStatSN);
		conn->StatSN = ExpStatSN;
#endif
	}

	if (F_bit && C_bit) {
		SPDK_ERRLOG("final and continue\n");
		return -1;
	}

	/*
	 * If this is the first text op in a sequence, save the ITT so we can
	 * compare it against the ITT for subsequent ops in the same sequence.
	 * If a subsequent text op in same sequence has a different ITT, reject
	 * that PDU.
	 */
	if (conn->sess->current_text_itt == 0xffffffffU) {
		conn->sess->current_text_itt = task_tag;
	} else if (conn->sess->current_text_itt != task_tag) {
		SPDK_ERRLOG("The correct itt is %u, and the current itt is %u...\n",
			    conn->sess->current_text_itt, task_tag);
		return spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	/* store incoming parameters */
	rc = spdk_iscsi_parse_params(&params, pdu->data, pdu->data_segment_len,
				     C_bit, &conn->partial_text_parameter);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_parse_params() failed\n");
		spdk_iscsi_param_free(params);
		return -1;
	}

	data = calloc(1, alloc_len);
	if (!data) {
		SPDK_ERRLOG("calloc() failed for data segment\n");
		spdk_iscsi_param_free(params);
		return -ENOMEM;
	}

	/* negotiate parameters */
	data_len = spdk_iscsi_negotiate_params(conn, params_p,
					       data, alloc_len, data_len);
	if (data_len < 0) {
		SPDK_ERRLOG("spdk_iscsi_negotiate_params() failed\n");
		spdk_iscsi_param_free(*params_p);
		free(data);
		return -1;
	}

	/* sendtargets is special case */
	val = spdk_iscsi_param_get_val(*params_p, "SendTargets");
	if (val != NULL) {
		if (spdk_iscsi_param_eq_val(conn->sess->params,
					    "SessionType", "Discovery")) {
			if (strcasecmp(val, "") == 0) {
				val = "ALL";
			}

			data_len = spdk_iscsi_send_tgts(conn,
							conn->initiator_name,
							conn->initiator_addr,
							val, data, alloc_len,
							data_len);
		} else {
			if (strcasecmp(val, "") == 0) {
				val = conn->target->name;
			}

			if (strcasecmp(val, "ALL") == 0) {
				/* not in discovery session */
				data_len = spdk_iscsi_append_text(conn,
								  "SendTargets",
								  "Reject", data,
								  alloc_len,
								  data_len);
			} else {
				data_len = spdk_iscsi_send_tgts(conn,
								conn->initiator_name,
								conn->initiator_addr,
								val, data, alloc_len,
								data_len);
			}
		}
	} else {
		if (spdk_iscsi_param_eq_val(conn->sess->params, "SessionType", "Discovery")) {
			spdk_iscsi_param_free(*params_p);
			free(data);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "Negotiated Params", data, data_len);

	/* response PDU */
	rsp_pdu = spdk_get_pdu();
	if (rsp_pdu == NULL) {
		spdk_iscsi_param_free(*params_p);
		free(data);
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	rsph = (struct iscsi_bhs_text_resp *)&rsp_pdu->bhs;

	rsp_pdu->data = data;
	rsph->opcode = ISCSI_OP_TEXT_RSP;

	if (F_bit) {
		rsph->flags |= ISCSI_FLAG_FINAL;
	}

	if (C_bit) {
		rsph->flags |= ISCSI_TEXT_CONTINUE;
	}

	DSET24(rsph->data_segment_len, data_len);
	to_be64(&rsph->lun, lun);
	to_be32(&rsph->itt, task_tag);

	if (F_bit) {
		rsph->ttt = 0xffffffffU;
		conn->sess->current_text_itt = 0xffffffffU;
	} else {
		to_be32(&rsph->ttt, 1 + conn->id);
	}

	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;

	if (reqh->immediate == 0) {
		conn->sess->MaxCmdSN++;
	}

	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);

	/* update internal variables */
	rc = spdk_iscsi_copy_param2var(conn);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_iscsi_copy_param2var() failed\n");
		spdk_iscsi_param_free(*params_p);
		return -1;
	}

	/* check value */
	rc = spdk_iscsi_check_values(conn);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_check_values() failed\n");
		spdk_iscsi_param_free(*params_p);
		return -1;
	}

	spdk_iscsi_param_free(*params_p);
	return 0;
}

static int
spdk_iscsi_op_logout(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	char buf[MAX_TMPBUF];
	struct spdk_iscsi_pdu *rsp_pdu;
	uint32_t task_tag;
	uint32_t CmdSN;
	uint32_t ExpStatSN;
	int response;
	struct iscsi_bhs_logout_req *reqh;
	struct iscsi_bhs_logout_resp *rsph;
	uint16_t cid;

	reqh = (struct iscsi_bhs_logout_req *)&pdu->bhs;

	cid = from_be16(&reqh->cid);
	task_tag = from_be32(&reqh->itt);
	CmdSN = from_be32(&reqh->cmd_sn);
	pdu->cmd_sn = CmdSN;
	ExpStatSN = from_be32(&reqh->exp_stat_sn);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "reason=%d, ITT=%x, cid=%d\n",
		      reqh->reason, task_tag, cid);

	if (reqh->reason != 0 && conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		SPDK_ERRLOG("only logout with close the session reason can be in discovery session");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	if (conn->sess != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI,
			      "CmdSN=%u, ExpStatSN=%u, StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
			      CmdSN, ExpStatSN, conn->StatSN,
			      conn->sess->ExpCmdSN, conn->sess->MaxCmdSN);

		if (CmdSN != conn->sess->ExpCmdSN) {
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "CmdSN(%u) might have dropped\n", CmdSN);
			/* ignore error */
		}
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "CmdSN=%u, ExpStatSN=%u, StatSN=%u\n",
			      CmdSN, ExpStatSN, conn->StatSN);
	}

	if (ExpStatSN != conn->StatSN) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "StatSN(%u/%u) might have dropped\n",
			      ExpStatSN, conn->StatSN);
		/* ignore error */
	}

	if (conn->id == cid) {
		response = 0; // connection or session closed successfully
		spdk_iscsi_conn_logout(conn);
	} else {
		response = 1;
	}

	/* response PDU */
	rsp_pdu = spdk_get_pdu();
	if (rsp_pdu == NULL) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	rsph = (struct iscsi_bhs_logout_resp *)&rsp_pdu->bhs;
	rsp_pdu->data = NULL;
	rsph->opcode = ISCSI_OP_LOGOUT_RSP;
	rsph->flags |= 0x80; /* bit 0 must be 1 */
	rsph->response = response;
	DSET24(rsph->data_segment_len, 0);
	to_be32(&rsph->itt, task_tag);

	if (conn->sess != NULL) {
		to_be32(&rsph->stat_sn, conn->StatSN);
		conn->StatSN++;

		if (conn->sess->connections == 1) {
			conn->sess->MaxCmdSN++;
		}

		to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
		to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);
	} else {
		to_be32(&rsph->stat_sn, conn->StatSN);
		conn->StatSN++;
		to_be32(&rsph->exp_cmd_sn, CmdSN);
		to_be32(&rsph->max_cmd_sn, CmdSN);
	}

	rsph->time_2_wait = 0;
	rsph->time_2_retain = 0;

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);

	if (conn->sess == NULL) {
		/*
		 * login failed but initiator still sent a logout rather than
		 *  just closing the TCP connection.
		 */
		snprintf(buf, sizeof buf, "Logout(login failed) from %s (%s) on"
			 " (%s:%s,%d)\n",
			 conn->initiator_name, conn->initiator_addr,
			 conn->portal_host, conn->portal_port, conn->pg_tag);
	} else if (spdk_iscsi_param_eq_val(conn->sess->params, "SessionType", "Normal")) {
		snprintf(buf, sizeof buf, "Logout from %s (%s) on %s tgt_node%d"
			 " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			 " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			 conn->initiator_name, conn->initiator_addr,
			 conn->target->name, conn->target->num,
			 conn->portal_host, conn->portal_port, conn->pg_tag,
			 conn->sess->isid, conn->sess->tsih, conn->cid,
			 (spdk_iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
			  ? "on" : "off"),
			 (spdk_iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
			  ? "on" : "off"));
	} else {
		/* discovery session */
		snprintf(buf, sizeof buf, "Logout(discovery) from %s (%s) on"
			 " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			 " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			 conn->initiator_name, conn->initiator_addr,
			 conn->portal_host, conn->portal_port, conn->pg_tag,
			 conn->sess->isid, conn->sess->tsih, conn->cid,
			 (spdk_iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
			  ? "on" : "off"),
			 (spdk_iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
			  ? "on" : "off"));
	}

	SPDK_NOTICELOG("%s", buf);

	return 0;
}

/* This function returns the spdk_scsi_task by searching the snack list via
 * task transfertag and the pdu's opcode
 */
static struct spdk_iscsi_task *
spdk_get_scsi_task_from_ttt(struct spdk_iscsi_conn *conn,
			    uint32_t transfer_tag)
{
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_data_in *datain_bhs;

	TAILQ_FOREACH(pdu, &conn->snack_pdu_list, tailq) {
		if (pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN) {
			datain_bhs = (struct iscsi_bhs_data_in *)&pdu->bhs;
			if (from_be32(&datain_bhs->ttt) == transfer_tag) {
				return pdu->task;
			}
		}
	}

	return NULL;
}

/* This function returns the spdk_scsi_task by searching the snack list via
 * initiator task tag and the pdu's opcode
 */
static struct spdk_iscsi_task *
spdk_get_scsi_task_from_itt(struct spdk_iscsi_conn *conn,
			    uint32_t task_tag, enum iscsi_op opcode)
{
	struct spdk_iscsi_pdu *pdu;

	TAILQ_FOREACH(pdu, &conn->snack_pdu_list, tailq) {
		if (pdu->bhs.opcode == opcode &&
		    pdu->task != NULL &&
		    pdu->task->tag == task_tag) {
			return pdu->task;
		}
	}

	return NULL;
}

static int
spdk_iscsi_send_datain(struct spdk_iscsi_conn *conn,
		       struct spdk_iscsi_task *task, int datain_flag,
		       int residual_len, int offset, int DataSN, int len)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_data_in *rsph;
	uint32_t task_tag;
	uint32_t transfer_tag;
	int F_bit, U_bit, O_bit, S_bit;
	struct spdk_iscsi_task *primary;

	primary = spdk_iscsi_task_get_primary(task);

	/* DATA PDU */
	rsp_pdu = spdk_get_pdu();
	rsph = (struct iscsi_bhs_data_in *)&rsp_pdu->bhs;
	rsp_pdu->data = task->scsi.iovs[0].iov_base + offset;
	rsp_pdu->data_from_mempool = true;

	task_tag = task->tag;
	transfer_tag = 0xffffffffU;

	F_bit = datain_flag & ISCSI_FLAG_FINAL;
	O_bit = datain_flag & ISCSI_DATAIN_OVERFLOW;
	U_bit = datain_flag & ISCSI_DATAIN_UNDERFLOW;
	S_bit = datain_flag & ISCSI_DATAIN_STATUS;

	/*
	 * we need to hold onto this task/cmd because until the
	 * PDU has been written out
	 */
	rsp_pdu->task = task;
	task->scsi.ref++;

	rsph->opcode = ISCSI_OP_SCSI_DATAIN;

	if (F_bit) {
		rsph->flags |= ISCSI_FLAG_FINAL;
	}

	/* we leave the A_bit clear */

	if (F_bit && S_bit)  {
		if (O_bit) {
			rsph->flags |= ISCSI_DATAIN_OVERFLOW;
		}

		if (U_bit) {
			rsph->flags |= ISCSI_DATAIN_UNDERFLOW;
		}
	}

	if (S_bit) {
		rsph->flags |= ISCSI_DATAIN_STATUS;
		rsph->status = task->scsi.status;
	}

	DSET24(rsph->data_segment_len, len);

	to_be32(&rsph->itt, task_tag);
	to_be32(&rsph->ttt, transfer_tag);

	if (S_bit) {
		to_be32(&rsph->stat_sn, conn->StatSN);
		conn->StatSN++;
	}

	if (F_bit && S_bit && !spdk_iscsi_task_is_immediate(primary)) {
		conn->sess->MaxCmdSN++;
	}

	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	to_be32(&rsph->data_sn, DataSN);

	if (conn->sess->ErrorRecoveryLevel >= 1) {
		primary->datain_datasn = DataSN;
	}
	DataSN++;

	if (task->parent) {
		offset += primary->scsi.data_transferred;
	}
	to_be32(&rsph->buffer_offset, (uint32_t)offset);

	if (F_bit && S_bit) {
		to_be32(&rsph->res_cnt, residual_len);
	}

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);

	return DataSN;
}

static int
spdk_iscsi_transfer_in(struct spdk_iscsi_conn *conn,
		       struct spdk_iscsi_task *task)
{
	uint32_t DataSN;
	int transfer_len;
	int data_len;
	int segment_len;
	int offset;
	int residual_len = 0;
	int sent_status;
	int len;
	int datain_flag = 0;
	int datain_seq_cnt;
	int i;
	int sequence_end;
	struct spdk_iscsi_task *primary;

	primary = spdk_iscsi_task_get_primary(task);
	segment_len = conn->MaxRecvDataSegmentLength;
	data_len = task->scsi.data_transferred;
	transfer_len = task->scsi.length;

	if (task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		if (task != primary) {
			conn->data_in_cnt--;
			/* Handle the case when primary task return success but the subtask failed */
			if (primary->bytes_completed == primary->scsi.transfer_len &&
			    primary->scsi.status == SPDK_SCSI_STATUS_GOOD) {
				conn->data_in_cnt--;
			}
		} else {
			/* handle the case that it is a primary task which has subtasks */
			if (primary->scsi.transfer_len != primary->scsi.length) {
				conn->data_in_cnt--;
			}
		}

		return 0;
	}

	if (data_len < transfer_len) {
		/* underflow */
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Underflow %u/%u\n", data_len, transfer_len);
		residual_len = transfer_len - data_len;
		transfer_len = data_len;
		datain_flag |= ISCSI_DATAIN_UNDERFLOW;
	} else if (data_len > transfer_len) {
		/* overflow */
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Overflow %u/%u\n", data_len, transfer_len);
		residual_len = data_len - transfer_len;
		datain_flag |= ISCSI_DATAIN_OVERFLOW;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Transfer %u\n", transfer_len);
		residual_len = 0;
	}

	DataSN = primary->datain_datasn;
	sent_status = 0;

	/* calculate the number of sequences for all data-in pdus */
	datain_seq_cnt = 1 + ((transfer_len - 1) / (int)conn->sess->MaxBurstLength);
	for (i = 0; i < datain_seq_cnt; i++) {
		offset = i * conn->sess->MaxBurstLength;
		sequence_end = DMIN32(((i + 1) * conn->sess->MaxBurstLength),
				      transfer_len);

		/* send data splitted by segment_len */
		for (; offset < sequence_end; offset += segment_len) {
			len = DMIN32(segment_len, (sequence_end - offset));

			datain_flag &= ~ISCSI_FLAG_FINAL;
			datain_flag &= ~ISCSI_DATAIN_STATUS;

			if (offset + len == sequence_end) {
				/* last PDU in a sequence */
				datain_flag |= ISCSI_FLAG_FINAL;
				if (task->scsi.sense_data_len == 0) {
					/* The last pdu in all data-in pdus */
					if ((offset + len) == transfer_len &&
					    (primary->bytes_completed == primary->scsi.transfer_len)) {
						datain_flag |= ISCSI_DATAIN_STATUS;
						sent_status = 1;
					}
				}
			}

			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Transfer=%d, Offset=%d, Len=%d\n",
				      sequence_end, offset, len);
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "StatSN=%u, DataSN=%u, Offset=%u, Len=%d\n",
				      conn->StatSN, DataSN, offset, len);

			DataSN = spdk_iscsi_send_datain(conn, task, datain_flag, residual_len,
							offset, DataSN, len);
		}
	}

	if (task != primary) {
		primary->scsi.data_transferred += task->scsi.data_transferred;
	}
	primary->datain_datasn = DataSN;

	return sent_status;
}

/*
 *  This function compare the input pdu's bhs with the pdu's bhs associated by
 *  active_r2t_tasks and queued_r2t_tasks in a connection
 */
static bool
spdk_iscsi_compare_pdu_bhs_within_existed_r2t_tasks(struct spdk_iscsi_conn *conn,
		struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task	*task;

	TAILQ_FOREACH(task, &conn->active_r2t_tasks, link) {
		if (!memcmp(&pdu->bhs, spdk_iscsi_task_get_bhs(task), ISCSI_BHS_LEN)) {
			return true;
		}
	}

	TAILQ_FOREACH(task, &conn->queued_r2t_tasks, link) {
		if (!memcmp(&pdu->bhs, spdk_iscsi_task_get_bhs(task), ISCSI_BHS_LEN)) {
			return true;
		}
	}

	return false;
}

static void spdk_iscsi_queue_task(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_task *task)
{
	spdk_trace_record(TRACE_ISCSI_TASK_QUEUE, conn->id, task->scsi.length,
			  (uintptr_t)task, (uintptr_t)task->pdu);
	task->is_queued = true;
	spdk_scsi_dev_queue_task(conn->dev, &task->scsi);
}

static void spdk_iscsi_queue_mgmt_task(struct spdk_iscsi_conn *conn,
				       struct spdk_iscsi_task *task,
				       enum spdk_scsi_task_func func)
{
	spdk_scsi_dev_queue_mgmt_task(conn->dev, &task->scsi, func);
}

int spdk_iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_task *task;

	while (!TAILQ_EMPTY(&conn->queued_datain_tasks) &&
	       conn->data_in_cnt < MAX_LARGE_DATAIN_PER_CONNECTION) {
		task = TAILQ_FIRST(&conn->queued_datain_tasks);
		assert(task->current_datain_offset <= task->scsi.transfer_len);

		if (task->current_datain_offset == 0) {
			task->scsi.lun = spdk_scsi_dev_get_lun(conn->dev, task->lun_id);
			if (task->scsi.lun == NULL) {
				TAILQ_REMOVE(&conn->queued_datain_tasks, task, link);
				spdk_scsi_task_process_null_lun(&task->scsi);
				spdk_iscsi_task_cpl(&task->scsi);
				return 0;
			}
			task->current_datain_offset = task->scsi.length;
			conn->data_in_cnt++;
			spdk_iscsi_queue_task(conn, task);
			continue;
		}
		if (task->current_datain_offset < task->scsi.transfer_len) {
			struct spdk_iscsi_task *subtask;
			uint32_t remaining_size = 0;

			remaining_size = task->scsi.transfer_len - task->current_datain_offset;
			subtask = spdk_iscsi_task_get(conn, task, spdk_iscsi_task_cpl);
			assert(subtask != NULL);
			subtask->scsi.offset = task->current_datain_offset;
			subtask->scsi.length = DMIN32(SPDK_BDEV_LARGE_BUF_MAX_SIZE, remaining_size);
			spdk_scsi_task_set_data(&subtask->scsi, NULL, 0);
			task->current_datain_offset += subtask->scsi.length;
			conn->data_in_cnt++;

			task->scsi.lun = spdk_scsi_dev_get_lun(conn->dev, task->lun_id);
			if (task->scsi.lun == NULL) {
				/* Remove the primary task from the list if this is the last subtask */
				if (task->current_datain_offset == task->scsi.transfer_len) {
					TAILQ_REMOVE(&conn->queued_datain_tasks, task, link);
				}
				subtask->scsi.transfer_len = subtask->scsi.length;
				spdk_scsi_task_process_null_lun(&subtask->scsi);
				spdk_iscsi_task_cpl(&subtask->scsi);
				return 0;
			}

			spdk_iscsi_queue_task(conn, subtask);
		}
		if (task->current_datain_offset == task->scsi.transfer_len) {
			TAILQ_REMOVE(&conn->queued_datain_tasks, task, link);
		}
	}
	return 0;
}

static int spdk_iscsi_op_scsi_read(struct spdk_iscsi_conn *conn,
				   struct spdk_iscsi_task *task)
{
	int32_t remaining_size;

	TAILQ_INIT(&task->subtask_list);
	task->scsi.dxfer_dir = SPDK_SCSI_DIR_FROM_DEV;
	task->parent = NULL;
	task->scsi.offset = 0;
	task->scsi.length = DMIN32(SPDK_BDEV_LARGE_BUF_MAX_SIZE, task->scsi.transfer_len);
	spdk_scsi_task_set_data(&task->scsi, NULL, 0);

	remaining_size = task->scsi.transfer_len - task->scsi.length;
	task->current_datain_offset = 0;

	if (remaining_size == 0) {
		spdk_iscsi_queue_task(conn, task);
		return 0;
	}

	TAILQ_INSERT_TAIL(&conn->queued_datain_tasks, task, link);

	return spdk_iscsi_conn_handle_queued_datain_tasks(conn);
}

static int
spdk_iscsi_op_scsi(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task	*task;
	struct spdk_scsi_dev	*dev;
	uint8_t *cdb;
	uint64_t lun;
	uint32_t task_tag;
	uint32_t transfer_len;
	int F_bit, R_bit, W_bit;
	int lun_i, rc;
	struct iscsi_bhs_scsi_req *reqh;

	if (conn->sess->session_type != SESSION_TYPE_NORMAL) {
		SPDK_ERRLOG("ISCSI_OP_SCSI not allowed in discovery and invalid session\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_scsi_req *)&pdu->bhs;

	F_bit = reqh->final_bit;
	R_bit = reqh->read_bit;
	W_bit = reqh->write_bit;
	lun = from_be64(&reqh->lun);
	task_tag = from_be32(&reqh->itt);
	transfer_len = from_be32(&reqh->expected_data_xfer_len);
	cdb = reqh->cdb;

	SPDK_TRACEDUMP(SPDK_LOG_ISCSI, "CDB", cdb, 16);

	task = spdk_iscsi_task_get(conn, NULL, spdk_iscsi_task_cpl);
	if (!task) {
		SPDK_ERRLOG("Unable to acquire task\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	spdk_iscsi_task_associate_pdu(task, pdu);
	lun_i = spdk_islun2lun(lun);
	task->lun_id = lun_i;
	dev = conn->dev;
	task->scsi.lun = spdk_scsi_dev_get_lun(dev, lun_i);

	if ((R_bit != 0) && (W_bit != 0)) {
		SPDK_ERRLOG("Bidirectional CDB is not supported\n");
		spdk_iscsi_task_put(task);
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	task->scsi.cdb = cdb;
	task->tag = task_tag;
	task->scsi.transfer_len = transfer_len;
	task->scsi.target_port = conn->target_port;
	task->scsi.initiator_port = conn->initiator_port;
	task->parent = NULL;

	if (task->scsi.lun == NULL) {
		spdk_scsi_task_process_null_lun(&task->scsi);
		spdk_iscsi_task_cpl(&task->scsi);
		return 0;
	}

	/* no bi-directional support */
	if (R_bit) {
		return spdk_iscsi_op_scsi_read(conn, task);
	} else if (W_bit) {
		task->scsi.dxfer_dir = SPDK_SCSI_DIR_TO_DEV;

		if ((conn->sess->ErrorRecoveryLevel >= 1) &&
		    (spdk_iscsi_compare_pdu_bhs_within_existed_r2t_tasks(conn, pdu))) {
			spdk_iscsi_task_response(conn, task);
			spdk_iscsi_task_put(task);
			return 0;
		}

		if (pdu->data_segment_len > transfer_len) {
			SPDK_ERRLOG("data segment len(=%d) > task transfer len(=%d)\n",
				    (int)pdu->data_segment_len, transfer_len);
			spdk_iscsi_task_put(task);
			rc = spdk_iscsi_reject(conn, pdu,
					       ISCSI_REASON_PROTOCOL_ERROR);
			if (rc < 0) {
				SPDK_ERRLOG("iscsi_reject() failed\n");
			}
			return rc;
		}

		/* check the ImmediateData and also pdu->data_segment_len */
		if ((!conn->sess->ImmediateData && (pdu->data_segment_len > 0)) ||
		    (pdu->data_segment_len > conn->sess->FirstBurstLength)) {
			spdk_iscsi_task_put(task);
			rc = spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
			if (rc < 0) {
				SPDK_ERRLOG("iscsi_reject() failed\n");
			}
			return rc;
		}

		if (F_bit && pdu->data_segment_len < transfer_len) {
			/* needs R2T */
			rc = spdk_add_transfer_task(conn, task);
			if (rc < 0) {
				SPDK_ERRLOG("add_transfer_task() failed\n");
				spdk_iscsi_task_put(task);
				return SPDK_ISCSI_CONNECTION_FATAL;
			}

			/* Non-immediate writes */
			if (pdu->data_segment_len == 0) {
				return 0;
			} else {
				/* we are doing the first partial write task */
				task->scsi.ref++;
				spdk_scsi_task_set_data(&task->scsi, pdu->data, pdu->data_segment_len);
				task->scsi.length = pdu->data_segment_len;
			}
		}

		if (pdu->data_segment_len == transfer_len) {
			/* we are doing small writes with no R2T */
			spdk_scsi_task_set_data(&task->scsi, pdu->data, transfer_len);
			task->scsi.length = transfer_len;
		}
	} else {
		/* neither R nor W bit set */
		task->scsi.dxfer_dir = SPDK_SCSI_DIR_NONE;
		if (transfer_len > 0) {
			spdk_iscsi_task_put(task);
			SPDK_ERRLOG("Reject scsi cmd with EDTL > 0 but (R | W) == 0\n");
			return spdk_iscsi_reject(conn, pdu, ISCSI_REASON_INVALID_PDU_FIELD);
		}
	}

	spdk_iscsi_queue_task(conn, task);
	return 0;
}

void
spdk_iscsi_task_mgmt_response(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_task *task)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_task_req *reqh;
	struct iscsi_bhs_task_resp *rsph;

	if (task->pdu == NULL) {
		/*
		 * This was an internally generated task management command,
		 *  usually from LUN cleanup when a connection closes.
		 */
		return;
	}

	reqh = (struct iscsi_bhs_task_req *)&task->pdu->bhs;
	/* response PDU */
	rsp_pdu = spdk_get_pdu();
	rsph = (struct iscsi_bhs_task_resp *)&rsp_pdu->bhs;
	rsph->opcode = ISCSI_OP_TASK_RSP;
	rsph->flags |= 0x80; /* bit 0 default to 1 */
	switch (task->scsi.response) {
	case SPDK_SCSI_TASK_MGMT_RESP_COMPLETE:
		rsph->response = ISCSI_TASK_FUNC_RESP_COMPLETE;
		break;
	case SPDK_SCSI_TASK_MGMT_RESP_SUCCESS:
		rsph->response = ISCSI_TASK_FUNC_RESP_COMPLETE;
		break;
	case SPDK_SCSI_TASK_MGMT_RESP_REJECT:
		rsph->response = ISCSI_TASK_FUNC_REJECTED;
		break;
	case SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN:
		rsph->response = ISCSI_TASK_FUNC_RESP_LUN_NOT_EXIST;
		break;
	case SPDK_SCSI_TASK_MGMT_RESP_TARGET_FAILURE:
		rsph->response = ISCSI_TASK_FUNC_REJECTED;
		break;
	case SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED:
		rsph->response = ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED;
		break;
	}
	rsph->itt = reqh->itt;

	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;

	if (reqh->immediate == 0) {
		conn->sess->MaxCmdSN++;
	}

	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);
}

void spdk_iscsi_task_response(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_task *task)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_scsi_resp *rsph;
	uint32_t task_tag;
	uint32_t transfer_len;
	size_t residual_len;
	size_t data_len;
	int O_bit, U_bit;
	int rc;
	struct spdk_iscsi_task *primary;

	primary = spdk_iscsi_task_get_primary(task);

	transfer_len = primary->scsi.transfer_len;
	task_tag = task->tag;

	/* transfer data from logical unit */
	/* (direction is view of initiator side) */
	if (spdk_iscsi_task_is_read(primary)) {
		rc = spdk_iscsi_transfer_in(conn, task);
		if (rc > 0) {
			/* sent status by last DATAIN PDU */
			return;
		}

		if (primary->bytes_completed != primary->scsi.transfer_len) {
			return;
		}
	}

	O_bit = U_bit = 0;
	residual_len = 0;
	data_len = primary->scsi.data_transferred;

	if ((transfer_len != 0) &&
	    (task->scsi.status == SPDK_SCSI_STATUS_GOOD)) {
		if (data_len < transfer_len) {
			/* underflow */
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Underflow %zu/%u\n", data_len, transfer_len);
			residual_len = transfer_len - data_len;
			U_bit = 1;
		} else if (data_len > transfer_len) {
			/* overflow */
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Overflow %zu/%u\n", data_len, transfer_len);
			residual_len = data_len - transfer_len;
			O_bit = 1;
		} else {
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Transfer %u\n", transfer_len);
		}
	}

	/* response PDU */
	rsp_pdu = spdk_get_pdu();
	assert(rsp_pdu != NULL);
	rsph = (struct iscsi_bhs_scsi_resp *)&rsp_pdu->bhs;
	assert(task->scsi.sense_data_len <= sizeof(rsp_pdu->sense.data));
	memcpy(rsp_pdu->sense.data, task->scsi.sense_data, task->scsi.sense_data_len);
	to_be16(&rsp_pdu->sense.length, task->scsi.sense_data_len);
	rsp_pdu->data = (uint8_t *)&rsp_pdu->sense;
	rsp_pdu->data_from_mempool = true;

	/*
	 * we need to hold onto this task/cmd because until the
	 * PDU has been written out
	 */
	rsp_pdu->task = task;
	task->scsi.ref++;

	rsph->opcode = ISCSI_OP_SCSI_RSP;
	rsph->flags |= 0x80; /* bit 0 is default to 1 */

	if (O_bit) {
		rsph->flags |= ISCSI_SCSI_OVERFLOW;
	}

	if (U_bit) {
		rsph->flags |= ISCSI_SCSI_UNDERFLOW;
	}

	rsph->status = task->scsi.status;
	if (task->scsi.sense_data_len) {
		/* SenseLength (2 bytes) + SenseData  */
		DSET24(rsph->data_segment_len, 2 + task->scsi.sense_data_len);
	}
	to_be32(&rsph->itt, task_tag);

	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;

	if (!spdk_iscsi_task_is_immediate(primary)) {
		conn->sess->MaxCmdSN++;
	}

	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	to_be32(&rsph->bi_read_res_cnt, 0);
	to_be32(&rsph->res_cnt, residual_len);

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);
}

static struct spdk_iscsi_task *
spdk_get_transfer_task(struct spdk_iscsi_conn *conn, uint32_t transfer_tag)
{
	int i;

	for (i = 0; i < conn->pending_r2t; i++) {
		if (conn->outstanding_r2t_tasks[i]->ttt == transfer_tag) {
			return (conn->outstanding_r2t_tasks[i]);
		}
	}

	return NULL;
}

static int
spdk_iscsi_op_task(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct iscsi_bhs_task_req *reqh;
	uint64_t lun;
	uint32_t task_tag;
	uint32_t ref_task_tag;
	uint8_t function;
	int lun_i;
	struct spdk_iscsi_task *task;
	struct spdk_scsi_dev *dev;

	if (conn->sess->session_type != SESSION_TYPE_NORMAL) {
		SPDK_ERRLOG("ISCSI_OP_TASK not allowed in discovery and invalid session\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_task_req *)&pdu->bhs;
	function = reqh->flags & ISCSI_TASK_FUNCTION_MASK;
	lun = from_be64(&reqh->lun);
	task_tag = from_be32(&reqh->itt);
	ref_task_tag = from_be32(&reqh->ref_task_tag);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "I=%d, func=%d, ITT=%x, ref TT=%x, LUN=0x%16.16"PRIx64"\n",
		      reqh->immediate, function, task_tag, ref_task_tag, lun);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      conn->StatSN, conn->sess->ExpCmdSN, conn->sess->MaxCmdSN);

	lun_i = spdk_islun2lun(lun);
	dev = conn->dev;

	task = spdk_iscsi_task_get(conn, NULL, spdk_iscsi_task_mgmt_cpl);
	if (!task) {
		SPDK_ERRLOG("Unable to acquire task\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	spdk_iscsi_task_associate_pdu(task, pdu);
	task->scsi.target_port = conn->target_port;
	task->scsi.initiator_port = conn->initiator_port;
	task->tag = task_tag;
	task->scsi.lun = spdk_scsi_dev_get_lun(dev, lun_i);

	switch (function) {
	/* abort task identified by Referenced Task Tag field */
	case ISCSI_TASK_FUNC_ABORT_TASK:
		SPDK_NOTICELOG("ABORT_TASK\n");

		task->scsi.abort_id = ref_task_tag;

		spdk_iscsi_queue_mgmt_task(conn, task, SPDK_SCSI_TASK_FUNC_ABORT_TASK);
		spdk_del_transfer_task(conn, ref_task_tag);

		return SPDK_SUCCESS;

	/* abort all tasks issued via this session on the LUN */
	case ISCSI_TASK_FUNC_ABORT_TASK_SET:
		SPDK_NOTICELOG("ABORT_TASK_SET\n");

		spdk_iscsi_queue_mgmt_task(conn, task, SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET);
		spdk_clear_all_transfer_task(conn, task->scsi.lun);

		return SPDK_SUCCESS;

	case ISCSI_TASK_FUNC_CLEAR_TASK_SET:
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		SPDK_NOTICELOG("CLEAR_TASK_SET (Unsupported)\n");
		break;

	case ISCSI_TASK_FUNC_CLEAR_ACA:
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		SPDK_NOTICELOG("CLEAR_ACA (Unsupported)\n");
		break;

	case ISCSI_TASK_FUNC_LOGICAL_UNIT_RESET:
		SPDK_NOTICELOG("LOGICAL_UNIT_RESET\n");

		spdk_iscsi_queue_mgmt_task(conn, task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
		spdk_clear_all_transfer_task(conn, task->scsi.lun);
		return SPDK_SUCCESS;

	case ISCSI_TASK_FUNC_TARGET_WARM_RESET:
		SPDK_NOTICELOG("TARGET_WARM_RESET (Unsupported)\n");

#if 0
		spdk_iscsi_drop_conns(conn, conn->initiator_name, 1 /* drop all */);
		rc = spdk_iscsi_tgt_node_reset(conn->sess->target, lun);
		if (rc < 0) {
			SPDK_ERRLOG("tgt_node reset failed\n");
		}
#else
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
#endif
		break;

	case ISCSI_TASK_FUNC_TARGET_COLD_RESET:
		SPDK_NOTICELOG("TARGET_COLD_RESET\n");

#if 0
		spdk_iscsi_drop_conns(conn, conn->initiator_name, 1 /* drop all */);

		rc = spdk_iscsi_tgt_node_reset(conn->sess->target, lun);
		if (rc < 0) {
			SPDK_ERRLOG("tgt_node reset failed\n");
		}

		conn->state = ISCSI_CONN_STATE_EXITING;
#else
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
#endif
		break;

	case ISCSI_TASK_FUNC_TASK_REASSIGN:
		SPDK_NOTICELOG("TASK_REASSIGN (Unsupported)\n");
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		break;

	default:
		SPDK_ERRLOG("unsupported function %d\n", function);
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT;
		break;
	}

	spdk_iscsi_task_mgmt_response(conn, task);
	spdk_iscsi_task_put(task);
	return 0;
}

static int
spdk_iscsi_op_nopout(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_nop_out *reqh;
	struct iscsi_bhs_nop_in *rsph;
	uint8_t *data;
	uint64_t lun;
	uint32_t task_tag;
	uint32_t transfer_tag;
	uint32_t CmdSN;
	int I_bit;
	int data_len;

	if (conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		SPDK_ERRLOG("ISCSI_OP_NOPOUT not allowed in discovery session\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_nop_out *)&pdu->bhs;
	I_bit = reqh->immediate;

	data_len = DGET24(reqh->data_segment_len);
	if (data_len > conn->MaxRecvDataSegmentLength) {
		data_len = conn->MaxRecvDataSegmentLength;
	}

	lun = from_be64(&reqh->lun);
	task_tag = from_be32(&reqh->itt);
	transfer_tag = from_be32(&reqh->ttt);
	CmdSN = from_be32(&reqh->cmd_sn);
	pdu->cmd_sn = CmdSN;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "I=%d, ITT=%x, TTT=%x\n",
		      I_bit, task_tag, transfer_tag);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "CmdSN=%u, StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      CmdSN, conn->StatSN, conn->sess->ExpCmdSN,
		      conn->sess->MaxCmdSN);

	if (transfer_tag != 0xFFFFFFFF && transfer_tag != (uint32_t)conn->id) {
		SPDK_ERRLOG("invalid transfer tag 0x%x\n", transfer_tag);
		/*
		 * Technically we should probably fail the connection here, but for now
		 *  just print the error message and continue.
		 */
	}

	/*
	 * We don't actually check to see if this is a response to the NOP-In
	 * that we sent.  Our goal is to just verify that the initiator is
	 * alive and responding to commands, not to verify that it tags
	 * NOP-Outs correctly
	 */
	conn->nop_outstanding = false;

	if (task_tag == 0xffffffffU) {
		if (I_bit == 1) {
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "got NOPOUT ITT=0xffffffff\n");
			return SPDK_SUCCESS;
		} else {
			SPDK_ERRLOG("got NOPOUT ITT=0xffffffff, I=0\n");
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	data = calloc(1, data_len);
	if (!data) {
		SPDK_ERRLOG("calloc() failed for ping data\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	/* response of NOPOUT */
	if (data_len > 0) {
		/* copy ping data */
		memcpy(data, pdu->data, data_len);
	}

	transfer_tag = 0xffffffffU;

	/* response PDU */
	rsp_pdu = spdk_get_pdu();
	if (rsp_pdu == NULL) {
		free(data);
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	rsph = (struct iscsi_bhs_nop_in *)&rsp_pdu->bhs;
	rsp_pdu->data = data;
	rsph->opcode = ISCSI_OP_NOPIN;
	rsph->flags |= 0x80; /* bit 0 default to 1 */
	DSET24(rsph->data_segment_len, data_len);
	to_be64(&rsph->lun, lun);
	to_be32(&rsph->itt, task_tag);
	to_be32(&rsph->ttt, transfer_tag);

	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;

	if (I_bit == 0) {
		conn->sess->MaxCmdSN++;
	}

	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);
	conn->last_nopin = spdk_get_ticks();

	return SPDK_SUCCESS;
}

static int
spdk_add_transfer_task(struct spdk_iscsi_conn *conn,
		       struct spdk_iscsi_task *task)
{
	uint32_t transfer_len;
	size_t max_burst_len;
	size_t segment_len;
	size_t data_len;
	int len;
	int idx;
	int rc;
	int data_out_req;

	transfer_len = task->scsi.transfer_len;
	data_len = spdk_iscsi_task_get_pdu(task)->data_segment_len;
	max_burst_len = conn->sess->MaxBurstLength;
	segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	data_out_req = 1 + (transfer_len - data_len - 1) / segment_len;
	task->data_out_cnt = data_out_req;

	/*
	 * If we already have too many tasks using R2T, then queue this task
	 *  and start sending R2T for it after some of the tasks using R2T/data
	 *  out buffers complete.
	 */
	if (conn->pending_r2t >= DEFAULT_MAXR2T) {
		TAILQ_INSERT_TAIL(&conn->queued_r2t_tasks, task, link);
		return SPDK_SUCCESS;
	}

	conn->data_out_cnt += data_out_req;
	idx = conn->pending_r2t++;

	conn->outstanding_r2t_tasks[idx] = task;
	task->next_expected_r2t_offset = data_len;
	task->current_r2t_length = 0;
	task->R2TSN = 0;
	/* According to RFC3720 10.8.5, 0xffffffff is
	 * reserved for TTT in R2T.
	 */
	if (++conn->ttt == 0xffffffffu) {
		conn->ttt = 0;
	}
	task->ttt = conn->ttt;

	while (data_len != transfer_len) {
		len = DMIN32(max_burst_len, (transfer_len - data_len));
		rc = spdk_iscsi_send_r2t(conn, task, data_len, len,
					 task->ttt, &task->R2TSN);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_send_r2t() failed\n");
			return rc;
		}
		data_len += len;
		task->next_r2t_offset = data_len;
		task->outstanding_r2t++;
		if (conn->sess->MaxOutstandingR2T == task->outstanding_r2t) {
			break;
		}
	}

	TAILQ_INSERT_TAIL(&conn->active_r2t_tasks, task, link);
	return SPDK_SUCCESS;
}

/* If there are additional large writes queued for R2Ts, start them now.
 *  This is called when a large write is just completed or when multiple LUNs
 *  are attached and large write tasks for the specific LUN are cleared.
 */
static void
spdk_start_queued_transfer_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_task *task, *tmp;

	TAILQ_FOREACH_SAFE(task, &conn->queued_r2t_tasks, link, tmp) {
		if (conn->pending_r2t < DEFAULT_MAXR2T) {
			TAILQ_REMOVE(&conn->queued_r2t_tasks, task, link);
			spdk_add_transfer_task(conn, task);
		} else {
			break;
		}
	}
}

void spdk_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t task_tag)
{
	struct spdk_iscsi_task *task;
	int i;

	for (i = 0; i < conn->pending_r2t; i++) {
		if (conn->outstanding_r2t_tasks[i]->tag == task_tag) {
			task = conn->outstanding_r2t_tasks[i];
			conn->data_out_cnt -= task->data_out_cnt;

			conn->pending_r2t--;
			for (; i < conn->pending_r2t; i++) {
				conn->outstanding_r2t_tasks[i] = conn->outstanding_r2t_tasks[i + 1];
			}
			conn->outstanding_r2t_tasks[conn->pending_r2t] = NULL;
			break;
		}
	}

	spdk_start_queued_transfer_tasks(conn);
}

static void
spdk_del_connection_queued_task(struct spdk_iscsi_conn *conn, void *tailq,
				struct spdk_scsi_lun *lun)
{
	struct spdk_iscsi_task *task, *task_tmp;
	/*
	 * Temporary used to index spdk_scsi_task related
	 *  queues of the connection.
	 */
	TAILQ_HEAD(queued_tasks, spdk_iscsi_task) *head;
	head = (struct queued_tasks *)tailq;

	TAILQ_FOREACH_SAFE(task, head, link, task_tmp) {
		if (lun == NULL || lun == task->scsi.lun) {
			TAILQ_REMOVE(head, task, link);
			if (lun != NULL && spdk_scsi_lun_is_removing(lun)) {
				spdk_scsi_task_process_null_lun(&task->scsi);
				spdk_iscsi_task_response(conn, task);
			}
			spdk_iscsi_task_put(task);
		}
	}
}

void spdk_clear_all_transfer_task(struct spdk_iscsi_conn *conn,
				  struct spdk_scsi_lun *lun)
{
	int i, j, pending_r2t;
	struct spdk_iscsi_task *task;

	pending_r2t = conn->pending_r2t;
	for (i = 0; i < pending_r2t; i++) {
		task = conn->outstanding_r2t_tasks[i];
		if (lun == NULL || lun == task->scsi.lun) {
			conn->outstanding_r2t_tasks[i] = NULL;
			task->outstanding_r2t = 0;
			task->next_r2t_offset = 0;
			task->next_expected_r2t_offset = 0;
			conn->data_out_cnt -= task->data_out_cnt;
			conn->pending_r2t--;
		}
	}

	for (i = 0; i < pending_r2t; i++) {
		if (conn->outstanding_r2t_tasks[i] != NULL) {
			continue;
		}
		for (j = i + 1; j < pending_r2t; j++) {
			if (conn->outstanding_r2t_tasks[j] != NULL) {
				conn->outstanding_r2t_tasks[i] = conn->outstanding_r2t_tasks[j];
				conn->outstanding_r2t_tasks[j] = NULL;
				break;
			}
		}
	}

	spdk_del_connection_queued_task(conn, &conn->active_r2t_tasks, lun);
	spdk_del_connection_queued_task(conn, &conn->queued_r2t_tasks, lun);

	spdk_start_queued_transfer_tasks(conn);
}

/* This function is used to handle the r2t snack */
static int
spdk_iscsi_handle_r2t_snack(struct spdk_iscsi_conn *conn,
			    struct spdk_iscsi_task *task,
			    struct spdk_iscsi_pdu *pdu, uint32_t beg_run,
			    uint32_t run_length, int32_t task_tag)
{
	int32_t last_r2tsn;
	int i;

	if (beg_run < task->acked_r2tsn) {
		SPDK_ERRLOG("ITT: 0x%08x, R2T SNACK requests retransmission of"
			    "R2TSN: from 0x%08x to 0x%08x. But it has already"
			    "ack to R2TSN:0x%08x, protocol error.\n",
			    task_tag, beg_run, (beg_run + run_length),
			    (task->acked_r2tsn - 1));
		return spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	if (run_length) {
		if ((beg_run + run_length) > task->R2TSN) {
			SPDK_ERRLOG("ITT: 0x%08x, received R2T SNACK with"
				    "BegRun: 0x%08x, RunLength: 0x%08x, exceeds"
				    "current R2TSN: 0x%08x, protocol error.\n",
				    task_tag, beg_run, run_length,
				    task->R2TSN);

			return spdk_iscsi_reject(conn, pdu,
						 ISCSI_REASON_INVALID_PDU_FIELD);
		}
		last_r2tsn = (beg_run + run_length);
	} else {
		last_r2tsn = task->R2TSN;
	}

	for (i = beg_run; i < last_r2tsn; i++) {
		if (spdk_iscsi_send_r2t_recovery(conn, task, i, false) < 0) {
			SPDK_ERRLOG("The r2t_sn=%d of r2t_task=%p is not sent\n", i, task);
		}
	}
	return 0;
}

/* This function is used to recover the data in packet */
static int
spdk_iscsi_handle_recovery_datain(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_task *task,
				  struct spdk_iscsi_pdu *pdu, uint32_t beg_run,
				  uint32_t run_length, uint32_t task_tag)
{
	struct spdk_iscsi_pdu *old_pdu, *pdu_temp;
	uint32_t i;
	struct iscsi_bhs_data_in *datain_header;
	uint32_t last_statsn;

	task = spdk_iscsi_task_get_primary(task);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "spdk_iscsi_handle_recovery_datain\n");

	if (beg_run < task->acked_data_sn) {
		SPDK_ERRLOG("ITT: 0x%08x, DATA IN SNACK requests retransmission of"
			    "DATASN: from 0x%08x to 0x%08x but already acked to "
			    "DATASN: 0x%08x protocol error\n",
			    task_tag, beg_run,
			    (beg_run + run_length), (task->acked_data_sn - 1));

		return spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	if (run_length == 0) {
		/* as the DataSN begins at 0 */
		run_length = task->datain_datasn + 1;
	}

	if ((beg_run + run_length - 1) > task->datain_datasn) {
		SPDK_ERRLOG("Initiator requests BegRun: 0x%08x, RunLength:"
			    "0x%08x greater than maximum DataSN: 0x%08x.\n",
			    beg_run, run_length, task->datain_datasn);

		return -1;
	} else {
		last_statsn = beg_run + run_length - 1;
	}

	for (i = beg_run; i <= last_statsn; i++) {
		TAILQ_FOREACH_SAFE(old_pdu, &conn->snack_pdu_list, tailq, pdu_temp) {
			if (old_pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN) {
				datain_header = (struct iscsi_bhs_data_in *)&old_pdu->bhs;
				if (from_be32(&datain_header->itt) == task_tag &&
				    from_be32(&datain_header->data_sn) == i) {
					TAILQ_REMOVE(&conn->snack_pdu_list, old_pdu, tailq);
					spdk_iscsi_conn_write_pdu(conn, old_pdu);
					break;
				}
			}
		}
	}
	return 0;
}

/* This function is used to handle the status snack */
static int
spdk_iscsi_handle_status_snack(struct spdk_iscsi_conn *conn,
			       struct spdk_iscsi_pdu *pdu)
{
	uint32_t beg_run;
	uint32_t run_length;
	struct iscsi_bhs_snack_req *reqh;
	uint32_t i;
	uint32_t last_statsn;
	bool found_pdu;
	struct spdk_iscsi_pdu *old_pdu;

	reqh = (struct iscsi_bhs_snack_req *)&pdu->bhs;
	beg_run = from_be32(&reqh->beg_run);
	run_length = from_be32(&reqh->run_len);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "beg_run=%d, run_length=%d, conn->StatSN="
		      "%d, conn->exp_statsn=%d\n", beg_run, run_length,
		      conn->StatSN, conn->exp_statsn);

	if (!beg_run) {
		beg_run = conn->exp_statsn;
	} else if (beg_run < conn->exp_statsn) {
		SPDK_ERRLOG("Got Status SNACK Begrun: 0x%08x, RunLength: 0x%08x "
			    "but already got ExpStatSN: 0x%08x on CID:%hu.\n",
			    beg_run, run_length, conn->StatSN, conn->cid);

		return spdk_iscsi_reject(conn, pdu, ISCSI_REASON_INVALID_PDU_FIELD);
	}

	last_statsn = (!run_length) ? conn->StatSN : (beg_run + run_length);

	for (i = beg_run; i < last_statsn; i++) {
		found_pdu = false;
		TAILQ_FOREACH(old_pdu, &conn->snack_pdu_list, tailq) {
			if (from_be32(&old_pdu->bhs.stat_sn) == i) {
				found_pdu = true;
				break;
			}
		}

		if (!found_pdu) {
			SPDK_ERRLOG("Unable to find StatSN: 0x%08x. For a Status"
				    "SNACK, assuming this is a proactive SNACK "
				    "for an untransmitted StatSN, ignoring.\n",
				    beg_run);
		} else {
			TAILQ_REMOVE(&conn->snack_pdu_list, old_pdu, tailq);
			spdk_iscsi_conn_write_pdu(conn, old_pdu);
		}
	}

	return 0;
}

/* This function is used to handle the data ack snack */
static int
spdk_iscsi_handle_data_ack(struct spdk_iscsi_conn *conn,
			   struct spdk_iscsi_pdu *pdu)
{
	uint32_t transfer_tag;
	uint32_t beg_run;
	uint32_t run_length;
	struct spdk_iscsi_pdu *old_pdu;
	uint32_t old_datasn;
	int rc;
	struct iscsi_bhs_snack_req *reqh;
	struct spdk_iscsi_task *task;
	struct iscsi_bhs_data_in *datain_header;
	struct spdk_iscsi_task *primary;

	reqh = (struct iscsi_bhs_snack_req *)&pdu->bhs;
	transfer_tag = from_be32(&reqh->ttt);
	beg_run = from_be32(&reqh->beg_run);
	run_length = from_be32(&reqh->run_len);
	task = NULL;
	datain_header = NULL;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "beg_run=%d,transfer_tag=%d,run_len=%d\n",
		      beg_run, transfer_tag, run_length);

	task = spdk_get_scsi_task_from_ttt(conn, transfer_tag);
	if (!task) {
		SPDK_ERRLOG("Data ACK SNACK for TTT: 0x%08x is invalid.\n",
			    transfer_tag);
		goto reject_return;
	}

	primary = spdk_iscsi_task_get_primary(task);
	if ((run_length != 0) || (beg_run < primary->acked_data_sn)) {
		SPDK_ERRLOG("TTT: 0x%08x Data ACK SNACK BegRUN: %d is less than "
			    "the next expected acked DataSN: %d\n",
			    transfer_tag, beg_run, primary->acked_data_sn);
		goto reject_return;
	}

	primary->acked_data_sn = beg_run;

	/* To free the pdu */
	TAILQ_FOREACH(old_pdu, &conn->snack_pdu_list, tailq) {
		if (old_pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN) {
			datain_header = (struct iscsi_bhs_data_in *) &old_pdu->bhs;
			old_datasn = from_be32(&datain_header->data_sn);
			if ((from_be32(&datain_header->ttt) == transfer_tag) &&
			    (old_datasn == beg_run - 1)) {
				TAILQ_REMOVE(&conn->snack_pdu_list, old_pdu, tailq);
				if (old_pdu->task) {
					spdk_iscsi_task_put(old_pdu->task);
				}
				spdk_put_pdu(old_pdu);
				break;
			}
		}
	}

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Received Data ACK SNACK for TTT: 0x%08x,"
		      " updated acked DataSN to 0x%08x.\n", transfer_tag,
		      (task->acked_data_sn - 1));

	return 0;

reject_return:
	rc = spdk_iscsi_reject(conn, pdu, ISCSI_REASON_INVALID_SNACK);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_reject() failed\n");
		return -1;
	}

	return 0;
}

/* This function is used to remove the r2t pdu from snack_pdu_list by < task, r2t_sn> info */
static struct spdk_iscsi_pdu *
spdk_iscsi_remove_r2t_pdu_from_snack_list(struct spdk_iscsi_conn *conn,
		struct spdk_iscsi_task *task,
		uint32_t r2t_sn)
{
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_r2t *r2t_header;

	TAILQ_FOREACH(pdu, &conn->snack_pdu_list, tailq) {
		if (pdu->bhs.opcode == ISCSI_OP_R2T) {
			r2t_header = (struct iscsi_bhs_r2t *)&pdu->bhs;
			if (pdu->task == task &&
			    from_be32(&r2t_header->r2t_sn) == r2t_sn) {
				TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
				return pdu;
			}
		}
	}

	return NULL;
}

/* This function is used re-send the r2t packet */
static int
spdk_iscsi_send_r2t_recovery(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_task *task, uint32_t r2t_sn,
			     bool send_new_r2tsn)
{
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_r2t *rsph;
	uint32_t transfer_len;
	uint32_t len;
	int rc;

	/* remove the r2t pdu from the snack_list */
	pdu = spdk_iscsi_remove_r2t_pdu_from_snack_list(conn, task, r2t_sn);
	if (!pdu) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "No pdu is found\n");
		return -1;
	}

	/* flag
	 * false: only need to re-send the old r2t with changing statsn
	 * true: we send a r2t with new r2tsn
	 */
	if (!send_new_r2tsn) {
		to_be32(&pdu->bhs.stat_sn, conn->StatSN);
		spdk_iscsi_conn_write_pdu(conn, pdu);
	} else {
		rsph = (struct iscsi_bhs_r2t *)&pdu->bhs;
		transfer_len = from_be32(&rsph->desired_xfer_len);

		/* still need to increase the acked r2tsn */
		task->acked_r2tsn++;
		len = DMIN32(conn->sess->MaxBurstLength, (transfer_len -
				task->next_expected_r2t_offset));

		/* remove the old_r2t_pdu */
		if (pdu->task) {
			spdk_iscsi_task_put(pdu->task);
		}
		spdk_put_pdu(pdu);

		/* re-send a new r2t pdu */
		rc = spdk_iscsi_send_r2t(conn, task, task->next_expected_r2t_offset,
					 len, task->ttt, &task->R2TSN);
		if (rc < 0) {
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	return 0;
}

/* This function is used to handle the snack request from the initiator */
static int
spdk_iscsi_op_snack(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct iscsi_bhs_snack_req *reqh;
	struct spdk_iscsi_task *task;
	int type;
	uint32_t task_tag;
	uint32_t beg_run;
	uint32_t run_length;
	int rc;

	if (conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		SPDK_ERRLOG("ISCSI_OP_SNACK not allowed in  discovery session\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_snack_req *)&pdu->bhs;
	if (!conn->sess->ErrorRecoveryLevel) {
		SPDK_ERRLOG("Got a SNACK request in ErrorRecoveryLevel=0\n");
		rc = spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_reject() failed\n");
			return -1;
		}
		return rc;
	}

	type = reqh->flags & ISCSI_FLAG_SNACK_TYPE_MASK;
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "The value of type is %d\n", type);

	switch (type) {
	case 0:
		reqh = (struct iscsi_bhs_snack_req *)&pdu->bhs;
		task_tag = from_be32(&reqh->itt);
		beg_run = from_be32(&reqh->beg_run);
		run_length = from_be32(&reqh->run_len);

		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "beg_run=%d, run_length=%d, "
			      "task_tag=%x, transfer_tag=%u\n", beg_run,
			      run_length, task_tag, from_be32(&reqh->ttt));

		task = spdk_get_scsi_task_from_itt(conn, task_tag,
						   ISCSI_OP_SCSI_DATAIN);
		if (task) {
			return spdk_iscsi_handle_recovery_datain(conn, task, pdu,
					beg_run, run_length, task_tag);
		}
		task = spdk_get_scsi_task_from_itt(conn, task_tag, ISCSI_OP_R2T);
		if (task) {
			return spdk_iscsi_handle_r2t_snack(conn, task, pdu, beg_run,
							   run_length, task_tag);
		}
		SPDK_ERRLOG("It is Neither datain nor r2t recovery request\n");
		rc = -1;
		break;
	case ISCSI_FLAG_SNACK_TYPE_STATUS:
		rc = spdk_iscsi_handle_status_snack(conn, pdu);
		break;
	case ISCSI_FLAG_SNACK_TYPE_DATA_ACK:
		rc = spdk_iscsi_handle_data_ack(conn, pdu);
		break;
	case ISCSI_FLAG_SNACK_TYPE_RDATA:
		SPDK_ERRLOG("R-Data SNACK is Not Supported int spdk\n");
		rc = spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		break;
	default:
		SPDK_ERRLOG("Unknown SNACK type %d, protocol error\n", type);
		rc = spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		break;
	}

	return rc;
}

/* This function is used to refree the pdu when it is acknowledged */
static void
spdk_remove_acked_pdu(struct spdk_iscsi_conn *conn,
		      uint32_t ExpStatSN)
{
	struct spdk_iscsi_pdu *pdu, *pdu_temp;
	uint32_t stat_sn;

	conn->exp_statsn = DMIN32(ExpStatSN, conn->StatSN);
	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, pdu_temp) {
		stat_sn = from_be32(&pdu->bhs.stat_sn);
		if (SN32_LT(stat_sn, conn->exp_statsn)) {
			TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
			spdk_iscsi_conn_free_pdu(conn, pdu);
		}
	}
}

static int spdk_iscsi_op_data(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task	*task, *subtask;
	struct iscsi_bhs_data_out *reqh;
	struct spdk_scsi_lun	*lun_dev;
	uint32_t transfer_tag;
	uint32_t task_tag;
	uint32_t transfer_len;
	uint32_t DataSN;
	uint32_t buffer_offset;
	uint32_t len;
	int F_bit;
	int rc;
	int reject_reason = ISCSI_REASON_INVALID_PDU_FIELD;

	if (conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		SPDK_ERRLOG("ISCSI_OP_SCSI_DATAOUT not allowed in discovery session\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_data_out *)&pdu->bhs;
	F_bit = !!(reqh->flags & ISCSI_FLAG_FINAL);
	transfer_tag = from_be32(&reqh->ttt);
	task_tag = from_be32(&reqh->itt);
	DataSN = from_be32(&reqh->data_sn);
	buffer_offset = from_be32(&reqh->buffer_offset);

	task = spdk_get_transfer_task(conn, transfer_tag);
	if (task == NULL) {
		SPDK_ERRLOG("Not found task for transfer_tag=%x\n", transfer_tag);
		goto reject_return;
	}

	lun_dev = spdk_scsi_dev_get_lun(conn->dev, task->lun_id);

	if (pdu->data_segment_len > task->desired_data_transfer_length) {
		SPDK_ERRLOG("the dataout pdu data length is larger than the value sent by R2T PDU\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	if (task->tag != task_tag) {
		SPDK_ERRLOG("The r2t task tag is %u, and the dataout task tag is %u\n",
			    task->tag, task_tag);
		goto reject_return;
	}

	if (DataSN != task->r2t_datasn) {
		SPDK_ERRLOG("DataSN(%u) exp=%d error\n", DataSN, task->r2t_datasn);
		if (conn->sess->ErrorRecoveryLevel >= 1) {
			goto send_r2t_recovery_return;
		} else {
			reject_reason = ISCSI_REASON_PROTOCOL_ERROR;
			goto reject_return;
		}
	}

	if (buffer_offset != task->next_expected_r2t_offset) {
		SPDK_ERRLOG("offset(%u) error\n", buffer_offset);
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	transfer_len = task->scsi.transfer_len;
	task->current_r2t_length += pdu->data_segment_len;
	task->next_expected_r2t_offset += pdu->data_segment_len;
	task->r2t_datasn++;

	if (task->current_r2t_length > conn->sess->MaxBurstLength) {
		SPDK_ERRLOG("R2T burst(%u) > MaxBurstLength(%u)\n",
			    task->current_r2t_length,
			    conn->sess->MaxBurstLength);
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	if (F_bit) {
		/*
		 * This R2T burst is done.  Clear the length before we
		 *  receive a PDU for the next R2T burst.
		 */
		task->current_r2t_length = 0;
	}

	subtask = spdk_iscsi_task_get(conn, task, spdk_iscsi_task_cpl);
	if (subtask == NULL) {
		SPDK_ERRLOG("Unable to acquire subtask\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	subtask->scsi.offset = buffer_offset;
	subtask->scsi.length = pdu->data_segment_len;
	spdk_scsi_task_set_data(&subtask->scsi, pdu->data, pdu->data_segment_len);
	spdk_iscsi_task_associate_pdu(subtask, pdu);

	if (task->next_expected_r2t_offset == transfer_len) {
		task->acked_r2tsn++;
	} else if (F_bit && (task->next_r2t_offset < transfer_len)) {
		task->acked_r2tsn++;
		len = DMIN32(conn->sess->MaxBurstLength, (transfer_len -
				task->next_r2t_offset));
		rc = spdk_iscsi_send_r2t(conn, task, task->next_r2t_offset, len,
					 task->ttt, &task->R2TSN);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_send_r2t() failed\n");
		}
		task->next_r2t_offset += len;
	}

	if (lun_dev == NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "LUN %d is removed, complete the task immediately\n",
			      task->lun_id);
		subtask->scsi.transfer_len = subtask->scsi.length;
		spdk_scsi_task_process_null_lun(&subtask->scsi);
		spdk_iscsi_task_cpl(&subtask->scsi);
		return 0;
	}

	spdk_iscsi_queue_task(conn, subtask);
	return 0;

send_r2t_recovery_return:
	rc = spdk_iscsi_send_r2t_recovery(conn, task, task->acked_r2tsn, true);
	if (rc == 0) {
		return 0;
	}

reject_return:
	rc = spdk_iscsi_reject(conn, pdu, reject_reason);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_reject() failed\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	return SPDK_SUCCESS;
}

static int
spdk_iscsi_send_r2t(struct spdk_iscsi_conn *conn,
		    struct spdk_iscsi_task *task, int offset,
		    int len, uint32_t transfer_tag, uint32_t *R2TSN)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_r2t *rsph;

	/* R2T PDU */
	rsp_pdu = spdk_get_pdu();
	if (rsp_pdu == NULL) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	rsph = (struct iscsi_bhs_r2t *)&rsp_pdu->bhs;
	rsp_pdu->data = NULL;
	rsph->opcode = ISCSI_OP_R2T;
	rsph->flags |= 0x80; /* bit 0 is default to 1 */
	to_be64(&rsph->lun, task->lun_id);
	to_be32(&rsph->itt, task->tag);
	to_be32(&rsph->ttt, transfer_tag);

	to_be32(&rsph->stat_sn, conn->StatSN);
	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	to_be32(&rsph->r2t_sn, *R2TSN);
	*R2TSN += 1;

	task->r2t_datasn = 0; /* next expected datasn to ack */

	to_be32(&rsph->buffer_offset, (uint32_t)offset);
	to_be32(&rsph->desired_xfer_len, (uint32_t)len);
	task->desired_data_transfer_length = (size_t)len;

	/* we need to hold onto this task/cmd because until the PDU has been
	 * written out */
	rsp_pdu->task = task;
	task->scsi.ref++;

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);

	return SPDK_SUCCESS;
}

void spdk_iscsi_send_nopin(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_nop_in	*rsp;

	/* Only send nopin if we have logged in and are in a normal session. */
	if (conn->sess == NULL ||
	    !conn->full_feature ||
	    !spdk_iscsi_param_eq_val(conn->sess->params, "SessionType", "Normal")) {
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "send NOPIN isid=%"PRIx64", tsih=%u, cid=%u\n",
		      conn->sess->isid, conn->sess->tsih, conn->cid);
	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      conn->StatSN, conn->sess->ExpCmdSN,
		      conn->sess->MaxCmdSN);

	rsp_pdu = spdk_get_pdu();
	rsp = (struct iscsi_bhs_nop_in *) &rsp_pdu->bhs;
	rsp_pdu->data = NULL;

	/*
	 * spdk_get_pdu() memset's the PDU for us, so only fill out the needed
	 *  fields.
	 */
	rsp->opcode = ISCSI_OP_NOPIN;
	rsp->flags = 0x80;
	/*
	 * Technically the to_be32() is not needed here, since
	 *  to_be32(0xFFFFFFFU) returns 0xFFFFFFFFU.
	 */
	to_be32(&rsp->itt, 0xFFFFFFFFU);
	to_be32(&rsp->ttt, conn->id);
	to_be32(&rsp->stat_sn, conn->StatSN);
	to_be32(&rsp->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsp->max_cmd_sn, conn->sess->MaxCmdSN);

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);
	conn->last_nopin = spdk_get_ticks();
	conn->nop_outstanding = true;
}

static void
spdk_init_login_reject_response(struct spdk_iscsi_pdu *pdu, struct spdk_iscsi_pdu *rsp_pdu)
{
	struct iscsi_bhs_login_rsp *rsph;

	memset(rsp_pdu, 0, sizeof(struct spdk_iscsi_pdu));
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	rsph->version_max = ISCSI_VERSION;
	rsph->version_act = ISCSI_VERSION;
	rsph->opcode = ISCSI_OP_LOGIN_RSP;
	rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
	rsph->status_detail = ISCSI_LOGIN_INVALID_LOGIN_REQUEST;
	rsph->itt = pdu->bhs.itt;
}

int
spdk_iscsi_execute(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int opcode;
	int rc;
	struct spdk_iscsi_pdu *rsp_pdu = NULL;
	uint32_t ExpStatSN;
	uint32_t QCmdSN;
	int I_bit;
	struct spdk_iscsi_sess *sess;
	struct iscsi_bhs_scsi_req *reqh;

	if (pdu == NULL) {
		return -1;
	}

	opcode = pdu->bhs.opcode;
	reqh = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
	pdu->cmd_sn = from_be32(&reqh->cmd_sn);

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "opcode %x\n", opcode);

	if (opcode == ISCSI_OP_LOGIN) {
		rc = spdk_iscsi_op_login(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_op_login() failed\n");
		}
		return rc;
	}

	/* connection in login phase but receive non-login opcode
	 * return response code 0x020b to initiator.
	 * */
	if (!conn->full_feature && conn->state == ISCSI_CONN_STATE_RUNNING) {
		rsp_pdu = spdk_get_pdu();
		if (rsp_pdu == NULL) {
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
		spdk_init_login_reject_response(pdu, rsp_pdu);
		spdk_iscsi_conn_write_pdu(conn, rsp_pdu);
		SPDK_ERRLOG("Received opcode %d in login phase\n", opcode);
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	} else if (conn->state == ISCSI_CONN_STATE_INVALID) {
		SPDK_ERRLOG("before Full Feature\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	sess = conn->sess;
	if (!sess) {
		SPDK_ERRLOG("Connection has no associated session!\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	I_bit = reqh->immediate;
	if (I_bit == 0) {
		if (SN32_LT(pdu->cmd_sn, sess->ExpCmdSN) ||
		    SN32_GT(pdu->cmd_sn, sess->MaxCmdSN)) {
			if (sess->session_type == SESSION_TYPE_NORMAL &&
			    opcode != ISCSI_OP_SCSI_DATAOUT) {
				SPDK_ERRLOG("CmdSN(%u) ignore (ExpCmdSN=%u, MaxCmdSN=%u)\n",
					    pdu->cmd_sn, sess->ExpCmdSN, sess->MaxCmdSN);

				if (sess->ErrorRecoveryLevel >= 1) {
					SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Skip the error in ERL 1 and 2\n");
				} else {
					return SPDK_PDU_FATAL;
				}
			}
		}
	} else if (pdu->cmd_sn != sess->ExpCmdSN) {
		SPDK_ERRLOG("CmdSN(%u) error ExpCmdSN=%u\n", pdu->cmd_sn, sess->ExpCmdSN);

		if (sess->ErrorRecoveryLevel >= 1) {
			SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Skip the error in ERL 1 and 2\n");
		} else if (opcode != ISCSI_OP_NOPOUT) {
			/*
			 * The Linux initiator does not send valid CmdSNs for
			 *  nopout under heavy load, so do not close the
			 *  connection in that case.
			 */
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	ExpStatSN = from_be32(&reqh->exp_stat_sn);
	if (SN32_GT(ExpStatSN, conn->StatSN)) {
		SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "StatSN(%u) advanced\n", ExpStatSN);
		ExpStatSN = conn->StatSN;
	}

	if (sess->ErrorRecoveryLevel >= 1) {
		spdk_remove_acked_pdu(conn, ExpStatSN);
	}

	if (opcode == ISCSI_OP_NOPOUT || opcode == ISCSI_OP_SCSI) {
		QCmdSN = sess->MaxCmdSN - sess->ExpCmdSN + 1;
		QCmdSN += sess->queue_depth;
		if (SN32_LT(ExpStatSN + QCmdSN, conn->StatSN)) {
			SPDK_ERRLOG("StatSN(%u/%u) QCmdSN(%u) error\n",
				    ExpStatSN, conn->StatSN, QCmdSN);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	if (!I_bit && opcode != ISCSI_OP_SCSI_DATAOUT) {
		sess->ExpCmdSN++;
	}

	switch (opcode) {
	case ISCSI_OP_NOPOUT:
		rc = spdk_iscsi_op_nopout(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_op_nopout() failed\n");
			return rc;
		}
		break;

	case ISCSI_OP_SCSI:
		rc = spdk_iscsi_op_scsi(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_op_scsi() failed\n");
			return rc;
		}
		break;
	case ISCSI_OP_TASK:
		rc = spdk_iscsi_op_task(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_op_task() failed\n");
			return rc;
		}
		break;

	case ISCSI_OP_TEXT:
		rc = spdk_iscsi_op_text(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_op_text() failed\n");
			return rc;
		}
		break;

	case ISCSI_OP_LOGOUT:
		rc = spdk_iscsi_op_logout(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_op_logout() failed\n");
			return rc;
		}
		break;

	case ISCSI_OP_SCSI_DATAOUT:
		rc = spdk_iscsi_op_data(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_op_data() failed\n");
			return rc;
		}
		break;

	case ISCSI_OP_SNACK:
		rc = spdk_iscsi_op_snack(conn, pdu);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_op_snack() failed\n");
			return rc;
		}
		break;

	default:
		SPDK_ERRLOG("unsupported opcode %x\n", opcode);
		rc = spdk_iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		if (rc < 0) {
			SPDK_ERRLOG("spdk_iscsi_reject() failed\n");
			return rc;
		}
		break;
	}

	return 0;
}

void spdk_free_sess(struct spdk_iscsi_sess *sess)
{
	if (sess == NULL) {
		return;
	}

	sess->tag = 0;
	sess->target = NULL;
	sess->session_type = SESSION_TYPE_INVALID;
	spdk_iscsi_param_free(sess->params);
	free(sess->conns);
	spdk_scsi_port_free(&sess->initiator_port);
	spdk_mempool_put(g_spdk_iscsi.session_pool, (void *)sess);
}

static int
spdk_create_iscsi_sess(struct spdk_iscsi_conn *conn,
		       struct spdk_iscsi_tgt_node *target,
		       enum session_type session_type)
{
	struct spdk_iscsi_sess *sess;
	int rc;

	sess = spdk_mempool_get(g_spdk_iscsi.session_pool);
	if (!sess) {
		SPDK_ERRLOG("Unable to get session object\n");
		SPDK_ERRLOG("MaxSessions set to %d\n", g_spdk_iscsi.MaxSessions);
		return -ENOMEM;
	}

	/* configuration values */
	pthread_mutex_lock(&g_spdk_iscsi.mutex);

	sess->MaxConnections = g_spdk_iscsi.MaxConnectionsPerSession;
	sess->MaxOutstandingR2T = DEFAULT_MAXOUTSTANDINGR2T;

	sess->DefaultTime2Wait = g_spdk_iscsi.DefaultTime2Wait;
	sess->DefaultTime2Retain = g_spdk_iscsi.DefaultTime2Retain;
	sess->FirstBurstLength = g_spdk_iscsi.FirstBurstLength;
	sess->MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;
	sess->InitialR2T = DEFAULT_INITIALR2T;
	sess->ImmediateData = g_spdk_iscsi.ImmediateData;
	sess->DataPDUInOrder = DEFAULT_DATAPDUINORDER;
	sess->DataSequenceInOrder = DEFAULT_DATASEQUENCEINORDER;
	sess->ErrorRecoveryLevel = g_spdk_iscsi.ErrorRecoveryLevel;

	pthread_mutex_unlock(&g_spdk_iscsi.mutex);

	sess->tag = conn->portal->group->tag;

	sess->conns = calloc(sess->MaxConnections, sizeof(*sess->conns));
	if (!sess->conns) {
		SPDK_ERRLOG("calloc() failed for connection array\n");
		return -ENOMEM;
	}

	sess->connections = 0;

	sess->conns[sess->connections] = conn;
	sess->connections++;

	sess->params = NULL;
	sess->target = NULL;
	sess->isid = 0;
	sess->session_type = session_type;
	sess->current_text_itt = 0xffffffffU;

	/* set default params */
	rc = spdk_iscsi_sess_params_init(&sess->params);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_sess_params_init() failed\n");
		goto error_return;
	}
	/* replace with config value */
	rc = spdk_iscsi_param_set_int(sess->params, "MaxConnections",
				      sess->MaxConnections);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set_int(sess->params, "MaxOutstandingR2T",
				      sess->MaxOutstandingR2T);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set_int(sess->params, "DefaultTime2Wait",
				      sess->DefaultTime2Wait);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set_int(sess->params, "DefaultTime2Retain",
				      sess->DefaultTime2Retain);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set_int(sess->params, "FirstBurstLength",
				      sess->FirstBurstLength);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set_int(sess->params, "MaxBurstLength",
				      sess->MaxBurstLength);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set(sess->params, "InitialR2T",
				  sess->InitialR2T ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set(sess->params, "ImmediateData",
				  sess->ImmediateData ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set(sess->params, "DataPDUInOrder",
				  sess->DataPDUInOrder ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set(sess->params, "DataSequenceInOrder",
				  sess->DataSequenceInOrder ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = spdk_iscsi_param_set_int(sess->params, "ErrorRecoveryLevel",
				      sess->ErrorRecoveryLevel);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	/* realloc buffer */
	rc = spdk_iscsi_param_set_int(conn->params, "MaxRecvDataSegmentLength",
				      conn->MaxRecvDataSegmentLength);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	/* sess for first connection of session */
	conn->sess = sess;
	return 0;

error_return:
	spdk_free_sess(sess);
	conn->sess = NULL;
	return -1;
}

static struct spdk_iscsi_sess *
spdk_get_iscsi_sess_by_tsih(uint16_t tsih)
{
	struct spdk_iscsi_sess *session;

	if (tsih == 0 || tsih > g_spdk_iscsi.MaxSessions) {
		return NULL;
	}

	session = g_spdk_iscsi.session[tsih - 1];
	assert(tsih == session->tsih);

	return session;
}

static int
spdk_append_iscsi_sess(struct spdk_iscsi_conn *conn,
		       const char *initiator_port_name, uint16_t tsih, uint16_t cid)
{
	struct spdk_iscsi_sess *sess;

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "append session: init port name=%s, tsih=%u, cid=%u\n",
		      initiator_port_name, tsih, cid);

	sess = spdk_get_iscsi_sess_by_tsih(tsih);
	if (sess == NULL) {
		SPDK_ERRLOG("spdk_get_iscsi_sess_by_tsih failed\n");
		return -1;
	}
	if ((conn->portal->group->tag != sess->tag) ||
	    (strcasecmp(initiator_port_name, spdk_scsi_port_get_name(sess->initiator_port)) != 0) ||
	    (conn->target != sess->target)) {
		/* no match */
		SPDK_ERRLOG("no MCS session for init port name=%s, tsih=%d, cid=%d\n",
			    initiator_port_name, tsih, cid);
		return -1;
	}

	if (sess->connections >= sess->MaxConnections) {
		/* no slot for connection */
		SPDK_ERRLOG("too many connections for init port name=%s, tsih=%d, cid=%d\n",
			    initiator_port_name, tsih, cid);
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_ISCSI, "Connections (tsih %d): %d\n", sess->tsih, sess->connections);
	conn->sess = sess;

	/*
	 * TODO: need a mutex or other sync mechanism to protect the session's
	 *  connection list.
	 */
	sess->conns[sess->connections] = conn;
	sess->connections++;

	return 0;
}

bool spdk_iscsi_is_deferred_free_pdu(struct spdk_iscsi_pdu *pdu)
{
	if (pdu == NULL) {
		return false;
	}

	if (pdu->bhs.opcode == ISCSI_OP_R2T ||
	    pdu->bhs.opcode == ISCSI_OP_SCSI_DATAIN) {
		return true;
	}

	return false;
}
