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
#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/blobfs_bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

struct rpc_blobfs_detect {
	char *bdev_name;

	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_blobfs_detect(struct rpc_blobfs_detect *req)
{
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_blobfs_detect_decoders[] = {
	{"bdev_name", offsetof(struct rpc_blobfs_detect, bdev_name), spdk_json_decode_string},
};

static void
_rpc_blobfs_detect_done(void *cb_arg, int fserrno)
{
	struct rpc_blobfs_detect *req = cb_arg;
	struct spdk_json_write_ctx *w;
	bool existed = true;

	if (fserrno == -EILSEQ) {
		/* There is no blobfs existing on bdev */
		existed = false;
	} else if (fserrno != 0) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-fserrno));

		return;
	}

	w = spdk_jsonrpc_begin_result(req->request);
	spdk_json_write_bool(w, existed);
	spdk_jsonrpc_end_result(req->request, w);

	free_rpc_blobfs_detect(req);
}

static void
spdk_rpc_blobfs_detect(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_blobfs_detect *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_blobfs_detect request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_blobfs_detect_decoders,
				    SPDK_COUNTOF(rpc_blobfs_detect_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		rc = -EINVAL;
		_rpc_blobfs_detect_done(req, rc);

		return;
	}

	req->request = request;
	rc = spdk_blobfs_bdev_detect(req->bdev_name, _rpc_blobfs_detect_done, req);
	if (rc != 0) {
		_rpc_blobfs_detect_done(req, rc);
	}
}

SPDK_RPC_REGISTER("blobfs_detect", spdk_rpc_blobfs_detect, SPDK_RPC_RUNTIME)

struct rpc_blobfs_create {
	char *bdev_name;
	uint32_t cluster_sz;

	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_blobfs_create(struct rpc_blobfs_create *req)
{
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_blobfs_create_decoders[] = {
	{"bdev_name", offsetof(struct rpc_blobfs_create, bdev_name), spdk_json_decode_string},
	{"cluster_sz", offsetof(struct rpc_blobfs_create, cluster_sz), spdk_json_decode_uint32, true},
};

static void
_rpc_blobfs_create_done(void *cb_arg, int fserrno)
{
	struct rpc_blobfs_create *req = cb_arg;
	struct spdk_json_write_ctx *w;

	if (fserrno != 0) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-fserrno));

		return;
	}

	w = spdk_jsonrpc_begin_result(req->request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(req->request, w);

	free_rpc_blobfs_create(req);
}

static void
spdk_rpc_blobfs_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_blobfs_create *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_blobfs_create request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_blobfs_create_decoders,
				    SPDK_COUNTOF(rpc_blobfs_create_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		rc = -EINVAL;
		_rpc_blobfs_create_done(req, rc);

		return;
	}

	req->request = request;
	rc = spdk_blobfs_bdev_create(req->bdev_name, req->cluster_sz, _rpc_blobfs_create_done, req);
	if (rc != 0) {
		_rpc_blobfs_create_done(req, rc);
	}
}

SPDK_RPC_REGISTER("blobfs_create", spdk_rpc_blobfs_create, SPDK_RPC_RUNTIME)
