/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#include "keyring_file.h"
#include "spdk/json.h"
#include "spdk/keyring_module.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

static const struct spdk_json_object_decoder keyring_file_key_opts_decoders[] = {
	{"name", offsetof(struct keyring_file_key_opts, name), spdk_json_decode_string},
	{"path", offsetof(struct keyring_file_key_opts, path), spdk_json_decode_string},
};

static void
free_keyring_file_key_opts(struct keyring_file_key_opts *opts)
{
	free(opts->name);
	free(opts->path);
}

static void
rpc_keyring_file_add_key(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct spdk_key_opts opts = {};
	struct keyring_file_key_opts kopts = {};
	int rc;

	if (spdk_json_decode_object_relaxed(params, keyring_file_key_opts_decoders,
					    SPDK_COUNTOF(keyring_file_key_opts_decoders),
					    &kopts)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		return;
	}

	opts.size = SPDK_SIZEOF(&opts, ctx);
	opts.name = kopts.name;
	opts.module = &g_keyring_file;
	opts.ctx = &kopts;
	rc = spdk_keyring_add_key(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	free_keyring_file_key_opts(&kopts);
}
SPDK_RPC_REGISTER("keyring_file_add_key", rpc_keyring_file_add_key, SPDK_RPC_RUNTIME)

struct rpc_keyring_file_remove_key {
	char *name;
};

static const struct spdk_json_object_decoder rpc_keyring_file_remove_key_decoders[] = {
	{"name", offsetof(struct rpc_keyring_file_remove_key, name), spdk_json_decode_string},
};

static void
free_rpc_keyring_file_remove_key(struct rpc_keyring_file_remove_key *r)
{
	free(r->name);
}

static void
rpc_keyring_file_remove_key(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_keyring_file_remove_key req = {};

	if (spdk_json_decode_object(params, rpc_keyring_file_remove_key_decoders,
				    SPDK_COUNTOF(rpc_keyring_file_remove_key_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		return;
	}

	spdk_keyring_remove_key(req.name);
	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_keyring_file_remove_key(&req);
}
SPDK_RPC_REGISTER("keyring_file_remove_key", rpc_keyring_file_remove_key, SPDK_RPC_RUNTIME)
