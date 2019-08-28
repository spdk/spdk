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

static void
_rpc_bdev_blobfs_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	SPDK_WARNLOG("Async evnet(%d) is triggered in bdev %s", type, spdk_bdev_get_name(bdev));
}

struct rpc_bdev_blobfs_detect {
	char *bdev_name;

	struct spdk_filesystem *fs;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_bdev_blobfs_detect(struct rpc_bdev_blobfs_detect *req)
{
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_bdev_blobfs_detect_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_blobfs_detect, bdev_name), spdk_json_decode_string},
};

static void
_bdev_blobfs_detect_unload_cb(void *ctx, int fserrno)
{
	struct rpc_bdev_blobfs_detect *req = ctx;
	struct spdk_jsonrpc_request *request = req->request;
	struct spdk_json_write_ctx *w;

	if (fserrno) {
		SPDK_ERRLOG("Failed to unload blobfs on bdev %s: errno %d", req->bdev_name, fserrno);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "blobfs unload failed");

		free_rpc_bdev_blobfs_detect(req);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_bdev_blobfs_detect(req);
}

static void
_bdev_blobfs_detect_unload(void *ctx)
{
	struct rpc_bdev_blobfs_detect *req = ctx;

	spdk_fs_unload(req->fs, _bdev_blobfs_detect_unload_cb, req);
}

static void
_bdev_blobfs_detect_load_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct rpc_bdev_blobfs_detect *req = ctx;

	if (fserrno == -EILSEQ) {
		struct spdk_json_write_ctx *w;

		w = spdk_jsonrpc_begin_result(req->request);
		spdk_json_write_bool(w, false);
		spdk_jsonrpc_end_result(req->request, w);

		free_rpc_bdev_blobfs_detect(req);
		return;
	} else if (fserrno) {
		SPDK_ERRLOG("Failed to load blobfs on bdev %s: errno %d\n", req->bdev_name, fserrno);
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "blobfs load failed");

		free_rpc_bdev_blobfs_detect(req);
		return;
	}

	req->fs = fs;

	spdk_thread_send_msg(spdk_get_thread(), _bdev_blobfs_detect_unload, req);
}

static void
spdk_rpc_bdev_blobfs_detect(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_blobfs_detect *req;
	struct spdk_bs_dev *bs_dev;
	struct spdk_bdev_desc *desc;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_bdev_blobfs_detect request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_blobfs_detect_decoders,
				    SPDK_COUNTOF(rpc_bdev_blobfs_detect_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	rc = spdk_bdev_open_ext(req->bdev_name, false, _rpc_bdev_blobfs_event_cb, NULL, &desc);
	if (rc != 0) {
		if (rc == -EINVAL) {
			SPDK_INFOLOG(SPDK_LOG_BLOBFS, "bdev %s not found\n", req->bdev_name);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Specified bdev doesn't exist");
		} else {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Failed to open bdev");
		}

		goto invalid;
	}

	req->request = request;
	bs_dev = spdk_bdev_create_bs_dev_from_desc(desc);
	if (bs_dev == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to create a blobstore block device from bdev desc");

		goto invalid;
	}

	spdk_fs_load(bs_dev, NULL, _bdev_blobfs_detect_load_cb, req);

	return;

invalid:
	free_rpc_bdev_blobfs_detect(req);
}

SPDK_RPC_REGISTER("bdev_blobfs_detect", spdk_rpc_bdev_blobfs_detect, SPDK_RPC_RUNTIME)

struct rpc_bdev_blobfs_create {
	char *bdev_name;
	uint32_t cluster_sz;

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
	struct spdk_blobfs_opts blobfs_opt;
	struct spdk_bs_dev *bs_dev;
	struct spdk_bdev_desc *desc;
	int rc;

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
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	rc = spdk_bdev_open_ext(req->bdev_name, true, _rpc_bdev_blobfs_event_cb, NULL, &desc);
	if (rc != 0) {
		if (rc == -EINVAL) {
			SPDK_INFOLOG(SPDK_LOG_BLOBFS, "bdev %s not found\n", req->bdev_name);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Specified bdev doesn't exist");
		} else {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Failed to open bdev");
		}

		goto invalid;
	}

	bs_dev = spdk_bdev_create_bs_dev_from_desc(desc);
	if (bs_dev == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to create a blobstore block device from bdev desc");

		goto invalid;
	}

	spdk_fs_opts_init(&blobfs_opt);
	if (req->cluster_sz) {
		blobfs_opt.cluster_sz = req->cluster_sz;
	}

	req->request = request;
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
