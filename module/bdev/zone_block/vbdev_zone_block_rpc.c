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

#include "spdk/stdinc.h"

#include "vbdev_zone_block.h"

#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/rpc.h"

#include "spdk/log.h"

struct rpc_construct_zone_block {
	char *name;
	char *base_bdev;
	uint64_t zone_capacity;
	uint64_t optimal_open_zones;
};

static void
free_rpc_construct_zone_block(struct rpc_construct_zone_block *req)
{
	free(req->name);
	free(req->base_bdev);
}

static const struct spdk_json_object_decoder rpc_construct_zone_block_decoders[] = {
	{"name", offsetof(struct rpc_construct_zone_block, name), spdk_json_decode_string},
	{"base_bdev", offsetof(struct rpc_construct_zone_block, base_bdev), spdk_json_decode_string},
	{"zone_capacity", offsetof(struct rpc_construct_zone_block, zone_capacity), spdk_json_decode_uint64},
	{"optimal_open_zones", offsetof(struct rpc_construct_zone_block, optimal_open_zones), spdk_json_decode_uint64},
};

static void
rpc_zone_block_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_construct_zone_block req = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_zone_block_decoders,
				    SPDK_COUNTOF(rpc_construct_zone_block_decoders),
				    &req)) {
		SPDK_ERRLOG("Failed to decode block create parameters");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	rc = vbdev_zone_block_create(req.base_bdev, req.name, req.zone_capacity,
				     req.optimal_open_zones);
	if (rc) {
		SPDK_ERRLOG("Failed to create block zoned vbdev: %s", spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create block zoned vbdev: %s",
						     spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_construct_zone_block(&req);
}
SPDK_RPC_REGISTER("bdev_zone_block_create", rpc_zone_block_create, SPDK_RPC_RUNTIME)

struct rpc_delete_zone_block {
	char *name;
};

static void
free_rpc_delete_zone_block(struct rpc_delete_zone_block *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_zone_block_decoders[] = {
	{"name", offsetof(struct rpc_delete_zone_block, name), spdk_json_decode_string},
};

static void
_rpc_delete_zone_block_cb(void *cb_ctx, int rc)
{
	struct spdk_jsonrpc_request *request = cb_ctx;

	spdk_jsonrpc_send_bool_response(request, rc == 0);
}

static void
rpc_zone_block_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_zone_block attrs = {};

	if (spdk_json_decode_object(params, rpc_delete_zone_block_decoders,
				    SPDK_COUNTOF(rpc_delete_zone_block_decoders),
				    &attrs)) {
		SPDK_ERRLOG("Failed to decode block delete parameters");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	vbdev_zone_block_delete(attrs.name, _rpc_delete_zone_block_cb, request);

cleanup:
	free_rpc_delete_zone_block(&attrs);
}
SPDK_RPC_REGISTER("bdev_zone_block_delete", rpc_zone_block_delete, SPDK_RPC_RUNTIME)
