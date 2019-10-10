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
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/json.h"
#include "fuzz_common.h"
#include "spdk/endian.h"
#include "spdk/bdev.h"
#include "spdk/notify.h"
#include "spdk/scsi.h"
#include "iscsi/conn.h"
#include "iscsi/task.h"
#include "iscsi/iscsi.h"
#include "iscsi/portal_grp.h"
#include "scsi/scsi_internal.h"
#include "spdk_internal/mock.h"
#include "spdk/scsi_spec.h"

#define UNIQUE_OPCODES 256
#define FUZZ_QUEUE_DEPTH	128
#define SCSI_IO_NAME		"scsi_cmd"
#define GET_PDU_LOOP_COUNT	16
#define DMIN32(A,B) ((uint32_t) ((uint32_t)(A) > (uint32_t)(B) ? (uint32_t)(B) : (uint32_t)(A)))

int valid_opcode_list[11] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10, 0x1c, 0x1d, 0x1e};
bool g_successful_io_opcodes[UNIQUE_OPCODES] = {0};

/* Global run state */
uint64_t	g_runtime_ticks;
int			g_runtime;
int			g_num_active_threads;
bool		g_run = true;
bool		g_verbose_mode = false;
bool		g_is_valid_opcode = false;

/* Global resources */
TAILQ_HEAD(, fuzz_iscsi_dev_ctx)	g_dev_list = TAILQ_HEAD_INITIALIZER(g_dev_list);
struct spdk_poller			*g_run_poller;
void					*g_valid_buffer;
unsigned int				g_random_seed;
char					*g_json_file = NULL;
struct spdk_iscsi_globals g_spdk_iscsi;

struct fuzz_iscsi_iov_ctx {
	struct iovec			iov_req;
	struct iovec			iov_data;
	struct iovec			iov_resp;
};

struct fuzz_iscsi_io_ctx {
	struct fuzz_iscsi_iov_ctx		iovs;
	union {
		struct iscsi_bhs_scsi_req	*scsi_req;
	} req;
	union {
		struct iscsi_bhs_scsi_resp	*scsi_resp;
	} resp;

	TAILQ_ENTRY(fuzz_iscsi_io_ctx) link;
};

struct fuzz_iscsi_dev_ctx {
	struct spdk_scsi_dev		*scsi_dev;
	struct spdk_thread			*thread;
	struct spdk_poller			*poller;

	struct fuzz_iscsi_io_ctx		*io_ctx_array;
	TAILQ_HEAD(, fuzz_iscsi_io_ctx)		free_io_ctx;
	TAILQ_HEAD(, fuzz_iscsi_io_ctx)		outstanding_io_ctx;

	unsigned int				random_seed;

	uint64_t				submitted_io;
	uint64_t				completed_io;
	uint64_t				successful_io;
	uint64_t				timeout_tsc;

	bool					valid_lun;
	bool					timed_out;

	TAILQ_ENTRY(fuzz_iscsi_dev_ctx)	link;
};

struct spdk_mempool *spdk_mempool_create1(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int socket_id);
void spdk_put_pdu1(struct spdk_iscsi_pdu *pdu);
struct spdk_iscsi_pdu *spdk_get_pdu1(void);
int spdk_iscsi_execute1(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu);

static void
cleanup(void)
{
	struct fuzz_iscsi_dev_ctx *dev_ctx, *tmp;
	TAILQ_FOREACH_SAFE(dev_ctx, &g_dev_list, link, tmp) {
		printf("device %p stats: Completed I/O: %lu, Successful I/O: %lu\n", dev_ctx,
		       dev_ctx->completed_io, dev_ctx->successful_io);
		free(dev_ctx->scsi_dev);//free_dev  ?????
		free(dev_ctx);
	}

	spdk_free(g_valid_buffer);
}


/* data dumping functions begin */
static int
dump_iscsi_cmd(void *ctx, const void *data, size_t size)
{
	fprintf(stderr, "%s\n", (const char *)data);
	return 0;
}

static void
print_iov_obj(struct spdk_json_write_ctx *w, const char *iov_name, struct iovec *iov)
{
	/* "0x" + up to 16 digits + null terminator */
	char hex_addr[19];
	int rc;

	rc = snprintf(hex_addr, 19, "%lx", (uintptr_t)iov->iov_base);

	/* default to 0. */
	if (rc < 0 || rc >= 19) {
		hex_addr[0] = '0';
		hex_addr[1] = '\0';
	}

	spdk_json_write_named_object_begin(w, iov_name);
	spdk_json_write_named_string(w, "iov_base", hex_addr);
	spdk_json_write_named_uint64(w, "iov_len", iov->iov_len);
	spdk_json_write_object_end(w);
}

