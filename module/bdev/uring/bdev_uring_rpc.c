/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_uring.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* Structure to hold the parameters for this RPC method. */
struct rpc_create_uring {
	char *name;
	char *filename;
	uint32_t block_size;
};

/* Free the allocated memory resource after the RPC handling. */
static void
free_rpc_create_uring(struct rpc_create_uring *r)
{
	free(r->name);
	free(r->filename);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_create_uring_decoders[] = {
	{"name", offsetof(struct rpc_create_uring, name), spdk_json_decode_string},
	{"filename", offsetof(struct rpc_create_uring, filename), spdk_json_decode_string},
	{"block_size", offsetof(struct rpc_create_uring, block_size), spdk_json_decode_uint32, true},
};

/* Decode the parameters for this RPC method and properly create the uring
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_uring_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_create_uring req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_create_uring_decoders,
				    SPDK_COUNTOF(rpc_create_uring_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = create_uring_bdev(req.name, req.filename, req.block_size);
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
	free_rpc_create_uring(&req);
}
SPDK_RPC_REGISTER("bdev_uring_create", rpc_bdev_uring_create, SPDK_RPC_RUNTIME)

struct rpc_delete_uring {
	char *name;
};

static void
free_rpc_delete_uring(struct rpc_delete_uring *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_uring_decoders[] = {
	{"name", offsetof(struct rpc_delete_uring, name), spdk_json_decode_string},
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
	struct rpc_delete_uring req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_uring_decoders,
				    SPDK_COUNTOF(rpc_delete_uring_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_uring_bdev(req.name, _rpc_bdev_uring_delete_cb, request);

cleanup:
	free_rpc_delete_uring(&req);
}
SPDK_RPC_REGISTER("bdev_uring_delete", rpc_bdev_uring_delete, SPDK_RPC_RUNTIME)
