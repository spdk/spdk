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

struct rpc_construct_vbdev {
	char *name;
	char *bdev_name;
	uint64_t zone_size;
	uint64_t optimal_open_zones;
};

struct rpc_construct_block_vbdev {
	struct rpc_construct_vbdev req;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_construct_vbdev(struct rpc_construct_vbdev *req)
{
	free(req->name);
	free(req->bdev_name);
}

static const struct spdk_json_object_decoder rpc_construct_vbdev_decoders[] = {
	{"name", offsetof(struct rpc_construct_vbdev, name), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_construct_vbdev, bdev_name), spdk_json_decode_string},
	{"zone_size", offsetof(struct rpc_construct_vbdev, zone_size), spdk_json_decode_uint64},
	{"optimal_open_zones", offsetof(struct rpc_construct_vbdev, optimal_open_zones), spdk_json_decode_uint64, true},
};

static void
rpc_vbdev_block_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_construct_vbdev req = {NULL};
	struct spdk_json_write_ctx *w;
	int rc;

	req.optimal_open_zones = DEFAULT_OPTIMAL_ZONES;

	if (spdk_json_decode_object(params, rpc_construct_vbdev_decoders,
				    SPDK_COUNTOF(rpc_construct_vbdev_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	rc = spdk_vbdev_zone_block_create(req.bdev_name, req.name, req.zone_size, req.optimal_open_zones);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create block zoned vbdev: %s",
						     spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_construct_vbdev(&req);
}
SPDK_RPC_REGISTER("bdev_zone_block_create", rpc_vbdev_block_create, SPDK_RPC_RUNTIME)

struct rpc_delete_vbdev {
	char *name;
};

static void
free_rpc_delete_vbdev(struct rpc_delete_vbdev *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_vbdev_decoders[] = {
	{"name", offsetof(struct rpc_delete_vbdev, name), spdk_json_decode_string},
};

static void
_rpc_vbdev_delete_block_cb(void *cb_ctx, int rc)
{
	struct spdk_jsonrpc_request *request = cb_ctx;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, rc == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_vbdev_block_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_delete_vbdev attrs = {};

	if (spdk_json_decode_object(params, rpc_delete_vbdev_decoders,
				    SPDK_COUNTOF(rpc_delete_vbdev_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	spdk_vbdev_zone_block_delete(attrs.name, _rpc_vbdev_delete_block_cb, request);

cleanup:
	free_rpc_delete_vbdev(&attrs);
}
SPDK_RPC_REGISTER("bdev_zone_block_delete", rpc_vbdev_block_delete, SPDK_RPC_RUNTIME)
