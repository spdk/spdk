/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_ubi.h"

struct rpc_construct_ubi {
	char *name;
	char *uuid;
	uint64_t num_blocks;
	uint32_t block_size;
	uint32_t physical_block_size;
	uint32_t md_size;
	int32_t dif_type;
	bool dif_is_head_of_md;
};

static void
free_rpc_construct_ubi(struct rpc_construct_ubi *req)
{
	free(req->name);
	free(req->uuid);
}

static const struct spdk_json_object_decoder rpc_construct_ubi_decoders[] = {
	{"name", offsetof(struct rpc_construct_ubi, name), spdk_json_decode_string}
};

static void
rpc_bdev_ubi_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_construct_ubi req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;
	struct spdk_bdev *bdev;
	struct spdk_ubi_bdev_opts opts = {};
	uint32_t data_block_size;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_construct_ubi_decoders,
				    SPDK_COUNTOF(rpc_construct_ubi_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_ubi, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	req.block_size = 4096;
	req.md_size = 512;
	req.physical_block_size = 512;
	req.num_blocks = 1000;
	req.dif_type = SPDK_DIF_DISABLE;

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

	if (req.physical_block_size % 512 != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "Physical block size %u is not a multiple of 512", req.physical_block_size);
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
	opts.physical_block_size = req.physical_block_size;
	opts.md_size = req.md_size;
	opts.md_interleave = true;
	opts.dif_type = req.dif_type;
	opts.dif_is_head_of_md = req.dif_is_head_of_md;
	rc = bdev_ubi_create(&bdev, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, bdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_construct_ubi(&req);
	return;

cleanup:
	free_rpc_construct_ubi(&req);
}
SPDK_RPC_REGISTER("bdev_ubi_create", rpc_bdev_ubi_create, SPDK_RPC_RUNTIME)

struct rpc_delete_ubi {
	char *name;
};

static void
free_rpc_delete_ubi(struct rpc_delete_ubi *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_ubi_decoders[] = {
	{"name", offsetof(struct rpc_delete_ubi, name), spdk_json_decode_string},
};

static void
rpc_bdev_ubi_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_ubi_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_delete_ubi req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_ubi_decoders,
				    SPDK_COUNTOF(rpc_delete_ubi_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_ubi_delete(req.name, rpc_bdev_ubi_delete_cb, request);

	free_rpc_delete_ubi(&req);

	return;

cleanup:
	free_rpc_delete_ubi(&req);
}
SPDK_RPC_REGISTER("bdev_ubi_delete", rpc_bdev_ubi_delete, SPDK_RPC_RUNTIME)

struct rpc_bdev_ubi_resize {
	char *name;
	uint64_t new_size;
};

static const struct spdk_json_object_decoder rpc_bdev_ubi_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ubi_resize, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_ubi_resize, new_size), spdk_json_decode_uint64}
};

static void
free_rpc_bdev_ubi_resize(struct rpc_bdev_ubi_resize *req)
{
	free(req->name);
}

static void
spdk_rpc_bdev_ubi_resize(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_ubi_resize req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_ubi_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_ubi_resize_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_ubi_resize(req.name, req.new_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_ubi_resize(&req);
}
SPDK_RPC_REGISTER("bdev_ubi_resize", spdk_rpc_bdev_ubi_resize, SPDK_RPC_RUNTIME)
