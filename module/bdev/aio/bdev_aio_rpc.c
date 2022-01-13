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

#include "bdev_aio.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_construct_aio {
	char *name;
	char *filename;
	uint32_t block_size;
};

struct rpc_construct_aio_ctx {
	struct rpc_construct_aio req;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_construct_aio(struct rpc_construct_aio_ctx *ctx)
{
	free(ctx->req.name);
	free(ctx->req.filename);
	free(ctx);
}

static const struct spdk_json_object_decoder rpc_construct_aio_decoders[] = {
	{"name", offsetof(struct rpc_construct_aio, name), spdk_json_decode_string},
	{"filename", offsetof(struct rpc_construct_aio, filename), spdk_json_decode_string},
	{"block_size", offsetof(struct rpc_construct_aio, block_size), spdk_json_decode_uint32, true},
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
	free_rpc_construct_aio(ctx);
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

	if (spdk_json_decode_object(params, rpc_construct_aio_decoders,
				    SPDK_COUNTOF(rpc_construct_aio_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_construct_aio(ctx);
		return;
	}

	ctx->request = request;
	rc = create_aio_bdev(ctx->req.name, ctx->req.filename, ctx->req.block_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_rpc_construct_aio(ctx);
		return;
	}

	spdk_bdev_wait_for_examine(rpc_bdev_aio_create_cb, ctx);
}
SPDK_RPC_REGISTER("bdev_aio_create", rpc_bdev_aio_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_aio_create, construct_aio_bdev)

struct rpc_rescan_aio {
	char *name;
};

static const struct spdk_json_object_decoder rpc_rescan_aio_decoders[] = {
	{"name", offsetof(struct rpc_rescan_aio, name), spdk_json_decode_string},
};

static void
rpc_bdev_aio_rescan(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_rescan_aio req = {NULL};
	struct spdk_bdev *bdev;
	int bdeverrno;

	if (spdk_json_decode_object(params, rpc_rescan_aio_decoders,
				    SPDK_COUNTOF(rpc_rescan_aio_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	bdeverrno = bdev_aio_rescan(bdev);
	spdk_jsonrpc_send_bool_response(request, bdeverrno);

cleanup:
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_aio_rescan", rpc_bdev_aio_rescan, SPDK_RPC_RUNTIME)

struct rpc_delete_aio {
	char *name;
};

static void
free_rpc_delete_aio(struct rpc_delete_aio *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_aio_decoders[] = {
	{"name", offsetof(struct rpc_delete_aio, name), spdk_json_decode_string},
};

static void
_rpc_bdev_aio_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_aio_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_delete_aio req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_aio_decoders,
				    SPDK_COUNTOF(rpc_delete_aio_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	bdev_aio_delete(bdev, _rpc_bdev_aio_delete_cb, request);

cleanup:
	free_rpc_delete_aio(&req);
}
SPDK_RPC_REGISTER("bdev_aio_delete", rpc_bdev_aio_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_aio_delete, delete_aio_bdev)
