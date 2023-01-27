/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) croit GmbH.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 */

#include "bdev_daos.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_construct_daos {
	char *name;
	char *uuid;
	char *pool;
	char *cont;
	char *oclass;
	uint64_t num_blocks;
	uint32_t block_size;
};

static void
free_rpc_construct_daos(struct rpc_construct_daos *r)
{
	free(r->name);
	free(r->uuid);
	free(r->pool);
	free(r->cont);
	free(r->oclass);
}

static const struct spdk_json_object_decoder rpc_construct_daos_decoders[] = {
	{"name", offsetof(struct rpc_construct_daos, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_construct_daos, uuid), spdk_json_decode_string, true},
	{"pool", offsetof(struct rpc_construct_daos, pool), spdk_json_decode_string},
	{"cont", offsetof(struct rpc_construct_daos, cont), spdk_json_decode_string},
	{"oclass", offsetof(struct rpc_construct_daos, oclass), spdk_json_decode_string, true},
	{"num_blocks", offsetof(struct rpc_construct_daos, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_construct_daos, block_size), spdk_json_decode_uint32},
};

static void
rpc_bdev_daos_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_construct_daos req = {NULL};
	struct spdk_json_write_ctx *w;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;
	struct spdk_bdev *bdev;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_construct_daos_decoders,
				    SPDK_COUNTOF(rpc_construct_daos_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_daos, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.uuid) {
		if (spdk_uuid_parse(&decoded_uuid, req.uuid)) {
			spdk_jsonrpc_send_error_response(request, -EINVAL,
							 "Failed to parse bdev UUID");
			goto cleanup;
		}
		uuid = &decoded_uuid;
	}

	rc = create_bdev_daos(&bdev, req.name, uuid, req.pool, req.cont, req.oclass,
			      req.num_blocks, req.block_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	free_rpc_construct_daos(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
	return;

cleanup:
	free_rpc_construct_daos(&req);
}
SPDK_RPC_REGISTER("bdev_daos_create", rpc_bdev_daos_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_daos_create, construct_daos_bdev)

struct rpc_delete_daos {
	char *name;
};

static void
free_rpc_delete_daos(struct rpc_delete_daos *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_daos_decoders[] = {
	{"name", offsetof(struct rpc_delete_daos, name), spdk_json_decode_string},
};

static void
rpc_bdev_daos_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_daos_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_delete_daos req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_daos_decoders,
				    SPDK_COUNTOF(rpc_delete_daos_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_daos, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_INFOLOG(bdev_daos, "bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	delete_bdev_daos(bdev, rpc_bdev_daos_delete_cb, request);

cleanup:
	free_rpc_delete_daos(&req);
}

SPDK_RPC_REGISTER("bdev_daos_delete", rpc_bdev_daos_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_daos_delete, delete_daos_bdev)

struct rpc_bdev_daos_resize {
	char *name;
	uint64_t new_size;
};

static const struct spdk_json_object_decoder rpc_bdev_daos_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_daos_resize, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_daos_resize, new_size), spdk_json_decode_uint64}
};

static void
free_rpc_bdev_daos_resize(struct rpc_bdev_daos_resize *req)
{
	free(req->name);
}

static void
rpc_bdev_daos_resize(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_daos_resize req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_daos_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_daos_resize_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_daos_resize(req.name, req.new_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_daos_resize(&req);
}
SPDK_RPC_REGISTER("bdev_daos_resize", rpc_bdev_daos_resize, SPDK_RPC_RUNTIME)
