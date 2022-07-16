/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "bdev_ftl.h"

static int
rpc_bdev_ftl_decode_uuid(const struct spdk_json_val *val, void *out)
{
	char *uuid_str;
	int ret;

	uuid_str = spdk_json_strdup(val);
	if (!uuid_str) {
		return -ENOMEM;
	}

	ret = spdk_uuid_parse(out, uuid_str);

	free(uuid_str);
	return ret;
}

static const struct spdk_json_object_decoder rpc_bdev_ftl_create_decoders[] = {
	{"name", offsetof(struct spdk_ftl_conf, name), spdk_json_decode_string},
	{"base_bdev", offsetof(struct spdk_ftl_conf, base_bdev), spdk_json_decode_string},
	{"uuid", offsetof(struct spdk_ftl_conf, uuid), rpc_bdev_ftl_decode_uuid, true},
	{"cache", offsetof(struct spdk_ftl_conf, cache_bdev), spdk_json_decode_string},
	{
		"overprovisioning", offsetof(struct spdk_ftl_conf, overprovisioning),
		spdk_json_decode_uint64, true
	},
	{
		"core_mask", offsetof(struct spdk_ftl_conf, core_mask),
		spdk_json_decode_string, true
	},
};

static void
rpc_bdev_ftl_create_cb(const struct ftl_bdev_info *bdev_info, void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;
	char bdev_uuid[SPDK_UUID_STRING_LEN];
	struct spdk_json_write_ctx *w;

	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create FTL bdev: %s",
						     spdk_strerror(-status));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_uuid_fmt_lower(bdev_uuid, sizeof(bdev_uuid), &bdev_info->uuid);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", bdev_info->name);
	spdk_json_write_named_string(w, "uuid", bdev_uuid);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_ftl_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct spdk_ftl_conf conf = {};
	struct spdk_json_write_ctx *w;
	int rc;

	spdk_ftl_get_default_conf(&conf);

	if (spdk_json_decode_object(params, rpc_bdev_ftl_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_ftl_create_decoders),
				    &conf)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	if (spdk_mem_all_zero(&conf.uuid, sizeof(conf.uuid))) {
		conf.mode |= SPDK_FTL_MODE_CREATE;
	}

	rc = bdev_ftl_create_bdev(&conf, rpc_bdev_ftl_create_cb, request);
	if (rc == -ENODEV) {
		rc = bdev_ftl_defer_init(&conf);
		if (rc == 0) {
			w = spdk_jsonrpc_begin_result(request);
			spdk_json_write_string_fmt(w, "FTL bdev: %s creation deferred", conf.name);
			spdk_jsonrpc_end_result(request, w);
		}
	}

	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create FTL bdev: %s",
						     spdk_strerror(-rc));
	}
out:
	spdk_ftl_conf_deinit(&conf);
}
SPDK_RPC_REGISTER("bdev_ftl_create", rpc_bdev_ftl_create, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ftl_load(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	rpc_bdev_ftl_create(request, params);
}
SPDK_RPC_REGISTER("bdev_ftl_load", rpc_bdev_ftl_load, SPDK_RPC_RUNTIME)

struct rpc_delete_ftl {
	char *name;
};

static const struct spdk_json_object_decoder rpc_delete_ftl_decoders[] = {
	{"name", offsetof(struct rpc_delete_ftl, name), spdk_json_decode_string},
};

static void
rpc_bdev_ftl_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_ftl_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_delete_ftl attrs = {};

	if (spdk_json_decode_object(params, rpc_delete_ftl_decoders,
				    SPDK_COUNTOF(rpc_delete_ftl_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto invalid;
	}

	bdev_ftl_delete_bdev(attrs.name, rpc_bdev_ftl_delete_cb, request);
invalid:
	free(attrs.name);
}
SPDK_RPC_REGISTER("bdev_ftl_delete", rpc_bdev_ftl_delete, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ftl_unload(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	rpc_bdev_ftl_delete(request, params);
}
SPDK_RPC_REGISTER("bdev_ftl_unload", rpc_bdev_ftl_unload, SPDK_RPC_RUNTIME)
