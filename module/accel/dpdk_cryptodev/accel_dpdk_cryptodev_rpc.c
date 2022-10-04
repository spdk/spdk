/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "accel_dpdk_cryptodev.h"

#include "spdk/rpc.h"
#include "spdk/util.h"

static void
rpc_dpdk_cryptodev_scan_accel_module(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters expected");
		return;
	}

	accel_dpdk_cryptodev_enable();
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("dpdk_cryptodev_scan_accel_module", rpc_dpdk_cryptodev_scan_accel_module,
		  SPDK_RPC_STARTUP)

struct rpc_set_driver {
	char *driver_name;
};

static const struct spdk_json_object_decoder rpc_set_driver_decoders[] = {
	{"driver_name", offsetof(struct rpc_set_driver, driver_name), spdk_json_decode_string},
};

static void
rpc_dpdk_cryptodev_set_driver(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_set_driver req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_set_driver_decoders,
				    SPDK_COUNTOF(rpc_set_driver_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = accel_dpdk_cryptodev_set_driver(req.driver_name);
	free(req.driver_name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "incorrect driver name");
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}
SPDK_RPC_REGISTER("dpdk_cryptodev_set_driver", rpc_dpdk_cryptodev_set_driver, SPDK_RPC_STARTUP)

static void
rpc_dpdk_cryptodev_get_driver(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	const char *driver_name;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters expected");
		return;
	}

	driver_name = accel_dpdk_cryptodev_get_driver();
	assert(driver_name);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, driver_name);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("dpdk_cryptodev_get_driver", rpc_dpdk_cryptodev_get_driver,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
