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
#define SCSI_IO_NAME		"bhs"
#define GET_PDU_LOOP_COUNT	16

/* Global run state */
uint64_t	g_runtime_ticks;
int		g_runtime;
int		g_num_active_threads;
bool		g_run = true;
bool		g_is_valid_opcode = true;

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

struct fuzz_iscsi_iov_ctx {
	struct iovec			iov_req;
	struct iovec			iov_data;
	struct iovec			iov_resp;
};

struct fuzz_iscsi_io_ctx {
	struct fuzz_iscsi_iov_ctx		iovs;
	union {
		struct iscsi_bhs			*bhs;
		struct iscsi_bhs_nop_out	*nop_out_req;
		struct iscsi_bhs_scsi_req	*scsi_req;
		struct iscsi_bhs_task_req	*task_req;
		struct iscsi_bhs_login_req	*login_req;
		struct iscsi_bhs_text_req	*text_req;
		struct iscsi_bhs_data_out	*data_out_req;
		struct iscsi_bhs_logout_req	*logout_req;
		struct iscsi_bhs_snack_req	*snack_req;
	} req;
	union {
		struct iscsi_bhs_scsi_resp	*scsi_resp;
		struct iscsi_bhs_scsi_resp	*login_resp;
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

	pdu->ref = 1;

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
	char *data_segment_len;

	data_segment_len = fuzz_get_value_base_64_buffer((void *)io_ctx->req.bhs->data_segment_len,
			   sizeof(io_ctx->req.bhs->data_segment_len));

	spdk_json_write_named_uint32(w, "opcode", io_ctx->req.bhs->opcode);
	spdk_json_write_named_uint32(w, "immediate", io_ctx->req.bhs->immediate);
	spdk_json_write_named_uint32(w, "reserved", io_ctx->req.bhs->reserved);
	spdk_json_write_named_uint32(w, "total_ahs_len", io_ctx->req.bhs->total_ahs_len);
	spdk_json_write_named_string(w, "data_segment_len", data_segment_len);
	spdk_json_write_named_uint32(w, "itt", io_ctx->req.bhs->itt);
	spdk_json_write_named_uint32(w, "exp_stat_sn", io_ctx->req.bhs->exp_stat_sn);

	free(data_segment_len);
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
prep_iscsi_pdu_bhs_opcode_cmd(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx,
			      int opcode)
{
	switch (opcode) {
	case ISCSI_OP_NOPOUT:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.nop_out_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.nop_out_req, sizeof(io_ctx->req.nop_out_req),
				       &dev_ctx->random_seed);
		break;
	case ISCSI_OP_SCSI:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.scsi_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.scsi_req, sizeof(io_ctx->req.scsi_req),
				       &dev_ctx->random_seed);
		break;
	case ISCSI_OP_TASK:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.task_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.task_req, sizeof(io_ctx->req.task_req),
				       &dev_ctx->random_seed);
		break;
	case ISCSI_OP_LOGIN:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.login_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.login_req, sizeof(io_ctx->req.login_req),
				       &dev_ctx->random_seed);
		break;
	case ISCSI_OP_TEXT:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.text_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.text_req, sizeof(io_ctx->req.text_req),
				       &dev_ctx->random_seed);
		break;
	case ISCSI_OP_SCSI_DATAOUT:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.data_out_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.data_out_req, sizeof(io_ctx->req.data_out_req),
				       &dev_ctx->random_seed);
		break;
	case ISCSI_OP_LOGOUT:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.logout_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.logout_req, sizeof(io_ctx->req.logout_req),
				       &dev_ctx->random_seed);
		break;
	case ISCSI_OP_SNACK:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.snack_req);
		fuzz_fill_random_bytes((char *)io_ctx->req.snack_req, sizeof(io_ctx->req.snack_req),
				       &dev_ctx->random_seed);
		break;
	default:
		io_ctx->iovs.iov_req.iov_len = sizeof(io_ctx->req.bhs);
		fuzz_fill_random_bytes((char *)io_ctx->req.bhs, sizeof(io_ctx->req.bhs),
				       &dev_ctx->random_seed);
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

static void
iscsi_fuzz_sock_connect(struct spdk_iscsi_conn *conn)
{
	const char *host = g_tgt_ip;
	const char *port = "3260";
	char saddr[INET6_ADDRSTRLEN], caddr[INET6_ADDRSTRLEN];
	uint16_t cport, sport;
	int rc = 0;

	conn->sock = spdk_sock_connect(host, 3260);
	if (conn->sock == NULL) {
		fprintf(stderr, "connect error(%d): %s\n", errno, spdk_strerror(errno));
		spdk_sock_close(&conn->sock);
		return;
	}
	fprintf(stderr, "\nConnecting to the server on %s:%s\n", host, port);

	rc = spdk_sock_getaddr(conn->sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);
	if (rc < 0) {
		fprintf(stderr, "Cannot get connection addresses\n");
		spdk_sock_close(&conn->sock);
		return;
	}

	fprintf(stderr, "Connection accepted from (%s, %hu) to (%s, %hu)\n", caddr, cport, saddr, sport);

}

