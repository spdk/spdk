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

#include "spdk_internal/bdev.h"
#include "bdev_virtio.h"

struct rpc_virtio_user_scsi_dev {
	char *path;
	uint32_t max_queue;
	uint32_t vq_size;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;

};

static const struct spdk_json_object_decoder rpc_construct_virtio_user_scsi_dev[] = {
	{"path", offsetof(struct rpc_virtio_user_scsi_dev, path), spdk_json_decode_string },
	{"max_queue", offsetof(struct rpc_virtio_user_scsi_dev, max_queue), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_virtio_user_scsi_dev, vq_size), spdk_json_decode_uint32, true },
};

static void
free_rpc_connect_virtio_user_scsi_dev(struct rpc_virtio_user_scsi_dev *req)
{
	if (!req) {
		return;
	}

	free(req->path);
	/* XXX: Debug */
	memset(req, 0, sizeof(*req));
	free(req);
}

static void
spdk_rpc_connect_virtio_user_scsi_done_cb(void *ctx, void *bdev_ptr)
{
	struct rpc_virtio_user_scsi_dev *req = ctx;
	struct spdk_bdev *bdev = bdev_ptr;

	if (req->w == NULL) {
		req->w = spdk_jsonrpc_begin_result(req->request);
		if (req->w) {
			spdk_json_write_array_begin(req->w);
		}
	}

	if (req->w) {
		if (bdev) {
			spdk_json_write_string(req->w, bdev->name);
			return;
		} else {
			spdk_json_write_array_end(req->w);
			spdk_jsonrpc_end_result(req->request, req->w);
		}
	}

	if (!bdev) {
		free_rpc_connect_virtio_user_scsi_dev(ctx);
	}
}

static void
spdk_rpc_connect_virtio_user_scsi(struct spdk_jsonrpc_request *request,
				      const struct spdk_json_val *params)
{
	struct rpc_virtio_user_scsi_dev *req;
	char buf[64];
	int rc;

	req = calloc(1, sizeof(*req));
	if (spdk_json_decode_object(params, rpc_construct_virtio_user_scsi_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_user_scsi_dev),
				    req)) {
		rc = -EINVAL;
		goto invalid;
	}

	req->request = request;
	rc = spdk_virtio_user_scsi_connect(req->path, req->max_queue, req->vq_size, spdk_rpc_connect_virtio_user_scsi_done_cb, req);
	if (rc < 0) {
		goto invalid;
	}

	return;

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_connect_virtio_user_scsi_dev(req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("spdk_rpc_connect_virtio_user_scsi", spdk_rpc_connect_virtio_user_scsi);