static void
print_iovs(struct spdk_json_write_ctx *w, struct fuzz_iscsi_io_ctx *io_ctx)
{
	print_iov_obj(w, "req_iov", &io_ctx->iovs.iov_req);
	print_iov_obj(w, "data_iov", &io_ctx->iovs.iov_data);
	print_iov_obj(w, "resp_iov", &io_ctx->iovs.iov_resp);
}

static void
print_req_obj(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx)
{
	struct spdk_json_write_ctx *w;

	w = spdk_json_write_begin(dump_iscsi_cmd, NULL, SPDK_JSON_WRITE_FLAG_FORMATTED);

	spdk_json_write_named_object_begin(w, SCSI_IO_NAME);
	print_iovs(w, io_ctx);

	spdk_json_write_object_end(w);
	spdk_json_write_end(w);
}

static void
dump_outstanding_io(struct fuzz_iscsi_dev_ctx *dev_ctx)
{
	struct fuzz_iscsi_io_ctx *io_ctx, *tmp;

	TAILQ_FOREACH_SAFE(io_ctx, &dev_ctx->outstanding_io_ctx, link, tmp) {
		print_req_obj(dev_ctx, io_ctx);
		TAILQ_REMOVE(&dev_ctx->outstanding_io_ctx, io_ctx, link);
		TAILQ_INSERT_TAIL(&dev_ctx->free_io_ctx, io_ctx, link);
	}
}
/* data dumping functions end */


/* dev initialization code begin. */
static int
fuzz_iscsi_dev_init(void)
{
	struct fuzz_iscsi_dev_ctx *dev_ctx;
	int rc = 0, i;

	dev_ctx = calloc(1, sizeof(*dev_ctx));
	if (dev_ctx == NULL) {
		return -ENOMEM;
	}

	dev_ctx->valid_lun = 1;
	TAILQ_INIT(&dev_ctx->free_io_ctx);
	TAILQ_INIT(&dev_ctx->outstanding_io_ctx);

	assert(sizeof(*dev_ctx->io_ctx_array) <= UINT64_MAX / FUZZ_QUEUE_DEPTH);
	dev_ctx->io_ctx_array = spdk_malloc(sizeof(*dev_ctx->io_ctx_array) * FUZZ_QUEUE_DEPTH,
					    0x0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (dev_ctx->io_ctx_array == NULL) {
		free(dev_ctx);
		return -ENOMEM;
	}

	for (i = 0; i < FUZZ_QUEUE_DEPTH; i++) {
		TAILQ_INSERT_HEAD(&dev_ctx->free_io_ctx, &dev_ctx->io_ctx_array[i], link);
	}

	dev_ctx->thread = spdk_thread_create(NULL, NULL);
	if (dev_ctx->thread == NULL) {
		fprintf(stderr, "Unable to allocate a thread for a fuzz device.\n");
		rc = -ENOMEM;
		goto error_out;
	}

	TAILQ_INSERT_TAIL(&g_dev_list, dev_ctx, link);
	return 0;

error_out:
	spdk_free(dev_ctx->io_ctx_array);
	free(dev_ctx);
	return rc;
}
/* dev initialization code end */

/* build requests begin */
static void
prep_iscsi_pdu_bhs_opcode_cmd(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx)
{
	io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.scsi_req);
	io_ctx->iovs.iov_resp.iov_len = sizeof(io_ctx->resp.scsi_resp);
	fuzz_fill_random_bytes((char *)io_ctx->req.scsi_req, sizeof(io_ctx->req.scsi_req),
			       &dev_ctx->random_seed);
}
/* build requests end */

static void
check_successful_op(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx);

/* submit requests begin */
struct test_mempool {
	size_t	count;
	size_t	ele_size;
	size_t	ops_index;
};

struct spdk_mempool *
spdk_mempool_create1(const char *name, size_t count,
		     size_t ele_size, size_t cache_size, int socket_id)
{
	struct test_mempool *mp;

	mp = calloc(1, sizeof(*mp));
	if (mp == NULL) {
		return NULL;
	}

	mp->count = count;
	mp->ele_size = ele_size;
	mp->ops_index = 1;

	return (struct spdk_mempool *)mp;
}

#define NUM_PDU_PER_CONNECTION(iscsi)	(2 * (iscsi->MaxQueueDepth + MAX_LARGE_DATAIN_PER_CONNECTION + 8))
#define PDU_POOL_SIZE(iscsi)		(iscsi->MaxConnections * NUM_PDU_PER_CONNECTION(iscsi))

