/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_malloc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

static const struct spdk_json_object_decoder rpc_bdev_malloc_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_malloc_create_ctx, name), spdk_json_decode_string, true},
	{"uuid", offsetof(struct rpc_bdev_malloc_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"num_blocks", offsetof(struct rpc_bdev_malloc_create_ctx, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_bdev_malloc_create_ctx, block_size), spdk_json_decode_uint32},
	{"physical_block_size", offsetof(struct rpc_bdev_malloc_create_ctx, physical_block_size), spdk_json_decode_uint32, true},
	{"optimal_io_boundary", offsetof(struct rpc_bdev_malloc_create_ctx, optimal_io_boundary), spdk_json_decode_uint32, true},
	{"md_size", offsetof(struct rpc_bdev_malloc_create_ctx, md_size), spdk_json_decode_uint32, true},
	{"md_interleave", offsetof(struct rpc_bdev_malloc_create_ctx, md_interleave), spdk_json_decode_bool, true},
	{"dif_type", offsetof(struct rpc_bdev_malloc_create_ctx, dif_type), spdk_json_decode_int32, true},
	{"dif_is_head_of_md", offsetof(struct rpc_bdev_malloc_create_ctx, dif_is_head_of_md), spdk_json_decode_bool, true},
	{"dif_pi_format", offsetof(struct rpc_bdev_malloc_create_ctx, dif_pi_format), spdk_json_decode_uint32, true},
	{"numa_id", offsetof(struct rpc_bdev_malloc_create_ctx, numa_id), spdk_json_decode_int32, true},
};

static void
rpc_bdev_malloc_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_malloc_create_ctx req = {};
	struct malloc_bdev_opts opts = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;

	req.numa_id = SPDK_ENV_NUMA_ID_ANY;

	if (spdk_json_decode_object(params, rpc_bdev_malloc_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_malloc_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.name = req.name; /* strdup() already happens in create_malloc_disk() */
	spdk_uuid_copy(&opts.uuid, &req.uuid);
	opts.num_blocks = req.num_blocks;
	opts.block_size = req.block_size;
	opts.physical_block_size = req.physical_block_size;
	opts.optimal_io_boundary = req.optimal_io_boundary;
	opts.md_size = req.md_size;
	opts.md_interleave = req.md_interleave;
	opts.dif_type = req.dif_type;
	opts.dif_is_head_of_md = req.dif_is_head_of_md;
	opts.dif_pi_format = req.dif_pi_format;
	opts.numa_id = req.numa_id;

	rc = create_malloc_disk(&bdev, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	free_rpc_bdev_malloc_create(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

cleanup:
	free_rpc_bdev_malloc_create(&req);
}
SPDK_RPC_REGISTER("bdev_malloc_create", rpc_bdev_malloc_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_malloc_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_malloc_delete_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_malloc_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_malloc_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_malloc_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_malloc_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_malloc_delete_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_malloc_disk(req.name, rpc_bdev_malloc_delete_cb, request);

cleanup:
	free_rpc_bdev_malloc_delete(&req);
}
SPDK_RPC_REGISTER("bdev_malloc_delete", rpc_bdev_malloc_delete, SPDK_RPC_RUNTIME)
