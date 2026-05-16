/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#include "spdk/json.h"
#include "spdk/module/keyring/file.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/rpc_autogen.h"


static const struct spdk_json_object_decoder rpc_keyring_file_add_key_decoders[] = {
	{"name", offsetof(struct rpc_keyring_file_add_key_ctx, name), spdk_json_decode_string},
	{"path", offsetof(struct rpc_keyring_file_add_key_ctx, path), spdk_json_decode_string},
};


static void
rpc_keyring_file_add_key(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_keyring_file_add_key_ctx opts = {};
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_keyring_file_add_key_decoders,
					    SPDK_COUNTOF(rpc_keyring_file_add_key_decoders),
					    &opts)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		return;
	}

	rc = spdk_keyring_file_add_key(opts.name, opts.path);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	free_rpc_keyring_file_add_key(&opts);
}
SPDK_RPC_REGISTER("keyring_file_add_key", rpc_keyring_file_add_key, SPDK_RPC_RUNTIME)


static const struct spdk_json_object_decoder rpc_keyring_file_remove_key_decoders[] = {
	{"name", offsetof(struct rpc_keyring_file_remove_key_ctx, name), spdk_json_decode_string},
};


static void
rpc_keyring_file_remove_key(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_keyring_file_remove_key_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_keyring_file_remove_key_decoders,
				    SPDK_COUNTOF(rpc_keyring_file_remove_key_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		return;
	}

	rc = spdk_keyring_file_remove_key(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	free_rpc_keyring_file_remove_key(&req);
}
SPDK_RPC_REGISTER("keyring_file_remove_key", rpc_keyring_file_remove_key, SPDK_RPC_RUNTIME)
