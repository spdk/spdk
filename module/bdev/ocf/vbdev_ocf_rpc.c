/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "vbdev_ocf.h"
#include "stats.h"
#include "utils.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

/* Common structure to hold the name parameter for RPC methods using bdev name only. */
struct rpc_bdev_ocf_name {
	char *name;             /* main vbdev name */
};

/* Common free function for RPC methods using bdev name only. */
static void
free_rpc_bdev_ocf_name(struct rpc_bdev_ocf_name *r)
{
	free(r->name);
}

/* Common function to decode the name input parameter for RPC methods using bdev name only. */
static const struct spdk_json_object_decoder rpc_bdev_ocf_name_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ocf_name, name), spdk_json_decode_string},
};


/* Structure to hold the parameters for this RPC method. */
struct rpc_bdev_ocf_create {
	char *name;			/* main vbdev */
	char *mode;			/* OCF mode (choose one) */
	uint64_t cache_line_size;	/* OCF cache line size */
	char *cache_bdev_name;		/* sub bdev */
	char *core_bdev_name;		/* sub bdev */
};

static void
free_rpc_bdev_ocf_create(struct rpc_bdev_ocf_create *r)
{
	free(r->name);
	free(r->core_bdev_name);
	free(r->cache_bdev_name);
	free(r->mode);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_bdev_ocf_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ocf_create, name), spdk_json_decode_string},
	{"mode", offsetof(struct rpc_bdev_ocf_create, mode), spdk_json_decode_string},
	{"cache_line_size", offsetof(struct rpc_bdev_ocf_create, cache_line_size), spdk_json_decode_uint64, true},
	{"cache_bdev_name", offsetof(struct rpc_bdev_ocf_create, cache_bdev_name), spdk_json_decode_string},
	{"core_bdev_name", offsetof(struct rpc_bdev_ocf_create, core_bdev_name), spdk_json_decode_string},
};

static void
construct_cb(int status, struct vbdev_ocf *vbdev, void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not create OCF vbdev: %d",
						     status);
	} else {
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_string(w, vbdev->name);
		spdk_jsonrpc_end_result(request, w);
	}
}

static void
rpc_bdev_ocf_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_create req = {NULL};
	int ret;

	ret = spdk_json_decode_object(params, rpc_bdev_ocf_create_decoders,
				      SPDK_COUNTOF(rpc_bdev_ocf_create_decoders),
				      &req);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_bdev_ocf_create(&req);
		return;
	}

	vbdev_ocf_construct(req.name, req.mode, req.cache_line_size, req.cache_bdev_name,
			    req.core_bdev_name, false, construct_cb, request);
	free_rpc_bdev_ocf_create(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_create", rpc_bdev_ocf_create, SPDK_RPC_RUNTIME)

static void
delete_cb(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not delete OCF vbdev: %d",
						     status);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}

static void
rpc_bdev_ocf_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_name req = {NULL};
	struct vbdev_ocf *vbdev;
	int status;

	status = spdk_json_decode_object(params, rpc_bdev_ocf_name_decoders,
					 SPDK_COUNTOF(rpc_bdev_ocf_name_decoders),
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

	status = vbdev_ocf_delete_clean(vbdev, delete_cb, request);
	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not delete OCF vbdev: %s",
						     spdk_strerror(-status));
		goto end;
	}

end:
	free_rpc_bdev_ocf_name(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_delete", rpc_bdev_ocf_delete, SPDK_RPC_RUNTIME)

struct get_ocf_stats_ctx {
	struct spdk_jsonrpc_request *request;
	char *core_name;
};

static void
rpc_bdev_ocf_get_stats_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct get_ocf_stats_ctx *ctx = (struct get_ocf_stats_ctx *) priv;
	struct spdk_json_write_ctx *w;
	struct vbdev_ocf_stats stats;

	if (error) {
		goto end;
	}

	error = vbdev_ocf_stats_get(cache, ctx->core_name, &stats);

	ocf_mngt_cache_read_unlock(cache);

	if (error) {
		goto end;
	}

	w = spdk_jsonrpc_begin_result(ctx->request);
	vbdev_ocf_stats_write_json(w, &stats);
	spdk_jsonrpc_end_result(ctx->request, w);

end:
	if (error) {
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not get stats: %s",
						     spdk_strerror(-error));
	}
	free(ctx);
}

