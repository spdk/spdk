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
#include "spdk/log.h"

SPDK_LOG_REGISTER_COMPONENT(lvol_rpc)

struct rpc_bdev_lvol_create_lvstore {
	char *lvs_name;
	char *bdev_name;
	uint32_t cluster_sz;
	char *clear_method;
};

static int
vbdev_get_lvol_store_by_uuid_xor_name(const char *uuid, const char *lvs_name,
				      struct spdk_lvol_store **lvs)
{
	if ((uuid == NULL && lvs_name == NULL)) {
		SPDK_INFOLOG(lvol_rpc, "lvs UUID nor lvs name specified\n");
		return -EINVAL;
	} else if ((uuid && lvs_name)) {
		SPDK_INFOLOG(lvol_rpc, "both lvs UUID '%s' and lvs name '%s' specified\n", uuid,
			     lvs_name);
		return -EINVAL;
	} else if (uuid) {
		*lvs = vbdev_get_lvol_store_by_uuid(uuid);

		if (*lvs == NULL) {
			SPDK_INFOLOG(lvol_rpc, "blobstore with UUID '%s' not found\n", uuid);
			return -ENODEV;
		}
	} else if (lvs_name) {

		*lvs = vbdev_get_lvol_store_by_name(lvs_name);

		if (*lvs == NULL) {
			SPDK_INFOLOG(lvol_rpc, "blobstore with name '%s' not found\n", lvs_name);
			return -ENODEV;
		}
	}
	return 0;
}

static void
free_rpc_bdev_lvol_create_lvstore(struct rpc_bdev_lvol_create_lvstore *req)
{
	free(req->bdev_name);
	free(req->lvs_name);
	free(req->clear_method);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_create_lvstore_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_lvol_create_lvstore, bdev_name), spdk_json_decode_string},
	{"cluster_sz", offsetof(struct rpc_bdev_lvol_create_lvstore, cluster_sz), spdk_json_decode_uint32, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_create_lvstore, lvs_name), spdk_json_decode_string},
	{"clear_method", offsetof(struct rpc_bdev_lvol_create_lvstore, clear_method), spdk_json_decode_string, true},
};

static void
rpc_lvol_store_construct_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct spdk_json_write_ctx *w;
	char lvol_store_uuid[SPDK_UUID_STRING_LEN];
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvserrno != 0) {
		goto invalid;
	}

	spdk_uuid_fmt_lower(lvol_store_uuid, sizeof(lvol_store_uuid), &lvol_store->uuid);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, lvol_store_uuid);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
}

static void
rpc_bdev_lvol_create_lvstore(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_create_lvstore req = {};
	int rc = 0;
	enum lvs_clear_method clear_method;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_create_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_create_lvstore_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.clear_method != NULL) {
		if (!strcasecmp(req.clear_method, "none")) {
			clear_method = LVS_CLEAR_WITH_NONE;
		} else if (!strcasecmp(req.clear_method, "unmap")) {
			clear_method = LVS_CLEAR_WITH_UNMAP;
		} else if (!strcasecmp(req.clear_method, "write_zeroes")) {
			clear_method = LVS_CLEAR_WITH_WRITE_ZEROES;
		} else {
			spdk_jsonrpc_send_error_response(request, -EINVAL, "Invalid clear_method parameter");
			goto cleanup;
		}
	} else {
		clear_method = LVS_CLEAR_WITH_UNMAP;
	}

	rc = vbdev_lvs_create(req.bdev_name, req.lvs_name, req.cluster_sz, clear_method,
			      rpc_lvol_store_construct_cb, request);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, -rc, spdk_strerror(rc));
		goto cleanup;
	}
	free_rpc_bdev_lvol_create_lvstore(&req);

	return;

cleanup:
	free_rpc_bdev_lvol_create_lvstore(&req);
}
SPDK_RPC_REGISTER("bdev_lvol_create_lvstore", rpc_bdev_lvol_create_lvstore, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_create_lvstore, construct_lvol_store)

struct rpc_bdev_lvol_rename_lvstore {
	char *old_name;
	char *new_name;
};

