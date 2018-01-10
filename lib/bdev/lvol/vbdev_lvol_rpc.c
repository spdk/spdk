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
#include "spdk/string.h"
#include "spdk_internal/log.h"

SPDK_LOG_REGISTER_COMPONENT("lvolrpc", SPDK_LOG_LVOL_RPC)

struct rpc_construct_lvol_store {
	char *lvs_name;
	char *bdev_name;
	uint32_t cluster_sz;
};

static int
vbdev_get_lvol_store_by_uuid_xor_name(const char *uuid, const char *lvs_name,
				      struct spdk_lvol_store **lvs)
{
	if ((uuid == NULL && lvs_name == NULL)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "lvs UUID nor lvs name specified\n");
		return -EINVAL;
	} else if ((uuid && lvs_name)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "both lvs UUID '%s' and lvs name '%s' specified\n", uuid,
			     lvs_name);
		return -EINVAL;
	} else if (uuid) {
		*lvs = vbdev_get_lvol_store_by_uuid(uuid);

		if (*lvs == NULL) {
			SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "blobstore with UUID '%s' not found\n", uuid);
			return -ENODEV;
		}
	} else if (lvs_name) {

		*lvs = vbdev_get_lvol_store_by_name(lvs_name);

		if (*lvs == NULL) {
			SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "blobstore with name '%s' not found\n", lvs_name);
			return -ENODEV;
		}
	}
	return 0;
}

static void
free_rpc_construct_lvol_store(struct rpc_construct_lvol_store *req)
{
	free(req->bdev_name);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_construct_lvol_store_decoders[] = {
	{"bdev_name", offsetof(struct rpc_construct_lvol_store, bdev_name), spdk_json_decode_string},
	{"cluster_sz", offsetof(struct rpc_construct_lvol_store, cluster_sz), spdk_json_decode_uint32, true},
	{"lvs_name", offsetof(struct rpc_construct_lvol_store, lvs_name), spdk_json_decode_string},
};

static void
_spdk_rpc_lvol_store_construct_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct spdk_json_write_ctx *w;
	char lvol_store_uuid[UUID_STRING_LEN];
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
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
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
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.bdev_name == NULL) {
		SPDK_ERRLOG("missing bdev_name param\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.lvs_name == NULL) {
		SPDK_ERRLOG("missing lvs_name param\n");
		rc = -EINVAL;
		goto invalid;
	}
	bdev = spdk_bdev_get_by_name(req.bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.bdev_name);
		rc = -ENODEV;
		goto invalid;
	}

	rc = vbdev_lvs_create(bdev, req.lvs_name, req.cluster_sz, _spdk_rpc_lvol_store_construct_cb,
			      request);
	if (rc < 0) {
		goto invalid;
	}
	free_rpc_construct_lvol_store(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_construct_lvol_store(&req);
}
SPDK_RPC_REGISTER("construct_lvol_store", spdk_rpc_construct_lvol_store)

struct rpc_rename_lvol_store {
	char *old_name;
	char *new_name;
};

static void
free_rpc_rename_lvol_store(struct rpc_rename_lvol_store *req)
{
	free(req->old_name);
	free(req->new_name);
}

static const struct spdk_json_object_decoder rpc_rename_lvol_store_decoders[] = {
	{"old_name", offsetof(struct rpc_rename_lvol_store, old_name), spdk_json_decode_string},
	{"new_name", offsetof(struct rpc_rename_lvol_store, new_name), spdk_json_decode_string},
};

static void
_spdk_rpc_rename_lvol_store_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;
	char buf[64];

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
	spdk_strerror(-lvserrno);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}

static void
spdk_rpc_rename_lvol_store(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_rename_lvol_store req = {};
	struct spdk_lvol_store *lvs;
	int rc;

	if (spdk_json_decode_object(params, rpc_rename_lvol_store_decoders,
				    SPDK_COUNTOF(rpc_rename_lvol_store_decoders),
				    &req)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	lvs = vbdev_get_lvol_store_by_name(req.old_name);
	if (lvs == NULL) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "no lvs existing for given name\n");
		rc = -ENOENT;
		goto invalid;
	}

	rc = vbdev_lvs_rename(lvs, req.new_name, _spdk_rpc_rename_lvol_store_cb, request);
	if (rc < 0) {
		goto invalid;
	}
	free_rpc_rename_lvol_store(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
	free_rpc_rename_lvol_store(&req);
}
SPDK_RPC_REGISTER("rename_lvol_store", spdk_rpc_rename_lvol_store)

