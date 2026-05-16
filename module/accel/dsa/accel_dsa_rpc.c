/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "accel_dsa.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk_internal/rpc_autogen.h"

static const struct spdk_json_object_decoder rpc_dsa_scan_accel_module_decoders[] = {
	{"config_kernel_mode", offsetof(struct rpc_dsa_scan_accel_module_ctx, config_kernel_mode), spdk_json_decode_bool, true},
};

static void
rpc_dsa_scan_accel_module(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_dsa_scan_accel_module_ctx req = {};
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_dsa_scan_accel_module_decoders,
					    SPDK_COUNTOF(rpc_dsa_scan_accel_module_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	rc = accel_dsa_enable_probe(req.config_kernel_mode);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	if (req.config_kernel_mode) {
		SPDK_NOTICELOG("Enabled DSA kernel-mode\n");
	} else {
		SPDK_NOTICELOG("Enabled DSA user-mode\n");
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("dsa_scan_accel_module", rpc_dsa_scan_accel_module, SPDK_RPC_STARTUP)
