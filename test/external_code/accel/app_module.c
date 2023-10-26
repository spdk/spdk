/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/event.h"
#include "spdk/accel.h"

#define TEST_XFER_SIZE 4096
#define FILL_PATTERN 255

enum test_state {
	TEST_STATE_FILL,
	TEST_STATE_COPY,
	TEST_STATE_COMPARE,
	TEST_STATE_WAIT_COMPLETION,
	TEST_STATE_DONE
};

struct test_ctx {
	enum test_state state;
	int status;
	char buf1[TEST_XFER_SIZE];
	char buf2[TEST_XFER_SIZE];
	struct spdk_io_channel *ch;
};

static void
test_ctx_fail(struct test_ctx *ctx)
{
	ctx->status = 1;
	ctx->state = TEST_STATE_DONE;
}

static void process_accel(void *);

static void
fill_cb(void *arg, int status)
{
	struct test_ctx *ctx = arg;
	char expected[TEST_XFER_SIZE];

	printf("Running fill callback\n");
	if (status != 0) {
		test_ctx_fail(ctx);
		goto out;
	}

	memset(expected, FILL_PATTERN, sizeof(expected));
	if (memcmp(ctx->buf1, expected,  TEST_XFER_SIZE) != 0) {
		SPDK_ERRLOG("Fill failed: buffer mismatch\n");
		test_ctx_fail(ctx);
		goto out;
	}

	ctx->state = TEST_STATE_COPY;
out:
	process_accel(ctx);
}

static void
copy_cb(void *arg, int status)
{
	struct test_ctx *ctx = arg;

	printf("Running copy callback\n");
	if (status != 0) {
		test_ctx_fail(ctx);
		goto out;
	}
	if (memcmp(ctx->buf1, ctx->buf2, TEST_XFER_SIZE) != 0) {
		SPDK_ERRLOG("Copy failed: buffer mismatch\n");
		test_ctx_fail(ctx);
		goto out;
	}

	ctx->state = TEST_STATE_COMPARE;
out:
	process_accel(ctx);
}

static void
compare_cb(void *arg, int status)
{
	struct test_ctx *ctx = arg;

	printf("Running compare callback\n");
	if (status != 0) {
		test_ctx_fail(ctx);
		goto out;
	}
	if (memcmp(ctx->buf1, ctx->buf2, TEST_XFER_SIZE) != 0) {
		SPDK_ERRLOG("Compare failed: buffer mismatch\n");
		test_ctx_fail(ctx);
		goto out;
	}

	ctx->state = TEST_STATE_DONE;
out:
	process_accel(ctx);
}

static void
process_accel(void *_ctx)
{
	int rc;
	struct test_ctx *ctx = _ctx;
	enum test_state prev_state;

	do {
		prev_state = ctx->state;

		switch (ctx->state) {
		case TEST_STATE_FILL:
			memset(ctx->buf1, 0, sizeof(ctx->buf1));
			memset(ctx->buf2, 0, sizeof(ctx->buf2));
			ctx->state = TEST_STATE_WAIT_COMPLETION;
			/* Submit fill command */
			rc = spdk_accel_submit_fill(ctx->ch, ctx->buf1, FILL_PATTERN,
						    TEST_XFER_SIZE, 0, fill_cb, ctx);
			if (rc) {
				SPDK_ERRLOG("ERROR running submit fill! exiting.\n");
				test_ctx_fail(ctx);
			}
			break;
		case TEST_STATE_COPY:
			ctx->state = TEST_STATE_WAIT_COMPLETION;
			/* Submit copy command */
			rc = spdk_accel_submit_copy(ctx->ch, ctx->buf1, ctx->buf2,
						    TEST_XFER_SIZE, 0, copy_cb, ctx);
			if (rc) {
				SPDK_ERRLOG("ERROR running submit copy! exiting.\n");
				test_ctx_fail(ctx);
			}
			break;
		case TEST_STATE_COMPARE:
			ctx->state = TEST_STATE_WAIT_COMPLETION;
			/* Submit compare command */
			rc = spdk_accel_submit_compare(ctx->ch, ctx->buf1, ctx->buf2,
						       TEST_XFER_SIZE, compare_cb, ctx);
			if (rc) {
				SPDK_ERRLOG("ERROR running submit compare! exiting.\n");
				test_ctx_fail(ctx);
			}
			break;
		case TEST_STATE_WAIT_COMPLETION:
			break;
		case TEST_STATE_DONE:
			spdk_put_io_channel(ctx->ch);
			spdk_app_stop(ctx->status);
			break;
		}
	} while (ctx->state != prev_state);
}

static void
start_accel(void *_ctx)
{
	struct test_ctx *ctx = _ctx;

	ctx->ch = spdk_accel_get_io_channel();
	if (ctx->ch == NULL) {
		SPDK_ERRLOG("Failed to get IO channel\n");
		spdk_app_stop(1);
		return;
	}

	process_accel(ctx);
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};
	struct test_ctx ctx = {.state = TEST_STATE_FILL, .status = 0};

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "accel_external_module";

	/*
	 * Parse built-in SPDK command line parameters as well
	 * as our custom one(s).
	 */
	if ((rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL,
				      NULL)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	rc = spdk_app_start(&opts, start_accel, &ctx);

	spdk_app_fini();
	return rc;
}
