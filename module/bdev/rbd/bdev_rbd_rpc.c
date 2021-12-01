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

#include "bdev_rbd.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_create_rbd {
	char *name;
	char *user_id;
	char *pool_name;
	char *rbd_name;
	uint32_t block_size;
	char **config;
	char *cluster_name;
	char *uuid;
};

static void
free_rpc_create_rbd(struct rpc_create_rbd *req)
{
	free(req->name);
	free(req->user_id);
	free(req->pool_name);
	free(req->rbd_name);
	bdev_rbd_free_config(req->config);
	free(req->cluster_name);
	free(req->uuid);
}

static int
bdev_rbd_decode_config(const struct spdk_json_val *values, void *out)
{
	char ***map = out;
	char **entry;
	uint32_t i;

	if (values->type == SPDK_JSON_VAL_NULL) {
		/* treated like empty object: empty config */
		*map = calloc(1, sizeof(**map));
		if (!*map) {
			return -1;
		}
		return 0;
	}

	if (values->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		return -1;
	}

	*map = calloc(values->len + 1, sizeof(**map));
	if (!*map) {
		return -1;
	}

	for (i = 0, entry = *map; i < values->len;) {
		const struct spdk_json_val *name = &values[i + 1];
		const struct spdk_json_val *v = &values[i + 2];
		/* Here we catch errors like invalid types. */
		if (!(entry[0] = spdk_json_strdup(name)) ||
		    !(entry[1] = spdk_json_strdup(v))) {
			bdev_rbd_free_config(*map);
			*map = NULL;
			return -1;
		}
		i += 1 + spdk_json_val_len(v);
		entry += 2;
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_create_rbd_decoders[] = {
	{"name", offsetof(struct rpc_create_rbd, name), spdk_json_decode_string, true},
	{"user_id", offsetof(struct rpc_create_rbd, user_id), spdk_json_decode_string, true},
	{"pool_name", offsetof(struct rpc_create_rbd, pool_name), spdk_json_decode_string},
	{"rbd_name", offsetof(struct rpc_create_rbd, rbd_name), spdk_json_decode_string},
	{"block_size", offsetof(struct rpc_create_rbd, block_size), spdk_json_decode_uint32},
	{"config", offsetof(struct rpc_create_rbd, config), bdev_rbd_decode_config, true},
	{"cluster_name", offsetof(struct rpc_create_rbd, cluster_name), spdk_json_decode_string, true},
	{"uuid", offsetof(struct rpc_create_rbd, uuid), spdk_json_decode_string, true}
};

static void
rpc_bdev_rbd_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_create_rbd req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;
	struct spdk_uuid *uuid = NULL;
	struct spdk_uuid decoded_uuid;

	if (spdk_json_decode_object(params, rpc_create_rbd_decoders,
				    SPDK_COUNTOF(rpc_create_rbd_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_rbd, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
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

	rc = bdev_rbd_create(&bdev, req.name, req.user_id, req.pool_name,
			     (const char *const *)req.config,
			     req.rbd_name,
			     req.block_size, req.cluster_name, uuid);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_create_rbd(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_create", rpc_bdev_rbd_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_rbd_create, construct_rbd_bdev)

struct rpc_bdev_rbd_delete {
	char *name;
};

static void
free_rpc_bdev_rbd_delete(struct rpc_bdev_rbd_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_rbd_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_delete, name), spdk_json_decode_string},
};

static void
_rpc_bdev_rbd_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_rbd_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_delete req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	bdev_rbd_delete(bdev, _rpc_bdev_rbd_delete_cb, request);

cleanup:
	free_rpc_bdev_rbd_delete(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_delete", rpc_bdev_rbd_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_rbd_delete, delete_rbd_bdev)

struct rpc_bdev_rbd_resize {
	char *name;
	uint64_t new_size;
};

static const struct spdk_json_object_decoder rpc_bdev_rbd_resize_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_resize, name), spdk_json_decode_string},
	{"new_size", offsetof(struct rpc_bdev_rbd_resize, new_size), spdk_json_decode_uint64}
};

static void
free_rpc_bdev_rbd_resize(struct rpc_bdev_rbd_resize *req)
{
	free(req->name);
}

static void
rpc_bdev_rbd_resize(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_resize req = {};
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_resize_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_resize_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	rc = bdev_rbd_resize(bdev, req.new_size);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);
cleanup:
	free_rpc_bdev_rbd_resize(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_resize", rpc_bdev_rbd_resize, SPDK_RPC_RUNTIME)

static void
free_rpc_register_cluster(struct cluster_register_info *req)
{
	free(req->name);
	free(req->user_id);
	bdev_rbd_free_config(req->config_param);
	free(req->config_file);
	free(req->key_file);
}

static const struct spdk_json_object_decoder rpc_register_cluster_decoders[] = {
	{"name", offsetof(struct cluster_register_info, name), spdk_json_decode_string, true},
	{"user_id", offsetof(struct cluster_register_info, user_id), spdk_json_decode_string, true},
	{"config_param", offsetof(struct cluster_register_info, config_param), bdev_rbd_decode_config, true},
	{"config_file", offsetof(struct cluster_register_info, config_file), spdk_json_decode_string, true},
	{"key_file", offsetof(struct cluster_register_info, key_file), spdk_json_decode_string, true}
};

static void
rpc_bdev_rbd_register_cluster(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct cluster_register_info req = {};
	int rc = 0;
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_register_cluster_decoders,
				    SPDK_COUNTOF(rpc_register_cluster_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_rbd, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_register_cluster(&req);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free_rpc_register_cluster(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_register_cluster", rpc_bdev_rbd_register_cluster, SPDK_RPC_RUNTIME)

struct rpc_bdev_rbd_unregister_cluster {
	char *name;
};

static void
free_rpc_bdev_cluster_unregister(struct rpc_bdev_rbd_unregister_cluster *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_rbd_unregister_cluster_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_unregister_cluster, name), spdk_json_decode_string},
};

static void
rpc_bdev_rbd_unregister_cluster(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_unregister_cluster req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_rbd_unregister_cluster_decoders,
				    SPDK_COUNTOF(rpc_bdev_rbd_unregister_cluster_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_unregister_cluster(req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_cluster_unregister(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_unregister_cluster", rpc_bdev_rbd_unregister_cluster, SPDK_RPC_RUNTIME)

struct rpc_bdev_rbd_get_cluster_info {
	char *name;
};

static void
free_rpc_bdev_rbd_get_cluster_info(struct rpc_bdev_rbd_get_cluster_info *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_rbd_get_cluster_info_decoders[] = {
	{"name", offsetof(struct rpc_bdev_rbd_get_cluster_info, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_rbd_get_clusters_info(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_bdev_rbd_get_cluster_info req = {NULL};
	int rc;

	if (params && spdk_json_decode_object(params, rpc_bdev_rbd_get_cluster_info_decoders,
					      SPDK_COUNTOF(rpc_bdev_rbd_get_cluster_info_decoders),
					      &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_rbd_get_clusters_info(request, req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

cleanup:
	free_rpc_bdev_rbd_get_cluster_info(&req);
}
SPDK_RPC_REGISTER("bdev_rbd_get_clusters_info", rpc_bdev_rbd_get_clusters_info, SPDK_RPC_RUNTIME)
