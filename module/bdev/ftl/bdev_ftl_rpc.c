/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

	spdk_ftl_get_default_conf(&conf, sizeof(conf));

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
rpc_bdev_ftl_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
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

	bdev_ftl_delete_bdev(attrs.name, attrs.fast_shutdown, rpc_bdev_ftl_delete_cb, request);
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
rpc_bdev_ftl_unmap_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_ftl_unmap(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_ftl_unmap attrs = {};

	if (spdk_json_decode_object(params, rpc_ftl_unmap_decoders, SPDK_COUNTOF(rpc_ftl_unmap_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto invalid;
	}

	bdev_ftl_unmap(attrs.name, attrs.lba, attrs.num_blocks, rpc_bdev_ftl_unmap_cb, request);
invalid:
	free(attrs.name);
}

SPDK_RPC_REGISTER("bdev_ftl_unmap", rpc_bdev_ftl_unmap, SPDK_RPC_RUNTIME)

struct rpc_ftl_stats {
	char *name;
};

static const struct spdk_json_object_decoder rpc_ftl_stats_decoders[] = {
	{"name", offsetof(struct rpc_ftl_stats, name), spdk_json_decode_string},
};

static void
_rpc_bdev_ftl_get_stats(void *cntx)
{
	struct rpc_ftl_stats_ctx *ftl_stats = cntx;
	struct spdk_jsonrpc_request *request = ftl_stats->request;
	struct ftl_stats *stats = ftl_stats->ftl_stats;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_desc_get_bdev(ftl_stats->ftl_bdev_desc)->name);

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

	free(stats);
}

static void
rpc_bdev_ftl_get_stats(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct ftl_stats *stats;
	struct rpc_ftl_stats attrs = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_ftl_stats_decoders, SPDK_COUNTOF(rpc_ftl_stats_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto invalid;
	}

	stats = calloc(1, sizeof(struct ftl_stats));
	if (!stats) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto invalid;
	}

	rc = bdev_ftl_get_stats(attrs.name, _rpc_bdev_ftl_get_stats, request, stats);
	if (rc) {
		free(stats);
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto invalid;
	}

invalid:
	free(attrs.name);
}

SPDK_RPC_REGISTER("bdev_ftl_get_stats", rpc_bdev_ftl_get_stats, SPDK_RPC_RUNTIME)
