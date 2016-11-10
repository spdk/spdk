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

#include "blockdev_rbd.h"
#include "bdev_rpc.h"
#include "spdk/log.h"
#include "spdk/rpc.h"

struct rpc_construct_rbd {
	char *pool_name;
	char *rbd_name;
	uint32_t block_size;
};

static void
free_rpc_construct_rbd(struct rpc_construct_rbd *req)
{
	free(req->pool_name);
	free(req->rbd_name);
}

static const struct spdk_json_object_decoder rpc_construct_rbd_decoders[] = {
	{"pool_name", offsetof(struct rpc_construct_rbd, pool_name), spdk_json_decode_string},
	{"rbd_name", offsetof(struct rpc_construct_rbd, rbd_name), spdk_json_decode_string},
	{"block_size", offsetof(struct rpc_construct_rbd, block_size), spdk_json_decode_uint32},
};

static void
spdk_rpc_construct_rbd_bdev(struct spdk_jsonrpc_server_conn *conn,
			    const struct spdk_json_val *params,
			    const struct spdk_json_val *id)
{
	struct rpc_construct_rbd req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_construct_rbd_decoders,
				    sizeof(rpc_construct_rbd_decoders) / sizeof(*rpc_construct_rbd_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	bdev = spdk_bdev_rbd_create(req.pool_name, req.rbd_name, req.block_size, NULL);
	if (bdev == NULL) {
		goto invalid;
	}

	free_rpc_construct_rbd(&req);

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
	free_rpc_construct_rbd(&req);
}

struct rpc_construct_rbd_in_target {
	char *target_name;
	char *pool_name;
	char *rbd_name;
	int32_t size;
};

static void
free_rpc_construct_rbd_in_target(struct rpc_construct_rbd_in_target *req)
{
	free(req->target_name);
	free(req->pool_name);
	free(req->rbd_name);
}

static const struct spdk_json_object_decoder rpc_construct_rbd_in_target_decoders[] = {
	{"target_name", offsetof(struct rpc_construct_rbd_in_target, target_name), spdk_json_decode_string},
	{"pool_name", offsetof(struct rpc_construct_rbd_in_target, pool_name), spdk_json_decode_string},
	{"rbd_name", offsetof(struct rpc_construct_rbd_in_target, rbd_name), spdk_json_decode_string},
	{"size", offsetof(struct rpc_construct_rbd_in_target, size), spdk_json_decode_int32},
};

static void
spdk_rpc_construct_rbd_bdev_in_target(struct spdk_jsonrpc_server_conn *conn,
			    const struct spdk_json_val *params,
			    const struct spdk_json_val *id)
{
	struct rpc_construct_rbd_in_target req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;

	if (spdk_json_decode_object(params, rpc_construct_rbd_in_target_decoders,
				    sizeof(rpc_construct_rbd_in_target_decoders) / sizeof(*rpc_construct_rbd_in_target_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if ((bdev = (struct spdk_bdev *)spdk_bdev_rbd_create(req.pool_name, req.rbd_name, req.size, req.target_name)) == NULL) {
		goto invalid;
	}

	if (id == NULL) {
		free_rpc_construct_rbd_in_target(&req);
		return;
	}

	if (spdk_bdev_rpc_add(bdev, req.target_name)) {
		blockdev_rbd_free_disk(bdev);
		SPDK_ERRLOG("spdk_bdev_rpc_add failed\n");
		goto invalid;
	}

	free_rpc_construct_rbd_in_target(&req);
	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, bdev->name);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_rbd_in_target(&req);
}
SPDK_RPC_REGISTER("construct_rbd_bdev", spdk_rpc_construct_rbd_bdev)
SPDK_RPC_REGISTER("construct_rbd_bdev_in_target", spdk_rpc_construct_rbd_bdev_in_target)
