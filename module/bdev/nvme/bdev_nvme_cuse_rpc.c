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
