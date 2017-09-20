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
};

static const struct spdk_json_object_decoder rpc_construct_virtio_user_scsi_dev[] = {
	{"path", offsetof(struct rpc_virtio_user_scsi_dev, path), spdk_json_decode_string },
	{"max_queue", offsetof(struct rpc_virtio_user_scsi_dev, max_queue), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_virtio_user_scsi_dev, vq_size), spdk_json_decode_uint32, true },
};

static void
free_rpc_connect_virtio_user_scsi_dev(struct rpc_virtio_user_scsi_dev *req)
{
	free(req->path);
}

static void
spdk_rpc_connect_virtio_user_scsi_dev(struct spdk_jsonrpc_request *request,
				      const struct spdk_json_val *params)
{
	struct rpc_virtio_user_scsi_dev req = {0};
	struct spdk_json_write_ctx *w;
	char buf[64];
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_virtio_user_scsi_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_user_scsi_dev),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_virtio_user_scsi_connect(req.path, req.max_queue, req.vq_size);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_connect_virtio_user_scsi_dev(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_connect_virtio_user_scsi_dev(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("connect_virtio_user_scsi_dev", spdk_rpc_connect_virtio_user_scsi_dev);

struct rpc_disconnect_virtio_user_dev {
	char *path;
};

static const struct spdk_json_object_decoder rpc_disconnect_virtio_user[] = {
	{"path", offsetof(struct rpc_disconnect_virtio_user_dev, path), spdk_json_decode_string },
};

static void
free_rpc_disconnect_virtio_user_dev(struct rpc_disconnect_virtio_user_dev *req)
{
	free(req->path);
}

static void
spdk_rpc_disconnect_virtio_user_dev(struct spdk_jsonrpc_request *request,
				      const struct spdk_json_val *params){

	struct spdk_json_write_ctx *w;
	struct rpc_disconnect_virtio_user_dev req = {0};
	char buf[64];
	int rc;

	if (spdk_json_decode_object(params, rpc_disconnect_virtio_user,
				    SPDK_COUNTOF(rpc_disconnect_virtio_user),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_virtio_user_disconnect(req.path);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_disconnect_virtio_user_dev(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_disconnect_virtio_user_dev(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("disconnect_virtio_user_dev", spdk_rpc_disconnect_virtio_user_dev);
