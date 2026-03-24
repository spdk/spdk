/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "vbdev_error.h"
#include "spdk_internal/rpc_autogen.h"

static int
rpc_error_bdev_decode_io_type(const struct spdk_json_val *val, void *out)
{
	uint32_t *io_type = out;

	if (spdk_json_strequal(val, "read") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_READ;
	} else if (spdk_json_strequal(val, "write") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_WRITE;
	} else if (spdk_json_strequal(val, "flush") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_FLUSH;
	} else if (spdk_json_strequal(val, "unmap") == true) {
		*io_type = SPDK_BDEV_IO_TYPE_UNMAP;
	} else if (spdk_json_strequal(val, "all") == true) {
		*io_type = 0xffffffff;
	} else if (spdk_json_strequal(val, "clear") == true) {
		*io_type = 0;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: io_type\n");
		return -EINVAL;
	}

	return 0;
}

static int
rpc_error_bdev_decode_error_type(const struct spdk_json_val *val, void *out)
{
	enum rpc_bdev_error_inject_error_type *error_type = out;

	if (spdk_json_strequal(val, "failure") == true) {
		*error_type = RPC_BDEV_ERROR_INJECT_ERROR_TYPE_FAILURE;
	} else if (spdk_json_strequal(val, "pending") == true) {
		*error_type = RPC_BDEV_ERROR_INJECT_ERROR_TYPE_PENDING;
	} else if (spdk_json_strequal(val, "corrupt_data") == true) {
		*error_type = RPC_BDEV_ERROR_INJECT_ERROR_TYPE_CORRUPT_DATA;
	} else if (spdk_json_strequal(val, "nomem") == true) {
		*error_type = RPC_BDEV_ERROR_INJECT_ERROR_TYPE_NOMEM;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: error_type\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_bdev_error_create_decoders[] = {
	{"base_name", offsetof(struct rpc_bdev_error_create_ctx, base_name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_bdev_error_create_ctx, uuid), spdk_json_decode_uuid, true},
};

static void
rpc_bdev_error_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_error_create_ctx req = {};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_error_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_error_create(req.base_name, &req.uuid);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_error_create(&req);
}
SPDK_RPC_REGISTER("bdev_error_create", rpc_bdev_error_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_error_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_error_delete_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_error_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_error_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_error_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_error_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	vbdev_error_delete(req.name, rpc_bdev_error_delete_cb, request);

cleanup:
	free_rpc_bdev_error_delete(&req);
}
SPDK_RPC_REGISTER("bdev_error_delete", rpc_bdev_error_delete, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_error_inject_error_decoders[] = {
	{"name", offsetof(struct rpc_bdev_error_inject_error_ctx, name), spdk_json_decode_string},
	{"io_type", offsetof(struct rpc_bdev_error_inject_error_ctx, io_type), rpc_error_bdev_decode_io_type},
	{"error_type", offsetof(struct rpc_bdev_error_inject_error_ctx, error_type), rpc_error_bdev_decode_error_type},
	{"num", offsetof(struct rpc_bdev_error_inject_error_ctx, num), spdk_json_decode_uint32, true},
	{"queue_depth", offsetof(struct rpc_bdev_error_inject_error_ctx, queue_depth), spdk_json_decode_uint64, true},
	{"corrupt_offset", offsetof(struct rpc_bdev_error_inject_error_ctx, corrupt_offset), spdk_json_decode_uint64, true},
	{"corrupt_value", offsetof(struct rpc_bdev_error_inject_error_ctx, corrupt_value), spdk_json_decode_uint8, true},
};

static void
rpc_bdev_error_inject_error(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_error_inject_error_ctx req = {.num = 1};
	struct vbdev_error_inject_opts opts;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_error_inject_error_decoders,
				    SPDK_COUNTOF(rpc_bdev_error_inject_error_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	opts.io_type = req.io_type;
	opts.error_type = req.error_type;
	opts.error_num = req.num;
	opts.error_qd = req.queue_depth;
	opts.corrupt_offset = req.corrupt_offset;
	opts.corrupt_value = req.corrupt_value;

	rc = vbdev_error_inject_error(req.name, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_error_inject_error(&req);
}
SPDK_RPC_REGISTER("bdev_error_inject_error", rpc_bdev_error_inject_error, SPDK_RPC_RUNTIME)
