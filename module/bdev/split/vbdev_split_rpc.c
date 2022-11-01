/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "vbdev_split.h"
#include "spdk/log.h"

struct rpc_construct_split {
	char *base_bdev;
	uint32_t split_count;
	uint64_t split_size_mb;
};

static const struct spdk_json_object_decoder rpc_construct_split_decoders[] = {
	{"base_bdev", offsetof(struct rpc_construct_split, base_bdev), spdk_json_decode_string},
	{"split_count", offsetof(struct rpc_construct_split, split_count), spdk_json_decode_uint32},
	{"split_size_mb", offsetof(struct rpc_construct_split, split_size_mb), spdk_json_decode_uint64, true},
};

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

static void
rpc_bdev_split_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_construct_split req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev_desc *base_desc;
	struct spdk_bdev *base_bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_split_decoders,
				    SPDK_COUNTOF(rpc_construct_split_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = create_vbdev_split(req.base_bdev, req.split_count, req.split_size_mb);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to create %"PRIu32" split bdevs from '%s': %s",
						     req.split_count, req.base_bdev, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	rc = spdk_bdev_open_ext(req.base_bdev, false, dummy_bdev_event_cb, NULL, &base_desc);
	if (rc == 0) {
		struct spdk_bdev_part_base *split_base;
		struct bdev_part_tailq *split_base_tailq;
		struct spdk_bdev_part *split_part;
		struct spdk_bdev *split_bdev;

		base_bdev = spdk_bdev_desc_get_bdev(base_desc);

		split_base = vbdev_split_get_part_base(base_bdev);

		assert(split_base != NULL);

		split_base_tailq = spdk_bdev_part_base_get_tailq(split_base);
		TAILQ_FOREACH(split_part, split_base_tailq, tailq) {
			split_bdev = spdk_bdev_part_get_bdev(split_part);
			spdk_json_write_string(w, spdk_bdev_get_name(split_bdev));
		}

		spdk_bdev_close(base_desc);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free(req.base_bdev);
}
SPDK_RPC_REGISTER("bdev_split_create", rpc_bdev_split_create, SPDK_RPC_RUNTIME)

struct rpc_delete_split {
	char *base_bdev;
};

static const struct spdk_json_object_decoder rpc_delete_split_decoders[] = {
	{"base_bdev", offsetof(struct rpc_delete_split, base_bdev), spdk_json_decode_string},
};

static void
rpc_bdev_split_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_split req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_split_decoders,
				    SPDK_COUNTOF(rpc_delete_split_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = vbdev_split_destruct(req.base_bdev);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	free(req.base_bdev);
}
SPDK_RPC_REGISTER("bdev_split_delete", rpc_bdev_split_delete, SPDK_RPC_RUNTIME)