struct rpc_destroy_lvol_store {
	char *uuid;
	char *lvs_name;
};

static void
free_rpc_destroy_lvol_store(struct rpc_destroy_lvol_store *req)
{
	free(req->uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_destroy_lvol_store_decoders[] = {
	{"uuid", offsetof(struct rpc_destroy_lvol_store, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_destroy_lvol_store, lvs_name), spdk_json_decode_string, true},
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
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
}

static void
spdk_rpc_destroy_lvol_store(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_destroy_lvol_store req = {};
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_destroy_lvol_store_decoders,
				    SPDK_COUNTOF(rpc_destroy_lvol_store_decoders),
				    &req)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
	if (rc != 0) {
		goto invalid;
	}

	vbdev_lvs_destruct(lvs, _spdk_rpc_lvol_store_destroy_cb, request);

	free_rpc_destroy_lvol_store(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_destroy_lvol_store(&req);
}
SPDK_RPC_REGISTER("destroy_lvol_store", spdk_rpc_destroy_lvol_store)

struct rpc_construct_lvol_bdev {
	char *uuid;
	char *lvs_name;
	char *lvol_name;
	uint64_t size;
	bool thin_provision;
};

static void
free_rpc_construct_lvol_bdev(struct rpc_construct_lvol_bdev *req)
{
	free(req->uuid);
	free(req->lvs_name);
	free(req->lvol_name);
}

static const struct spdk_json_object_decoder rpc_construct_lvol_bdev_decoders[] = {
	{"uuid", offsetof(struct rpc_construct_lvol_bdev, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_construct_lvol_bdev, lvs_name), spdk_json_decode_string, true},
	{"lvol_name", offsetof(struct rpc_construct_lvol_bdev, lvol_name), spdk_json_decode_string, true},
	{"size", offsetof(struct rpc_construct_lvol_bdev, size), spdk_json_decode_uint64},
	{"thin_provision", offsetof(struct rpc_construct_lvol_bdev, thin_provision), spdk_json_decode_bool, true},
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
	spdk_json_write_string(w, lvol->bdev->name);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
spdk_rpc_construct_lvol_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_lvol_bdev req = {};
	size_t sz;
	int rc;
	struct spdk_lvol_store *lvs = NULL;

	SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "Creating blob\n");

	if (spdk_json_decode_object(params, rpc_construct_lvol_bdev_decoders,
				    SPDK_COUNTOF(rpc_construct_lvol_bdev_decoders),
				    &req)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
	if (rc != 0) {
		goto invalid;
	}

	if (req.lvol_name == NULL) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "no bdev name\n");
		rc = -EINVAL;
		goto invalid;
	}

	sz = (size_t)req.size;

	rc = vbdev_lvol_create(lvs, req.lvol_name, sz, req.thin_provision,
			       _spdk_rpc_construct_lvol_bdev_cb, request);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_construct_lvol_bdev(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_construct_lvol_bdev(&req);
}

SPDK_RPC_REGISTER("construct_lvol_bdev", spdk_rpc_construct_lvol_bdev)

struct rpc_rename_lvol_bdev {
	char *old_name;
	char *new_name;
};

static void
free_rpc_rename_lvol_bdev(struct rpc_rename_lvol_bdev *req)
{
	free(req->old_name);
	free(req->new_name);
}

static const struct spdk_json_object_decoder rpc_rename_lvol_bdev_decoders[] = {
	{"old_name", offsetof(struct rpc_rename_lvol_bdev, old_name), spdk_json_decode_string, true},
	{"new_name", offsetof(struct rpc_rename_lvol_bdev, new_name), spdk_json_decode_string, true},
};

static void
_spdk_rpc_rename_lvol_bdev_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;
	char buf[64];

	if (lvolerrno != 0) {
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
	spdk_strerror(-lvolerrno);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}

static void
spdk_rpc_rename_lvol_bdev(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_rename_lvol_bdev req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;
	int rc = 0;

	SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "Renaming lvol\n");

	if (spdk_json_decode_object(params, rpc_rename_lvol_bdev_decoders,
				    SPDK_COUNTOF(rpc_rename_lvol_bdev_decoders),
				    &req)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.old_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.old_name);
		rc = -ENODEV;
		goto invalid;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		rc = -ENODEV;
		goto invalid;
	}

	rc = vbdev_lvol_rename(lvol, req.new_name,
			       _spdk_rpc_rename_lvol_bdev_cb, request);

	if (rc < 0) {
		goto invalid;
	}

	free_rpc_rename_lvol_bdev(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
	free_rpc_rename_lvol_bdev(&req);
}

