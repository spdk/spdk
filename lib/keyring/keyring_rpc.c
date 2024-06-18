/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#include "keyring_internal.h"
#include "spdk/keyring.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

static void
rpc_keyring_for_each_key_cb(void *ctx, struct spdk_key *key)
{
	struct spdk_json_write_ctx *w = ctx;

	spdk_json_write_object_begin(w);
	keyring_dump_key_info(key, w);
	spdk_json_write_object_end(w);
}

static void
rpc_keyring_get_keys(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	spdk_keyring_for_each_key(NULL, w, rpc_keyring_for_each_key_cb, SPDK_KEYRING_FOR_EACH_ALL);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

}
SPDK_RPC_REGISTER("keyring_get_keys", rpc_keyring_get_keys, SPDK_RPC_RUNTIME)
