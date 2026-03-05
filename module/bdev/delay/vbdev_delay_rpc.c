/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "vbdev_delay.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/assert.h"
#include "spdk_internal/rpc_autogen.h"

static const struct spdk_json_object_decoder rpc_bdev_delay_update_latency_decoders[] = {
	{"delay_bdev_name", offsetof(struct rpc_bdev_delay_update_latency_ctx, delay_bdev_name), spdk_json_decode_string},
	{"latency_type", offsetof(struct rpc_bdev_delay_update_latency_ctx, latency_type), spdk_json_decode_string},
	{"latency_us", offsetof(struct rpc_bdev_delay_update_latency_ctx, latency_us), spdk_json_decode_uint64}
};

static void
rpc_bdev_delay_update_latency(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_delay_update_latency_ctx req = {};
	enum delay_io_type latency_type;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_delay_update_latency_decoders,
				    SPDK_COUNTOF(rpc_bdev_delay_update_latency_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_delay, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (!strncmp(req.latency_type, "avg_read", 9)) {
		latency_type = DELAY_AVG_READ;
	} else if (!strncmp(req.latency_type, "p99_read", 9)) {
		latency_type = DELAY_P99_READ;
	} else if (!strncmp(req.latency_type, "avg_write", 10)) {
		latency_type = DELAY_AVG_WRITE;
	} else if (!strncmp(req.latency_type, "p99_write", 10)) {
		latency_type = DELAY_P99_WRITE;
	} else {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Please specify a valid latency type.");
		goto cleanup;
	}

	rc = vbdev_delay_update_latency_value(req.delay_bdev_name, req.latency_us, latency_type);

	if (rc == -ENODEV) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "The requested bdev does not exist.");
		goto cleanup;
	} else if (rc == -EINVAL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST,
						 "The requested bdev is not a delay bdev.");
		goto cleanup;
	} else if (rc) {
		SPDK_UNREACHABLE();
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_delay_update_latency(&req);
}
SPDK_RPC_REGISTER("bdev_delay_update_latency", rpc_bdev_delay_update_latency, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_delay_create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_bdev_delay_create_ctx, base_bdev_name), spdk_json_decode_string},
	{"name", offsetof(struct rpc_bdev_delay_create_ctx, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_bdev_delay_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"avg_read_latency", offsetof(struct rpc_bdev_delay_create_ctx, avg_read_latency), spdk_json_decode_uint64},
	{"p99_read_latency", offsetof(struct rpc_bdev_delay_create_ctx, p99_read_latency), spdk_json_decode_uint64},
	{"avg_write_latency", offsetof(struct rpc_bdev_delay_create_ctx, avg_write_latency), spdk_json_decode_uint64},
	{"p99_write_latency", offsetof(struct rpc_bdev_delay_create_ctx, p99_write_latency), spdk_json_decode_uint64},
};

static void
rpc_bdev_delay_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_delay_create_ctx req = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_delay_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_delay_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_delay, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = create_delay_disk(req.base_bdev_name, req.name, &req.uuid, req.avg_read_latency,
			       req.p99_read_latency,
			       req.avg_write_latency, req.p99_write_latency);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_delay_create(&req);
}
SPDK_RPC_REGISTER("bdev_delay_create", rpc_bdev_delay_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_delay_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_delay_delete_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_delay_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_delay_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_delay_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_delay_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_delay_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_delay_disk(req.name, rpc_bdev_delay_delete_cb, request);

cleanup:
	free_rpc_bdev_delay_delete(&req);
}
SPDK_RPC_REGISTER("bdev_delay_delete", rpc_bdev_delay_delete, SPDK_RPC_RUNTIME)
