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

#include "vbdev_compress.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"

struct rpc_set_compress_pmd {
	enum compress_pmd pmd;
};

static const struct spdk_json_object_decoder rpc_compress_pmd_decoder[] = {
	{"pmd", offsetof(struct rpc_set_compress_pmd, pmd), spdk_json_decode_int32},
};

static void
spdk_rpc_set_compress_pmd(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_set_compress_pmd req;
	struct spdk_json_write_ctx *w;
	int rc, jerr = 0;

	if (spdk_json_decode_object(params, rpc_compress_pmd_decoder,
				    SPDK_COUNTOF(rpc_compress_pmd_decoder),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		jerr = SPDK_JSONRPC_ERROR_PARSE_ERROR;
		goto invalid;
	}

	if (req.pmd >= COMPRESS_PMD_MAX) {
		rc = -EINVAL;
		jerr = SPDK_JSONRPC_ERROR_INVALID_PARAMS;
		goto invalid;
	}

	rc = set_compress_pmd(&req.pmd);
	if (rc) {
		rc = -EINVAL;
		jerr = SPDK_JSONRPC_ERROR_INTERNAL_ERROR;
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w != NULL) {
		spdk_json_write_bool(w, true);
		spdk_jsonrpc_end_result(request, w);
	}

	return;
invalid:
	spdk_jsonrpc_send_error_response(request, jerr, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("set_compress_pmd", spdk_rpc_set_compress_pmd, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_construct_compress {
	char *base_bdev_name;
	char *pm_path;
};

/* Free the allocated memory resource after the RPC handling. */
static void
free_rpc_construct_compress(struct rpc_construct_compress *r)
{
	free(r->base_bdev_name);
	free(r->pm_path);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_construct_compress_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_compress, base_bdev_name), spdk_json_decode_string},
	{"pm_path", offsetof(struct rpc_construct_compress, pm_path), spdk_json_decode_string},
};

/* Decode the parameters for this RPC method and properly construct the compress
 * device. Error status returned in the failed cases.
 */
static void
spdk_rpc_construct_compress_bdev(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_construct_compress req = {NULL};
	struct spdk_json_write_ctx *w;
	char *name;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_compress_decoders,
				    SPDK_COUNTOF(rpc_construct_compress_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_COMPRESS, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	rc = create_compress_bdev(req.base_bdev_name, req.pm_path);
	if (rc != 0) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_construct_compress(&req);
		return;
	}

	name = spdk_sprintf_alloc("COMP_%s", req.base_bdev_name);
	spdk_json_write_string(w, name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_construct_compress(&req);
	free(name);
	return;

invalid:
	free_rpc_construct_compress(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}
SPDK_RPC_REGISTER("construct_compress_bdev", spdk_rpc_construct_compress_bdev, SPDK_RPC_RUNTIME)

struct rpc_delete_compress {
	char *name;
};

static void
free_rpc_delete_compress(struct rpc_delete_compress *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_compress_decoders[] = {
	{"name", offsetof(struct rpc_delete_compress, name), spdk_json_decode_string},
};

static void
_spdk_rpc_delete_compress_bdev_cb(void *cb_arg, int bdeverrno)
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
spdk_rpc_delete_compress_bdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_delete_compress req = {NULL};
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_compress_decoders,
				    SPDK_COUNTOF(rpc_delete_compress_decoders),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	delete_compress_bdev(bdev, _spdk_rpc_delete_compress_bdev_cb, request);

	free_rpc_delete_compress(&req);

	return;

invalid:
	free_rpc_delete_compress(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("delete_compress_bdev", spdk_rpc_delete_compress_bdev, SPDK_RPC_RUNTIME)
