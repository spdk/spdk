/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_null.h"

struct rpc_construct_null {
	char *name;
	char *uuid;
	uint64_t num_blocks;
	uint32_t block_size;
	uint32_t md_size;
	int32_t dif_type;
	bool dif_is_head_of_md;
};

static void
free_rpc_construct_null(struct rpc_construct_null *req)
{
	free(req->name);
	free(req->uuid);
}

static const struct spdk_json_object_decoder rpc_construct_null_decoders[] = {
	{"name", offsetof(struct rpc_construct_null, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_construct_null, uuid), spdk_json_decode_string, true},
	{"num_blocks", offsetof(struct rpc_construct_null, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_construct_null, block_size), spdk_json_decode_uint32},
	{"md_size", offsetof(struct rpc_construct_null, md_size), spdk_json_decode_uint32, true},
	{"dif_type", offsetof(struct rpc_construct_null, dif_type), spdk_json_decode_int32, true},
	{"dif_is_head_of_md", offsetof(struct rpc_construct_null, dif_is_head_of_md), spdk_json_decode_bool, true},
};

static void
rpc_bdev_null_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_construct_null req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;
	struct spdk_bdev *bdev;
	struct spdk_null_bdev_opts opts = {};
	uint32_t data_block_size;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_construct_null_decoders,
				    SPDK_COUNTOF(rpc_construct_null_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_null, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.block_size < req.md_size) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "Interleaved metadata size can not be greater than block size");
		goto cleanup;
	}
	data_block_size = req.block_size - req.md_size;
	if (data_block_size % 512 != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "Data block size %u is not a multiple of 512", req.block_size);
		goto cleanup;
	}

	if (req.num_blocks == 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Disk num_blocks must be greater than 0");
		goto cleanup;
	}

	if (req.uuid) {
		if (spdk_uuid_parse(&decoded_uuid, req.uuid)) {
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "Failed to parse bdev UUID");
			goto cleanup;
		}
		uuid = &decoded_uuid;
	}

	if (req.dif_type < SPDK_DIF_DISABLE || req.dif_type > SPDK_DIF_TYPE3) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Invalid protection information type");
		goto cleanup;
	}

	if (req.dif_type != SPDK_DIF_DISABLE && !req.md_size) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Interleaved metadata size should be set for DIF");
		goto cleanup;
	}

	opts.name = req.name;
	opts.uuid = uuid;
	opts.num_blocks = req.num_blocks;
	opts.block_size = req.block_size;
	opts.md_size = req.md_size;
	opts.md_interleave = true;
	opts.dif_type = req.dif_type;
	opts.dif_is_head_of_md = req.dif_is_head_of_md;
	rc = bdev_null_create(&bdev, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, bdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_construct_null(&req);
	return;

cleanup:
	free_rpc_construct_null(&req);
}
SPDK_RPC_REGISTER("bdev_null_create", rpc_bdev_null_create, SPDK_RPC_RUNTIME)

struct rpc_delete_null {
	char *name;
};

static void
free_rpc_delete_null(struct rpc_delete_null *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_null_decoders[] = {
	{"name", offsetof(struct rpc_delete_null, name), spdk_json_decode_string},
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
	struct rpc_delete_null req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_null_decoders,
				    SPDK_COUNTOF(rpc_delete_null_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_null_delete(req.name, rpc_bdev_null_delete_cb, request);

	free_rpc_delete_null(&req);

	return;

cleanup:
	free_rpc_delete_null(&req);
}
SPDK_RPC_REGISTER("bdev_null_delete", rpc_bdev_null_delete, SPDK_RPC_RUNTIME)

struct rpc_bdev_null_resize {
	char *name;
	uint64_t new_size;
};

static const struct spdk_json_object_decoder rpc_bdev_null_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_null_resize, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_null_resize, new_size), spdk_json_decode_uint64}
};

static void
free_rpc_bdev_null_resize(struct rpc_bdev_null_resize *req)
{
	free(req->name);
}

static void
spdk_rpc_bdev_null_resize(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_null_resize req = {};
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
SPDK_RPC_REGISTER("bdev_null_resize", spdk_rpc_bdev_null_resize, SPDK_RPC_RUNTIME)
