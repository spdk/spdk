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

#include "gpt.h"
#include "spdk/rpc.h"
#include "spdk_internal/log.h"
#include "spdk/bdev_module.h"

/* Structure to hold the parameters for this RPC method. */
struct rpc_gpt_release_bdev {
	char *name;
};

/* Free the allocated memory resource after the RPC handling. */
static void
free_rpc_gpt_release_bdev(struct rpc_gpt_release_bdev *r)
{
	free(r->name);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_gpt_release_bdev_decoders[] = {
	{"name", offsetof(struct rpc_gpt_release_bdev, name), spdk_json_decode_string},
};

/* Decode the parameters for this RPC method and properly construct the crypto
 * device. Error status returned in the failed cases.
 */
static void
spdk_rpc_gpt_release_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_gpt_release_bdev req = {NULL};
	struct spdk_bdev *bdev;
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_gpt_release_bdev_decoders,
				    SPDK_COUNTOF(rpc_gpt_release_bdev_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_GPT_PARSE, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (!bdev) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							"Invalid parameters");
		goto cleanup;
	}

	if (vbdev_gpt_release_bdev(bdev) == false) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							"Invalid parameters");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_gpt_release_bdev(&req);
	return;

cleanup:
	free_rpc_gpt_release_bdev(&req);
}
SPDK_RPC_REGISTER("gpt_release_bdev", spdk_rpc_gpt_release_bdev, SPDK_RPC_RUNTIME)