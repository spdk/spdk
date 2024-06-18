/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation. All rights reserved.
 */

#include "keyring_linux.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

static const struct spdk_json_object_decoder rpc_keyring_linux_set_options_decoders[] = {
	{"enable", offsetof(struct keyring_linux_opts, enable), spdk_json_decode_bool, true},
};

static void
rpc_keyring_linux_set_options(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct keyring_linux_opts opts = {};
	int rc;

	keyring_linux_get_opts(&opts);
	if (spdk_json_decode_object(params, rpc_keyring_linux_set_options_decoders,
				    SPDK_COUNTOF(rpc_keyring_linux_set_options_decoders), &opts)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		return;
	}

	rc = keyring_linux_set_opts(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("keyring_linux_set_options", rpc_keyring_linux_set_options, SPDK_RPC_STARTUP)
