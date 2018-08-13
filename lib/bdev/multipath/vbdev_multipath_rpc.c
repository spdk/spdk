/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 JetStream Software Inc.
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
 *     * Neither the name of JetStream Software Inc. nor the names of its
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

#include "vbdev_multipath.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

struct rpc_construct_multipath {
	char *multipath_bdev_name;
	char *bdev_names[MULTIPATH_MAX_PATHS + 1];
};

static void
free_rpc_construct_multipath(struct rpc_construct_multipath *m)
{
	if (m->multipath_bdev_name) {
		free(m->multipath_bdev_name);
	}

	for (char **p = m->bdev_names; *p; p ++) {
		free(*p);
	}
}

static int
mpath_json_decode_string_array(const struct spdk_json_val *values, void *out)
{
	size_t out_size;

	return spdk_json_decode_array(values, spdk_json_decode_string, out,
				      MULTIPATH_MAX_PATHS, &out_size, sizeof(char *));
}

static const struct spdk_json_object_decoder rpc_construct_multipath_decoders[] = {
	{
		"base_bdev_names", offsetof(struct rpc_construct_multipath, bdev_names),
		mpath_json_decode_string_array
	},
	{
		"multipath_bdev_name",
		offsetof(struct rpc_construct_multipath, multipath_bdev_name),
		spdk_json_decode_string
	},
};

/* Decode the parameters for the RPC method and execute the provided call.
 * Error status returned in the failed cases.
 */
static void
multipath_exec_rpc(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params, int (*multipath_call)(const char *, const char **))
{
	struct rpc_construct_multipath req;
	struct spdk_json_write_ctx *w;
	int rc;

	memset(&req, 0, sizeof(req));
	if (spdk_json_decode_object(params, rpc_construct_multipath_decoders,
				    SPDK_COUNTOF(rpc_construct_multipath_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto free_req;
	}

	rc = multipath_call(req.multipath_bdev_name, (const char **) req.bdev_names);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INVALID_REQUEST, strerror(-rc));
		goto free_req;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		goto free_req;
	}

	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, req.multipath_bdev_name);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

free_req:
	free_rpc_construct_multipath(&req);
	return;
}

static void
spdk_rpc_multipath_construct_vbdev(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	multipath_exec_rpc(request, params, vbdev_multipath_create_vbdev);
}
SPDK_RPC_REGISTER("vbdev_multipath_construct_vbdev",
		  spdk_rpc_multipath_construct_vbdev, SPDK_RPC_RUNTIME)

static void
spdk_rpc_multipath_path_up(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	multipath_exec_rpc(request, params, vbdev_multipath_path_up);
}
SPDK_RPC_REGISTER("vbdev_multipath_path_up", spdk_rpc_multipath_path_up, SPDK_RPC_RUNTIME)

static void
spdk_rpc_multipath_path_down(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	multipath_exec_rpc(request, params, vbdev_multipath_path_down);
}
SPDK_RPC_REGISTER("vbdev_multipath_path_down", spdk_rpc_multipath_path_down, SPDK_RPC_RUNTIME)
