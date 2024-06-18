/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/trace.h"
#include "spdk/log.h"
#include "trace_internal.h"

struct rpc_tpoint_group {
	char *name;
	uint64_t tpoint_mask;
};

static void
free_rpc_tpoint_group(struct rpc_tpoint_group *p)
{
	free(p->name);
}

static const struct spdk_json_object_decoder rpc_tpoint_mask_decoders[] = {
	{"name", offsetof(struct rpc_tpoint_group, name), spdk_json_decode_string},
	{"tpoint_mask", offsetof(struct rpc_tpoint_group, tpoint_mask), spdk_json_decode_uint64, true},
};

static void
rpc_trace_set_tpoint_mask(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};
	uint64_t tpoint_group_mask = 0;

	if (spdk_json_decode_object(params, rpc_tpoint_mask_decoders,
				    SPDK_COUNTOF(rpc_tpoint_mask_decoders), &req)) {
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(req.name);
	if (tpoint_group_mask == 0) {
		goto invalid;
	}

	spdk_trace_set_tpoints(spdk_u64log2(tpoint_group_mask), req.tpoint_mask);

	free_rpc_tpoint_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);
}
SPDK_RPC_REGISTER("trace_set_tpoint_mask", rpc_trace_set_tpoint_mask,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

static void
rpc_trace_clear_tpoint_mask(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_tpoint_group req = {};
	uint64_t tpoint_group_mask = 0;

	if (spdk_json_decode_object(params, rpc_tpoint_mask_decoders,
				    SPDK_COUNTOF(rpc_tpoint_mask_decoders), &req)) {
		SPDK_DEBUGLOG(trace, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_DEBUGLOG(trace, "flag was NULL\n");
		goto invalid;
	}

	tpoint_group_mask = spdk_trace_create_tpoint_group_mask(req.name);
	if (tpoint_group_mask == 0) {
		goto invalid;
	}

	spdk_trace_clear_tpoints(spdk_u64log2(tpoint_group_mask), req.tpoint_mask);

	free_rpc_tpoint_group(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_tpoint_group(&req);
}
SPDK_RPC_REGISTER("trace_clear_tpoint_mask", rpc_trace_clear_tpoint_mask,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

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

static void
rpc_trace_get_tpoint_group_mask(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	uint64_t tpoint_group_mask;
	char mask_str[20];
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

static void
rpc_trace_get_info(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	char shm_path[128];
	uint64_t tpoint_group_mask;
	uint64_t tpoint_mask;
	char tpoint_mask_str[20];
	char mask_str[20];
	struct spdk_json_write_ctx *w;
	struct spdk_trace_register_fn *register_fn;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "trace_get_info requires no parameters");
		return;
	}

	snprintf(shm_path, sizeof(shm_path), "/dev/shm%s", trace_get_shm_name());
	tpoint_group_mask = spdk_trace_get_tpoint_group_mask();

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "tpoint_shm_path", shm_path);

	snprintf(mask_str, sizeof(mask_str), "0x%" PRIx64, tpoint_group_mask);
	spdk_json_write_named_string(w, "tpoint_group_mask", mask_str);

	register_fn = spdk_trace_get_first_register_fn();
	while (register_fn) {

		tpoint_mask = spdk_trace_get_tpoint_mask(register_fn->tgroup_id);

		spdk_json_write_named_object_begin(w, register_fn->name);
		snprintf(mask_str, sizeof(mask_str), "0x%lx", (1UL << register_fn->tgroup_id));
		spdk_json_write_named_string(w, "mask", mask_str);
		snprintf(tpoint_mask_str, sizeof(tpoint_mask_str), "0x%lx", tpoint_mask);
		spdk_json_write_named_string(w, "tpoint_mask", tpoint_mask_str);
		spdk_json_write_object_end(w);

		register_fn = spdk_trace_get_next_register_fn(register_fn);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("trace_get_info", rpc_trace_get_info,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
