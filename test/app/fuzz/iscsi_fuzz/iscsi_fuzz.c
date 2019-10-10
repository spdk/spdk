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
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/json.h"
#include "spdk/endian.h"
#include "spdk/bdev.h"
#include "spdk/notify.h"
#include "spdk/scsi.h"
#include "spdk_internal/mock.h"
#include "spdk/scsi_spec.h"
#include "fuzz_common.h"
#include "iscsi/conn.h"
#include "iscsi/iscsi.c"
#include "scsi/scsi_internal.h"


#define UNIQUE_OPCODES		256
#define FUZZ_QUEUE_DEPTH	128
#define SCSI_IO_NAME		"scsi_cmd"
#define GET_PDU_LOOP_COUNT	16

/* Global run state */
uint64_t	g_runtime_ticks;
int		g_runtime;
int		g_num_active_threads;
bool		g_run = true;
bool		g_is_valid_opcode = false;

struct spdk_log_flag SPDK_LOG_ISCSI = {
	.name = "iscsi",
	.enabled = false,
};

/* Global resources */
TAILQ_HEAD(, fuzz_iscsi_dev_ctx)	g_dev_list = TAILQ_HEAD_INITIALIZER(g_dev_list);
struct spdk_poller			*g_run_poller;
void					*g_valid_buffer;
unsigned int				g_random_seed;
char					*g_json_file = NULL;
struct fuzz_iscsi_io_ctx		*g_scsi_cmd_array = NULL;
size_t					g_scsi_cmd_array_size;
struct spdk_iscsi_pdu	spdk_mempool;

TAILQ_HEAD(, spdk_iscsi_pdu) g_get_pdu_list;

struct fuzz_iscsi_iov_ctx {
	struct iovec			iov_req;
	struct iovec			iov_data;
	struct iovec			iov_resp;
};

struct fuzz_iscsi_io_ctx {
	struct fuzz_iscsi_iov_ctx		iovs;
	union {
		struct iscsi_bhs_nop_out	*nop_out_req;
		struct iscsi_bhs_scsi_req	*scsi_req;
		struct iscsi_bhs_task_req	*task_req;
		struct iscsi_bhs_login_req	*login_req;
		struct iscsi_bhs_text_req	*text_req;
		struct iscsi_bhs_data_out	*data_req;
		struct iscsi_bhs_logout_req	*logout_req;
		struct iscsi_bhs_snack_req	*snack_req;
	} req;
	union {
		struct iscsi_bhs_nop_in	*nop_in_resp;
		struct iscsi_bhs_login_rsp	*login_resp;
		struct iscsi_bhs_logout_resp	*logout_resp;
		struct iscsi_bhs_scsi_resp	*scsi_resp;
		struct iscsi_bhs_task_resp	*task_resp;
		struct iscsi_bhs_text_resp	*text_resp;
		struct iscsi_bhs_data_in	*data_in_resp;
		struct iscsi_bhs_r2t	*r2t_resp;
		struct iscsi_bhs_async	*async_resp;
		struct iscsi_bhs_reject	*reject_resp;
	} resp;

	TAILQ_ENTRY(fuzz_iscsi_io_ctx) link;
};

struct fuzz_iscsi_dev_ctx {
	struct spdk_scsi_dev			*scsi_dev;
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

	TAILQ_ENTRY(fuzz_iscsi_dev_ctx)		link;
};

void *
spdk_fuzz_task_mempool_get(struct spdk_mempool *_mp);
struct spdk_mobj *
spdk_fuzz_mobj_mempool_get(struct spdk_mempool *_mp);
void
spdk_fuzz_mempool_put(struct spdk_mempool *_mp, void *ele);
struct spdk_scsi_lun *spdk_fuzz_scsi_lun_construct(struct spdk_bdev *bdev,
		void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
		void *hotremove_ctx);

int
spdk_iscsi_chap_get_authinfo(struct iscsi_chap_auth *auth, const char *authuser,
			     int ag_tag)
{
	/* TODO: fill something */
	return 0;
}

void
spdk_shutdown_iscsi_conns_done(void)
{
	/* TODO: fill something */
	return;

}

