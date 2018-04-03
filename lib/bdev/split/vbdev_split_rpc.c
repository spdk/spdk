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
#include "spdk/string.h"

#include "vbdev_split.h"
#include "spdk_internal/log.h"

struct rpc_construct_split {
	char *base_bdev;
	uint32_t split_count;
	uint64_t split_size_mb;
};

static const struct spdk_json_object_decoder rpc_construct_split_decoders[] = {
	{"base_bdev", offsetof(struct rpc_construct_split, base_bdev), spdk_json_decode_string},
	{"split_count", offsetof(struct rpc_construct_split, split_count), spdk_json_decode_uint32},
	{"split_size_mb", offsetof(struct rpc_construct_split, split_size_mb), spdk_json_decode_uint64, true},
};

static void
spdk_rpc_construct_split_vbdev(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_construct_split req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *base_bdev;
	size_t i;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_split_decoders,
				    SPDK_COUNTOF(rpc_construct_split_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = create_vbdev_split(req.base_bdev, req.split_count, req.split_size_mb);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to create %"PRIu32" split bdevs from '%s': %s",
						     req.split_count, req.base_bdev, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		goto out;
	}

	spdk_json_write_array_begin(w);

	base_bdev = spdk_bdev_get_by_name(req.base_bdev);
	if (base_bdev != NULL) {
		for (i = 0; i < base_bdev->vbdevs_cnt; i++) {
			spdk_json_write_string(w, spdk_bdev_get_name(base_bdev->vbdevs[i]));
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free(req.base_bdev);
}
SPDK_RPC_REGISTER("construct_split_vbdev", spdk_rpc_construct_split_vbdev)

struct rpc_destruct_split {
	char *base_bdev;
};

static const struct spdk_json_object_decoder rpc_destruct_split_decoders[] = {
	{"base_bdev", offsetof(struct rpc_destruct_split, base_bdev), spdk_json_decode_string},
};

static void
spdk_rpc_destruct_split(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_destruct_split req = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_destruct_split_decoders,
				    SPDK_COUNTOF(rpc_destruct_split_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = spdk_vbdev_split_destruct(req.base_bdev);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

out:
	free(req.base_bdev);
}
SPDK_RPC_REGISTER("destruct_split_vbdev", spdk_rpc_destruct_split)
