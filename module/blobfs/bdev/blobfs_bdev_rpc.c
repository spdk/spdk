/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/blobfs_bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define MIN_CLUSTER_SZ (1024 * 1024)

struct rpc_blobfs_set_cache_size {
	uint64_t size_in_mb;
};

static const struct spdk_json_object_decoder rpc_blobfs_set_cache_size_decoders[] = {
	{"size_in_mb", offsetof(struct rpc_blobfs_set_cache_size, size_in_mb), spdk_json_decode_uint64},
};

static void
rpc_blobfs_set_cache_size(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_blobfs_set_cache_size req;
	int rc;

	if (spdk_json_decode_object(params, rpc_blobfs_set_cache_size_decoders,
				    SPDK_COUNTOF(rpc_blobfs_set_cache_size_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		return;
	}

	if (req.size_in_mb == 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		return;
	}

	rc = spdk_fs_set_cache_size(req.size_in_mb);

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}

SPDK_RPC_REGISTER("blobfs_set_cache_size", rpc_blobfs_set_cache_size,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

struct rpc_blobfs_detect {
	char *bdev_name;

	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_blobfs_detect(struct rpc_blobfs_detect *req)
{
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_blobfs_detect_decoders[] = {
	{"bdev_name", offsetof(struct rpc_blobfs_detect, bdev_name), spdk_json_decode_string},
};

static void
_rpc_blobfs_detect_done(void *cb_arg, int fserrno)
{
	struct rpc_blobfs_detect *req = cb_arg;
	bool existed = true;

	if (fserrno == -EILSEQ) {
		/* There is no blobfs existing on bdev */
		existed = false;
	} else if (fserrno != 0) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-fserrno));
		free_rpc_blobfs_detect(req);
		return;
	}

	spdk_jsonrpc_send_bool_response(req->request, existed);

	free_rpc_blobfs_detect(req);
}

static void
rpc_blobfs_detect(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_blobfs_detect *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_blobfs_detect request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_blobfs_detect_decoders,
				    SPDK_COUNTOF(rpc_blobfs_detect_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		free_rpc_blobfs_detect(req);

		return;
	}

	req->request = request;
	spdk_blobfs_bdev_detect(req->bdev_name, _rpc_blobfs_detect_done, req);
}

SPDK_RPC_REGISTER("blobfs_detect", rpc_blobfs_detect, SPDK_RPC_RUNTIME)

struct rpc_blobfs_create {
	char *bdev_name;
	uint64_t cluster_sz;

	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_blobfs_create(struct rpc_blobfs_create *req)
{
	free(req->bdev_name);
	free(req);
}

static int
rpc_decode_cluster_sz(const struct spdk_json_val *val, void *out)
{
	uint64_t *cluster_sz = out;
	char *sz_str = NULL;
	bool has_prefix;
	int rc;

	rc = spdk_json_decode_string(val, &sz_str);
	if (rc) {
		SPDK_NOTICELOG("Invalid parameter value: cluster_sz\n");
		return -EINVAL;
	}

	rc = spdk_parse_capacity(sz_str, cluster_sz, &has_prefix);
	free(sz_str);

	if (rc || *cluster_sz % PAGE_SIZE != 0 || *cluster_sz < MIN_CLUSTER_SZ) {
		SPDK_NOTICELOG("Invalid parameter value: cluster_sz\n");
		return -EINVAL;
	}

	SPDK_DEBUGLOG(blobfs_bdev_rpc, "cluster_sz of blobfs: %" PRId64 "\n", *cluster_sz);
	return 0;
}

static const struct spdk_json_object_decoder rpc_blobfs_create_decoders[] = {
	{"bdev_name", offsetof(struct rpc_blobfs_create, bdev_name), spdk_json_decode_string},
	{"cluster_sz", offsetof(struct rpc_blobfs_create, cluster_sz), rpc_decode_cluster_sz, true},
};

static void
_rpc_blobfs_create_done(void *cb_arg, int fserrno)
{
	struct rpc_blobfs_create *req = cb_arg;

	if (fserrno != 0) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-fserrno));

		return;
	}

	spdk_jsonrpc_send_bool_response(req->request, true);

	free_rpc_blobfs_create(req);
}

static void
rpc_blobfs_create(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_blobfs_create *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_blobfs_create request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_blobfs_create_decoders,
				    SPDK_COUNTOF(rpc_blobfs_create_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		free_rpc_blobfs_create(req);

		return;
	}

	req->request = request;
	spdk_blobfs_bdev_create(req->bdev_name, req->cluster_sz, _rpc_blobfs_create_done, req);
}

SPDK_RPC_REGISTER("blobfs_create", rpc_blobfs_create, SPDK_RPC_RUNTIME)

SPDK_LOG_REGISTER_COMPONENT(blobfs_bdev_rpc)
#ifdef SPDK_CONFIG_FUSE

struct rpc_blobfs_mount {
	char *bdev_name;
	char *mountpoint;

	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_blobfs_mount(struct rpc_blobfs_mount *req)
{
	free(req->bdev_name);
	free(req->mountpoint);
	free(req);
}

static const struct spdk_json_object_decoder rpc_blobfs_mount_decoders[] = {
	{"bdev_name", offsetof(struct rpc_blobfs_mount, bdev_name), spdk_json_decode_string},
	{"mountpoint", offsetof(struct rpc_blobfs_mount, mountpoint), spdk_json_decode_string},
};

static void
_rpc_blobfs_mount_done(void *cb_arg, int fserrno)
{
	struct rpc_blobfs_mount *req = cb_arg;

	if (fserrno == -EILSEQ) {
		/* There is no blobfs existing on bdev */
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No blobfs detected on given bdev");

		return;
	} else if (fserrno != 0) {
		spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-fserrno));

		return;
	}

	spdk_jsonrpc_send_bool_response(req->request, true);

	free_rpc_blobfs_mount(req);
}

static void
rpc_blobfs_mount(struct spdk_jsonrpc_request *request,
		 const struct spdk_json_val *params)
{
	struct rpc_blobfs_mount *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("could not allocate rpc_blobfs_mount request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, rpc_blobfs_mount_decoders,
				    SPDK_COUNTOF(rpc_blobfs_mount_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");

		free_rpc_blobfs_mount(req);

		return;
	}

	req->request = request;
	spdk_blobfs_bdev_mount(req->bdev_name, req->mountpoint, _rpc_blobfs_mount_done, req);
}

SPDK_RPC_REGISTER("blobfs_mount", rpc_blobfs_mount, SPDK_RPC_RUNTIME)

#endif
