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

#include "spdk/base64.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/trace.h"
#include "spdk/sock.h"
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

#include "spdk/log.h"

#define MAX_TMPBUF 1024

#define SPDK_CRC32C_INITIAL    0xffffffffUL
#define SPDK_CRC32C_XOR        0xffffffffUL

#ifdef __FreeBSD__
#define HAVE_SRANDOMDEV 1
#define HAVE_ARC4RANDOM 1
#endif

struct spdk_iscsi_globals g_iscsi = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.portal_head = TAILQ_HEAD_INITIALIZER(g_iscsi.portal_head),
	.pg_head = TAILQ_HEAD_INITIALIZER(g_iscsi.pg_head),
	.ig_head = TAILQ_HEAD_INITIALIZER(g_iscsi.ig_head),
	.target_head = TAILQ_HEAD_INITIALIZER(g_iscsi.target_head),
	.auth_group_head = TAILQ_HEAD_INITIALIZER(g_iscsi.auth_group_head),
	.poll_group_head = TAILQ_HEAD_INITIALIZER(g_iscsi.poll_group_head),
};

#define MATCH_DIGEST_WORD(BUF, CRC32C) \
	(    ((((uint32_t) *((uint8_t *)(BUF)+0)) << 0)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+1)) << 8)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+2)) << 16)	\
	    | (((uint32_t) *((uint8_t *)(BUF)+3)) << 24))	\
	    == (CRC32C))

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
static int g_arc4random_initialized = 0;

static uint32_t
arc4random(void)
{
	uint32_t r;
	uint32_t r1, r2;

	if (!g_arc4random_initialized) {
		srandomdev();
		g_arc4random_initialized = 1;
	}
	r1 = (uint32_t)(random() & 0xffff);
	r2 = (uint32_t)(random() & 0xffff);
	r = (r1 << 16) | r2;
	return r;
}
#endif /* HAVE_ARC4RANDOM */

static void
gen_random(uint8_t *buf, size_t len)
{
	uint32_t r;
	size_t idx;

	for (idx = 0; idx < len; idx++) {
		r = arc4random();
		buf[idx] = (uint8_t) r;
	}
}

static uint64_t
iscsi_get_isid(const uint8_t isid[6])
{
	return (uint64_t)isid[0] << 40 |
	       (uint64_t)isid[1] << 32 |
	       (uint64_t)isid[2] << 24 |
	       (uint64_t)isid[3] << 16 |
	       (uint64_t)isid[4] << 8 |
	       (uint64_t)isid[5];
}

static int
bin2hex(char *buf, size_t len, const uint8_t *data, size_t data_len)
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
hex2bin(uint8_t *data, size_t data_len, const char *str)
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
iscsi_reject(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
	     int reason)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_reject *rsph;
	uint8_t *data;
	int total_ahs_len;
	int data_len;
	int alloc_len;

	pdu->is_rejected = true;

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

	SPDK_DEBUGLOG(iscsi, "Reject PDU reason=%d\n", reason);

	if (conn->sess != NULL) {
		SPDK_DEBUGLOG(iscsi,
			      "StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
			      conn->StatSN, conn->sess->ExpCmdSN,
			      conn->sess->MaxCmdSN);
	} else {
		SPDK_DEBUGLOG(iscsi, "StatSN=%u\n", conn->StatSN);
	}

	memcpy(data, &pdu->bhs, ISCSI_BHS_LEN);
	data_len += ISCSI_BHS_LEN;

	if (total_ahs_len != 0) {
		total_ahs_len = spdk_min((4 * total_ahs_len), ISCSI_AHS_LEN);
		memcpy(data + data_len, pdu->ahs, total_ahs_len);
		data_len += total_ahs_len;
	}

	if (conn->header_digest) {
		memcpy(data + data_len, pdu->header_digest, ISCSI_DIGEST_LEN);
		data_len += ISCSI_DIGEST_LEN;
	}

	rsp_pdu = iscsi_get_pdu(conn);
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

	SPDK_LOGDUMP(iscsi, "PDU", (void *)&rsp_pdu->bhs, ISCSI_BHS_LEN);

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);

	return 0;
}

uint32_t
iscsi_pdu_calc_header_digest(struct spdk_iscsi_pdu *pdu)
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

