/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/nvme_spec.h"
#include "spdk/nvme.h"
#include "spdk/likely.h"

#define IO_TIMEOUT_S 1

char *g_conf_file;
int g_app_rc;
bool g_run = false;
bool g_exit = false;
bool g_valid_ns_only = false;
bool g_verbose_mode = false;

struct nvme_fused_qp;

struct nvme_fused_ctx {
	int			index;
	struct nvme_fused_qp	*qp;
	bool			first_complete;
	bool			second_complete;
	struct			spdk_nvme_cpl cpl_first;
	struct			spdk_nvme_cpl cpl_second;
	struct			spdk_nvme_status status_first;
	struct			spdk_nvme_status status_second;
	uint8_t			*cmp_buf;
	uint8_t			*write_buf;
	int			rv;
	uint64_t		timeout_tsc;
	void	(*done)(struct nvme_fused_ctx *ctx);
	bool			is_done;
};

struct nvme_fused_trid {
	struct spdk_nvme_transport_id	trid;
	TAILQ_ENTRY(nvme_fused_trid)	tailq;
};

struct nvme_fused_ctrlr {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(nvme_fused_ctrlr)	tailq;
};

struct nvme_fused_qp {
	struct spdk_nvme_qpair          *qpair1;
	struct spdk_nvme_qpair          *qpair2;
	int				req_pending;
	int				req_num;
	struct nvme_fused_ctx           ctx[1024];
	uint64_t			timeout_tsc;
};

struct nvme_fused_ns {
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_thread		*thread;
	struct spdk_poller		*req_poller;
	uint32_t			nsid;
	struct nvme_fused_qp		qp;
	TAILQ_ENTRY(nvme_fused_ns)	tailq;
};

static TAILQ_HEAD(, nvme_fused_ns) g_ns_list = TAILQ_HEAD_INITIALIZER(g_ns_list);
static TAILQ_HEAD(, nvme_fused_ctrlr) g_ctrlr_list = TAILQ_HEAD_INITIALIZER(g_ctrlr_list);
static TAILQ_HEAD(, nvme_fused_trid) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

struct spdk_poller *g_app_completion_poller;
static int g_num_active_threads;
static int g_counter;

static void
nvme_fused_first_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fused_ctx *ctx = cb_arg;

	if (ctx->first_complete) {
		SPDK_ERRLOG("fused first command received twice\n");
		ctx->rv = -1;
		ctx->done(ctx);
		return;
	}

	memcpy(&ctx->cpl_first, cpl, sizeof(struct spdk_nvme_cpl));
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();

	ctx->first_complete = true;
	if (ctx->second_complete) {
		ctx->done(ctx);
	}
}

static void
nvme_fused_second_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fused_ctx *ctx = cb_arg;

	if (ctx->second_complete == true) {
		SPDK_ERRLOG("fused second command received twice\n");
		ctx->rv = -1;
		ctx->done(ctx);
		return;
	}

	memcpy(&ctx->cpl_second, cpl, sizeof(struct spdk_nvme_cpl));
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();

	ctx->second_complete = true;
	if (ctx->first_complete) {
		ctx->done(ctx);
	}
}

static bool
compare_status(struct spdk_nvme_status *status1, struct spdk_nvme_status *status2)
{

	return status1->sct == status2->sct && status1->sc == status2->sc;
}

static void
nvme_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_fused_ctx *ctx = cb_arg;

	memcpy(&ctx->cpl_first, cpl, sizeof(struct spdk_nvme_cpl));
	ctx->done(ctx);
}

