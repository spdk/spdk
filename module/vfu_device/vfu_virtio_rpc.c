/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/thread.h"

#include "vfu_virtio_internal.h"

struct rpc_delete_vfu_endpoint {
	char		*name;
};

static const struct spdk_json_object_decoder rpc_delete_vfu_endpoint_decode[] = {
	{"name", offsetof(struct rpc_delete_vfu_endpoint, name), spdk_json_decode_string }
};

static void
free_rpc_delete_vfu_endpoint(struct rpc_delete_vfu_endpoint *req)
{
	free(req->name);
}

static void
rpc_vfu_virtio_delete_endpoint(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_delete_vfu_endpoint req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_vfu_endpoint_decode,
				    SPDK_COUNTOF(rpc_delete_vfu_endpoint_decode),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vfu_delete_endpoint(req.name);
	if (rc < 0) {
		goto invalid;
	}
	free_rpc_delete_vfu_endpoint(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_delete_vfu_endpoint(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_delete_endpoint", rpc_vfu_virtio_delete_endpoint,
		  SPDK_RPC_RUNTIME)

struct rpc_vfu_virtio_create_blk {
	char		*name;
	char		*bdev_name;
	char		*cpumask;
	uint16_t	num_queues;
	uint16_t	qsize;
	bool		packed_ring;
};

static const struct spdk_json_object_decoder rpc_construct_vfu_virtio_create_blk[] = {
	{"name", offsetof(struct rpc_vfu_virtio_create_blk, name), spdk_json_decode_string },
	{"bdev_name", offsetof(struct rpc_vfu_virtio_create_blk, bdev_name), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vfu_virtio_create_blk, cpumask), spdk_json_decode_string, true},
	{"num_queues", offsetof(struct rpc_vfu_virtio_create_blk, num_queues), spdk_json_decode_uint16, true },
	{"qsize", offsetof(struct rpc_vfu_virtio_create_blk, qsize), spdk_json_decode_uint16, true },
	{"packed_ring", offsetof(struct rpc_vfu_virtio_create_blk, packed_ring), spdk_json_decode_bool, true},
};

static void
free_rpc_vfu_virtio_create_blk(struct rpc_vfu_virtio_create_blk *req)
{
	free(req->name);
	free(req->bdev_name);
	free(req->cpumask);
}

static void
rpc_vfu_virtio_create_blk_endpoint(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_create_blk req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_vfu_virtio_create_blk,
				    SPDK_COUNTOF(rpc_construct_vfu_virtio_create_blk),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vfu_create_endpoint(req.name, req.cpumask, "virtio_blk");
	if (rc) {
		SPDK_ERRLOG("Failed to create virtio_blk endpoint\n");
		goto invalid;
	}

	rc = vfu_virtio_blk_add_bdev(req.name, req.bdev_name, req.num_queues, req.qsize,
				     req.packed_ring);
	if (rc < 0) {
		spdk_vfu_delete_endpoint(req.name);
		goto invalid;
	}
	free_rpc_vfu_virtio_create_blk(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_create_blk(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_create_blk_endpoint", rpc_vfu_virtio_create_blk_endpoint,
		  SPDK_RPC_RUNTIME)

struct rpc_vfu_virtio_scsi {
	char		*name;
	uint8_t		scsi_target_num;
	char		*bdev_name;
};

static const struct spdk_json_object_decoder rpc_construct_vfu_virtio_scsi[] = {
	{"name", offsetof(struct rpc_vfu_virtio_scsi, name), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_vfu_virtio_scsi, scsi_target_num), spdk_json_decode_uint8 },
	{"bdev_name", offsetof(struct rpc_vfu_virtio_scsi, bdev_name), spdk_json_decode_string },
};

static void
free_rpc_vfu_virtio_scsi(struct rpc_vfu_virtio_scsi *req)
{
	free(req->name);
	free(req->bdev_name);
}

static void
rpc_vfu_virtio_scsi_add_target(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_scsi req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_vfu_virtio_scsi,
				    SPDK_COUNTOF(rpc_construct_vfu_virtio_scsi),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vfu_virtio_scsi_add_target(req.name, req.scsi_target_num, req.bdev_name);;
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vfu_virtio_scsi(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_scsi(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_scsi_add_target", rpc_vfu_virtio_scsi_add_target,
		  SPDK_RPC_RUNTIME)

struct rpc_vfu_virtio_scsi_remove {
	char		*name;
	uint8_t		scsi_target_num;
};

static const struct spdk_json_object_decoder rpc_remove_vfu_virtio_scsi_target[] = {
	{"name", offsetof(struct rpc_vfu_virtio_scsi_remove, name), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_vfu_virtio_scsi_remove, scsi_target_num), spdk_json_decode_uint8 },
};

static void
free_rpc_vfu_virtio_scsi_remove(struct rpc_vfu_virtio_scsi_remove *req)
{
	free(req->name);
}

static void
rpc_vfu_virtio_scsi_remove_target(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_scsi_remove req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_remove_vfu_virtio_scsi_target,
				    SPDK_COUNTOF(rpc_remove_vfu_virtio_scsi_target),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vfu_virtio_scsi_remove_target(req.name, req.scsi_target_num);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vfu_virtio_scsi_remove(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_scsi_remove(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_scsi_remove_target", rpc_vfu_virtio_scsi_remove_target,
		  SPDK_RPC_RUNTIME)

struct rpc_vfu_virtio_create_scsi {
	char		*name;
	char		*cpumask;
	uint16_t	num_io_queues;
	uint16_t	qsize;
	bool		packed_ring;
};

static const struct spdk_json_object_decoder rpc_construct_vfu_virtio_create_scsi[] = {
	{"name", offsetof(struct rpc_vfu_virtio_create_scsi, name), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vfu_virtio_create_scsi, cpumask), spdk_json_decode_string, true},
	{"num_io_queues", offsetof(struct rpc_vfu_virtio_create_scsi, num_io_queues), spdk_json_decode_uint16, true },
	{"qsize", offsetof(struct rpc_vfu_virtio_create_scsi, qsize), spdk_json_decode_uint16, true },
	{"packed_ring", offsetof(struct rpc_vfu_virtio_create_scsi, packed_ring), spdk_json_decode_bool, true},
};

static void
free_rpc_vfu_virtio_create_scsi(struct rpc_vfu_virtio_create_scsi *req)
{
	free(req->name);
	free(req->cpumask);
}

static void
rpc_vfu_virtio_create_scsi_endpoint(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_create_scsi req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_vfu_virtio_create_scsi,
				    SPDK_COUNTOF(rpc_construct_vfu_virtio_create_scsi),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vfu_create_endpoint(req.name, req.cpumask, "virtio_scsi");
	if (rc) {
		SPDK_ERRLOG("Failed to create virtio_blk endpoint\n");
		goto invalid;
	}

	rc = vfu_virtio_scsi_set_options(req.name, req.num_io_queues, req.qsize, req.packed_ring);
	if (rc < 0) {
		spdk_vfu_delete_endpoint(req.name);
		goto invalid;
	}
	free_rpc_vfu_virtio_create_scsi(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_create_scsi(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_create_scsi_endpoint", rpc_vfu_virtio_create_scsi_endpoint,
		  SPDK_RPC_RUNTIME)
