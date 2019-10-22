/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2019, Peng Yu <yupeng0921@gmail.com>.
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
#include "bdev_linear.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"
#include "spdk/env.h"

#define RPC_MAX_BASE_BDEVS 255

SPDK_LOG_REGISTER_COMPONENT("linearrpc", SPDK_LOG_LINEAR_RPC)

/*
 * Base bdevs in RPC bdev_linear_create
 */
struct rpc_bdev_linear_create_base_bdevs {
	/* Number of base bdevs */
	size_t num_base_bdevs;

	/* List of base bdevs names */
	char *base_bdevs[RPC_MAX_BASE_BDEVS];
};

/*
 * Input structure for RPC rpc_bdev_linear_create
 */
struct rpc_bdev_linear_create {
	/* Linear bdev name */
	char *name;

	/* Base bdevs information */
	struct rpc_bdev_linear_create_base_bdevs base_bdevs;
};

/*
 * brief:
 * free_rpc_bdev_linear_create function is to free RPC bdev_linear_create related parameters
 * params:
 * req - pointer to RPC request
 * returns:
 * none
 */
static void
free_rpc_bdev_linear_create(struct rpc_bdev_linear_create *req)
{
	free(req->name);
	for (size_t i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		free(req->base_bdevs.base_bdevs[i]);
	}
}

/*
 * Decoder function for RPC bdev_linear_create to decode base bdevs list
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_linear_create_base_bdevs *base_bdevs = out;
	return spdk_json_decode_array(val, spdk_json_decode_string, base_bdevs->base_bdevs,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(char *));
}

/*
 * Decoder object for RPC bdev_linear_create
 */
static const struct spdk_json_object_decoder rpc_bdev_linear_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_linear_create, name), spdk_json_decode_string},
	{"base_bdevs", offsetof(struct rpc_bdev_linear_create, base_bdevs), decode_base_bdevs},
};

/*
 * brief:
 * spdk_rpc_bdev_linear_create function is the RPC for creating LINEAR bdevs. It takes
 * input as linear bdev name and list of base bdev names.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_bdev_linear_create(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_linear_create req = {};
	struct spdk_json_write_ctx *w;
	struct linear_bdev_config *linear_cfg;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_linear_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_linear_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = linear_bdev_config_add(req.name, req.base_bdevs.num_base_bdevs,
				    &linear_cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to add LINEAR bdev config %s: %s",
						     req.name, spdk_strerror(-rc));
		goto cleanup;
	}

	for (size_t i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		rc = linear_bdev_config_add_base_bdev(linear_cfg, req.base_bdevs.base_bdevs[i], i);
		if (rc != 0) {
			linear_bdev_config_cleanup(linear_cfg);
			spdk_jsonrpc_send_error_response_fmt(request, rc,
							     "Failed to add bse bdev %s to LINEAR bdev config %s: %s",
							     req.base_bdevs.base_bdevs[i], req.name,
							     spdk_strerror(-rc));
			goto cleanup;
		}
	}

	rc = linear_bdev_create(linear_cfg);
	if (rc != 0) {
		linear_bdev_config_cleanup(linear_cfg);
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to create LINEAR bdev %s: %s",
						     req.name, spdk_strerror(-rc));
		goto cleanup;
	}

	rc = linear_bdev_add_base_devices(linear_cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "Failed to add any bse bdev to LINEAR bdev %s: %s",
						     req.name, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_linear_create(&req);
}
SPDK_RPC_REGISTER("bdev_linear_create", spdk_rpc_bdev_linear_create, SPDK_RPC_RUNTIME)

/*
 * Input structure for RPC deleting a linear bdev
 */
struct rpc_bdev_linear_delete {
	/* linear bdev name */
	char *name;
};

/*
 * brief:
 * free_rpc_bdev_linear_delete function is used to free RPC bdev_linear_delete related parameters
 * params:
 * req - pointer to RPC request
 * returns
 * none
 */
static void
free_rpc_bdev_linear_delete(struct rpc_bdev_linear_delete *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC linear_bdev_delete
 */
static const struct spdk_json_object_decoder rpc_bdev_linear_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_linear_delete, name), spdk_json_decode_string},
};

struct rpc_bdev_linear_delete_ctx {
	struct rpc_bdev_linear_delete req;
	struct linear_bdev_config *linear_cfg;
	struct spdk_jsonrpc_request *request;
};

/*
 * brief:
 * params:
 * cb_arg - pointer to the callback context.
 * rc - return code of the deletion of the linear bdev.
 * returns:
 * none
 */
static void
bdev_linear_delete_done(void *cb_arg, int rc)
{
	struct rpc_bdev_linear_delete_ctx *ctx = cb_arg;
	struct linear_bdev_config *linear_cfg;
	struct spdk_jsonrpc_request *request = ctx->request;
	struct spdk_json_write_ctx *w;

	if (rc != 0) {
		SPDK_ERRLOG("Failed to delete linear bdev %s (%d): %s\n",
			    ctx->req.name, rc, spdk_strerror(-rc));
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-rc));
		goto exit;
	}

	linear_cfg = ctx->linear_cfg;
	assert(linear_cfg->linear_bdev == NULL);

	linear_bdev_config_cleanup(linear_cfg);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
exit:
	free_rpc_bdev_linear_delete(&ctx->req);
	free(ctx);
}

/*
 * brief:
 * spdk_rpc_bdev_linear_delete function is the RPC for deleting a linear bdev. It takes linear
 * name as input and delete that linear bdev including freeing the base bdev
 * resources.
 * params:
 * request - pointer to json rpc request
 * params - pointer to request parameters
 * returns:
 * none
 */
static void
spdk_rpc_bdev_linear_delete(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_linear_delete_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		return;
	}

	if (spdk_json_decode_object(params, rpc_bdev_linear_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_linear_delete_decoders),
				    &ctx->req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	ctx->linear_cfg = linear_bdev_config_find_by_name(ctx->req.name);
	if (ctx->linear_cfg == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, ENODEV,
						     "linear bdev %s is not found in config",
						     ctx->req.name);
		goto cleanup;
	}

	ctx->request = request;

	/* Remove all the base bdevs from thsi linear bdev before deleting the linear bdev */
	linear_bdev_remove_base_devices(ctx->linear_cfg, bdev_linear_delete_done, ctx);

	return;

cleanup:
	free_rpc_bdev_linear_delete(&ctx->req);
	free(ctx);
}
SPDK_RPC_REGISTER("bdev_linear_delete", spdk_rpc_bdev_linear_delete, SPDK_RPC_RUNTIME)