static void
compare_and_write_done(struct nvme_fused_ctx *ctx)
{
	printf("Done request #%d (%d)\n", ctx->index, ctx->rv);
	printf("[First] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->cpl_first.status));
	printf("[Second] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->cpl_second.status));
	ctx->is_done = true;
	ctx->qp->req_pending--;

	if ((!compare_status(&ctx->cpl_first.status, &ctx->status_first) &&
	     ctx->status_first.sc != 0xFF && ctx->status_first.sct != 0x6) ||
	    (!compare_status(&ctx->cpl_second.status, &ctx->status_second) &&
	     ctx->status_second.sc != 0xFF && ctx->status_second.sct != 0x6)) {

		printf(" --> [First] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->status_first));
		printf(" --> [Second] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->status_second));

		g_app_rc = -1;
	}
}

static void
cmd_done(struct nvme_fused_ctx *ctx)
{
	printf("Done request #%d (%d)\n", ctx->index, ctx->rv);
	printf("[CMD] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->cpl_first.status));
	ctx->is_done = true;
	ctx->qp->req_pending--;

	if (!compare_status(&ctx->cpl_first.status, &ctx->status_first)) {
		printf(" --> [CMD] Status: %s\n", spdk_nvme_cpl_get_status_string(&ctx->status_first));
		g_app_rc = -1;
	}
}

static void *
fused_alloc(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	void *buf;

	buf = spdk_nvme_ctrlr_alloc_cmb_io_buffer(ctrlr, size);
	if (buf == NULL) {
		buf = spdk_zmalloc(size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	}

	return buf;
}

static struct nvme_fused_ctx *
fused_ctx_get(struct nvme_fused_ns *ns_entry, struct nvme_fused_qp *qp)
{
	struct nvme_fused_ctx *ctx;

	qp->req_pending++;
	ctx = &qp->ctx[qp->req_num++];
	ctx->index = qp->req_num - 1;
	ctx->qp = qp;

	ctx->cmp_buf = fused_alloc(ns_entry->ctrlr, 0x1000);
	if (ctx->cmp_buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		exit(-1);
	}

	ctx->write_buf = fused_alloc(ns_entry->ctrlr, 0x1000);
	if (ctx->write_buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		exit(-1);
	}

	return ctx;
}

static int
compare_and_write(void *arg)
{
	int rc;
	struct nvme_fused_ctx *ctx;
	struct nvme_fused_ctx *ctx2;
	struct nvme_fused_ns *ns_entry = (struct nvme_fused_ns *)arg;

	printf("Send NVMe commands.\n");
	memset(&ns_entry->qp.ctx, 0, sizeof(ns_entry->qp.ctx));

	/* Prepare device buffer (request #0) */
	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->done = cmd_done;
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;

	snprintf(ctx2->write_buf, 0x1000, "%s", "Starting buffer\n");

	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx2->write_buf,
				    0, /* LBA start */
				    1, /* number of LBAs */
				    nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}


	/*
	 * Case 1: Successful fused command (request #1)
	 */

	ctx = fused_ctx_get(ns_entry, &ns_entry->qp);

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Starting buffer\n");
	snprintf(ctx->write_buf, 0x1000, "%s", "Hello world!\n");

	ctx->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	/* Fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair1, ctx->cmp_buf,
				      0, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx->write_buf,
				    0, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/*
	 * Case 2: Reading pending fused op (request #3)
	 */

	/* Prepare device buffer (request #2) */
	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->done = cmd_done;
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;

	snprintf(ctx2->write_buf, 0x1000, "%s", "Starting buffer\n");

	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx2->write_buf,
				    1, /* LBA start */
				    1, /* number of LBAs */
				    nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Starting buffer\n");
	snprintf(ctx->write_buf, 0x1000, "%s", "2 fused commands\n");

	/* First fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair2, ctx->cmp_buf,
				      1, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* read operation request #4 */
	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = cmd_done;

	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qp.qpair1, ctx2->cmp_buf,
				   1, /* LBA start */
				   1, /* number of LBAs */
				   nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of first fused command */
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair2, ctx->write_buf,
				    1, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/*
	 * Case 3: fused op pending another fused op (for stress tests purpose only) -- request #4
	 */
#if 0
	ctx = fused_ctx_get(ns_entry, &ns_entry->qp);
	/* Results are unpredictable because we cannot guarantee the order of below operations */
	ctx->status_first.sc = 0xFF;
	ctx->status_first.sct = 0x6;
	ctx->status_second.sc = 0xFF;
	ctx->status_second.sct = 0x6;
	ctx->done = compare_and_write_done;

	snprintf(ctx->write_buf, 0x1000, "%s", "Next cmp\n");

	/* First fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair2, ctx->cmp_buf,
				      2, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->status_first.sc = 0xFF;
	ctx2->status_first.sct = 0x6;
	ctx2->status_second.sc = 0xFF;
	ctx2->status_second.sct = 0x6;
	ctx2->done = compare_and_write_done;

	snprintf(ctx2->cmp_buf, 0x1000, "%s", "Next cmp\n");

	/* Second fused compare and write operation (reques #5) */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair1, ctx2->cmp_buf,
				      2, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of first fused command */
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair2, ctx->write_buf,
				    2, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of second fused command */
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx2->write_buf,
				    2, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
#endif
	/*
	 * Case 4: Fused op pending another fused op (request #8)
	 */

	/* Prepare device buffer (request #7) */
	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->done = cmd_done;
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;

	snprintf(ctx2->write_buf, 0x1000, "%s", "Starting buffer\n");

	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx2->write_buf,
				    3, /* LBA start */
				    1, /* number of LBAs */
				    nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Starting buffer\n");
	snprintf(ctx->write_buf, 0x1000, "%s", "2 fused commands\n");

	/* First fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair2, ctx->cmp_buf,
				      3, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->status_first.sc = SPDK_NVME_SC_COMPARE_FAILURE;
	ctx2->status_first.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	ctx2->status_second.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	ctx2->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = compare_and_write_done;

	snprintf(ctx2->cmp_buf, 0x1000, "%s", "Wrong buffer\n");

	/* Second fused compare and write operation (request #8) */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair1, ctx2->cmp_buf,
				      3, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of first fused command */
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair2, ctx->write_buf,
				    3, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of second fused command */
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx2->write_buf,
				    3, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}


	/*
	 * Case 5: Fused compare didn't match (request #9)
	 */

	ctx = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx->status_first.sc = SPDK_NVME_SC_COMPARE_FAILURE;
	ctx->status_first.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	ctx->status_second.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Wrong buffer\n");

	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair2, ctx->cmp_buf,
				      4, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair2, ctx->write_buf,
				    4, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}


	/*
	 * Case 6: Fused commands not in sequence (request #10)
	 */

	ctx = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx->status_first.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	/* Fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair1, ctx->cmp_buf,
				      5, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = cmd_done;

	snprintf(ctx2->write_buf, 0x1000, "%s", "Not fused\n");

	/* Not fused write op (request #11) */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx2->write_buf,
				    5, /* LBA start */
				    1, /* number of LBAs */
				    nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	snprintf(ctx->write_buf, 0x1000, "%s", "Fused\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx->write_buf,
				    5, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = cmd_done;

	snprintf(ctx2->cmp_buf, 0x1000, "%s", "Not fused\n");

	/* Not fused compare op (request #12) */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair1, ctx2->cmp_buf,
				      5, /* LBA start */
				      1, /* number of LBAs */
				      nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}


	/*
	 * Case 7: Regions do not match (request #13)
	 */

	ctx = fused_ctx_get(ns_entry, &ns_entry->qp);
	ctx->status_first.sc = SPDK_NVME_SC_INVALID_FIELD;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, ns_entry->qp.qpair1, ctx->cmp_buf,
				      6, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qp.qpair1, ctx->write_buf,
				    7, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	printf("Done.\n");
	return 0;
}

static void
cleanup_queue(struct nvme_fused_qp *qp)
{
	int i;
	for (i = 0; i < qp->req_num; i++) {
		spdk_free(qp->ctx[i].cmp_buf);
		spdk_free(qp->ctx[i].write_buf);
	}
	qp->req_num = 0;
}

static int
poll_for_completions(void *arg)
{
	struct nvme_fused_ns *ns_entry = arg;
	int32_t rv;
	uint64_t current_ticks;
	int i;

	if (g_run) {

		if (ns_entry->qp.req_pending == 0) {
			cleanup_queue(&ns_entry->qp);
			compare_and_write(arg);
		}

		rv = spdk_nvme_qpair_process_completions(ns_entry->qp.qpair1, 0);
		if (rv < 0) {
			goto exit_handler;
		}

		rv = spdk_nvme_qpair_process_completions(ns_entry->qp.qpair2, 0);
		if (rv < 0) {
			goto exit_handler;
		}

		rv = spdk_nvme_ctrlr_process_admin_completions(ns_entry->ctrlr);
		if (rv < 0) {
			goto exit_handler;
		}

		current_ticks = spdk_get_ticks();
		if (rv > 0) {
			ns_entry->qp.timeout_tsc = current_ticks + IO_TIMEOUT_S * spdk_get_ticks_hz();
		}
		if (ns_entry->qp.timeout_tsc < current_ticks) {
			SPDK_NOTICELOG("Queue IO Timeout\n");
			goto exit_handler;
		}

		for (i = 0; i < ns_entry->qp.req_num; i++) {
			if (ns_entry->qp.ctx[i].is_done) {
				continue;
			}
			if (ns_entry->qp.ctx[i].rv < 0) {
				goto exit_handler;
			}
			if (ns_entry->qp.ctx[i].timeout_tsc < current_ticks) {
				SPDK_NOTICELOG("Request #%d IO Timeout\n", ns_entry->qp.ctx[i].index);
				goto exit_handler;
			}
		}
	}
	return 0;

exit_handler:

	g_counter--;
	if (g_counter > 0) {
		cleanup_queue(&ns_entry->qp);
		current_ticks = spdk_get_ticks();
		ns_entry->qp.timeout_tsc = current_ticks + IO_TIMEOUT_S * spdk_get_ticks_hz();
		return 0;
	}

	SPDK_NOTICELOG("Finishing queue IO poller\n");
	spdk_poller_unregister(&ns_entry->req_poller);
	__sync_sub_and_fetch(&g_num_active_threads, 1);
	spdk_thread_exit(ns_entry->thread);
	return 0;
}

static void
free_namespaces(void)
{
	struct nvme_fused_ns *ns, *tmp;

	TAILQ_FOREACH_SAFE(ns, &g_ns_list, tailq, tmp) {

		cleanup_queue(&ns->qp);

		if (ns->qp.qpair1) {
			spdk_nvme_ctrlr_free_io_qpair(ns->qp.qpair1);
		}
		if (ns->qp.qpair2) {
			spdk_nvme_ctrlr_free_io_qpair(ns->qp.qpair2);
		}

		TAILQ_REMOVE(&g_ns_list, ns, tailq);
		free(ns);
	}
}

static void
free_controllers(void)
{
	struct nvme_fused_ctrlr *ctrlr, *tmp;

	TAILQ_FOREACH_SAFE(ctrlr, &g_ctrlr_list, tailq, tmp) {
		TAILQ_REMOVE(&g_ctrlr_list, ctrlr, tailq);
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr);
	}
}

static void
free_trids(void)
{
	struct nvme_fused_trid *trid, *tmp;

	TAILQ_FOREACH_SAFE(trid, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid, tailq);
		free(trid);
	}
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns, uint32_t nsid)
{
	struct nvme_fused_ns *ns_entry;

	ns_entry = calloc(1, sizeof(struct nvme_fused_ns));
	if (ns_entry == NULL) {
		fprintf(stderr, "Unable to allocate an entry for a namespace\n");
		return;
	}

	ns_entry->ns = ns;
	ns_entry->ctrlr = ctrlr;
	ns_entry->nsid = nsid;

	TAILQ_INSERT_TAIL(&g_ns_list, ns_entry, tailq);
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_fused_ctrlr *ctrlr_entry;
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
#if 0
	if (!ctrlr->cdata->fuses.compare_and_write) {
		fprintf(stderr, "Controller doesn't support fused compare and write\n");
		return;
	}

	if (!ctrlr->cdata->oncs.compare) {
		fprintf(stderr, "Controller doesn't support compare\n");
		return;
	}
#endif

	ctrlr_entry = calloc(1, sizeof(struct nvme_fused_ctrlr));
	if (ctrlr_entry == NULL) {
		fprintf(stderr, "Unable to allocate an entry for a controller\n");
		return;
	}

	ctrlr_entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_ctrlr_list, ctrlr_entry, tailq);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns, nsid);
	}
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	register_ctrlr(ctrlr);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Controller trtype %s\ttraddr %s\ttrsvcid %s\n",
	       spdk_nvme_transport_id_trtype_str(trid->trtype),
	       trid->traddr, trid->trsvcid);

	return true;
}

