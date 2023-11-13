/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/fsdev.h"

static void
rpc_fsdev_get_opts(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_fsdev_opts opts = {};
	int rc;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'fsdev_get_opts' requires no arguments");
		return;
	}

	rc = spdk_fsdev_get_opts(&opts, sizeof(opts));
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_get_opts failed with %d", rc);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "fsdev_io_pool_size", opts.fsdev_io_pool_size);
	spdk_json_write_named_uint32(w, "fsdev_io_cache_size", opts.fsdev_io_cache_size);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("fsdev_get_opts", rpc_fsdev_get_opts, SPDK_RPC_RUNTIME)

struct rpc_fsdev_set_opts {
	uint32_t fsdev_io_pool_size;
	uint32_t fsdev_io_cache_size;
};

static const struct spdk_json_object_decoder rpc_fsdev_set_opts_decoders[] = {
	{"fsdev_io_pool_size", offsetof(struct rpc_fsdev_set_opts, fsdev_io_pool_size), spdk_json_decode_uint32, false},
	{"fsdev_io_cache_size", offsetof(struct rpc_fsdev_set_opts, fsdev_io_cache_size), spdk_json_decode_uint32, false},
};

static void
rpc_fsdev_set_opts(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsdev_set_opts req = {};
	int rc;
	struct spdk_fsdev_opts opts = {};

	if (spdk_json_decode_object(params, rpc_fsdev_set_opts_decoders,
				    SPDK_COUNTOF(rpc_fsdev_set_opts_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_fsdev_get_opts(&opts, sizeof(opts));
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_get_opts failed with %d", rc);
		return;
	}

	opts.fsdev_io_pool_size = req.fsdev_io_pool_size;
	opts.fsdev_io_cache_size = req.fsdev_io_cache_size;

	rc = spdk_fsdev_set_opts(&opts);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "spdk_fsdev_set_opts failed with %d", rc);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("fsdev_set_opts", rpc_fsdev_set_opts, SPDK_RPC_RUNTIME)
