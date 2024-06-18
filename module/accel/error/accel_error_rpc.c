/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2023 Intel Corporation. All rights reserved.
 */

#include "accel_error.h"
#include "spdk/accel.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

static int
rpc_accel_error_decode_opcode(const struct spdk_json_val *val, void *out)
{
	enum spdk_accel_opcode *opcode = out;
	char *opstr = NULL;
	int i, rc;

	rc = spdk_json_decode_string(val, &opstr);
	if (rc != 0) {
		return rc;
	}

	rc = -EINVAL;
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; ++i) {
		if (strcmp(spdk_accel_get_opcode_name((enum spdk_accel_opcode)i), opstr) == 0) {
			*opcode = (enum spdk_accel_opcode)i;
			rc = 0;
			break;
		}
	}

	free(opstr);

	return rc;
}

static int
rpc_accel_error_decode_type(const struct spdk_json_val *val, void *out)
{
	enum accel_error_inject_type *type = out;
	char *typestr = NULL;
	int i, rc;

	rc = spdk_json_decode_string(val, &typestr);
	if (rc != 0) {
		return rc;
	}

	rc = -EINVAL;
	for (i = 0; i < ACCEL_ERROR_INJECT_MAX; ++i) {
		if (strcmp(accel_error_get_type_name(i), typestr) == 0) {
			*type = (enum accel_error_inject_type)i;
			rc = 0;
			break;
		}
	}

	free(typestr);

	return rc;
}

static const struct spdk_json_object_decoder rpc_accel_error_inject_error_decoders[] = {
	{"opcode", offsetof(struct accel_error_inject_opts, opcode), rpc_accel_error_decode_opcode},
	{"type", offsetof(struct accel_error_inject_opts, type), rpc_accel_error_decode_type},
	{"count", offsetof(struct accel_error_inject_opts, count), spdk_json_decode_uint64, true},
	{"interval", offsetof(struct accel_error_inject_opts, interval), spdk_json_decode_uint64, true},
	{"errcode", offsetof(struct accel_error_inject_opts, errcode), spdk_json_decode_int32, true},
};

static void
rpc_accel_error_inject_error(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct accel_error_inject_opts opts = {.count = UINT64_MAX};
	int rc;

	rc = spdk_json_decode_object(params, rpc_accel_error_inject_error_decoders,
				     SPDK_COUNTOF(rpc_accel_error_inject_error_decoders), &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	rc = accel_error_inject_error(&opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("accel_error_inject_error", rpc_accel_error_inject_error, SPDK_RPC_RUNTIME)