void
spdk_put_pdu(struct spdk_iscsi_pdu *pdu)
{
	if (!pdu) {
		return;
	}

	pdu->ref--;
	if (pdu->ref < 0) {
		pdu->ref = 0;
	}

	if (pdu->ref == 0) {
		if (pdu->mobj) {
			free(pdu->mobj);
		}
		if (pdu->data && !pdu->data_from_mempool) {
			free(pdu->data);
		}
		TAILQ_REMOVE(&g_get_pdu_list, pdu, tailq);
		free(pdu);
	}
}

struct spdk_iscsi_pdu *
spdk_get_pdu(void)
{
	struct spdk_iscsi_pdu *pdu;

	pdu = malloc(sizeof(*pdu));
	if (!pdu) {
		return NULL;
	}
	memset(pdu, 0, offsetof(struct spdk_iscsi_pdu, ahs));

	pdu->ref = 1;
	TAILQ_INSERT_TAIL(&g_get_pdu_list, pdu, tailq);

	return pdu;
}

struct test_mempool {
	size_t	count;
	size_t	ele_size;
};

void *
spdk_fuzz_task_mempool_get(struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;
	void *buf;

	if (mp && mp->count == 0) {
		return NULL;
	}

	buf = malloc(sizeof(struct spdk_iscsi_task));
	if (!buf) {
		return NULL;
	} else {
		if (mp) {
			mp->count--;
		}
		return buf;
	}
}

struct spdk_mobj *
spdk_fuzz_mobj_mempool_get(struct spdk_mempool *_mp)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;
	struct spdk_mobj *buf1;
	void *buf2;

	if (mp && mp->count == 0) {
		return NULL;
	}

	buf1 = malloc(sizeof(struct spdk_mobj));
	if (!buf1) {
		return NULL;
	} else {
		buf2 = malloc(100);
		buf1->buf = buf2;
		if (mp) {
			mp->count--;
		}
		return buf1;
	}
}

void
spdk_fuzz_mempool_put(struct spdk_mempool *_mp, void *ele)
{
	struct test_mempool *mp = (struct test_mempool *)_mp;

	if (mp) {
		mp->count++;
	}
	free(ele);
}

static void
iscsi_task_free(struct spdk_scsi_task *scsi_task)
{
	struct spdk_iscsi_task *task = spdk_iscsi_task_from_scsi_task(scsi_task);

	if (task->parent) {
		spdk_scsi_task_put(&task->parent->scsi);
		task->parent = NULL;
	}

	spdk_iscsi_task_disassociate_pdu(task);
	assert(task->conn->pending_task_cnt > 0);
	task->conn->pending_task_cnt--;
	spdk_fuzz_mempool_put(g_spdk_iscsi.task_pool, (void *)task);
}

struct spdk_iscsi_task *
spdk_iscsi_task_get(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *parent,
		    spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_iscsi_task *task;

	task = spdk_fuzz_task_mempool_get(g_spdk_iscsi.task_pool);
	if (!task) {
		printf("Unable to get task\n");
		abort();
	}

	memset(task, 0, sizeof(*task));
	task->conn = conn;
	assert(conn->pending_task_cnt < UINT32_MAX);
	conn->pending_task_cnt++;
	spdk_scsi_task_construct(&task->scsi,
				 cpl_fn,
				 iscsi_task_free);
	if (parent) {
		parent->scsi.ref++;
		task->parent = parent;
		task->tag = parent->tag;
		task->lun_id = parent->lun_id;
		task->scsi.dxfer_dir = parent->scsi.dxfer_dir;
		task->scsi.transfer_len = parent->scsi.transfer_len;
		task->scsi.lun = parent->scsi.lun;
		task->scsi.cdb = parent->scsi.cdb;
		task->scsi.target_port = parent->scsi.target_port;
		task->scsi.initiator_port = parent->scsi.initiator_port;
	}

	return task;
}

struct spdk_scsi_lun *
	spdk_fuzz_scsi_lun_construct(struct spdk_bdev *bdev,
			     void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			     void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun;

	lun = calloc(1, sizeof(struct spdk_scsi_lun));
	if (lun == NULL) {
		printf("could not allocate lun\n");
		return NULL;
	}

	lun->bdev = bdev;

	TAILQ_INIT(&lun->tasks);
	TAILQ_INIT(&lun->pending_tasks);
	TAILQ_INIT(&lun->mgmt_tasks);
	TAILQ_INIT(&lun->pending_mgmt_tasks);

	lun->bdev = bdev;
	lun->io_channel = NULL;
	lun->hotremove_cb = hotremove_cb;
	lun->hotremove_ctx = hotremove_ctx;
	TAILQ_INIT(&lun->open_descs);
	TAILQ_INIT(&lun->reg_head);

	return lun;
}

