/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Nutanix Inc. All rights reserved.
 */

#include "sock_error.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

static void
rpc_sock_error_inject_error(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_sock_error_inject_error_ctx req = {.count = UINT64_MAX};
	struct sock_error_inject_opts opts = {};
	int rc;

	rc = spdk_json_decode_object(params, rpc_sock_error_inject_error_decoders,
				     SPDK_COUNTOF(rpc_sock_error_inject_error_decoders), &req);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}

	opts.operation = (enum spdk_sock_error_operation)req.operation;
	opts.type = (enum spdk_sock_error_type)req.type;
	opts.count = req.count;
	opts.interval = req.interval;
	opts.errcode = req.errcode;
	opts.match_len = req.match_len;

	rc = sock_error_inject_error(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("sock_error_inject_error", rpc_sock_error_inject_error, SPDK_RPC_RUNTIME)

static void
rpc_sock_error_register(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "sock_error_register requires no parameters");
		return;
	}

	rc = sock_error_register_impl("posix");
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("sock_error_register", rpc_sock_error_register, SPDK_RPC_STARTUP)
