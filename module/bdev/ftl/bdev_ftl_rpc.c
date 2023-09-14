/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "bdev_ftl.h"

static void
rpc_bdev_ftl_basic_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

struct rpc_ftl_basic_param {
	char *name;
};

static const struct spdk_json_object_decoder rpc_ftl_basic_decoders[] = {
	{"name", offsetof(struct rpc_ftl_basic_param, name), spdk_json_decode_string},
};

static const struct spdk_json_object_decoder rpc_bdev_ftl_create_decoders[] = {
	{"name", offsetof(struct spdk_ftl_conf, name), spdk_json_decode_string},
	{"base_bdev", offsetof(struct spdk_ftl_conf, base_bdev), spdk_json_decode_string},
	{"uuid", offsetof(struct spdk_ftl_conf, uuid), spdk_json_decode_uuid, true},
	{"cache", offsetof(struct spdk_ftl_conf, cache_bdev), spdk_json_decode_string},
	{
		"overprovisioning", offsetof(struct spdk_ftl_conf, overprovisioning),
		spdk_json_decode_uint64, true
	},
	{
		"l2p_dram_limit", offsetof(struct spdk_ftl_conf, l2p_dram_limit),
		spdk_json_decode_uint64, true
	},
	{
		"core_mask", offsetof(struct spdk_ftl_conf, core_mask),
		spdk_json_decode_string, true
	},
	{
		"fast_shutdown", offsetof(struct spdk_ftl_conf, fast_shutdown),
		spdk_json_decode_bool, true
	},
};

static void
rpc_bdev_ftl_create_cb(const struct ftl_bdev_info *bdev_info, void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;
	struct spdk_json_write_ctx *w;

	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create FTL bdev: %s",
						     spdk_strerror(-status));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", bdev_info->name);
	spdk_json_write_named_uuid(w, "uuid", &bdev_info->uuid);
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

	spdk_ftl_get_default_conf(&conf, sizeof(conf));

	if (spdk_json_decode_object(params, rpc_bdev_ftl_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_ftl_create_decoders),
				    &conf)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	if (spdk_uuid_is_null(&conf.uuid)) {
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
	bool fast_shutdown;
};

static const struct spdk_json_object_decoder rpc_delete_ftl_decoders[] = {
	{"name", offsetof(struct rpc_delete_ftl, name), spdk_json_decode_string},
	{
		"fast_shutdown", offsetof(struct rpc_delete_ftl, fast_shutdown),
		spdk_json_decode_bool, true
	},
};

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

	bdev_ftl_delete_bdev(attrs.name, attrs.fast_shutdown, rpc_bdev_ftl_basic_cb, request);
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

struct rpc_ftl_unmap {
	char *name;
	uint64_t lba;
	uint64_t num_blocks;
};

static const struct spdk_json_object_decoder rpc_ftl_unmap_decoders[] = {
	{"name", offsetof(struct rpc_delete_ftl, name), spdk_json_decode_string},
	{"lba", offsetof(struct rpc_ftl_unmap, lba), spdk_json_decode_uint64, true},
	{"num_blocks", offsetof(struct rpc_ftl_unmap, num_blocks), spdk_json_decode_uint64, true},
};


