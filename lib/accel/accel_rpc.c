/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "accel_internal.h"
#include "spdk_internal/accel_module.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/util.h"

static void
rpc_accel_get_opc_assignments(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	enum accel_opcode opcode;
	const char *name, *module_name = NULL;
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "accel_get_opc_assignments requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	for (opcode = 0; opcode < ACCEL_OPC_LAST; opcode++) {
		rc = _accel_get_opc_name(opcode, &name);
		if (rc == 0) {
			rc = spdk_accel_get_opc_module_name(opcode, &module_name);
			if (rc == 0) {
				spdk_json_write_named_string(w, name, module_name);
			} else {
				/* This isn't fatal but throw an informational message if we
				 * cant get an module name right now */
				SPDK_NOTICELOG("FYI error (%d) getting module name.\n", rc);
			}
		} else {
			/* this should never happen */
			SPDK_ERRLOG("Invalid opcode (%d)).\n", opcode);
			assert(0);
		}
	}
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("accel_get_opc_assignments", rpc_accel_get_opc_assignments, SPDK_RPC_RUNTIME)

static void
rpc_dump_module_info(struct module_info *info)
{
	struct spdk_json_write_ctx *w = info->w;
	const char *name;
	uint32_t i;
	int rc;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "module", info->name);
	spdk_json_write_named_array_begin(w, "supported ops");

	for (i = 0; i < info->num_ops; i++) {
		rc = _accel_get_opc_name(i, &name);
		if (rc == 0) {
			spdk_json_write_string(w, name);
		} else {
			/* this should never happen */
			SPDK_ERRLOG("Invalid opcode (%d)).\n", i);
			assert(0);
		}
	}

	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}