static void
free_rpc_bdev_lvol_rename_lvstore(struct rpc_bdev_lvol_rename_lvstore *req)
{
	free(req->old_name);
	free(req->new_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_rename_lvstore_decoders[] = {
	{"old_name", offsetof(struct rpc_bdev_lvol_rename_lvstore, old_name), spdk_json_decode_string},
	{"new_name", offsetof(struct rpc_bdev_lvol_rename_lvstore, new_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_rename_lvstore_cb(void *cb_arg, int lvserrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvserrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
}

static void
rpc_bdev_lvol_rename_lvstore(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_rename_lvstore req = {};
	struct spdk_lvol_store *lvs;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_rename_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_rename_lvstore_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	lvs = vbdev_get_lvol_store_by_name(req.old_name);
	if (lvs == NULL) {
		SPDK_INFOLOG(lvol_rpc, "no lvs existing for given name\n");
		spdk_jsonrpc_send_error_response_fmt(request, -ENOENT, "Lvol store %s not found", req.old_name);
		goto cleanup;
	}

	vbdev_lvs_rename(lvs, req.new_name, rpc_bdev_lvol_rename_lvstore_cb, request);

cleanup:
	free_rpc_bdev_lvol_rename_lvstore(&req);
}
SPDK_RPC_REGISTER("bdev_lvol_rename_lvstore", rpc_bdev_lvol_rename_lvstore, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_rename_lvstore, rename_lvol_store)

struct rpc_bdev_lvol_delete_lvstore {
	char *uuid;
	char *lvs_name;
};

static void
free_rpc_bdev_lvol_delete_lvstore(struct rpc_bdev_lvol_delete_lvstore *req)
{
	free(req->uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_delete_lvstore_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_delete_lvstore, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_delete_lvstore, lvs_name), spdk_json_decode_string, true},
};

static void
rpc_lvol_store_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvserrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvserrno));
}

static void
rpc_bdev_lvol_delete_lvstore(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_delete_lvstore req = {};
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_delete_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_delete_lvstore_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	vbdev_lvs_destruct(lvs, rpc_lvol_store_destroy_cb, request);

cleanup:
	free_rpc_bdev_lvol_delete_lvstore(&req);
}
SPDK_RPC_REGISTER("bdev_lvol_delete_lvstore", rpc_bdev_lvol_delete_lvstore, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_delete_lvstore, destroy_lvol_store)

struct rpc_bdev_lvol_create {
	char *uuid;
	char *lvs_name;
	char *lvol_name;
	uint64_t size;
	bool thin_provision;
	char *clear_method;
};

static void
free_rpc_bdev_lvol_create(struct rpc_bdev_lvol_create *req)
{
	free(req->uuid);
	free(req->lvs_name);
	free(req->lvol_name);
	free(req->clear_method);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_create_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_create, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_create, lvs_name), spdk_json_decode_string, true},
	{"lvol_name", offsetof(struct rpc_bdev_lvol_create, lvol_name), spdk_json_decode_string},
	{"size", offsetof(struct rpc_bdev_lvol_create, size), spdk_json_decode_uint64},
	{"thin_provision", offsetof(struct rpc_bdev_lvol_create, thin_provision), spdk_json_decode_bool, true},
	{"clear_method", offsetof(struct rpc_bdev_lvol_create, clear_method), spdk_json_decode_string, true},
};

static void
rpc_bdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, lvol->unique_id);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_create req = {};
	enum lvol_clear_method clear_method;
	int rc = 0;
	struct spdk_lvol_store *lvs = NULL;

	SPDK_INFOLOG(lvol_rpc, "Creating blob\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_create_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	if (req.clear_method != NULL) {
		if (!strcasecmp(req.clear_method, "none")) {
			clear_method = LVOL_CLEAR_WITH_NONE;
		} else if (!strcasecmp(req.clear_method, "unmap")) {
			clear_method = LVOL_CLEAR_WITH_UNMAP;
		} else if (!strcasecmp(req.clear_method, "write_zeroes")) {
			clear_method = LVOL_CLEAR_WITH_WRITE_ZEROES;
		} else {
			spdk_jsonrpc_send_error_response(request, -EINVAL, "Invalid clean_method option");
			goto cleanup;
		}
	} else {
		clear_method = LVOL_CLEAR_WITH_DEFAULT;
	}

	rc = vbdev_lvol_create(lvs, req.lvol_name, req.size, req.thin_provision,
			       clear_method, rpc_bdev_lvol_create_cb, request);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