static int
iscsi_initialize_pdu_pool(void)
{
	struct spdk_iscsi_globals *iscsi = &g_spdk_iscsi;

	/* create PDU pool */
	iscsi->pdu_pool = spdk_mempool_create1("PDU_Pool",
					       PDU_POOL_SIZE(iscsi),
					       sizeof(struct spdk_iscsi_pdu),
					       256, SPDK_ENV_SOCKET_ID_ANY);
	if (!iscsi->pdu_pool) {
		printf("create PDU pool failed\n");
		return -1;
	}

	return 0;
}

static int
iscsi_initialize_all_pools(void)
{
	if (iscsi_initialize_pdu_pool() != 0) {
		return -1;
	}

	return 0;
}

void
spdk_put_pdu1(struct spdk_iscsi_pdu *pdu)
{
	if (!pdu) {
		return;
	}

	pdu->ref--;
	if (pdu->ref < 0) {
		pdu->ref = 0;
	}

	if (pdu->ref == 0) {
		if (pdu->data && !pdu->data_from_mempool) {
			free(pdu->data);
		}
		free(pdu);
	}
}

struct spdk_iscsi_pdu *
spdk_get_pdu1(void)
{
	struct spdk_iscsi_pdu *pdu;

	pdu = malloc(sizeof(*pdu));
	if (!pdu) {
		return NULL;
	}

	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));
	pdu->ref = 1;

	return pdu;
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

	rsp_pdu = spdk_get_pdu1();
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

	SPDK_LOGDUMP(SPDK_LOG_ISCSI, "PDU", (void *)&rsp_pdu->bhs, ISCSI_BHS_LEN);

	spdk_iscsi_conn_write_pdu(conn, rsp_pdu);

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
remove_acked_pdu(struct spdk_iscsi_conn *conn, uint32_t ExpStatSN)
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
static void
iscsi_pdu_dump(struct spdk_iscsi_pdu *pdu)
{
	SPDK_ERRLOGDUMP("PDU", (uint8_t *)&pdu->bhs, ISCSI_BHS_LEN);
}

int
spdk_iscsi_execute1(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu)
{
	int opcode;
	struct spdk_iscsi_pdu *rsp_pdu = NULL;
	uint32_t ExpStatSN;
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

	/* connection in login phase but receive non-login opcode
	 * return response code 0x020b to initiator.
	 * */
	if (!conn->full_feature && conn->state == ISCSI_CONN_STATE_RUNNING) {
		rsp_pdu = spdk_get_pdu1();
		if (rsp_pdu == NULL) {
			return SPDK_ISCSI_CONNECTION_FATAL;
		}
		init_login_reject_response(pdu, rsp_pdu);
		spdk_iscsi_conn_write_pdu(conn, rsp_pdu);
		SPDK_ERRLOG("Received opcode %d in login phase\n", opcode);
		return SPDK_ISCSI_LOGIN_ERROR_RESPONSE;
	} else if (conn->state == ISCSI_CONN_STATE_INVALID) {
		SPDK_ERRLOG("before Full Feature\n");
		iscsi_pdu_dump(pdu);
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
		remove_acked_pdu(conn, ExpStatSN);
	}

	if (!I_bit && opcode != ISCSI_OP_SCSI_DATAOUT) {
		sess->ExpCmdSN++;
	}

	switch (opcode) {
	default:
		SPDK_ERRLOG("unsupported opcode %x\n", opcode);
		return iscsi_reject(conn, pdu, ISCSI_REASON_PROTOCOL_ERROR);
	}

	return 0;
}

