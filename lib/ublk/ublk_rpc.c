/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "ublk_internal.h"

struct rpc_ublk_create_target {
	char		*cpumask;
};

static const struct spdk_json_object_decoder rpc_ublk_create_target_decoders[] = {
	{"cpumask", offsetof(struct rpc_ublk_create_target, cpumask), spdk_json_decode_string, true},
};

static void
free_rpc_ublk_create_target(struct rpc_ublk_create_target *req)
{
	free(req->cpumask);
}

static void
rpc_ublk_create_target(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc = 0;
	struct rpc_ublk_create_target req = {};

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_ublk_create_target_decoders,
					    SPDK_COUNTOF(rpc_ublk_create_target_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}
	}
	rc = ublk_create_target(req.cpumask);
	if (rc != 0) {
		goto invalid;
	}
	spdk_jsonrpc_send_bool_response(request, true);
	free_rpc_ublk_create_target(&req);
	return;
invalid:
	SPDK_ERRLOG("Can't create ublk target: %s\n", spdk_strerror(-rc));
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
	free_rpc_ublk_create_target(&req);
}
SPDK_RPC_REGISTER("ublk_create_target", rpc_ublk_create_target, SPDK_RPC_RUNTIME)

static void
ublk_destroy_target_done(void *arg)
{
	struct spdk_jsonrpc_request *req = arg;

	spdk_jsonrpc_send_bool_response(req, true);
	SPDK_NOTICELOG("ublk target has been destroyed\n");
}

static void
rpc_ublk_destroy_target(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	int rc = 0;

	rc = ublk_destroy_target(ublk_destroy_target_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		SPDK_ERRLOG("Can't destroy ublk target: %s\n", spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("ublk_destroy_target", rpc_ublk_destroy_target, SPDK_RPC_RUNTIME)

struct rpc_ublk_start_disk {
	char		*bdev_name;
	uint32_t	ublk_id;
	uint32_t	num_queues;
	uint32_t	queue_depth;
};

static const struct spdk_json_object_decoder rpc_ublk_start_disk_decoders[] = {
	{"bdev_name", offsetof(struct rpc_ublk_start_disk, bdev_name), spdk_json_decode_string},
	{"ublk_id", offsetof(struct rpc_ublk_start_disk, ublk_id), spdk_json_decode_uint32},
	{"num_queues", offsetof(struct rpc_ublk_start_disk, num_queues), spdk_json_decode_uint32, true},
	{"queue_depth", offsetof(struct rpc_ublk_start_disk, queue_depth), spdk_json_decode_uint32, true},
};

static void
rpc_ublk_start_disk(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct rpc_ublk_start_disk req = {};
	int rc;

	req.queue_depth = UBLK_DEV_QUEUE_DEPTH;
	req.num_queues = UBLK_DEV_NUM_QUEUE;

	if (spdk_json_decode_object(params, rpc_ublk_start_disk_decoders,
				    SPDK_COUNTOF(rpc_ublk_start_disk_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto out;
	}

	rc = ublk_start_disk(req.bdev_name, req.ublk_id, req.num_queues, req.queue_depth);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_uint32(w, req.ublk_id);
	spdk_jsonrpc_end_result(request, w);
	goto out;

out:
	free(req.bdev_name);
}

SPDK_RPC_REGISTER("ublk_start_disk", rpc_ublk_start_disk, SPDK_RPC_RUNTIME)

struct rpc_ublk_stop_disk {
	uint32_t ublk_id;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_ublk_stop_disk(struct rpc_ublk_stop_disk *req)
{
	free(req);
}

static const struct spdk_json_object_decoder rpc_ublk_stop_disk_decoders[] = {
	{"ublk_id", offsetof(struct rpc_ublk_stop_disk, ublk_id), spdk_json_decode_uint32},
};

static void
rpc_ublk_stop_disk_done(void *cb_arg)
{
	struct rpc_ublk_stop_disk *req = cb_arg;

	spdk_jsonrpc_send_bool_response(req->request, true);
	free_rpc_ublk_stop_disk(req);
}

static void
rpc_ublk_stop_disk(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_ublk_stop_disk *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req->request = request;

	if (spdk_json_decode_object(params, rpc_ublk_stop_disk_decoders,
				    SPDK_COUNTOF(rpc_ublk_stop_disk_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto invalid;
	}

	rc = ublk_stop_disk(req->ublk_id, rpc_ublk_stop_disk_done, req);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto invalid;
	}
	return;

invalid:
	free_rpc_ublk_stop_disk(req);
}

SPDK_RPC_REGISTER("ublk_stop_disk", rpc_ublk_stop_disk, SPDK_RPC_RUNTIME)
