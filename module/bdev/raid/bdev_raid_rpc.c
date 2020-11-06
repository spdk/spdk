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
#include "bdev_raid.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/env.h"

#define RPC_MAX_BASE_BDEVS 255

/*
 * Input structure for bdev_raid_get_bdevs RPC
 */
struct rpc_bdev_raid_get_bdevs {
	/* category - all or online or configuring or offline */
	char *category;
};

/*
 * brief:
 * free_rpc_bdev_raid_get_bdevs function frees RPC bdev_raid_get_bdevs related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_raid_get_bdevs(struct rpc_bdev_raid_get_bdevs *req)
{
	free(req->category);
}

/*
 * Decoder object for RPC get_raids
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_get_bdevs_decoders[] = {
	{"category", offsetof(struct rpc_bdev_raid_get_bdevs, category), spdk_json_decode_string},
};

/*
 * brief:
 * rpc_bdev_raid_get_bdevs function is the RPC for rpc_bdev_raid_get_bdevs. This is used to list
 * all the raid bdev names based on the input category requested. Category should be
 * one of "all", "online", "configuring" or "offline". "all" means all the raids
 * whether they are online or configuring or offline. "online" is the raid bdev which
 * is registered with bdev layer. "configuring" is the raid bdev which does not have
 * full configuration discovered yet. "offline" is the raid bdev which is not
 * registered with bdev as of now and it has encountered any error or user has
 * requested to offline the raid.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_get_bdevs(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_get_bdevs   req = {};
	struct spdk_json_write_ctx  *w;
	struct raid_bdev            *raid_bdev;

	if (spdk_json_decode_object(params, rpc_bdev_raid_get_bdevs_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_get_bdevs_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (!(strcmp(req.category, "all") == 0 ||
	      strcmp(req.category, "online") == 0 ||
	      strcmp(req.category, "configuring") == 0 ||
	      strcmp(req.category, "offline") == 0)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	/* Get raid bdev list based on the category requested */
	if (strcmp(req.category, "all") == 0) {
		TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
			spdk_json_write_string(w, raid_bdev->bdev.name);
		}
	} else if (strcmp(req.category, "online") == 0) {
		TAILQ_FOREACH(raid_bdev, &g_raid_bdev_configured_list, state_link) {
			spdk_json_write_string(w, raid_bdev->bdev.name);
		}
	} else if (strcmp(req.category, "configuring") == 0) {
		TAILQ_FOREACH(raid_bdev, &g_raid_bdev_configuring_list, state_link) {
			spdk_json_write_string(w, raid_bdev->bdev.name);
		}
	} else {
		TAILQ_FOREACH(raid_bdev, &g_raid_bdev_offline_list, state_link) {
			spdk_json_write_string(w, raid_bdev->bdev.name);
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_raid_get_bdevs(&req);
}
SPDK_RPC_REGISTER("bdev_raid_get_bdevs", rpc_bdev_raid_get_bdevs, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_raid_get_bdevs, get_raid_bdevs)

/*
 * Base bdevs in RPC bdev_raid_create
 */
struct rpc_bdev_raid_create_base_bdevs {
	/* Number of base bdevs */
	size_t           num_base_bdevs;

	/* List of base bdevs names */
	char             *base_bdevs[RPC_MAX_BASE_BDEVS];
};

/*
 * Input structure for RPC rpc_bdev_raid_create
 */
struct rpc_bdev_raid_create {
	/* Raid bdev name */
	char                                 *name;

	/* RAID strip size KB, 'strip_size' is deprecated. */
	uint32_t                             strip_size;
	uint32_t                             strip_size_kb;

	/* RAID raid level */
	enum raid_level                      level;

	/* Base bdevs information */
	struct rpc_bdev_raid_create_base_bdevs base_bdevs;
};

/*
 * brief:
 * free_rpc_bdev_raid_create function is to free RPC bdev_raid_create related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_raid_create(struct rpc_bdev_raid_create *req)
{
	size_t i;

	free(req->name);
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		free(req->base_bdevs.base_bdevs[i]);
	}
}

/*
 * Decoder function for RPC bdev_raid_create to decode raid level
 */
static int
decode_raid_level(const struct spdk_json_val *val, void *out)
{
	int ret;
	char *str = NULL;
	enum raid_level level;

	ret = spdk_json_decode_string(val, &str);
	if (ret == 0 && str != NULL) {
		level = raid_bdev_parse_raid_level(str);
		if (level == INVALID_RAID_LEVEL) {
			ret = -EINVAL;
		} else {
			*(enum raid_level *)out = level;
		}
	}

	free(str);
	return ret;
}

/*
 * Decoder function for RPC bdev_raid_create to decode base bdevs list
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_raid_create_base_bdevs *base_bdevs = out;
	return spdk_json_decode_array(val, spdk_json_decode_string, base_bdevs->base_bdevs,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(char *));
}

/*
 * Decoder object for RPC bdev_raid_create
 */
/* Note: strip_size is deprecated, one of the two options must be specified but not both. */
static const struct spdk_json_object_decoder rpc_bdev_raid_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_raid_create, name), spdk_json_decode_string},
	{"strip_size", offsetof(struct rpc_bdev_raid_create, strip_size), spdk_json_decode_uint32, true},
	{"strip_size_kb", offsetof(struct rpc_bdev_raid_create, strip_size_kb), spdk_json_decode_uint32, true},
	{"raid_level", offsetof(struct rpc_bdev_raid_create, level), decode_raid_level},
	{"base_bdevs", offsetof(struct rpc_bdev_raid_create, base_bdevs), decode_base_bdevs},
};

