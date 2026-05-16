/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"
#include "accel_mlx5.h"

/* TODO: replace with rpc_mlx5_scan_accel_module_ctx */
static const struct spdk_json_object_decoder rpc_mlx5_scan_accel_module_decoders[] = {
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
		if (spdk_json_decode_object(params, rpc_mlx5_scan_accel_module_decoders,
					    SPDK_COUNTOF(rpc_mlx5_scan_accel_module_decoders),
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

/* TODO: replace with rpc_accel_mlx5_dump_stats_ctx */
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

static const struct spdk_json_object_decoder rpc_accel_mlx5_dump_stats_decoders[] = {
	{"level", 0, rpc_decode_accel_mlx5_dump_state_level, true},
};

static void
rpc_accel_mlx5_dump_stats(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct accel_mlx5_rpc_dump_stats_ctx *ctx;
	enum spdk_accel_mlx5_dump_state_level level = SPDK_ACCEL_MLX5_DUMP_STAT_LEVEL_CHANNEL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_accel_mlx5_dump_stats_decoders,
					    SPDK_COUNTOF(rpc_accel_mlx5_dump_stats_decoders),
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
