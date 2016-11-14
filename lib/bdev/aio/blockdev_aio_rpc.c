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

#include "blockdev_aio.h"
#include "bdev_rpc.h"
#include "spdk/log.h"
#include "spdk/rpc.h"

struct rpc_construct_aio {
	char *fname;
};

static void
free_rpc_construct_aio(struct rpc_construct_aio *req)
{
	free(req->fname);
}

static const struct spdk_json_object_decoder rpc_construct_aio_decoders[] = {
	{"fname", offsetof(struct rpc_construct_aio, fname), spdk_json_decode_string},
};

static void
spdk_rpc_construct_aio_bdev(struct spdk_jsonrpc_server_conn *conn,
			    const struct spdk_json_val *params,
			    const struct spdk_json_val *id)
{
	struct rpc_construct_aio req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_construct_aio_decoders,
				    sizeof(rpc_construct_aio_decoders) / sizeof(*rpc_construct_aio_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	bdev = create_aio_disk(req.fname, NULL);
	if (bdev == NULL) {
		goto invalid;
	}

	free_rpc_construct_aio(&req);

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, bdev->name);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_aio(&req);
}

struct rpc_construct_aio_in_target {
	char *target_name;
	char *fname;
};

static void
free_rpc_construct_aio_in_target(struct rpc_construct_aio_in_target *req)
{
	free(req->target_name);
	free(req->fname);
}

static const struct spdk_json_object_decoder rpc_construct_aio_in_target_decoders[] = {
	{"target_name", offsetof(struct rpc_construct_aio_in_target, target_name), spdk_json_decode_string},
	{"fname", offsetof(struct rpc_construct_aio_in_target, fname), spdk_json_decode_string},
};

static void
spdk_rpc_construct_aio_bdev_in_target(struct spdk_jsonrpc_server_conn *conn,
			    const struct spdk_json_val *params,
			    const struct spdk_json_val *id)
{
	struct rpc_construct_aio_in_target req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;

	if (spdk_json_decode_object(params, rpc_construct_aio_in_target_decoders,
				    sizeof(rpc_construct_aio_in_target_decoders) / sizeof(*rpc_construct_aio_in_target_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if ((bdev = (struct spdk_bdev *)create_aio_disk(req.fname, req.target_name)) == NULL) {
		goto invalid;
	}

	if (id == NULL) {
		free_rpc_construct_aio_in_target(&req);
		return;
	}

	if (spdk_bdev_rpc_add(bdev, req.target_name)) {
		SPDK_ERRLOG("spdk_bdev_rpc_add failed\n");
		blockdev_aio_free_disk(bdev);
		goto invalid;
	}

	free_rpc_construct_aio_in_target(&req);
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, bdev->name);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_aio_in_target(&req);
}
SPDK_RPC_REGISTER("construct_aio_bdev", spdk_rpc_construct_aio_bdev)
SPDK_RPC_REGISTER("construct_aio_bdev_in_target", spdk_rpc_construct_aio_bdev_in_target)
