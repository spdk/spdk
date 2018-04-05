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

#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include "bdev_null.h"

struct rpc_construct_null {
	char *name;
	char *uuid;
	uint64_t num_blocks;
	uint32_t block_size;
};

static void
free_rpc_construct_null(struct rpc_construct_null *req)
{
	free(req->name);
	free(req->uuid);
}

static const struct spdk_json_object_decoder rpc_construct_null_decoders[] = {
	{"name", offsetof(struct rpc_construct_null, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_construct_null, uuid), spdk_json_decode_string, true},
	{"num_blocks", offsetof(struct rpc_construct_null, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_construct_null, block_size), spdk_json_decode_uint32},
};

static void
spdk_rpc_construct_null_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_null req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_construct_null_decoders,
				    SPDK_COUNTOF(rpc_construct_null_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NULL, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.uuid) {
		if (spdk_uuid_parse(&decoded_uuid, req.uuid)) {
			goto invalid;
		}
		uuid = &decoded_uuid;
	}

	bdev = create_null_bdev(req.name, uuid, req.num_blocks, req.block_size);
	if (bdev == NULL) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_construct_null(&req);
		return;
	}

	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, bdev->name);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_construct_null(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_null(&req);
}
SPDK_RPC_REGISTER("construct_null_bdev", spdk_rpc_construct_null_bdev)
