/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "accel_iaa.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"

static void
rpc_iaa_scan_accel_module(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "iaa_scan_accel_module requires no parameters");
		return;
	}

	SPDK_NOTICELOG("Enabling IAA user-mode\n");
	accel_iaa_enable_probe();
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iaa_scan_accel_module", rpc_iaa_scan_accel_module, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(iaa_scan_accel_module, iaa_scan_accel_engine)
