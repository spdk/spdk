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

#define SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT		1
#define SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE		512

struct rpc_construct_virtio_scsi_dev {
	char *path;
	char *pci_address;
	char *name;
	uint32_t vq_count;
	uint32_t vq_size;
	struct spdk_jsonrpc_request *request;

};

static const struct spdk_json_object_decoder rpc_construct_virtio_user_scsi_dev[] = {
	{"path", offsetof(struct rpc_construct_virtio_scsi_dev, path), spdk_json_decode_string },
	{"name", offsetof(struct rpc_construct_virtio_scsi_dev, name), spdk_json_decode_string },
	{"vq_count", offsetof(struct rpc_construct_virtio_scsi_dev, vq_size), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_construct_virtio_scsi_dev, vq_size), spdk_json_decode_uint32, true },
};

static void
free_rpc_construct_virtio_scsi_dev(struct rpc_construct_virtio_scsi_dev *req)
{
	if (!req) {
		return;
	}

	free(req->path);
	free(req->pci_address);
	free(req->name);
	free(req);
}

static void
rpc_construct_virtio_scsi_dev_cb(void *ctx, int result, struct spdk_bdev **bdevs, size_t cnt)
{
	struct rpc_construct_virtio_scsi_dev *req = ctx;
	struct spdk_json_write_ctx *w;
	size_t i;

	if (result) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-result));
		free_rpc_construct_virtio_scsi_dev(req);
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

	free_rpc_construct_virtio_scsi_dev(ctx);
}

