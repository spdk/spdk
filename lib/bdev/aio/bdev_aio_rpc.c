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
#include "spdk_internal/log.h"

struct rpc_construct_aio {
	char *name;
	char *filename;
	uint32_t block_size;
};

static void
free_rpc_construct_aio(struct rpc_construct_aio *req)
{
	free(req->name);
	free(req->filename);
}

static const struct spdk_json_object_decoder rpc_construct_aio_decoders[] = {
	{"name", offsetof(struct rpc_construct_aio, name), spdk_json_decode_string},
	{"filename", offsetof(struct rpc_construct_aio, filename), spdk_json_decode_string, true},
	{"block_size", offsetof(struct rpc_construct_aio, block_size), spdk_json_decode_uint32, true},
};

static void
spdk_rpc_construct_aio_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_construct_aio req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_construct_aio_decoders,
				    SPDK_COUNTOF(rpc_construct_aio_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.filename == NULL) {
		goto invalid;
	}

	bdev = create_aio_disk(req.name, req.filename, req.block_size);
	if (bdev == NULL) {
		goto invalid;
	}

	free_rpc_construct_aio(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_aio(&req);
}
SPDK_RPC_REGISTER("construct_aio_bdev", spdk_rpc_construct_aio_bdev, SPDK_RPC_RUNTIME)

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
_spdk_rpc_delete_aio_bdev_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_delete_aio_bdev(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_delete_aio req = {NULL};
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_aio_decoders,
				    SPDK_COUNTOF(rpc_delete_aio_decoders),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	delete_aio_disk(bdev, _spdk_rpc_delete_aio_bdev_cb, request);

	free_rpc_delete_aio(&req);

	return;

invalid:
	free_rpc_delete_aio(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("delete_aio_bdev", spdk_rpc_delete_aio_bdev, SPDK_RPC_RUNTIME)