static void
rpc_bdev_ocf_get_stats(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_name req = {NULL};
	struct vbdev_ocf *vbdev;
	struct get_ocf_stats_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Not enough memory to process request");
		goto end;
	}

	if (spdk_json_decode_object(params, rpc_bdev_ocf_name_decoders,
				    SPDK_COUNTOF(rpc_bdev_ocf_name_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free(ctx);
		goto end;
	}

	vbdev = vbdev_ocf_get_by_name(req.name);
	if (vbdev == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(ENODEV));
		free(ctx);
		goto end;
	}

	ctx->core_name = vbdev->core.name;
	ctx->request = request;
	ocf_mngt_cache_read_lock(vbdev->ocf_cache, rpc_bdev_ocf_get_stats_cmpl, ctx);

end:
	free_rpc_bdev_ocf_name(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_get_stats", rpc_bdev_ocf_get_stats, SPDK_RPC_RUNTIME)

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_bdev_ocf_get_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ocf_name, name), spdk_json_decode_string, true},
};

struct bdev_get_bdevs_ctx {
	char *name;
	struct spdk_json_write_ctx *w;
};

static void
bdev_get_bdevs_fn(struct vbdev_ocf *vbdev, void *ctx)
{
	struct bdev_get_bdevs_ctx *cctx = ctx;
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
rpc_bdev_ocf_get_bdevs(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct rpc_bdev_ocf_name req = {NULL};
	struct bdev_get_bdevs_ctx cctx;

	if (params && spdk_json_decode_object(params, rpc_bdev_ocf_get_bdevs_decoders,
					      SPDK_COUNTOF(rpc_bdev_ocf_get_bdevs_decoders),
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

	cctx.name    = req.name;
	cctx.w       = w;

	spdk_json_write_array_begin(w);
	vbdev_ocf_foreach(bdev_get_bdevs_fn, &cctx);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

end:
	free_rpc_bdev_ocf_name(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_get_bdevs", rpc_bdev_ocf_get_bdevs, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_bdev_ocf_set_cache_mode {
	char *name;			/* main vbdev name */
	char *mode;			/* OCF cache mode to switch to */
};

static void
free_rpc_bdev_ocf_set_cache_mode(struct rpc_bdev_ocf_set_cache_mode *r)
{
	free(r->name);
	free(r->mode);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_bdev_ocf_set_cache_mode_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ocf_set_cache_mode, name), spdk_json_decode_string},
	{"mode", offsetof(struct rpc_bdev_ocf_set_cache_mode, mode), spdk_json_decode_string},
};

static void
cache_mode_cb(int status, struct vbdev_ocf *vbdev, void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not change OCF vbdev cache mode: %d",
						     status);
	} else {
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_string(w, ocf_get_cache_modename(
					       ocf_cache_get_mode(vbdev->ocf_cache)));
		spdk_jsonrpc_end_result(request, w);
	}
}

