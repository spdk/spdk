/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_kvmalloc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/rpc_autogen.h"

static void
rpc_bdev_kvmalloc_create(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_kvmalloc_create_ctx req = {};
	struct kvmalloc_bdev_opts opts = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;

	req.numa_id = SPDK_ENV_NUMA_ID_ANY;

	if (params && spdk_json_decode_object(params, rpc_bdev_kvmalloc_create_decoders,
					      SPDK_COUNTOF(rpc_bdev_kvmalloc_create_decoders),
					      &req)) {
		SPDK_DEBUGLOG(bdev_kvmalloc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.name = req.name;
	opts.uuid = req.uuid;
	opts.max_key_size = req.max_key_size;
	opts.max_value_size = req.max_value_size;
	opts.optimal_value_granularity = req.optimal_value_granularity;
	opts.numa_id = req.numa_id;

	rc = create_kvmalloc_disk(&bdev, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	free_rpc_bdev_kvmalloc_create(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

cleanup:
	free_rpc_bdev_kvmalloc_create(&req);
}
SPDK_RPC_REGISTER("bdev_kvmalloc_create", rpc_bdev_kvmalloc_create, SPDK_RPC_RUNTIME)

static void
rpc_bdev_kvmalloc_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_kvmalloc_delete(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_kvmalloc_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_kvmalloc_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_kvmalloc_delete_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_kvmalloc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_kvmalloc_disk(req.name, rpc_bdev_kvmalloc_delete_cb, request);

cleanup:
	free_rpc_bdev_kvmalloc_delete(&req);
}
SPDK_RPC_REGISTER("bdev_kvmalloc_delete", rpc_bdev_kvmalloc_delete, SPDK_RPC_RUNTIME)
