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

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);

}

static void
rpc_bdev_uring_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_uring req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_uring_decoders,
				    SPDK_COUNTOF(rpc_delete_uring_decoders),
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

	delete_uring_bdev(bdev, _rpc_bdev_uring_delete_cb, request);

cleanup:
	free_rpc_delete_uring(&req);
}
SPDK_RPC_REGISTER("bdev_uring_delete", rpc_bdev_uring_delete, SPDK_RPC_RUNTIME)
