/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/uuid.h"
#include "spdk_internal/rpc_autogen.h"

#include "bdev_null.h"


static const struct spdk_json_object_decoder rpc_bdev_null_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_null_create_ctx, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_bdev_null_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"num_blocks", offsetof(struct rpc_bdev_null_create_ctx, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_bdev_null_create_ctx, block_size), spdk_json_decode_uint32},
	{"physical_block_size", offsetof(struct rpc_bdev_null_create_ctx, physical_block_size), spdk_json_decode_uint32, true},
	{"md_size", offsetof(struct rpc_bdev_null_create_ctx, md_size), spdk_json_decode_uint32, true},
	{"dif_type", offsetof(struct rpc_bdev_null_create_ctx, dif_type), spdk_json_decode_int32, true},
	{"dif_is_head_of_md", offsetof(struct rpc_bdev_null_create_ctx, dif_is_head_of_md), spdk_json_decode_bool, true},
	{"dif_pi_format", offsetof(struct rpc_bdev_null_create_ctx, dif_pi_format), spdk_json_decode_uint32, true},
	{"preferred_write_alignment", offsetof(struct rpc_bdev_null_create_ctx, preferred_write_alignment), spdk_json_decode_uint32, true},
	{"preferred_write_granularity", offsetof(struct rpc_bdev_null_create_ctx, preferred_write_granularity), spdk_json_decode_uint32, true},
	{"optimal_write_size", offsetof(struct rpc_bdev_null_create_ctx, optimal_write_size), spdk_json_decode_uint32, true},
	{"preferred_unmap_alignment", offsetof(struct rpc_bdev_null_create_ctx, preferred_unmap_alignment), spdk_json_decode_uint32, true},
	{"preferred_unmap_granularity", offsetof(struct rpc_bdev_null_create_ctx, preferred_unmap_granularity), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_null_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_null_create_ctx req = {};
	struct null_bdev_opts opts = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_null_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_null_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_null, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.name = req.name; /* strdup() already happens in bdev_null_create() */
	spdk_uuid_copy(&opts.uuid, &req.uuid);
	opts.num_blocks = req.num_blocks;
	opts.block_size = req.block_size;
	opts.physical_block_size = req.physical_block_size;
	opts.md_size = req.md_size;
	opts.dif_type = req.dif_type;
	opts.dif_is_head_of_md = req.dif_is_head_of_md;
	opts.dif_pi_format = req.dif_pi_format;
	opts.preferred_write_alignment = req.preferred_write_alignment;
	opts.preferred_write_granularity = req.preferred_write_granularity;
	opts.optimal_write_size = req.optimal_write_size;
	opts.preferred_unmap_alignment = req.preferred_unmap_alignment;
	opts.preferred_unmap_granularity = req.preferred_unmap_granularity;

	rc = bdev_null_create(&bdev, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, bdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_bdev_null_create(&req);
	return;

cleanup:
	free_rpc_bdev_null_create(&req);
}
SPDK_RPC_REGISTER("bdev_null_create", rpc_bdev_null_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_null_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_null_delete_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_null_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_null_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_null_delete_ctx req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_null_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_null_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_null_delete(req.name, rpc_bdev_null_delete_cb, request);

	free_rpc_bdev_null_delete(&req);

	return;

cleanup:
	free_rpc_bdev_null_delete(&req);
}
SPDK_RPC_REGISTER("bdev_null_delete", rpc_bdev_null_delete, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_null_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_null_resize_ctx, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_null_resize_ctx, new_size), spdk_json_decode_uint64}
};

static void
rpc_bdev_null_resize(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_null_resize_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_null_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_null_resize_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_null_resize(req.name, req.new_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_null_resize(&req);
}
SPDK_RPC_REGISTER("bdev_null_resize", rpc_bdev_null_resize, SPDK_RPC_RUNTIME)
