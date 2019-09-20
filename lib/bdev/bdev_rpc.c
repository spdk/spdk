/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/bdev.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

struct spdk_rpc_set_bdev_opts {
	uint32_t bdev_io_pool_size;
	uint32_t bdev_io_cache_size;
};

static const struct spdk_json_object_decoder rpc_set_bdev_opts_decoders[] = {
	{"bdev_io_pool_size", offsetof(struct spdk_rpc_set_bdev_opts, bdev_io_pool_size), spdk_json_decode_uint32, true},
	{"bdev_io_cache_size", offsetof(struct spdk_rpc_set_bdev_opts, bdev_io_cache_size), spdk_json_decode_uint32, true},
};

static void
spdk_rpc_bdev_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_rpc_set_bdev_opts rpc_opts;
	struct spdk_bdev_opts bdev_opts;
	struct spdk_json_write_ctx *w;
	int rc;

	rpc_opts.bdev_io_pool_size = UINT32_MAX;
	rpc_opts.bdev_io_cache_size = UINT32_MAX;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_set_bdev_opts_decoders,
					    SPDK_COUNTOF(rpc_set_bdev_opts_decoders), &rpc_opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	spdk_bdev_get_opts(&bdev_opts);
	if (rpc_opts.bdev_io_pool_size != UINT32_MAX) {
		bdev_opts.bdev_io_pool_size = rpc_opts.bdev_io_pool_size;
	}
	if (rpc_opts.bdev_io_cache_size != UINT32_MAX) {
		bdev_opts.bdev_io_cache_size = rpc_opts.bdev_io_cache_size;
	}
	rc = spdk_bdev_set_opts(&bdev_opts);

	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Pool size %" PRIu32 " too small for cache size %" PRIu32,
						     bdev_opts.bdev_io_pool_size, bdev_opts.bdev_io_cache_size);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_set_options", spdk_rpc_bdev_set_options, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_set_options, set_bdev_options)
