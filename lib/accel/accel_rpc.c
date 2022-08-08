/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "accel_internal.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"

const char *g_opcode_strings[ACCEL_OPC_LAST] = {
	"copy", "fill", "dualcast", "compare", "crc32c", "copy_crc32c",
	"compress", "decompress"
};

static int
_get_opc_name(enum accel_opcode opcode, const char **opcode_name)
{
	int rc = 0;

	if (opcode < ACCEL_OPC_LAST) {
		*opcode_name = g_opcode_strings[opcode];
	} else {
		/* invalid opcode */
		rc = -EINVAL;
	}

	return rc;
}

static void
rpc_accel_get_opc_assignments(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	enum accel_opcode opcode;
	const char *name, *engine_name;
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "accel_get_opc_assignments requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	for (opcode = 0; opcode < ACCEL_OPC_LAST; opcode++) {
		rc = _get_opc_name(opcode, &name);
		if (rc == 0) {
			rc = spdk_accel_get_opc_engine_name(opcode, &engine_name);
			if (rc != 0) {
				/* This isn't fatal but throw an informational message if we
				 * cant get an engine name right now */
				SPDK_NOTICELOG("FYI error (%d) getting engine name.\n", rc);
			}
			spdk_json_write_named_string(w, name, engine_name);
		} else {
			/* this should never happen */
			SPDK_ERRLOG("Invalid opcode (%d)).\n", opcode);
			assert(0);
		}
	}
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("accel_get_opc_assignments", rpc_accel_get_opc_assignments,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

static void
rpc_dump_engine_info(struct engine_info *info)
{
	struct spdk_json_write_ctx *w = info->w;
	const char *name;
	uint32_t i;
	int rc;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "engine", info->name);
	spdk_json_write_named_array_begin(w, "suppoerted ops");

	for (i = 0; i < info->num_ops; i++) {
		rc = _get_opc_name(i, &name);
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
rpc_accel_get_engine_info(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct engine_info info;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "accel_get_engine_info requires no parameters");
		return;
	}

	info.w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(info.w);

	_accel_for_each_engine(&info, rpc_dump_engine_info);

	spdk_json_write_array_end(info.w);
	spdk_jsonrpc_end_result(request, info.w);
}
SPDK_RPC_REGISTER("accel_get_engine_info", rpc_accel_get_engine_info,
		  SPDK_RPC_RUNTIME)

struct rpc_accel_assign_opc {
	char *opname;
	char *engine;
};

static const struct spdk_json_object_decoder rpc_accel_assign_opc_decoders[] = {
	{"opname", offsetof(struct rpc_accel_assign_opc, opname), spdk_json_decode_string},
	{"engine", offsetof(struct rpc_accel_assign_opc, engine), spdk_json_decode_string},
};

static void
free_accel_assign_opc(struct rpc_accel_assign_opc *r)
{
	free(r->opname);
	free(r->engine);
}

static void
rpc_accel_assign_opc(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_accel_assign_opc req = {};
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
		if (strcmp(g_opcode_strings[opcode], req.opname) == 0) {
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

	rc = spdk_accel_assign_opc(opcode, req.engine);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "error assigning opcode");
		goto cleanup;
	}

	SPDK_NOTICELOG("Operation %s will be assigned to engine %s\n", req.opname, req.engine);
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_accel_assign_opc(&req);

}
SPDK_RPC_REGISTER("accel_assign_opc", rpc_accel_assign_opc, SPDK_RPC_STARTUP)
