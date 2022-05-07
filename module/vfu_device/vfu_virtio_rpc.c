/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
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
