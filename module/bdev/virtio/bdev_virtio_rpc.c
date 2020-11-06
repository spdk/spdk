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
#include "spdk/log.h"

#include "bdev_virtio.h"

#define SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT		1
#define SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE		512

struct rpc_remove_virtio_dev {
	char *name;
};

static const struct spdk_json_object_decoder rpc_remove_virtio_dev[] = {
	{"name", offsetof(struct rpc_remove_virtio_dev, name), spdk_json_decode_string },
};

static void
rpc_bdev_virtio_detach_controller_cb(void *ctx, int errnum)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (errnum != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-errnum));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_virtio_detach_controller(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_remove_virtio_dev req = {NULL};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_remove_virtio_dev,
				    SPDK_COUNTOF(rpc_remove_virtio_dev),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_virtio_blk_dev_remove(req.name, rpc_bdev_virtio_detach_controller_cb, request);
	if (rc == -ENODEV) {
		rc = bdev_virtio_scsi_dev_remove(req.name, rpc_bdev_virtio_detach_controller_cb, request);
	}

	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}

cleanup:
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_virtio_detach_controller",
		  rpc_bdev_virtio_detach_controller, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_virtio_detach_controller, remove_virtio_bdev)

static void
rpc_bdev_virtio_scsi_get_devices(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev_virtio_scsi_get_devices requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	bdev_virtio_scsi_dev_list(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_virtio_scsi_get_devices",
		  rpc_bdev_virtio_scsi_get_devices, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_virtio_scsi_get_devices, get_virtio_scsi_devs)

struct rpc_bdev_virtio_attach_controller_ctx {
	char *name;
	char *trtype;
	char *traddr;
	char *dev_type;
	uint32_t vq_count;
	uint32_t vq_size;
	struct spdk_jsonrpc_request *request;
};

static const struct spdk_json_object_decoder rpc_bdev_virtio_attach_controller_ctx[] = {
	{"name", offsetof(struct rpc_bdev_virtio_attach_controller_ctx, name), spdk_json_decode_string },
	{"trtype", offsetof(struct rpc_bdev_virtio_attach_controller_ctx, trtype), spdk_json_decode_string },
	{"traddr", offsetof(struct rpc_bdev_virtio_attach_controller_ctx, traddr), spdk_json_decode_string },
	{"dev_type", offsetof(struct rpc_bdev_virtio_attach_controller_ctx, dev_type), spdk_json_decode_string },
	{"vq_count", offsetof(struct rpc_bdev_virtio_attach_controller_ctx, vq_count), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_bdev_virtio_attach_controller_ctx, vq_size), spdk_json_decode_uint32, true },
};

static void
free_rpc_bdev_virtio_attach_controller_ctx(struct rpc_bdev_virtio_attach_controller_ctx *req)
{
	free(req->name);
	free(req->trtype);
	free(req->traddr);
	free(req->dev_type);
	free(req);
}

static void
rpc_create_virtio_dev_cb(void *ctx, int result, struct spdk_bdev **bdevs, size_t cnt)
{
	struct rpc_bdev_virtio_attach_controller_ctx *req = ctx;
	struct spdk_json_write_ctx *w;
	size_t i;

	if (result) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-result));
		free_rpc_bdev_virtio_attach_controller_ctx(req);
		return;
	}

	w = spdk_jsonrpc_begin_result(req->request);
	spdk_json_write_array_begin(w);

	for (i = 0; i < cnt; i++) {
		spdk_json_write_string(w, spdk_bdev_get_name(bdevs[i]));
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(req->request, w);

	free_rpc_bdev_virtio_attach_controller_ctx(ctx);
}

static void
rpc_bdev_virtio_attach_controller(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_bdev_virtio_attach_controller_ctx *req;
	struct spdk_bdev *bdev;
	struct spdk_pci_addr pci_addr;
	bool pci;
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("calloc() failed\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_virtio_attach_controller_ctx,
				    SPDK_COUNTOF(rpc_bdev_virtio_attach_controller_ctx),
				    req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (strcmp(req->trtype, "pci") == 0) {
		if (req->vq_count != 0 || req->vq_size != 0) {
			SPDK_ERRLOG("VQ count or size is not allowed for PCI transport type\n");
			spdk_jsonrpc_send_error_response(request, EINVAL,
							 "vq_count or vq_size is not allowed for PCI transport type.");
			goto cleanup;
		}

		if (spdk_pci_addr_parse(&pci_addr, req->traddr) != 0) {
			SPDK_ERRLOG("Invalid PCI address '%s'\n", req->traddr);
			spdk_jsonrpc_send_error_response_fmt(request, EINVAL, "Invalid PCI address '%s'", req->traddr);
			goto cleanup;
		}

		pci = true;
	} else if (strcmp(req->trtype, "user") == 0) {
		req->vq_count = req->vq_count == 0 ? SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT : req->vq_count;
		req->vq_size = req->vq_size == 0 ? SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE : req->vq_size;
		pci = false;
	} else {
		SPDK_ERRLOG("Invalid trtype '%s'\n", req->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, EINVAL, "Invalid trtype '%s'", req->trtype);
		goto cleanup;
	}

	req->request = request;
	if (strcmp(req->dev_type, "blk") == 0) {
		if (pci) {
			bdev = bdev_virtio_pci_blk_dev_create(req->name, &pci_addr);
		} else {
			bdev = bdev_virtio_user_blk_dev_create(req->name, req->traddr, req->vq_count, req->vq_size);
		}

		/* Virtio blk doesn't use callback so call it manually to send result. */
		rc = bdev ? 0 : -EINVAL;
		rpc_create_virtio_dev_cb(req, rc, &bdev, bdev ? 1 : 0);
	} else if (strcmp(req->dev_type, "scsi") == 0) {
		if (pci) {
			rc = bdev_virtio_pci_scsi_dev_create(req->name, &pci_addr, rpc_create_virtio_dev_cb, req);
		} else {
			rc = bdev_virtio_user_scsi_dev_create(req->name, req->traddr, req->vq_count, req->vq_size,
							      rpc_create_virtio_dev_cb, req);
		}

		if (rc < 0) {
			/* In case of error callback is not called so do it manually to send result. */
			rpc_create_virtio_dev_cb(req, rc, NULL, 0);
		}
	} else {
		SPDK_ERRLOG("Invalid dev_type '%s'\n", req->dev_type);
		spdk_jsonrpc_send_error_response_fmt(request, EINVAL, "Invalid dev_type '%s'", req->dev_type);
		goto cleanup;
	}

	return;

cleanup:
	free_rpc_bdev_virtio_attach_controller_ctx(req);
}
SPDK_RPC_REGISTER("bdev_virtio_attach_controller",
		  rpc_bdev_virtio_attach_controller, SPDK_RPC_RUNTIME);
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_virtio_attach_controller, construct_virtio_dev)
