/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk_internal/init.h"

int iobuf_set_opts(struct spdk_iobuf_opts *opts);

static const struct spdk_json_object_decoder rpc_iobuf_set_options_decoders[] = {
	{"small_pool_count", offsetof(struct spdk_iobuf_opts, small_pool_count), spdk_json_decode_uint64, true},
	{"large_pool_count", offsetof(struct spdk_iobuf_opts, large_pool_count), spdk_json_decode_uint64, true},
	{"small_bufsize", offsetof(struct spdk_iobuf_opts, small_bufsize), spdk_json_decode_uint32, true},
	{"large_bufsize", offsetof(struct spdk_iobuf_opts, large_bufsize), spdk_json_decode_uint32, true},
};

static void
rpc_iobuf_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_iobuf_opts opts;
	int rc;

	spdk_iobuf_get_opts(&opts);
	rc = spdk_json_decode_object(params, rpc_iobuf_set_options_decoders,
				     SPDK_COUNTOF(rpc_iobuf_set_options_decoders), &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = iobuf_set_opts(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iobuf_set_options", rpc_iobuf_set_options, SPDK_RPC_STARTUP)
