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
#include "spdk/config.h"
#include "spdk_internal/rpc_autogen.h"

#include "vfu_virtio_internal.h"

static const struct spdk_json_object_decoder rpc_vfu_virtio_delete_endpoint_decoders[] = {
	{"name", offsetof(struct rpc_vfu_virtio_delete_endpoint_ctx, name), spdk_json_decode_string }
};

static void
rpc_vfu_virtio_delete_endpoint(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_delete_endpoint_ctx req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_vfu_virtio_delete_endpoint_decoders,
				    SPDK_COUNTOF(rpc_vfu_virtio_delete_endpoint_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vfu_delete_endpoint(req.name);
	if (rc < 0) {
		goto invalid;
	}
	free_rpc_vfu_virtio_delete_endpoint(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_delete_endpoint(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_delete_endpoint", rpc_vfu_virtio_delete_endpoint,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_vfu_virtio_create_blk_endpoint_decoders[] = {
	{"name", offsetof(struct rpc_vfu_virtio_create_blk_endpoint_ctx, name), spdk_json_decode_string },
	{"bdev_name", offsetof(struct rpc_vfu_virtio_create_blk_endpoint_ctx, bdev_name), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vfu_virtio_create_blk_endpoint_ctx, cpumask), spdk_json_decode_string, true},
	{"num_queues", offsetof(struct rpc_vfu_virtio_create_blk_endpoint_ctx, num_queues), spdk_json_decode_uint16, true },
	{"qsize", offsetof(struct rpc_vfu_virtio_create_blk_endpoint_ctx, qsize), spdk_json_decode_uint16, true },
	{"packed_ring", offsetof(struct rpc_vfu_virtio_create_blk_endpoint_ctx, packed_ring), spdk_json_decode_bool, true},
};

static void
rpc_vfu_virtio_create_blk_endpoint(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_create_blk_endpoint_ctx req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_vfu_virtio_create_blk_endpoint_decoders,
				    SPDK_COUNTOF(rpc_vfu_virtio_create_blk_endpoint_decoders),
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
	free_rpc_vfu_virtio_create_blk_endpoint(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_create_blk_endpoint(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_create_blk_endpoint", rpc_vfu_virtio_create_blk_endpoint,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_vfu_virtio_scsi_add_target_decoders[] = {
	{"name", offsetof(struct rpc_vfu_virtio_scsi_add_target_ctx, name), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_vfu_virtio_scsi_add_target_ctx, scsi_target_num), spdk_json_decode_uint8 },
	{"bdev_name", offsetof(struct rpc_vfu_virtio_scsi_add_target_ctx, bdev_name), spdk_json_decode_string },
};

static void
rpc_vfu_virtio_scsi_add_target(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_scsi_add_target_ctx req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_vfu_virtio_scsi_add_target_decoders,
				    SPDK_COUNTOF(rpc_vfu_virtio_scsi_add_target_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vfu_virtio_scsi_add_target(req.name, req.scsi_target_num, req.bdev_name);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vfu_virtio_scsi_add_target(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_scsi_add_target(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_scsi_add_target", rpc_vfu_virtio_scsi_add_target,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_vfu_virtio_scsi_remove_target_decoders[] = {
	{"name", offsetof(struct rpc_vfu_virtio_scsi_remove_target_ctx, name), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_vfu_virtio_scsi_remove_target_ctx, scsi_target_num), spdk_json_decode_uint8 },
};

static void
rpc_vfu_virtio_scsi_remove_target(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_scsi_remove_target_ctx req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_vfu_virtio_scsi_remove_target_decoders,
				    SPDK_COUNTOF(rpc_vfu_virtio_scsi_remove_target_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vfu_virtio_scsi_remove_target(req.name, req.scsi_target_num);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vfu_virtio_scsi_remove_target(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_scsi_remove_target(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_scsi_remove_target", rpc_vfu_virtio_scsi_remove_target,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_vfu_virtio_create_scsi_endpoint_decoders[] = {
	{"name", offsetof(struct rpc_vfu_virtio_create_scsi_endpoint_ctx, name), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vfu_virtio_create_scsi_endpoint_ctx, cpumask), spdk_json_decode_string, true},
	{"num_io_queues", offsetof(struct rpc_vfu_virtio_create_scsi_endpoint_ctx, num_io_queues), spdk_json_decode_uint16, true },
	{"qsize", offsetof(struct rpc_vfu_virtio_create_scsi_endpoint_ctx, qsize), spdk_json_decode_uint16, true },
	{"packed_ring", offsetof(struct rpc_vfu_virtio_create_scsi_endpoint_ctx, packed_ring), spdk_json_decode_bool, true},
};

static void
rpc_vfu_virtio_create_scsi_endpoint(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_create_scsi_endpoint_ctx req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_vfu_virtio_create_scsi_endpoint_decoders,
				    SPDK_COUNTOF(rpc_vfu_virtio_create_scsi_endpoint_decoders),
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
	free_rpc_vfu_virtio_create_scsi_endpoint(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vfu_virtio_create_scsi_endpoint(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_create_scsi_endpoint", rpc_vfu_virtio_create_scsi_endpoint,
		  SPDK_RPC_RUNTIME)

#ifdef SPDK_CONFIG_FSDEV

static const struct spdk_json_object_decoder rpc_vfu_virtio_create_fs_endpoint_decoders[] = {
	{"name", offsetof(struct rpc_vfu_virtio_create_fs_endpoint_ctx, name), spdk_json_decode_string },
	{"fsdev_name", offsetof(struct rpc_vfu_virtio_create_fs_endpoint_ctx, fsdev_name), spdk_json_decode_string },
	{"tag", offsetof(struct rpc_vfu_virtio_create_fs_endpoint_ctx, tag), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vfu_virtio_create_fs_endpoint_ctx, cpumask), spdk_json_decode_string, true},
	{"num_queues", offsetof(struct rpc_vfu_virtio_create_fs_endpoint_ctx, num_queues), spdk_json_decode_uint16, true },
	{"qsize", offsetof(struct rpc_vfu_virtio_create_fs_endpoint_ctx, qsize), spdk_json_decode_uint16, true },
	{"packed_ring", offsetof(struct rpc_vfu_virtio_create_fs_endpoint_ctx, packed_ring), spdk_json_decode_bool, true},
};

static void
rpc_vfu_virtio_create_fs_endpoint_cpl(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_vfu_virtio_create_fs_endpoint(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_vfu_virtio_create_fs_endpoint_ctx req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_vfu_virtio_create_fs_endpoint_decoders,
				    SPDK_COUNTOF(rpc_vfu_virtio_create_fs_endpoint_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vfu_create_endpoint(req.name, req.cpumask, "virtio_fs");
	if (rc) {
		SPDK_ERRLOG("Failed to create virtio_fs endpoint\n");
		goto invalid;
	}

	rc = vfu_virtio_fs_add_fsdev(req.name, req.fsdev_name, req.tag, req.num_queues, req.qsize,
				     req.packed_ring, rpc_vfu_virtio_create_fs_endpoint_cpl, request);
	if (rc < 0) {
		spdk_vfu_delete_endpoint(req.name);
		goto invalid;
	}

	free_rpc_vfu_virtio_create_fs_endpoint(&req);
	return;

invalid:
	free_rpc_vfu_virtio_create_fs_endpoint(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vfu_virtio_create_fs_endpoint", rpc_vfu_virtio_create_fs_endpoint,
		  SPDK_RPC_RUNTIME)
#endif