static void
spdk_rpc_create_virtio_user_scsi_bdev(struct spdk_jsonrpc_request *request,
				      const struct spdk_json_val *params)
{
	struct rpc_construct_virtio_scsi_dev *req;
	int rc;

	SPDK_WARNLOG("construct_virtio_user_scsi_bdev command has been deprecated and will be removed "
		     "in the subsequent release. Please use construct_virtio_dev instead.\n");

	req = calloc(1, sizeof(*req));
	if (!req) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->pci_address = NULL;
	req->vq_count = SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT;
	req->vq_size = SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE;

	if (spdk_json_decode_object(params, rpc_construct_virtio_user_scsi_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_user_scsi_dev),
				    req)) {
		rc = -EINVAL;
		goto invalid;
	}

	req->request = request;
	rc = bdev_virtio_user_scsi_dev_create(req->name, req->path, req->vq_count, req->vq_size,
					      rpc_construct_virtio_scsi_dev_cb, req);
	if (rc < 0) {
		goto invalid;
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_construct_virtio_scsi_dev(req);
}
SPDK_RPC_REGISTER("construct_virtio_user_scsi_bdev", spdk_rpc_create_virtio_user_scsi_bdev,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_construct_virtio_pci_scsi_dev[] = {
	{"pci_address", offsetof(struct rpc_construct_virtio_scsi_dev, pci_address), spdk_json_decode_string },
	{"name", offsetof(struct rpc_construct_virtio_scsi_dev, name), spdk_json_decode_string },
};

static void
spdk_rpc_construct_virtio_pci_scsi_dev(struct spdk_jsonrpc_request *request,
				       const struct spdk_json_val *params)
{
	struct rpc_construct_virtio_scsi_dev *req;
	struct spdk_pci_addr pci_addr;
	int rc;

	SPDK_WARNLOG("construct_virtio_pci_scsi_bdev command has been deprecated and will be removed "
		     "in the subsequent release. Please use construct_virtio_dev instead.\n");

	req = calloc(1, sizeof(*req));
	if (!req) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->path = NULL;

	if (spdk_json_decode_object(params, rpc_construct_virtio_pci_scsi_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_pci_scsi_dev),
				    req)) {
		rc = -EINVAL;
		goto invalid;
	}

	if (spdk_pci_addr_parse(&pci_addr, req->pci_address) != 0) {
		SPDK_ERRLOG("Invalid PCI address '%s'\n", req->pci_address);
		rc = -EINVAL;
		goto invalid;
	}

	req->request = request;
	rc = bdev_virtio_pci_scsi_dev_create(req->name, &pci_addr,
					     rpc_construct_virtio_scsi_dev_cb, req);
	if (rc < 0) {
		goto invalid;
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_construct_virtio_scsi_dev(req);
}
SPDK_RPC_REGISTER("construct_virtio_pci_scsi_bdev", spdk_rpc_construct_virtio_pci_scsi_dev,
		  SPDK_RPC_RUNTIME);

struct rpc_remove_virtio_dev {
	char *name;
};

static const struct spdk_json_object_decoder rpc_remove_virtio_dev[] = {
	{"name", offsetof(struct rpc_remove_virtio_dev, name), spdk_json_decode_string },
};

static void
spdk_rpc_remove_virtio_scsi_bdev_cb(void *ctx, int errnum)
{
	struct spdk_jsonrpc_request *request = ctx;
	struct spdk_json_write_ctx *w;

	if (errnum != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-errnum));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_remove_virtio_scsi_bdev(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_remove_virtio_dev req = {NULL};
	int rc;

	SPDK_WARNLOG("remove_virtio_scsi_bdev command has been deprecated and will be removed "
		     "in the subsequent release. Please use remove_virtio_bdev instead.\n");

	if (spdk_json_decode_object(params, rpc_remove_virtio_dev,
				    SPDK_COUNTOF(rpc_remove_virtio_dev),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	rc = bdev_virtio_scsi_dev_remove(req.name, spdk_rpc_remove_virtio_scsi_bdev_cb, request);
	if (rc != 0) {
		goto invalid;
	}

	free(req.name);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free(req.name);
}
SPDK_RPC_REGISTER("remove_virtio_scsi_bdev", spdk_rpc_remove_virtio_scsi_bdev, SPDK_RPC_RUNTIME);

static void
spdk_rpc_remove_virtio_bdev_cb(void *ctx, int errnum)
{
	struct spdk_jsonrpc_request *request = ctx;
	struct spdk_json_write_ctx *w;

	if (errnum != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-errnum));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_remove_virtio_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_remove_virtio_dev req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_remove_virtio_dev,
				    SPDK_COUNTOF(rpc_remove_virtio_dev),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	rc = bdev_virtio_blk_dev_remove(req.name, spdk_rpc_remove_virtio_bdev_cb, request);
	if (rc == -ENODEV) {
		rc = bdev_virtio_scsi_dev_remove(req.name, spdk_rpc_remove_virtio_bdev_cb, request);
	}

	if (rc != 0) {
		goto invalid;
	}

	free(req.name);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free(req.name);
}
SPDK_RPC_REGISTER("remove_virtio_bdev", spdk_rpc_remove_virtio_bdev, SPDK_RPC_RUNTIME);

static void
spdk_rpc_get_virtio_scsi_devs(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_virtio_scsi_devs requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	bdev_virtio_scsi_dev_list(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_virtio_scsi_devs", spdk_rpc_get_virtio_scsi_devs, SPDK_RPC_RUNTIME)

struct rpc_construct_virtio_blk_dev {
	char *path;
	char *pci_address;
	char *name;
	uint32_t vq_count;
	uint32_t vq_size;
};

static void
free_rpc_construct_virtio_blk_dev(struct rpc_construct_virtio_blk_dev *req)
{
	free(req->path);
	free(req->pci_address);
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_construct_virtio_user_blk_dev[] = {
	{"path", offsetof(struct rpc_construct_virtio_blk_dev, path), spdk_json_decode_string },
	{"name", offsetof(struct rpc_construct_virtio_blk_dev, name), spdk_json_decode_string },
	{"vq_count", offsetof(struct rpc_construct_virtio_blk_dev, vq_count), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_construct_virtio_blk_dev, vq_size), spdk_json_decode_uint32, true },
};

static void
spdk_rpc_create_virtio_user_blk_bdev(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_construct_virtio_blk_dev req = {0};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc;

	req.pci_address = NULL;
	req.vq_count = SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT;
	req.vq_size = SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE;

	SPDK_WARNLOG("construct_virtio_user_blk_bdev command has been deprecated and will be removed "
		     "in the subsequent release. Please use construct_virtio_dev instead.\n");

	if (spdk_json_decode_object(params, rpc_construct_virtio_user_blk_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_user_blk_dev),
				    &req)) {
		free_rpc_construct_virtio_blk_dev(&req);
		rc = -EINVAL;
		goto invalid;
	}

	bdev = bdev_virtio_user_blk_dev_create(req.name, req.path, req.vq_count, req.vq_size);
	free_rpc_construct_virtio_blk_dev(&req);
	if (bdev == NULL) {
		rc = -EINVAL;
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("construct_virtio_user_blk_bdev", spdk_rpc_create_virtio_user_blk_bdev,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder rpc_construct_virtio_pci_blk_dev[] = {
	{"pci_address", offsetof(struct rpc_construct_virtio_blk_dev, pci_address), spdk_json_decode_string },
	{"name", offsetof(struct rpc_construct_virtio_blk_dev, name), spdk_json_decode_string },
};

static void
spdk_rpc_create_virtio_pci_blk_bdev(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_construct_virtio_blk_dev req = {0};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	struct spdk_pci_addr pci_addr;
	int rc;

	req.pci_address = NULL;

	SPDK_WARNLOG("construct_virtio_pci_blk_bdev command has been deprecated and will be removed "
		     "in the subsequent release. Please use construct_virtio_dev instead.\n");

	if (spdk_json_decode_object(params, rpc_construct_virtio_pci_blk_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_pci_blk_dev),
				    &req)) {
		free_rpc_construct_virtio_blk_dev(&req);
		rc = -EINVAL;
		goto invalid;
	}

	if (spdk_pci_addr_parse(&pci_addr, req.pci_address) != 0) {
		SPDK_ERRLOG("Invalid PCI address '%s'\n", req.pci_address);
		free_rpc_construct_virtio_blk_dev(&req);
		rc = -EINVAL;
		goto invalid;
	}

	bdev = bdev_virtio_pci_blk_dev_create(req.name, &pci_addr);
	free_rpc_construct_virtio_blk_dev(&req);
	if (bdev == NULL) {
		rc = -EINVAL;
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("construct_virtio_pci_blk_bdev", spdk_rpc_create_virtio_pci_blk_bdev,
		  SPDK_RPC_RUNTIME);

struct rpc_construct_virtio_dev {
	char *name;
	char *trtype;
	char *traddr;
	char *dev_type;
	uint32_t vq_count;
	uint32_t vq_size;
	struct spdk_jsonrpc_request *request;
};

static const struct spdk_json_object_decoder rpc_construct_virtio_dev[] = {
	{"name", offsetof(struct rpc_construct_virtio_dev, name), spdk_json_decode_string },
	{"trtype", offsetof(struct rpc_construct_virtio_dev, trtype), spdk_json_decode_string },
	{"traddr", offsetof(struct rpc_construct_virtio_dev, traddr), spdk_json_decode_string },
	{"dev_type", offsetof(struct rpc_construct_virtio_dev, dev_type), spdk_json_decode_string },
	{"vq_count", offsetof(struct rpc_construct_virtio_dev, vq_count), spdk_json_decode_uint32, true },
	{"vq_size", offsetof(struct rpc_construct_virtio_dev, vq_size), spdk_json_decode_uint32, true },
};

static void
free_rpc_construct_virtio_dev(struct rpc_construct_virtio_dev *req)
{
	free(req->name);
	free(req->trtype);
	free(req->traddr);
	free(req->dev_type);
	free(req);
}

static void
spdk_rpc_create_virtio_dev_cb(void *ctx, int result, struct spdk_bdev **bdevs, size_t cnt)
{
	struct rpc_construct_virtio_dev *req = ctx;
	struct spdk_json_write_ctx *w;
	size_t i;

	if (result) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-result));
		free_rpc_construct_virtio_dev(req);
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

	free_rpc_construct_virtio_dev(ctx);
}

static void
spdk_rpc_create_virtio_dev(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_construct_virtio_dev *req;
	struct spdk_bdev *bdev;
	struct spdk_pci_addr pci_addr;
	bool pci;
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("calloc() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_construct_virtio_dev,
				    SPDK_COUNTOF(rpc_construct_virtio_dev),
				    req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(EINVAL));
		goto invalid;
	}

	if (strcmp(req->trtype, "pci") == 0) {
		if (req->vq_count != 0 || req->vq_size != 0) {
			SPDK_ERRLOG("VQ count or size is not allowed for PCI transport type\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "vq_count or vq_size is not allowed for PCI transport type.");
			goto invalid;
		}

		if (spdk_pci_addr_parse(&pci_addr, req->traddr) != 0) {
			SPDK_ERRLOG("Invalid PCI address '%s'\n", req->traddr);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid PCI address '%s'", req->traddr);
			goto invalid;
		}

		pci = true;
	} else if (strcmp(req->trtype, "user") == 0) {
		req->vq_count = req->vq_count == 0 ? SPDK_VIRTIO_USER_DEFAULT_VQ_COUNT : req->vq_count;
		req->vq_size = req->vq_size == 0 ? SPDK_VIRTIO_USER_DEFAULT_QUEUE_SIZE : req->vq_size;
		pci = false;
	} else {
		SPDK_ERRLOG("Invalid trtype '%s'\n", req->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid trtype '%s'", req->trtype);
		goto invalid;
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
		spdk_rpc_create_virtio_dev_cb(req, rc, &bdev, bdev ? 1 : 0);
	} else if (strcmp(req->dev_type, "scsi") == 0) {
		if (pci) {
			rc = bdev_virtio_pci_scsi_dev_create(req->name, &pci_addr, spdk_rpc_create_virtio_dev_cb, req);
		} else {
			rc = bdev_virtio_user_scsi_dev_create(req->name, req->traddr, req->vq_count, req->vq_size,
							      spdk_rpc_create_virtio_dev_cb, req);
		}

		if (rc < 0) {
			/* In case of error callback is not called so do it manually to send result. */
			spdk_rpc_create_virtio_dev_cb(req, rc, NULL, 0);
		}
	} else {
		SPDK_ERRLOG("Invalid dev_type '%s'\n", req->dev_type);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid dev_type '%s'", req->dev_type);
		goto invalid;
	}

	return;
invalid:
	free_rpc_construct_virtio_dev(req);
}
SPDK_RPC_REGISTER("construct_virtio_dev", spdk_rpc_create_virtio_dev, SPDK_RPC_RUNTIME);
