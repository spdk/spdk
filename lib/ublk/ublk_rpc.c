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
	struct spdk_jsonrpc_request *request;
};

static const struct spdk_json_object_decoder rpc_ublk_start_disk_decoders[] = {
	{"bdev_name", offsetof(struct rpc_ublk_start_disk, bdev_name), spdk_json_decode_string},
	{"ublk_id", offsetof(struct rpc_ublk_start_disk, ublk_id), spdk_json_decode_uint32},
	{"num_queues", offsetof(struct rpc_ublk_start_disk, num_queues), spdk_json_decode_uint32, true},
	{"queue_depth", offsetof(struct rpc_ublk_start_disk, queue_depth), spdk_json_decode_uint32, true},
};

static void
free_rpc_ublk_start_disk(struct rpc_ublk_start_disk *req)
{
	free(req->bdev_name);
	free(req);
}

static void
rpc_ublk_start_disk_done(void *cb_arg, int rc)
{
	struct rpc_ublk_start_disk *req = cb_arg;
	struct spdk_json_write_ctx *w;

	if (rc == 0) {
		w = spdk_jsonrpc_begin_result(req->request);
		spdk_json_write_uint32(w, req->ublk_id);
		spdk_jsonrpc_end_result(req->request, w);
	} else {
		spdk_jsonrpc_send_error_response(req->request, rc, spdk_strerror(-rc));
	}

	free_rpc_ublk_start_disk(req);
}

static void
rpc_ublk_start_disk(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_ublk_start_disk *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	req->request = request;
	req->queue_depth = UBLK_DEV_QUEUE_DEPTH;
	req->num_queues = UBLK_DEV_NUM_QUEUE;

	if (spdk_json_decode_object(params, rpc_ublk_start_disk_decoders,
				    SPDK_COUNTOF(rpc_ublk_start_disk_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto out;
	}

	rc = ublk_start_disk(req->bdev_name, req->ublk_id, req->num_queues, req->queue_depth,
			     rpc_ublk_start_disk_done, req);
	if (rc != 0) {
		rpc_ublk_start_disk_done(req, rc);
	}

	return;

out:
	free_rpc_ublk_start_disk(req);
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

static void
rpc_dump_ublk_info(struct spdk_json_write_ctx *w,
		   struct spdk_ublk_dev *ublk)
{
	char ublk_path[32];

	snprintf(ublk_path, 32, "%s%u", "/dev/ublkb", ublk_dev_get_id(ublk));
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "ublk_device", ublk_path);
	spdk_json_write_named_uint32(w, "id", ublk_dev_get_id(ublk));
	spdk_json_write_named_uint32(w, "queue_depth", ublk_dev_get_queue_depth(ublk));
	spdk_json_write_named_uint32(w, "num_queues", ublk_dev_get_num_queues(ublk));
	spdk_json_write_named_string(w, "bdev_name", ublk_dev_get_bdev_name(ublk));

	spdk_json_write_object_end(w);
}

struct rpc_ublk_get_disks {
	uint32_t ublk_id;
};

static const struct spdk_json_object_decoder rpc_ublk_get_disks_decoders[] = {
	{"ublk_id", offsetof(struct rpc_ublk_get_disks, ublk_id), spdk_json_decode_uint32, true},
};

static void
rpc_ublk_get_disks(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_ublk_get_disks req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_ublk_dev *ublk = NULL;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_ublk_get_disks_decoders,
					    SPDK_COUNTOF(rpc_ublk_get_disks_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			return;
		}

		if (req.ublk_id) {
			ublk = ublk_dev_find_by_id(req.ublk_id);
			if (ublk == NULL) {
				SPDK_ERRLOG("ublk device '%d' does not exist\n", req.ublk_id);
				spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
				return;
			}
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (ublk != NULL) {
		rpc_dump_ublk_info(w, ublk);
	} else {
		for (ublk = ublk_dev_first(); ublk != NULL; ublk = ublk_dev_next(ublk)) {
			rpc_dump_ublk_info(w, ublk);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

	return;
}
SPDK_RPC_REGISTER("ublk_get_disks", rpc_ublk_get_disks, SPDK_RPC_RUNTIME)
