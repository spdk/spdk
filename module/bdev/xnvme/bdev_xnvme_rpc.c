/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Samsung Electronics Co., Ltd.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 */

#include "bdev_xnvme.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* Structure to hold the parameters for this RPC method. */
struct rpc_create_xnvme {
	char *name;
	char *filename;
	char *io_mechanism;
	bool conserve_cpu;
};

/* Free the allocated memory resource after the RPC handling. */
static void
free_rpc_create_xnvme(struct rpc_create_xnvme *r)
{
	free(r->name);
	free(r->filename);
	free(r->io_mechanism);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_create_xnvme_decoders[] = {
	{"name", offsetof(struct rpc_create_xnvme, name), spdk_json_decode_string},
	{"filename", offsetof(struct rpc_create_xnvme, filename), spdk_json_decode_string},
	{"io_mechanism", offsetof(struct rpc_create_xnvme, io_mechanism), spdk_json_decode_string},
	{"conserve_cpu", offsetof(struct rpc_create_xnvme, conserve_cpu), spdk_json_decode_bool, true},
};

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

/* Decode the parameters for this RPC method and properly create the xnvme
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_xnvme_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_create_xnvme req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_create_xnvme_decoders,
				    SPDK_COUNTOF(rpc_create_xnvme_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = create_xnvme_bdev(req.name, req.filename, req.io_mechanism, req.conserve_cpu);
	if (!bdev) {
		SPDK_ERRLOG("Unable to create xNVMe bdev from file %s\n", req.filename);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create xNVMe bdev.");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_create_xnvme(&req);
}
SPDK_RPC_REGISTER("bdev_xnvme_create", rpc_bdev_xnvme_create, SPDK_RPC_RUNTIME)

struct rpc_delete_xnvme {
	char *name;
};

static void
free_rpc_delete_xnvme(struct rpc_delete_xnvme *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_xnvme_decoders[] = {
	{"name", offsetof(struct rpc_delete_xnvme, name), spdk_json_decode_string},
};

static void
_rpc_bdev_xnvme_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_xnvme_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_xnvme req = {NULL};
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_xnvme_decoders,
				    SPDK_COUNTOF(rpc_delete_xnvme_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_open_ext(req.name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc == 0) {
		bdev = spdk_bdev_desc_get_bdev(desc);
		spdk_bdev_close(desc);
	} else {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	delete_xnvme_bdev(bdev, _rpc_bdev_xnvme_delete_cb, request);

cleanup:
	free_rpc_delete_xnvme(&req);
}
SPDK_RPC_REGISTER("bdev_xnvme_delete", rpc_bdev_xnvme_delete, SPDK_RPC_RUNTIME)