SPDK_RPC_REGISTER("rename_lvol_bdev", spdk_rpc_rename_lvol_bdev)

struct rpc_resize_lvol_bdev {
	char *name;
	uint64_t size;
};

static void
free_rpc_resize_lvol_bdev(struct rpc_resize_lvol_bdev *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_resize_lvol_bdev_decoders[] = {
	{"name", offsetof(struct rpc_resize_lvol_bdev, name), spdk_json_decode_string},
	{"size", offsetof(struct rpc_resize_lvol_bdev, size), spdk_json_decode_uint64},
};

static void
_spdk_rpc_resize_lvol_bdev_cb(void *cb_arg, int lvolerrno)
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

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void __attribute__((unused))
spdk_rpc_resize_lvol_bdev(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_resize_lvol_bdev req = {};
	int rc = 0;

	SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "Resizing lvol\n");

	if (spdk_json_decode_object(params, rpc_resize_lvol_bdev_decoders,
				    SPDK_COUNTOF(rpc_resize_lvol_bdev_decoders),
				    &req)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = vbdev_lvol_resize(req.name, (size_t)req.size, _spdk_rpc_resize_lvol_bdev_cb, request);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_resize_lvol_bdev(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_resize_lvol_bdev(&req);
}

/* Logical volume resize feature is disabled, as it is currently work in progress
SPDK_RPC_REGISTER("resize_lvol_bdev", spdk_rpc_resize_lvol_bdev) */

struct rpc_get_lvol_stores {
	char *uuid;
	char *lvs_name;
};

static void
free_rpc_get_lvol_stores(struct rpc_get_lvol_stores *req)
{
	free(req->uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_get_lvol_stores_decoders[] = {
	{"uuid", offsetof(struct rpc_get_lvol_stores, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_get_lvol_stores, lvs_name), spdk_json_decode_string, true},
};

static void
spdk_rpc_dump_lvol_store_info(struct spdk_json_write_ctx *w, struct lvol_store_bdev *lvs_bdev)
{
	struct spdk_blob_store *bs;
	uint64_t cluster_size, block_size;
	char uuid[UUID_STRING_LEN];

	bs = lvs_bdev->lvs->blobstore;
	cluster_size = spdk_bs_get_cluster_size(bs);
	/* Block size of lvols is always size of blob store page */
	block_size = spdk_bs_get_page_size(bs);

	spdk_json_write_object_begin(w);

	uuid_unparse(lvs_bdev->lvs->uuid, uuid);
	spdk_json_write_name(w, "uuid");
	spdk_json_write_string(w, uuid);

	spdk_json_write_name(w, "name");
	spdk_json_write_string(w, lvs_bdev->lvs->name);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(lvs_bdev->bdev));

	spdk_json_write_name(w, "total_data_clusters");
	spdk_json_write_uint64(w, spdk_bs_total_data_cluster_count(bs));

	spdk_json_write_name(w, "free_clusters");
	spdk_json_write_uint64(w, spdk_bs_free_cluster_count(bs));

	spdk_json_write_name(w, "block_size");
	spdk_json_write_uint64(w, block_size);

	spdk_json_write_name(w, "cluster_size");
	spdk_json_write_uint64(w, cluster_size);

	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_lvol_stores(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_get_lvol_stores req = {};
	struct spdk_json_write_ctx *w;
	struct lvol_store_bdev *lvs_bdev = NULL;
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_get_lvol_stores_decoders,
					    SPDK_COUNTOF(rpc_get_lvol_stores_decoders),
					    &req)) {
			SPDK_INFOLOG(SPDK_LOG_LVOL_RPC, "spdk_json_decode_object failed\n");
			rc = -EINVAL;
			goto invalid;
		}

		rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
		if (rc != 0) {
			goto invalid;
		}

		lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
		if (lvs_bdev == NULL) {
			rc = -ENODEV;
			goto invalid;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	if (lvs_bdev != NULL) {
		spdk_rpc_dump_lvol_store_info(w, lvs_bdev);
	} else {
		for (lvs_bdev = vbdev_lvol_store_first(); lvs_bdev != NULL;
		     lvs_bdev = vbdev_lvol_store_next(lvs_bdev)) {
			spdk_rpc_dump_lvol_store_info(w, lvs_bdev);
		}
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	free_rpc_get_lvol_stores(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_get_lvol_stores(&req);
}

SPDK_RPC_REGISTER("get_lvol_stores", spdk_rpc_get_lvol_stores)