uint32_t
iscsi_pdu_calc_data_digest(struct spdk_iscsi_pdu *pdu)
{
	uint32_t data_len = DGET24(pdu->bhs.data_segment_len);
	uint32_t crc32c;
	uint32_t mod;
	struct iovec iov;
	uint32_t num_blocks;

	crc32c = SPDK_CRC32C_INITIAL;
	if (spdk_likely(!pdu->dif_insert_or_strip)) {
		crc32c = spdk_crc32c_update(pdu->data, data_len, crc32c);
	} else {
		iov.iov_base = pdu->data_buf;
		iov.iov_len = pdu->data_buf_len;
		num_blocks = pdu->data_buf_len / pdu->dif_ctx.block_size;

		spdk_dif_update_crc32c(&iov, 1, num_blocks, &crc32c, &pdu->dif_ctx);
	}

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

static int
iscsi_conn_read_data_segment(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_pdu *pdu,
			     uint32_t segment_len)
{
	struct iovec buf_iov, iovs[32];
	int rc, _rc;

	if (spdk_likely(!pdu->dif_insert_or_strip)) {
		return iscsi_conn_read_data(conn,
					    segment_len - pdu->data_valid_bytes,
					    pdu->data_buf + pdu->data_valid_bytes);
	} else {
		buf_iov.iov_base = pdu->data_buf;
		buf_iov.iov_len = pdu->data_buf_len;
		rc = spdk_dif_set_md_interleave_iovs(iovs, 32, &buf_iov, 1,
						     pdu->data_valid_bytes,
						     segment_len - pdu->data_valid_bytes, NULL,
						     &pdu->dif_ctx);
		if (rc > 0) {
			rc = iscsi_conn_readv_data(conn, iovs, rc);
			if (rc > 0) {
				_rc = spdk_dif_generate_stream(&buf_iov, 1,
							       pdu->data_valid_bytes, rc,
							       &pdu->dif_ctx);
				if (_rc != 0) {
					SPDK_ERRLOG("DIF generate failed\n");
					rc = _rc;
				}
			}
		} else {
			SPDK_ERRLOG("Setup iovs for interleaved metadata failed\n");
		}
		return rc;
	}
}

struct _iscsi_sgl {
	struct iovec	*iov;
	int		iovcnt;
	uint32_t	iov_offset;
	uint32_t	total_size;
};

static inline void
_iscsi_sgl_init(struct _iscsi_sgl *s, struct iovec *iovs, int iovcnt,
		uint32_t iov_offset)
{
	s->iov = iovs;
	s->iovcnt = iovcnt;
	s->iov_offset = iov_offset;
	s->total_size = 0;
}

static inline bool
_iscsi_sgl_append(struct _iscsi_sgl *s, uint8_t *data, uint32_t data_len)
{
	if (s->iov_offset >= data_len) {
		s->iov_offset -= data_len;
	} else {
		assert(s->iovcnt > 0);
		s->iov->iov_base = data + s->iov_offset;
		s->iov->iov_len = data_len - s->iov_offset;
		s->total_size += data_len - s->iov_offset;
		s->iov_offset = 0;
		s->iov++;
		s->iovcnt--;
		if (s->iovcnt == 0) {
			return false;
		}
	}

	return true;
}

/* Build iovec array to leave metadata space for every data block
 * when reading data segment from socket.
 */
static inline bool
_iscsi_sgl_append_with_md(struct _iscsi_sgl *s,
			  void *buf, uint32_t buf_len, uint32_t data_len,
			  struct spdk_dif_ctx *dif_ctx)
{
	int rc;
	uint32_t total_size = 0;
	struct iovec buf_iov;

	if (s->iov_offset >= data_len) {
		s->iov_offset -= data_len;
	} else {
		buf_iov.iov_base = buf;
		buf_iov.iov_len = buf_len;
		rc = spdk_dif_set_md_interleave_iovs(s->iov, s->iovcnt, &buf_iov, 1,
						     s->iov_offset, data_len - s->iov_offset,
						     &total_size, dif_ctx);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to setup iovs for DIF strip\n");
			return false;
		}

		s->total_size += total_size;
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

int
iscsi_build_iovs(struct spdk_iscsi_conn *conn, struct iovec *iovs, int iovcnt,
		 struct spdk_iscsi_pdu *pdu, uint32_t *_mapped_length)
{
	struct _iscsi_sgl sgl;
	int enable_digest;
	uint32_t total_ahs_len;
	uint32_t data_len;

	if (iovcnt == 0) {
		return 0;
	}

	total_ahs_len = pdu->bhs.total_ahs_len;
	data_len = DGET24(pdu->bhs.data_segment_len);
	data_len = ISCSI_ALIGN(data_len);

	enable_digest = 1;
	if (pdu->bhs.opcode == ISCSI_OP_LOGIN_RSP) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	_iscsi_sgl_init(&sgl, iovs, iovcnt, pdu->writev_offset);

	/* BHS */
	if (!_iscsi_sgl_append(&sgl, (uint8_t *)&pdu->bhs, ISCSI_BHS_LEN)) {
		goto end;
	}
	/* AHS */
	if (total_ahs_len > 0) {
		if (!_iscsi_sgl_append(&sgl, pdu->ahs, 4 * total_ahs_len)) {
			goto end;
		}
	}

	/* Header Digest */
	if (enable_digest && conn->header_digest) {
		if (!_iscsi_sgl_append(&sgl, pdu->header_digest, ISCSI_DIGEST_LEN)) {
			goto end;
		}
	}

	/* Data Segment */
	if (data_len > 0) {
		if (!pdu->dif_insert_or_strip) {
			if (!_iscsi_sgl_append(&sgl, pdu->data, data_len)) {
				goto end;
			}
		} else {
			if (!_iscsi_sgl_append_with_md(&sgl, pdu->data, pdu->data_buf_len,
						       data_len, &pdu->dif_ctx)) {
				goto end;
			}
		}
	}

	/* Data Digest */
	if (enable_digest && conn->data_digest && data_len != 0) {
		_iscsi_sgl_append(&sgl, pdu->data_digest, ISCSI_DIGEST_LEN);
	}

end:
	if (_mapped_length != NULL) {
		*_mapped_length = sgl.total_size;
	}

	return iovcnt - sgl.iovcnt;
}

void iscsi_free_sess(struct spdk_iscsi_sess *sess)
{
	if (sess == NULL) {
		return;
	}

	sess->tag = 0;
	sess->target = NULL;
	sess->session_type = SESSION_TYPE_INVALID;
	iscsi_param_free(sess->params);
	free(sess->conns);
	spdk_scsi_port_free(&sess->initiator_port);
	spdk_mempool_put(g_iscsi.session_pool, (void *)sess);
}

static int
create_iscsi_sess(struct spdk_iscsi_conn *conn,
		  struct spdk_iscsi_tgt_node *target,
		  enum session_type session_type)
{
	struct spdk_iscsi_sess *sess;
	int rc;

	sess = spdk_mempool_get(g_iscsi.session_pool);
	if (!sess) {
		SPDK_ERRLOG("Unable to get session object\n");
		SPDK_ERRLOG("MaxSessions set to %d\n", g_iscsi.MaxSessions);
		return -ENOMEM;
	}

	/* configuration values */
	pthread_mutex_lock(&g_iscsi.mutex);

	sess->MaxConnections = g_iscsi.MaxConnectionsPerSession;
	sess->MaxOutstandingR2T = DEFAULT_MAXOUTSTANDINGR2T;

	sess->DefaultTime2Wait = g_iscsi.DefaultTime2Wait;
	sess->DefaultTime2Retain = g_iscsi.DefaultTime2Retain;
	sess->FirstBurstLength = g_iscsi.FirstBurstLength;
	sess->MaxBurstLength = SPDK_ISCSI_MAX_BURST_LENGTH;
	sess->InitialR2T = DEFAULT_INITIALR2T;
	sess->ImmediateData = g_iscsi.ImmediateData;
	sess->DataPDUInOrder = DEFAULT_DATAPDUINORDER;
	sess->DataSequenceInOrder = DEFAULT_DATASEQUENCEINORDER;
	sess->ErrorRecoveryLevel = g_iscsi.ErrorRecoveryLevel;

	pthread_mutex_unlock(&g_iscsi.mutex);

	sess->tag = conn->pg_tag;

	sess->conns = calloc(sess->MaxConnections, sizeof(*sess->conns));
	if (!sess->conns) {
		SPDK_ERRLOG("calloc() failed for connection array\n");
		return -ENOMEM;
	}

	sess->connections = 0;

	sess->conns[sess->connections] = conn;
	sess->connections++;

	sess->params = NULL;
	sess->target = target;
	sess->isid = 0;
	sess->session_type = session_type;
	sess->current_text_itt = 0xffffffffU;

	/* set default params */
	rc = iscsi_sess_params_init(&sess->params);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_sess_params_init() failed\n");
		goto error_return;
	}
	/* replace with config value */
	rc = iscsi_param_set_int(sess->params, "MaxConnections",
				 sess->MaxConnections);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set_int(sess->params, "MaxOutstandingR2T",
				 sess->MaxOutstandingR2T);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set_int(sess->params, "DefaultTime2Wait",
				 sess->DefaultTime2Wait);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set_int(sess->params, "DefaultTime2Retain",
				 sess->DefaultTime2Retain);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set_int(sess->params, "FirstBurstLength",
				 sess->FirstBurstLength);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set_int(sess->params, "MaxBurstLength",
				 sess->MaxBurstLength);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set(sess->params, "InitialR2T",
			     sess->InitialR2T ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set(sess->params, "ImmediateData",
			     sess->ImmediateData ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set(sess->params, "DataPDUInOrder",
			     sess->DataPDUInOrder ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set(sess->params, "DataSequenceInOrder",
			     sess->DataSequenceInOrder ? "Yes" : "No");
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		goto error_return;
	}

	rc = iscsi_param_set_int(sess->params, "ErrorRecoveryLevel",
				 sess->ErrorRecoveryLevel);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	/* realloc buffer */
	rc = iscsi_param_set_int(conn->params, "MaxRecvDataSegmentLength",
				 conn->MaxRecvDataSegmentLength);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set_int() failed\n");
		goto error_return;
	}

	/* sess for first connection of session */
	conn->sess = sess;
	return 0;

error_return:
	iscsi_free_sess(sess);
	conn->sess = NULL;
	return -1;
}

static struct spdk_iscsi_sess *
get_iscsi_sess_by_tsih(uint16_t tsih)
{
	struct spdk_iscsi_sess *session;

	if (tsih == 0 || tsih > g_iscsi.MaxSessions) {
		return NULL;
	}

	session = g_iscsi.session[tsih - 1];
	assert(tsih == session->tsih);

	return session;
}

static uint8_t
append_iscsi_sess(struct spdk_iscsi_conn *conn,
		  const char *initiator_port_name, uint16_t tsih, uint16_t cid)
{
	struct spdk_iscsi_sess *sess;

	SPDK_DEBUGLOG(iscsi, "append session: init port name=%s, tsih=%u, cid=%u\n",
		      initiator_port_name, tsih, cid);

	sess = get_iscsi_sess_by_tsih(tsih);
	if (sess == NULL) {
		SPDK_ERRLOG("spdk_get_iscsi_sess_by_tsih failed\n");
		return ISCSI_LOGIN_CONN_ADD_FAIL;
	}
	if ((conn->pg_tag != sess->tag) ||
	    (strcasecmp(initiator_port_name, spdk_scsi_port_get_name(sess->initiator_port)) != 0) ||
	    (conn->target != sess->target)) {
		/* no match */
		SPDK_ERRLOG("no MCS session for init port name=%s, tsih=%d, cid=%d\n",
			    initiator_port_name, tsih, cid);
		return ISCSI_LOGIN_CONN_ADD_FAIL;
	}

	if (sess->connections >= sess->MaxConnections) {
		/* no slot for connection */
		SPDK_ERRLOG("too many connections for init port name=%s, tsih=%d, cid=%d\n",
			    initiator_port_name, tsih, cid);
		return ISCSI_LOGIN_TOO_MANY_CONNECTIONS;
	}

	SPDK_DEBUGLOG(iscsi, "Connections (tsih %d): %d\n", sess->tsih, sess->connections);
	conn->sess = sess;

	/*
	 * TODO: need a mutex or other sync mechanism to protect the session's
	 *  connection list.
	 */
	sess->conns[sess->connections] = conn;
	sess->connections++;

	return 0;
}

static int
iscsi_append_text(const char *key, const char *val, uint8_t *data,
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
iscsi_append_param(struct spdk_iscsi_conn *conn, const char *key,
		   uint8_t *data, int alloc_len, int data_len)
{
	struct iscsi_param *param;

	param = iscsi_param_find(conn->params, key);
	if (param == NULL) {
		param = iscsi_param_find(conn->sess->params, key);
		if (param == NULL) {
			SPDK_DEBUGLOG(iscsi, "no key %.64s\n", key);
			return data_len;
		}
	}
	return iscsi_append_text(param->key, param->val, data,
				 alloc_len, data_len);
}

static int
iscsi_auth_params(struct spdk_iscsi_conn *conn,
		  struct iscsi_param *params, const char *method, uint8_t *data,
		  int alloc_len, int data_len)
{
	char *in_val;
	char *in_next;
	char *new_val;
	const char *algorithm;
	const char *name;
	const char *response;
	const char *identifier;
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
	if ((algorithm = iscsi_param_get_val(params, "CHAP_A")) != NULL) {
		if (conn->auth.chap_phase != ISCSI_CHAP_PHASE_WAIT_A) {
			SPDK_ERRLOG("CHAP sequence error\n");
			goto error_return;
		}

		/* CHAP_A is LIST type */
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN + 1, "%s", algorithm);
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
			iscsi_append_text("CHAP_A", new_val, data, alloc_len, total);
			goto error_return;
		}
		/* selected algorithm is 5 (MD5) */
		SPDK_DEBUGLOG(iscsi, "got CHAP_A=%s\n", new_val);
		total = iscsi_append_text("CHAP_A", new_val, data, alloc_len, total);

		/* Identifier is one octet */
		gen_random(conn->auth.chap_id, 1);
		snprintf(in_val, ISCSI_TEXT_MAX_VAL_LEN, "%d",
			 (int) conn->auth.chap_id[0]);
		total = iscsi_append_text("CHAP_I", in_val, data, alloc_len, total);

		/* Challenge Value is a variable stream of octets */
		/* (binary length MUST not exceed 1024 bytes) */
		conn->auth.chap_challenge_len = ISCSI_CHAP_CHALLENGE_LEN;
		gen_random(conn->auth.chap_challenge, conn->auth.chap_challenge_len);
		bin2hex(in_val, ISCSI_TEXT_MAX_VAL_LEN,
			conn->auth.chap_challenge, conn->auth.chap_challenge_len);
		total = iscsi_append_text("CHAP_C", in_val, data, alloc_len, total);

		conn->auth.chap_phase = ISCSI_CHAP_PHASE_WAIT_NR;
	} else if ((name = iscsi_param_get_val(params, "CHAP_N")) != NULL) {
		uint8_t resmd5[SPDK_MD5DIGEST_LEN];
		uint8_t tgtmd5[SPDK_MD5DIGEST_LEN];
		struct spdk_md5ctx md5ctx;
		size_t decoded_len = 0;

		if (conn->auth.chap_phase != ISCSI_CHAP_PHASE_WAIT_NR) {
			SPDK_ERRLOG("CHAP sequence error\n");
			goto error_return;
		}

		response = iscsi_param_get_val(params, "CHAP_R");
		if (response == NULL) {
			SPDK_ERRLOG("no response\n");
			goto error_return;
		}
		if (response[0] == '0' &&
		    (response[1] == 'x' || response[1] == 'X')) {
			rc = hex2bin(resmd5, SPDK_MD5DIGEST_LEN, response);
			if (rc < 0 || rc != SPDK_MD5DIGEST_LEN) {
				SPDK_ERRLOG("response format error\n");
				goto error_return;
			}
		} else if (response[0] == '0' &&
			   (response[1] == 'b' || response[1] == 'B')) {
			response += 2;
			rc = spdk_base64_decode(resmd5, &decoded_len, response);
			if (rc < 0 || decoded_len != SPDK_MD5DIGEST_LEN) {
				SPDK_ERRLOG("response format error\n");
				goto error_return;
			}
		} else {
			SPDK_ERRLOG("response format error\n");
			goto error_return;
		}
		SPDK_DEBUGLOG(iscsi, "got CHAP_N/CHAP_R\n");

		SPDK_DEBUGLOG(iscsi, "ag_tag=%d\n", conn->chap_group);

		rc = iscsi_chap_get_authinfo(&conn->auth, name, conn->chap_group);
		if (rc < 0) {
			/* SPDK_ERRLOG("auth user or secret is missing\n"); */
			SPDK_ERRLOG("iscsi_chap_get_authinfo() failed\n");
			goto error_return;
		}
		if (conn->auth.user[0] == '\0' || conn->auth.secret[0] == '\0') {
			/* SPDK_ERRLOG("auth user or secret is missing\n"); */
			SPDK_ERRLOG("auth failed (name %.64s)\n", name);
			goto error_return;
		}

		md5init(&md5ctx);
		/* Identifier */
		md5update(&md5ctx, conn->auth.chap_id, 1);
		/* followed by secret */
		md5update(&md5ctx, conn->auth.secret,
			  strlen(conn->auth.secret));
		/* followed by Challenge Value */
		md5update(&md5ctx, conn->auth.chap_challenge,
			  conn->auth.chap_challenge_len);
		/* tgtmd5 is expecting Response Value */
		md5final(tgtmd5, &md5ctx);

		bin2hex(in_val, ISCSI_TEXT_MAX_VAL_LEN, tgtmd5, SPDK_MD5DIGEST_LEN);

#if 0
		SPDK_DEBUGLOG(iscsi, "tgtmd5=%s, resmd5=%s\n", in_val, response);
		spdk_dump("tgtmd5", tgtmd5, SPDK_MD5DIGEST_LEN);
		spdk_dump("resmd5", resmd5, SPDK_MD5DIGEST_LEN);
#endif

		/* compare MD5 digest */
		if (memcmp(tgtmd5, resmd5, SPDK_MD5DIGEST_LEN) != 0) {
			/* not match */
			/* SPDK_ERRLOG("auth user or secret is missing\n"); */
			SPDK_ERRLOG("auth failed (name %.64s)\n", name);
			goto error_return;
		}
		/* OK initiator's secret */
		conn->authenticated = true;

		/* mutual CHAP? */
		identifier = iscsi_param_get_val(params, "CHAP_I");
		if (identifier != NULL) {
			conn->auth.chap_mid[0] = (uint8_t) strtol(identifier, NULL, 10);
			challenge = iscsi_param_get_val(params, "CHAP_C");
			if (challenge == NULL) {
				SPDK_ERRLOG("CHAP sequence error\n");
				goto error_return;
			}
			if (challenge[0] == '0' &&
			    (challenge[1] == 'x' || challenge[1] == 'X')) {
				rc = hex2bin(conn->auth.chap_mchallenge,
					     ISCSI_CHAP_CHALLENGE_LEN, challenge);
				if (rc < 0) {
					SPDK_ERRLOG("challenge format error\n");
					goto error_return;
				}
				conn->auth.chap_mchallenge_len = rc;
			} else if (challenge[0] == '0' &&
				   (challenge[1] == 'b' || challenge[1] == 'B')) {
				challenge += 2;
				rc = spdk_base64_decode(conn->auth.chap_mchallenge,
							&decoded_len, challenge);
				if (rc < 0) {
					SPDK_ERRLOG("challenge format error\n");
					goto error_return;
				}
				conn->auth.chap_mchallenge_len = decoded_len;
			} else {
				SPDK_ERRLOG("challenge format error\n");
				goto error_return;
			}
#if 0
			spdk_dump("MChallenge", conn->auth.chap_mchallenge,
				  conn->auth.chap_mchallenge_len);
#endif
			SPDK_DEBUGLOG(iscsi, "got CHAP_I/CHAP_C\n");

			if (conn->auth.muser[0] == '\0' || conn->auth.msecret[0] == '\0') {
				/* SPDK_ERRLOG("mutual auth user or secret is missing\n"); */
				SPDK_ERRLOG("auth failed (name %.64s)\n", name);
				goto error_return;
			}

			md5init(&md5ctx);
			/* Identifier */
			md5update(&md5ctx, conn->auth.chap_mid, 1);
			/* followed by secret */
			md5update(&md5ctx, conn->auth.msecret,
				  strlen(conn->auth.msecret));
			/* followed by Challenge Value */
			md5update(&md5ctx, conn->auth.chap_mchallenge,
				  conn->auth.chap_mchallenge_len);
			/* tgtmd5 is Response Value */
			md5final(tgtmd5, &md5ctx);

			bin2hex(in_val, ISCSI_TEXT_MAX_VAL_LEN, tgtmd5, SPDK_MD5DIGEST_LEN);

			total = iscsi_append_text("CHAP_N", conn->auth.muser, data,
						  alloc_len, total);
			total = iscsi_append_text("CHAP_R", in_val, data, alloc_len, total);
		} else {
			/* not mutual */
			if (conn->mutual_chap) {
				SPDK_ERRLOG("required mutual CHAP\n");
				goto error_return;
			}
		}

		conn->auth.chap_phase = ISCSI_CHAP_PHASE_END;
	} else {
		/* not found CHAP keys */
		SPDK_DEBUGLOG(iscsi, "start CHAP\n");
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
iscsi_check_values(struct spdk_iscsi_conn *conn)
{
	if (conn->sess->FirstBurstLength > conn->sess->MaxBurstLength) {
		SPDK_ERRLOG("FirstBurstLength(%d) > MaxBurstLength(%d)\n",
			    conn->sess->FirstBurstLength,
			    conn->sess->MaxBurstLength);
		return -1;
	}
	if (conn->sess->FirstBurstLength > g_iscsi.FirstBurstLength) {
		SPDK_ERRLOG("FirstBurstLength(%d) > iSCSI target restriction(%d)\n",
			    conn->sess->FirstBurstLength, g_iscsi.FirstBurstLength);
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

static int
iscsi_conn_params_update(struct spdk_iscsi_conn *conn)
{
	int rc;
	uint32_t recv_buf_size;

	/* update internal variables */
	rc = iscsi_copy_param2var(conn);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_copy_param2var() failed\n");
		if (conn->state < ISCSI_CONN_STATE_EXITING) {
			conn->state = ISCSI_CONN_STATE_EXITING;
		}
		return rc;
	}

	/* check value */
	rc = iscsi_check_values(conn);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_check_values() failed\n");
		if (conn->state < ISCSI_CONN_STATE_EXITING) {
			conn->state = ISCSI_CONN_STATE_EXITING;
		}
	}

	/* The socket receive buffer may need to be adjusted based on the new parameters */

	/* Don't allow the recv buffer to be 0 or very large. */
	recv_buf_size = spdk_max(0x1000, spdk_min(0x2000, conn->sess->FirstBurstLength));

	/* Add in extra space for the PDU */
	recv_buf_size += ISCSI_BHS_LEN + ISCSI_AHS_LEN;

	if (conn->header_digest) {
		recv_buf_size += ISCSI_DIGEST_LEN;
	}

	if (conn->data_digest) {
		recv_buf_size += ISCSI_DIGEST_LEN;
	}

	/* Set up to buffer up to 4 commands with immediate data at once */
	if (spdk_sock_set_recvbuf(conn->sock, recv_buf_size * 4) < 0) {
		/* Not fatal. */
	}

	return rc;
}

static void
iscsi_conn_login_pdu_err_complete(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->full_feature) {
		iscsi_conn_params_update(conn);
	}
}

static void
iscsi_conn_login_pdu_success_complete(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;


	if (conn->state >= ISCSI_CONN_STATE_EXITING) {
		/* Connection is being exited before this callback is executed. */
		SPDK_DEBUGLOG(iscsi, "Connection is already exited.\n");
		return;
	}
	if (conn->full_feature) {
		if (iscsi_conn_params_update(conn) != 0) {
			return;
		}
	}
	conn->state = ISCSI_CONN_STATE_RUNNING;
	if (conn->full_feature != 0) {
		iscsi_conn_schedule(conn);
	}
}

/*
 * The response function of spdk_iscsi_op_login
 */
static void
iscsi_op_login_response(struct spdk_iscsi_conn *conn,
			struct spdk_iscsi_pdu *rsp_pdu, struct iscsi_param *params,
			iscsi_conn_xfer_complete_cb cb_fn)
{
	struct iscsi_bhs_login_rsp *rsph;

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

	SPDK_LOGDUMP(iscsi, "PDU", (uint8_t *)rsph, ISCSI_BHS_LEN);
	SPDK_LOGDUMP(iscsi, "DATA", rsp_pdu->data, rsp_pdu->data_segment_len);

	/* Set T/CSG/NSG to reserved if login error. */
	if (rsph->status_class != 0) {
		rsph->flags &= ~(ISCSI_LOGIN_TRANSIT | ISCSI_LOGIN_CURRENT_STAGE_MASK |
				 ISCSI_LOGIN_NEXT_STAGE_MASK);
	}
	iscsi_param_free(params);
	iscsi_conn_write_pdu(conn, rsp_pdu, cb_fn, conn);
}

/*
 * The function which is used to initialize the internal response data
 * structure of iscsi login function.
 * return:
 * 0, success;
 * otherwise, error;
 */
static int
iscsi_op_login_rsp_init(struct spdk_iscsi_conn *conn,
			struct spdk_iscsi_pdu *pdu, struct spdk_iscsi_pdu *rsp_pdu)
{
	struct iscsi_bhs_login_req *reqh;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	rsph->opcode = ISCSI_OP_LOGIN_RSP;
	rsph->status_class = ISCSI_CLASS_SUCCESS;
	rsph->status_detail = ISCSI_LOGIN_ACCEPT;
	rsp_pdu->data_segment_len = 0;

	/* The default MaxRecvDataSegmentLength 8192 is used during login. - RFC3720 */
	rsp_pdu->data = calloc(1, 8192);
	if (!rsp_pdu->data) {
		SPDK_ERRLOG("calloc() failed for data segment\n");
		rsph->status_class = ISCSI_CLASS_TARGET_ERROR;
		rsph->status_detail = ISCSI_LOGIN_STATUS_NO_RESOURCES;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	rsp_pdu->data_buf_len = 8192;

	reqh = (struct iscsi_bhs_login_req *)&pdu->bhs;
	rsph->flags |= (reqh->flags & (ISCSI_LOGIN_TRANSIT | ISCSI_LOGIN_CONTINUE |
				       ISCSI_LOGIN_CURRENT_STAGE_MASK));
	if (ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags)) {
		rsph->flags |= (reqh->flags & ISCSI_LOGIN_NEXT_STAGE_MASK);
	}

	/* We don't need to convert from network byte order. Just store it */
	memcpy(&rsph->isid, reqh->isid, 6);
	rsph->tsih = reqh->tsih;
	rsph->itt = reqh->itt;
	rsp_pdu->cmd_sn = from_be32(&reqh->cmd_sn);

	if (rsph->tsih) {
		rsph->stat_sn = reqh->exp_stat_sn;
	}

	SPDK_LOGDUMP(iscsi, "PDU", (uint8_t *)&pdu->bhs, ISCSI_BHS_LEN);

	SPDK_DEBUGLOG(iscsi,
		      "T=%d, C=%d, CSG=%d, NSG=%d, Min=%d, Max=%d, ITT=%x\n",
		      ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags),
		      ISCSI_BHS_LOGIN_GET_CBIT(rsph->flags),
		      ISCSI_BHS_LOGIN_GET_CSG(rsph->flags),
		      ISCSI_BHS_LOGIN_GET_NSG(rsph->flags),
		      reqh->version_min, reqh->version_max, from_be32(&rsph->itt));

	if (conn->sess != NULL) {
		SPDK_DEBUGLOG(iscsi,
			      "CmdSN=%u, ExpStatSN=%u, StatSN=%u, ExpCmdSN=%u,"
			      "MaxCmdSN=%u\n", rsp_pdu->cmd_sn,
			      from_be32(&rsph->stat_sn), conn->StatSN,
			      conn->sess->ExpCmdSN,
			      conn->sess->MaxCmdSN);
	} else {
		SPDK_DEBUGLOG(iscsi,
			      "CmdSN=%u, ExpStatSN=%u, StatSN=%u\n",
			      rsp_pdu->cmd_sn, from_be32(&rsph->stat_sn),
			      conn->StatSN);
	}

	if (ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags) &&
	    ISCSI_BHS_LOGIN_GET_CBIT(rsph->flags)) {
		SPDK_ERRLOG("transit error\n");
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	/* make sure reqh->version_max < ISCSI_VERSION */
	if (reqh->version_min > ISCSI_VERSION) {
		SPDK_ERRLOG("unsupported version min %d/max %d, expecting %d\n", reqh->version_min,
			    reqh->version_max, ISCSI_VERSION);
		/* Unsupported version */
		/* set all reserved flag to zero */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_UNSUPPORTED_VERSION;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	if ((ISCSI_BHS_LOGIN_GET_NSG(rsph->flags) == ISCSI_NSG_RESERVED_CODE) &&
	    ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags)) {
		/* set NSG and other bits to zero */
		rsph->flags &= ~(ISCSI_LOGIN_NEXT_STAGE_MASK | ISCSI_LOGIN_TRANSIT |
				 ISCSI_LOGIN_CURRENT_STAGE_MASK);
		SPDK_ERRLOG("Received reserved NSG code: %d\n", ISCSI_NSG_RESERVED_CODE);
		/* Initiator error */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	return 0;
}

static int
iscsi_op_login_store_incoming_params(struct spdk_iscsi_conn *conn,
				     struct spdk_iscsi_pdu *pdu, struct spdk_iscsi_pdu *rsp_pdu,
				     struct iscsi_param **params)
{
	struct iscsi_bhs_login_req *reqh;
	struct iscsi_bhs_login_rsp *rsph;
	int rc;

	reqh = (struct iscsi_bhs_login_req *)&pdu->bhs;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	rc = iscsi_parse_params(params, pdu->data,
				pdu->data_segment_len, ISCSI_BHS_LOGIN_GET_CBIT(reqh->flags),
				&conn->partial_text_parameter);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_parse_params() failed\n");
		iscsi_param_free(*params);
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}

	return 0;
}

/*
 * This function is used to initialize the port info
 * return
 * 0: success
 * otherwise: error
 */
static int
iscsi_op_login_initialize_port(struct spdk_iscsi_conn *conn,
			       struct spdk_iscsi_pdu *rsp_pdu,
			       char *initiator_port_name,
			       uint32_t name_length,
			       struct iscsi_param *params)
{
	const char *val;
	struct iscsi_bhs_login_rsp *rsph;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	/* Initiator Name and Port */
	val = iscsi_param_get_val(params, "InitiatorName");
	if (val == NULL) {
		SPDK_ERRLOG("InitiatorName is empty\n");
		/* Missing parameter */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	snprintf(conn->initiator_name, sizeof(conn->initiator_name), "%s", val);
	snprintf(initiator_port_name, name_length,
		 "%s,i,0x%12.12" PRIx64, val, iscsi_get_isid(rsph->isid));
	spdk_strlwr(conn->initiator_name);
	spdk_strlwr(initiator_port_name);
	SPDK_DEBUGLOG(iscsi, "Initiator name: %s\n", conn->initiator_name);
	SPDK_DEBUGLOG(iscsi, "Initiator port: %s\n", initiator_port_name);

	return 0;
}

/*
 * This function is used to judge the session type
 * return
 * 0: success
 * Other value: error
 */
static int
iscsi_op_login_session_type(struct spdk_iscsi_conn *conn,
			    struct spdk_iscsi_pdu *rsp_pdu,
			    enum session_type *session_type,
			    struct iscsi_param *params)
{
	const char *session_type_str;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	session_type_str = iscsi_param_get_val(params, "SessionType");
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
	SPDK_DEBUGLOG(iscsi, "Session Type: %s\n", session_type_str);

	return 0;
}

/*
 * This function is used to check the target info
 * return:
 * 0: success
 * otherwise: error
 */
static int
iscsi_op_login_check_target(struct spdk_iscsi_conn *conn,
			    struct spdk_iscsi_pdu *rsp_pdu,
			    const char *target_name,
			    struct spdk_iscsi_tgt_node **target)
{
	struct iscsi_bhs_login_rsp *rsph;
	char buf[MAX_TMPBUF] = {};

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	*target = iscsi_find_tgt_node(target_name);
	if (*target == NULL) {
		SPDK_WARNLOG("target %s not found\n", target_name);
		/* Not found */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_TARGET_NOT_FOUND;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	if (iscsi_tgt_node_is_destructed(*target)) {
		SPDK_ERRLOG("target %s is removed\n", target_name);
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_TARGET_REMOVED;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	if (iscsi_tgt_node_is_redirected(conn, *target, buf, MAX_TMPBUF)) {
		SPDK_INFOLOG(iscsi, "target %s is redirectd\n", target_name);
		rsp_pdu->data_segment_len = iscsi_append_text("TargetAddress",
					    buf,
					    rsp_pdu->data,
					    rsp_pdu->data_buf_len,
					    rsp_pdu->data_segment_len);
		rsph->status_class = ISCSI_CLASS_REDIRECT;
		rsph->status_detail = ISCSI_LOGIN_TARGET_TEMPORARILY_MOVED;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}
	if (!iscsi_tgt_node_access(conn, *target, conn->initiator_name,
				   conn->initiator_addr)) {
		SPDK_ERRLOG("access denied\n");
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_AUTHORIZATION_FAIL;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
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
iscsi_op_login_check_session(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_pdu *rsp_pdu,
			     char *initiator_port_name, int cid)

{
	int rc = 0;
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	/* check existing session */
	SPDK_DEBUGLOG(iscsi, "isid=%"PRIx64", tsih=%u, cid=%u\n",
		      iscsi_get_isid(rsph->isid), from_be16(&rsph->tsih), cid);
	if (rsph->tsih != 0) {
		/* multiple connections */
		rc = append_iscsi_sess(conn, initiator_port_name,
				       from_be16(&rsph->tsih), cid);
		if (rc != 0) {
			SPDK_ERRLOG("isid=%"PRIx64", tsih=%u, cid=%u:"
				    "spdk_append_iscsi_sess() failed\n",
				    iscsi_get_isid(rsph->isid), from_be16(&rsph->tsih),
				    cid);
			/* Can't include in session */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = rc;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
	} else if (!g_iscsi.AllowDuplicateIsid) {
		/* new session, drop old sess by the initiator */
		iscsi_drop_conns(conn, initiator_port_name, 0 /* drop old */);
	}

	return rc;
}

/*
 * This function is used to del the original param and update it with new
 * value
 * return:
 * 0: success
 * otherwise: error
 */
static int
iscsi_op_login_update_param(struct spdk_iscsi_conn *conn,
			    const char *key, const char *value,
			    const char *list)
{
	int rc = 0;
	struct iscsi_param *new_param, *orig_param;
	int index;

	orig_param = iscsi_param_find(conn->params, key);
	if (orig_param == NULL) {
		SPDK_ERRLOG("orig_param %s not found\n", key);
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}

	index = orig_param->state_index;
	rc = iscsi_param_del(&conn->params, key);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_del(%s) failed\n", key);
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	rc = iscsi_param_add(&conn->params, key, value, list, ISPT_LIST);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_add() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	new_param = iscsi_param_find(conn->params, key);
	if (new_param == NULL) {
		SPDK_ERRLOG("iscsi_param_find() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	new_param->state_index = index;
	return rc;
}

static int
iscsi_negotiate_chap_param(struct spdk_iscsi_conn *conn)
{
	int rc = 0;

	if (conn->disable_chap) {
		rc = iscsi_op_login_update_param(conn, "AuthMethod", "None", "None");
	} else if (conn->require_chap) {
		rc = iscsi_op_login_update_param(conn, "AuthMethod", "CHAP", "CHAP");
	}

	return rc;
}

/*
 * The function which is used to handle the part of session discovery
 * return:
 * 0, success;
 * otherwise: error;
 */
static int
iscsi_op_login_session_discovery_chap(struct spdk_iscsi_conn *conn)
{
	return iscsi_negotiate_chap_param(conn);
}

/*
 * This function is used to update the param related with chap
 * return:
 * 0: success
 * otherwise: error
 */
static int
iscsi_op_login_negotiate_chap_param(struct spdk_iscsi_conn *conn,
				    struct spdk_iscsi_tgt_node *target)
{
	conn->disable_chap = target->disable_chap;
	conn->require_chap = target->require_chap;
	conn->mutual_chap = target->mutual_chap;
	conn->chap_group = target->chap_group;

	return iscsi_negotiate_chap_param(conn);
}

static int
iscsi_op_login_negotiate_digest_param(struct spdk_iscsi_conn *conn,
				      struct spdk_iscsi_tgt_node *target)
{
	int rc;

	if (target->header_digest) {
		/*
		 * User specified header digests, so update the list of
		 *  HeaderDigest values to remove "None" so that only
		 *  initiators who support CRC32C can connect.
		 */
		rc = iscsi_op_login_update_param(conn, "HeaderDigest", "CRC32C", "CRC32C");
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
		rc = iscsi_op_login_update_param(conn, "DataDigest", "CRC32C", "CRC32C");
		if (rc < 0) {
			return rc;
		}
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
iscsi_op_login_session_normal(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_pdu *rsp_pdu,
			      char *initiator_port_name,
			      struct iscsi_param *params,
			      int cid)
{
	struct spdk_iscsi_tgt_node *target = NULL;
	const char *target_name;
	const char *target_short_name;
	struct iscsi_bhs_login_rsp *rsph;
	int rc = 0;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	target_name = iscsi_param_get_val(params, "TargetName");

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
			/* Invalid request */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = ISCSI_LOGIN_INVALID_LOGIN_REQUEST;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
		snprintf(conn->target_short_name, MAX_TARGET_NAME, "%s",
			 target_short_name);
	}

	pthread_mutex_lock(&g_iscsi.mutex);
	rc = iscsi_op_login_check_target(conn, rsp_pdu, target_name, &target);
	pthread_mutex_unlock(&g_iscsi.mutex);

	if (rc < 0) {
		return rc;
	}

	conn->target = target;
	conn->dev = target->dev;
	conn->target_port = spdk_scsi_dev_find_port_by_id(target->dev,
			    conn->pg_tag);

	rc = iscsi_op_login_check_session(conn, rsp_pdu,
					  initiator_port_name, cid);
	if (rc < 0) {
		return rc;
	}

	/* force target flags */
	pthread_mutex_lock(&target->mutex);
	rc = iscsi_op_login_negotiate_chap_param(conn, target);
	pthread_mutex_unlock(&target->mutex);

	if (rc == 0) {
		rc = iscsi_op_login_negotiate_digest_param(conn, target);
	}

	if (rc != 0) {
		/* Invalid request */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INVALID_LOGIN_REQUEST;
	}

	return rc;
}

/*
 * This function is used to set the info in the connection data structure
 * return
 * 0: success
 * otherwise: error
 */
static int
iscsi_op_login_set_conn_info(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_pdu *rsp_pdu,
			     char *initiator_port_name,
			     enum session_type session_type, int cid)
{
	int rc = 0;
	struct spdk_iscsi_tgt_node *target;
	struct iscsi_bhs_login_rsp *rsph;
	struct spdk_scsi_port *initiator_port;

	target = conn->target;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	conn->authenticated = false;
	conn->auth.chap_phase = ISCSI_CHAP_PHASE_WAIT_A;
	conn->cid = cid;

	if (conn->sess == NULL) {
		/* create initiator port */
		initiator_port = spdk_scsi_port_create(iscsi_get_isid(rsph->isid), 0, initiator_port_name);
		if (initiator_port == NULL) {
			SPDK_ERRLOG("create_port() failed\n");
			rsph->status_class = ISCSI_CLASS_TARGET_ERROR;
			rsph->status_detail = ISCSI_LOGIN_STATUS_NO_RESOURCES;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}

		/* new session */
		rc = create_iscsi_sess(conn, target, session_type);
		if (rc < 0) {
			spdk_scsi_port_free(&initiator_port);
			SPDK_ERRLOG("create_sess() failed\n");
			rsph->status_class = ISCSI_CLASS_TARGET_ERROR;
			rsph->status_detail = ISCSI_LOGIN_STATUS_NO_RESOURCES;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
		/* initialize parameters */
		conn->sess->initiator_port = initiator_port;
		conn->StatSN = from_be32(&rsph->stat_sn);
		conn->sess->isid = iscsi_get_isid(rsph->isid);

		/* Initiator port TransportID */
		spdk_scsi_port_set_iscsi_transport_id(conn->sess->initiator_port,
						      conn->initiator_name,
						      conn->sess->isid);

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
iscsi_op_login_set_target_info(struct spdk_iscsi_conn *conn,
			       struct spdk_iscsi_pdu *rsp_pdu,
			       enum session_type session_type)
{
	char buf[MAX_TMPBUF];
	const char *val;
	int rc = 0;
	struct spdk_iscsi_tgt_node *target = conn->target;

	/* declarative parameters */
	if (target != NULL) {
		pthread_mutex_lock(&target->mutex);
		if (target->alias[0] != '\0') {
			snprintf(buf, sizeof buf, "%s", target->alias);
		} else {
			snprintf(buf, sizeof buf, "%s", "");
		}
		pthread_mutex_unlock(&target->mutex);
		rc = iscsi_param_set(conn->sess->params, "TargetAlias", buf);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_param_set() failed\n");
			return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
		}
	}
	snprintf(buf, sizeof buf, "%s:%s,%d", conn->portal_host, conn->portal_port,
		 conn->pg_tag);
	rc = iscsi_param_set(conn->sess->params, "TargetAddress", buf);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}
	snprintf(buf, sizeof buf, "%d", conn->pg_tag);
	rc = iscsi_param_set(conn->sess->params, "TargetPortalGroupTag", buf);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_param_set() failed\n");
		return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
	}

	/* write in response */
	if (target != NULL) {
		val = iscsi_param_get_val(conn->sess->params, "TargetAlias");
		if (val != NULL && strlen(val) != 0) {
			rsp_pdu->data_segment_len = iscsi_append_param(conn,
						    "TargetAlias",
						    rsp_pdu->data,
						    rsp_pdu->data_buf_len,
						    rsp_pdu->data_segment_len);
		}
		if (session_type == SESSION_TYPE_DISCOVERY) {
			rsp_pdu->data_segment_len = iscsi_append_param(conn,
						    "TargetAddress",
						    rsp_pdu->data,
						    rsp_pdu->data_buf_len,
						    rsp_pdu->data_segment_len);
		}
		rsp_pdu->data_segment_len = iscsi_append_param(conn,
					    "TargetPortalGroupTag",
					    rsp_pdu->data,
					    rsp_pdu->data_buf_len,
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
iscsi_op_login_phase_none(struct spdk_iscsi_conn *conn,
			  struct spdk_iscsi_pdu *rsp_pdu,
			  struct iscsi_param *params, int cid)
{
	enum session_type session_type;
	char initiator_port_name[MAX_INITIATOR_PORT_NAME];
	struct iscsi_bhs_login_rsp *rsph;
	int rc = 0;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	conn->target = NULL;
	conn->dev = NULL;

	rc = iscsi_op_login_initialize_port(conn, rsp_pdu, initiator_port_name,
					    MAX_INITIATOR_PORT_NAME, params);
	if (rc < 0) {
		return rc;
	}

	rc = iscsi_op_login_session_type(conn, rsp_pdu, &session_type, params);
	if (rc < 0) {
		return rc;
	}

	/* Target Name and Port */
	if (session_type == SESSION_TYPE_NORMAL) {
		rc = iscsi_op_login_session_normal(conn, rsp_pdu,
						   initiator_port_name,
						   params, cid);
		if (rc < 0) {
			return rc;
		}

	} else if (session_type == SESSION_TYPE_DISCOVERY) {
		rsph->tsih = 0;

		/* force target flags */
		pthread_mutex_lock(&g_iscsi.mutex);
		rc = iscsi_op_login_session_discovery_chap(conn);
		pthread_mutex_unlock(&g_iscsi.mutex);
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

	rc = iscsi_op_login_set_conn_info(conn, rsp_pdu, initiator_port_name,
					  session_type, cid);
	if (rc < 0) {
		return rc;
	}

	/* limit conns on discovery session */
	if (session_type == SESSION_TYPE_DISCOVERY) {
		conn->sess->MaxConnections = 1;
		rc = iscsi_param_set_int(conn->sess->params,
					 "MaxConnections",
					 conn->sess->MaxConnections);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_param_set_int() failed\n");
			return SPDK_ISCSI_LOGIN_ERROR_PARAMETER;
		}
	}

	return iscsi_op_login_set_target_info(conn, rsp_pdu, session_type);
}

/*
 * This function is used to set the csg bit case in rsp
 * return:
 * 0, success
 * otherwise: error
 */
static int
iscsi_op_login_rsp_handle_csg_bit(struct spdk_iscsi_conn *conn,
				  struct spdk_iscsi_pdu *rsp_pdu,
				  struct iscsi_param *params)
{
	const char *auth_method;
	int rc;
	struct iscsi_bhs_login_rsp *rsph;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	switch (ISCSI_BHS_LOGIN_GET_CSG(rsph->flags)) {
	case ISCSI_SECURITY_NEGOTIATION_PHASE:
		/* SecurityNegotiation */
		auth_method = iscsi_param_get_val(conn->params, "AuthMethod");
		if (auth_method == NULL) {
			SPDK_ERRLOG("AuthMethod is empty\n");
			/* Missing parameter */
			rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
			rsph->status_detail = ISCSI_LOGIN_MISSING_PARMS;
			return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
		}
		if (strcasecmp(auth_method, "None") == 0) {
			conn->authenticated = true;
		} else {
			rc = iscsi_auth_params(conn, params, auth_method,
					       rsp_pdu->data, rsp_pdu->data_buf_len,
					       rsp_pdu->data_segment_len);
			if (rc < 0) {
				SPDK_ERRLOG("iscsi_auth_params() failed\n");
				/* Authentication failure */
				rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
				rsph->status_detail = ISCSI_LOGIN_AUTHENT_FAIL;
				return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
			}
			rsp_pdu->data_segment_len = rc;
			if (!conn->authenticated) {
				/* not complete */
				rsph->flags &= ~ISCSI_LOGIN_TRANSIT;
			} else {
				if (conn->auth.chap_phase != ISCSI_CHAP_PHASE_END) {
					SPDK_DEBUGLOG(iscsi, "CHAP phase not complete");
				}
			}

			SPDK_LOGDUMP(iscsi, "Negotiated Auth Params",
				     rsp_pdu->data, rsp_pdu->data_segment_len);
		}
		break;

	case ISCSI_OPERATIONAL_NEGOTIATION_PHASE:
		/* LoginOperationalNegotiation */
		if (conn->state == ISCSI_CONN_STATE_INVALID) {
			if (conn->require_chap) {
				/* Authentication failure */
				rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
				rsph->status_detail = ISCSI_LOGIN_AUTHENT_FAIL;
				return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
			} else {
				/* AuthMethod=None */
				conn->authenticated = true;
			}
		}
		if (!conn->authenticated) {
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
iscsi_op_login_notify_session_info(struct spdk_iscsi_conn *conn,
				   struct spdk_iscsi_pdu *rsp_pdu)
{
	struct iscsi_bhs_login_rsp *rsph;

	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;
	if (conn->sess->session_type == SESSION_TYPE_NORMAL) {
		/* normal session */
		SPDK_DEBUGLOG(iscsi, "Login from %s (%s) on %s tgt_node%d"
			      " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			      " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			      conn->initiator_name, conn->initiator_addr,
			      conn->target->name, conn->target->num,
			      conn->portal_host, conn->portal_port, conn->pg_tag,
			      conn->sess->isid, conn->sess->tsih, conn->cid,
			      (iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
			       ? "on" : "off"),
			      (iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
			       ? "on" : "off"));
	} else if (conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		/* discovery session */
		SPDK_DEBUGLOG(iscsi, "Login(discovery) from %s (%s) on"
			      " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			      " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			      conn->initiator_name, conn->initiator_addr,
			      conn->portal_host, conn->portal_port, conn->pg_tag,
			      conn->sess->isid, conn->sess->tsih, conn->cid,
			      (iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
			       ? "on" : "off"),
			      (iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
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
iscsi_op_login_rsp_handle_t_bit(struct spdk_iscsi_conn *conn,
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

		rc = iscsi_op_login_notify_session_info(conn, rsp_pdu);
		if (rc < 0) {
			return rc;
		}

		conn->full_feature = 1;
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
iscsi_op_login_rsp_handle(struct spdk_iscsi_conn *conn,
			  struct spdk_iscsi_pdu *rsp_pdu, struct iscsi_param **params)
{
	int rc;
	struct iscsi_bhs_login_rsp *rsph;
	rsph = (struct iscsi_bhs_login_rsp *)&rsp_pdu->bhs;

	/* negotiate parameters */
	rc = iscsi_negotiate_params(conn, params, rsp_pdu->data,
				    rsp_pdu->data_buf_len,
				    rsp_pdu->data_segment_len);
	if (rc < 0) {
		/*
		 * iscsi_negotiate_params just returns -1 on failure,
		 *  so translate this into meaningful response codes and
		 *  return values.
		 */
		rsph->status_class = ISCSI_CLASS_INITIATOR_ERROR;
		rsph->status_detail = ISCSI_LOGIN_INITIATOR_ERROR;
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	}

	rsp_pdu->data_segment_len = rc;
	SPDK_LOGDUMP(iscsi, "Negotiated Params", rsp_pdu->data, rc);

	/* handle the CSG bit case */
	rc = iscsi_op_login_rsp_handle_csg_bit(conn, rsp_pdu, *params);
	if (rc < 0) {
		return rc;
	}

	/* handle the T bit case */
	if (ISCSI_BHS_LOGIN_GET_TBIT(rsph->flags)) {
		rc = iscsi_op_login_rsp_handle_t_bit(conn, rsp_pdu);
	}

	return rc;
}

static int
iscsi_pdu_hdr_op_login(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int rc;
	struct iscsi_bhs_login_req *reqh;
	struct spdk_iscsi_pdu *rsp_pdu;

	if (conn->full_feature && conn->sess != NULL &&
	    conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_login_req *)&pdu->bhs;
	pdu->cmd_sn = from_be32(&reqh->cmd_sn);

	/* During login processing, use the 8KB default FirstBurstLength as
	 *  our maximum data segment length value.
	 */
	if (pdu->data_segment_len > SPDK_ISCSI_FIRST_BURST_LENGTH) {
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	rsp_pdu = iscsi_get_pdu(conn);
	if (rsp_pdu == NULL) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	rc = iscsi_op_login_rsp_init(conn, pdu, rsp_pdu);
	if (rc < 0) {
		iscsi_op_login_response(conn, rsp_pdu, NULL, iscsi_conn_login_pdu_err_complete);
		return 0;
	}

	conn->login_rsp_pdu = rsp_pdu;
	return 0;
}

static int
iscsi_pdu_payload_op_login(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int rc;
	struct iscsi_bhs_login_req *reqh;
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_param *params = NULL;
	int cid;

	if (conn->login_rsp_pdu == NULL) {
		return 0;
	}

	spdk_poller_unregister(&conn->login_timer);
	rsp_pdu = conn->login_rsp_pdu;

	reqh = (struct iscsi_bhs_login_req *)&pdu->bhs;
	cid = from_be16(&reqh->cid);

	rc = iscsi_op_login_store_incoming_params(conn, pdu, rsp_pdu, &params);
	if (rc < 0) {
		iscsi_op_login_response(conn, rsp_pdu, NULL, iscsi_conn_login_pdu_err_complete);
		return 0;
	}

	if (conn->state == ISCSI_CONN_STATE_INVALID) {
		rc = iscsi_op_login_phase_none(conn, rsp_pdu, params, cid);
		if (rc == SPDK_ISCSI_LOGIN_ERROR_RESPONSE || rc == SPDK_ISCSI_LOGIN_ERROR_PARAMETER) {
			iscsi_op_login_response(conn, rsp_pdu, params, iscsi_conn_login_pdu_err_complete);
			return 0;
		}
	}

	rc = iscsi_op_login_rsp_handle(conn, rsp_pdu, &params);
	if (rc == SPDK_ISCSI_LOGIN_ERROR_RESPONSE) {
		iscsi_op_login_response(conn, rsp_pdu, params, iscsi_conn_login_pdu_err_complete);
		return 0;
	}

	iscsi_op_login_response(conn, rsp_pdu, params, iscsi_conn_login_pdu_success_complete);
	return 0;
}

static int
iscsi_pdu_hdr_op_text(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	uint32_t task_tag;
	uint32_t ExpStatSN;
	int F_bit, C_bit;
	struct iscsi_bhs_text_req *reqh;

	if (pdu->data_segment_len > iscsi_get_max_immediate_data_size()) {
		SPDK_ERRLOG("data segment len(=%zu) > immediate data len(=%"PRIu32")\n",
			    pdu->data_segment_len, iscsi_get_max_immediate_data_size());
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	reqh = (struct iscsi_bhs_text_req *)&pdu->bhs;

	F_bit = !!(reqh->flags & ISCSI_FLAG_FINAL);
	C_bit = !!(reqh->flags & ISCSI_TEXT_CONTINUE);
	task_tag = from_be32(&reqh->itt);
	ExpStatSN = from_be32(&reqh->exp_stat_sn);

	SPDK_DEBUGLOG(iscsi, "I=%d, F=%d, C=%d, ITT=%x, TTT=%x\n",
		      reqh->immediate, F_bit, C_bit, task_tag, from_be32(&reqh->ttt));

	SPDK_DEBUGLOG(iscsi,
		      "CmdSN=%u, ExpStatSN=%u, StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      pdu->cmd_sn, ExpStatSN, conn->StatSN, conn->sess->ExpCmdSN,
		      conn->sess->MaxCmdSN);

	if (ExpStatSN != conn->StatSN) {
#if 0
		SPDK_ERRLOG("StatSN(%u) error\n", ExpStatSN);
		return -1;
#else
		/* StarPort have a bug */
		SPDK_DEBUGLOG(iscsi, "StatSN(%u) rewound\n", ExpStatSN);
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
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	return 0;
}

static void
iscsi_conn_text_pdu_complete(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	iscsi_conn_params_update(conn);
}

static int
iscsi_pdu_payload_op_text(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct iscsi_param *params = NULL;
	struct spdk_iscsi_pdu *rsp_pdu;
	uint8_t *data;
	uint64_t lun;
	uint32_t task_tag;
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

	/* store incoming parameters */
	rc = iscsi_parse_params(&params, pdu->data, pdu->data_segment_len,
				C_bit, &conn->partial_text_parameter);
	if (rc < 0) {
		SPDK_ERRLOG("iscsi_parse_params() failed\n");
		iscsi_param_free(params);
		return -1;
	}

	if (pdu->data_segment_len == 0 && params == NULL) {
		params = conn->params_text;
		conn->params_text = NULL;
	}

	data = calloc(1, alloc_len);
	if (!data) {
		SPDK_ERRLOG("calloc() failed for data segment\n");
		iscsi_param_free(params);
		return -ENOMEM;
	}

	/* negotiate parameters */
	data_len = iscsi_negotiate_params(conn, &params,
					  data, alloc_len, data_len);
	if (data_len < 0) {
		SPDK_ERRLOG("iscsi_negotiate_params() failed\n");
		iscsi_param_free(params);
		free(data);
		return -1;
	}

	/* sendtargets is special case */
	val = iscsi_param_get_val(params, "SendTargets");
	if (val != NULL) {
		if (iscsi_param_eq_val(conn->sess->params,
				       "SessionType", "Discovery")) {
			if (strcasecmp(val, "") == 0) {
				val = "ALL";
			}

			data_len = iscsi_send_tgts(conn,
						   conn->initiator_name,
						   val, data, alloc_len,
						   data_len);
		} else {
			if (strcasecmp(val, "") == 0) {
				val = conn->target->name;
			}

			if (strcasecmp(val, "ALL") == 0) {
				/* not in discovery session */
				data_len = iscsi_append_text("SendTargets", "Reject",
							     data, alloc_len, data_len);
			} else {
				data_len = iscsi_send_tgts(conn,
							   conn->initiator_name,
							   val, data, alloc_len,
							   data_len);
			}
		}

		if (conn->send_tgt_completed_size != 0) {
			F_bit = 0;
			C_bit = 1;
		}
	} else {
		if (iscsi_param_eq_val(conn->sess->params, "SessionType", "Discovery")) {
			iscsi_param_free(params);
			free(data);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	if (spdk_likely(conn->send_tgt_completed_size == 0)) {
		iscsi_param_free(params);
	} else {
		conn->params_text = params;
	}
	SPDK_LOGDUMP(iscsi, "Negotiated Params", data, data_len);

	/* response PDU */
	rsp_pdu = iscsi_get_pdu(conn);
	if (rsp_pdu == NULL) {
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

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_text_pdu_complete, conn);
	return 0;
}

static void iscsi_conn_logout_pdu_complete(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	if (conn->sess == NULL) {
		/*
		 * login failed but initiator still sent a logout rather than
		 *  just closing the TCP connection.
		 */
		SPDK_DEBUGLOG(iscsi, "Logout(login failed) from %s (%s) on"
			      " (%s:%s,%d)\n",
			      conn->initiator_name, conn->initiator_addr,
			      conn->portal_host, conn->portal_port, conn->pg_tag);
	} else if (iscsi_param_eq_val(conn->sess->params, "SessionType", "Normal")) {
		SPDK_DEBUGLOG(iscsi, "Logout from %s (%s) on %s tgt_node%d"
			      " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			      " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			      conn->initiator_name, conn->initiator_addr,
			      conn->target->name, conn->target->num,
			      conn->portal_host, conn->portal_port, conn->pg_tag,
			      conn->sess->isid, conn->sess->tsih, conn->cid,
			      (iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
			       ? "on" : "off"),
			      (iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
			       ? "on" : "off"));
	} else {
		/* discovery session */
		SPDK_DEBUGLOG(iscsi, "Logout(discovery) from %s (%s) on"
			      " (%s:%s,%d), ISID=%"PRIx64", TSIH=%u,"
			      " CID=%u, HeaderDigest=%s, DataDigest=%s\n",
			      conn->initiator_name, conn->initiator_addr,
			      conn->portal_host, conn->portal_port, conn->pg_tag,
			      conn->sess->isid, conn->sess->tsih, conn->cid,
			      (iscsi_param_eq_val(conn->params, "HeaderDigest", "CRC32C")
			       ? "on" : "off"),
			      (iscsi_param_eq_val(conn->params, "DataDigest", "CRC32C")
			       ? "on" : "off"));
	}
}

static int
iscsi_pdu_hdr_op_logout(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	uint32_t task_tag;
	uint32_t ExpStatSN;
	int response;
	struct iscsi_bhs_logout_req *reqh;
	struct iscsi_bhs_logout_resp *rsph;
	uint16_t cid;

	reqh = (struct iscsi_bhs_logout_req *)&pdu->bhs;

	cid = from_be16(&reqh->cid);
	task_tag = from_be32(&reqh->itt);
	ExpStatSN = from_be32(&reqh->exp_stat_sn);

	SPDK_DEBUGLOG(iscsi, "reason=%d, ITT=%x, cid=%d\n",
		      reqh->reason, task_tag, cid);

	if (conn->sess != NULL) {
		if (conn->sess->session_type == SESSION_TYPE_DISCOVERY &&
		    reqh->reason != ISCSI_LOGOUT_REASON_CLOSE_SESSION) {
			SPDK_ERRLOG("Target can accept logout only with reason \"close the session\" "
				    "on discovery session. %d is not acceptable reason.\n",
				    reqh->reason);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}

		SPDK_DEBUGLOG(iscsi,
			      "CmdSN=%u, ExpStatSN=%u, StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
			      pdu->cmd_sn, ExpStatSN, conn->StatSN,
			      conn->sess->ExpCmdSN, conn->sess->MaxCmdSN);

		if (pdu->cmd_sn != conn->sess->ExpCmdSN) {
			SPDK_DEBUGLOG(iscsi, "CmdSN(%u) might have dropped\n", pdu->cmd_sn);
			/* ignore error */
		}
	} else {
		SPDK_DEBUGLOG(iscsi, "CmdSN=%u, ExpStatSN=%u, StatSN=%u\n",
			      pdu->cmd_sn, ExpStatSN, conn->StatSN);
	}

	if (ExpStatSN != conn->StatSN) {
		SPDK_DEBUGLOG(iscsi, "StatSN(%u/%u) might have dropped\n",
			      ExpStatSN, conn->StatSN);
		/* ignore error */
	}

	if (conn->id == cid) {
		/* connection or session closed successfully */
		response = 0;
		iscsi_conn_logout(conn);
	} else {
		response = 1;
	}

	/* response PDU */
	rsp_pdu = iscsi_get_pdu(conn);
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
		to_be32(&rsph->exp_cmd_sn, pdu->cmd_sn);
		to_be32(&rsph->max_cmd_sn, pdu->cmd_sn);
	}

	rsph->time_2_wait = 0;
	rsph->time_2_retain = 0;

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_logout_pdu_complete, conn);

	return 0;
}

static int
iscsi_send_r2t(struct spdk_iscsi_conn *conn,
	       struct spdk_iscsi_task *task, int offset,
	       int len, uint32_t transfer_tag, uint32_t *R2TSN)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_r2t *rsph;
	uint64_t fmt_lun;

	/* R2T PDU */
	rsp_pdu = iscsi_get_pdu(conn);
	if (rsp_pdu == NULL) {
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	rsph = (struct iscsi_bhs_r2t *)&rsp_pdu->bhs;
	rsp_pdu->data = NULL;
	rsph->opcode = ISCSI_OP_R2T;
	rsph->flags |= 0x80; /* bit 0 is default to 1 */
	fmt_lun = spdk_scsi_lun_id_int_to_fmt(task->lun_id);
	to_be64(&rsph->lun, fmt_lun);
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

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);

	return 0;
}

/* This function is used to remove the r2t pdu from snack_pdu_list by < task, r2t_sn> info */
static struct spdk_iscsi_pdu *
iscsi_remove_r2t_pdu_from_snack_list(struct spdk_iscsi_conn *conn,
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
iscsi_send_r2t_recovery(struct spdk_iscsi_conn *conn,
			struct spdk_iscsi_task *task, uint32_t r2t_sn,
			bool send_new_r2tsn)
{
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_r2t *rsph;
	uint32_t transfer_len;
	uint32_t len;
	int rc;

	/* remove the r2t pdu from the snack_list */
	pdu = iscsi_remove_r2t_pdu_from_snack_list(conn, task, r2t_sn);
	if (!pdu) {
		SPDK_DEBUGLOG(iscsi, "No pdu is found\n");
		return -1;
	}

	/* flag
	 * false: only need to re-send the old r2t with changing statsn
	 * true: we send a r2t with new r2tsn
	 */
	if (!send_new_r2tsn) {
		to_be32(&pdu->bhs.stat_sn, conn->StatSN);
		iscsi_conn_write_pdu(conn, pdu, iscsi_conn_pdu_generic_complete, NULL);
	} else {
		rsph = (struct iscsi_bhs_r2t *)&pdu->bhs;
		transfer_len = from_be32(&rsph->desired_xfer_len);

		/* still need to increase the acked r2tsn */
		task->acked_r2tsn++;
		len = spdk_min(conn->sess->MaxBurstLength,
			       (transfer_len - task->next_expected_r2t_offset));

		/* remove the old_r2t_pdu */
		iscsi_conn_free_pdu(conn, pdu);

		/* re-send a new r2t pdu */
		rc = iscsi_send_r2t(conn, task, task->next_expected_r2t_offset,
				    len, task->ttt, &task->R2TSN);
		if (rc < 0) {
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
	}

	return 0;
}

static int
add_transfer_task(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task)
{
	uint32_t transfer_len;
	size_t max_burst_len;
	size_t segment_len;
	size_t data_len;
	int len;
	int rc;
	int data_out_req;

	transfer_len = task->scsi.transfer_len;
	data_len = iscsi_task_get_pdu(task)->data_segment_len;
	max_burst_len = conn->sess->MaxBurstLength;
	segment_len = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	data_out_req = 1 + (transfer_len - data_len - 1) / segment_len;
	task->data_out_cnt = data_out_req;

	/*
	 * If we already have too many tasks using R2T, then queue this task
	 *  and start sending R2T for it after some of the tasks using R2T/data
	 *  out buffers complete.
	 */
	if (conn->pending_r2t >= g_iscsi.MaxR2TPerConnection) {
		TAILQ_INSERT_TAIL(&conn->queued_r2t_tasks, task, link);
		return 0;
	}

	conn->data_out_cnt += data_out_req;
	conn->pending_r2t++;

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
		len = spdk_min(max_burst_len, (transfer_len - data_len));
		rc = iscsi_send_r2t(conn, task, data_len, len,
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
	task->is_r2t_active = true;
	return 0;
}

/* If there are additional large writes queued for R2Ts, start them now.
 *  This is called when a large write is just completed or when multiple LUNs
 *  are attached and large write tasks for the specific LUN are cleared.
 */
static void
start_queued_transfer_tasks(struct spdk_iscsi_conn *conn)
{
	struct spdk_iscsi_task *task, *tmp;

	TAILQ_FOREACH_SAFE(task, &conn->queued_r2t_tasks, link, tmp) {
		if (conn->pending_r2t < g_iscsi.MaxR2TPerConnection) {
			TAILQ_REMOVE(&conn->queued_r2t_tasks, task, link);
			add_transfer_task(conn, task);
		} else {
			break;
		}
	}
}

bool
iscsi_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t task_tag)
{
	struct spdk_iscsi_task *task, *tmp;

	TAILQ_FOREACH_SAFE(task, &conn->active_r2t_tasks, link, tmp) {
		if (task->tag == task_tag) {
			assert(conn->data_out_cnt >= task->data_out_cnt);
			conn->data_out_cnt -= task->data_out_cnt;

			assert(conn->pending_r2t > 0);
			conn->pending_r2t--;

			assert(task->is_r2t_active == true);
			TAILQ_REMOVE(&conn->active_r2t_tasks, task, link);
			task->is_r2t_active = false;
			iscsi_task_put(task);

			start_queued_transfer_tasks(conn);
			return true;
		}
	}
	return false;
}

void iscsi_clear_all_transfer_task(struct spdk_iscsi_conn *conn,
				   struct spdk_scsi_lun *lun,
				   struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task *task, *task_tmp;
	struct spdk_iscsi_pdu *pdu_tmp;

	TAILQ_FOREACH_SAFE(task, &conn->active_r2t_tasks, link, task_tmp) {
		pdu_tmp = iscsi_task_get_pdu(task);
		if ((lun == NULL || lun == task->scsi.lun) &&
		    (pdu == NULL || spdk_sn32_lt(pdu_tmp->cmd_sn, pdu->cmd_sn))) {
			task->outstanding_r2t = 0;
			task->next_r2t_offset = 0;
			task->next_expected_r2t_offset = 0;
			assert(conn->data_out_cnt >= task->data_out_cnt);
			conn->data_out_cnt -= task->data_out_cnt;
			assert(conn->pending_r2t > 0);
			conn->pending_r2t--;

			TAILQ_REMOVE(&conn->active_r2t_tasks, task, link);
			task->is_r2t_active = false;
			if (lun != NULL && spdk_scsi_lun_is_removing(lun)) {
				spdk_scsi_task_process_null_lun(&task->scsi);
				iscsi_task_response(conn, task);
			}
			iscsi_task_put(task);
		}
	}

	TAILQ_FOREACH_SAFE(task, &conn->queued_r2t_tasks, link, task_tmp) {
		pdu_tmp = iscsi_task_get_pdu(task);
		if ((lun == NULL || lun == task->scsi.lun) &&
		    (pdu == NULL || spdk_sn32_lt(pdu_tmp->cmd_sn, pdu->cmd_sn))) {
			TAILQ_REMOVE(&conn->queued_r2t_tasks, task, link);
			task->is_r2t_active = false;
			if (lun != NULL && spdk_scsi_lun_is_removing(lun)) {
				spdk_scsi_task_process_null_lun(&task->scsi);
				iscsi_task_response(conn, task);
			}
			iscsi_task_put(task);
		}
	}

	start_queued_transfer_tasks(conn);
}

static struct spdk_iscsi_task *
get_transfer_task(struct spdk_iscsi_conn *conn, uint32_t transfer_tag)
{
	struct spdk_iscsi_task *task;

	TAILQ_FOREACH(task, &conn->active_r2t_tasks, link) {
		if (task->ttt == transfer_tag) {
			return task;
		}
	}

	return NULL;
}

static void
iscsi_conn_datain_pdu_complete(void *arg)
{
	struct spdk_iscsi_conn *conn = arg;

	iscsi_conn_handle_queued_datain_tasks(conn);
}

static int
iscsi_send_datain(struct spdk_iscsi_conn *conn,
		  struct spdk_iscsi_task *task, int datain_flag,
		  int residual_len, int offset, int DataSN, int len)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_data_in *rsph;
	uint32_t task_tag;
	uint32_t transfer_tag;
	int F_bit, U_bit, O_bit, S_bit;
	struct spdk_iscsi_task *primary;
	struct spdk_scsi_lun *lun_dev;

	primary = iscsi_task_get_primary(task);

	/* DATA PDU */
	rsp_pdu = iscsi_get_pdu(conn);
	rsph = (struct iscsi_bhs_data_in *)&rsp_pdu->bhs;
	rsp_pdu->data = task->scsi.iovs[0].iov_base + offset;
	rsp_pdu->data_buf_len = task->scsi.iovs[0].iov_len - offset;
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

	if (F_bit && S_bit && !iscsi_task_is_immediate(primary)) {
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
	task->scsi.offset = offset;

	if (F_bit && S_bit) {
		to_be32(&rsph->res_cnt, residual_len);
	}

	lun_dev = spdk_scsi_dev_get_lun(conn->dev, task->lun_id);
	if (spdk_likely(lun_dev != NULL)) {
		if (spdk_unlikely(spdk_scsi_lun_get_dif_ctx(lun_dev, &task->scsi,
				  &rsp_pdu->dif_ctx))) {
			rsp_pdu->dif_insert_or_strip = true;
		}
	}

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_datain_pdu_complete, conn);

	return DataSN;
}

static int
iscsi_transfer_in(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task)
{
	uint32_t DataSN;
	uint32_t transfer_len;
	uint32_t data_len;
	uint32_t segment_len;
	uint32_t offset;
	uint32_t residual_len = 0;
	int sent_status;
	uint32_t len;
	int datain_flag = 0;
	int datain_seq_cnt;
	int i;
	uint32_t sequence_end;
	struct spdk_iscsi_task *primary;

	primary = iscsi_task_get_primary(task);
	segment_len = conn->MaxRecvDataSegmentLength;
	data_len = task->scsi.data_transferred;
	transfer_len = task->scsi.length;

	if (task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		return 0;
	}

	if (data_len < transfer_len) {
		/* underflow */
		SPDK_DEBUGLOG(iscsi, "Underflow %u/%u\n", data_len, transfer_len);
		residual_len = transfer_len - data_len;
		transfer_len = data_len;
		datain_flag |= ISCSI_DATAIN_UNDERFLOW;
	} else if (data_len > transfer_len) {
		/* overflow */
		SPDK_DEBUGLOG(iscsi, "Overflow %u/%u\n", data_len, transfer_len);
		residual_len = data_len - transfer_len;
		datain_flag |= ISCSI_DATAIN_OVERFLOW;
	} else {
		SPDK_DEBUGLOG(iscsi, "Transfer %u\n", transfer_len);
		residual_len = 0;
	}

	DataSN = primary->datain_datasn;
	sent_status = 0;

	/* calculate the number of sequences for all data-in pdus */
	datain_seq_cnt = 1 + ((transfer_len - 1) / (int)conn->sess->MaxBurstLength);
	for (i = 0; i < datain_seq_cnt; i++) {
		offset = i * conn->sess->MaxBurstLength;
		sequence_end = spdk_min(((i + 1) * conn->sess->MaxBurstLength),
					transfer_len);

		/* send data splitted by segment_len */
		for (; offset < sequence_end; offset += segment_len) {
			len = spdk_min(segment_len, (sequence_end - offset));

			datain_flag &= ~(ISCSI_FLAG_FINAL | ISCSI_DATAIN_STATUS);

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

			SPDK_DEBUGLOG(iscsi, "Transfer=%d, Offset=%d, Len=%d\n",
				      sequence_end, offset, len);
			SPDK_DEBUGLOG(iscsi, "StatSN=%u, DataSN=%u, Offset=%u, Len=%d\n",
				      conn->StatSN, DataSN, offset, len);

			DataSN = iscsi_send_datain(conn, task, datain_flag, residual_len,
						   offset, DataSN, len);
		}
	}

	if (task != primary) {
		primary->scsi.data_transferred += task->scsi.data_transferred;
	}
	primary->datain_datasn = DataSN;

	return sent_status;
}

void iscsi_task_response(struct spdk_iscsi_conn *conn,
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

	primary = iscsi_task_get_primary(task);

	transfer_len = primary->scsi.transfer_len;
	task_tag = task->tag;

	/* transfer data from logical unit */
	/* (direction is view of initiator side) */
	if (iscsi_task_is_read(primary)) {
		rc = iscsi_transfer_in(conn, task);
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
			SPDK_DEBUGLOG(iscsi, "Underflow %zu/%u\n", data_len, transfer_len);
			residual_len = transfer_len - data_len;
			U_bit = 1;
		} else if (data_len > transfer_len) {
			/* overflow */
			SPDK_DEBUGLOG(iscsi, "Overflow %zu/%u\n", data_len, transfer_len);
			residual_len = data_len - transfer_len;
			O_bit = 1;
		} else {
			SPDK_DEBUGLOG(iscsi, "Transfer %u\n", transfer_len);
		}
	}

	/* response PDU */
	rsp_pdu = iscsi_get_pdu(conn);
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

	if (!iscsi_task_is_immediate(primary)) {
		conn->sess->MaxCmdSN++;
	}

	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	to_be32(&rsph->bi_read_res_cnt, 0);
	to_be32(&rsph->res_cnt, residual_len);

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);
}

/*
 *  This function compare the input pdu's bhs with the pdu's bhs associated by
 *  active_r2t_tasks and queued_r2t_tasks in a connection
 */
static bool
iscsi_compare_pdu_bhs_within_existed_r2t_tasks(struct spdk_iscsi_conn *conn,
		struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task	*task;

	TAILQ_FOREACH(task, &conn->active_r2t_tasks, link) {
		if (!memcmp(&pdu->bhs, iscsi_task_get_bhs(task), ISCSI_BHS_LEN)) {
			return true;
		}
	}

	TAILQ_FOREACH(task, &conn->queued_r2t_tasks, link) {
		if (!memcmp(&pdu->bhs, iscsi_task_get_bhs(task), ISCSI_BHS_LEN)) {
			return true;
		}
	}

	return false;
}

void
iscsi_queue_task(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task)
{
	spdk_trace_record(TRACE_ISCSI_TASK_QUEUE, conn->id, task->scsi.length,
			  (uintptr_t)task, (uintptr_t)task->pdu);
	task->is_queued = true;
	spdk_scsi_dev_queue_task(conn->dev, &task->scsi);
}

static int
iscsi_pdu_payload_op_scsi_read(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task)
{
	if (task->scsi.transfer_len <= SPDK_BDEV_LARGE_BUF_MAX_SIZE) {
		task->parent = NULL;
		task->scsi.offset = 0;
		task->scsi.length = task->scsi.transfer_len;
		spdk_scsi_task_set_data(&task->scsi, NULL, 0);

		iscsi_queue_task(conn, task);
		return 0;
	} else {
		TAILQ_INIT(&task->subtask_list);
		task->current_datain_offset = 0;
		TAILQ_INSERT_TAIL(&conn->queued_datain_tasks, task, link);

		return iscsi_conn_handle_queued_datain_tasks(conn);
	}
}

static int
iscsi_pdu_payload_op_scsi_write(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task)
{
	struct spdk_iscsi_pdu *pdu;
	struct iscsi_bhs_scsi_req *reqh;
	uint32_t transfer_len;
	uint32_t scsi_data_len;
	int rc;

	pdu = iscsi_task_get_pdu(task);
	reqh = (struct iscsi_bhs_scsi_req *)&pdu->bhs;

	transfer_len = task->scsi.transfer_len;

	if (spdk_likely(!pdu->dif_insert_or_strip)) {
		scsi_data_len = pdu->data_segment_len;
	} else {
		scsi_data_len = pdu->data_buf_len;
	}

	if (reqh->final_bit &&
	    pdu->data_segment_len < transfer_len) {
		/* needs R2T */
		rc = add_transfer_task(conn, task);
		if (rc < 0) {
			SPDK_ERRLOG("add_transfer_task() failed\n");
			iscsi_task_put(task);
			return SPDK_ISCSI_CONNECTION_FATAL;
		}

		/* Non-immediate writes */
		if (pdu->data_segment_len == 0) {
			return 0;
		} else {
			/* we are doing the first partial write task */
			task->scsi.ref++;
			spdk_scsi_task_set_data(&task->scsi, pdu->data, scsi_data_len);
			task->scsi.length = pdu->data_segment_len;
		}
	}

	if (pdu->data_segment_len == transfer_len) {
		/* we are doing small writes with no R2T */
		spdk_scsi_task_set_data(&task->scsi, pdu->data, scsi_data_len);
		task->scsi.length = transfer_len;
	}

	iscsi_queue_task(conn, task);
	return 0;
}

static int
iscsi_pdu_hdr_op_scsi(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task	*task;
	struct spdk_scsi_dev	*dev;
	uint8_t *cdb;
	uint64_t lun;
	uint32_t task_tag;
	uint32_t transfer_len;
	int R_bit, W_bit;
	int lun_i;
	struct iscsi_bhs_scsi_req *reqh;

	if (conn->sess->session_type != SESSION_TYPE_NORMAL) {
		SPDK_ERRLOG("ISCSI_OP_SCSI not allowed in discovery and invalid session\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_scsi_req *)&pdu->bhs;

	R_bit = reqh->read_bit;
	W_bit = reqh->write_bit;
	lun = from_be64(&reqh->lun);
	task_tag = from_be32(&reqh->itt);
	transfer_len = from_be32(&reqh->expected_data_xfer_len);
	cdb = reqh->cdb;

	SPDK_LOGDUMP(iscsi, "CDB", cdb, 16);

	task = iscsi_task_get(conn, NULL, iscsi_task_cpl);
	if (!task) {
		SPDK_ERRLOG("Unable to acquire task\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	iscsi_task_associate_pdu(task, pdu);
	lun_i = spdk_scsi_lun_id_fmt_to_int(lun);
	task->lun_id = lun_i;
	dev = conn->dev;
	task->scsi.lun = spdk_scsi_dev_get_lun(dev, lun_i);

	if ((R_bit != 0) && (W_bit != 0)) {
		SPDK_ERRLOG("Bidirectional CDB is not supported\n");
		iscsi_task_put(task);
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	task->scsi.cdb = cdb;
	task->tag = task_tag;
	task->scsi.transfer_len = transfer_len;
	task->scsi.target_port = conn->target_port;
	task->scsi.initiator_port = conn->initiator_port;
	task->parent = NULL;
	task->rsp_scsi_status = SPDK_SCSI_STATUS_GOOD;

	if (task->scsi.lun == NULL) {
		spdk_scsi_task_process_null_lun(&task->scsi);
		iscsi_task_cpl(&task->scsi);
		return 0;
	}

	/* no bi-directional support */
	if (R_bit) {
		task->scsi.dxfer_dir = SPDK_SCSI_DIR_FROM_DEV;
	} else if (W_bit) {
		task->scsi.dxfer_dir = SPDK_SCSI_DIR_TO_DEV;

		if ((conn->sess->ErrorRecoveryLevel >= 1) &&
		    (iscsi_compare_pdu_bhs_within_existed_r2t_tasks(conn, pdu))) {
			iscsi_task_response(conn, task);
			iscsi_task_put(task);
			return 0;
		}

		if (pdu->data_segment_len > iscsi_get_max_immediate_data_size()) {
			SPDK_ERRLOG("data segment len(=%zu) > immediate data len(=%"PRIu32")\n",
				    pdu->data_segment_len, iscsi_get_max_immediate_data_size());
			iscsi_task_put(task);
			return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		}

		if (pdu->data_segment_len > transfer_len) {
			SPDK_ERRLOG("data segment len(=%zu) > task transfer len(=%d)\n",
				    pdu->data_segment_len, transfer_len);
			iscsi_task_put(task);
			return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		}

		/* check the ImmediateData and also pdu->data_segment_len */
		if ((!conn->sess->ImmediateData && (pdu->data_segment_len > 0)) ||
		    (pdu->data_segment_len > conn->sess->FirstBurstLength)) {
			iscsi_task_put(task);
			return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		}

		if (spdk_unlikely(spdk_scsi_lun_get_dif_ctx(task->scsi.lun, &task->scsi, &pdu->dif_ctx))) {
			pdu->dif_insert_or_strip = true;
		}
	} else {
		/* neither R nor W bit set */
		task->scsi.dxfer_dir = SPDK_SCSI_DIR_NONE;
		if (transfer_len > 0) {
			iscsi_task_put(task);
			SPDK_ERRLOG("Reject scsi cmd with EDTL > 0 but (R | W) == 0\n");
			return iscsi_reject(conn, pdu, ISCSI_REASON_INVALID_PDU_FIELD);
		}
	}

	pdu->task = task;
	return 0;
}

static int
iscsi_pdu_payload_op_scsi(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task *task;

	if (pdu->task == NULL) {
		return 0;
	}

	task = pdu->task;

	if (spdk_scsi_dev_get_lun(conn->dev, task->lun_id) == NULL) {
		spdk_scsi_task_process_null_lun(&task->scsi);
		iscsi_task_cpl(&task->scsi);
		return 0;
	}

	switch (task->scsi.dxfer_dir) {
	case SPDK_SCSI_DIR_FROM_DEV:
		return iscsi_pdu_payload_op_scsi_read(conn, task);
	case SPDK_SCSI_DIR_TO_DEV:
		return iscsi_pdu_payload_op_scsi_write(conn, task);
	case SPDK_SCSI_DIR_NONE:
		iscsi_queue_task(conn, task);
		return 0;
	default:
		assert(false);
		iscsi_task_put(task);
		break;
	}

	return SPDK_ISCSI_CONNECTION_FATAL;
}

void
iscsi_task_mgmt_response(struct spdk_iscsi_conn *conn,
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
	rsp_pdu = iscsi_get_pdu(conn);
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

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);
}

static void
iscsi_queue_mgmt_task(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task)
{
	struct spdk_scsi_lun *lun;

	lun = spdk_scsi_dev_get_lun(conn->dev, task->lun_id);
	if (lun == NULL) {
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN;
		iscsi_task_mgmt_response(conn, task);
		iscsi_task_put(task);
		return;
	}

	spdk_scsi_dev_queue_mgmt_task(conn->dev, &task->scsi);
}

static int
_iscsi_op_abort_task(void *arg)
{
	struct spdk_iscsi_task *task = arg;
	int rc;

	rc = iscsi_conn_abort_queued_datain_task(task->conn, task->scsi.abort_id);
	if (rc != 0) {
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&task->mgmt_poller);
	iscsi_queue_mgmt_task(task->conn, task);
	return SPDK_POLLER_BUSY;
}

static void
iscsi_op_abort_task(struct spdk_iscsi_task *task, uint32_t ref_task_tag)
{
	task->scsi.abort_id = ref_task_tag;
	task->scsi.function = SPDK_SCSI_TASK_FUNC_ABORT_TASK;
	task->mgmt_poller = SPDK_POLLER_REGISTER(_iscsi_op_abort_task, task, 10);
}

static int
_iscsi_op_abort_task_set(void *arg)
{
	struct spdk_iscsi_task *task = arg;
	int rc;

	rc = iscsi_conn_abort_queued_datain_tasks(task->conn, task->scsi.lun,
			task->pdu);
	if (rc != 0) {
		return SPDK_POLLER_BUSY;
	}

	spdk_poller_unregister(&task->mgmt_poller);
	iscsi_queue_mgmt_task(task->conn, task);
	return SPDK_POLLER_BUSY;
}

void
iscsi_op_abort_task_set(struct spdk_iscsi_task *task, uint8_t function)
{
	task->scsi.function = function;
	task->mgmt_poller = SPDK_POLLER_REGISTER(_iscsi_op_abort_task_set, task, 10);
}

static int
iscsi_pdu_hdr_op_task(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
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

	SPDK_DEBUGLOG(iscsi, "I=%d, func=%d, ITT=%x, ref TT=%x, LUN=0x%16.16"PRIx64"\n",
		      reqh->immediate, function, task_tag, ref_task_tag, lun);

	SPDK_DEBUGLOG(iscsi, "StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      conn->StatSN, conn->sess->ExpCmdSN, conn->sess->MaxCmdSN);

	lun_i = spdk_scsi_lun_id_fmt_to_int(lun);
	dev = conn->dev;

	task = iscsi_task_get(conn, NULL, iscsi_task_mgmt_cpl);
	if (!task) {
		SPDK_ERRLOG("Unable to acquire task\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	iscsi_task_associate_pdu(task, pdu);
	task->scsi.target_port = conn->target_port;
	task->scsi.initiator_port = conn->initiator_port;
	task->tag = task_tag;
	task->scsi.lun = spdk_scsi_dev_get_lun(dev, lun_i);
	task->lun_id = lun_i;

	if (task->scsi.lun == NULL) {
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_INVALID_LUN;
		iscsi_task_mgmt_response(conn, task);
		iscsi_task_put(task);
		return 0;
	}

	switch (function) {
	/* abort task identified by Referenced Task Tag field */
	case ISCSI_TASK_FUNC_ABORT_TASK:
		SPDK_NOTICELOG("ABORT_TASK\n");

		iscsi_del_transfer_task(conn, ref_task_tag);
		iscsi_op_abort_task(task, ref_task_tag);
		return 0;

	/* abort all tasks issued via this session on the LUN */
	case ISCSI_TASK_FUNC_ABORT_TASK_SET:
		SPDK_NOTICELOG("ABORT_TASK_SET\n");

		iscsi_clear_all_transfer_task(conn, task->scsi.lun, pdu);
		iscsi_op_abort_task_set(task, SPDK_SCSI_TASK_FUNC_ABORT_TASK_SET);
		return 0;

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

		iscsi_clear_all_transfer_task(conn, task->scsi.lun, pdu);
		iscsi_op_abort_task_set(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
		return 0;

	case ISCSI_TASK_FUNC_TARGET_WARM_RESET:
		SPDK_NOTICELOG("TARGET_WARM_RESET (Unsupported)\n");
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
		break;

	case ISCSI_TASK_FUNC_TARGET_COLD_RESET:
		SPDK_NOTICELOG("TARGET_COLD_RESET (Unsupported)\n");
		task->scsi.response = SPDK_SCSI_TASK_MGMT_RESP_REJECT_FUNC_NOT_SUPPORTED;
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

	iscsi_task_mgmt_response(conn, task);
	iscsi_task_put(task);
	return 0;
}

static int
iscsi_pdu_hdr_op_nopout(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct iscsi_bhs_nop_out *reqh;
	uint32_t task_tag;
	uint32_t transfer_tag;
	int I_bit;

	if (conn->sess->session_type == SESSION_TYPE_DISCOVERY) {
		SPDK_ERRLOG("ISCSI_OP_NOPOUT not allowed in discovery session\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	reqh = (struct iscsi_bhs_nop_out *)&pdu->bhs;
	I_bit = reqh->immediate;

	if (pdu->data_segment_len > SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH) {
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	task_tag = from_be32(&reqh->itt);
	transfer_tag = from_be32(&reqh->ttt);

	SPDK_DEBUGLOG(iscsi, "I=%d, ITT=%x, TTT=%x\n",
		      I_bit, task_tag, transfer_tag);

	SPDK_DEBUGLOG(iscsi, "CmdSN=%u, StatSN=%u, ExpCmdSN=%u, MaxCmdSN=%u\n",
		      pdu->cmd_sn, conn->StatSN, conn->sess->ExpCmdSN,
		      conn->sess->MaxCmdSN);

	if (transfer_tag != 0xFFFFFFFF && transfer_tag != (uint32_t)conn->id) {
		SPDK_ERRLOG("invalid transfer tag 0x%x\n", transfer_tag);
		/*
		 * Technically we should probably fail the connection here, but for now
		 *  just print the error message and continue.
		 */
	}

	if (task_tag == 0xffffffffU && I_bit == 0) {
		SPDK_ERRLOG("got NOPOUT ITT=0xffffffff, I=0\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	return 0;
}

static int
iscsi_pdu_payload_op_nopout(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_nop_out *reqh;
	struct iscsi_bhs_nop_in *rsph;
	uint8_t *data;
	uint64_t lun;
	uint32_t task_tag;
	int I_bit;
	int data_len;

	reqh = (struct iscsi_bhs_nop_out *)&pdu->bhs;
	I_bit = reqh->immediate;

	data_len = pdu->data_segment_len;
	if (data_len > conn->MaxRecvDataSegmentLength) {
		data_len = conn->MaxRecvDataSegmentLength;
	}

	lun = from_be64(&reqh->lun);
	task_tag = from_be32(&reqh->itt);

	/*
	 * We don't actually check to see if this is a response to the NOP-In
	 * that we sent.  Our goal is to just verify that the initiator is
	 * alive and responding to commands, not to verify that it tags
	 * NOP-Outs correctly
	 */
	conn->nop_outstanding = false;

	if (task_tag == 0xffffffffU) {
		assert(I_bit == 1);
		SPDK_DEBUGLOG(iscsi, "got NOPOUT ITT=0xffffffff\n");
		return 0;
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

	/* response PDU */
	rsp_pdu = iscsi_get_pdu(conn);
	assert(rsp_pdu != NULL);

	rsph = (struct iscsi_bhs_nop_in *)&rsp_pdu->bhs;
	rsp_pdu->data = data;
	rsph->opcode = ISCSI_OP_NOPIN;
	rsph->flags |= 0x80; /* bit 0 default to 1 */
	DSET24(rsph->data_segment_len, data_len);
	to_be64(&rsph->lun, lun);
	to_be32(&rsph->itt, task_tag);
	to_be32(&rsph->ttt, 0xffffffffU);

	to_be32(&rsph->stat_sn, conn->StatSN);
	conn->StatSN++;

	if (I_bit == 0) {
		conn->sess->MaxCmdSN++;
	}

	to_be32(&rsph->exp_cmd_sn, conn->sess->ExpCmdSN);
	to_be32(&rsph->max_cmd_sn, conn->sess->MaxCmdSN);

	iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);
	conn->last_nopin = spdk_get_ticks();

	return 0;
}

/* This function returns the spdk_scsi_task by searching the snack list via
 * task transfertag and the pdu's opcode
 */
static struct spdk_iscsi_task *
get_scsi_task_from_ttt(struct spdk_iscsi_conn *conn, uint32_t transfer_tag)
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
get_scsi_task_from_itt(struct spdk_iscsi_conn *conn,
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

/* This function is used to handle the r2t snack */
static int
iscsi_handle_r2t_snack(struct spdk_iscsi_conn *conn,
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
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	if (run_length) {
		if ((beg_run + run_length) > task->R2TSN) {
			SPDK_ERRLOG("ITT: 0x%08x, received R2T SNACK with"
				    "BegRun: 0x%08x, RunLength: 0x%08x, exceeds"
				    "current R2TSN: 0x%08x, protocol error.\n",
				    task_tag, beg_run, run_length,
				    task->R2TSN);

			return iscsi_reject(conn, pdu, ISCSI_REASON_INVALID_PDU_FIELD);
		}
		last_r2tsn = (beg_run + run_length);
	} else {
		last_r2tsn = task->R2TSN;
	}

	for (i = beg_run; i < last_r2tsn; i++) {
		if (iscsi_send_r2t_recovery(conn, task, i, false) < 0) {
			SPDK_ERRLOG("The r2t_sn=%d of r2t_task=%p is not sent\n", i, task);
		}
	}
	return 0;
}

/* This function is used to recover the data in packet */
static int
iscsi_handle_recovery_datain(struct spdk_iscsi_conn *conn,
			     struct spdk_iscsi_task *task,
			     struct spdk_iscsi_pdu *pdu, uint32_t beg_run,
			     uint32_t run_length, uint32_t task_tag)
{
	struct spdk_iscsi_pdu *old_pdu, *pdu_temp;
	uint32_t i;
	struct iscsi_bhs_data_in *datain_header;
	uint32_t last_statsn;

	task = iscsi_task_get_primary(task);

	SPDK_DEBUGLOG(iscsi, "iscsi_handle_recovery_datain\n");

	if (beg_run < task->acked_data_sn) {
		SPDK_ERRLOG("ITT: 0x%08x, DATA IN SNACK requests retransmission of"
			    "DATASN: from 0x%08x to 0x%08x but already acked to "
			    "DATASN: 0x%08x protocol error\n",
			    task_tag, beg_run,
			    (beg_run + run_length), (task->acked_data_sn - 1));

		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
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
					iscsi_conn_write_pdu(conn, old_pdu, old_pdu->cb_fn, old_pdu->cb_arg);
					break;
				}
			}
		}
	}
	return 0;
}

/* This function is used to handle the status snack */
static int
iscsi_handle_status_snack(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
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

	SPDK_DEBUGLOG(iscsi, "beg_run=%d, run_length=%d, conn->StatSN="
		      "%d, conn->exp_statsn=%d\n", beg_run, run_length,
		      conn->StatSN, conn->exp_statsn);

	if (!beg_run) {
		beg_run = conn->exp_statsn;
	} else if (beg_run < conn->exp_statsn) {
		SPDK_ERRLOG("Got Status SNACK Begrun: 0x%08x, RunLength: 0x%08x "
			    "but already got ExpStatSN: 0x%08x on CID:%hu.\n",
			    beg_run, run_length, conn->StatSN, conn->cid);

		return iscsi_reject(conn, pdu, ISCSI_REASON_INVALID_PDU_FIELD);
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
			iscsi_conn_write_pdu(conn, old_pdu, old_pdu->cb_fn, old_pdu->cb_arg);
		}
	}

	return 0;
}

/* This function is used to handle the data ack snack */
static int
iscsi_handle_data_ack(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	uint32_t transfer_tag;
	uint32_t beg_run;
	uint32_t run_length;
	struct spdk_iscsi_pdu *old_pdu;
	uint32_t old_datasn;
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

	SPDK_DEBUGLOG(iscsi, "beg_run=%d,transfer_tag=%d,run_len=%d\n",
		      beg_run, transfer_tag, run_length);

	task = get_scsi_task_from_ttt(conn, transfer_tag);
	if (!task) {
		SPDK_ERRLOG("Data ACK SNACK for TTT: 0x%08x is invalid.\n",
			    transfer_tag);
		goto reject_return;
	}

	primary = iscsi_task_get_primary(task);
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
				iscsi_conn_free_pdu(conn, old_pdu);
				break;
			}
		}
	}

	SPDK_DEBUGLOG(iscsi, "Received Data ACK SNACK for TTT: 0x%08x,"
		      " updated acked DataSN to 0x%08x.\n", transfer_tag,
		      (task->acked_data_sn - 1));

	return 0;

reject_return:
	return iscsi_reject(conn, pdu, ISCSI_REASON_INVALID_SNACK);
}

/* This function is used to handle the snack request from the initiator */
static int
iscsi_pdu_hdr_op_snack(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
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
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	type = reqh->flags & ISCSI_FLAG_SNACK_TYPE_MASK;
	SPDK_DEBUGLOG(iscsi, "The value of type is %d\n", type);

	switch (type) {
	case 0:
		reqh = (struct iscsi_bhs_snack_req *)&pdu->bhs;
		task_tag = from_be32(&reqh->itt);
		beg_run = from_be32(&reqh->beg_run);
		run_length = from_be32(&reqh->run_len);

		SPDK_DEBUGLOG(iscsi, "beg_run=%d, run_length=%d, "
			      "task_tag=%x, transfer_tag=%u\n", beg_run,
			      run_length, task_tag, from_be32(&reqh->ttt));

		task = get_scsi_task_from_itt(conn, task_tag,
					      ISCSI_OP_SCSI_DATAIN);
		if (task) {
			return iscsi_handle_recovery_datain(conn, task, pdu,
							    beg_run, run_length, task_tag);
		}
		task = get_scsi_task_from_itt(conn, task_tag, ISCSI_OP_R2T);
		if (task) {
			return iscsi_handle_r2t_snack(conn, task, pdu, beg_run,
						      run_length, task_tag);
		}
		SPDK_ERRLOG("It is Neither datain nor r2t recovery request\n");
		rc = -1;
		break;
	case ISCSI_FLAG_SNACK_TYPE_STATUS:
		rc = iscsi_handle_status_snack(conn, pdu);
		break;
	case ISCSI_FLAG_SNACK_TYPE_DATA_ACK:
		rc = iscsi_handle_data_ack(conn, pdu);
		break;
	case ISCSI_FLAG_SNACK_TYPE_RDATA:
		SPDK_ERRLOG("R-Data SNACK is Not Supported int spdk\n");
		rc = iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		break;
	default:
		SPDK_ERRLOG("Unknown SNACK type %d, protocol error\n", type);
		rc = iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
		break;
	}

	return rc;
}

static int
iscsi_pdu_hdr_op_data(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
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

	if (pdu->data_segment_len > SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH) {
		reject_reason = ISCSI_REASON_PROTOCOL_ERROR;
		goto reject_return;
	}

	task = get_transfer_task(conn, transfer_tag);
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
		 * This R2T burst is done. Clear the length before we
		 *  receive a PDU for the next R2t burst.
		 */
		task->current_r2t_length = 0;
	}

	subtask = iscsi_task_get(conn, task, iscsi_task_cpl);
	if (subtask == NULL) {
		SPDK_ERRLOG("Unable to acquire subtask\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}
	subtask->scsi.offset = buffer_offset;
	subtask->scsi.length = pdu->data_segment_len;
	iscsi_task_associate_pdu(subtask, pdu);

	if (task->next_expected_r2t_offset == transfer_len) {
		task->acked_r2tsn++;
	} else if (F_bit && (task->next_r2t_offset < transfer_len)) {
		task->acked_r2tsn++;
		len = spdk_min(conn->sess->MaxBurstLength,
			       (transfer_len - task->next_r2t_offset));
		rc = iscsi_send_r2t(conn, task, task->next_r2t_offset, len,
				    task->ttt, &task->R2TSN);
		if (rc < 0) {
			SPDK_ERRLOG("iscsi_send_r2t() failed\n");
		}
		task->next_r2t_offset += len;
	}

	if (lun_dev == NULL) {
		SPDK_DEBUGLOG(iscsi, "LUN %d is removed, complete the task immediately\n",
			      task->lun_id);
		subtask->scsi.transfer_len = subtask->scsi.length;
		spdk_scsi_task_process_null_lun(&subtask->scsi);
		iscsi_task_cpl(&subtask->scsi);
		return 0;
	}

	if (spdk_unlikely(spdk_scsi_lun_get_dif_ctx(lun_dev, &subtask->scsi, &pdu->dif_ctx))) {
		pdu->dif_insert_or_strip = true;
	}

	pdu->task = subtask;
	return 0;

send_r2t_recovery_return:
	rc = iscsi_send_r2t_recovery(conn, task, task->acked_r2tsn, true);
	if (rc == 0) {
		return 0;
	}

reject_return:
	return iscsi_reject(conn, pdu, reject_reason);
}

static int
iscsi_pdu_payload_op_data(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	struct spdk_iscsi_task *subtask;
	struct iscsi_bhs_data_out *reqh;
	uint32_t transfer_tag;

	if (pdu->task == NULL) {
		return 0;
	}

	subtask = pdu->task;

	reqh = (struct iscsi_bhs_data_out *)&pdu->bhs;
	transfer_tag = from_be32(&reqh->ttt);

	if (get_transfer_task(conn, transfer_tag) == NULL) {
		SPDK_ERRLOG("Not found for transfer_tag=%x\n", transfer_tag);
		subtask->scsi.transfer_len = subtask->scsi.length;
		spdk_scsi_task_process_abort(&subtask->scsi);
		iscsi_task_cpl(&subtask->scsi);
		return 0;
	}

	if (spdk_likely(!pdu->dif_insert_or_strip)) {
		spdk_scsi_task_set_data(&subtask->scsi, pdu->data, pdu->data_segment_len);
	} else {
		spdk_scsi_task_set_data(&subtask->scsi, pdu->data, pdu->data_buf_len);
	}

	if (spdk_scsi_dev_get_lun(conn->dev, subtask->lun_id) == NULL) {
		SPDK_DEBUGLOG(iscsi, "LUN %d is removed, complete the task immediately\n",
			      subtask->lun_id);
		subtask->scsi.transfer_len = subtask->scsi.length;
		spdk_scsi_task_process_null_lun(&subtask->scsi);
		iscsi_task_cpl(&subtask->scsi);
		return 0;
	}

	iscsi_queue_task(conn, subtask);
	return 0;
}

static void
init_login_reject_response(struct spdk_iscsi_pdu *pdu, struct spdk_iscsi_pdu *rsp_pdu)
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

static void
iscsi_pdu_dump(struct spdk_iscsi_pdu *pdu)
{
	spdk_log_dump(stderr, "PDU", (uint8_t *)&pdu->bhs, ISCSI_BHS_LEN);
}

/* This function is used to refree the pdu when it is acknowledged */
static void
remove_acked_pdu(struct spdk_iscsi_conn *conn, uint32_t ExpStatSN)
{
	struct spdk_iscsi_pdu *pdu, *pdu_temp;
	uint32_t stat_sn;

	conn->exp_statsn = spdk_min(ExpStatSN, conn->StatSN);
	TAILQ_FOREACH_SAFE(pdu, &conn->snack_pdu_list, tailq, pdu_temp) {
		stat_sn = from_be32(&pdu->bhs.stat_sn);
		if (spdk_sn32_lt(stat_sn, conn->exp_statsn)) {
			TAILQ_REMOVE(&conn->snack_pdu_list, pdu, tailq);
			iscsi_conn_free_pdu(conn, pdu);
		}
	}
}

static int
iscsi_update_cmdsn(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int opcode;
	uint32_t ExpStatSN;
	int I_bit;
	struct spdk_iscsi_sess *sess;
	struct iscsi_bhs_scsi_req *reqh;

	sess = conn->sess;
	if (!sess) {
		SPDK_ERRLOG("Connection has no associated session!\n");
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	opcode = pdu->bhs.opcode;
	reqh = (struct iscsi_bhs_scsi_req *)&pdu->bhs;

	pdu->cmd_sn = from_be32(&reqh->cmd_sn);

	I_bit = reqh->immediate;
	if (I_bit == 0) {
		if (spdk_sn32_lt(pdu->cmd_sn, sess->ExpCmdSN) ||
		    spdk_sn32_gt(pdu->cmd_sn, sess->MaxCmdSN)) {
			if (sess->session_type == SESSION_TYPE_NORMAL &&
			    opcode != ISCSI_OP_SCSI_DATAOUT) {
				SPDK_ERRLOG("CmdSN(%u) ignore (ExpCmdSN=%u, MaxCmdSN=%u)\n",
					    pdu->cmd_sn, sess->ExpCmdSN, sess->MaxCmdSN);

				if (sess->ErrorRecoveryLevel >= 1) {
					SPDK_DEBUGLOG(iscsi, "Skip the error in ERL 1 and 2\n");
				} else {
					return SPDK_PDU_FATAL;
				}
			}
		}
	} else if (pdu->cmd_sn != sess->ExpCmdSN) {
		SPDK_ERRLOG("CmdSN(%u) error ExpCmdSN=%u\n", pdu->cmd_sn, sess->ExpCmdSN);

		if (sess->ErrorRecoveryLevel >= 1) {
			SPDK_DEBUGLOG(iscsi, "Skip the error in ERL 1 and 2\n");
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
	if (spdk_sn32_gt(ExpStatSN, conn->StatSN)) {
		SPDK_DEBUGLOG(iscsi, "StatSN(%u) advanced\n", ExpStatSN);
		ExpStatSN = conn->StatSN;
	}

	if (sess->ErrorRecoveryLevel >= 1) {
		remove_acked_pdu(conn, ExpStatSN);
	}

	if (!I_bit && opcode != ISCSI_OP_SCSI_DATAOUT) {
		sess->ExpCmdSN++;
	}

	return 0;
}

static int
iscsi_pdu_hdr_handle(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int opcode;
	int rc;
	struct spdk_iscsi_pdu *rsp_pdu = NULL;

	if (pdu == NULL) {
		return -1;
	}

	opcode = pdu->bhs.opcode;

	SPDK_DEBUGLOG(iscsi, "opcode %x\n", opcode);

	if (opcode == ISCSI_OP_LOGIN) {
		return iscsi_pdu_hdr_op_login(conn, pdu);
	}

	/* connection in login phase but receive non-login opcode
	 * return response code 0x020b to initiator.
	 * */
	if (!conn->full_feature && conn->state == ISCSI_CONN_STATE_RUNNING) {
		rsp_pdu = iscsi_get_pdu(conn);
		if (rsp_pdu == NULL) {
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
		init_login_reject_response(pdu, rsp_pdu);
		iscsi_conn_write_pdu(conn, rsp_pdu, iscsi_conn_pdu_generic_complete, NULL);
		SPDK_ERRLOG("Received opcode %d in login phase\n", opcode);
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	} else if (conn->state == ISCSI_CONN_STATE_INVALID) {
		SPDK_ERRLOG("before Full Feature\n");
		iscsi_pdu_dump(pdu);
		return SPDK_ISCSI_CONNECTION_FATAL;
	}

	rc = iscsi_update_cmdsn(conn, pdu);
	if (rc != 0) {
		return rc;
	}

	switch (opcode) {
	case ISCSI_OP_NOPOUT:
		rc = iscsi_pdu_hdr_op_nopout(conn, pdu);
		break;

	case ISCSI_OP_SCSI:
		rc = iscsi_pdu_hdr_op_scsi(conn, pdu);
		break;
	case ISCSI_OP_TASK:
		rc = iscsi_pdu_hdr_op_task(conn, pdu);
		break;

	case ISCSI_OP_TEXT:
		rc = iscsi_pdu_hdr_op_text(conn, pdu);
		break;

	case ISCSI_OP_LOGOUT:
		rc = iscsi_pdu_hdr_op_logout(conn, pdu);
		break;

	case ISCSI_OP_SCSI_DATAOUT:
		rc = iscsi_pdu_hdr_op_data(conn, pdu);
		break;

	case ISCSI_OP_SNACK:
		rc = iscsi_pdu_hdr_op_snack(conn, pdu);
		break;

	default:
		SPDK_ERRLOG("unsupported opcode %x\n", opcode);
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	if (rc < 0) {
		SPDK_ERRLOG("processing PDU header (opcode=%x) failed on %s(%s)\n",
			    opcode,
			    conn->target_port != NULL ? spdk_scsi_port_get_name(conn->target_port) : "NULL",
			    conn->initiator_port != NULL ? spdk_scsi_port_get_name(conn->initiator_port) : "NULL");
	}

	return rc;
}

static int
iscsi_pdu_payload_handle(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int opcode;
	int rc = 0;

	opcode = pdu->bhs.opcode;

	SPDK_DEBUGLOG(iscsi, "opcode %x\n", opcode);

	switch (opcode) {
	case ISCSI_OP_LOGIN:
		rc = iscsi_pdu_payload_op_login(conn, pdu);
		break;
	case ISCSI_OP_NOPOUT:
		rc = iscsi_pdu_payload_op_nopout(conn, pdu);
		break;
	case ISCSI_OP_SCSI:
		rc = iscsi_pdu_payload_op_scsi(conn, pdu);
		break;
	case ISCSI_OP_TASK:
		break;
	case ISCSI_OP_TEXT:
		rc = iscsi_pdu_payload_op_text(conn, pdu);
		break;
	case ISCSI_OP_LOGOUT:
		break;
	case ISCSI_OP_SCSI_DATAOUT:
		rc = iscsi_pdu_payload_op_data(conn, pdu);
		break;
	case ISCSI_OP_SNACK:
		break;
	default:
		SPDK_ERRLOG("unsupported opcode %x\n", opcode);
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	if (rc < 0) {
		SPDK_ERRLOG("processing PDU payload (opcode=%x) failed on %s(%s)\n",
			    opcode,
			    conn->target_port != NULL ? spdk_scsi_port_get_name(conn->target_port) : "NULL",
			    conn->initiator_port != NULL ? spdk_scsi_port_get_name(conn->initiator_port) : "NULL");
	}

	return rc;
}

static int
iscsi_read_pdu(struct spdk_iscsi_conn *conn)
{
	enum iscsi_pdu_recv_state prev_state;
	struct spdk_iscsi_pdu *pdu;
	struct spdk_mempool *pool;
	uint32_t crc32c;
	int ahs_len;
	uint32_t data_len;
	int rc;

	do {
		prev_state = conn->pdu_recv_state;
		pdu = conn->pdu_in_progress;

		switch (conn->pdu_recv_state) {
		case ISCSI_PDU_RECV_STATE_AWAIT_PDU_READY:
			assert(conn->pdu_in_progress == NULL);

			conn->pdu_in_progress = iscsi_get_pdu(conn);
			if (conn->pdu_in_progress == NULL) {
				return SPDK_ISCSI_CONNECTION_FATAL;
			}
			conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR;
			break;
		case ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR:
			if (pdu->bhs_valid_bytes < ISCSI_BHS_LEN) {
				rc = iscsi_conn_read_data(conn,
							  ISCSI_BHS_LEN - pdu->bhs_valid_bytes,
							  (uint8_t *)&pdu->bhs + pdu->bhs_valid_bytes);
				if (rc < 0) {
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}
				pdu->bhs_valid_bytes += rc;
				if (pdu->bhs_valid_bytes < ISCSI_BHS_LEN) {
					return 0;
				}
			}

			/* conn->is_logged_out must be checked after completing to process
			 * logout request, i.e., before processing PDU header in this state
			 * machine, otherwise logout response may not be sent to initiator
			 * and initiator may get logout timeout.
			 */
			if (spdk_unlikely(conn->is_logged_out)) {
				SPDK_DEBUGLOG(iscsi, "pdu received after logout\n");
				conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
				break;
			}

			pdu->data_segment_len = ISCSI_ALIGN(DGET24(pdu->bhs.data_segment_len));

			/* AHS */
			ahs_len = pdu->bhs.total_ahs_len * 4;
			if (ahs_len > ISCSI_AHS_LEN) {
				SPDK_DEBUGLOG(iscsi, "pdu ahs length %d is invalid\n", ahs_len);
				conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
				break;
			}

			if (pdu->ahs_valid_bytes < ahs_len) {
				rc = iscsi_conn_read_data(conn,
							  ahs_len - pdu->ahs_valid_bytes,
							  pdu->ahs + pdu->ahs_valid_bytes);
				if (rc < 0) {
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}

				pdu->ahs_valid_bytes += rc;
				if (pdu->ahs_valid_bytes < ahs_len) {
					return 0;
				}
			}

			/* Header Digest */
			if (conn->header_digest &&
			    pdu->hdigest_valid_bytes < ISCSI_DIGEST_LEN) {
				rc = iscsi_conn_read_data(conn,
							  ISCSI_DIGEST_LEN - pdu->hdigest_valid_bytes,
							  pdu->header_digest + pdu->hdigest_valid_bytes);
				if (rc < 0) {
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}

				pdu->hdigest_valid_bytes += rc;
				if (pdu->hdigest_valid_bytes < ISCSI_DIGEST_LEN) {
					return 0;
				}
			}

			if (conn->header_digest) {
				crc32c = iscsi_pdu_calc_header_digest(pdu);
				rc = MATCH_DIGEST_WORD(pdu->header_digest, crc32c);
				if (rc == 0) {
					SPDK_ERRLOG("header digest error (%s)\n", conn->initiator_name);
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}
			}

			rc = iscsi_pdu_hdr_handle(conn, pdu);
			if (rc < 0) {
				SPDK_ERRLOG("Critical error is detected. Close the connection\n");
				conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
				break;
			}

			conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD;
			break;
		case ISCSI_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			data_len = pdu->data_segment_len;

			if (data_len != 0 && pdu->data_buf == NULL) {
				if (data_len <= iscsi_get_max_immediate_data_size()) {
					pool = g_iscsi.pdu_immediate_data_pool;
					pdu->data_buf_len = SPDK_BDEV_BUF_SIZE_WITH_MD(iscsi_get_max_immediate_data_size());
				} else if (data_len <= SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH) {
					pool = g_iscsi.pdu_data_out_pool;
					pdu->data_buf_len = SPDK_BDEV_BUF_SIZE_WITH_MD(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH);
				} else {
					SPDK_ERRLOG("Data(%d) > MaxSegment(%d)\n",
						    data_len, SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH);
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}
				pdu->mobj = spdk_mempool_get(pool);
				if (pdu->mobj == NULL) {
					return 0;
				}
				pdu->data_buf = pdu->mobj->buf;
				pdu->data = pdu->mobj->buf;
				pdu->data_from_mempool = true;
			}

			/* copy the actual data into local buffer */
			if (pdu->data_valid_bytes < data_len) {
				rc = iscsi_conn_read_data_segment(conn, pdu, data_len);
				if (rc < 0) {
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}

				pdu->data_valid_bytes += rc;
				if (pdu->data_valid_bytes < data_len) {
					return 0;
				}
			}

			/* copy out the data digest */
			if (conn->data_digest && data_len != 0 &&
			    pdu->ddigest_valid_bytes < ISCSI_DIGEST_LEN) {
				rc = iscsi_conn_read_data(conn,
							  ISCSI_DIGEST_LEN - pdu->ddigest_valid_bytes,
							  pdu->data_digest + pdu->ddigest_valid_bytes);
				if (rc < 0) {
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}

				pdu->ddigest_valid_bytes += rc;
				if (pdu->ddigest_valid_bytes < ISCSI_DIGEST_LEN) {
					return 0;
				}
			}

			/* All data for this PDU has now been read from the socket. */
			spdk_trace_record(TRACE_ISCSI_READ_PDU, conn->id, pdu->data_valid_bytes,
					  (uintptr_t)pdu, pdu->bhs.opcode);

			/* check data digest */
			if (conn->data_digest && data_len != 0) {
				crc32c = iscsi_pdu_calc_data_digest(pdu);
				rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
				if (rc == 0) {
					SPDK_ERRLOG("data digest error (%s)\n", conn->initiator_name);
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}
			}

			if (!pdu->is_rejected) {
				rc = iscsi_pdu_payload_handle(conn, pdu);
			} else {
				rc = 0;
			}
			if (rc == 0) {
				spdk_trace_record(TRACE_ISCSI_TASK_EXECUTED, 0, 0, (uintptr_t)pdu, 0);
				iscsi_put_pdu(pdu);
				conn->pdu_in_progress = NULL;
				conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_READY;
				return 1;
			} else {
				conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
			}
			break;
		case ISCSI_PDU_RECV_STATE_ERROR:
			return SPDK_ISCSI_CONNECTION_FATAL;
		default:
			assert(false);
			SPDK_ERRLOG("code should not come here\n");
			break;
		}
	} while (prev_state != conn->pdu_recv_state);

	return 0;
}

#define GET_PDU_LOOP_COUNT	16

int
iscsi_handle_incoming_pdus(struct spdk_iscsi_conn *conn)
{
	int i, rc;

	/* Read new PDUs from network */
	for (i = 0; i < GET_PDU_LOOP_COUNT; i++) {
		rc = iscsi_read_pdu(conn);
		if (rc == 0) {
			break;
		} else if (rc < 0) {
			return rc;
		}

		if (conn->is_stopped) {
			break;
		}
	}

	return i;
}
