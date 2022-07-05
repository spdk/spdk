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
SPDK_RPC_REGISTER("enable_vmd", rpc_vmd_enable, SPDK_RPC_STARTUP)

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