static void
iscsi_fuzz_handle_tgt_pdus(struct spdk_iscsi_conn *conn, uint8_t opcode)
{
	struct spdk_iscsi_pdu *rsp_pdu;
	struct iscsi_bhs_reject *rsph;
	int rc = 0;
	int i, looptimes;

	/* handle response pdu from Target */
	rsp_pdu = spdk_get_pdu();

	if (opcode == ISCSI_OP_LOGIN) {
		/* empty all login related rsp_pdu */
		looptimes = 10;
	} else {
		looptimes = 1;
	}

	for (i = 0; i < looptimes; i++) {
		rc = spdk_iscsi_conn_read_data(conn, ISCSI_BHS_LEN,
					       (uint8_t *)&rsp_pdu->bhs + rsp_pdu->bhs_valid_bytes);
		if (rc < 0) {
			fprintf(stderr, "spdk_iscsi_conn_read_data() error\n");
			return;
		}
	}

	if (opcode != ISCSI_OP_LOGIN) {
		/* Negative 1: unsupported opcode,	 */
		fprintf(stderr, "bhs.opcode of response PDU from Target is 0x%x\n", rsp_pdu->bhs.opcode);
		if (rsp_pdu->bhs.opcode == ISCSI_OP_REJECT) {
			rsph = (struct iscsi_bhs_reject *)&rsp_pdu->bhs;
			fprintf(stderr, "Reject PDU reason = %d\n", rsph->reason);
		}
		/* else Negative 2: supported opcode but param invalid */
	}

	spdk_put_pdu(rsp_pdu);
}

