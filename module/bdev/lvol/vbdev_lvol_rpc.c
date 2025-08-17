/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/bdev.h"
#include "spdk/util.h"
#include "vbdev_lvol.h"
#include "spdk/string.h"
#include "spdk/log.h"

SPDK_LOG_REGISTER_COMPONENT(lvol_rpc)

struct rpc_shallow_copy_status {
	uint32_t				operation_id;
	/*
	 * 0 means ongoing or successfully completed operation
	 * a negative value is the -errno of an aborted operation
	 */
	int					result;
	uint64_t				copied_clusters;
	uint64_t				total_clusters;
	LIST_ENTRY(rpc_shallow_copy_status)	link;
};

static uint32_t g_shallow_copy_count = 0;
static LIST_HEAD(, rpc_shallow_copy_status) g_shallow_copy_status_list = LIST_HEAD_INITIALIZER(
			&g_shallow_copy_status_list);

struct rpc_bdev_lvol_create_lvstore {
	char *lvs_name;
	char *bdev_name;
	uint32_t cluster_sz;
	char *clear_method;
	uint32_t num_md_pages_per_cluster_ratio;
	uint32_t md_page_size;
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
	{"num_md_pages_per_cluster_ratio", offsetof(struct rpc_bdev_lvol_create_lvstore, num_md_pages_per_cluster_ratio), spdk_json_decode_uint32, true},
	{"md_page_size", offsetof(struct rpc_bdev_lvol_create_lvstore, md_page_size), spdk_json_decode_uint32, true},
};

static void
rpc_lvol_store_construct_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = cb_arg;

	if (lvserrno != 0) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_uuid(w, &lvol_store->uuid);
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

	rc = vbdev_lvs_create_ext(req.bdev_name, req.lvs_name, req.cluster_sz, clear_method,
				  req.num_md_pages_per_cluster_ratio, req.md_page_size,
				  rpc_lvol_store_construct_cb, request);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}
	free_rpc_bdev_lvol_create_lvstore(&req);

	return;

cleanup:
	free_rpc_bdev_lvol_create_lvstore(&req);
}
SPDK_RPC_REGISTER("bdev_lvol_create_lvstore", rpc_bdev_lvol_create_lvstore, SPDK_RPC_RUNTIME)

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

struct rpc_bdev_lvol_create {
	char *uuid;
	char *lvs_name;
	char *lvol_name;
	uint64_t size_in_mib;
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
	{"size_in_mib", offsetof(struct rpc_bdev_lvol_create, size_in_mib), spdk_json_decode_uint64},
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

