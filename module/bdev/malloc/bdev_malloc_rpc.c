/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_malloc.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_construct_malloc {
	char *name;
	char *uuid;
	uint64_t num_blocks;
	uint32_t block_size;
	uint32_t optimal_io_boundary;
};

static void
free_rpc_construct_malloc(struct rpc_construct_malloc *r)
{
	free(r->name);
	free(r->uuid);
}

static const struct spdk_json_object_decoder rpc_construct_malloc_decoders[] = {
	{"name", offsetof(struct rpc_construct_malloc, name), spdk_json_decode_string, true},
	{"uuid", offsetof(struct rpc_construct_malloc, uuid), spdk_json_decode_string, true},
	{"num_blocks", offsetof(struct rpc_construct_malloc, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_construct_malloc, block_size), spdk_json_decode_uint32},
	{"optimal_io_boundary", offsetof(struct rpc_construct_malloc, optimal_io_boundary), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_malloc_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_construct_malloc req = {NULL};
	struct spdk_json_write_ctx *w;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;
	struct spdk_bdev *bdev;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_construct_malloc_decoders,
				    SPDK_COUNTOF(rpc_construct_malloc_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
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

	rc = create_malloc_disk(&bdev, req.name, uuid, req.num_blocks, req.block_size,
				req.optimal_io_boundary);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	free_rpc_construct_malloc(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

cleanup:
	free_rpc_construct_malloc(&req);
}
SPDK_RPC_REGISTER("bdev_malloc_create", rpc_bdev_malloc_create, SPDK_RPC_RUNTIME)

struct rpc_delete_malloc {
	char *name;
};

static void
free_rpc_delete_malloc(struct rpc_delete_malloc *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_malloc_decoders[] = {
	{"name", offsetof(struct rpc_delete_malloc, name), spdk_json_decode_string},
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
	struct rpc_delete_malloc req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_malloc_decoders,
				    SPDK_COUNTOF(rpc_delete_malloc_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_malloc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_malloc_disk(req.name, rpc_bdev_malloc_delete_cb, request);

cleanup:
	free_rpc_delete_malloc(&req);
}
SPDK_RPC_REGISTER("bdev_malloc_delete", rpc_bdev_malloc_delete, SPDK_RPC_RUNTIME)