static void
dev_submit_requests(struct fuzz_iscsi_dev_ctx *dev_ctx, uint64_t max_io_to_submit)
{
	struct fuzz_iscsi_io_ctx *io_ctx = NULL;
	uint8_t opcode;
	uint64_t current_ticks;

	struct spdk_iscsi_pdu *req_pdu;

	struct iscsi_bhs_nop_out	*nop_out_req;
	struct iscsi_bhs_scsi_req	*scsi_req;
	struct iscsi_bhs_task_req	*task_req;
	struct iscsi_bhs_login_req	*login_req;
	struct iscsi_bhs_text_req	*text_req;
	struct iscsi_bhs_data_out	*data_out_req;
	struct iscsi_bhs_logout_req *logout_req;
	struct iscsi_bhs_snack_req	*snack_req;

	struct spdk_iscsi_sess sess = {};
	struct iscsi_param param = {};
	struct spdk_iscsi_conn *conn;

	conn = malloc(sizeof(*conn));
	memset(&sess, 0, sizeof(sess));
	memset(conn, 0, sizeof(*conn));
	memset(&param, 0, sizeof(param));

	sess.ExpCmdSN = 0;
	sess.MaxCmdSN = 64;
	sess.MaxBurstLength = SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
	sess.MaxOutstandingR2T = 1;
	sess.tag = 1;

	g_spdk_iscsi.MaxSessions = 256 * 2;// can be del
	g_spdk_iscsi.session = calloc(1, sizeof(void *) * g_spdk_iscsi.MaxSessions);
	g_spdk_iscsi.session[256 - 1] = &sess;
	sess.tsih = 256;
	sess.tag = 1;
	g_spdk_iscsi.AllowDuplicateIsid = false;


	conn->data_in_cnt = 0;
	conn->params = NULL;

	conn->full_feature = 0;
	conn->sess = &sess;

	conn->header_digest = true;
	conn->data_digest = false;
	conn->pdu_recv_state = ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR;
	conn->header_digest = 0;

	dev_ctx->scsi_dev = calloc(1, sizeof(*dev_ctx->scsi_dev));
	conn->dev = dev_ctx->scsi_dev;

	TAILQ_INIT(&conn->write_pdu_list);

	iscsi_fuzz_sock_connect(conn);
	usleep(1000);


	/* Discovery PDU */

	req_pdu = spdk_get_pdu();
	req_pdu->writev_offset = 0;
	req_pdu->hdigest_valid_bytes = ISCSI_DIGEST_LEN;
	req_pdu->ahs_valid_bytes = 0;
	req_pdu->data =
		"InitiatorName=iqn.1994-05.com.redhat:c7bb74ede1a\0SessionType=Discovery\0HeaderDigest=None\0DataDigest=None\0DefaultTime2Wait=2\0DefaultTime2Retain=0\0IFMarker=No\0OFMarker=No\0ErrorRecoveryLevel=0\0MaxRecvDataSegmentLength=32768\0";
	req_pdu->data_buf_len = 48;

	conn->sess->session_type = SESSION_TYPE_DISCOVERY;
	conn->MaxRecvDataSegmentLength = 8192;

	login_req = (struct iscsi_bhs_login_req *)&req_pdu->bhs;
	io_ctx = TAILQ_FIRST(&dev_ctx->free_io_ctx);
	io_ctx->req.login_req = login_req;
	io_ctx->req.login_req->version_min = 0;
	/* a new session */
	io_ctx->req.login_req->tsih = 0;

	req_pdu->bhs.opcode = ISCSI_OP_LOGIN;
	req_pdu->bhs.immediate = 1;
	req_pdu->bhs.reserved = 0;
	req_pdu->bhs_valid_bytes = ISCSI_BHS_LEN;
	req_pdu->bhs.total_ahs_len = 0;
	req_pdu->bhs.data_segment_len[0] = 0;
	req_pdu->bhs.data_segment_len[1] = 0;
	req_pdu->bhs.data_segment_len[2] = 251;
	req_pdu->bhs.flags = 135;

	spdk_iscsi_conn_write_pdu(conn, req_pdu);

	while (!TAILQ_EMPTY(&dev_ctx->free_io_ctx) && dev_ctx->submitted_io < max_io_to_submit) {
		iscsi_fuzz_sock_connect(conn);
		usleep(100000);

		/* Login PDU */

		req_pdu = spdk_get_pdu();
		req_pdu->writev_offset = 0;
		req_pdu->hdigest_valid_bytes = ISCSI_DIGEST_LEN;
		req_pdu->ahs_valid_bytes = 0;
		req_pdu->data =
			"InitiatorName=iqn.1994-05.com.redhat:c7bb74ede1a\0TargetName=iqn.2016-06.io.spdk:disk1\0SessionType=Normal\0HeaderDigest=None\0DataDigest=None\0DefaultTime2Wait=2\0DefaultTime2Retain=0\0IFMarker=No\0OFMarker=No\0ErrorRecoveryLevel=0\0InitialR2T=No\0ImmediateData=Yes\0MaxBurstLength=16776192\0FirstBurstLength=262144\0MaxOutstandingR2T=1\0MaxConnections=1\0DataPDUInOrder=Yes\0DataSequenceInOrder=Yes\0MaxRecvDataSegmentLength=262144\0";
		req_pdu->data_buf_len = 48;

		conn->sess->session_type = SESSION_TYPE_NORMAL;

		login_req = (struct iscsi_bhs_login_req *)&req_pdu->bhs;
		io_ctx = TAILQ_FIRST(&dev_ctx->free_io_ctx);
		io_ctx->req.login_req = login_req;
		io_ctx->req.login_req->version_min = 0;
		io_ctx->req.login_req->tsih = 0;

		req_pdu->bhs.opcode = ISCSI_OP_LOGIN;
		req_pdu->bhs.immediate = 1;
		req_pdu->bhs.reserved = 0;
		req_pdu->bhs_valid_bytes = ISCSI_BHS_LEN;
		req_pdu->bhs.total_ahs_len = 0;
		req_pdu->bhs.data_segment_len[0] = 0;
		req_pdu->bhs.data_segment_len[1] = 1;
		req_pdu->bhs.data_segment_len[2] = 190;
		req_pdu->bhs.flags = 135;

		spdk_iscsi_conn_write_pdu(conn, req_pdu);
		usleep(100000);
		iscsi_fuzz_handle_tgt_pdus(conn, ISCSI_OP_LOGIN);

		/* Random PDU */

		req_pdu = spdk_get_pdu();
		req_pdu->writev_offset = 0;
		req_pdu->hdigest_valid_bytes = ISCSI_DIGEST_LEN;
		req_pdu->ahs_valid_bytes = 0;
		req_pdu->data_buf_len = 0;

		conn->sess->session_type = SESSION_TYPE_NORMAL;

		io_ctx = TAILQ_FIRST(&dev_ctx->free_io_ctx);
		req_pdu->bhs.opcode = rand() % 0xff;
		opcode = req_pdu->bhs.opcode;
		switch (req_pdu->bhs.opcode) {
		case ISCSI_OP_NOPOUT:
			nop_out_req = (struct iscsi_bhs_nop_out *)&req_pdu->bhs;
			io_ctx->req.nop_out_req = nop_out_req;
			/* TODO: fill valid param */
			break;
		case ISCSI_OP_SCSI:
			scsi_req = (struct iscsi_bhs_scsi_req *)&req_pdu->bhs;
			io_ctx->req.scsi_req = scsi_req;
			io_ctx->req.scsi_req->read_bit = 0;
			io_ctx->req.scsi_req->write_bit = 0;
			break;
		case ISCSI_OP_TASK:
			task_req = (struct iscsi_bhs_task_req *)&req_pdu->bhs;
			io_ctx->req.task_req = task_req;
			/* TODO: fill valid param */
			break;
		case ISCSI_OP_LOGIN:
			login_req = (struct iscsi_bhs_login_req *)&req_pdu->bhs;
			io_ctx->req.login_req = login_req;
			/* TODO: fill valid param */
			break;
		case ISCSI_OP_TEXT:
			text_req = (struct iscsi_bhs_text_req *)&req_pdu->bhs;
			io_ctx->req.text_req = text_req;
			/* TODO: fill valid param */
			break;
		case ISCSI_OP_SCSI_DATAOUT:
			data_out_req = (struct iscsi_bhs_data_out *)&req_pdu->bhs;
			io_ctx->req.data_out_req = data_out_req;
			/* TODO: fill valid param */
			break;
		case ISCSI_OP_LOGOUT:
			logout_req = (struct iscsi_bhs_logout_req *)&req_pdu->bhs;
			io_ctx->req.logout_req = logout_req;
			/* TODO: fill valid param */
			break;
		case ISCSI_OP_SNACK:
			snack_req = (struct iscsi_bhs_snack_req *)&req_pdu->bhs;
			io_ctx->req.snack_req = snack_req;
			/* TODO: fill valid param */
			break;
		default:
			g_is_valid_opcode = false;
			break;
		}
		prep_iscsi_pdu_bhs_opcode_cmd(dev_ctx, io_ctx, req_pdu->bhs.opcode);

		io_ctx->req.bhs->opcode = opcode;
		req_pdu->bhs.opcode = opcode;
		fprintf(stderr,	"Random request bhs.opcode of Initiator is 0x%x.\n", req_pdu->bhs.opcode);
		req_pdu->bhs.immediate = 1;
		req_pdu->bhs.reserved = 0;
		req_pdu->bhs_valid_bytes = ISCSI_BHS_LEN;
		req_pdu->bhs.total_ahs_len = 0;
		req_pdu->bhs.data_segment_len[0] = 0;
		req_pdu->bhs.data_segment_len[1] = 0;
		req_pdu->bhs.data_segment_len[2] = 0;

		if (io_ctx) {
			TAILQ_REMOVE(&dev_ctx->outstanding_io_ctx, io_ctx, link);
			TAILQ_INSERT_HEAD(&dev_ctx->free_io_ctx, io_ctx, link);
			check_successful_op(dev_ctx, io_ctx);
			dev_ctx->completed_io++;
			dev_ctx->timeout_tsc = fuzz_refresh_timeout();
		}

		spdk_iscsi_conn_write_pdu(conn, req_pdu);
		usleep(100000);

		iscsi_fuzz_handle_tgt_pdus(conn, opcode);

		fprintf(stderr,	"Initiator will connect to Target again...\n");
		spdk_sock_close(&conn->sock);
		usleep(1000);

		current_ticks = spdk_get_ticks();
		if (current_ticks > g_runtime_ticks) {
			g_run = false;
			spdk_sock_close(&conn->sock);
			free(conn);
			return;
		}

		g_is_valid_opcode = true;

	}

	spdk_sock_close(&conn->sock);
	free(conn);

}

/* submit requests end */

/* complete requests begin */
static void
check_successful_op(struct fuzz_iscsi_dev_ctx *dev_ctx, struct fuzz_iscsi_io_ctx *io_ctx)
{
	if (g_is_valid_opcode) {
		fprintf(stderr, "The PDU transmission completed with an valid bhs opcode.\n");
		dev_ctx->successful_io++;
	} else {
		fprintf(stderr, "The PDU transmission failed as expected.\n");
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
	fprintf(stderr, " -T <path>                 iSCSI Target IP address.\n");
	fprintf(stderr, " -I <path>                 iSCSI Initiator IP address.\n");
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
	case 'T':
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
	srand((unsigned)time(0));

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
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "T:j:S:t:", NULL, iscsi_fuzz_parse,
				      iscsi_fuzz_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	rc = spdk_app_start(&opts, begin_iscsi_fuzz, NULL);

	return rc;
}
