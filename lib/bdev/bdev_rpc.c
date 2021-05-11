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

#include "spdk/bdev.h"

#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/base64.h"
#include "spdk/bdev_module.h"

#include "spdk/log.h"

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
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_set_options, set_bdev_options)

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

struct rpc_bdev_get_iostat_ctx {
	int bdev_count;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static void
rpc_bdev_get_iostat_cb(struct spdk_bdev *bdev,
		       struct spdk_bdev_io_stat *stat, void *cb_arg, int rc)
{
	struct rpc_bdev_get_iostat_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w = ctx->w;
	const char *bdev_name;

	if (rc != 0) {
		goto done;
	}

	bdev_name = spdk_bdev_get_name(bdev);
	if (bdev_name != NULL) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "name", bdev_name);

		spdk_json_write_named_uint64(w, "bytes_read", stat->bytes_read);

		spdk_json_write_named_uint64(w, "num_read_ops", stat->num_read_ops);

		spdk_json_write_named_uint64(w, "bytes_written", stat->bytes_written);

		spdk_json_write_named_uint64(w, "num_write_ops", stat->num_write_ops);

		spdk_json_write_named_uint64(w, "bytes_unmapped", stat->bytes_unmapped);

		spdk_json_write_named_uint64(w, "num_unmap_ops", stat->num_unmap_ops);

		spdk_json_write_named_uint64(w, "read_latency_ticks", stat->read_latency_ticks);

		spdk_json_write_named_uint64(w, "write_latency_ticks", stat->write_latency_ticks);

		spdk_json_write_named_uint64(w, "unmap_latency_ticks", stat->unmap_latency_ticks);

		if (spdk_bdev_get_qd_sampling_period(bdev)) {
			spdk_json_write_named_uint64(w, "queue_depth_polling_period",
						     spdk_bdev_get_qd_sampling_period(bdev));

			spdk_json_write_named_uint64(w, "queue_depth", spdk_bdev_get_qd(bdev));

			spdk_json_write_named_uint64(w, "io_time", spdk_bdev_get_io_time(bdev));

			spdk_json_write_named_uint64(w, "weighted_io_time",
						     spdk_bdev_get_weighted_io_time(bdev));
		}

		spdk_json_write_object_end(w);
	}

done:
	free(stat);
	if (--ctx->bdev_count == 0) {
		spdk_json_write_array_end(ctx->w);
		spdk_json_write_object_end(w);
		spdk_jsonrpc_end_result(ctx->request, ctx->w);
		free(ctx);
	}
}

struct rpc_bdev_get_iostat {
	char *name;
};