static void
dev_submit_requests(struct fuzz_iscsi_dev_ctx *dev_ctx, uint64_t max_io_to_submit)
{
	struct fuzz_iscsi_io_ctx *io_ctx = NULL;
	int rc;
	uint64_t current_ticks;

	struct spdk_iscsi_pdu *pdu;
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct iscsi_bhs_scsi_req *scsi_req;
	struct spdk_iscsi_portal portal = {};
	struct spdk_iscsi_portal_grp group = {};

	uint8_t *data;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));

	sess.ExpCmdSN = 0;
	sess.MaxCmdSN = 64;
	sess.session_type = SESSION_TYPE_NORMAL;
	sess.MaxBurstLength = 1024;

	g_spdk_iscsi.MaxSessions = 512;
	g_spdk_iscsi.session = calloc(1, sizeof(void *) * g_spdk_iscsi.MaxSessions);
	g_spdk_iscsi.session[256 - 1] = &sess;

	rc = iscsi_initialize_all_pools();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_initialize_all_pools() failed\n");
		free(g_spdk_iscsi.session);
		g_spdk_iscsi.session = NULL;
		return;
	}

	TAILQ_INIT(&conn.queued_datain_tasks);
	conn.data_in_cnt = 0;
	conn.portal = &portal;
	portal.group = &group;
	conn.portal->group->tag = 0;
	conn.params = NULL;

	conn.full_feature = 1;
	conn.sess = &sess;
	conn.state = ISCSI_CONN_STATE_RUNNING;

	conn.header_digest = true;
	conn.data_digest = true;


	TAILQ_INIT(&conn.write_pdu_list);
	TAILQ_HEAD(, spdk_iscsi_pdu) g_write_pdu_list;
	TAILQ_INIT(&g_write_pdu_list);

	while (!TAILQ_EMPTY(&dev_ctx->free_io_ctx) && dev_ctx->submitted_io < max_io_to_submit) {

		current_ticks = spdk_get_ticks();
		if (current_ticks > g_runtime_ticks) {
			g_run = 0;
		}
		if (!g_run) {
			goto out;
		}

		pdu = spdk_get_pdu1();
		DSET24(&pdu->bhs.data_segment_len, 512);
		data = calloc(1, 512);
		pdu->data = data;
		pdu->writev_offset = 0;
		scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;

		io_ctx = TAILQ_FIRST(&dev_ctx->free_io_ctx);
		io_ctx->req.scsi_req = scsi_req;
		prep_iscsi_pdu_bhs_opcode_cmd(dev_ctx, io_ctx);

		g_is_valid_opcode = false;
		for (int i = 0 ; i < 11 ; i++) {
			if (valid_opcode_list[ i ] == pdu->bhs.opcode) {
				g_is_valid_opcode = true;
				break;
			}
		}
		if (!g_is_valid_opcode) {
			rc = spdk_iscsi_execute1(&conn, pdu);
			if (rc == 0) {
				TAILQ_REMOVE(&dev_ctx->free_io_ctx, io_ctx, link);
				TAILQ_INSERT_TAIL(&dev_ctx->outstanding_io_ctx, io_ctx, link);
				dev_ctx->submitted_io++;
			} else {
				SPDK_ERRLOG("spdk_iscsi_execute() fatal error.\n");
				return;
			}
		}

		if (io_ctx) {
			TAILQ_REMOVE(&dev_ctx->outstanding_io_ctx, io_ctx, link);
			TAILQ_INSERT_HEAD(&dev_ctx->free_io_ctx, io_ctx, link);
			check_successful_op(dev_ctx, io_ctx);
			dev_ctx->completed_io++;
			dev_ctx->timeout_tsc = fuzz_refresh_timeout();
		}

		pdu = TAILQ_FIRST(&conn.write_pdu_list);
		if (pdu == NULL) {
			return;
		}

		TAILQ_REMOVE(&conn.write_pdu_list, pdu, tailq);
		spdk_put_pdu1(pdu);

	}

out:
	free(g_spdk_iscsi.session);

}

/* submit requests end */

/* complete requests begin */
static void
check_successful_op(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx)
{
	bool is_successful = false;

	if (g_is_valid_opcode) {
		is_successful = true;
	}

	if (is_successful) {
		fprintf(stderr, "An I/O completed without an error status. This could be worth looking into.\n");
		fprintf(stderr,
			"There is also a good chance that the target just failed before setting a status.\n");
		dev_ctx->successful_io++;
		print_req_obj(dev_ctx, io_ctx);
	} else {
		fprintf(stderr, "The following I/O failed as expected.\n");
		print_req_obj(dev_ctx, io_ctx);
	}

}
static bool
parse_scsi_cmds(void *item, struct spdk_json_val *value, size_t num_values)
{
/* TODO: after added parsing g_json_file */
	return true;
}

