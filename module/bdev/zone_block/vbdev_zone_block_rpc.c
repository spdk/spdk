/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
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

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
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