/*
 * brief:
 * rpc_bdev_raid_create function is the RPC for creating RAID bdevs. It takes
 * input as raid bdev name, raid level, strip size in KB and list of base bdev names.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_create	req = {};
	struct raid_bdev_config		*raid_cfg;
	int				rc;
	size_t				i;

	if (spdk_json_decode_object(params, rpc_bdev_raid_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	if (req.strip_size == 0 && req.strip_size_kb == 0) {
		spdk_jsonrpc_send_error_response(request, EINVAL, "strip size not specified");
		goto cleanup;
	} else if (req.strip_size > 0 && req.strip_size_kb > 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "please use only one strip size option");
		goto cleanup;
	} else if (req.strip_size > 0 && req.strip_size_kb == 0) {
		SPDK_ERRLOG("the rpc param strip_size is deprecated.\n");
		req.strip_size_kb = req.strip_size;
	}

	rc = raid_bdev_config_add(req.name, req.strip_size_kb, req.base_bdevs.num_base_bdevs,
				  req.level,
				  &raid_cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to add RAID bdev config %s: %s",
						     req.name, spdk_strerror(-rc));
		goto cleanup;
	}

	for (i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		rc = raid_bdev_config_add_base_bdev(raid_cfg, req.base_bdevs.base_bdevs[i], i);
		if (rc != 0) {
			raid_bdev_config_cleanup(raid_cfg);
			spdk_jsonrpc_send_error_response_fmt(request, rc,
							     "Failed to add base bdev %s to RAID bdev config %s: %s",
							     req.base_bdevs.base_bdevs[i], req.name,
							     spdk_strerror(-rc));
			goto cleanup;
		}
	}

	rc = raid_bdev_create(raid_cfg);
	if (rc != 0) {
		raid_bdev_config_cleanup(raid_cfg);
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create RAID bdev %s: %s",
						     req.name, spdk_strerror(-rc));
		goto cleanup;
	}

	rc = raid_bdev_add_base_devices(raid_cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to add any base bdev to RAID bdev %s: %s",
						     req.name, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_raid_create(&req);
}
SPDK_RPC_REGISTER("bdev_raid_create", rpc_bdev_raid_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_raid_create, construct_raid_bdev)

/*
 * Input structure for RPC deleting a raid bdev
 */
struct rpc_bdev_raid_delete {
	/* raid bdev name */
	char *name;
};

/*
 * brief:
 * free_rpc_bdev_raid_delete function is used to free RPC bdev_raid_delete related parameters
 * params:
 * req - pointer to RPC request
 * params:
 * none
 */
static void
free_rpc_bdev_raid_delete(struct rpc_bdev_raid_delete *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC raid_bdev_delete
 */
static const struct spdk_json_object_decoder rpc_bdev_raid_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_raid_delete, name), spdk_json_decode_string},
};

struct rpc_bdev_raid_delete_ctx {
	struct rpc_bdev_raid_delete req;
	struct raid_bdev_config *raid_cfg;
	struct spdk_jsonrpc_request *request;
};

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the deletion of the raid bdev.
 * returns:
 * none
 */
static void
bdev_raid_delete_done(void *cb_arg, int rc)
{
	struct rpc_bdev_raid_delete_ctx *ctx = cb_arg;
	struct raid_bdev_config *raid_cfg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to delete raid bdev %s (%d): %s\n",
			    ctx->req.name, rc, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	raid_cfg = ctx->raid_cfg;
	assert(raid_cfg->raid_bdev == NULL);

	raid_bdev_config_cleanup(raid_cfg);

	spdk_jsonrpc_send_bool_response(request, true);
exit:
	free_rpc_bdev_raid_delete(&ctx->req);
	free(ctx);
}

/*
 * brief:
 * rpc_bdev_raid_delete function is the RPC for deleting a raid bdev. It takes raid
 * name as input and delete that raid bdev including freeing the base bdev
 * resources.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
rpc_bdev_raid_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_raid_delete_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_raid_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_raid_delete_decoders),
				    &ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->raid_cfg = raid_bdev_config_find_by_name(ctx->req.name);
	if (ctx->raid_cfg == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, ENODEV,
						     "raid bdev %s is not found in config",
						     ctx->req.name);
		goto cleanup;
	}

	ctx->request = request;

	/* Remove all the base bdevs from this raid bdev before deleting the raid bdev */
	raid_bdev_remove_base_devices(ctx->raid_cfg, bdev_raid_delete_done, ctx);

	return;

cleanup:
	free_rpc_bdev_raid_delete(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_raid_delete", rpc_bdev_raid_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_raid_delete, destroy_raid_bdev)
