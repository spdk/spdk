/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "ublk_internal.h"

struct rpc_ublk_create_target {
	char		*cpumask;
};

static const struct spdk_json_object_decoder rpc_ublk_create_target_decoders[] = {
	{"cpumask", offsetof(struct rpc_ublk_create_target, cpumask), spdk_json_decode_string, true},
};

static void
free_rpc_ublk_create_target(struct rpc_ublk_create_target *req)
{
	free(req->cpumask);
}

static void
rpc_ublk_create_target(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc = 0;
	struct rpc_ublk_create_target req = {};

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_ublk_create_target_decoders,
					    SPDK_COUNTOF(rpc_ublk_create_target_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}
	rc = ublk_create_target(req.cpumask);
	if (rc != 0) {
		goto invalid;
	}
	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_ublk_create_target(&req);
	return;
invalid:
	SPDK_ERRLOG("Can't create ublk target: %s\n", spdk_strerror(-rc));
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
	free_rpc_ublk_create_target(&req);
}
SPDK_RPC_REGISTER("ublk_create_target", rpc_ublk_create_target, SPDK_RPC_RUNTIME)

static void
ublk_destroy_target_done(void *arg)
{
	struct spdk_jsonrpc_request *req = arg;

	spdk_jsonrpc_send_bool_response(req, true);
	SPDK_NOTICELOG("ublk target has been destroyed\n");
}

static void
rpc_ublk_destroy_target(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc = 0;

	rc = ublk_destroy_target(ublk_destroy_target_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		SPDK_ERRLOG("Can't destroy ublk target: %s\n", spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("ublk_destroy_target", rpc_ublk_destroy_target, SPDK_RPC_RUNTIME)
