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

#include "vbdev_ocf.h"
#include "stats.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

/* Structure to hold the parameters for this RPC method. */
struct rpc_construct_ocf_bdev {
	char *name;             /* master vbdev */
	char *mode;             /* OCF mode (choose one) */
	char *cache_bdev_name;  /* sub bdev */
	char *core_bdev_name;   /* sub bdev */
};

static void
free_rpc_construct_ocf_bdev(struct rpc_construct_ocf_bdev *r)
{
	free(r->name);
	free(r->core_bdev_name);
	free(r->cache_bdev_name);
	free(r->mode);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_construct_ocf_bdev_decoders[] = {
	{"name", offsetof(struct rpc_construct_ocf_bdev, name), spdk_json_decode_string},
	{"mode", offsetof(struct rpc_construct_ocf_bdev, mode), spdk_json_decode_string},
	{"cache_bdev_name", offsetof(struct rpc_construct_ocf_bdev, cache_bdev_name), spdk_json_decode_string},
	{"core_bdev_name", offsetof(struct rpc_construct_ocf_bdev, core_bdev_name), spdk_json_decode_string},
};

static void
spdk_rpc_construct_ocf_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	int ret = 0;
	struct rpc_construct_ocf_bdev req = {NULL};
	struct spdk_json_write_ctx *w;

	ret = spdk_json_decode_object(params, rpc_construct_ocf_bdev_decoders,
				      SPDK_COUNTOF(rpc_construct_ocf_bdev_decoders),
				      &req);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	ret = vbdev_ocf_construct(req.name, req.mode, req.cache_bdev_name, req.core_bdev_name);
	if (ret) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not create OCF vbdev: %s",
						     spdk_strerror(-ret));
		goto end;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w) {
		spdk_json_write_string(w, req.name);
		spdk_jsonrpc_end_result(request, w);
	}

end:
	free_rpc_construct_ocf_bdev(&req);
}
SPDK_RPC_REGISTER("construct_ocf_bdev", spdk_rpc_construct_ocf_bdev, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_delete_ocf_bdev {
	char *name;             /* master vbdev name */
};

static void
free_rpc_delete_ocf_bdev(struct rpc_delete_ocf_bdev *r)
{
	free(r->name);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_delete_ocf_bdev_decoders[] = {
	{"name", offsetof(struct rpc_delete_ocf_bdev, name), spdk_json_decode_string},
};

static void
delete_cb(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not delete OCF vbdev: %d",
						     status);
	} else {
		w = spdk_jsonrpc_begin_result(request);
		if (w) {
			spdk_json_write_bool(w, true);
			spdk_jsonrpc_end_result(request, w);
		}
	}
}

static void
spdk_rpc_delete_ocf_bdev(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_delete_ocf_bdev req = {NULL};
	struct vbdev_ocf *vbdev;
	int status;

	status = spdk_json_decode_object(params, rpc_delete_ocf_bdev_decoders,
					 SPDK_COUNTOF(rpc_delete_ocf_bdev_decoders),
					 &req);
	if (status) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	vbdev = vbdev_ocf_get_by_name(req.name);
	if (vbdev == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(ENODEV));
		goto end;
	}

	status = vbdev_ocf_delete(vbdev, delete_cb, request);
	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not delete OCF vbdev: %s",
						     spdk_strerror(-status));
		goto end;
	}

end:
	free_rpc_delete_ocf_bdev(&req);
}
SPDK_RPC_REGISTER("delete_ocf_bdev", spdk_rpc_delete_ocf_bdev, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_get_ocf_stats {
	char *name;             /* master vbdev name */
};

static void
free_rpc_get_ocf_stats(struct rpc_get_ocf_stats *r)
{
	free(r->name);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_get_ocf_stats_decoders[] = {
	{"name", offsetof(struct rpc_get_ocf_stats, name), spdk_json_decode_string},
};

static void
spdk_rpc_get_ocf_stats(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_get_ocf_stats req = {NULL};
	struct spdk_json_write_ctx *w;
	struct vbdev_ocf *vbdev;
	struct vbdev_ocf_stats stats;
	int status;

	if (spdk_json_decode_object(params, rpc_get_ocf_stats_decoders,
				    SPDK_COUNTOF(rpc_get_ocf_stats_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	vbdev = vbdev_ocf_get_by_name(req.name);
	if (vbdev == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(ENODEV));
		goto end;
	}

	status = vbdev_ocf_stats_get(vbdev->ocf_cache, vbdev->core.id, &stats);
	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not get stats: %s",
						     spdk_strerror(-status));
		goto end;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w) {
		vbdev_ocf_stats_write_json(w, &stats);
		spdk_jsonrpc_end_result(request, w);
	}

end:
	free_rpc_get_ocf_stats(&req);
}
SPDK_RPC_REGISTER("get_ocf_stats", spdk_rpc_get_ocf_stats, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_get_ocf_bdevs {
	char *name;
};

static void
free_rpc_get_ocf_bdevs(struct rpc_get_ocf_bdevs *r)
{
	free(r->name);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_get_ocf_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_get_ocf_bdevs, name), spdk_json_decode_string, true},
};

struct get_bdevs_ctx {
	char *name;
	struct spdk_json_write_ctx *w;
};

static void
get_bdevs_fn(struct vbdev_ocf *vbdev, void *ctx)
{
	struct get_bdevs_ctx *cctx = ctx;
	struct spdk_json_write_ctx *w = cctx->w;

	if (cctx->name != NULL &&
	    strcmp(vbdev->name, cctx->name) &&
	    strcmp(vbdev->cache.name, cctx->name) &&
	    strcmp(vbdev->core.name, cctx->name)) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", vbdev->name);
	spdk_json_write_named_bool(w, "started", vbdev->state.started);

	spdk_json_write_named_object_begin(w, "cache");
	spdk_json_write_named_string(w, "name", vbdev->cache.name);
	spdk_json_write_named_bool(w, "attached", vbdev->cache.attached);
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "core");
	spdk_json_write_named_string(w, "name", vbdev->core.name);
	spdk_json_write_named_bool(w, "attached", vbdev->core.attached);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_ocf_bdevs(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct rpc_get_ocf_bdevs req = {NULL};
	struct get_bdevs_ctx cctx;

	if (params && spdk_json_decode_object(params, rpc_get_ocf_bdevs_decoders,
					      SPDK_COUNTOF(rpc_get_ocf_bdevs_decoders),
					      &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	if (req.name) {
		if (!(vbdev_ocf_get_by_name(req.name) || vbdev_ocf_get_base_by_name(req.name))) {
			spdk_jsonrpc_send_error_response(request,
							 SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 spdk_strerror(ENODEV));
			goto end;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	cctx.name    = req.name;
	cctx.w       = w;

	spdk_json_write_array_begin(w);
	vbdev_ocf_foreach(get_bdevs_fn, &cctx);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

end:
	free_rpc_get_ocf_bdevs(&req);
}
SPDK_RPC_REGISTER("get_ocf_bdevs", spdk_rpc_get_ocf_bdevs, SPDK_RPC_RUNTIME)
