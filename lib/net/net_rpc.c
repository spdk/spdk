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

#include "spdk/rpc.h"
#include "spdk/net.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

struct rpc_ip_address {
	int32_t ifc_index;
	char *ip_address;
};

static void
free_rpc_ip_address(struct rpc_ip_address *req)
{
	free(req->ip_address);
}

static const struct spdk_json_object_decoder rpc_ip_address_decoders[] = {
	{"ifc_index", offsetof(struct rpc_ip_address, ifc_index), spdk_json_decode_int32},
	{"ip_address", offsetof(struct rpc_ip_address, ip_address), spdk_json_decode_string},
};

static void
spdk_rpc_add_ip_address(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_ip_address req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_ip_address_decoders,
				    SPDK_COUNTOF(rpc_ip_address_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_NET, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (spdk_interface_add_ip_address(req.ifc_index, req.ip_address)) {
		goto invalid;
	}

	free_rpc_ip_address(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_ip_address(&req);
}
SPDK_RPC_REGISTER("add_ip_address", spdk_rpc_add_ip_address)

static void
spdk_rpc_delete_ip_address(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_ip_address req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_ip_address_decoders,
				    SPDK_COUNTOF(rpc_ip_address_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_NET, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (spdk_interface_delete_ip_address(req.ifc_index, req.ip_address)) {
		goto invalid;
	}

	free_rpc_ip_address(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_ip_address(&req);
}
SPDK_RPC_REGISTER("delete_ip_address", spdk_rpc_delete_ip_address)

static void
spdk_rpc_get_interfaces(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	TAILQ_HEAD(, spdk_interface) *interface_head = spdk_interface_get_list();
	struct spdk_interface *ifc;
	char *ip_address;
	struct in_addr inaddr;
	uint32_t i;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_interfaces requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(ifc, interface_head, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_name(w, "name");
		spdk_json_write_string(w, ifc->name);

		spdk_json_write_name(w, "ifc_index");
		spdk_json_write_int32(w, ifc->index);

		spdk_json_write_name(w, "ip_addr");
		spdk_json_write_array_begin(w);
		for (i = 0; i < ifc->num_ip_addresses; i++) {
			memcpy(&inaddr, &ifc->ip_address[i], sizeof(uint32_t));
			ip_address = inet_ntoa(inaddr);
			spdk_json_write_string(w, ip_address);
		}
		spdk_json_write_array_end(w);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_interfaces", spdk_rpc_get_interfaces)

SPDK_LOG_REGISTER_TRACE_FLAG("net", SPDK_TRACE_NET)
