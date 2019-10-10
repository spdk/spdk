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
#include "spdk/sock.h"


#define UNIQUE_OPCODES		256
#define FUZZ_QUEUE_DEPTH	128
#define SCSI_IO_NAME		"scsi_cmd"
#define GET_PDU_LOOP_COUNT	16

int valid_opcode_list[11] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10, 0x1c, 0x1d, 0x1e};

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
char					*g_tgt_ip = "127.0.0.1";

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
		struct iscsi_bhs_scsi_req	*scsi_req;
	} req;
	union {
		struct iscsi_bhs_scsi_resp	*scsi_resp;
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
spdk_iscsi_fuzz_mempool_get(struct spdk_mempool *_mp);
void
spdk_iscsi_fuzz_mempool_put(struct spdk_mempool *_mp, void *ele);
struct spdk_scsi_lun *
	spdk_scsi_fuzz_lun_construct(struct spdk_bdev *bdev,
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
		if (pdu->data && !pdu->data_from_mempool) {
			free(pdu->data);
		}
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

	g_spdk_iscsi.task_pool = (struct spdk_mempool *)pdu;
	g_spdk_iscsi.pdu_pool = (struct spdk_mempool *)pdu;

	pdu->data = malloc(sizeof(*pdu->data));
	if (!pdu->data) {
		return NULL;
	}

	pdu->ref = 1;
	TAILQ_INSERT_TAIL(&g_get_pdu_list, pdu, tailq);

	return pdu;
}

struct test_mempool {
	size_t	count;
	size_t	ele_size;
};

void *
spdk_iscsi_fuzz_mempool_get(struct spdk_mempool *_mp)
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

void
spdk_iscsi_fuzz_mempool_put(struct spdk_mempool *_mp, void *ele)
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
	spdk_iscsi_fuzz_mempool_put(g_spdk_iscsi.task_pool, (void *)task);
}

