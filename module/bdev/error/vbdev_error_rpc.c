/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "vbdev_error.h"
#include "spdk_internal/rpc_autogen.h"

static void
rpc_bdev_error_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_error_create_ctx req = {};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_error_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_error_create(req.base_name, &req.uuid);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_error_create(&req);
}
SPDK_RPC_REGISTER("bdev_error_create", rpc_bdev_error_create, SPDK_RPC_RUNTIME)

static void
rpc_bdev_error_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_error_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_error_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_error_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	vbdev_error_delete(req.name, rpc_bdev_error_delete_cb, request);

cleanup:
	free_rpc_bdev_error_delete(&req);
}
SPDK_RPC_REGISTER("bdev_error_delete", rpc_bdev_error_delete, SPDK_RPC_RUNTIME)

static void
rpc_bdev_error_inject_error(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_error_inject_error_ctx req = {.num = 1};
	struct vbdev_error_inject_opts opts;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_error_inject_error_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_inject_error_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if ((req.error_type == RPC_BDEV_ERROR_INJECT_ERROR_TYPE_NVME_FAILURE &&
	     (req.nvme_sct == 0 && req.nvme_sc == 0)) ||
	    (req.error_type != RPC_BDEV_ERROR_INJECT_ERROR_TYPE_NVME_FAILURE &&
	     (req.nvme_sct != 0 || req.nvme_sc != 0))) {
		SPDK_ERRLOG("nvme_sct or nvme_sc should be specified for NVMe error injection.\n");
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "nvme_sct or nvme_sc should be specified for NVMe error injection");
		goto cleanup;
	}

	opts.io_type = req.io_type;
	opts.error_type = req.error_type;
	opts.error_num = req.num;
	opts.error_qd = req.queue_depth;
	opts.corrupt_offset = req.corrupt_offset;
	opts.corrupt_value = req.corrupt_value;
	opts.nvme_sct = req.nvme_sct;
	opts.nvme_sc = req.nvme_sc;

	rc = vbdev_error_inject_error(req.name, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_error_inject_error(&req);
}
SPDK_RPC_REGISTER("bdev_error_inject_error", rpc_bdev_error_inject_error, SPDK_RPC_RUNTIME)

static void
rpc_bdev_error_resume_pending(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_error_resume_pending_ctx req = {NULL};
	struct spdk_bdev *vbdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_error_resume_pending_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_resume_pending_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	vbdev = spdk_bdev_get_by_name(req.name);
	if (vbdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = vbdev_error_resume_pending(vbdev);

	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_error_resume_pending(&req);
}
SPDK_RPC_REGISTER("bdev_error_resume_pending", rpc_bdev_error_resume_pending,
		  SPDK_RPC_RUNTIME)