static void
cleanup(void)
{
	struct fuzz_iscsi_dev_ctx *dev_ctx, *tmp;

	TAILQ_FOREACH_SAFE(dev_ctx, &g_dev_list, link, tmp) {
		printf("device %p stats: Completed I/O: %lu, Successful I/O: %lu\n", dev_ctx,
		       dev_ctx->completed_io, dev_ctx->successful_io);
		free(dev_ctx->scsi_dev);
		free(dev_ctx);
	}

	spdk_free(g_valid_buffer);

	if (g_scsi_cmd_array) {
		free(g_scsi_cmd_array);
	}
}

/* data dumping functions begin */
static int
dump_iscsi_cmd(void *ctx, const void *data, size_t size)
{
	fprintf(stderr, "%s\n", (const char *)data);
	return 0;
}

static void
print_scsi_io_data(struct spdk_json_write_ctx *w, struct fuzz_iscsi_io_ctx *io_ctx)
{
	char *lun_data;
	char *cdb_data;

	lun_data = fuzz_get_value_base_64_buffer((void *)io_ctx->req.scsi_req->lun,
			sizeof(io_ctx->req.scsi_req->lun));
	cdb_data = fuzz_get_value_base_64_buffer((void *)io_ctx->req.scsi_req->cdb,
			sizeof(io_ctx->req.scsi_req->cdb));

	if (g_json_file) {
		/* TODO: fill g_json_file */
		spdk_json_write_named_string(w, "luns", lun_data);
		spdk_json_write_named_string(w, "cdb", cdb_data);
	}

	free(lun_data);
	free(cdb_data);
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
	if (g_json_file) {
		struct spdk_json_write_ctx *w;

		w = spdk_json_write_begin(dump_iscsi_cmd, NULL, SPDK_JSON_WRITE_FLAG_FORMATTED);

		spdk_json_write_named_object_begin(w, SCSI_IO_NAME);
		print_iovs(w, io_ctx);

		print_scsi_io_data(w, io_ctx);

		spdk_json_write_object_end(w);
		spdk_json_write_end(w);
	}
}

static void
dump_outstanding_io(struct fuzz_iscsi_dev_ctx *dev_ctx)
{
	struct fuzz_iscsi_io_ctx *io_ctx, *tmp;

	TAILQ_FOREACH_SAFE(io_ctx, &dev_ctx->outstanding_io_ctx, link, tmp) {
		if (g_json_file) {
			print_req_obj(dev_ctx, io_ctx);
		}
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
	free(dev_ctx->io_ctx_array);
	free(dev_ctx);
	return rc;
}
/* dev initialization code end */

/* build requests begin */
static void
prep_iscsi_pdu_bhs_opcode_cmd(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx,
			      int req_type)
{
	io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req);

	switch (req_type) {
	case 0:
		fuzz_fill_random_bytes((char *)io_ctx->req.nop_out_req, sizeof(io_ctx->req.nop_out_req),
				       &dev_ctx->random_seed);
		break;
	case 1:
		fuzz_fill_random_bytes((char *)io_ctx->req.scsi_req, sizeof(io_ctx->req.scsi_req),
				       &dev_ctx->random_seed);
		break;
	case 2:
		fuzz_fill_random_bytes((char *)io_ctx->req.task_req, sizeof(io_ctx->req.task_req),
				       &dev_ctx->random_seed);
		break;
	case 3:
		fuzz_fill_random_bytes((char *)io_ctx->req.login_req, sizeof(io_ctx->req.login_req),
				       &dev_ctx->random_seed);
		break;
	case 4:
		fuzz_fill_random_bytes((char *)io_ctx->req.text_req, sizeof(io_ctx->req.text_req),
				       &dev_ctx->random_seed);
		break;
	case 5:
		fuzz_fill_random_bytes((char *)io_ctx->req.data_req, sizeof(io_ctx->req.data_req),
				       &dev_ctx->random_seed);
		break;
	case 6:
		fuzz_fill_random_bytes((char *)io_ctx->req.logout_req, sizeof(io_ctx->req.logout_req),
				       &dev_ctx->random_seed);
		break;
	case 7:
		fuzz_fill_random_bytes((char *)io_ctx->req.snack_req, sizeof(io_ctx->req.snack_req),
				       &dev_ctx->random_seed);
		break;
	default:
		break;
	}

}
/* build requests end */

