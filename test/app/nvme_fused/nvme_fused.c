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
int g_app_rc = 0;
bool g_exit = false;
bool g_valid_ns_only = false;
bool g_verbose_mode = false;
int g_workers_per_ns = 10;
int g_counter = 100;

struct nvme_fused_worker;
struct nvme_fused_ns;

struct nvme_fused_ctx {
	int			index;
	struct nvme_fused_worker	*worker;
	bool			first_complete;
	bool			second_complete;
	struct			spdk_nvme_cpl cpl_first;
	struct			spdk_nvme_cpl cpl_second;
	struct			spdk_nvme_status status_first;
	struct			spdk_nvme_status status_second;
	uint8_t			*cmp_buf;
	uint8_t			*write_buf;
	int			rv;
	uint64_t			timeout_tsc;
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

struct nvme_fused_worker {
	int				index;
	int				counter;
	struct spdk_thread		*thread;
	struct nvme_fused_ns		*ns;
	struct spdk_poller		*req_poller;
	struct spdk_nvme_qpair          *qpair1;
	struct spdk_nvme_qpair          *qpair2;
	int				req_pending;
	int				req_num;
	struct nvme_fused_ctx           ctx[1024];
};

struct nvme_fused_ns {
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_ctrlr		*ctrlr;
	uint32_t			nsid;
	struct nvme_fused_worker	worker[1024];
	TAILQ_ENTRY(nvme_fused_ns)	tailq;
};

static TAILQ_HEAD(, nvme_fused_ns) g_ns_list = TAILQ_HEAD_INITIALIZER(g_ns_list);
static TAILQ_HEAD(, nvme_fused_ctrlr) g_ctrlr_list = TAILQ_HEAD_INITIALIZER(g_ctrlr_list);
static TAILQ_HEAD(, nvme_fused_trid) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

struct spdk_poller *g_app_completion_poller;
static int g_num_active_threads;

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
	if ((!compare_status(&ctx->cpl_first.status, &ctx->status_first) &&
	     ctx->status_first.sc != 0xFF && ctx->status_first.sct != 0x6) ||
	    (!compare_status(&ctx->cpl_second.status, &ctx->status_second) &&
	     ctx->status_second.sc != 0xFF && ctx->status_second.sct != 0x6)) {

		printf("Incorrect status for request #%d (%d)\n", ctx->index, ctx->rv);
		printf(" --> [First] Status: %s (%s)\n", spdk_nvme_cpl_get_status_string(&ctx->status_first),
		       spdk_nvme_cpl_get_status_string(&ctx->cpl_first.status));
		printf(" --> [Second] Status: %s (%s)\n", spdk_nvme_cpl_get_status_string(&ctx->status_second),
		       spdk_nvme_cpl_get_status_string(&ctx->cpl_second.status));

		g_app_rc = -1;
		ctx->rv = -1;
	}

	ctx->is_done = true;
	ctx->worker->req_pending--;
}

