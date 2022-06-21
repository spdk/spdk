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
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "bdev_ftl.h"

struct rpc_bdev_ftl_create {
	char *name;
	char *base_bdev;
	char *uuid;
	char *cache_bdev;
	struct spdk_ftl_conf ftl_conf;
};

static void
free_rpc_bdev_ftl_create(struct rpc_bdev_ftl_create *req)
{
	free(req->name);
	free(req->base_bdev);
	free(req->uuid);
	free(req->cache_bdev);
	free((char *)req->ftl_conf.l2p_path);
	free((char *)req->ftl_conf.core_mask);
}

static const struct spdk_json_object_decoder rpc_bdev_ftl_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ftl_create, name), spdk_json_decode_string},
	{"base_bdev", offsetof(struct rpc_bdev_ftl_create, base_bdev), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_bdev_ftl_create, uuid), spdk_json_decode_string, true},
	{"cache", offsetof(struct rpc_bdev_ftl_create, cache_bdev), spdk_json_decode_string},
	{
		"overprovisioning", offsetof(struct rpc_bdev_ftl_create, ftl_conf) +
		offsetof(struct spdk_ftl_conf, lba_rsvd), spdk_json_decode_uint64, true
	},
	{
		"l2p_dram_limit", offsetof(struct rpc_bdev_ftl_create, ftl_conf) +
		offsetof(struct spdk_ftl_conf, l2p_dram_limit), spdk_json_decode_uint64, true
	},

	{
		"use_append", offsetof(struct rpc_bdev_ftl_create, ftl_conf) +
		offsetof(struct spdk_ftl_conf, use_append), spdk_json_decode_bool, true
	},
	{
		"l2p_path", offsetof(struct rpc_bdev_ftl_create, ftl_conf) +
		offsetof(struct spdk_ftl_conf, l2p_path),
		spdk_json_decode_string, true
	},
	{
		"core_mask", offsetof(struct rpc_bdev_ftl_create, ftl_conf) +
		offsetof(struct spdk_ftl_conf, core_mask),
		spdk_json_decode_string, true
	},
	{
		"fast_shutdown", offsetof(struct rpc_bdev_ftl_create, ftl_conf) +
		offsetof(struct spdk_ftl_conf, fast_shdn),
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
	struct rpc_bdev_ftl_create req = {};
	struct ftl_bdev_init_opts opts = {};
	struct spdk_json_write_ctx *w;
	int rc;

	spdk_ftl_conf_init_defaults(&req.ftl_conf);

	if (spdk_json_decode_object(params, rpc_bdev_ftl_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_ftl_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto invalid;
	}

	opts.name = req.name;
	opts.mode = SPDK_FTL_MODE_CREATE;
	opts.base_bdev = req.base_bdev;
	opts.cache_bdev = req.cache_bdev;
	opts.ftl_conf = req.ftl_conf;

	if (req.uuid) {
		if (spdk_uuid_parse(&opts.uuid, req.uuid) < 0) {
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Failed to parse uuid: %s",
							     req.uuid);
			goto invalid;
		}

		if (!spdk_mem_all_zero(&opts.uuid, sizeof(opts.uuid))) {
			opts.mode &= ~SPDK_FTL_MODE_CREATE;
		}
	}

	rc = bdev_ftl_create_bdev(&opts, rpc_bdev_ftl_create_cb, request);
	if (rc) {
		if (rc == -ENODEV) {
			w = spdk_jsonrpc_begin_result(request);
			spdk_json_write_string_fmt(w, "FTL bdev: %s creation deferred", req.name);
			spdk_jsonrpc_end_result(request, w);
		} else {
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Failed to create FTL bdev: %s",
							     spdk_strerror(-rc));
		}
		goto invalid;
	}

invalid:
	free_rpc_bdev_ftl_create(&req);
}

SPDK_RPC_REGISTER("bdev_ftl_create", rpc_bdev_ftl_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_ftl_create, construct_ftl_bdev)

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

	bdev_ftl_delete_bdev(attrs.name, attrs.fast_shutdown,
			     rpc_bdev_ftl_delete_cb, request);
invalid:
	free(attrs.name);
}

SPDK_RPC_REGISTER("bdev_ftl_delete", rpc_bdev_ftl_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_ftl_delete, delete_ftl_bdev)

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

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_ftl_unmap(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_ftl_unmap attrs = {};

	if (spdk_json_decode_object(params, rpc_ftl_unmap_decoders,
				    SPDK_COUNTOF(rpc_ftl_unmap_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto invalid;
	}

	bdev_ftl_unmap(attrs.name, attrs.lba, attrs.num_blocks,
		       rpc_bdev_ftl_unmap_cb, request);
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

static void _rpc_bdev_ftl_get_stats(void *cntx)
{
	struct rpc_ftl_stats_ctx *ftl_stats = cntx;
	struct spdk_jsonrpc_request *request = ftl_stats->request;
	struct spdk_ftl_dev *dev = ftl_stats->dev;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", dev->name);

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
		spdk_json_write_named_uint64(w, "ios", dev->stats.entries[i].read.ios);
		spdk_json_write_named_uint64(w, "blocks", dev->stats.entries[i].read.blocks);
		spdk_json_write_named_object_begin(w, "errors");
		spdk_json_write_named_uint64(w, "media", dev->stats.entries[i].read.errors.media);
		spdk_json_write_named_uint64(w, "crc", dev->stats.entries[i].read.errors.crc);
		spdk_json_write_named_uint64(w, "other", dev->stats.entries[i].read.errors.other);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);

		spdk_json_write_named_object_begin(w, "write");
		spdk_json_write_named_uint64(w, "ios", dev->stats.entries[i].write.ios);
		spdk_json_write_named_uint64(w, "blocks", dev->stats.entries[i].write.blocks);
		spdk_json_write_named_object_begin(w, "errors");
		spdk_json_write_named_uint64(w, "media", dev->stats.entries[i].write.errors.media);
		spdk_json_write_named_uint64(w, "other", dev->stats.entries[i].write.errors.other);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

	free(cntx);
}

static void
rpc_bdev_ftl_get_stats(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_ftl_stats attrs = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_ftl_stats_decoders,
					SPDK_COUNTOF(rpc_ftl_stats_decoders),
					&attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto invalid;
	}

	rc = bdev_ftl_get_stats(attrs.name, _rpc_bdev_ftl_get_stats, request);
	if (rc) {
		spdk_jsonrpc_send_bool_response(request, false);
		goto invalid;
	}

invalid:
	free(attrs.name);
}

SPDK_RPC_REGISTER("bdev_ftl_get_stats", rpc_bdev_ftl_get_stats, SPDK_RPC_RUNTIME)