static void
check_successful_op(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx);

/* submit requests begin */
static uint64_t
get_max_num_io(struct fuzz_iscsi_dev_ctx *dev_ctx)
{
	return g_scsi_cmd_array_size;
}
struct spdk_bdev {
	char name[100];
};


static int
iscsi_fuzz_read_pdu(struct spdk_iscsi_conn *conn)
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
			conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR;
			break;
		case ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR:

			pdu->data_segment_len = ISCSI_ALIGN(DGET24(pdu->bhs.data_segment_len));

			/* AHS */
			ahs_len = pdu->bhs.total_ahs_len * 4;
			assert(ahs_len <= ISCSI_AHS_LEN);

			/* Header Digest */
			if (conn->header_digest) {
				crc32c = spdk_iscsi_pdu_calc_header_digest(pdu);
				rc = MATCH_DIGEST_WORD(pdu->header_digest, crc32c);
				if (rc == 0) {
					printf("header digest error (%s)\n", conn->initiator_name);
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}
			}

			rc = iscsi_pdu_hdr_handle(conn, pdu);
			g_is_valid_opcode = !pdu->is_rejected;
			if (rc < 0) {
				printf("Critical error is detected. Close the connection\n");
				conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
				break;
			}

			conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD;
			break;
		case ISCSI_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			data_len = pdu->data_segment_len;

			if (data_len != 0 && pdu->data_buf == NULL) {
				if (data_len <= spdk_get_max_immediate_data_size()) {
					pool = g_spdk_iscsi.pdu_immediate_data_pool;
					pdu->data_buf_len = SPDK_BDEV_BUF_SIZE_WITH_MD(spdk_get_max_immediate_data_size());
				} else if (data_len <= SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH) {
					pool = g_spdk_iscsi.pdu_data_out_pool;
					pdu->data_buf_len = SPDK_BDEV_BUF_SIZE_WITH_MD(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH);
				} else {
					printf("Data(%d) > MaxSegment(%d)\n",
					       data_len, SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH);
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}
				pdu->mobj = spdk_fuzz_mobj_mempool_get(pool);
				if (pdu->mobj == NULL) {
					return 0;
				}
				pdu->data_buf = pdu->mobj->buf;
				pdu->data = pdu->mobj->buf;
				printf("pdu->data pointer is %p\n", pdu->data);
				pdu->data_from_mempool = true;
			}


			/* All data for this PDU has now been read from the socket. */
			spdk_trace_record(TRACE_ISCSI_READ_PDU, conn->id, pdu->data_valid_bytes,
					  (uintptr_t)pdu, pdu->bhs.opcode);

			/* check data digest */
			if (conn->data_digest && data_len != 0) {
				crc32c = spdk_iscsi_pdu_calc_data_digest(pdu);
				rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
				if (rc == 0) {
					printf("data digest error (%s)\n", conn->initiator_name);
					conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
					break;
				}
			}

			if (conn->is_logged_out) {
				printf("pdu received after logout\n");
				conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_ERROR;
				break;
			}

			if (!pdu->is_rejected) {
				rc = iscsi_pdu_payload_handle(conn, pdu);
			} else {
				rc = 0;
			}
			if (rc == 0) {
				spdk_trace_record(TRACE_ISCSI_TASK_EXECUTED, 0, 0, (uintptr_t)pdu, 0);
				spdk_put_pdu(pdu);
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
			printf("code should not come here\n");
			break;
		}
	} while (prev_state != conn->pdu_recv_state);

	return 0;
}




