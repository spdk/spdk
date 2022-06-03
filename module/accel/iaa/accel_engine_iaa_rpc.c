/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "accel_engine_iaa.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"

static void
rpc_iaa_scan_accel_engine(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iaa_scan_accel_engine requires no parameters");
		return;
	}

	SPDK_NOTICELOG("Enabling IAA user-mode\n");
	accel_engine_iaa_enable_probe();
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iaa_scan_accel_engine", rpc_iaa_scan_accel_engine, SPDK_RPC_STARTUP)
