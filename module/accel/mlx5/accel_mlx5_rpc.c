/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "accel_mlx5.h"

static const struct spdk_json_object_decoder rpc_mlx5_module_decoder[] = {
	{"qp_size", offsetof(struct accel_mlx5_attr, qp_size), spdk_json_decode_uint16, true},
	{"num_requests", offsetof(struct accel_mlx5_attr, num_requests), spdk_json_decode_uint32, true},
};

static void
rpc_mlx5_scan_accel_module(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct accel_mlx5_attr attr;
	int rc;

	accel_mlx5_get_default_attr(&attr);

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_mlx5_module_decoder,
					    SPDK_COUNTOF(rpc_mlx5_module_decoder),
					    &attr)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
							 "spdk_json_decode_object failed");
			return;
		}
	}

	rc = accel_mlx5_enable(&attr);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "mlx5 scan failed with %d\n", rc);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}
SPDK_RPC_REGISTER("mlx5_scan_accel_module", rpc_mlx5_scan_accel_module, SPDK_RPC_STARTUP)
