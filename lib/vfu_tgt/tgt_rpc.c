/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/thread.h"

#include "tgt_internal.h"

struct rpc_set_vfu_path {
	char		*path;
};

static const struct spdk_json_object_decoder rpc_set_vfu_path_decode[] = {
	{"path", offsetof(struct rpc_set_vfu_path, path), spdk_json_decode_string }
};

static void
free_rpc_set_vfu_path(struct rpc_set_vfu_path *req)
{
	free(req->path);
}

static void
rpc_vfu_set_base_path(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_set_vfu_path req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_set_vfu_path_decode,
				    SPDK_COUNTOF(rpc_set_vfu_path_decode),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vfu_set_socket_path(req.path);
	if (rc < 0) {
		goto invalid;
	}
	free_rpc_set_vfu_path(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_set_vfu_path(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_tgt_set_base_path", rpc_vfu_set_base_path,
		  SPDK_RPC_RUNTIME)
