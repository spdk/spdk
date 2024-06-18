/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "bdev_nvme.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/nvme.h"

#include "spdk/log.h"

struct rpc_nvme_cuse_register {
	char *name;
};

static void
free_rpc_nvme_cuse_register(struct rpc_nvme_cuse_register *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_nvme_cuse_register_decoders[] = {
	{"name", offsetof(struct rpc_nvme_cuse_register, name), spdk_json_decode_string},
};

static void
rpc_nvme_cuse_register(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_nvme_cuse_register req = {};
	struct nvme_ctrlr *bdev_ctrlr = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvme_cuse_register_decoders,
				    SPDK_COUNTOF(rpc_nvme_cuse_register_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_ctrlr = nvme_ctrlr_get_by_name(req.name);
	if (!bdev_ctrlr) {
		SPDK_ERRLOG("No such controller\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = spdk_nvme_cuse_register(bdev_ctrlr->ctrlr);
	if (rc) {
		SPDK_ERRLOG("Failed to register CUSE devices: %s\n", spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_nvme_cuse_register(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_cuse_register", rpc_nvme_cuse_register, SPDK_RPC_RUNTIME)

struct rpc_nvme_cuse_unregister {
	char *name;
};

static void
free_rpc_nvme_cuse_unregister(struct rpc_nvme_cuse_unregister *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_nvme_cuse_unregister_decoders[] = {
	{"name", offsetof(struct rpc_nvme_cuse_unregister, name), spdk_json_decode_string, true},
};

static void
rpc_nvme_cuse_unregister(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_nvme_cuse_unregister req = {};
	struct nvme_ctrlr *bdev_ctrlr = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_nvme_cuse_unregister_decoders,
				    SPDK_COUNTOF(rpc_nvme_cuse_unregister_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_ctrlr = nvme_ctrlr_get_by_name(req.name);
	if (!bdev_ctrlr) {
		SPDK_ERRLOG("No such controller\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = spdk_nvme_cuse_unregister(bdev_ctrlr->ctrlr);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_nvme_cuse_unregister(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_cuse_unregister", rpc_nvme_cuse_unregister, SPDK_RPC_RUNTIME)