static int
poll_dev(void *ctx)
{
	struct fuzz_iscsi_dev_ctx *dev_ctx = ctx;
	int num_active_threads;
	uint64_t max_io_to_complete = UINT64_MAX;
	uint64_t current_ticks;

	if (g_json_file) {
		struct spdk_json_val *value = NULL;
		struct fuzz_iscsi_io_ctx *io_ctx = ctx;

		if (fuzz_parse_json_num(value, UINT32_MAX, &current_ticks)) {
			return 0;
		}
		if (fuzz_parse_args_into_array(g_json_file,
							(void **)&dev_ctx,
							sizeof(struct fuzz_iscsi_io_ctx),
							SCSI_IO_NAME, parse_scsi_cmds)) {
			return 0;
		}

		if (fuzz_get_base_64_buffer_value(io_ctx->req.scsi_req,
										  sizeof(io_ctx->req.scsi_req->lun),
										  (char *)value->start,
										  value->len)) {
			return 0;
		}
		if (fuzz_get_value_base_64_buffer(io_ctx->req.scsi_req,
	 			sizeof(io_ctx->req.scsi_req->lun))) {
			return 0;
		}
	}

	current_ticks = spdk_get_ticks();

	if (current_ticks > dev_ctx->timeout_tsc) {
		dev_ctx->timed_out = true;
		g_run = false;
		fprintf(stderr, "The test on device %p timed out. Dumping contents now.\n", dev_ctx);
		dump_outstanding_io(dev_ctx);
	}

	if (current_ticks > g_runtime_ticks) {
		g_run = 0;
	}

	if (!g_run || dev_ctx->completed_io >= max_io_to_complete) {
		if (TAILQ_EMPTY(&dev_ctx->outstanding_io_ctx)) {
			spdk_poller_unregister(&dev_ctx->poller);
			num_active_threads = __sync_sub_and_fetch(&g_num_active_threads, 1);
			if (num_active_threads == 0) {
				g_run = 0;
			}
			spdk_thread_exit(dev_ctx->thread);
		}
		return 0;
	}

	dev_submit_requests(dev_ctx, max_io_to_complete);
	return 0;
}
/* complete requests end */

static void
start_io(void *ctx)
{
	struct fuzz_iscsi_dev_ctx *dev_ctx = ctx;

	if (g_random_seed) {
		dev_ctx->random_seed = g_random_seed;
	} else {
		dev_ctx->random_seed = spdk_get_ticks();
	}

	dev_ctx->timeout_tsc = fuzz_refresh_timeout();

	dev_ctx->poller = spdk_poller_register(poll_dev, dev_ctx, 0);
	if (dev_ctx->poller == NULL) {
		return;
	}
}

static int
end_fuzz(void *ctx)
{
	if (!g_run && !g_num_active_threads) {
		spdk_poller_unregister(&g_run_poller);

		cleanup();
		spdk_app_stop(0);

		printf("Fuzzing completed. Shutting down the fuzz application\n\n");
	}

	return 0;
}

static void
begin_fuzz(void *ctx)
{
	struct fuzz_iscsi_dev_ctx *dev_ctx;

	g_runtime_ticks = spdk_get_ticks() + g_runtime * spdk_get_ticks_hz();

	g_valid_buffer = spdk_malloc(0x1000, 0x200, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (g_valid_buffer == NULL) {
		fprintf(stderr, "Failed to allocate a valid buffer for I/O\n");
		goto out;
	}

	g_run_poller = spdk_poller_register(end_fuzz, NULL, 0);
	if (g_run_poller == NULL) {
		fprintf(stderr, "Failed to register a poller for test completion checking.\n");
	}

	fuzz_iscsi_dev_init();

	TAILQ_FOREACH(dev_ctx, &g_dev_list, link) {
		assert(dev_ctx->thread != NULL);
		spdk_thread_send_msg(dev_ctx->thread, start_io, dev_ctx);
		__sync_add_and_fetch(&g_num_active_threads, 1);
	}

	return;
out:
	cleanup();
	spdk_app_stop(0);
}


static void
iscsi_fuzz_usage(void)
{
	fprintf(stderr, " -j <path>                 Path to a json file containing named objects.\n");
	fprintf(stderr, " -S <integer>              Seed value for test.\n");
	fprintf(stderr,
		" -t <integer>              Time in seconds to run the fuzz test. Only valid if -j is not specified.\n");
}

static int
iscsi_fuzz_parse(int ch, char *arg)
{
	int64_t error_test;

	switch (ch) {
	case 'j':
		g_json_file = optarg;
		break;
	case 'S':
		error_test = spdk_strtol(arg, 10);
		if (error_test < 0) {
			fprintf(stderr, "Invalid value supplied for the random seed.\n");
			return -1;
		} else {
			g_random_seed = error_test;
		}
		break;
	case 't':
		g_runtime = spdk_strtol(optarg, 10);
		if (g_runtime < 0 || g_runtime > MAX_RUNTIME_S) {
			fprintf(stderr, "You must supply a positive runtime value less than 86401.\n");
			return -1;
		}
		break;
	case '?':
	default:
		return -EINVAL;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	g_runtime = DEFAULT_RUNTIME;

	spdk_app_opts_init(&opts);
	opts.name = "iscsi_fuzz";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "j:S:t:", NULL, iscsi_fuzz_parse,
				      iscsi_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	rc = spdk_app_start(&opts, begin_fuzz, NULL);

	return rc;
}
