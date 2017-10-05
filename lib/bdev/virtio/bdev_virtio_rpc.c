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

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk_internal/log.h"

#include "spdk_internal/bdev.h"
#include "bdev_virtio.h"

struct rpc_virtio_user_scsi_dev {
	char *path;
	char *prefix;
	uint32_t vq_count;
	uint32_t vq_size;
	struct spdk_jsonrpc_request *request;

};

static const struct spdk_json_object_decoder rpc_construct_virtio_user_scsi_dev[] = {
	{"path", offsetof(struct rpc_virtio_user_scsi_dev, path), spdk_json_decode_string },
	{"prefix", offsetof(struct rpc_virtio_user_scsi_dev, path), spdk_json_decode_string, true },
	{"vq_count", offsetof(struct rpc_virtio_user_scsi_dev, vq_count), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_virtio_user_scsi_dev, vq_size), spdk_json_decode_uint32, true },
};

static void
free_rpc_connect_virtio_user_scsi_dev(struct rpc_virtio_user_scsi_dev *req)
{
	if (req) {
		free(req->path);
		free(req->prefix);
		free(req);
	}
}

static void
rpc_create_virtio_user_scsi_bdev_cb(void *ctx, struct spdk_bdev **bdevs, size_t cnt)
{
	struct rpc_virtio_user_scsi_dev *req = ctx;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(req->request);

	if (w) {
		spdk_json_write_array_begin(w);

		for (; cnt; cnt--) {
			spdk_json_write_string(w, (*bdevs)->name);
			bdevs++;
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
	char buf[64];
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->vq_count = spdk_env_get_core_count();
	req->vq_size = 128;

	if (spdk_json_decode_object(params, rpc_construct_virtio_user_scsi_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_user_scsi_dev),
				    req)) {
		rc = -EINVAL;
		goto invalid;
	}

	req->request = request;
	rc = create_virtio_user_scsi_device(req->path, req->prefix, req->vq_count, req->vq_size,
					    rpc_create_virtio_user_scsi_bdev_cb, req);
	if (rc < 0) {
		goto invalid;
	}

	return;

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_connect_virtio_user_scsi_dev(req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("construct_virtio_user_scsi_bdev", spdk_rpc_create_virtio_user_scsi_bdev);
