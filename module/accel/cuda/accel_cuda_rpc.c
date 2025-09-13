/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) 2025 StarWind Software, Inc.
 *   All rights reserved.
 */

#include "accel_cuda.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"

static void
rpc_cuda_scan_accel_module(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "cuda_scan_accel_module requires no parameters");
		return;
	}

	SPDK_NOTICELOG("Enabling accel_cuda\n");
	accel_cuda_enable_probe();

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("cuda_scan_accel_module", rpc_cuda_scan_accel_module, SPDK_RPC_STARTUP)
