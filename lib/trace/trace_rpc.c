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

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/trace.h"
#include "spdk/log.h"

struct rpc_tpoint_group {
	char *name;
};

static void
free_rpc_tpoint_group(struct rpc_tpoint_group *p)
{
	free(p->name);
}

static const struct spdk_json_object_decoder rpc_tpoint_group_decoders[] = {
	{"name", offsetof(struct rpc_tpoint_group, name), spdk_json_decode_string},
};

static void
rpc_trace_enable_tpoint_group(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};

	if (spdk_json_decode_object(params, rpc_tpoint_group_decoders,
				    SPDK_COUNTOF(rpc_tpoint_group_decoders), &req)) {
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	if (spdk_trace_enable_tpoint_group(req.name)) {
		goto invalid;
	}

	free_rpc_tpoint_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);
}
SPDK_RPC_REGISTER("trace_enable_tpoint_group", rpc_trace_enable_tpoint_group,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(trace_enable_tpoint_group, enable_tpoint_group)

static void
rpc_trace_disable_tpoint_group(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};

	if (spdk_json_decode_object(params, rpc_tpoint_group_decoders,
				    SPDK_COUNTOF(rpc_tpoint_group_decoders), &req)) {
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	if (spdk_trace_disable_tpoint_group(req.name)) {
		goto invalid;
	}

	free_rpc_tpoint_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);
}
SPDK_RPC_REGISTER("trace_disable_tpoint_group", rpc_trace_disable_tpoint_group,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(trace_disable_tpoint_group, disable_tpoint_group)

static void
rpc_trace_get_tpoint_group_mask(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	uint64_t tpoint_group_mask;
	char mask_str[7];
	bool enabled;
	struct spdk_json_write_ctx *w;
	struct spdk_trace_register_fn *register_fn;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "trace_get_tpoint_group_mask requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	tpoint_group_mask = spdk_trace_get_tpoint_group_mask();

	spdk_json_write_object_begin(w);

	snprintf(mask_str, sizeof(mask_str), "0x%" PRIx64, tpoint_group_mask);
	spdk_json_write_named_string(w, "tpoint_group_mask", mask_str);

	register_fn = spdk_trace_get_first_register_fn();
	while (register_fn) {
		enabled = spdk_trace_get_tpoint_mask(register_fn->tgroup_id) != 0;

		spdk_json_write_named_object_begin(w, register_fn->name);
		spdk_json_write_named_bool(w, "enabled", enabled);

		snprintf(mask_str, sizeof(mask_str), "0x%lx", (1UL << register_fn->tgroup_id));
		spdk_json_write_named_string(w, "mask", mask_str);
		spdk_json_write_object_end(w);

		register_fn = spdk_trace_get_next_register_fn(register_fn);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("trace_get_tpoint_group_mask", rpc_trace_get_tpoint_group_mask,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(trace_get_tpoint_group_mask, get_tpoint_group_mask)
