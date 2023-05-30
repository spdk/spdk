/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk_internal/init.h"

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

	rc = spdk_iobuf_set_opts(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("iobuf_set_options", rpc_iobuf_set_options, SPDK_RPC_STARTUP)

static void
rpc_iobuf_get_stats_done(struct spdk_iobuf_module_stats *modules, uint32_t num_modules,
			 void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;
	struct spdk_iobuf_module_stats *it;
	uint32_t i;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	for (i = 0; i < num_modules; ++i) {
		it = &modules[i];

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "module", it->module);

		spdk_json_write_named_object_begin(w, "small_pool");
		spdk_json_write_named_uint64(w, "cache", it->small_pool.cache);
		spdk_json_write_named_uint64(w, "main", it->small_pool.main);
		spdk_json_write_named_uint64(w, "retry", it->small_pool.retry);
		spdk_json_write_object_end(w);

		spdk_json_write_named_object_begin(w, "large_pool");
		spdk_json_write_named_uint64(w, "cache", it->large_pool.cache);
		spdk_json_write_named_uint64(w, "main", it->large_pool.main);
		spdk_json_write_named_uint64(w, "retry", it->large_pool.retry);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_iobuf_get_stats(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc;

	rc = spdk_iobuf_get_stats(rpc_iobuf_get_stats_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("iobuf_get_stats", rpc_iobuf_get_stats, SPDK_RPC_RUNTIME)
