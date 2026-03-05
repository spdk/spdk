/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_uring.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_bdev_uring_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_uring_create_ctx, name), spdk_json_decode_string},
	{"filename", offsetof(struct rpc_bdev_uring_create_ctx, filename), spdk_json_decode_string},
	{"block_size", offsetof(struct rpc_bdev_uring_create_ctx, block_size), spdk_json_decode_uint32, true},
	{"uuid", offsetof(struct rpc_bdev_uring_create_ctx, uuid), spdk_json_decode_uuid, true},
};

/* Decode the parameters for this RPC method and properly create the uring
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_uring_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_uring_create_ctx req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	struct bdev_uring_opts opts = {};

	if (spdk_json_decode_object(params, rpc_bdev_uring_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_uring_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.block_size = req.block_size;
	opts.filename = req.filename;
	opts.name = req.name;
	opts.uuid = req.uuid;

	bdev = create_uring_bdev(&opts);
	if (!bdev) {
		SPDK_ERRLOG("Unable to create URING bdev from file %s\n", req.filename);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create URING bdev.");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_uring_create(&req);
}
SPDK_RPC_REGISTER("bdev_uring_create", rpc_bdev_uring_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_uring_rescan_decoders[] = {
	{"name", offsetof(struct rpc_bdev_uring_rescan_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_uring_rescan(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_uring_rescan_ctx req = {};
	int bdeverrno;

	if (spdk_json_decode_object(params, rpc_bdev_uring_rescan_decoders,
				    SPDK_COUNTOF(rpc_bdev_uring_rescan_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdeverrno = bdev_uring_rescan(req.name);
	if (bdeverrno) {
		spdk_jsonrpc_send_error_response(request, bdeverrno,
						 spdk_strerror(-bdeverrno));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_uring_rescan(&req);
}
SPDK_RPC_REGISTER("bdev_uring_rescan", rpc_bdev_uring_rescan, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_uring_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_uring_delete_ctx, name), spdk_json_decode_string},
};

static void
_rpc_bdev_uring_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}

}

static void
rpc_bdev_uring_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_uring_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_uring_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_uring_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_uring_bdev(req.name, _rpc_bdev_uring_delete_cb, request);

cleanup:
	free_rpc_bdev_uring_delete(&req);
}
SPDK_RPC_REGISTER("bdev_uring_delete", rpc_bdev_uring_delete, SPDK_RPC_RUNTIME)