cleanup:
	free_rpc_bdev_lvol_create(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_create", rpc_bdev_lvol_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_create, construct_lvol_bdev)

struct rpc_bdev_lvol_snapshot {
	char *lvol_name;
	char *snapshot_name;
};

static void
free_rpc_bdev_lvol_snapshot(struct rpc_bdev_lvol_snapshot *req)
{
	free(req->lvol_name);
	free(req->snapshot_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_snapshot_decoders[] = {
	{"lvol_name", offsetof(struct rpc_bdev_lvol_snapshot, lvol_name), spdk_json_decode_string},
	{"snapshot_name", offsetof(struct rpc_bdev_lvol_snapshot, snapshot_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_snapshot_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, lvol->unique_id);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_snapshot(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_snapshot req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Snapshotting blob\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_snapshot_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_snapshot_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.lvol_name);
	if (bdev == NULL) {
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' does not exist\n", req.lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_create_snapshot(lvol, req.snapshot_name, rpc_bdev_lvol_snapshot_cb, request);

cleanup:
	free_rpc_bdev_lvol_snapshot(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_snapshot", rpc_bdev_lvol_snapshot, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_snapshot, snapshot_lvol_bdev)

struct rpc_bdev_lvol_clone {
	char *snapshot_name;
	char *clone_name;
};

static void
free_rpc_bdev_lvol_clone(struct rpc_bdev_lvol_clone *req)
{
	free(req->snapshot_name);
	free(req->clone_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_clone_decoders[] = {
	{"snapshot_name", offsetof(struct rpc_bdev_lvol_clone, snapshot_name), spdk_json_decode_string},
	{"clone_name", offsetof(struct rpc_bdev_lvol_clone, clone_name), spdk_json_decode_string, true},
};

static void
rpc_bdev_lvol_clone_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, lvol->unique_id);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_clone(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_clone req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Cloning blob\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_clone_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_clone_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.snapshot_name);
	if (bdev == NULL) {
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' does not exist\n", req.snapshot_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_create_clone(lvol, req.clone_name, rpc_bdev_lvol_clone_cb, request);

cleanup:
	free_rpc_bdev_lvol_clone(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_clone", rpc_bdev_lvol_clone, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_clone, clone_lvol_bdev)

struct rpc_bdev_lvol_rename {
	char *old_name;
	char *new_name;
};

static void
free_rpc_bdev_lvol_rename(struct rpc_bdev_lvol_rename *req)
{
	free(req->old_name);
	free(req->new_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_rename_decoders[] = {
	{"old_name", offsetof(struct rpc_bdev_lvol_rename, old_name), spdk_json_decode_string},
	{"new_name", offsetof(struct rpc_bdev_lvol_rename, new_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_rename(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_rename req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Renaming lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_rename_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_rename_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.old_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.old_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_rename(lvol, req.new_name, rpc_bdev_lvol_rename_cb, request);

cleanup:
	free_rpc_bdev_lvol_rename(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_rename", rpc_bdev_lvol_rename, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_rename, rename_lvol_bdev)

struct rpc_bdev_lvol_inflate {
	char *name;
};

static void
free_rpc_bdev_lvol_inflate(struct rpc_bdev_lvol_inflate *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_inflate_decoders[] = {
	{"name", offsetof(struct rpc_bdev_lvol_inflate, name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_inflate_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_inflate(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_inflate req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Inflating lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_inflate_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_inflate_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_lvol_inflate(lvol, rpc_bdev_lvol_inflate_cb, request);

cleanup:
	free_rpc_bdev_lvol_inflate(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_inflate", rpc_bdev_lvol_inflate, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_inflate, inflate_lvol_bdev)

static void
rpc_bdev_lvol_decouple_parent(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_inflate req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Decoupling parent of lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_inflate_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_inflate_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_lvol_decouple_parent(lvol, rpc_bdev_lvol_inflate_cb, request);

cleanup:
	free_rpc_bdev_lvol_inflate(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_decouple_parent", rpc_bdev_lvol_decouple_parent, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_decouple_parent, decouple_parent_lvol_bdev)

struct rpc_bdev_lvol_resize {
	char *name;
	uint64_t size;
};

static void
free_rpc_bdev_lvol_resize(struct rpc_bdev_lvol_resize *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_lvol_resize, name), spdk_json_decode_string},
	{"size", offsetof(struct rpc_bdev_lvol_resize, size), spdk_json_decode_uint64},
};

static void
rpc_bdev_lvol_resize_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_resize(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_resize req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Resizing lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_resize_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev for provided name %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_resize(lvol, req.size, rpc_bdev_lvol_resize_cb, request);

cleanup:
	free_rpc_bdev_lvol_resize(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_resize", rpc_bdev_lvol_resize, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_resize, resize_lvol_bdev)

struct rpc_set_ro_lvol_bdev {
	char *name;
};

static void
free_rpc_set_ro_lvol_bdev(struct rpc_set_ro_lvol_bdev *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_set_ro_lvol_bdev_decoders[] = {
	{"name", offsetof(struct rpc_set_ro_lvol_bdev, name), spdk_json_decode_string},
};

static void
rpc_set_ro_lvol_bdev_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_set_read_only(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_set_ro_lvol_bdev req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	SPDK_INFOLOG(lvol_rpc, "Setting lvol as read only\n");

	if (spdk_json_decode_object(params, rpc_set_ro_lvol_bdev_decoders,
				    SPDK_COUNTOF(rpc_set_ro_lvol_bdev_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Missing name parameter");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev for provided name %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_set_read_only(lvol, rpc_set_ro_lvol_bdev_cb, request);

cleanup:
	free_rpc_set_ro_lvol_bdev(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_set_read_only", rpc_bdev_lvol_set_read_only, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_set_read_only, set_read_only_lvol_bdev)

struct rpc_bdev_lvol_delete {
	char *name;
};

static void
free_rpc_bdev_lvol_delete(struct rpc_bdev_lvol_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_lvol_delete, name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_delete_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvolerrno != 0) {
		goto invalid;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-lvolerrno));
}

static void
rpc_bdev_lvol_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_delete req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_delete_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev for provided name %s\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_destroy(lvol, rpc_bdev_lvol_delete_cb, request);

cleanup:
	free_rpc_bdev_lvol_delete(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_delete", rpc_bdev_lvol_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_delete, destroy_lvol_bdev)

struct rpc_bdev_lvol_get_lvstores {
	char *uuid;
	char *lvs_name;
};

static void
free_rpc_bdev_lvol_get_lvstores(struct rpc_bdev_lvol_get_lvstores *req)
{
	free(req->uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_get_lvstores_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_get_lvstores, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_get_lvstores, lvs_name), spdk_json_decode_string, true},
};

static void
rpc_dump_lvol_store_info(struct spdk_json_write_ctx *w, struct lvol_store_bdev *lvs_bdev)
{
	struct spdk_blob_store *bs;
	uint64_t cluster_size;
	char uuid[SPDK_UUID_STRING_LEN];

	bs = lvs_bdev->lvs->blobstore;
	cluster_size = spdk_bs_get_cluster_size(bs);

	spdk_json_write_object_begin(w);

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &lvs_bdev->lvs->uuid);
	spdk_json_write_named_string(w, "uuid", uuid);

	spdk_json_write_named_string(w, "name", lvs_bdev->lvs->name);

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(lvs_bdev->bdev));

	spdk_json_write_named_uint64(w, "total_data_clusters", spdk_bs_total_data_cluster_count(bs));

	spdk_json_write_named_uint64(w, "free_clusters", spdk_bs_free_cluster_count(bs));

	spdk_json_write_named_uint64(w, "block_size", spdk_bs_get_io_unit_size(bs));

	spdk_json_write_named_uint64(w, "cluster_size", cluster_size);

	spdk_json_write_object_end(w);
}

static void
rpc_bdev_lvol_get_lvstores(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_get_lvstores req = {};
	struct spdk_json_write_ctx *w;
	struct lvol_store_bdev *lvs_bdev = NULL;
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_lvol_get_lvstores_decoders,
					    SPDK_COUNTOF(rpc_bdev_lvol_get_lvstores_decoders),
					    &req)) {
			SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}

		rc = vbdev_get_lvol_store_by_uuid_xor_name(req.uuid, req.lvs_name, &lvs);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
			goto cleanup;
		}

		lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
		if (lvs_bdev == NULL) {
			spdk_jsonrpc_send_error_response(request, ENODEV, spdk_strerror(-ENODEV));
			goto cleanup;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (lvs_bdev != NULL) {
		rpc_dump_lvol_store_info(w, lvs_bdev);
	} else {
		for (lvs_bdev = vbdev_lvol_store_first(); lvs_bdev != NULL;
		     lvs_bdev = vbdev_lvol_store_next(lvs_bdev)) {
			rpc_dump_lvol_store_info(w, lvs_bdev);
		}
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_lvol_get_lvstores(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_get_lvstores", rpc_bdev_lvol_get_lvstores, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_lvol_get_lvstores, get_lvol_stores)
