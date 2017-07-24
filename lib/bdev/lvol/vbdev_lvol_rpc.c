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

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "spdk/util.h"
#include "vbdev_lvol.h"
#include "lvs_bdev.h"

#include "spdk_internal/log.h"

struct rpc_construct_lvol_store {
	char *base_name;
};

static void
free_rpc_construct_lvol_store(struct rpc_construct_lvol_store *req)
{
	free(req->base_name);
}

static const struct spdk_json_object_decoder rpc_construct_lvol_store_decoders[] = {
	{"base_name", offsetof(struct rpc_construct_lvol_store, base_name), spdk_json_decode_string},
};

static void
_spdk_rpc_lvol_store_construct_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct spdk_json_write_ctx *w;
	char lvol_store_uuid[37];
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvserrno != 0) {
		goto invalid;
	}

	uuid_unparse(lvol_store->uuid, lvol_store_uuid);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, lvol_store_uuid);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-lvserrno));
}

static void
spdk_rpc_construct_lvol_store(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_construct_lvol_store req = {};
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_lvol_store_decoders,
				    SPDK_COUNTOF(rpc_construct_lvol_store_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.base_name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		rc = -EINVAL;
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.base_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.base_name);
		rc = -ENODEV;
		goto invalid;
	}

	rc = vbdev_lvs_create(bdev, _spdk_rpc_lvol_store_construct_cb, request);
	if (rc < 0) {
		goto invalid;
	}
	free_rpc_construct_lvol_store(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-rc));
	free_rpc_construct_lvol_store(&req);
}
SPDK_RPC_REGISTER("construct_lvol_store", spdk_rpc_construct_lvol_store)

struct rpc_destroy_lvol_store {
	char *lvol_store_uuid;
};

static void
free_rpc_destroy_lvol_store(struct rpc_destroy_lvol_store *req)
{
	free(req->lvol_store_uuid);
}

static const struct spdk_json_object_decoder rpc_destroy_lvol_store_decoders[] = {
	{"lvol_store_uuid", offsetof(struct rpc_destroy_lvol_store, lvol_store_uuid), spdk_json_decode_string},
};

static void
_spdk_rpc_lvol_store_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvserrno != 0) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-lvserrno));
}

static void
spdk_rpc_destroy_lvol_store(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_destroy_lvol_store req = {};
	struct spdk_lvol_store *lvs;
	uuid_t lvol_store_uuid;
	int rc;

	if (spdk_json_decode_object(params, rpc_destroy_lvol_store_decoders,
				    SPDK_COUNTOF(rpc_destroy_lvol_store_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (uuid_parse(req.lvol_store_uuid, lvol_store_uuid)) {
		SPDK_TRACELOG(SPDK_TRACE_LVS_BDEV, "incorrect UUID '%s'\n", req.lvol_store_uuid);
		rc = -EINVAL;
		goto invalid;
	}

	lvs = vbdev_get_lvol_store_by_uuid(lvol_store_uuid);
	if (lvs == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_LVS_BDEV, "blobstore with UUID '%p' not found\n", &lvol_store_uuid);
		rc = -ENODEV;
		goto invalid;
	}

	vbdev_lvs_destruct(lvs, _spdk_rpc_lvol_store_destroy_cb, request);

	free_rpc_destroy_lvol_store(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-rc));
	free_rpc_destroy_lvol_store(&req);
}
SPDK_RPC_REGISTER("destroy_lvol_store", spdk_rpc_destroy_lvol_store)

struct rpc_construct_lvol_bdev {
	char *lvol_store_uuid;
	uint32_t size;
};

static void
free_rpc_construct_lvol_bdev(struct rpc_construct_lvol_bdev *req)
{
	free(req->lvol_store_uuid);
}

static const struct spdk_json_object_decoder rpc_construct_lvol_bdev_decoders[] = {
	{"lvol_store_uuid", offsetof(struct rpc_construct_lvol_bdev, lvol_store_uuid), spdk_json_decode_string},
	{"size", offsetof(struct rpc_construct_lvol_bdev, size), spdk_json_decode_uint32},
};

static void
_spdk_rpc_construct_lvol_bdev_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	spdk_json_write_string(w, lvol->name);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-lvolerrno));
}

static void
spdk_rpc_construct_lvol_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_lvol_bdev req = {};
	uuid_t lvol_store_uuid;
	size_t sz;
	int rc;

	SPDK_TRACELOG(SPDK_TRACE_LVS_BDEV, "Creating blob\n");

	if (spdk_json_decode_object(params, rpc_construct_lvol_bdev_decoders,
				    SPDK_COUNTOF(rpc_construct_lvol_bdev_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (uuid_parse(req.lvol_store_uuid, lvol_store_uuid)) {
		SPDK_TRACELOG(SPDK_TRACE_LVS_BDEV, "incorrect UUID '%s'\n", req.lvol_store_uuid);
		rc = -EINVAL;
		goto invalid;
	}

	sz = (size_t)req.size;

	rc = vbdev_lvol_create(lvol_store_uuid, sz, _spdk_rpc_construct_lvol_bdev_cb, request);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_construct_lvol_bdev(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-rc));
	free_rpc_construct_lvol_bdev(&req);
}

SPDK_RPC_REGISTER("construct_lvol_bdev", spdk_rpc_construct_lvol_bdev)
