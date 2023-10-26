/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/event.h"
#include "spdk/accel.h"

#define TEST_XFER_SIZE 4096
#define INITIAL_PATTERN_0 0
#define INITIAL_PATTERN_1 1
#define INITIAL_PATTERN_2 2
#define FILL_PATTERN_1 255
#define FILL_PATTERN_2 111
#define IOVCNT 1

struct test_ctx {
	char *driver_name;
	struct spdk_io_channel *ch;
	char buf1[TEST_XFER_SIZE];
	char buf1_bck[TEST_XFER_SIZE];
	char buf2[TEST_XFER_SIZE];
	char buf2_bck[TEST_XFER_SIZE];
	struct iovec iov1, iov2, iov1_bck, iov2_bck;
};

static void
test_seq_complete_cb(void *_ctx, int status)
{
	struct test_ctx *ctx = _ctx;
	char expected_buf1[TEST_XFER_SIZE], expected_buf2[TEST_XFER_SIZE];
	char expected_buf1_bck[TEST_XFER_SIZE], expected_buf2_bck[TEST_XFER_SIZE];

	printf("Running sequence callback\n");

	if (status != 0) {
		SPDK_ERRLOG("Unexpected status code: %d", status);
		goto out;
	}

	memset(expected_buf1, FILL_PATTERN_1, sizeof(expected_buf1));
	memset(expected_buf2, FILL_PATTERN_2, sizeof(expected_buf2));
	memset(expected_buf1_bck, 1, sizeof(expected_buf2));
	memset(expected_buf2_bck, 2, sizeof(expected_buf2));

	if (memcmp(ctx->buf1, expected_buf1, TEST_XFER_SIZE) != 0 ||
	    memcmp(ctx->buf2, expected_buf2, TEST_XFER_SIZE) != 0 ||
	    memcmp(ctx->buf1_bck, expected_buf1_bck, TEST_XFER_SIZE != 0) ||
	    memcmp(ctx->buf2_bck, expected_buf2_bck, TEST_XFER_SIZE != 0)) {
		SPDK_ERRLOG("Sequence failed: buffers mismatch\n");
		status = 1;
	}
out:
	spdk_put_io_channel(ctx->ch);
	spdk_app_stop(status);
}

static void
start_driver(void *_ctx)
{
	int rc = 0, completed = 0;
	struct test_ctx *ctx = _ctx;
	struct spdk_accel_sequence *seq = NULL;

	ctx->ch = spdk_accel_get_io_channel();
	if (ctx->ch == NULL) {
		SPDK_ERRLOG("Failed to get IO channel\n");
		spdk_app_stop(1);
		return;
	}

	/* Prepare buffers */
	memset(ctx->buf1, INITIAL_PATTERN_1, TEST_XFER_SIZE);
	memset(ctx->buf2, INITIAL_PATTERN_2, TEST_XFER_SIZE);
	memset(ctx->buf1_bck, INITIAL_PATTERN_0, TEST_XFER_SIZE);
	memset(ctx->buf2_bck, INITIAL_PATTERN_0, TEST_XFER_SIZE);

	ctx->iov1.iov_base = ctx->buf1;
	ctx->iov1.iov_len = TEST_XFER_SIZE;

	ctx->iov2.iov_base = ctx->buf2;
	ctx->iov2.iov_len = TEST_XFER_SIZE;

	ctx->iov1_bck.iov_base = ctx->buf1_bck;
	ctx->iov1_bck.iov_len = TEST_XFER_SIZE;

	ctx->iov2_bck.iov_base = ctx->buf2_bck;
	ctx->iov2_bck.iov_len = TEST_XFER_SIZE;

	/* Test driver implementation. Test scenario is:
	 *	copy buf1 -> buf1_bck
	 *	fill buf1 <- FILL_PATTERN_1
	 *	copy buf2 -> buf2_bck
	 *	fill buf2 <- FILL_PATTERN_2
	 */
	rc = spdk_accel_append_copy(&seq, ctx->ch, &ctx->iov1_bck, IOVCNT, NULL, NULL,
				    &ctx->iov1, IOVCNT, NULL, NULL, 0,
				    NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("ERROR running append copy 1! exiting.\n");
		goto error;
	}
	rc = spdk_accel_append_fill(&seq, ctx->ch, &ctx->buf1, TEST_XFER_SIZE,
				    NULL, NULL, FILL_PATTERN_1, 0,
				    NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("ERROR running append fill 1! exiting.\n");
		goto error;
	}
	rc = spdk_accel_append_copy(&seq, ctx->ch, &ctx->iov2_bck, IOVCNT, NULL, NULL,
				    &ctx->iov2, IOVCNT, NULL, NULL, 0,
				    NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("ERROR running append copy 2! exiting.\n");
		goto error;
	}
	rc = spdk_accel_append_fill(&seq, ctx->ch, &ctx->buf2, TEST_XFER_SIZE,
				    NULL, NULL, FILL_PATTERN_2, 0,
				    NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("ERROR running append fill 2! exiting.\n");
		goto error;
	}
	spdk_accel_sequence_finish(seq, test_seq_complete_cb, ctx);
	return;
error:
	spdk_accel_sequence_abort(seq);
	spdk_put_io_channel(ctx->ch);
	spdk_app_stop(rc);
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};
	struct test_ctx ctx = {};

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "accel_external_driver";

	/*
	 * Parse built-in SPDK command line parameters as well
	 * as our custom one(s).
	 */
	if ((rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL,
				      NULL)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	rc = spdk_app_start(&opts, start_driver, &ctx);

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
