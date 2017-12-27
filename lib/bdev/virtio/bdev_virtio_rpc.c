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

#include "bdev_virtio.h"

struct rpc_virtio_user_scsi_dev {
	char *path;
	char *name;
	uint32_t vq_count;
	uint32_t vq_size;
	struct spdk_jsonrpc_request *request;

};

static const struct spdk_json_object_decoder rpc_construct_virtio_user_scsi_dev[] = {
	{"path", offsetof(struct rpc_virtio_user_scsi_dev, path), spdk_json_decode_string },
	{"name", offsetof(struct rpc_virtio_user_scsi_dev, name), spdk_json_decode_string },
	{"vq_count", offsetof(struct rpc_virtio_user_scsi_dev, vq_size), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_virtio_user_scsi_dev, vq_size), spdk_json_decode_uint32, true },
};

static void
free_rpc_connect_virtio_user_scsi_dev(struct rpc_virtio_user_scsi_dev *req)
{
	if (!req) {
		return;
	}

	free(req->path);
	free(req->name);
	free(req);
}

static void
rpc_create_virtio_user_scsi_bdev_cb(void *ctx, int result, struct spdk_bdev **bdevs, size_t cnt)
{
	struct rpc_virtio_user_scsi_dev *req = ctx;
	struct spdk_json_write_ctx *w;
	size_t i;

	if (result) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-result));
		free_rpc_connect_virtio_user_scsi_dev(req);
		return;
	}

	w = spdk_jsonrpc_begin_result(req->request);
	if (w) {
		spdk_json_write_array_begin(w);

		for (i = 0; i < cnt; i++) {
			spdk_json_write_string(w, spdk_bdev_get_name(bdevs[i]));
		}

		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(req->request, w);
	}

	free_rpc_connect_virtio_user_scsi_dev(ctx);
}

static void
spdk_rpc_create_virtio_user_scsi_bdev(struct spdk_jsonrpc_request *request,
				      const struct spdk_json_val *params)
{
	struct rpc_virtio_user_scsi_dev *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->vq_count = 1;
	req->vq_size = 512;

	if (spdk_json_decode_object(params, rpc_construct_virtio_user_scsi_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_user_scsi_dev),
				    req)) {
		rc = -EINVAL;
		goto invalid;
	}

	req->request = request;
	rc = bdev_virtio_scsi_dev_create(req->name, req->path, req->vq_count, req->vq_size,
					 rpc_create_virtio_user_scsi_bdev_cb, req);
	if (rc < 0) {
		goto invalid;
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_connect_virtio_user_scsi_dev(req);
}
SPDK_RPC_REGISTER("construct_virtio_user_scsi_bdev", spdk_rpc_create_virtio_user_scsi_bdev);
