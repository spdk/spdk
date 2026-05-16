/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) croit GmbH.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 */

#include "bdev_daos.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

static const struct spdk_json_object_decoder rpc_bdev_daos_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_daos_create_ctx, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_bdev_daos_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"pool", offsetof(struct rpc_bdev_daos_create_ctx, pool), spdk_json_decode_string},
	{"cont", offsetof(struct rpc_bdev_daos_create_ctx, cont), spdk_json_decode_string},
	{"oclass", offsetof(struct rpc_bdev_daos_create_ctx, oclass), spdk_json_decode_string, true},
	{"num_blocks", offsetof(struct rpc_bdev_daos_create_ctx, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_bdev_daos_create_ctx, block_size), spdk_json_decode_uint32},
};

static void
rpc_bdev_daos_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_daos_create_ctx req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_daos_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_daos_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_daos, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = create_bdev_daos(&bdev, req.name, &req.uuid, req.pool, req.cont, req.oclass,
			      req.num_blocks, req.block_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	free_rpc_bdev_daos_create(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

cleanup:
	free_rpc_bdev_daos_create(&req);
}
SPDK_RPC_REGISTER("bdev_daos_create", rpc_bdev_daos_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_daos_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_daos_delete_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_daos_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_daos_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_daos_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_daos_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_daos_delete_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_daos, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_bdev_daos(req.name, rpc_bdev_daos_delete_cb, request);

cleanup:
	free_rpc_bdev_daos_delete(&req);
}

SPDK_RPC_REGISTER("bdev_daos_delete", rpc_bdev_daos_delete, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_daos_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_daos_resize_ctx, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_daos_resize_ctx, new_size), spdk_json_decode_uint64}
};

static void
rpc_bdev_daos_resize(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_daos_resize_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_daos_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_daos_resize_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_daos_resize(req.name, req.new_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_daos_resize(&req);
}
SPDK_RPC_REGISTER("bdev_daos_resize", rpc_bdev_daos_resize, SPDK_RPC_RUNTIME)
