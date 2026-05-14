/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
 */

#include "accel_error.h"
#include "spdk/accel.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/rpc_autogen.h"

static void
rpc_accel_error_inject_error(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_accel_error_inject_error_ctx req = {.count = UINT64_MAX};
	struct accel_error_inject_opts opts = {.count = UINT64_MAX};
	int rc;

	rc = spdk_json_decode_object(params, rpc_accel_error_inject_error_decoders,
				     SPDK_COUNTOF(rpc_accel_error_inject_error_decoders), &req);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}
	opts.opcode = (enum spdk_accel_opcode)req.opcode;
	opts.type = (enum accel_error_inject_type)req.type;
	opts.count = req.count;
	opts.interval = req.interval;
	opts.errcode = req.errcode;

	rc = accel_error_inject_error(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("accel_error_inject_error", rpc_accel_error_inject_error, SPDK_RPC_RUNTIME)