static void
rpc_accel_get_module_info(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct module_info info;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "accel_get_module_info requires no parameters");
		return;
	}

	info.w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(info.w);

	_accel_for_each_module(&info, rpc_dump_module_info);

	spdk_json_write_array_end(info.w);
	spdk_jsonrpc_end_result(request, info.w);
}
SPDK_RPC_REGISTER("accel_get_module_info", rpc_accel_get_module_info, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(accel_get_module_info, accel_get_engine_info)

struct rpc_accel_assign_opc {
	char *opname;
	char *module;
};

static const struct spdk_json_object_decoder rpc_accel_assign_opc_decoders[] = {
	{"opname", offsetof(struct rpc_accel_assign_opc, opname), spdk_json_decode_string},
	{"module", offsetof(struct rpc_accel_assign_opc, module), spdk_json_decode_string},
};

static void
free_accel_assign_opc(struct rpc_accel_assign_opc *r)
{
	free(r->opname);
	free(r->module);
}

static void
rpc_accel_assign_opc(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_accel_assign_opc req = {};
	const char *opcode_str;
	enum accel_opcode opcode;
	bool found = false;
	int rc;

	if (spdk_json_decode_object(params, rpc_accel_assign_opc_decoders,
				    SPDK_COUNTOF(rpc_accel_assign_opc_decoders),
				    &req)) {
		SPDK_DEBUGLOG(accel, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	for (opcode = 0; opcode < ACCEL_OPC_LAST; opcode++) {
		rc = _accel_get_opc_name(opcode, &opcode_str);
		assert(!rc);
		if (strcmp(opcode_str, req.opname) == 0) {
			found = true;
			break;
		}
	}

	if (found == false) {
		SPDK_DEBUGLOG(accel, "Invalid operation name\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_accel_assign_opc(opcode, req.module);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "error assigning opcode");
		goto cleanup;
	}

	SPDK_NOTICELOG("Operation %s will be assigned to module %s\n", req.opname, req.module);
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_accel_assign_opc(&req);

}
SPDK_RPC_REGISTER("accel_assign_opc", rpc_accel_assign_opc, SPDK_RPC_STARTUP)

struct rpc_accel_crypto_key_create {
	struct spdk_accel_crypto_key_create_param param;
};

static const struct spdk_json_object_decoder rpc_accel_dek_create_decoders[] = {
	{"cipher", offsetof(struct rpc_accel_crypto_key_create, param.cipher), spdk_json_decode_string},
	{"key", offsetof(struct rpc_accel_crypto_key_create, param.hex_key),   spdk_json_decode_string},
	{"key2", offsetof(struct rpc_accel_crypto_key_create, param.hex_key2), spdk_json_decode_string, true},
	{"name", offsetof(struct rpc_accel_crypto_key_create, param.key_name), spdk_json_decode_string},
};

static void
rpc_accel_crypto_key_create(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_accel_crypto_key_create req = {};
	size_t key_size;
	int rc;

	if (spdk_json_decode_object(params, rpc_accel_dek_create_decoders,
				    SPDK_COUNTOF(rpc_accel_dek_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_accel_crypto_key_create(&req.param);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "failed to create DEK, rc %d", rc);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}

cleanup:
	free(req.param.cipher);
	if (req.param.hex_key) {
		key_size = strnlen(req.param.hex_key, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		spdk_memset_s(req.param.hex_key, key_size, 0, key_size);
		free(req.param.hex_key);
	}
	if (req.param.hex_key2) {
		key_size = strnlen(req.param.hex_key2, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		spdk_memset_s(req.param.hex_key2, key_size, 0, key_size);
		free(req.param.hex_key2);
	}
	free(req.param.key_name);
}
SPDK_RPC_REGISTER("accel_crypto_key_create", rpc_accel_crypto_key_create, SPDK_RPC_RUNTIME)

struct rpc_accel_crypto_keys_get_ctx {
	char *key_name;
};

static const struct spdk_json_object_decoder rpc_accel_crypto_keys_get_decoders[] = {
	{"key_name", offsetof(struct rpc_accel_crypto_keys_get_ctx, key_name), spdk_json_decode_string, true},
};

static void
rpc_accel_crypto_keys_get(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_accel_crypto_keys_get_ctx req = {};
	struct spdk_accel_crypto_key *key = NULL;
	struct spdk_json_write_ctx *w;

	if (params && spdk_json_decode_object(params, rpc_accel_crypto_keys_get_decoders,
					      SPDK_COUNTOF(rpc_accel_crypto_keys_get_decoders),
					      &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		free(req.key_name);
		return;
	}

	if (req.key_name) {
		key = spdk_accel_crypto_key_get(req.key_name);
		free(req.key_name);
		if (!key) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "key was not found\n");
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (key) {
		_accel_crypto_key_dump_param(w, key);
	} else {
		_accel_crypto_keys_dump_param(w);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("accel_crypto_keys_get", rpc_accel_crypto_keys_get, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_accel_crypto_key_destroy_decoders[] = {
	{"key_name", offsetof(struct rpc_accel_crypto_keys_get_ctx, key_name), spdk_json_decode_string},
};

static void
rpc_accel_crypto_key_destroy(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_accel_crypto_keys_get_ctx req = {};
	struct spdk_accel_crypto_key *key = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_accel_crypto_key_destroy_decoders,
				    SPDK_COUNTOF(rpc_accel_crypto_key_destroy_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		free(req.key_name);
		return;
	}

	key = spdk_accel_crypto_key_get(req.key_name);
	if (!key) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No key object found");
		free(req.key_name);
		return;

	}
	rc = spdk_accel_crypto_key_destroy(key);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to destroy key, rc %d\n", rc);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}

	free(req.key_name);
}
SPDK_RPC_REGISTER("accel_crypto_key_destroy", rpc_accel_crypto_key_destroy, SPDK_RPC_RUNTIME)

struct rpc_accel_set_driver {
	char *name;
};

static const struct spdk_json_object_decoder rpc_accel_set_driver_decoders[] = {
	{"name", offsetof(struct rpc_accel_set_driver, name), spdk_json_decode_string},
};

static void
free_rpc_accel_set_driver(struct rpc_accel_set_driver *r)
{
	free(r->name);
}

static void
rpc_accel_set_driver(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_accel_set_driver req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_accel_set_driver_decoders,
				    SPDK_COUNTOF(rpc_accel_set_driver_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	rc = spdk_accel_set_driver(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	SPDK_NOTICELOG("Using accel driver: %s\n", req.name);
	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_accel_set_driver(&req);
}
SPDK_RPC_REGISTER("accel_set_driver", rpc_accel_set_driver, SPDK_RPC_STARTUP)
