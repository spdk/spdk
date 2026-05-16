/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "accel_dpdk_compressdev.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

static const struct spdk_json_object_decoder rpc_compressdev_scan_accel_module_decoders[] = {
	{"pmd", offsetof(struct rpc_compressdev_scan_accel_module_ctx, pmd), spdk_json_decode_uint32},
};

static void
rpc_compressdev_scan_accel_module(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_compressdev_scan_accel_module_ctx req;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_compressdev_scan_accel_module_decoders,
				    SPDK_COUNTOF(rpc_compressdev_scan_accel_module_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	if (req.pmd >= COMPRESS_PMD_MAX) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "PMD value %d should be less than %d", req.pmd, COMPRESS_PMD_MAX);
		return;
	}

	rc = accel_compressdev_enable_probe(&req.pmd);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	accel_dpdk_compressdev_enable();
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("compressdev_scan_accel_module", rpc_compressdev_scan_accel_module,
		  SPDK_RPC_STARTUP)
