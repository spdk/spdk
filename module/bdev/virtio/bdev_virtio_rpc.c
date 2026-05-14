/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk_internal/rpc_autogen.h"

#include "bdev_virtio.h"

#define SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT		1
#define SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE		512

static void
rpc_bdev_virtio_blk_set_hotplug(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_virtio_blk_set_hotplug_ctx req = {false, 0};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_virtio_blk_set_hotplug_decoders,
				    SPDK_COUNTOF(rpc_bdev_virtio_blk_set_hotplug_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = bdev_virtio_pci_blk_set_hotplug(req.enable, req.period_us);
	if (rc) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;
invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("bdev_virtio_blk_set_hotplug", rpc_bdev_virtio_blk_set_hotplug, SPDK_RPC_RUNTIME)

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
	struct rpc_bdev_virtio_detach_controller_ctx req = {};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_virtio_detach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_virtio_detach_controller_decoders),
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
	free_rpc_bdev_virtio_detach_controller(&req);
}
SPDK_RPC_REGISTER("bdev_virtio_detach_controller",
		  rpc_bdev_virtio_detach_controller, SPDK_RPC_RUNTIME)

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

static void
rpc_create_virtio_dev_cb(void *ctx, int result, struct spdk_bdev **bdevs, size_t cnt)
{
	struct rpc_bdev_virtio_attach_controller_ctx *req = ctx;
	struct spdk_json_write_ctx *w;
	size_t i;

	if (result) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-result));
		free_rpc_bdev_virtio_attach_controller(req);
		free(req);
		return;
	}

	w = spdk_jsonrpc_begin_result(req->request);
	spdk_json_write_array_begin(w);

	for (i = 0; i < cnt; i++) {
		spdk_json_write_string(w, spdk_bdev_get_name(bdevs[i]));
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(req->request, w);

	free_rpc_bdev_virtio_attach_controller_heap(req);
}

static void
rpc_bdev_virtio_attach_controller(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_bdev_virtio_attach_controller_ctx *req;
	struct spdk_bdev *bdev = NULL;
	struct spdk_pci_addr pci_addr;
	int rc = 0;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("calloc() failed\n");
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_virtio_attach_controller_decoders,
				    SPDK_COUNTOF(rpc_bdev_virtio_attach_controller_decoders),
				    req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_PCI) {
		if (req->vq_count != 0 || req->vq_size != 0) {
			SPDK_ERRLOG("VQ count or size is not allowed for PCI transport type\n");
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "vq_count or vq_size is not allowed for PCI transport type.");
			goto cleanup;
		}

		if (spdk_pci_addr_parse(&pci_addr, req->traddr) != 0) {
			SPDK_ERRLOG("Invalid PCI address '%s'\n", req->traddr);
			spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Invalid PCI address '%s'", req->traddr);
			goto cleanup;
		}
	} else if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_USER) {
		req->vq_count = req->vq_count == 0 ? SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT : req->vq_count;
		req->vq_size = req->vq_size == 0 ? SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE : req->vq_size;
	} else if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_VFIO_USER) {
		if (req->vq_count != 0 || req->vq_size != 0) {
			SPDK_ERRLOG("VQ count or size is not allowed for vfio-user transport type\n");
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "vq_count or vq_size is not allowed for vfio-user transport type.");
			goto cleanup;
		}
	}

	req->request = request;
	if (req->dev_type == RPC_BDEV_VIRTIO_DEV_TYPE_BLK) {
		if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_PCI) {
			bdev = bdev_virtio_pci_blk_dev_create(req->name, &pci_addr);
		} else if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_USER) {
			bdev = bdev_virtio_user_blk_dev_create(req->name, req->traddr, req->vq_count, req->vq_size);
		} else if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_VFIO_USER) {
			bdev = bdev_virtio_vfio_user_blk_dev_create(req->name, req->traddr);
		}

		/* Virtio blk doesn't use callback so call it manually to send result. */
		rc = bdev ? 0 : -EINVAL;
		rpc_create_virtio_dev_cb(req, rc, &bdev, bdev ? 1 : 0);
	} else if (req->dev_type == RPC_BDEV_VIRTIO_DEV_TYPE_SCSI) {
		if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_PCI) {
			rc = bdev_virtio_pci_scsi_dev_create(req->name, &pci_addr, rpc_create_virtio_dev_cb, req);
		} else if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_USER) {
			rc = bdev_virtio_user_scsi_dev_create(req->name, req->traddr, req->vq_count, req->vq_size,
							      rpc_create_virtio_dev_cb, req);
		} else if (req->trtype == RPC_BDEV_VIRTIO_TRTYPE_VFIO_USER) {
			rc = bdev_vfio_user_scsi_dev_create(req->name, req->traddr, rpc_create_virtio_dev_cb, req);
		}

		if (rc < 0) {
			/* In case of error callback is not called so do it manually to send result. */
			rpc_create_virtio_dev_cb(req, rc, NULL, 0);
		}
	}

	return;

cleanup:
	free_rpc_bdev_virtio_attach_controller_heap(req);
}
SPDK_RPC_REGISTER("bdev_virtio_attach_controller",
		  rpc_bdev_virtio_attach_controller, SPDK_RPC_RUNTIME);
