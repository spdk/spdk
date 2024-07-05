/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "accel_mlx5.h"

static const struct spdk_json_object_decoder rpc_mlx5_module_decoder[] = {
	{"qp_size", offsetof(struct accel_mlx5_attr, qp_size), spdk_json_decode_uint16, true},
	{"num_requests", offsetof(struct accel_mlx5_attr, num_requests), spdk_json_decode_uint32, true},
	{"allowed_devs", offsetof(struct accel_mlx5_attr, allowed_devs), spdk_json_decode_string, true},
	{"crypto_split_blocks", offsetof(struct accel_mlx5_attr, crypto_split_blocks), spdk_json_decode_uint16, true},
	{"enable_driver", offsetof(struct accel_mlx5_attr, enable_driver), spdk_json_decode_bool, true},
};

static void
rpc_mlx5_scan_accel_module(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct accel_mlx5_attr attr = {};
	int rc;

	accel_mlx5_get_default_attr(&attr);

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_mlx5_module_decoder,
					    SPDK_COUNTOF(rpc_mlx5_module_decoder),
					    &attr)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			free(attr.allowed_devs);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
							 "spdk_json_decode_object failed");
			return;
		}
	}

	rc = accel_mlx5_enable(&attr);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "mlx5 scan failed with %d", rc);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
	free(attr.allowed_devs);
}
SPDK_RPC_REGISTER("mlx5_scan_accel_module", rpc_mlx5_scan_accel_module, SPDK_RPC_STARTUP)

static int
rpc_decode_dump_stat_level(const struct spdk_json_val *val, void *out)
{
	enum accel_mlx5_dump_state_level *level = out;

	if (spdk_json_strequal(val, "total") == true) {
		*level = ACCEL_MLX5_DUMP_STAT_LEVEL_TOTAL;
	} else if (spdk_json_strequal(val, "channel") == true) {
		*level = ACCEL_MLX5_DUMP_STAT_LEVEL_CHANNEL;
	} else if (spdk_json_strequal(val, "device") == true) {
		*level = ACCEL_MLX5_DUMP_STAT_LEVEL_DEV;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: level\n");
		return -EINVAL;
	}

	return 0;
}

struct accel_mlx5_rpc_dump_stats_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static void
accel_mlx5_dump_stats_done(void *_ctx, int rc)
{
	struct accel_mlx5_rpc_dump_stats_ctx *ctx = _ctx;
	if (rc) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to dump stats");
	} else {
		spdk_jsonrpc_end_result(ctx->request, ctx->w);
	}
	free(ctx);
}

static const struct spdk_json_object_decoder rpc_accel_mlx5_dump_stats_decoder[] = {
	{"level", 0, rpc_decode_dump_stat_level, true},
};

static void
rpc_accel_mlx5_dump_stats(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct accel_mlx5_rpc_dump_stats_ctx *ctx;
	enum accel_mlx5_dump_state_level level = ACCEL_MLX5_DUMP_STAT_LEVEL_CHANNEL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_accel_mlx5_dump_stats_decoder,
					    SPDK_COUNTOF(rpc_accel_mlx5_dump_stats_decoder),
					    &level)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
							 "spdk_json_decode_object failed");
			return;
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "memory allocation failed");
		return;
	}
	ctx->request = request;
	ctx->w = spdk_jsonrpc_begin_result(ctx->request);
	rc = accel_mlx5_dump_stats(ctx->w, level, accel_mlx5_dump_stats_done, ctx);
	if (rc) {
		spdk_json_write_null(ctx->w);
		spdk_jsonrpc_end_result(ctx->request, ctx->w);
		free(ctx);
	}
}
SPDK_RPC_REGISTER("accel_mlx5_dump_stats", rpc_accel_mlx5_dump_stats, SPDK_RPC_RUNTIME)