static int
prepare_qpairs(void)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct nvme_fused_ns *ns_entry;

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &opts, sizeof(opts));
		ns_entry->qp.qpair1 = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
		ns_entry->qp.qpair2 = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
		if (ns_entry->qp.qpair1 == NULL || ns_entry->qp.qpair2 == NULL) {
			fprintf(stderr, "Unable to create a qpair for a namespace\n");
			return -1;
		}

	}
	return 0;
}

static void
start_ns_poller(void *ctx)
{
	struct nvme_fused_ns *ns_entry = ctx;

	ns_entry->qp.timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	ns_entry->req_poller = spdk_poller_register(poll_for_completions, ns_entry, 0);
}

static int
check_app_completion(void *ctx)
{
	if (g_num_active_threads <= 0) {
		spdk_poller_unregister(&g_app_completion_poller);
		printf("End of test\n");
		free_namespaces();
		free_controllers();
		free_trids();
		spdk_app_stop(g_app_rc);
	}
	return 1;
}

static void
begin_fused(void *ctx)
{
	struct nvme_fused_ns *ns_entry;
	struct nvme_fused_trid *trid;
	int rc;

	TAILQ_FOREACH(trid, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid->trid, trid, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid->trid.traddr);
			rc = -1;
			goto out;
		}
	}

	if (TAILQ_EMPTY(&g_ns_list)) {
		fprintf(stderr, "No valid NVMe Namespaces to fused\n");
		rc = -EINVAL;
		goto out;
	}

	rc = prepare_qpairs();

	if (rc < 0) {
		fprintf(stderr, "Unable to prepare the qpairs\n");
		goto out;
	}

	/* Assigning all of the threads and then starting them makes cleanup easier. */
	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		ns_entry->thread = spdk_thread_create(NULL, NULL);
		if (ns_entry->thread == NULL) {
			fprintf(stderr, "Failed to allocate thread for namespace.\n");
			goto out;
		}
	}

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_thread_send_msg(ns_entry->thread, start_ns_poller, ns_entry);
		__sync_add_and_fetch(&g_num_active_threads, 1);
	}

	g_app_completion_poller = spdk_poller_register(check_app_completion, NULL, 1000000);

	return;
