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

#include "spdk/stdinc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "bdev_ocssd.h"

struct rpc_ocssd_attach_controller {
	char *name;
	char *trtype;
	char *traddr;
};

static const struct spdk_json_object_decoder rpc_ocssd_attach_controller_decoders[] = {
	{"name", offsetof(struct rpc_ocssd_attach_controller, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_ocssd_attach_controller, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_ocssd_attach_controller, traddr), spdk_json_decode_string},
};

static void
free_rpc_ocssd_attach_controller(struct rpc_ocssd_attach_controller *rpc)
{
	assert(rpc != NULL);

	free(rpc->name);
	free(rpc->trtype);
	free(rpc->traddr);
}

struct rpc_bdev_ocssd_attach_ctx {
	struct spdk_jsonrpc_request		*request;
#define OCSSD_BDEV_MAX_BDEV_NAMES 128
	const char				*bdev_names[OCSSD_BDEV_MAX_BDEV_NAMES];
	size_t					num_bdevs;
	struct rpc_ocssd_attach_controller	req;
};

static void
rpc_bdev_ocssd_attach_controller_done(void *_ctx)
{
	struct rpc_bdev_ocssd_attach_ctx *ctx = _ctx;
	struct spdk_json_write_ctx *w;
	size_t i;

	w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_array_begin(w);
	for (i = 0; i < spdk_min(ctx->num_bdevs, OCSSD_BDEV_MAX_BDEV_NAMES); i++) {
		spdk_json_write_string(w, ctx->bdev_names[i]);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(ctx->request, w);

	free_rpc_ocssd_attach_controller(&ctx->req);
	free(ctx);
}

static void
rpc_bdev_ocssd_attach_controller(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_bdev_ocssd_attach_ctx *ctx;
	struct spdk_nvme_transport_id trid = {};
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(ENOMEM));
		goto cleanup;
	}

	if (spdk_json_decode_object(params, rpc_ocssd_attach_controller_decoders,
				    SPDK_COUNTOF(rpc_ocssd_attach_controller_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed to parse the request");
		goto cleanup;
	}

	rc = spdk_nvme_transport_id_parse_trtype(&trid.trtype, ctx->req.trtype);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", ctx->req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse trtype: %s",
						     ctx->req.trtype);
		goto cleanup;
	}

	rc = snprintf(trid.traddr, sizeof(trid.traddr), "%s", ctx->req.traddr);
	if (rc < 0 || rc >= (int)sizeof(trid.traddr)) {
		SPDK_ERRLOG("Failed to parse traddr: %s\n", ctx->req.trtype);
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL, "Failed to parse traddr: %s",
						     ctx->req.traddr);
		goto cleanup;
	}

	ctx->num_bdevs = OCSSD_BDEV_MAX_BDEV_NAMES;
	ctx->request = request;

	rc = spdk_bdev_ocssd_attach_controller(&trid, ctx->req.name, ctx->bdev_names,
					       &ctx->num_bdevs,
					       rpc_bdev_ocssd_attach_controller_done,
					       ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	return;
cleanup:
	free_rpc_ocssd_attach_controller(&ctx->req);
	free(ctx);
}

SPDK_RPC_REGISTER("bdev_ocssd_attach_controller", rpc_bdev_ocssd_attach_controller,
		  SPDK_RPC_RUNTIME)


struct rpc_ocssd_detach_controller {
	char *name;
};

static const struct spdk_json_object_decoder rpc_ocssd_detach_controller_decoders[] = {
	{"name", offsetof(struct rpc_ocssd_detach_controller, name), spdk_json_decode_string},
};

static void
free_rpc_ocssd_detach_controller(struct rpc_ocssd_detach_controller *rpc)
{
	free(rpc->name);
}

static void
rpc_bdev_ocssd_detach_controller(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_ocssd_detach_controller rpc = {};
	struct spdk_json_write_ctx *w;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_ocssd_detach_controller_decoders,
				    SPDK_COUNTOF(rpc_ocssd_detach_controller_decoders),
				    &rpc)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = spdk_bdev_ocssd_detach_controller(rpc.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_ocssd_detach_controller(&rpc);
}

SPDK_RPC_REGISTER("bdev_ocssd_detach_controller", rpc_bdev_ocssd_detach_controller,
		  SPDK_RPC_RUNTIME)
