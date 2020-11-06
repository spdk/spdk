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

#include "vbdev_delay.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/assert.h"

struct rpc_update_latency {
	char *delay_bdev_name;
	char *latency_type;
	uint64_t latency_us;
};

static const struct spdk_json_object_decoder rpc_update_latency_decoders[] = {
	{"delay_bdev_name", offsetof(struct rpc_update_latency, delay_bdev_name), spdk_json_decode_string},
	{"latency_type", offsetof(struct rpc_update_latency, latency_type), spdk_json_decode_string},
	{"latency_us", offsetof(struct rpc_update_latency, latency_us), spdk_json_decode_uint64}
};

static void
free_rpc_update_latency(struct rpc_update_latency *req)
{
	free(req->delay_bdev_name);
	free(req->latency_type);
}

static void
rpc_bdev_delay_update_latency(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_update_latency req = {NULL};
	enum delay_io_type latency_type;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_update_latency_decoders,
				    SPDK_COUNTOF(rpc_update_latency_decoders),
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
	free_rpc_update_latency(&req);
}
SPDK_RPC_REGISTER("bdev_delay_update_latency", rpc_bdev_delay_update_latency, SPDK_RPC_RUNTIME)

struct rpc_construct_delay {
	char *base_bdev_name;
	char *name;
	uint64_t avg_read_latency;
	uint64_t p99_read_latency;
	uint64_t avg_write_latency;
	uint64_t p99_write_latency;
};

static void
free_rpc_construct_delay(struct rpc_construct_delay *r)
{
	free(r->base_bdev_name);
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_construct_delay_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_delay, base_bdev_name), spdk_json_decode_string},
	{"name", offsetof(struct rpc_construct_delay, name), spdk_json_decode_string},
	{"avg_read_latency", offsetof(struct rpc_construct_delay, avg_read_latency), spdk_json_decode_uint64},
	{"p99_read_latency", offsetof(struct rpc_construct_delay, p99_read_latency), spdk_json_decode_uint64},
	{"avg_write_latency", offsetof(struct rpc_construct_delay, avg_write_latency), spdk_json_decode_uint64},
	{"p99_write_latency", offsetof(struct rpc_construct_delay, p99_write_latency), spdk_json_decode_uint64},
};

static void
rpc_bdev_delay_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_construct_delay req = {NULL};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_delay_decoders,
				    SPDK_COUNTOF(rpc_construct_delay_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_delay, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = create_delay_disk(req.base_bdev_name, req.name, req.avg_read_latency, req.p99_read_latency,
			       req.avg_write_latency, req.p99_write_latency);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_construct_delay(&req);
}
SPDK_RPC_REGISTER("bdev_delay_create", rpc_bdev_delay_create, SPDK_RPC_RUNTIME)

struct rpc_delete_delay {
	char *name;
};

static void
free_rpc_delete_delay(struct rpc_delete_delay *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_delay_decoders[] = {
	{"name", offsetof(struct rpc_delete_delay, name), spdk_json_decode_string},
};

static void
rpc_bdev_delay_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_delay_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_delay req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_delay_decoders,
				    SPDK_COUNTOF(rpc_delete_delay_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	delete_delay_disk(bdev, rpc_bdev_delay_delete_cb, request);

cleanup:
	free_rpc_delete_delay(&req);
}
SPDK_RPC_REGISTER("bdev_delay_delete", rpc_bdev_delay_delete, SPDK_RPC_RUNTIME)