static void
free_rpc_bdev_get_iostat(struct rpc_bdev_get_iostat *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_get_iostat_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_iostat, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_get_iostat(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_get_iostat req = {};
	struct spdk_bdev *bdev = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_bdev_io_stat *stat;
	struct rpc_bdev_get_iostat_ctx *ctx;

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

		if (req.name) {
			bdev = spdk_bdev_get_by_name(req.name);
			if (bdev == NULL) {
				SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
				spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
				free_rpc_bdev_get_iostat(&req);
				return;
			}
		}
	}

	free_rpc_bdev_get_iostat(&req);

	ctx = calloc(1, sizeof(struct rpc_bdev_get_iostat_ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_bdev_get_iostat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	ctx->bdev_count++;
	ctx->request = request;
	ctx->w = w;


	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint64(w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_uint64(w, "ticks", spdk_get_ticks());

	spdk_json_write_named_array_begin(w, "bdevs");

	if (bdev != NULL) {
		stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
		if (stat == NULL) {
			SPDK_ERRLOG("Failed to allocate rpc_bdev_get_iostat_ctx struct\n");
		} else {
			ctx->bdev_count++;
			spdk_bdev_get_device_stat(bdev, stat, rpc_bdev_get_iostat_cb, ctx);
		}
	} else {
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
			if (stat == NULL) {
				SPDK_ERRLOG("Failed to allocate spdk_bdev_io_stat struct\n");
				break;
			}
			ctx->bdev_count++;
			spdk_bdev_get_device_stat(bdev, stat, rpc_bdev_get_iostat_cb, ctx);
		}
	}

	if (--ctx->bdev_count == 0) {
		spdk_json_write_array_end(w);
		spdk_json_write_object_end(w);
		spdk_jsonrpc_end_result(request, w);
		free(ctx);
	}
}
SPDK_RPC_REGISTER("bdev_get_iostat", rpc_bdev_get_iostat, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_get_iostat, get_bdevs_iostat)

static void
rpc_dump_bdev_info(struct spdk_json_write_ctx *w,
		   struct spdk_bdev *bdev)
{
	struct spdk_bdev_alias *tmp;
	uint64_t qos_limits[SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES];
	int i;

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

	spdk_json_write_named_bool(w, "claimed", (bdev->internal.claim_module != NULL));

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
	spdk_json_write_named_bool(w, "nvme_admin",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN));
	spdk_json_write_named_bool(w, "nvme_io",
				   spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO));
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "driver_specific");
	spdk_bdev_dump_info_json(bdev, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

struct rpc_bdev_get_bdevs {
	char *name;
};

static void
free_rpc_bdev_get_bdevs(struct rpc_bdev_get_bdevs *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_get_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_bdev_get_bdevs, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_get_bdevs(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_bdev_get_bdevs req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;

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
		bdev = spdk_bdev_get_by_name(req.name);
		if (bdev == NULL) {
			SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free_rpc_bdev_get_bdevs(&req);
			return;
		}
	}

	free_rpc_bdev_get_bdevs(&req);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (bdev != NULL) {
		rpc_dump_bdev_info(w, bdev);
	} else {
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			rpc_dump_bdev_info(w, bdev);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_get_bdevs", rpc_bdev_get_bdevs, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_get_bdevs, get_bdevs)

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
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_bdev_set_qd_sampling_period_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qd_sampling_period_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_bdev_set_qd_sampling_period(bdev, req.period);
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_set_qd_sampling_period(&req);
}
SPDK_RPC_REGISTER("bdev_set_qd_sampling_period",
		  rpc_bdev_set_qd_sampling_period,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_set_qd_sampling_period,
				   set_bdev_qd_sampling_period)

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
	struct spdk_bdev *bdev;
	int i;

	if (spdk_json_decode_object(params, rpc_bdev_set_qos_limit_decoders,
				    SPDK_COUNTOF(rpc_bdev_set_qos_limit_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	for (i = 0; i < SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES; i++) {
		if (req.limits[i] != UINT64_MAX) {
			break;
		}
	}
	if (i == SPDK_BDEV_QOS_NUM_RATE_LIMIT_TYPES) {
		SPDK_ERRLOG("no rate limits specified\n");
		spdk_jsonrpc_send_error_response(request, -EINVAL, "No rate limits specified");
		goto cleanup;
	}

	spdk_bdev_set_qos_rate_limits(bdev, req.limits, rpc_bdev_set_qos_limit_complete, request);

cleanup:
	free_rpc_bdev_set_qos_limit(&req);
}

SPDK_RPC_REGISTER("bdev_set_qos_limit", rpc_bdev_set_qos_limit, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_set_qos_limit, set_bdev_qos_limit)

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
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_bdev_enable_histogram_request_decoders,
				    SPDK_COUNTOF(rpc_bdev_enable_histogram_request_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_bdev_histogram_enable(bdev, bdev_histogram_status_cb, request, req.enable);

cleanup:
	free_rpc_bdev_enable_histogram_request(&req);
}

SPDK_RPC_REGISTER("bdev_enable_histogram", rpc_bdev_enable_histogram, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_enable_histogram, enable_bdev_histogram)

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
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_bdev_get_histogram_request_decoders,
				    SPDK_COUNTOF(rpc_bdev_get_histogram_request_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	histogram = spdk_histogram_data_alloc();
	if (histogram == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	spdk_bdev_histogram_get(bdev, histogram, _rpc_bdev_histogram_data_cb, request);

cleanup:
	free_rpc_bdev_get_histogram_request(&req);
}

SPDK_RPC_REGISTER("bdev_get_histogram", rpc_bdev_get_histogram, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_get_histogram, get_bdev_histogram)