static void
dev_submit_requests(struct fuzz_iscsi_dev_ctx *dev_ctx, uint64_t max_io_to_submit)
{
	struct fuzz_iscsi_io_ctx *io_ctx = NULL;
	uint64_t current_ticks;
	int req_type;
	int rc = 0;

	struct spdk_iscsi_pdu		*pdu;
	struct spdk_iscsi_pdu		*entry_pdu;
	struct spdk_iscsi_sess		sess = {};
	struct spdk_iscsi_conn		conn = {};
	struct spdk_scsi_lun		*lun;
	struct iscsi_bhs_nop_out	*nop_out_req;
	struct iscsi_bhs_scsi_req	*scsi_req;
	struct iscsi_bhs_task_req	*task_req;
	struct iscsi_bhs_login_req	*login_req;
	struct iscsi_bhs_text_req	*text_req;
	struct iscsi_bhs_data_out	*data_req;
	struct iscsi_bhs_logout_req *logout_req;
	struct iscsi_bhs_snack_req	*snack_req;
	struct spdk_bdev bdev = {};

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&lun, 0, sizeof(lun));

	sess.ExpCmdSN = 0;
	sess.MaxCmdSN = 64;
	sess.session_type = SESSION_TYPE_NORMAL;
	sess.MaxBurstLength = 1024;
	sess.ErrorRecoveryLevel = 1;
	sess.MaxOutstandingR2T = 1;

	conn.data_in_cnt = 0;
	conn.params = NULL;

	conn.full_feature = 1;
	conn.sess = &sess;
	conn.state = ISCSI_CONN_STATE_RUNNING;
	conn.MaxRecvDataSegmentLength = 8192;

	conn.header_digest = false;
	conn.data_digest = false;

	dev_ctx->scsi_dev = calloc(1, sizeof(*dev_ctx->scsi_dev));
	conn.dev = dev_ctx->scsi_dev;
	lun = spdk_fuzz_scsi_lun_construct(&bdev, NULL, NULL);
	if (lun == NULL) {
		printf("spdk_fuzz_scsi_lun_construct is failed.\n");
		return;
	}
	lun->id = 0;
	lun->dev = dev_ctx->scsi_dev;
	dev_ctx->scsi_dev->lun[0] = lun;

	TAILQ_INIT(&g_get_pdu_list);
	TAILQ_INIT(&conn.write_pdu_list);
	TAILQ_INIT(&conn.snack_pdu_list);
	TAILQ_INIT(&conn.queued_r2t_tasks);
	TAILQ_INIT(&conn.active_r2t_tasks);
	TAILQ_INIT(&conn.queued_datain_tasks);
	memset(&conn.open_lun_descs, 0, sizeof(conn.open_lun_descs));


	for (req_type = 0; req_type < 8; req_type++) {
		while (!TAILQ_EMPTY(&dev_ctx->free_io_ctx) && dev_ctx->submitted_io < max_io_to_submit) {
			printf("\n");

			pdu = spdk_get_pdu();
			pdu->writev_offset = 0;
			pdu->data = "InitiatorName=iqn.1994-05.com.redhat:c7bb74ede1a\0";
			conn.pdu_in_progress = pdu;
			conn.pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_READY;
			conn.login_rsp_pdu = pdu;

			io_ctx = TAILQ_FIRST(&dev_ctx->free_io_ctx);

			switch (req_type) {
			case 0:
				nop_out_req = (struct iscsi_bhs_nop_out *)&pdu->bhs;
				io_ctx->req.nop_out_req = nop_out_req;
				break;
			case 1:
				scsi_req = (struct iscsi_bhs_scsi_req *)&pdu->bhs;
				io_ctx->req.scsi_req = scsi_req;
				break;
			case 2:
				task_req = (struct iscsi_bhs_task_req *)&pdu->bhs;
				io_ctx->req.task_req = task_req;
				break;
			case 3:
				login_req = (struct iscsi_bhs_login_req *)&pdu->bhs;
				io_ctx->req.login_req = login_req;
				break;
			case 4:
				text_req = (struct iscsi_bhs_text_req *)&pdu->bhs;
				io_ctx->req.text_req = text_req;
				break;
			case 5:
				data_req = (struct iscsi_bhs_data_out *)&pdu->bhs;
				io_ctx->req.data_req = data_req;
				break;
			case 6:
				logout_req = (struct iscsi_bhs_logout_req *)&pdu->bhs;
				io_ctx->req.logout_req = logout_req;
				break;
			case 7:
				snack_req = (struct iscsi_bhs_snack_req *)&pdu->bhs;
				io_ctx->req.snack_req = snack_req;
				break;
			default:
				break;
			}


			prep_iscsi_pdu_bhs_opcode_cmd(dev_ctx, io_ctx, req_type);

			pdu->bhs_valid_bytes = ISCSI_BHS_LEN;
			pdu->bhs.data_segment_len[0] = 0;
			pdu->bhs.data_segment_len[1] = 0;
			pdu->bhs.data_segment_len[2] = 64;
			pdu->bhs.flags = 1;
			pdu->bhs.total_ahs_len = 0;
			pdu->bhs.immediate = 1;
			if (req_type == 3) {
				login_req->version_min = 0;
			}

			rc = iscsi_fuzz_read_pdu(&conn);
			if (rc == 0) {
				TAILQ_REMOVE(&dev_ctx->free_io_ctx, io_ctx, link);
				TAILQ_INSERT_TAIL(&dev_ctx->outstanding_io_ctx, io_ctx, link);
				dev_ctx->submitted_io++;
			} else {
				printf("iscsi_fuzz_read_pdu() fatal error.\n");
			}

			if (io_ctx) {
				TAILQ_REMOVE(&dev_ctx->outstanding_io_ctx, io_ctx, link);
				TAILQ_INSERT_HEAD(&dev_ctx->free_io_ctx, io_ctx, link);
				check_successful_op(dev_ctx, io_ctx);
				dev_ctx->completed_io++;
				dev_ctx->timeout_tsc = fuzz_refresh_timeout();
			}

			conn.pdu_in_progress = NULL;
			usleep(10000);

			current_ticks = spdk_get_ticks();
			if (current_ticks > g_runtime_ticks) {
				g_run = false;
				req_type = 10; /* go out of for() */
				break;
			}

		}
		TAILQ_FOREACH_SAFE(pdu, &g_get_pdu_list, tailq, entry_pdu) {
			TAILQ_REMOVE(&g_get_pdu_list, pdu, tailq);
			spdk_put_pdu(pdu);
		}

	}
	return;
}