struct spdk_iscsi_task *
spdk_iscsi_task_get(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *parent,
		    spdk_scsi_task_cpl cpl_fn)
{
	struct spdk_iscsi_task *task;

	task = spdk_iscsi_fuzz_mempool_get(g_spdk_iscsi.task_pool);
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
	spdk_scsi_fuzz_lun_construct(struct spdk_bdev *bdev,
			     void (*hotremove_cb)(const struct spdk_scsi_lun *, void *),
			     void *hotremove_ctx)
{
	struct spdk_scsi_lun *lun;

	lun = calloc(1, sizeof(struct spdk_scsi_lun));
	assert(lun != NULL);

	lun->bdev = bdev;

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
	char *res, *data_segment_len, *cdb;

	res = fuzz_get_value_base_64_buffer((void *)io_ctx->req.scsi_req->res,
					    sizeof(io_ctx->req.scsi_req->res));
	data_segment_len = fuzz_get_value_base_64_buffer((void *)io_ctx->req.scsi_req->data_segment_len,
			   sizeof(io_ctx->req.scsi_req->data_segment_len));
	cdb = fuzz_get_value_base_64_buffer((void *)io_ctx->req.scsi_req->cdb,
					    sizeof(io_ctx->req.scsi_req->cdb));

	spdk_json_write_named_uint32(w, "opcode", io_ctx->req.scsi_req->opcode);
	spdk_json_write_named_uint32(w, "immediate", io_ctx->req.scsi_req->immediate);
	spdk_json_write_named_uint32(w, "reserved", io_ctx->req.scsi_req->reserved);
	spdk_json_write_named_uint32(w, "attribute", io_ctx->req.scsi_req->attribute);
	spdk_json_write_named_uint32(w, "reserved2", io_ctx->req.scsi_req->reserved2);
	spdk_json_write_named_uint32(w, "write_bit", io_ctx->req.scsi_req->write_bit);
	spdk_json_write_named_uint32(w, "read_bit", io_ctx->req.scsi_req->read_bit);
	spdk_json_write_named_uint32(w, "final_bit", io_ctx->req.scsi_req->final_bit);
	spdk_json_write_named_string(w, "res_data", res);
	spdk_json_write_named_uint32(w, "total_ahs_len", io_ctx->req.scsi_req->total_ahs_len);
	spdk_json_write_named_string(w, "data_segment_len", data_segment_len);
	spdk_json_write_named_uint64(w, "lun", io_ctx->req.scsi_req->lun);
	spdk_json_write_named_uint32(w, "itt", io_ctx->req.scsi_req->itt);
	spdk_json_write_named_uint32(w, "expected_data_xfer_len",
				     io_ctx->req.scsi_req->expected_data_xfer_len);
	spdk_json_write_named_uint32(w, "cmd_sn", io_ctx->req.scsi_req->cmd_sn);
	spdk_json_write_named_uint32(w, "exp_stat_sn", io_ctx->req.scsi_req->exp_stat_sn);
	spdk_json_write_named_string(w, "cdb", cdb);

	free(res);
	free(data_segment_len);
	free(cdb);
}

static void
print_req_obj(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx)
{
	struct spdk_json_write_ctx *w;

	w = spdk_json_write_begin(dump_iscsi_cmd, NULL, SPDK_JSON_WRITE_FLAG_FORMATTED);
	spdk_json_write_named_object_begin(w, SCSI_IO_NAME);
	print_scsi_io_data(w, io_ctx);
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
static uint64_t
get_max_num_io(struct fuzz_iscsi_dev_ctx *dev_ctx)
{
	return g_scsi_cmd_array_size;
}
struct spdk_bdev {
	char name[100];
};

static int
iscsi_fuzz_conn_flush_pdus_internal(struct spdk_iscsi_conn *conn)
{
	const int num_iovs = 32;
	struct iovec iovs[num_iovs];
	struct iovec *iov = iovs;
	int iovcnt = 0;
	int bytes = 0;
	uint32_t total_length = 0;
	uint32_t mapped_length = 0;
	struct spdk_iscsi_pdu *pdu;

	pdu = TAILQ_FIRST(&g_get_pdu_list);

	if (pdu == NULL) {
		return 0;
	}

	/*
	 * Build up a list of iovecs for the first few PDUs in the
	 *  connection's write_pdu_list. For the first PDU, check if it was
	 *  partially written out the last time this function was called, and
	 *  if so adjust the iovec array accordingly. This check is done in
	 *  spdk_iscsi_build_iovs() and so applied to remaining PDUs too.
	 *  But extra overhead is negligible.
	 */
	while (pdu != NULL && ((num_iovs - iovcnt) > 0)) {
		iovcnt += spdk_iscsi_build_iovs(conn, &iovs[iovcnt], num_iovs - iovcnt,
						pdu, &mapped_length);
		total_length += mapped_length;
		pdu = TAILQ_NEXT(pdu, tailq);
	}
	spdk_trace_record(TRACE_ISCSI_FLUSH_WRITEBUF_START, conn->id, total_length, 0, iovcnt);

	bytes = spdk_sock_writev(conn->sock, iov, 1);
	if (bytes == -1) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return 1;
		} else {
			SPDK_ERRLOG("spdk_sock_writev() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
			return -1;
		}
	}

	spdk_trace_record(TRACE_ISCSI_FLUSH_WRITEBUF_DONE, conn->id, bytes, 0, 0);

	return 0;
}

static void
dev_submit_requests(struct fuzz_iscsi_dev_ctx *dev_ctx, uint64_t max_io_to_submit)
{
	struct fuzz_iscsi_io_ctx *io_ctx = NULL;
	int rc = 0;
	int i;
	uint64_t current_ticks;

	struct spdk_iscsi_pdu *req_pdu;
	struct spdk_iscsi_pdu *rsp_pdu;
	struct spdk_iscsi_pdu *entry_pdu;
	struct spdk_iscsi_sess sess = {};
	struct spdk_iscsi_conn conn = {};
	struct spdk_scsi_lun *lun;
	struct iscsi_bhs_scsi_req *scsi_req;
	struct spdk_bdev		*bdev;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(&lun, 0, sizeof(lun));

	sess.ExpCmdSN = 0;
	sess.MaxCmdSN = 64;
	sess.session_type = SESSION_TYPE_NORMAL;
	sess.MaxBurstLength = 1024;

	TAILQ_INIT(&conn.active_r2t_tasks);
	conn.data_in_cnt = 0;
	conn.params = NULL;

	conn.full_feature = 0;
	conn.sess = &sess;
	conn.state = ISCSI_CONN_STATE_RUNNING;

	conn.header_digest = true;
	conn.data_digest = true;
	conn.pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR;

	bdev = spdk_bdev_get_by_name("Malloc0");
	dev_ctx->scsi_dev = calloc(1, sizeof(*dev_ctx->scsi_dev));
	conn.dev = dev_ctx->scsi_dev;
	lun = spdk_scsi_fuzz_lun_construct(bdev, NULL, NULL);
	if (lun == NULL) {
		printf("spdk_scsi_lun_construct is failed.\n");
		return;
	}
	lun->id = 0;
	lun->dev = dev_ctx->scsi_dev;
	dev_ctx->scsi_dev->lun[0] = lun;

	TAILQ_INIT(&conn.write_pdu_list);
	TAILQ_INIT(&conn.snack_pdu_list);
	TAILQ_INIT(&conn.queued_r2t_tasks);
	TAILQ_INIT(&conn.active_r2t_tasks);
	TAILQ_INIT(&conn.queued_datain_tasks);
	TAILQ_INIT(&g_get_pdu_list);

	const char *host = g_tgt_ip;
	const char *port = "3260";
	char saddr[INET6_ADDRSTRLEN], caddr[INET6_ADDRSTRLEN];
	uint16_t cport, sport;

	while (!TAILQ_EMPTY(&dev_ctx->free_io_ctx) && dev_ctx->submitted_io < max_io_to_submit) {
		conn.sock = spdk_sock_connect(host, 3260);
		if (conn.sock == NULL) {
			fprintf(stderr, "connect error(%d): %s\n", errno, spdk_strerror(errno));
			spdk_sock_close(&conn.sock);
			return;
		}
		fprintf(stderr, "Connecting to the server on %s:%s\n", host, port);

		rc = spdk_sock_getaddr(conn.sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);
		if (rc < 0) {
			fprintf(stderr, "Cannot get connection addresses\n");
			spdk_sock_close(&conn.sock);
			return;
		}
		fprintf(stderr, "Connection accepted from (%s, %hu) to (%s, %hu)\n", caddr, cport, saddr, sport);
		usleep(1000);

		req_pdu = spdk_get_pdu();
		req_pdu->writev_offset = 0;
		req_pdu->hdigest_valid_bytes = ISCSI_DIGEST_LEN;
		req_pdu->ahs_valid_bytes = 0;

		conn.pdu_in_progress = req_pdu;
		conn.header_digest = 1;

		scsi_req = (struct iscsi_bhs_scsi_req *)&req_pdu->bhs;
		io_ctx = TAILQ_FIRST(&dev_ctx->free_io_ctx);
		io_ctx->req.scsi_req = scsi_req;
		prep_iscsi_pdu_bhs_opcode_cmd(dev_ctx, io_ctx);

		req_pdu->bhs_valid_bytes = ISCSI_BHS_LEN;
		req_pdu->bhs.total_ahs_len = 0;
		req_pdu->data_segment_len = 0;

		rc = iscsi_fuzz_conn_flush_pdus_internal(&conn);
		if (rc == 0) {
			TAILQ_REMOVE(&dev_ctx->free_io_ctx, io_ctx, link);
			TAILQ_INSERT_TAIL(&dev_ctx->outstanding_io_ctx, io_ctx, link);
			dev_ctx->submitted_io++;
		} else {
			fprintf(stderr, "iscsi_pdu_hdr_handle() fatal error.\n");
		}

		g_is_valid_opcode = false;
		for (i = 0 ; i < 11 ; i++) {
			if (valid_opcode_list[ i ] == req_pdu->bhs.opcode) {
				g_is_valid_opcode = true;
				break;
			}
		}

		if (io_ctx) {
			TAILQ_REMOVE(&dev_ctx->outstanding_io_ctx, io_ctx, link);
			TAILQ_INSERT_HEAD(&dev_ctx->free_io_ctx, io_ctx, link);
			check_successful_op(dev_ctx, io_ctx);
			dev_ctx->completed_io++;
			dev_ctx->timeout_tsc = fuzz_refresh_timeout();
		}

		TAILQ_FOREACH_SAFE(req_pdu, &g_get_pdu_list, tailq, entry_pdu) {
			TAILQ_REMOVE(&g_get_pdu_list, req_pdu, tailq);
			spdk_put_pdu(req_pdu);
		}

		TAILQ_FOREACH_SAFE(rsp_pdu, &conn.write_pdu_list, tailq, entry_pdu) {
			TAILQ_REMOVE(&conn.write_pdu_list, rsp_pdu, tailq);
			spdk_put_pdu(rsp_pdu);
		}

		current_ticks = spdk_get_ticks();
		if (current_ticks > g_runtime_ticks) {
			g_run = false;
			if (lun) {
				free(lun);
			}
			spdk_sock_close(&conn.sock);
			return;
		}
		spdk_sock_close(&conn.sock);
	}

	/* internal of delete_portal_group */
	spdk_io_device_unregister(&g_spdk_iscsi, NULL);

	if (lun) {
		free(lun);
	}
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
			if (!strncmp(prev_value->start, "lun", prev_value->len)) {
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
	fprintf(stderr, " -D <path>                 iSCSI Target IP address.\n");
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
	case 'D':
		g_tgt_ip = optarg;
		break;
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
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "D:j:S:t:", NULL, iscsi_fuzz_parse,
				      iscsi_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	rc = spdk_app_start(&opts, begin_iscsi_fuzz, NULL);

	return rc;
}
