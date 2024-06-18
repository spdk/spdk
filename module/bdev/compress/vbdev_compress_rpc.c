/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 */

#include "vbdev_compress.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_bdev_compress_get_orphans {
	char *name;
};

static void
free_rpc_bdev_compress_get_orphans(struct rpc_bdev_compress_get_orphans *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_compress_get_orphans_decoders[] = {
	{"name", offsetof(struct rpc_bdev_compress_get_orphans, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_compress_get_orphans(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_compress_get_orphans req = {};
	struct spdk_json_write_ctx *w;
	struct vbdev_compress *comp_bdev;
	bool found = false;


	if (params && spdk_json_decode_object(params, rpc_bdev_compress_get_orphans_decoders,
					      SPDK_COUNTOF(rpc_bdev_compress_get_orphans_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_bdev_compress_get_orphans(&req);
		return;
	}

	if (req.name) {
		if (compress_has_orphan(req.name) == false) {
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free_rpc_bdev_compress_get_orphans(&req);
			return;
		}
		found = true;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	if (found) {
		spdk_json_write_string(w, req.name);
	} else {
		for (comp_bdev = compress_bdev_first(); comp_bdev != NULL;
		     comp_bdev = compress_bdev_next(comp_bdev)) {
			if (compress_has_orphan(compress_get_name(comp_bdev))) {
				spdk_json_write_string(w, compress_get_name(comp_bdev));
			}
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_bdev_compress_get_orphans(&req);
}
SPDK_RPC_REGISTER("bdev_compress_get_orphans", rpc_bdev_compress_get_orphans, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_construct_compress {
	char *base_bdev_name;
	char *pm_path;
	uint32_t lb_size;
};

/* Free the allocated memory resource after the RPC handling. */
static void
free_rpc_construct_compress(struct rpc_construct_compress *r)
{
	free(r->base_bdev_name);
	free(r->pm_path);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_construct_compress_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_compress, base_bdev_name), spdk_json_decode_string},
	{"pm_path", offsetof(struct rpc_construct_compress, pm_path), spdk_json_decode_string},
	{"lb_size", offsetof(struct rpc_construct_compress, lb_size), spdk_json_decode_uint32, true},
};

/* Decode the parameters for this RPC method and properly construct the compress
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_compress_create(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_construct_compress req = {NULL};
	struct spdk_json_write_ctx *w;
	char *name;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_compress_decoders,
				    SPDK_COUNTOF(rpc_construct_compress_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_compress, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = create_compress_bdev(req.base_bdev_name, req.pm_path, req.lb_size);
	if (rc != 0) {
		if (rc == -EBUSY) {
			spdk_jsonrpc_send_error_response(request, rc, "Base bdev already in use for compression.");
		} else {
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		}
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	name = spdk_sprintf_alloc("COMP_%s", req.base_bdev_name);
	spdk_json_write_string(w, name);
	spdk_jsonrpc_end_result(request, w);
	free(name);

cleanup:
	free_rpc_construct_compress(&req);
}
SPDK_RPC_REGISTER("bdev_compress_create", rpc_bdev_compress_create, SPDK_RPC_RUNTIME)

struct rpc_delete_compress {
	char *name;
};

static void
free_rpc_delete_compress(struct rpc_delete_compress *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_compress_decoders[] = {
	{"name", offsetof(struct rpc_delete_compress, name), spdk_json_decode_string},
};

static void
_rpc_bdev_compress_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_compress_delete(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_delete_compress req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_compress_decoders,
				    SPDK_COUNTOF(rpc_delete_compress_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
	} else {
		bdev_compress_delete(req.name, _rpc_bdev_compress_delete_cb, request);
	}

	free_rpc_delete_compress(&req);
}
SPDK_RPC_REGISTER("bdev_compress_delete", rpc_bdev_compress_delete, SPDK_RPC_RUNTIME)
