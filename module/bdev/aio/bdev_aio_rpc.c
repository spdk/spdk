/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_aio.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

struct rpc_construct_aio_ctx {
	struct rpc_bdev_aio_create_ctx req;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_bdev_aio_create_ctx(struct rpc_construct_aio_ctx *ctx)
{
	free_rpc_bdev_aio_create(&ctx->req);
	free(ctx);
}

static const struct spdk_json_object_decoder rpc_bdev_aio_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_aio_create_ctx, name), spdk_json_decode_string},
	{"filename", offsetof(struct rpc_bdev_aio_create_ctx, filename), spdk_json_decode_string},
	{"block_size", offsetof(struct rpc_bdev_aio_create_ctx, block_size), spdk_json_decode_uint32, true},
	{"readonly", offsetof(struct rpc_bdev_aio_create_ctx, readonly), spdk_json_decode_bool, true},
	{"fallocate", offsetof(struct rpc_bdev_aio_create_ctx, fallocate), spdk_json_decode_bool, true},
	{"uuid", offsetof(struct rpc_bdev_aio_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"nowait", offsetof(struct rpc_bdev_aio_create_ctx, nowait), spdk_json_decode_bool, true},
};

static void
rpc_bdev_aio_create_cb(void *cb_arg)
{
	struct rpc_construct_aio_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, ctx->req.name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_bdev_aio_create_ctx(ctx);
}

static void
rpc_bdev_aio_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_construct_aio_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_aio_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_aio_create_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_bdev_aio_create_ctx(ctx);
		return;
	}

	ctx->request = request;
	rc = create_aio_bdev(ctx->req.name, ctx->req.filename, ctx->req.block_size,
			     ctx->req.readonly, ctx->req.fallocate, &ctx->req.uuid, ctx->req.nowait);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_rpc_bdev_aio_create_ctx(ctx);
		return;
	}

	spdk_bdev_wait_for_examine(rpc_bdev_aio_create_cb, ctx);
}
SPDK_RPC_REGISTER("bdev_aio_create", rpc_bdev_aio_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_aio_rescan_decoders[] = {
	{"name", offsetof(struct rpc_bdev_aio_rescan_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_aio_rescan(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_aio_rescan_ctx req = {NULL};
	int bdeverrno;

	if (spdk_json_decode_object(params, rpc_bdev_aio_rescan_decoders,
				    SPDK_COUNTOF(rpc_bdev_aio_rescan_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdeverrno = bdev_aio_rescan(req.name);
	if (bdeverrno) {
		spdk_jsonrpc_send_error_response(request, bdeverrno,
						 spdk_strerror(-bdeverrno));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_aio_rescan(&req);
}
SPDK_RPC_REGISTER("bdev_aio_rescan", rpc_bdev_aio_rescan, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_aio_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_aio_delete_ctx, name), spdk_json_decode_string},
};

static void
_rpc_bdev_aio_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_aio_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_aio_delete_ctx req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_aio_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_aio_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_aio_delete(req.name, _rpc_bdev_aio_delete_cb, request);

cleanup:
	free_rpc_bdev_aio_delete(&req);
}
SPDK_RPC_REGISTER("bdev_aio_delete", rpc_bdev_aio_delete, SPDK_RPC_RUNTIME)