/* submit requests end */

/* complete requests begin */
static void
check_successful_op(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx)
{
	if (g_is_valid_opcode) {
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

static int
hex_value(uint8_t c)
{
#define V(x, y) [x] = y + 1
	static const int8_t val[256] = {
		V('0', 0), V('1', 1), V('2', 2), V('3', 3), V('4', 4),
		V('5', 5), V('6', 6), V('7', 7), V('8', 8), V('9', 9),
		V('A', 0xA), V('B', 0xB), V('C', 0xC), V('D', 0xD), V('E', 0xE), V('F', 0xF),
		V('a', 0xA), V('b', 0xB), V('c', 0xC), V('d', 0xD), V('e', 0xE), V('f', 0xF),
	};
#undef V

	return val[c] - 1;
}

static int
fuzz_json_decode_hex_uint64(const struct spdk_json_val *val, void *out)
{
	uint64_t *out_val = out;
	size_t i;
	char *val_pointer = val->start;
	int current_val;

	if (val->len > 16) {
		return -EINVAL;
	}

	*out_val = 0;
	for (i = 0; i < val->len; i++) {
		*out_val = *out_val << 4;
		current_val = hex_value(*val_pointer);
		if (current_val < 0) {
			return -EINVAL;
		}
		*out_val += current_val;
		val_pointer++;
	}

	return 0;
}

static const struct spdk_json_object_decoder fuzz_iscsi_iov_decoders[] = {
	{"iov_base", offsetof(struct iovec, iov_base), fuzz_json_decode_hex_uint64},
	{"iov_len", offsetof(struct iovec, iov_len), spdk_json_decode_uint64},
};

static size_t
parse_iov_struct(struct iovec *iovec, struct spdk_json_val *value)
{
	int rc;

	if (value->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		return -1;
	}

	rc = spdk_json_decode_object(value,
				     fuzz_iscsi_iov_decoders,
				     SPDK_COUNTOF(fuzz_iscsi_iov_decoders),
				     iovec);
	if (rc) {
		return -1;
	}

	while (value->type != SPDK_JSON_VAL_OBJECT_END) {
		value++;
		rc++;
	}

	/* The +1 instructs the calling function to skip over the OBJECT_END function. */
	rc += 1;
	return rc;
}

static bool
parse_scsi_cmds(void *item, struct spdk_json_val *value, size_t num_values)
{
	struct fuzz_iscsi_io_ctx *io_ctx = item;
	struct spdk_json_val *prev_value;
	int nested_object_size;
	uint64_t tmp_val;
	size_t i = 0;

	while (i < num_values) {
		nested_object_size = 1;
		if (value->type == SPDK_JSON_VAL_NAME) {
			prev_value = value;
			value++;
			i++;
			if (!strncmp(prev_value->start, "req_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_req, value);
			} else if (!strncmp(prev_value->start, "data_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "resp_iov", prev_value->len)) {
				nested_object_size = parse_iov_struct(&io_ctx->iovs.iov_data, value);
			} else if (!strncmp(prev_value->start, "lun", prev_value->len)) {
				if (fuzz_get_base_64_buffer_value(&io_ctx->req.scsi_req->lun,
								  sizeof(io_ctx->req.scsi_req->lun),
								  (char *)value->start,
								  value->len)) {
					nested_object_size = -1;
				}
			} else if (!strncmp(prev_value->start, "itt", prev_value->len)) {
				if (fuzz_parse_json_num(value, UINT64_MAX, &tmp_val)) {
					nested_object_size = -1;
				} else {
					io_ctx->req.scsi_req->itt = tmp_val;
				}
			} else if (!strncmp(prev_value->start, "cdb", prev_value->len)) {
				if (fuzz_get_base_64_buffer_value(&io_ctx->req.scsi_req->cdb,
								  sizeof(io_ctx->req.scsi_req->cdb),
								  (char *)value->start,
								  value->len)) {
					nested_object_size = -1;
				}
			}
		}
		if (nested_object_size < 0) {
			fprintf(stderr, "Invalid value supplied for io_ctx->%.*s: %.*s\n", prev_value->len,
				(char *)prev_value->start, value->len, (char *)value->start);
			return false;
		}
		value += nested_object_size;
		i += nested_object_size;
	}
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
		max_io_to_complete = get_max_num_io(dev_ctx);
	}

	current_ticks = spdk_get_ticks();

	if (current_ticks > dev_ctx->timeout_tsc) {
		dev_ctx->timed_out = true;
		g_run = false;
		fprintf(stderr, "The test on device %p timed out. Dumping contents now.\n", dev_ctx);
		dump_outstanding_io(dev_ctx);
	}

	if (current_ticks > g_runtime_ticks) {
		g_run = false;
	}

	if (!g_run || dev_ctx->completed_io >= max_io_to_complete) {
		if (TAILQ_EMPTY(&dev_ctx->outstanding_io_ctx)) {
			spdk_poller_unregister(&dev_ctx->poller);
			num_active_threads = __sync_sub_and_fetch(&g_num_active_threads, 1);
			if (num_active_threads == 0) {
				g_run = false;
			}
			spdk_thread_exit(dev_ctx->thread);
		}
		return -1;
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
end_iscsi_fuzz(void *ctx)
{
	if (!g_run && !g_num_active_threads) {
		spdk_poller_unregister(&g_run_poller);

		cleanup();
		spdk_app_stop(0);

		printf("Fuzzing completed. Shutting down the fuzz application.\n\n");
	}

	return 0;
}

static void
begin_iscsi_fuzz(void *ctx)
{
	struct fuzz_iscsi_dev_ctx *dev_ctx;
	int rc;

	g_runtime_ticks = spdk_get_ticks() + g_runtime * spdk_get_ticks_hz();

	g_valid_buffer = spdk_malloc(0x1000, 0x200, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_SHARE);
	if (g_valid_buffer == NULL) {
		fprintf(stderr, "Failed to allocate a valid buffer for I/O\n");
		goto out;
	}

	g_run_poller = spdk_poller_register(end_iscsi_fuzz, NULL, 0);
	if (g_run_poller == NULL) {
		fprintf(stderr, "Failed to register a poller for test completion checking.\n");
		goto out;
	}

	rc = fuzz_iscsi_dev_init();
	if (rc) {
		fprintf(stderr, "fuzz_iscsi_dev_init() failed.\n");
		goto out;
	}

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
		if (g_runtime <= 0 || g_runtime > MAX_RUNTIME_S) {
			fprintf(stderr, "You must supply a positive runtime value less than %d.\n", MAX_RUNTIME_S);
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

	if (g_json_file) {
		g_scsi_cmd_array_size = fuzz_parse_args_into_array(g_json_file,
					(void **)&g_scsi_cmd_array,
					sizeof(struct fuzz_iscsi_io_ctx),
					SCSI_IO_NAME, parse_scsi_cmds);
		if (g_scsi_cmd_array_size == 0) {
			fprintf(stderr, "The provided json file did not contain any valid commands. Exiting.\n");
			return -EINVAL;
		}
	}
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "j:S:t:", NULL, iscsi_fuzz_parse,
				      iscsi_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	rc = spdk_app_start(&opts, begin_iscsi_fuzz, NULL);

	return rc;
}