static void
rpc_bdev_ftl_unmap(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_ftl_unmap attrs = {};

	if (spdk_json_decode_object(params, rpc_ftl_unmap_decoders, SPDK_COUNTOF(rpc_ftl_unmap_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	} else {
		bdev_ftl_unmap(attrs.name, attrs.lba, attrs.num_blocks, rpc_bdev_ftl_basic_cb, request);
	}
	free(attrs.name);
}

SPDK_RPC_REGISTER("bdev_ftl_unmap", rpc_bdev_ftl_unmap, SPDK_RPC_RUNTIME)

static void
_rpc_bdev_ftl_get_stats(void *ctx, int rc)
{
	struct rpc_ftl_stats_ctx *ftl_stats_ctx = ctx;
	struct spdk_jsonrpc_request *request = ftl_stats_ctx->request;
	struct ftl_stats *stats = &ftl_stats_ctx->ftl_stats;
	struct spdk_json_write_ctx *w;

	if (rc) {
		free(ftl_stats_ctx);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name",
				     spdk_bdev_desc_get_bdev(ftl_stats_ctx->ftl_bdev_desc)->name);

	/* TODO: Instead of named objects, store them in an array with the name being an attribute */
	for (uint64_t i = 0; i < FTL_STATS_TYPE_MAX; i++) {
		switch (i) {
		case FTL_STATS_TYPE_USER:
			spdk_json_write_named_object_begin(w, "user");
			break;
		case FTL_STATS_TYPE_CMP:
			spdk_json_write_named_object_begin(w, "cmp");
			break;
		case FTL_STATS_TYPE_GC:
			spdk_json_write_named_object_begin(w, "gc");
			break;
		case FTL_STATS_TYPE_MD_BASE:
			spdk_json_write_named_object_begin(w, "md_base");
			break;
		case FTL_STATS_TYPE_MD_NV_CACHE:
			spdk_json_write_named_object_begin(w, "md_nv_cache");
			break;
		case FTL_STATS_TYPE_L2P:
			spdk_json_write_named_object_begin(w, "l2p");
			break;
		default:
			assert(false);
			continue;
		}

		spdk_json_write_named_object_begin(w, "read");
		spdk_json_write_named_uint64(w, "ios", stats->entries[i].read.ios);
		spdk_json_write_named_uint64(w, "blocks", stats->entries[i].read.blocks);
		spdk_json_write_named_object_begin(w, "errors");
		spdk_json_write_named_uint64(w, "media", stats->entries[i].read.errors.media);
		spdk_json_write_named_uint64(w, "crc", stats->entries[i].read.errors.crc);
		spdk_json_write_named_uint64(w, "other", stats->entries[i].read.errors.other);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);

		spdk_json_write_named_object_begin(w, "write");
		spdk_json_write_named_uint64(w, "ios", stats->entries[i].write.ios);
		spdk_json_write_named_uint64(w, "blocks", stats->entries[i].write.blocks);
		spdk_json_write_named_object_begin(w, "errors");
		spdk_json_write_named_uint64(w, "media", stats->entries[i].write.errors.media);
		spdk_json_write_named_uint64(w, "other", stats->entries[i].write.errors.other);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(ftl_stats_ctx);
}

static void
rpc_bdev_ftl_get_stats(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_ftl_basic_param attrs = {};
	struct rpc_ftl_stats_ctx *ctx = calloc(1, sizeof(*ctx));

	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(-ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_ftl_basic_decoders, SPDK_COUNTOF(rpc_ftl_basic_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free(ctx);
		free(attrs.name);
		return;
	}

	ctx->request = request;
	bdev_ftl_get_stats(attrs.name, _rpc_bdev_ftl_get_stats, ctx);
	free(attrs.name);
}

SPDK_RPC_REGISTER("bdev_ftl_get_stats", rpc_bdev_ftl_get_stats, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ftl_get_properties_cb(void *ctx, int rc)
{
	struct spdk_jsonrpc_request *request = ctx;

	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}

static void
rpc_bdev_ftl_get_properties(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_ftl_basic_param attrs = {};

	if (spdk_json_decode_object(params, rpc_ftl_basic_decoders, SPDK_COUNTOF(rpc_ftl_basic_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free(attrs.name);
		return;
	}

	bdev_ftl_get_properties(attrs.name, rpc_bdev_ftl_get_properties_cb, request);
	free(attrs.name);
}

SPDK_RPC_REGISTER("bdev_ftl_get_properties", rpc_bdev_ftl_get_properties, SPDK_RPC_RUNTIME)

struct rpc_ftl_set_property_param {
	char *name;
	char *ftl_property;
	char *value;
};

static const struct spdk_json_object_decoder rpc_ftl_set_property_decoders[] = {
	{"name", offsetof(struct rpc_ftl_set_property_param, name), spdk_json_decode_string},
	{"ftl_property", offsetof(struct rpc_ftl_set_property_param, ftl_property), spdk_json_decode_string},
	{"value", offsetof(struct rpc_ftl_set_property_param, value), spdk_json_decode_string},
};

static void
rpc_bdev_ftl_set_property(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_ftl_set_property_param attrs = {};

	if (spdk_json_decode_object(params, rpc_ftl_set_property_decoders,
				    SPDK_COUNTOF(rpc_ftl_set_property_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free(attrs.name);
		free(attrs.ftl_property);
		free(attrs.value);
		return;
	}

	bdev_ftl_set_property(attrs.name, attrs.ftl_property, attrs.value,
			      rpc_bdev_ftl_basic_cb, request);
	free(attrs.name);
	free(attrs.ftl_property);
	free(attrs.value);
}

SPDK_RPC_REGISTER("bdev_ftl_set_property", rpc_bdev_ftl_set_property, SPDK_RPC_RUNTIME)