static void
cmd_done(struct nvme_fused_ctx *ctx)
{
	if (!compare_status(&ctx->cpl_first.status, &ctx->status_first)) {
		printf("Incorrect status for request #%d (%d)\n", ctx->index, ctx->rv);
		printf(" --> [CMD] Status: %s (%s)\n", spdk_nvme_cpl_get_status_string(&ctx->status_first),
		       spdk_nvme_cpl_get_status_string(&ctx->cpl_first.status));
		g_app_rc = -1;
		ctx->rv = -1;
	}

	ctx->is_done = true;
	ctx->worker->req_pending--;
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
fused_ctx_get(struct nvme_fused_worker *worker)
{
	struct nvme_fused_ctx *ctx;

	worker->req_pending++;
	ctx = &worker->ctx[worker->req_num++];
	ctx->index = worker->req_num - 1;
	ctx->worker = worker;

	ctx->cmp_buf = fused_alloc(worker->ns->ctrlr, 0x1000);
	if (ctx->cmp_buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		exit(-1);
	}

	ctx->write_buf = fused_alloc(worker->ns->ctrlr, 0x1000);
	if (ctx->write_buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		exit(-1);
	}

	return ctx;
}

struct write_ctx {
	struct nvme_fused_worker *worker;
	void *buffer;
};

/*
 * Case 1: Successful fused command
 */
static void
test_case_1_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct write_ctx *write_ctx = cb_arg;
	struct nvme_fused_worker *worker = write_ctx->worker;
	struct nvme_fused_ctx *ctx;
	uint64_t first_lba = worker->index * 8;
	int rc;

	spdk_free(write_ctx->buffer);
	free(write_ctx);

	ctx = fused_ctx_get(worker);

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Starting buffer\n");
	snprintf(ctx->write_buf, 0x1000, "%s", "Hello world!\n");

	ctx->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	/* Fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(worker->ns->ns, worker->qpair1, ctx->cmp_buf,
				      first_lba, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	rc = spdk_nvme_ns_cmd_write(worker->ns->ns, worker->qpair1, ctx->write_buf,
				    first_lba, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
}

/*
 * Case 2: Reading pending fused op
 */
static void
test_case_2_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct write_ctx *write_ctx = cb_arg;
	struct nvme_fused_worker *worker = write_ctx->worker;
	struct nvme_fused_ctx *ctx, *ctx2;
	uint64_t first_lba = worker->index * 8;
	int rc;

	spdk_free(write_ctx->buffer);
	free(write_ctx);

	ctx = fused_ctx_get(worker);
	ctx->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Starting buffer\n");
	snprintf(ctx->write_buf, 0x1000, "%s", "2 fused commands\n");

	/* First fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(worker->ns->ns, worker->qpair2, ctx->cmp_buf,
				      first_lba + 1, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* read operation */
	ctx2 = fused_ctx_get(worker);
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = cmd_done;

	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_read(worker->ns->ns, worker->qpair1, ctx2->cmp_buf,
				   first_lba + 1, /* LBA start */
				   1, /* number of LBAs */
				   nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of first fused command */
	rc = spdk_nvme_ns_cmd_write(worker->ns->ns, worker->qpair2, ctx->write_buf,
				    first_lba + 1, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
}

/*
 * Case 3: fused op pending another fused op (for stress tests purpose only)
 */
static void
test_case_3_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct write_ctx *write_ctx = cb_arg;
	struct nvme_fused_worker *worker = write_ctx->worker;
	struct nvme_fused_ctx *ctx, *ctx2;
	uint64_t first_lba = worker->index * 8;
	int rc;

	spdk_free(write_ctx->buffer);
	free(write_ctx);

	ctx = fused_ctx_get(worker);
	/* Results are unpredictable because we cannot guarantee the order of below operations */
	ctx->status_first.sc = 0xFF;
	ctx->status_first.sct = 0x6;
	ctx->status_second.sc = 0xFF;
	ctx->status_second.sct = 0x6;
	ctx->done = compare_and_write_done;

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Starting buffer\n");
	snprintf(ctx->write_buf, 0x1000, "%s", "Next cmp\n");

	/* First fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(worker->ns->ns, worker->qpair2, ctx->cmp_buf,
				      first_lba + 2, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(worker);
	ctx2->status_first.sc = 0xFF;
	ctx2->status_first.sct = 0x6;
	ctx2->status_second.sc = 0xFF;
	ctx2->status_second.sct = 0x6;
	ctx2->done = compare_and_write_done;

	snprintf(ctx2->cmp_buf, 0x1000, "%s", "Next cmp\n");

	/* Second fused compare and write operation */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(worker->ns->ns, worker->qpair1, ctx2->cmp_buf,
				      first_lba + 2, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of first fused command */
	rc = spdk_nvme_ns_cmd_write(worker->ns->ns, worker->qpair2, ctx->write_buf,
				    first_lba + 2, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of second fused command */
	rc = spdk_nvme_ns_cmd_write(worker->ns->ns, worker->qpair1, ctx2->write_buf,
				    first_lba + 2, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
}

/*
 * Case 4: Fused op pending another fused op
 */
static void
test_case_4_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct write_ctx *write_ctx = cb_arg;
	struct nvme_fused_worker *worker = write_ctx->worker;
	struct nvme_fused_ctx *ctx, *ctx2;
	uint64_t first_lba = worker->index * 8;
	int rc;

	spdk_free(write_ctx->buffer);
	free(write_ctx);

	ctx = fused_ctx_get(worker);
	ctx->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Starting buffer\n");
	snprintf(ctx->write_buf, 0x1000, "%s", "2 fused commands\n");

	/* First fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(worker->ns->ns, worker->qpair2, ctx->cmp_buf,
				      first_lba + 3, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(worker);
	ctx2->status_first.sc = SPDK_NVME_SC_COMPARE_FAILURE;
	ctx2->status_first.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	ctx2->status_second.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	ctx2->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = compare_and_write_done;

	snprintf(ctx2->cmp_buf, 0x1000, "%s", "Wrong buffer\n");

	/* Second fused compare and write operation */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(worker->ns->ns, worker->qpair1, ctx2->cmp_buf,
				      first_lba + 3, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of first fused command */
	rc = spdk_nvme_ns_cmd_write(worker->ns->ns, worker->qpair2, ctx->write_buf,
				    first_lba + 3, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Second part of second fused command */
	rc = spdk_nvme_ns_cmd_write(worker->ns->ns, worker->qpair1, ctx2->write_buf,
				    first_lba + 3, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx2, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
}

/*
 * Case 5: Fused compare didn't match
 */
static void
test_case_5_cpl_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct write_ctx *write_ctx = cb_arg;
	struct nvme_fused_worker *worker = write_ctx->worker;
	struct nvme_fused_ctx *ctx;
	uint64_t first_lba = worker->index * 8;
	int rc;

	spdk_free(write_ctx->buffer);
	free(write_ctx);

	ctx = fused_ctx_get(worker);
	ctx->status_first.sc = SPDK_NVME_SC_COMPARE_FAILURE;
	ctx->status_first.sct = SPDK_NVME_SCT_MEDIA_ERROR;
	ctx->status_second.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	snprintf(ctx->cmp_buf, 0x1000, "%s", "Wrong buffer\n");

	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(worker->ns->ns, worker->qpair2, ctx->cmp_buf,
				      first_lba + 4, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	rc = spdk_nvme_ns_cmd_write(worker->ns->ns, worker->qpair2, ctx->write_buf,
				    first_lba + 4, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
}

static int
compare_and_write(void *arg)
{
	int rc;
	struct nvme_fused_ctx *ctx;
	struct nvme_fused_ctx *ctx2;
	struct nvme_fused_worker *worker = (struct nvme_fused_worker *)arg;
	struct nvme_fused_ns *ns_entry = worker->ns;
	uint64_t first_lba = worker->index * 8;
	struct write_ctx *write_ctx;

	memset(&worker->ctx, 0, sizeof(ns_entry->worker[0].ctx));

	/* Start case 1 */
	write_ctx = malloc(sizeof(struct write_ctx));
	write_ctx->worker = worker;
	write_ctx->buffer = fused_alloc(ns_entry->ctrlr, 0x1000);
	if (write_ctx->buffer == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		free(write_ctx);
		exit(-1);
	}
	snprintf(write_ctx->buffer, 0x1000, "%s", "Starting buffer\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, write_ctx->buffer,
				    first_lba, /* LBA start */
				    1, /* number of LBAs */
				    test_case_1_cpl_cb, write_ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Start case 2 */
	write_ctx = malloc(sizeof(struct write_ctx));
	write_ctx->worker = worker;
	write_ctx->buffer = fused_alloc(ns_entry->ctrlr, 0x1000);
	if (write_ctx->buffer == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		free(write_ctx);
		exit(-1);
	}
	snprintf(write_ctx->buffer, 0x1000, "%s", "Starting buffer\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, write_ctx->buffer,
				    first_lba + 1, /* LBA start */
				    1, /* number of LBAs */
				    test_case_2_cpl_cb, write_ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Start case 3 */
	write_ctx = malloc(sizeof(struct write_ctx));
	write_ctx->worker = worker;
	write_ctx->buffer = fused_alloc(ns_entry->ctrlr, 0x1000);
	if (write_ctx->buffer == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		free(write_ctx);
		exit(-1);
	}
	snprintf(write_ctx->buffer, 0x1000, "%s", "Starting buffer\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, write_ctx->buffer,
				    first_lba + 2, /* LBA start */
				    1, /* number of LBAs */
				    test_case_3_cpl_cb, write_ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Start case 4 */
	write_ctx = malloc(sizeof(struct write_ctx));
	write_ctx->worker = worker;
	write_ctx->buffer = fused_alloc(ns_entry->ctrlr, 0x1000);
	if (write_ctx->buffer == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		free(write_ctx);
		exit(-1);
	}
	snprintf(write_ctx->buffer, 0x1000, "%s", "Starting buffer\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, write_ctx->buffer,
				    first_lba + 3, /* LBA start */
				    1, /* number of LBAs */
				    test_case_4_cpl_cb, write_ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/* Start case 5 */
	write_ctx = malloc(sizeof(struct write_ctx));
	write_ctx->worker = worker;
	write_ctx->buffer = fused_alloc(ns_entry->ctrlr, 0x1000);
	if (write_ctx->buffer == NULL) {
		SPDK_ERRLOG("Cannot allocate memory");
		free(write_ctx);
		exit(-1);
	}
	snprintf(write_ctx->buffer, 0x1000, "%s", "Starting buffer\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, write_ctx->buffer,
				    first_lba + 4, /* LBA start */
				    1, /* number of LBAs */
				    test_case_5_cpl_cb, write_ctx, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/*
	 * Case 6: Fused commands not in sequence
	 */
	ctx = fused_ctx_get(worker);
	ctx->status_first.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	/* Fused compare and write operation */
	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, worker->qpair1, ctx->cmp_buf,
				      first_lba + 5, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(worker);
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = cmd_done;

	snprintf(ctx2->write_buf, 0x1000, "%s", "Not fused\n");

	/* Not fused write op */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, ctx2->write_buf,
				    first_lba + 5, /* LBA start */
				    1, /* number of LBAs */
				    nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	snprintf(ctx->write_buf, 0x1000, "%s", "Fused\n");

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, ctx->write_buf,
				    first_lba + 5, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	ctx2 = fused_ctx_get(worker);
	ctx2->status_first.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->status_second.sc = SPDK_NVME_SC_SUCCESS;
	ctx2->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx2->done = cmd_done;

	snprintf(ctx2->cmp_buf, 0x1000, "%s", "Not fused\n");

	/* Not fused compare op */
	ctx2->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, worker->qpair1, ctx2->cmp_buf,
				      first_lba + 5, /* LBA start */
				      1, /* number of LBAs */
				      nvme_cpl_cb, ctx2, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	/*
	 * Case 7: Regions do not match
	 */
	ctx = fused_ctx_get(worker);
	ctx->status_first.sc = SPDK_NVME_SC_INVALID_FIELD;
	ctx->status_first.sct = SPDK_NVME_SCT_GENERIC;
	ctx->status_second.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
	ctx->status_second.sct = SPDK_NVME_SCT_GENERIC;
	ctx->done = compare_and_write_done;

	ctx->timeout_tsc = spdk_get_ticks() + IO_TIMEOUT_S * spdk_get_ticks_hz();
	rc = spdk_nvme_ns_cmd_compare(ns_entry->ns, worker->qpair1, ctx->cmp_buf,
				      first_lba + 6, /* LBA start */
				      1, /* number of LBAs */
				      nvme_fused_first_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_FIRST);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair1, ctx->write_buf,
				    first_lba + 7, /* LBA start */
				    1, /* number of LBAs */
				    nvme_fused_second_cpl_cb, ctx, SPDK_NVME_CMD_FUSE_SECOND);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}

	return 0;
}

static void
cleanup_worker(struct nvme_fused_worker *worker)
{
	int i;
	for (i = 0; i < worker->req_num; i++) {
		spdk_free(worker->ctx[i].cmp_buf);
		spdk_free(worker->ctx[i].write_buf);
	}
	worker->req_num = 0;
}

static int
poll_for_completions(void *arg)
{
	struct nvme_fused_worker *worker = arg;
	int32_t rv;
	uint64_t current_ticks;
	int i;
	struct nvme_fused_ns *ns_entry = worker->ns;

	if (worker->req_pending == 0) {
		if (worker->counter < 1) {
			goto exit_handler;
		}
		worker->counter--;

		cleanup_worker(worker);
		compare_and_write(arg);
	}

	rv = spdk_nvme_qpair_process_completions(worker->qpair1, 0);
	if (rv < 0) {
		goto exit_handler;
	}

	rv = spdk_nvme_qpair_process_completions(worker->qpair2, 0);
	if (rv < 0) {
		goto exit_handler;
	}

	rv = spdk_nvme_ctrlr_process_admin_completions(ns_entry->ctrlr);
	if (rv < 0) {
		goto exit_handler;
	}

	current_ticks = spdk_get_ticks();

	for (i = 0; i < worker->req_num; i++) {
		if (worker->ctx[i].is_done) {
			continue;
		}
		if (worker->ctx[i].rv < 0) {
			SPDK_ERRLOG("Request #%d finished with rv=%d\n", worker->ctx[i].index, worker->ctx[i].rv);
			goto exit_handler;
		}
		if (worker->ctx[i].timeout_tsc < current_ticks) {
			SPDK_ERRLOG("Request #%d IO Timeout\n", worker->ctx[i].index);
			goto exit_handler;
		}
	}

	return 0;

exit_handler:
	SPDK_NOTICELOG("Finishing worker poller\n");
	spdk_poller_unregister(&worker->req_poller);
	__sync_sub_and_fetch(&g_num_active_threads, 1);
	spdk_thread_exit(worker->thread);
	return 0;
}

static void
free_namespaces(void)
{
	struct nvme_fused_ns *ns, *tmp;
	int i;

	TAILQ_FOREACH_SAFE(ns, &g_ns_list, tailq, tmp) {
		for (i = 0; i < g_workers_per_ns; i++) {
			cleanup_worker(&ns->worker[i]);

			if (ns->worker[i].qpair1) {
				spdk_nvme_ctrlr_free_io_qpair(ns->worker[i].qpair1);
			}
			if (ns->worker[i].qpair2) {
				spdk_nvme_ctrlr_free_io_qpair(ns->worker[i].qpair2);
			}
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
	int i;

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &opts, sizeof(opts));
		for (i = 0; i < g_workers_per_ns; i++) {
			ns_entry->worker[i].index = i;
			ns_entry->worker[i].ns = ns_entry;
			ns_entry->worker[i].qpair1 = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
			ns_entry->worker[i].qpair2 = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
			if (ns_entry->worker[i].qpair1 == NULL || ns_entry->worker[i].qpair2 == NULL) {
				fprintf(stderr, "Unable to create a qpair for a namespace\n");
				return -1;
			}
		}
	}
	return 0;
}

static void
start_worker_poller(void *ctx)
{
	struct nvme_fused_worker *worker = ctx;

	worker->counter = g_counter;
	worker->req_poller = spdk_poller_register(poll_for_completions, worker, 0);
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
	int i;

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
		for (i = 0; i < g_workers_per_ns; i++) {
			ns_entry->worker[i].thread = spdk_thread_create(NULL, NULL);
			if (ns_entry->worker[i].thread == NULL) {
				fprintf(stderr, "Failed to allocate thread for namespace.\n");
				goto out;
			}
		}
	}

	TAILQ_FOREACH(ns_entry, &g_ns_list, tailq) {
		for (i = 0; i < g_workers_per_ns; i++) {
			spdk_thread_send_msg(ns_entry->worker[i].thread, start_worker_poller, &ns_entry->worker[i]);
			__sync_add_and_fetch(&g_num_active_threads, 1);
		}
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
	fprintf(stderr, " -V                        Enable logging of each submitted command.\n");
	fprintf(stderr, " -t <num>                  Number of repetitions per worker.\n");
	fprintf(stderr, " -w <num>                  Number of workers per namespace.\n");
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
	case 't':
		g_counter = spdk_strtol(optarg, 10);
		break;
	case 'w':
		g_workers_per_ns = spdk_strtol(optarg, 10);
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

	spdk_app_opts_init(&opts);
	opts.name = "nvme_fused";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:Nt:Vw:", NULL, nvme_fused_parse,
				      nvme_fused_usage) != SPDK_APP_PARSE_ARGS_SUCCESS)) {
		return rc;
	}

	if (g_conf_file) {
		parse_trids();
	}

	rc = spdk_app_start(&opts, begin_fused, NULL);

	return rc;
}
