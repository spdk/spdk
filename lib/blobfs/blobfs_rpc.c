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
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

struct rpc_bdev_blobfs_check {
	char *bdev_name;

	/* Used to search one available nbd device */
	struct spdk_filesystem *fs;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_bdev_blobfs_check(struct rpc_bdev_blobfs_check *req)
{
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_bdev_blobfs_check_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_blobfs_check, bdev_name), spdk_json_decode_string},
};

static void
_bdev_blobfs_check_unload_cb(void *ctx, int fserrno)
{
	struct rpc_bdev_blobfs_check *req = ctx;
	struct spdk_jsonrpc_request *request = req->request;
	struct spdk_json_write_ctx *w;

	if (fserrno) {
		SPDK_ERRLOG("Failed to unload blobfs on bdev %s: errno %d", req->bdev_name, fserrno);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "blobfs unload failed");

		free_rpc_bdev_blobfs_check(req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_bdev_blobfs_check(req);
}

static void
_bdev_blobfs_check_unload(void *ctx)
{
	struct rpc_bdev_blobfs_check *req = ctx;

	spdk_fs_unload(req->fs, _bdev_blobfs_check_unload_cb, req);
}

static void
_bdev_blobfs_check_load_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct rpc_bdev_blobfs_check *req = ctx;

	if (fserrno == -EILSEQ) {
		struct spdk_json_write_ctx *w;

		w = spdk_jsonrpc_begin_result(req->request);
		spdk_json_write_bool(w, false);
		spdk_jsonrpc_end_result(req->request, w);

		free_rpc_bdev_blobfs_check(req);
		return;
	} else if (fserrno) {
		SPDK_ERRLOG("Failed to load blobfs on bdev %s: errno %d\n", req->bdev_name, fserrno);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "blobfs load failed");

		free_rpc_bdev_blobfs_check(req);
		return;
	}

	req->fs = fs;

	spdk_thread_send_msg(spdk_get_thread(), _bdev_blobfs_check_unload, req);
}

static void
spdk_rpc_bdev_blobfs_check(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_blobfs_check *req;
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *bs_dev;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_bdev_blobfs_check request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_blobfs_check_decoders,
				    SPDK_COUNTOF(rpc_bdev_blobfs_check_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	if (req->bdev_name == NULL) {
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req->bdev_name);
	if (bdev == NULL) {
		SPDK_INFOLOG(SPDK_LOG_BLOBFS, "bdev %s not found\n", req->bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Specified bdev doesn't exist");
		goto invalid;
	}

	req->request = request;
	bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	spdk_fs_load(bs_dev, NULL, _bdev_blobfs_check_load_cb, req);

	return;

invalid:
	free_rpc_bdev_blobfs_check(req);
}

SPDK_RPC_REGISTER("bdev_blobfs_check", spdk_rpc_bdev_blobfs_check, SPDK_RPC_RUNTIME)

struct rpc_bdev_blobfs_create {
	char *bdev_name;
	uint32_t cluster_sz;

	/* Used to search one available nbd device */
	struct spdk_filesystem *fs;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_bdev_blobfs_create(struct rpc_bdev_blobfs_create *req)
{
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_bdev_blobfs_create_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_blobfs_create, bdev_name), spdk_json_decode_string},
	{"cluster_sz", offsetof(struct rpc_bdev_blobfs_create, cluster_sz), spdk_json_decode_uint32, true},
};

static void
_bdev_blobfs_create_unload_cb(void *ctx, int fserrno)
{
	struct rpc_bdev_blobfs_create *req = ctx;
	struct spdk_jsonrpc_request *request = req->request;
	struct spdk_json_write_ctx *w;

	if (fserrno) {
		SPDK_ERRLOG("Failed to unload blobfs on bdev %s: errno %d\n", req->bdev_name, fserrno);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "blobfs unload failed");

		free_rpc_bdev_blobfs_create(req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_bdev_blobfs_create(req);
}

static void
_bdev_blobfs_create_unload(void *ctx)
{
	struct rpc_bdev_blobfs_create *req = ctx;

	spdk_fs_unload(req->fs, _bdev_blobfs_create_unload_cb, req);
}

static void
_bdev_blobfs_create_init_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct rpc_bdev_blobfs_create *req = ctx;

	if (fserrno) {
		SPDK_ERRLOG("Failed to init blobfs on bdev %s: errno %d", req->bdev_name, fserrno);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "blobfs init failed");

		free_rpc_bdev_blobfs_create(req);
		return;
	}

	req->fs = fs;

	spdk_thread_send_msg(spdk_get_thread(), _bdev_blobfs_create_unload, req);
}

static void
spdk_rpc_bdev_blobfs_create(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_blobfs_create *req;
	struct spdk_bdev *bdev;
	struct spdk_blobfs_opts blobfs_opt;
	struct spdk_bs_dev *bs_dev;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_bdev_blobfs_create request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_blobfs_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_blobfs_create_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	if (req->bdev_name == NULL) {
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req->bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", req->bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Specified bdev doesn't exist");
		goto invalid;
	}

	spdk_fs_opts_init(&blobfs_opt);
	if (req->cluster_sz) {
		blobfs_opt.cluster_sz = req->cluster_sz;
	}

	req->request = request;
	bs_dev = spdk_bdev_create_bs_dev(bdev, NULL, NULL);
	if (blobfs_opt.cluster_sz) {
		spdk_fs_init(bs_dev, &blobfs_opt, NULL, _bdev_blobfs_create_init_cb, req);
	} else {
		spdk_fs_init(bs_dev, NULL, NULL, _bdev_blobfs_create_init_cb, req);
	}

	return;

invalid:
	free_rpc_bdev_blobfs_create(req);
}

SPDK_RPC_REGISTER("bdev_blobfs_create", spdk_rpc_bdev_blobfs_create, SPDK_RPC_RUNTIME)
