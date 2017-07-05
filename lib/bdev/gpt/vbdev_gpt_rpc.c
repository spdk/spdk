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

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"
#include "gpt.h"

struct rpc_construct_gpt_bdev {
	char *base_name;
};

static void
free_rpc_check_gpt_bdev(struct rpc_construct_gpt_bdev *req)
{
	free(req->base_name);
}

static const struct spdk_json_object_decoder rpc_construct_gpt_bdev_decoders[] = {
	{"base_name", offsetof(struct rpc_construct_gpt_bdev, base_name), spdk_json_decode_string},
};

static void
spdk_rpc_check_gpt_bdev(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)

{
	struct rpc_construct_gpt_bdev req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *base_bdev;

	if (spdk_json_decode_object(params, rpc_construct_gpt_bdev_decoders,
				    SPDK_COUNTOF(rpc_construct_gpt_bdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	base_bdev = spdk_bdev_get_by_name(req.base_name);
	if (!base_bdev) {
		SPDK_ERRLOG("Could not find bdev %s\n", req.base_name);
		goto invalid;
	}

	if (spdk_vbdev_gpt_read_gpt(base_bdev)) {
		SPDK_ERRLOG("Could not read gpt partition of bdev %s\n", req.base_name);
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_check_gpt_bdev(&req);
		return;
	}
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_check_gpt_bdev(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_check_gpt_bdev(&req);
}
SPDK_RPC_REGISTER("check_gpt_bdev", spdk_rpc_check_gpt_bdev)
