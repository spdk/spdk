/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "accel_engine_dsa.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"

struct rpc_dsa_scan_accel_engine {
	bool config_kernel_mode;
};

static const struct spdk_json_object_decoder rpc_dsa_scan_accel_engine_decoder[] = {
	{"config_kernel_mode", offsetof(struct rpc_dsa_scan_accel_engine, config_kernel_mode), spdk_json_decode_bool, true},
};

static void
rpc_dsa_scan_accel_engine(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_dsa_scan_accel_engine req = {};

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_dsa_scan_accel_engine_decoder,
					    SPDK_COUNTOF(rpc_dsa_scan_accel_engine_decoder),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	if (req.config_kernel_mode) {
		SPDK_NOTICELOG("Enabling DSA kernel-mode\n");
	} else {
		SPDK_NOTICELOG("Enabling DSA user-mode\n");
	}

	accel_engine_dsa_enable_probe(req.config_kernel_mode);
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("dsa_scan_accel_engine", rpc_dsa_scan_accel_engine, SPDK_RPC_STARTUP)
