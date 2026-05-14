/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"
#include "accel_mlx5.h"

static void
rpc_mlx5_scan_accel_module(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct accel_mlx5_attr attr = {};
	struct rpc_mlx5_scan_accel_module_ctx req = {};
	int rc;

	accel_mlx5_get_default_attr(&attr);
	req.qp_size = attr.qp_size;
	req.num_requests = attr.num_requests;
	req.allowed_devs = attr.allowed_devs;
	req.crypto_split_blocks = attr.crypto_split_blocks;
	req.enable_driver = attr.enable_driver;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_mlx5_scan_accel_module_decoders,
					    SPDK_COUNTOF(rpc_mlx5_scan_accel_module_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			free_rpc_mlx5_scan_accel_module(&req);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
							 "spdk_json_decode_object failed");
			return;
		}
	}

	attr.qp_size = req.qp_size;
	attr.num_requests = req.num_requests;
	attr.allowed_devs = req.allowed_devs;
	attr.crypto_split_blocks = req.crypto_split_blocks;
	attr.enable_driver = req.enable_driver;

	rc = accel_mlx5_enable(&attr);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "mlx5 scan failed with %d", rc);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
	free_rpc_mlx5_scan_accel_module(&req);
}
SPDK_RPC_REGISTER("mlx5_scan_accel_module", rpc_mlx5_scan_accel_module, SPDK_RPC_STARTUP)

struct rpc_accel_mlx5_dump_stats_ext {
	struct rpc_accel_mlx5_dump_stats_ctx req;
	struct spdk_json_write_ctx *w;
};

static void
accel_mlx5_dump_stats_done(void *_ctx, int rc)
{
	struct rpc_accel_mlx5_dump_stats_ext *ereq = _ctx;
	if (rc) {
		spdk_jsonrpc_send_error_response(ereq->req.request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to dump stats");
	} else {
		spdk_jsonrpc_end_result(ereq->req.request, ereq->w);
	}
	free_rpc_accel_mlx5_dump_stats(&ereq->req);
	free(ereq);
}

static void
rpc_accel_mlx5_dump_stats(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_accel_mlx5_dump_stats_ext *ereq;
	int rc;

	ereq = calloc(1, sizeof(*ereq));
	if (!ereq) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "memory allocation failed");
		return;
	}

	ereq->req.request = request;
	ereq->req.level = RPC_ACCEL_MLX5_DUMP_STATE_LEVEL_CHANNEL;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_accel_mlx5_dump_stats_decoders,
					    SPDK_COUNTOF(rpc_accel_mlx5_dump_stats_decoders),
					    &ereq->req)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
							 "spdk_json_decode_object failed");
			free_rpc_accel_mlx5_dump_stats(&ereq->req);
			free(ereq);
			return;
		}
	}

	ereq->w = spdk_jsonrpc_begin_result(ereq->req.request);
	rc = accel_mlx5_dump_stats(ereq->w, (enum spdk_accel_mlx5_dump_state_level)ereq->req.level,
				   accel_mlx5_dump_stats_done, ereq);
	if (rc) {
		spdk_json_write_null(ereq->w);
		spdk_jsonrpc_end_result(ereq->req.request, ereq->w);
		free_rpc_accel_mlx5_dump_stats(&ereq->req);
		free(ereq);
	}
}
SPDK_RPC_REGISTER("accel_mlx5_dump_stats", rpc_accel_mlx5_dump_stats, SPDK_RPC_RUNTIME)
