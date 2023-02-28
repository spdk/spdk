/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/vmd.h"

#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/log.h"
#include "event_vmd.h"

static void
rpc_vmd_enable(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	vmd_subsystem_enable();

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("vmd_enable", rpc_vmd_enable, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(vmd_enable, enable_vmd)

struct rpc_vmd_remove_device {
	char *addr;
};

static const struct spdk_json_object_decoder rpc_vmd_remove_device_decoders[] = {
	{"addr", offsetof(struct rpc_vmd_remove_device, addr), spdk_json_decode_string},
};

static void
rpc_vmd_remove_device(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_vmd_remove_device req = {};
	struct spdk_pci_addr addr;
	int rc;

	if (!vmd_subsystem_is_enabled()) {
		spdk_jsonrpc_send_error_response(request, -EPERM, "VMD subsystem is disabled");
		return;
	}

	rc = spdk_json_decode_object(params, rpc_vmd_remove_device_decoders,
				     SPDK_COUNTOF(rpc_vmd_remove_device_decoders),
				     &req);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_pci_addr_parse(&addr, req.addr);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed to parse PCI address");
		goto out;
	}

	rc = spdk_vmd_remove_device(&addr);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	free(req.addr);
}
SPDK_RPC_REGISTER("vmd_remove_device", rpc_vmd_remove_device, SPDK_RPC_RUNTIME)

static void
rpc_vmd_rescan(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	int rc;

	if (!vmd_subsystem_is_enabled()) {
		spdk_jsonrpc_send_error_response(request, -EPERM, "VMD subsystem is disabled");
		return;
	}

	rc = spdk_vmd_rescan();
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "count", (uint32_t)rc);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("vmd_rescan", rpc_vmd_rescan, SPDK_RPC_RUNTIME)