static void
rpc_bdev_ocf_set_cache_mode(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_set_cache_mode req = {NULL};
	struct vbdev_ocf *vbdev;
	int status;

	status = spdk_json_decode_object(params, rpc_bdev_ocf_set_cache_mode_decoders,
					 SPDK_COUNTOF(rpc_bdev_ocf_set_cache_mode_decoders),
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

	vbdev_ocf_set_cache_mode(vbdev, req.mode, cache_mode_cb, request);

end:
	free_rpc_bdev_ocf_set_cache_mode(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_set_cache_mode", rpc_bdev_ocf_set_cache_mode, SPDK_RPC_RUNTIME)

static void
seqcutoff_cb(int status, void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (status) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "OCF could not set sequential cutoff parameters: %d", status);
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}

/* Structure to hold the parameters for this RPC method. */
struct rpc_bdev_ocf_set_seqcutoff {
	char *name;		/* main vbdev name */
	char *policy;
	uint32_t threshold;
	uint32_t promotion_count;
};

static void
free_rpc_bdev_ocf_set_seqcutoff(struct rpc_bdev_ocf_set_seqcutoff *r)
{
	free(r->name);
	free(r->policy);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_bdev_ocf_set_seqcutoff_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ocf_set_seqcutoff, name), spdk_json_decode_string},
	{"policy", offsetof(struct rpc_bdev_ocf_set_seqcutoff, policy), spdk_json_decode_string},
	{"threshold", offsetof(struct rpc_bdev_ocf_set_seqcutoff, threshold), spdk_json_decode_uint32, true},
	{"promotion_count", offsetof(struct rpc_bdev_ocf_set_seqcutoff, promotion_count), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_ocf_set_seqcutoff(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_set_seqcutoff req = {NULL};
	struct vbdev_ocf *vbdev;
	int ret;

	ret = spdk_json_decode_object(params, rpc_bdev_ocf_set_seqcutoff_decoders,
				      SPDK_COUNTOF(rpc_bdev_ocf_set_seqcutoff_decoders), &req);
	if (ret) {
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

	vbdev_ocf_set_seqcutoff(vbdev, req.policy, req.threshold, req.promotion_count, seqcutoff_cb,
				request);

end:
	free_rpc_bdev_ocf_set_seqcutoff(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_set_seqcutoff", rpc_bdev_ocf_set_seqcutoff, SPDK_RPC_RUNTIME)

struct get_ocf_flush_start_ctx {
	struct spdk_jsonrpc_request *request;
	struct vbdev_ocf *vbdev;
};

static void
rpc_bdev_ocf_flush_start_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct get_ocf_flush_start_ctx *ctx = priv;

	ctx->vbdev->flush.in_progress = false;
	ctx->vbdev->flush.status = error;

	ocf_mngt_cache_read_unlock(cache);

	free(ctx);
}

static void
rpc_bdev_ocf_flush_start_lock_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct get_ocf_flush_start_ctx *ctx = priv;

	if (error) {
		spdk_jsonrpc_send_error_response_fmt(ctx->request,
						     SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not lock cache: %d", error);
		free(ctx);
		return;
	}

	ctx->vbdev->flush.in_progress = true;
	ocf_mngt_cache_flush(cache, rpc_bdev_ocf_flush_start_cmpl, ctx);

	spdk_jsonrpc_send_bool_response(ctx->request, true);
}

static void
rpc_bdev_ocf_flush_start(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_name req = {NULL};
	struct get_ocf_flush_start_ctx *ctx;
	int status;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Not enough memory to process request");
		goto end;
	}

	status = spdk_json_decode_object(params, rpc_bdev_ocf_name_decoders,
					 SPDK_COUNTOF(rpc_bdev_ocf_name_decoders),
					 &req);
	if (status) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free(ctx);
		goto end;
	}

	ctx->vbdev = vbdev_ocf_get_by_name(req.name);
	if (ctx->vbdev == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(ENODEV));
		free(ctx);
		goto end;
	}

	if (!ctx->vbdev->ocf_cache) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Couldn't flush cache: device not attached");
		free(ctx);
		goto end;
	}

	ctx->request = request;
	ocf_mngt_cache_read_lock(ctx->vbdev->ocf_cache, rpc_bdev_ocf_flush_start_lock_cmpl, ctx);

end:
	free_rpc_bdev_ocf_name(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_flush_start", rpc_bdev_ocf_flush_start, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_flush_status(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_name req = {NULL};
	struct spdk_json_write_ctx *w;
	struct vbdev_ocf *vbdev;
	int status;

	status = spdk_json_decode_object(params, rpc_bdev_ocf_name_decoders,
					 SPDK_COUNTOF(rpc_bdev_ocf_name_decoders),
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

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_bool(w, "in_progress", vbdev->flush.in_progress);
	if (!vbdev->flush.in_progress) {
		spdk_json_write_named_int32(w, "status", vbdev->flush.status);
	}
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);

end:
	free_rpc_bdev_ocf_name(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_flush_status", rpc_bdev_ocf_flush_status, SPDK_RPC_RUNTIME)