out:
	printf("Shutting down the fused application\n");
	free_namespaces();
	free_controllers();
	free_trids();
	spdk_app_stop(rc);
}

static int
parse_trids(void)
{
	struct spdk_conf *config = NULL;
	struct spdk_conf_section *sp;
	const char *trid_char;
	struct nvme_fused_trid *current_trid;
	int num_subsystems = 0;
	int rc = 0;

	if (g_conf_file) {
		config = spdk_conf_allocate();
		if (!config) {
			fprintf(stderr, "Unable to allocate an spdk_conf object\n");
			return -1;
		}

		rc = spdk_conf_read(config, g_conf_file);
		if (rc) {
			fprintf(stderr, "Unable to convert the conf file into a readable system\n");
			rc = -1;
			goto exit;
		}

		sp = spdk_conf_find_section(config, "Nvme");

		if (sp == NULL) {
			fprintf(stderr, "No Nvme configuration in conf file\n");
			goto exit;
		}

		while ((trid_char = spdk_conf_section_get_nmval(sp, "TransportID", num_subsystems, 0)) != NULL) {
			current_trid = malloc(sizeof(struct nvme_fused_trid));
			if (!current_trid) {
				fprintf(stderr, "Unable to allocate memory for transport ID\n");
				rc = -1;
				goto exit;
			}
			rc = spdk_nvme_transport_id_parse(&current_trid->trid, trid_char);

			if (rc < 0) {
				fprintf(stderr, "failed to parse transport ID: %s\n", trid_char);
				free(current_trid);
				rc = -1;
				goto exit;
			}
			TAILQ_INSERT_TAIL(&g_trid_list, current_trid, tailq);
			num_subsystems++;
		}
	}

exit:
	if (config != NULL) {
		spdk_conf_free(config);
	}
	return rc;
}

static void
nvme_fused_usage(void)
{
	fprintf(stderr, " -C <path>                 Path to a configuration file.\n");
	fprintf(stderr, " -N                        Target only valid namespace with commands. \
This helps dig deeper into other errors besides invalid namespace.\n");
	fprintf(stderr,
		" -t <integer>              Time in seconds to run the fused test. Only valid if -j is not specified.\n");
	fprintf(stderr, " -V                        Enable logging of each submitted command.\n");
}

static int
nvme_fused_parse(int ch, char *arg)
{
	switch (ch) {
	case 'C':
		g_conf_file = optarg;
		break;
	case 'N':
		g_valid_ns_only = true;
		break;
	case 'V':
		g_verbose_mode = true;
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

	g_counter = 1000;

	spdk_app_opts_init(&opts);
	opts.name = "nvme_fused";

	g_run = true;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "aC:j:NS:t:V", NULL, nvme_fused_parse,
				      nvme_fused_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	if (g_conf_file) {
		parse_trids();
	}

	rc = spdk_app_start(&opts, begin_fused, NULL);

	return rc;
}