	rc = vbdev_lvol_create(lvs, req.lvol_name, req.size_in_mib * 1024 * 1024,
			       req.thin_provision, clear_method, rpc_bdev_lvol_create_cb, request);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

cleanup:
	free_rpc_bdev_lvol_create(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_create", rpc_bdev_lvol_create, SPDK_RPC_RUNTIME)

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

struct rpc_bdev_lvol_clone_bdev {
	/* name or UUID. Whichever is used, the UUID will be stored in the lvol's metadata. */
	char *bdev_name;
	char *lvs_name;
	char *clone_name;
};

static void
free_rpc_bdev_lvol_clone_bdev(struct rpc_bdev_lvol_clone_bdev *req)
{
	free(req->bdev_name);
	free(req->lvs_name);
	free(req->clone_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_clone_bdev_decoders[] = {
	{
		"bdev", offsetof(struct rpc_bdev_lvol_clone_bdev, bdev_name),
		spdk_json_decode_string, false
	},
	{
		"lvs_name", offsetof(struct rpc_bdev_lvol_clone_bdev, lvs_name),
		spdk_json_decode_string, false
	},
	{
		"clone_name", offsetof(struct rpc_bdev_lvol_clone_bdev, clone_name),
		spdk_json_decode_string, false
	},
};

static void
rpc_bdev_lvol_clone_bdev(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_clone_bdev req = {};
	struct spdk_bdev *bdev;
	struct spdk_lvol_store *lvs = NULL;
	struct spdk_lvol *lvol;
	int rc;

	SPDK_INFOLOG(lvol_rpc, "Cloning bdev\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_clone_bdev_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_clone_bdev_decoders), &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = vbdev_get_lvol_store_by_uuid_xor_name(NULL, req.lvs_name, &lvs);
	if (rc != 0) {
		SPDK_INFOLOG(lvol_rpc, "lvs_name '%s' not found\n", req.lvs_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "lvs does not exist");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.bdev_name);
	if (bdev == NULL) {
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' does not exist\n", req.bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev does not exist");
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(bdev);
	if (lvol != NULL && lvol->lvol_store == lvs) {
		SPDK_INFOLOG(lvol_rpc, "bdev '%s' is an lvol in lvstore '%s\n", req.bdev_name,
			     req.lvs_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "bdev is an lvol in same lvs as clone; "
						 "use bdev_lvol_clone instead");
		goto cleanup;
	}

	vbdev_lvol_create_bdev_clone(req.bdev_name, lvs, req.clone_name,
				     rpc_bdev_lvol_clone_cb, request);
cleanup:
	free_rpc_bdev_lvol_clone_bdev(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_clone_bdev", rpc_bdev_lvol_clone_bdev, SPDK_RPC_RUNTIME)

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

struct rpc_bdev_lvol_resize {
	char *name;
	uint64_t size_in_mib;
};

static void
free_rpc_bdev_lvol_resize(struct rpc_bdev_lvol_resize *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_lvol_resize, name), spdk_json_decode_string},
	{"size_in_mib", offsetof(struct rpc_bdev_lvol_resize, size_in_mib), spdk_json_decode_uint64},
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


	vbdev_lvol_resize(lvol, req.size_in_mib * 1024 * 1024, rpc_bdev_lvol_resize_cb, request);

cleanup:
	free_rpc_bdev_lvol_resize(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_resize", rpc_bdev_lvol_resize, SPDK_RPC_RUNTIME)

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
	struct spdk_uuid uuid;
	char *lvs_name, *lvol_name;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_delete_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* lvol is not degraded, get lvol via bdev name or alias */
	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev != NULL) {
		lvol = vbdev_lvol_get_from_bdev(bdev);
		if (lvol != NULL) {
			goto done;
		}
	}

	/* lvol is degraded, get lvol via UUID */
	if (spdk_uuid_parse(&uuid, req.name) == 0) {
		lvol = spdk_lvol_get_by_uuid(&uuid);
		if (lvol != NULL) {
			goto done;
		}
	}

	/* lvol is degraded, get lvol via lvs_name/lvol_name */
	lvol_name = strchr(req.name, '/');
	if (lvol_name != NULL) {
		*lvol_name = '\0';
		lvol_name++;
		lvs_name = req.name;
		lvol = spdk_lvol_get_by_names(lvs_name, lvol_name);
		if (lvol != NULL) {
			goto done;
		}
	}

	/* Could not find lvol, degraded or not. */
	spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
	goto cleanup;

done:
	vbdev_lvol_destroy(lvol, rpc_bdev_lvol_delete_cb, request);

cleanup:
	free_rpc_bdev_lvol_delete(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_delete", rpc_bdev_lvol_delete, SPDK_RPC_RUNTIME)

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

	bs = lvs_bdev->lvs->blobstore;
	cluster_size = spdk_bs_get_cluster_size(bs);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uuid(w, "uuid", &lvs_bdev->lvs->uuid);
	spdk_json_write_named_string(w, "name", lvs_bdev->lvs->name);
	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(lvs_bdev->bdev));
	spdk_json_write_named_uint64(w, "total_data_clusters", spdk_bs_total_data_cluster_count(bs));
	spdk_json_write_named_uint64(w, "free_clusters", spdk_bs_free_cluster_count(bs));
	spdk_json_write_named_uint64(w, "block_size", spdk_bs_get_io_unit_size(bs));
	spdk_json_write_named_uint64(w, "cluster_size", cluster_size);
	spdk_json_write_named_uint64(w, "max_growable_size", spdk_bs_get_max_growable_size(bs));

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

struct rpc_bdev_lvol_get_lvols {
	char *lvs_uuid;
	char *lvs_name;
};

static void
free_rpc_bdev_lvol_get_lvols(struct rpc_bdev_lvol_get_lvols *req)
{
	free(req->lvs_uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_get_lvols_decoders[] = {
	{"lvs_uuid", offsetof(struct rpc_bdev_lvol_get_lvols, lvs_uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_get_lvols, lvs_name), spdk_json_decode_string, true},
};

static void
rpc_dump_lvol(struct spdk_json_write_ctx *w, struct spdk_lvol *lvol)
{
	struct spdk_lvol_store *lvs = lvol->lvol_store;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string_fmt(w, "alias", "%s/%s", lvs->name, lvol->name);
	spdk_json_write_named_string(w, "uuid", lvol->uuid_str);
	spdk_json_write_named_string(w, "name", lvol->name);
	spdk_json_write_named_bool(w, "is_thin_provisioned", spdk_blob_is_thin_provisioned(lvol->blob));
	spdk_json_write_named_bool(w, "is_snapshot", spdk_blob_is_snapshot(lvol->blob));
	spdk_json_write_named_bool(w, "is_clone", spdk_blob_is_clone(lvol->blob));
	spdk_json_write_named_bool(w, "is_esnap_clone", spdk_blob_is_esnap_clone(lvol->blob));
	spdk_json_write_named_bool(w, "is_degraded", spdk_blob_is_degraded(lvol->blob));
	spdk_json_write_named_uint64(w, "num_allocated_clusters",
				     spdk_blob_get_num_allocated_clusters(lvol->blob));

	spdk_json_write_named_object_begin(w, "lvs");
	spdk_json_write_named_string(w, "name", lvs->name);
	spdk_json_write_named_uuid(w, "uuid", &lvs->uuid);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
rpc_dump_lvols(struct spdk_json_write_ctx *w, struct lvol_store_bdev *lvs_bdev)
{
	struct spdk_lvol_store *lvs = lvs_bdev->lvs;
	struct spdk_lvol *lvol;

	TAILQ_FOREACH(lvol, &lvs->lvols, link) {
		if (lvol->ref_count == 0) {
			continue;
		}
		rpc_dump_lvol(w, lvol);
	}
}

static void
rpc_bdev_lvol_get_lvols(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_get_lvols req = {};
	struct spdk_json_write_ctx *w;
	struct lvol_store_bdev *lvs_bdev = NULL;
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_bdev_lvol_get_lvols_decoders,
					    SPDK_COUNTOF(rpc_bdev_lvol_get_lvols_decoders),
					    &req)) {
			SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "spdk_json_decode_object failed");
			goto cleanup;
		}

		rc = vbdev_get_lvol_store_by_uuid_xor_name(req.lvs_uuid, req.lvs_name, &lvs);
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
		rpc_dump_lvols(w, lvs_bdev);
	} else {
		for (lvs_bdev = vbdev_lvol_store_first(); lvs_bdev != NULL;
		     lvs_bdev = vbdev_lvol_store_next(lvs_bdev)) {
			rpc_dump_lvols(w, lvs_bdev);
		}
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_lvol_get_lvols(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_get_lvols", rpc_bdev_lvol_get_lvols, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_grow_lvstore {
	char *uuid;
	char *lvs_name;
};

static void
free_rpc_bdev_lvol_grow_lvstore(struct rpc_bdev_lvol_grow_lvstore *req)
{
	free(req->uuid);
	free(req->lvs_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_grow_lvstore_decoders[] = {
	{"uuid", offsetof(struct rpc_bdev_lvol_grow_lvstore, uuid), spdk_json_decode_string, true},
	{"lvs_name", offsetof(struct rpc_bdev_lvol_grow_lvstore, lvs_name), spdk_json_decode_string, true},
};

static void
rpc_bdev_lvol_grow_lvstore_cb(void *cb_arg, int lvserrno)
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
rpc_bdev_lvol_grow_lvstore(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_grow_lvstore req = {};
	struct spdk_lvol_store *lvs = NULL;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_lvol_grow_lvstore_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_grow_lvstore_decoders),
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
	spdk_bdev_update_bs_blockcnt(lvs->bs_dev);
	spdk_lvs_grow_live(lvs, rpc_bdev_lvol_grow_lvstore_cb, request);

cleanup:
	free_rpc_bdev_lvol_grow_lvstore(&req);
}
SPDK_RPC_REGISTER("bdev_lvol_grow_lvstore", rpc_bdev_lvol_grow_lvstore, SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_shallow_copy {
	char *src_lvol_name;
	char *dst_bdev_name;
};

struct rpc_bdev_lvol_shallow_copy_ctx {
	struct spdk_jsonrpc_request *request;
	struct rpc_shallow_copy_status *status;
};

static void
free_rpc_bdev_lvol_shallow_copy(struct rpc_bdev_lvol_shallow_copy *req)
{
	free(req->src_lvol_name);
	free(req->dst_bdev_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_shallow_copy_decoders[] = {
	{"src_lvol_name", offsetof(struct rpc_bdev_lvol_shallow_copy, src_lvol_name), spdk_json_decode_string},
	{"dst_bdev_name", offsetof(struct rpc_bdev_lvol_shallow_copy, dst_bdev_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_shallow_copy_cb(void *cb_arg, int lvolerrno)
{
	struct rpc_bdev_lvol_shallow_copy_ctx *ctx = cb_arg;

	ctx->status->result = lvolerrno;

	free(ctx);
}

static void
rpc_bdev_lvol_shallow_copy_status_cb(uint64_t copied_clusters, void *cb_arg)
{
	struct rpc_shallow_copy_status *status = cb_arg;

	status->copied_clusters = copied_clusters;
}

static void
rpc_bdev_lvol_start_shallow_copy(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_shallow_copy req = {};
	struct rpc_bdev_lvol_shallow_copy_ctx *ctx;
	struct spdk_lvol *src_lvol;
	struct spdk_bdev *src_lvol_bdev;
	struct rpc_shallow_copy_status *status;
	struct spdk_json_write_ctx *w;
	int rc;

	SPDK_INFOLOG(lvol_rpc, "Shallow copying lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_shallow_copy_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_shallow_copy_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	src_lvol_bdev = spdk_bdev_get_by_name(req.src_lvol_name);
	if (src_lvol_bdev == NULL) {
		SPDK_ERRLOG("lvol bdev '%s' does not exist\n", req.src_lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	src_lvol = vbdev_lvol_get_from_bdev(src_lvol_bdev);
	if (src_lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		SPDK_ERRLOG("Cannot allocate status entry for shallow copy of '%s'\n", req.src_lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	status->operation_id = ++g_shallow_copy_count;
	status->total_clusters = spdk_blob_get_num_allocated_clusters(src_lvol->blob);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Cannot allocate context for shallow copy of '%s'\n", req.src_lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		free(status);
		goto cleanup;
	}
	ctx->request = request;
	ctx->status = status;

	LIST_INSERT_HEAD(&g_shallow_copy_status_list, status, link);
	rc = vbdev_lvol_shallow_copy(src_lvol, req.dst_bdev_name,
				     rpc_bdev_lvol_shallow_copy_status_cb, status,
				     rpc_bdev_lvol_shallow_copy_cb, ctx);

	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-rc));
		LIST_REMOVE(status, link);
		free(ctx);
		free(status);
	} else {
		w = spdk_jsonrpc_begin_result(request);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "operation_id", status->operation_id);
		spdk_json_write_object_end(w);

		spdk_jsonrpc_end_result(request, w);
	}

cleanup:
	free_rpc_bdev_lvol_shallow_copy(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_start_shallow_copy", rpc_bdev_lvol_start_shallow_copy,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_shallow_copy_status {
	char		*src_lvol_name;
	uint32_t	operation_id;
};

static void
free_rpc_bdev_lvol_shallow_copy_status(struct rpc_bdev_lvol_shallow_copy_status *req)
{
	free(req->src_lvol_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_shallow_copy_status_decoders[] = {
	{"operation_id", offsetof(struct rpc_bdev_lvol_shallow_copy_status, operation_id), spdk_json_decode_uint32},
};

static void
rpc_bdev_lvol_check_shallow_copy(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_shallow_copy_status req = {};
	struct rpc_shallow_copy_status *status;
	struct spdk_json_write_ctx *w;
	uint64_t copied_clusters, total_clusters;
	int result;

	SPDK_INFOLOG(lvol_rpc, "Shallow copy check\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_shallow_copy_status_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_shallow_copy_status_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	LIST_FOREACH(status, &g_shallow_copy_status_list, link) {
		if (status->operation_id == req.operation_id) {
			break;
		}
	}

	if (!status) {
		SPDK_ERRLOG("operation id '%d' does not exist\n", req.operation_id);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	copied_clusters = status->copied_clusters;
	total_clusters = status->total_clusters;
	result = status->result;

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint64(w, "copied_clusters", copied_clusters);
	spdk_json_write_named_uint64(w, "total_clusters", total_clusters);
	if (copied_clusters < total_clusters && result == 0) {
		spdk_json_write_named_string(w, "state", "in progress");
	} else if (copied_clusters == total_clusters && result == 0) {
		spdk_json_write_named_string(w, "state", "complete");
		LIST_REMOVE(status, link);
		free(status);
	} else {
		spdk_json_write_named_string(w, "state", "error");
		spdk_json_write_named_string(w, "error", spdk_strerror(-result));
		LIST_REMOVE(status, link);
		free(status);
	}

	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_lvol_shallow_copy_status(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_check_shallow_copy", rpc_bdev_lvol_check_shallow_copy,
		  SPDK_RPC_RUNTIME)

struct rpc_bdev_lvol_set_parent {
	char *lvol_name;
	char *parent_name;
};

static void
free_rpc_bdev_lvol_set_parent(struct rpc_bdev_lvol_set_parent *req)
{
	free(req->lvol_name);
	free(req->parent_name);
}

static const struct spdk_json_object_decoder rpc_bdev_lvol_set_parent_decoders[] = {
	{"lvol_name", offsetof(struct rpc_bdev_lvol_set_parent, lvol_name), spdk_json_decode_string},
	{"parent_name", offsetof(struct rpc_bdev_lvol_set_parent, parent_name), spdk_json_decode_string},
};

static void
rpc_bdev_lvol_set_parent_cb(void *cb_arg, int lvolerrno)
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
rpc_bdev_lvol_set_parent(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_set_parent req = {};
	struct spdk_lvol *lvol, *snapshot;
	struct spdk_bdev *lvol_bdev, *snapshot_bdev;

	SPDK_INFOLOG(lvol_rpc, "Set parent of lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_set_parent_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_set_parent_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	lvol_bdev = spdk_bdev_get_by_name(req.lvol_name);
	if (lvol_bdev == NULL) {
		SPDK_ERRLOG("lvol bdev '%s' does not exist\n", req.lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(lvol_bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	snapshot_bdev = spdk_bdev_get_by_name(req.parent_name);
	if (snapshot_bdev == NULL) {
		SPDK_ERRLOG("snapshot bdev '%s' does not exist\n", req.parent_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	snapshot = vbdev_lvol_get_from_bdev(snapshot_bdev);
	if (snapshot == NULL) {
		SPDK_ERRLOG("snapshot does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	spdk_lvol_set_parent(lvol, snapshot, rpc_bdev_lvol_set_parent_cb, request);

cleanup:
	free_rpc_bdev_lvol_set_parent(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_set_parent", rpc_bdev_lvol_set_parent, SPDK_RPC_RUNTIME)

static void
rpc_bdev_lvol_set_parent_bdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_lvol_set_parent req = {};
	struct spdk_lvol *lvol;
	struct spdk_bdev *lvol_bdev;

	SPDK_INFOLOG(lvol_rpc, "Set external parent of lvol\n");

	if (spdk_json_decode_object(params, rpc_bdev_lvol_set_parent_decoders,
				    SPDK_COUNTOF(rpc_bdev_lvol_set_parent_decoders),
				    &req)) {
		SPDK_INFOLOG(lvol_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	lvol_bdev = spdk_bdev_get_by_name(req.lvol_name);
	if (lvol_bdev == NULL) {
		SPDK_ERRLOG("lvol bdev '%s' does not exist\n", req.lvol_name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	lvol = vbdev_lvol_get_from_bdev(lvol_bdev);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	vbdev_lvol_set_external_parent(lvol, req.parent_name, rpc_bdev_lvol_set_parent_cb, request);

cleanup:
	free_rpc_bdev_lvol_set_parent(&req);
}

SPDK_RPC_REGISTER("bdev_lvol_set_parent_bdev", rpc_bdev_lvol_set_parent_bdev,
		  SPDK_RPC_RUNTIME)
