/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "fsdev_aio.h"
#include "spdk_internal/rpc_autogen.h"

static const struct spdk_json_object_decoder rpc_fsdev_aio_create_decoders[] = {
	{"name", offsetof(struct rpc_fsdev_aio_create_ctx, name), spdk_json_decode_string},
	{"root_path", offsetof(struct rpc_fsdev_aio_create_ctx, root_path), spdk_json_decode_string},
	{"enable_xattr", offsetof(struct rpc_fsdev_aio_create_ctx, enable_xattr), spdk_json_decode_bool, true},
	{"enable_writeback_cache", offsetof(struct rpc_fsdev_aio_create_ctx, enable_writeback_cache), spdk_json_decode_bool, true},
	{"max_write", offsetof(struct rpc_fsdev_aio_create_ctx, max_write), spdk_json_decode_uint32, true},
	{"skip_rw", offsetof(struct rpc_fsdev_aio_create_ctx, skip_rw), spdk_json_decode_bool, true},
};

static void
rpc_fsdev_aio_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsdev_aio_create_ctx req = {};
	struct spdk_fsdev_aio_opts opts = {};
	struct spdk_json_write_ctx *w;
	struct spdk_fsdev *fsdev;
	int rc;

	/* Load defaults, then copy to req so JSON decode overrides only specified fields */
	spdk_fsdev_aio_get_default_opts(&opts);
	req.enable_xattr = opts.xattr_enabled;
	req.enable_writeback_cache = opts.writeback_cache_enabled;
	req.max_write = opts.max_write;
	req.skip_rw = opts.skip_rw;

	if (spdk_json_decode_object(params, rpc_fsdev_aio_create_decoders,
				    SPDK_COUNTOF(rpc_fsdev_aio_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		free_rpc_fsdev_aio_create(&req);
		return;
	}

	opts.xattr_enabled = req.enable_xattr;
	opts.writeback_cache_enabled = req.enable_writeback_cache;
	opts.max_write = req.max_write;
	opts.skip_rw = req.skip_rw;

	rc = spdk_fsdev_aio_create(&fsdev, req.name, req.root_path, &opts);
	if (rc) {
		SPDK_ERRLOG("Failed to create aio %s: rc %d\n", req.name, rc);
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		free_rpc_fsdev_aio_create(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, fsdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_fsdev_aio_create(&req);
}
SPDK_RPC_REGISTER("fsdev_aio_create", rpc_fsdev_aio_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_fsdev_aio_delete_decoders[] = {
	{"name", offsetof(struct rpc_fsdev_aio_delete_ctx, name), spdk_json_decode_string},
};

static void
rpc_aio_delete_cb(void *cb_arg, int fsdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (fsdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, fsdeverrno, spdk_strerror(-fsdeverrno));
	}
}

static void
rpc_fsdev_aio_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_fsdev_aio_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_fsdev_aio_delete_decoders,
				    SPDK_COUNTOF(rpc_fsdev_aio_delete_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		free_rpc_fsdev_aio_delete(&req);
		return;
	}

	spdk_fsdev_aio_delete(req.name, rpc_aio_delete_cb, request);
	free_rpc_fsdev_aio_delete(&req);
}
SPDK_RPC_REGISTER("fsdev_aio_delete", rpc_fsdev_aio_delete, SPDK_RPC_RUNTIME)
