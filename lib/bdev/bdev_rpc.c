/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/bdev.h"

#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/base64.h"
#include "spdk/bdev_module.h"
#include "spdk/dma.h"

#include "spdk/log.h"

#include "bdev_internal.h"

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

struct spdk_rpc_set_bdev_opts {
	uint32_t bdev_io_pool_size;
	uint32_t bdev_io_cache_size;
	bool bdev_auto_examine;
	uint32_t small_buf_pool_size;
	uint32_t large_buf_pool_size;
};

static const struct spdk_json_object_decoder rpc_set_bdev_opts_decoders[] = {
	{"bdev_io_pool_size", offsetof(struct spdk_rpc_set_bdev_opts, bdev_io_pool_size), spdk_json_decode_uint32, true},
	{"bdev_io_cache_size", offsetof(struct spdk_rpc_set_bdev_opts, bdev_io_cache_size), spdk_json_decode_uint32, true},
	{"bdev_auto_examine", offsetof(struct spdk_rpc_set_bdev_opts, bdev_auto_examine), spdk_json_decode_bool, true},
	{"small_buf_pool_size", offsetof(struct spdk_rpc_set_bdev_opts, small_buf_pool_size), spdk_json_decode_uint32, true},
	{"large_buf_pool_size", offsetof(struct spdk_rpc_set_bdev_opts, large_buf_pool_size), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_rpc_set_bdev_opts rpc_opts;
	struct spdk_bdev_opts bdev_opts;
	int rc;

	rpc_opts.bdev_io_pool_size = UINT32_MAX;
	rpc_opts.bdev_io_cache_size = UINT32_MAX;
	rpc_opts.small_buf_pool_size = UINT32_MAX;
	rpc_opts.large_buf_pool_size = UINT32_MAX;
	rpc_opts.bdev_auto_examine = true;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_set_bdev_opts_decoders,
					    SPDK_COUNTOF(rpc_set_bdev_opts_decoders), &rpc_opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
	if (rpc_opts.bdev_io_pool_size != UINT32_MAX) {
		bdev_opts.bdev_io_pool_size = rpc_opts.bdev_io_pool_size;
	}
	if (rpc_opts.bdev_io_cache_size != UINT32_MAX) {
		bdev_opts.bdev_io_cache_size = rpc_opts.bdev_io_cache_size;
	}
	bdev_opts.bdev_auto_examine = rpc_opts.bdev_auto_examine;
	if (rpc_opts.small_buf_pool_size != UINT32_MAX) {
		bdev_opts.small_buf_pool_size = rpc_opts.small_buf_pool_size;
	}
	if (rpc_opts.large_buf_pool_size != UINT32_MAX) {
		bdev_opts.large_buf_pool_size = rpc_opts.large_buf_pool_size;
	}

	rc = spdk_bdev_set_opts(&bdev_opts);

	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Pool size %" PRIu32 " too small for cache size %" PRIu32,
						     bdev_opts.bdev_io_pool_size, bdev_opts.bdev_io_cache_size);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_set_options", rpc_bdev_set_options, SPDK_RPC_STARTUP)

static void
rpc_bdev_wait_for_examine_cpl(void *arg)
{
	struct spdk_jsonrpc_request *request = arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_wait_for_examine(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev_wait_for_examine requires no parameters");
		return;
	}

	rc = spdk_bdev_wait_for_examine(rpc_bdev_wait_for_examine_cpl, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("bdev_wait_for_examine", rpc_bdev_wait_for_examine, SPDK_RPC_RUNTIME)

struct rpc_bdev_examine {
	char *name;
};

static void
free_rpc_bdev_examine(struct rpc_bdev_examine *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_examine_bdev_decoders[] = {
	{"name", offsetof(struct rpc_bdev_examine, name), spdk_json_decode_string},
};

static void
rpc_bdev_examine_bdev(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_examine req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_examine_bdev_decoders,
				    SPDK_COUNTOF(rpc_examine_bdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_examine(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_examine(&req);
}
SPDK_RPC_REGISTER("bdev_examine", rpc_bdev_examine_bdev, SPDK_RPC_RUNTIME)

struct rpc_get_iostat_ctx {
	int bdev_count;
	int rc;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
	bool per_channel;
};

struct bdev_get_iostat_ctx {
	struct spdk_bdev_io_stat *stat;
	struct rpc_get_iostat_ctx *rpc_ctx;
	struct spdk_bdev_desc *desc;
};

static void
rpc_get_iostat_started(struct rpc_get_iostat_ctx *rpc_ctx)
{
	rpc_ctx->w = spdk_jsonrpc_begin_result(rpc_ctx->request);

	spdk_json_write_object_begin(rpc_ctx->w);
	spdk_json_write_named_uint64(rpc_ctx->w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_uint64(rpc_ctx->w, "ticks", spdk_get_ticks());
}

static void
rpc_get_iostat_done(struct rpc_get_iostat_ctx *rpc_ctx)
{
	if (--rpc_ctx->bdev_count != 0) {
		return;
	}

	if (rpc_ctx->rc == 0) {
		spdk_json_write_array_end(rpc_ctx->w);
		spdk_json_write_object_end(rpc_ctx->w);
		spdk_jsonrpc_end_result(rpc_ctx->request, rpc_ctx->w);
	} else {
		/* Return error response after processing all specified bdevs
		 * completed or failed.
		 */
		spdk_jsonrpc_send_error_response(rpc_ctx->request, rpc_ctx->rc,
						 spdk_strerror(-rpc_ctx->rc));
	}

	free(rpc_ctx);
}

static struct bdev_get_iostat_ctx *
bdev_iostat_ctx_alloc(bool iostat_ext)
{
	struct bdev_get_iostat_ctx *ctx;

	ctx = calloc(1, sizeof(struct bdev_get_iostat_ctx));
	if (ctx == NULL) {
		return NULL;
	}

	ctx->stat = bdev_alloc_io_stat(iostat_ext);
	if (ctx->stat == NULL) {
		free(ctx);
		return NULL;
	}

	return ctx;
}

static void
bdev_iostat_ctx_free(struct bdev_get_iostat_ctx *ctx)
{
	bdev_free_io_stat(ctx->stat);
	free(ctx);
}

static void
bdev_get_iostat_done(struct spdk_bdev *bdev, struct spdk_bdev_io_stat *stat,
		     void *cb_arg, int rc)
{
	struct bdev_get_iostat_ctx *bdev_ctx = cb_arg;
	struct rpc_get_iostat_ctx *rpc_ctx = bdev_ctx->rpc_ctx;
	struct spdk_json_write_ctx *w = rpc_ctx->w;

	if (rc != 0 || rpc_ctx->rc != 0) {
		if (rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
		goto done;
	}

	assert(stat == bdev_ctx->stat);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(bdev));

	spdk_bdev_dump_io_stat_json(stat, w);

	if (spdk_bdev_get_qd_sampling_period(bdev)) {
		spdk_json_write_named_uint64(w, "queue_depth_polling_period",
					     spdk_bdev_get_qd_sampling_period(bdev));

		spdk_json_write_named_uint64(w, "queue_depth", spdk_bdev_get_qd(bdev));

		spdk_json_write_named_uint64(w, "io_time", spdk_bdev_get_io_time(bdev));

		spdk_json_write_named_uint64(w, "weighted_io_time",
					     spdk_bdev_get_weighted_io_time(bdev));
	}

	if (bdev->fn_table->dump_device_stat_json) {
		spdk_json_write_named_object_begin(w, "driver_specific");
		bdev->fn_table->dump_device_stat_json(bdev->ctxt, w);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);

done:
	rpc_get_iostat_done(rpc_ctx);

	spdk_bdev_close(bdev_ctx->desc);
	bdev_iostat_ctx_free(bdev_ctx);
}

static int
bdev_get_iostat(void *ctx, struct spdk_bdev *bdev)
{
	struct rpc_get_iostat_ctx *rpc_ctx = ctx;
	struct bdev_get_iostat_ctx *bdev_ctx;
	int rc;

	bdev_ctx = bdev_iostat_ctx_alloc(true);
	if (bdev_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, dummy_bdev_event_cb, NULL,
				&bdev_ctx->desc);
	if (rc != 0) {
		bdev_iostat_ctx_free(bdev_ctx);
		SPDK_ERRLOG("Failed to open bdev\n");
		return rc;
	}

	rpc_ctx->bdev_count++;
	bdev_ctx->rpc_ctx = rpc_ctx;
	spdk_bdev_get_device_stat(bdev, bdev_ctx->stat, bdev_get_iostat_done, bdev_ctx);

	return 0;
}

static void
bdev_get_per_channel_stat_done(struct spdk_bdev *bdev, void *ctx, int status)
{
	struct bdev_get_iostat_ctx *bdev_ctx = ctx;

	rpc_get_iostat_done(bdev_ctx->rpc_ctx);

	spdk_bdev_close(bdev_ctx->desc);

	bdev_iostat_ctx_free(bdev_ctx);
}

static void
bdev_get_per_channel_stat(struct spdk_bdev_channel_iter *i, struct spdk_bdev *bdev,
			  struct spdk_io_channel *ch, void *ctx)
{
	struct bdev_get_iostat_ctx *bdev_ctx = ctx;
	struct spdk_json_write_ctx *w = bdev_ctx->rpc_ctx->w;

	spdk_bdev_get_io_stat(bdev, ch, bdev_ctx->stat);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint64(w, "thread_id", spdk_thread_get_id(spdk_get_thread()));
	spdk_bdev_dump_io_stat_json(bdev_ctx->stat, w);
	spdk_json_write_object_end(w);

	spdk_bdev_for_each_channel_continue(i, 0);
}

struct rpc_bdev_get_iostat {
	char *name;
	bool per_channel;
};

static void
free_rpc_bdev_get_iostat(struct rpc_bdev_get_iostat *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_get_iostat_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_iostat, name), spdk_json_decode_string, true},
	{"per_channel", offsetof(struct rpc_bdev_get_iostat, per_channel), spdk_json_decode_bool, true},
};

static void
rpc_bdev_get_iostat(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_get_iostat req = {};
	struct spdk_bdev_desc *desc = NULL;
	struct rpc_get_iostat_ctx *rpc_ctx;
	struct bdev_get_iostat_ctx *bdev_ctx;
	struct spdk_bdev *bdev;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_get_iostat_decoders,
					    SPDK_COUNTOF(rpc_bdev_get_iostat_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			free_rpc_bdev_get_iostat(&req);
			return;
		}

		if (req.per_channel == true && !req.name) {
			SPDK_ERRLOG("Bdev name is required for per channel IO statistics\n");
			spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
			free_rpc_bdev_get_iostat(&req);
			return;
		}

		if (req.name) {
			rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
				spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
				free_rpc_bdev_get_iostat(&req);
				return;
			}
		}
	}

	free_rpc_bdev_get_iostat(&req);

	rpc_ctx = calloc(1, sizeof(struct rpc_get_iostat_ctx));
	if (rpc_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_iostat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	rpc_ctx->bdev_count++;
	rpc_ctx->request = request;
	rpc_ctx->per_channel = req.per_channel;

	if (desc != NULL) {
		bdev = spdk_bdev_desc_get_bdev(desc);

		bdev_ctx = bdev_iostat_ctx_alloc(req.per_channel == false);
		if (bdev_ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
			rpc_ctx->rc = -ENOMEM;

			spdk_bdev_close(desc);
		} else {
			bdev_ctx->desc = desc;

			rpc_ctx->bdev_count++;
			bdev_ctx->rpc_ctx = rpc_ctx;
			if (req.per_channel == false) {
				spdk_bdev_get_device_stat(bdev, bdev_ctx->stat, bdev_get_iostat_done,
							  bdev_ctx);
			} else {
				/* If per_channel is true, there is no failure after here and
				 * we have to start RPC response before executing
				 * spdk_bdev_for_each_channel().
				 */
				rpc_get_iostat_started(rpc_ctx);
				spdk_json_write_named_string(rpc_ctx->w, "name", spdk_bdev_get_name(bdev));
				spdk_json_write_named_array_begin(rpc_ctx->w, "channels");

				spdk_bdev_for_each_channel(bdev,
							   bdev_get_per_channel_stat,
							   bdev_ctx,
							   bdev_get_per_channel_stat_done);

				rpc_get_iostat_done(rpc_ctx);
				return;
			}
		}
	} else {
		rc = spdk_for_each_bdev(rpc_ctx, bdev_get_iostat);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	if (rpc_ctx->rc == 0) {
		/* We want to fail the RPC for all failures. If per_channel is false,
		 * it is enough to defer starting RPC response until it is ensured that
		 * all spdk_bdev_for_each_channel() calls will succeed or there is no bdev.
		 */
		rpc_get_iostat_started(rpc_ctx);
		spdk_json_write_named_array_begin(rpc_ctx->w, "bdevs");
	}

	rpc_get_iostat_done(rpc_ctx);
}
SPDK_RPC_REGISTER("bdev_get_iostat", rpc_bdev_get_iostat, SPDK_RPC_RUNTIME)

struct rpc_reset_iostat_ctx {
	int bdev_count;
	int rc;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
	enum spdk_bdev_reset_stat_mode mode;
};

struct bdev_reset_iostat_ctx {
	struct rpc_reset_iostat_ctx *rpc_ctx;
	struct spdk_bdev_desc *desc;
};

static void
rpc_reset_iostat_done(struct rpc_reset_iostat_ctx *rpc_ctx)
{
	if (--rpc_ctx->bdev_count != 0) {
		return;
	}

	if (rpc_ctx->rc == 0) {
		spdk_jsonrpc_send_bool_response(rpc_ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(rpc_ctx->request, rpc_ctx->rc,
						 spdk_strerror(-rpc_ctx->rc));
	}

	free(rpc_ctx);
}

static void
bdev_reset_iostat_done(struct spdk_bdev *bdev, void *cb_arg, int rc)
{
	struct bdev_reset_iostat_ctx *bdev_ctx = cb_arg;
	struct rpc_reset_iostat_ctx *rpc_ctx = bdev_ctx->rpc_ctx;

	if (rc != 0 || rpc_ctx->rc != 0) {
		if (rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	rpc_reset_iostat_done(rpc_ctx);

	spdk_bdev_close(bdev_ctx->desc);
	free(bdev_ctx);
}

static int
bdev_reset_iostat(void *ctx, struct spdk_bdev *bdev)
{
	struct rpc_reset_iostat_ctx *rpc_ctx = ctx;
	struct bdev_reset_iostat_ctx *bdev_ctx;
	int rc;

	bdev_ctx = calloc(1, sizeof(struct bdev_reset_iostat_ctx));
	if (bdev_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, dummy_bdev_event_cb, NULL,
				&bdev_ctx->desc);
	if (rc != 0) {
		free(bdev_ctx);
		SPDK_ERRLOG("Failed to open bdev\n");
		return rc;
	}

	if (bdev->fn_table->reset_device_stat) {
		bdev->fn_table->reset_device_stat(bdev->ctxt);
	}

	rpc_ctx->bdev_count++;
	bdev_ctx->rpc_ctx = rpc_ctx;
	bdev_reset_device_stat(bdev, rpc_ctx->mode, bdev_reset_iostat_done, bdev_ctx);

	return 0;
}

struct rpc_bdev_reset_iostat {
	char *name;
	enum spdk_bdev_reset_stat_mode mode;
};

static void
free_rpc_bdev_reset_iostat(struct rpc_bdev_reset_iostat *r)
{
	free(r->name);
}

static int
rpc_decode_reset_iostat_mode(const struct spdk_json_val *val, void *out)
{
	enum spdk_bdev_reset_stat_mode *mode = out;

	if (spdk_json_strequal(val, "all") == true) {
		*mode = SPDK_BDEV_RESET_STAT_ALL;
	} else if (spdk_json_strequal(val, "maxmin") == true) {
		*mode = SPDK_BDEV_RESET_STAT_MAXMIN;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: mode\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_reset_iostat_decoders[] = {
	{"name", offsetof(struct rpc_bdev_reset_iostat, name), spdk_json_decode_string, true},
	{"mode", offsetof(struct rpc_bdev_reset_iostat, mode), rpc_decode_reset_iostat_mode, true},
};

static void
rpc_bdev_reset_iostat(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_reset_iostat req = { .mode = SPDK_BDEV_RESET_STAT_ALL, };
	struct spdk_bdev_desc *desc = NULL;
	struct rpc_reset_iostat_ctx *rpc_ctx;
	struct bdev_reset_iostat_ctx *bdev_ctx;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_reset_iostat_decoders,
					    SPDK_COUNTOF(rpc_bdev_reset_iostat_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			free_rpc_bdev_reset_iostat(&req);
			return;
		}

		if (req.name) {
			rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
				spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
				free_rpc_bdev_reset_iostat(&req);
				return;
			}
		}
	}


	rpc_ctx = calloc(1, sizeof(struct rpc_reset_iostat_ctx));
	if (rpc_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_iostat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		free_rpc_bdev_reset_iostat(&req);
		return;
	}

	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	rpc_ctx->bdev_count++;
	rpc_ctx->request = request;
	rpc_ctx->mode = req.mode;

	free_rpc_bdev_reset_iostat(&req);

	if (desc != NULL) {
		bdev_ctx = calloc(1, sizeof(struct bdev_reset_iostat_ctx));
		if (bdev_ctx == NULL) {
			SPDK_ERRLOG("Failed to allocate bdev_iostat_ctx struct\n");
			rpc_ctx->rc = -ENOMEM;

			spdk_bdev_close(desc);
		} else {
			bdev_ctx->desc = desc;

			rpc_ctx->bdev_count++;
			bdev_ctx->rpc_ctx = rpc_ctx;
			bdev_reset_device_stat(spdk_bdev_desc_get_bdev(desc), rpc_ctx->mode,
					       bdev_reset_iostat_done, bdev_ctx);
		}
	} else {
		rc = spdk_for_each_bdev(rpc_ctx, bdev_reset_iostat);
		if (rc != 0 && rpc_ctx->rc == 0) {
			rpc_ctx->rc = rc;
		}
	}

	rpc_reset_iostat_done(rpc_ctx);
}
SPDK_RPC_REGISTER("bdev_reset_iostat", rpc_bdev_reset_iostat, SPDK_RPC_RUNTIME)

static int
rpc_dump_bdev_info(void *ctx, struct spdk_bdev *bdev)
{
	struct spdk_json_write_ctx *w = ctx;
	struct spdk_bdev_alias *tmp;
	uint64_t qos_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
	struct spdk_memory_domain **domains;
	int i, rc;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(bdev));

	spdk_json_write_named_array_begin(w, "aliases");

	TAILQ_FOREACH(tmp, spdk_bdev_get_aliases(bdev), tailq) {
		spdk_json_write_string(w, tmp->alias.name);
	}

	spdk_json_write_array_end(w);

	spdk_json_write_named_string(w, "product_name", spdk_bdev_get_product_name(bdev));

	spdk_json_write_named_uint32(w, "block_size", spdk_bdev_get_block_size(bdev));

	spdk_json_write_named_uint64(w, "num_blocks", spdk_bdev_get_num_blocks(bdev));

	if (!spdk_mem_all_zero(&bdev->uuid, sizeof(bdev->uuid))) {
		char uuid_str[SPDK_UUID_STRING_LEN];

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
		spdk_json_write_named_string(w, "uuid", uuid_str);
	}

	if (spdk_bdev_get_md_size(bdev) != 0) {
		spdk_json_write_named_uint32(w, "md_size", spdk_bdev_get_md_size(bdev));
		spdk_json_write_named_bool(w, "md_interleave", spdk_bdev_is_md_interleaved(bdev));
		spdk_json_write_named_uint32(w, "dif_type", spdk_bdev_get_dif_type(bdev));
		if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
			spdk_json_write_named_bool(w, "dif_is_head_of_md", spdk_bdev_is_dif_head_of_md(bdev));
			spdk_json_write_named_object_begin(w, "enabled_dif_check_types");
			spdk_json_write_named_bool(w, "reftag",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG));
			spdk_json_write_named_bool(w, "apptag",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_APPTAG));
			spdk_json_write_named_bool(w, "guard",
						   spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD));
			spdk_json_write_object_end(w);
		}
	}

	spdk_json_write_named_object_begin(w, "assigned_rate_limits");
	spdk_bdev_get_qos_rate_limits(bdev, qos_limits);
	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		spdk_json_write_named_uint64(w, spdk_bdev_get_qos_rpc_type(i), qos_limits[i]);
	}
	spdk_json_write_object_end(w);

	spdk_json_write_named_bool(w, "claimed",
				   (bdev->internal.claim_type != SPDK_BDEV_CLAIM_NONE));

	spdk_json_write_named_bool(w, "zoned", bdev->zoned);
	if (bdev->zoned) {
		spdk_json_write_named_uint64(w, "zone_size", bdev->zone_size);
		spdk_json_write_named_uint64(w, "max_open_zones", bdev->max_open_zones);
		spdk_json_write_named_uint64(w, "optimal_open_zones", bdev->optimal_open_zones);
	}

	spdk_json_write_named_object_begin(w, "supported_io_types");
	spdk_json_write_named_bool(w, "read",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ));
	spdk_json_write_named_bool(w, "write",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE));
	spdk_json_write_named_bool(w, "unmap",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP));
	spdk_json_write_named_bool(w, "write_zeroes",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES));
	spdk_json_write_named_bool(w, "flush",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH));
	spdk_json_write_named_bool(w, "reset",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESET));
	spdk_json_write_named_bool(w, "compare",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE));
	spdk_json_write_named_bool(w, "compare_and_write",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE));
	spdk_json_write_named_bool(w, "abort",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ABORT));
	spdk_json_write_named_bool(w, "nvme_admin",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN));
	spdk_json_write_named_bool(w, "nvme_io",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO));
	spdk_json_write_object_end(w);

	rc = spdk_bdev_get_memory_domains(bdev, NULL, 0);
	if (rc > 0) {
		domains = calloc(rc, sizeof(struct spdk_memory_domain *));
		if (domains) {
			i = spdk_bdev_get_memory_domains(bdev, domains, rc);
			if (i == rc) {
				spdk_json_write_named_array_begin(w, "memory_domains");
				for (i = 0; i < rc; i++) {
					spdk_json_write_object_begin(w);
					spdk_json_write_named_string(w, "dma_device_id", spdk_memory_domain_get_dma_device_id(domains[i]));
					spdk_json_write_named_int32(w, "dma_device_type",
								    spdk_memory_domain_get_dma_device_type(domains[i]));
					spdk_json_write_object_end(w);
				}
				spdk_json_write_array_end(w);
			} else {
				SPDK_ERRLOG("Unexpected number of memory domains %d (should be %d)\n", i, rc);
			}

			free(domains);
		} else {
			SPDK_ERRLOG("Memory allocation failed\n");
		}
	}

	spdk_json_write_named_object_begin(w, "driver_specific");
	spdk_bdev_dump_info_json(bdev, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

struct rpc_bdev_get_bdevs {
	char		*name;
	uint64_t	timeout;
};

struct rpc_bdev_get_bdevs_ctx {
	struct rpc_bdev_get_bdevs	rpc;
	struct spdk_jsonrpc_request	*request;
	struct spdk_poller		*poller;
	uint64_t			timeout_ticks;
};

static void
free_rpc_bdev_get_bdevs(struct rpc_bdev_get_bdevs *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_get_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_bdevs, name), spdk_json_decode_string, true},
	{"timeout", offsetof(struct rpc_bdev_get_bdevs, timeout), spdk_json_decode_uint64, true},
};

static int
get_bdevs_poller(void *_ctx)
{
	struct rpc_bdev_get_bdevs_ctx *ctx = _ctx;
	struct spdk_json_write_ctx *w;
	struct spdk_bdev_desc *desc;
	int rc;

	rc = spdk_bdev_open_ext(ctx->rpc.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0 && spdk_get_ticks() < ctx->timeout_ticks) {
		return SPDK_POLLER_BUSY;
	}

	if (rc != 0) {
		SPDK_ERRLOG("Timed out while waiting for bdev '%s' to appear\n", ctx->rpc.name);
		spdk_jsonrpc_send_error_response(ctx->request, -ENODEV, spdk_strerror(ENODEV));
	} else {
		w = spdk_jsonrpc_begin_result(ctx->request);
		spdk_json_write_array_begin(w);
		rpc_dump_bdev_info(w, spdk_bdev_desc_get_bdev(desc));
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(ctx->request, w);

		spdk_bdev_close(desc);
	}

	spdk_poller_unregister(&ctx->poller);
	free_rpc_bdev_get_bdevs(&ctx->rpc);
	free(ctx);

	return SPDK_POLLER_BUSY;
}

static void
rpc_bdev_get_bdevs(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_bdev_get_bdevs req = {};
	struct rpc_bdev_get_bdevs_ctx *ctx;
	struct spdk_json_write_ctx *w;
	struct spdk_bdev_desc *desc = NULL;
	int rc;

	if (params && spdk_json_decode_object(params, rpc_bdev_get_bdevs_decoders,
					      SPDK_COUNTOF(rpc_bdev_get_bdevs_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_bdev_get_bdevs(&req);
		return;
	}

	if (req.name) {
		rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
		if (rc != 0) {
			if (req.timeout == 0) {
				SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
				spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
				free_rpc_bdev_get_bdevs(&req);
				return;
			}

			ctx = calloc(1, sizeof(*ctx));
			if (ctx == NULL) {
				SPDK_ERRLOG("Failed to allocate bdev_get_bdevs context\n");
				spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
				free_rpc_bdev_get_bdevs(&req);
				return;
			}

			ctx->poller = SPDK_POLLER_REGISTER(get_bdevs_poller, ctx, 10 * 1000);
			if (ctx->poller == NULL) {
				SPDK_ERRLOG("Failed to register bdev_get_bdevs poller\n");
				spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
				free_rpc_bdev_get_bdevs(&req);
				free(ctx);
				return;
			}

			memcpy(&ctx->rpc, &req, sizeof(req));
			ctx->timeout_ticks = spdk_get_ticks() + req.timeout *
					     spdk_get_ticks_hz() / 1000ull;
			ctx->request = request;
			return;
		}
	}

	free_rpc_bdev_get_bdevs(&req);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (desc != NULL) {
		rpc_dump_bdev_info(w, spdk_bdev_desc_get_bdev(desc));
		spdk_bdev_close(desc);
	} else {
		spdk_for_each_bdev(w, rpc_dump_bdev_info);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_get_bdevs", rpc_bdev_get_bdevs, SPDK_RPC_RUNTIME)

struct rpc_bdev_set_qd_sampling_period {
	char *name;
	uint64_t period;
};

static void
free_rpc_bdev_set_qd_sampling_period(struct rpc_bdev_set_qd_sampling_period *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder
	rpc_bdev_set_qd_sampling_period_decoders[] = {
	{"name", offsetof(struct rpc_bdev_set_qd_sampling_period, name), spdk_json_decode_string},
	{"period", offsetof(struct rpc_bdev_set_qd_sampling_period, period), spdk_json_decode_uint64},
};

static void
rpc_bdev_set_qd_sampling_period(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_set_qd_sampling_period req = {0};
	struct spdk_bdev_desc *desc;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_set_qd_sampling_period_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qd_sampling_period_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_bdev_set_qd_sampling_period(spdk_bdev_desc_get_bdev(desc), req.period);
	spdk_jsonrpc_send_bool_response(request, true);

	spdk_bdev_close(desc);

cleanup:
	free_rpc_bdev_set_qd_sampling_period(&req);
}
SPDK_RPC_REGISTER("bdev_set_qd_sampling_period",
		  rpc_bdev_set_qd_sampling_period,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_set_qos_limit {
	char		*name;
	uint64_t	limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
};

static void
free_rpc_bdev_set_qos_limit(struct rpc_bdev_set_qos_limit *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_set_qos_limit_decoders[] = {
	{"name", offsetof(struct rpc_bdev_set_qos_limit, name), spdk_json_decode_string},
	{
		"rw_ios_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					   limits[SPDK_BDEV_QOS_RW_IOPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		"rw_mbytes_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					      limits[SPDK_BDEV_QOS_RW_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		"r_mbytes_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					     limits[SPDK_BDEV_QOS_R_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
	{
		"w_mbytes_per_sec", offsetof(struct rpc_bdev_set_qos_limit,
					     limits[SPDK_BDEV_QOS_W_BPS_RATE_LIMIT]),
		spdk_json_decode_uint64, true
	},
};

static void
rpc_bdev_set_qos_limit_complete(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (status != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to configure rate limit: %s",
						     spdk_strerror(-status));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_set_qos_limit(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_set_qos_limit req = {NULL, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
	struct spdk_bdev_desc *desc;
	int i, rc;

	if (spdk_json_decode_object(params, rpc_bdev_set_qos_limit_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qos_limit_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to open bdev '%s': %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (req.limits[i] != UINT64_MAX) {
			break;
		}
	}
	if (i == SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES) {
		SPDK_ERRLOG("no rate limits specified\n");
		spdk_bdev_close(desc);
		spdk_jsonrpc_send_error_response(request, -EINVAL, "No rate limits specified");
		goto cleanup;
	}

	spdk_bdev_set_qos_rate_limits(spdk_bdev_desc_get_bdev(desc), req.limits,
				      rpc_bdev_set_qos_limit_complete, request);

	spdk_bdev_close(desc);

cleanup:
	free_rpc_bdev_set_qos_limit(&req);
}

SPDK_RPC_REGISTER("bdev_set_qos_limit", rpc_bdev_set_qos_limit, SPDK_RPC_RUNTIME)

/* SPDK_RPC_ENABLE_BDEV_HISTOGRAM */

struct rpc_bdev_enable_histogram_request {
	char *name;
	bool enable;
};

static void
free_rpc_bdev_enable_histogram_request(struct rpc_bdev_enable_histogram_request *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_enable_histogram_request_decoders[] = {
	{"name", offsetof(struct rpc_bdev_enable_histogram_request, name), spdk_json_decode_string},
	{"enable", offsetof(struct rpc_bdev_enable_histogram_request, enable), spdk_json_decode_bool},
};

static void
bdev_histogram_status_cb(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, status == 0);
}

static void
rpc_bdev_enable_histogram(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_enable_histogram_request req = {NULL};
	struct spdk_bdev_desc *desc;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_enable_histogram_request_decoders,
				    SPDK_COUNTOF(rpc_bdev_enable_histogram_request_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_bdev_histogram_enable(spdk_bdev_desc_get_bdev(desc), bdev_histogram_status_cb,
				   request, req.enable);

	spdk_bdev_close(desc);

cleanup:
	free_rpc_bdev_enable_histogram_request(&req);
}

SPDK_RPC_REGISTER("bdev_enable_histogram", rpc_bdev_enable_histogram, SPDK_RPC_RUNTIME)

/* SPDK_RPC_GET_BDEV_HISTOGRAM */

struct rpc_bdev_get_histogram_request {
	char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_get_histogram_request_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_histogram_request, name), spdk_json_decode_string}
};

static void
free_rpc_bdev_get_histogram_request(struct rpc_bdev_get_histogram_request *r)
{
	free(r->name);
}

static void
_rpc_bdev_histogram_data_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;
	int rc;
	char *encoded_histogram;
	size_t src_len, dst_len;


	if (status != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-status));
		goto invalid;
	}

	src_len = SPDK_HISTOGRAM_NUM_BUCKETS(histogram) * sizeof(uint64_t);
	dst_len = spdk_base64_get_encoded_strlen(src_len) + 1;

	encoded_histogram = malloc(dst_len);
	if (encoded_histogram == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		goto invalid;
	}

	rc = spdk_base64_encode(encoded_histogram, histogram->bucket, src_len);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto free_encoded_histogram;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "histogram", encoded_histogram);
	spdk_json_write_named_int64(w, "bucket_shift", histogram->bucket_shift);
	spdk_json_write_named_int64(w, "tsc_rate", spdk_get_ticks_hz());
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

free_encoded_histogram:
	free(encoded_histogram);
invalid:
	spdk_histogram_data_free(histogram);
}

static void
rpc_bdev_get_histogram(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_get_histogram_request req = {NULL};
	struct spdk_histogram_data *histogram;
	struct spdk_bdev_desc *desc;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_get_histogram_request_decoders,
				    SPDK_COUNTOF(rpc_bdev_get_histogram_request_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	histogram = spdk_histogram_data_alloc();
	if (histogram == NULL) {
		spdk_bdev_close(desc);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	spdk_bdev_histogram_get(spdk_bdev_desc_get_bdev(desc), histogram,
				_rpc_bdev_histogram_data_cb, request);

	spdk_bdev_close(desc);

cleanup:
	free_rpc_bdev_get_histogram_request(&req);
}

SPDK_RPC_REGISTER("bdev_get_histogram", rpc_bdev_get_histogram, SPDK_RPC_RUNTIME)
