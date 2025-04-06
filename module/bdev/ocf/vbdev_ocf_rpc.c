/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#include "vbdev_ocf.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

struct rpc_bdev_ocf_start_cache {
	char *cache_name;
	char *bdev_name;
	char *cache_mode;
	uint8_t cache_line_size;
};

static void
free_rpc_bdev_ocf_start_cache(struct rpc_bdev_ocf_start_cache *r) {
	free(r->cache_name);
	free(r->bdev_name);
	free(r->cache_mode);
}

static const struct spdk_json_object_decoder rpc_bdev_ocf_start_cache_decoders[] = {
	{"cache_name", offsetof(struct rpc_bdev_ocf_start_cache, cache_name), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_bdev_ocf_start_cache, bdev_name), spdk_json_decode_string},
	{"cache_mode", offsetof(struct rpc_bdev_ocf_start_cache, cache_mode), spdk_json_decode_string, true},
	{"cache_line_size", offsetof(struct rpc_bdev_ocf_start_cache, cache_line_size), spdk_json_decode_uint8, true},
};

static void
rpc_bdev_ocf_start_cache_cb(struct vbdev_ocf_cache *vbdev_cache, void *cb_arg, int error)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (error && error != -ENODEV) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not start OCF cache: %s",
						     spdk_strerror(-error));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, vbdev_cache->name);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_ocf_start_cache(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_start_cache req = {};

	if (spdk_json_decode_object(params, rpc_bdev_ocf_start_cache_decoders,
				    SPDK_COUNTOF(rpc_bdev_ocf_start_cache_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_ocf_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parametes");
		goto cleanup;
	}

	vbdev_ocf_cache_start(req.cache_name, req.bdev_name, req.cache_mode, req.cache_line_size,
			      rpc_bdev_ocf_start_cache_cb, request);

cleanup:
	free_rpc_bdev_ocf_start_cache(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_start_cache", rpc_bdev_ocf_start_cache, SPDK_RPC_RUNTIME)

struct rpc_bdev_ocf_stop_cache {
	char *cache_name;
};

static void
free_rpc_bdev_ocf_stop_cache(struct rpc_bdev_ocf_stop_cache *r) {
	free(r->cache_name);
}

static const struct spdk_json_object_decoder rpc_bdev_ocf_stop_cache_decoders[] = {
	{"cache_name", offsetof(struct rpc_bdev_ocf_stop_cache, cache_name), spdk_json_decode_string},
};

static void
rpc_bdev_ocf_stop_cache_cb(void *cb_arg, int error)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (error) {
		spdk_jsonrpc_send_error_response(request, error, spdk_strerror(-error));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_ocf_stop_cache(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_stop_cache req = {};

	if (spdk_json_decode_object(params, rpc_bdev_ocf_stop_cache_decoders,
				    SPDK_COUNTOF(rpc_bdev_ocf_stop_cache_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_ocf_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parametes");
		goto cleanup;
	}

	vbdev_ocf_cache_stop(req.cache_name, rpc_bdev_ocf_stop_cache_cb, request);

cleanup:
	free_rpc_bdev_ocf_stop_cache(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_stop_cache", rpc_bdev_ocf_stop_cache, SPDK_RPC_RUNTIME)

struct rpc_bdev_ocf_add_core {
	char *core_name;
	char *bdev_name;
	char *cache_name;
};

static void
free_rpc_bdev_ocf_add_core(struct rpc_bdev_ocf_add_core *r) {
	free(r->core_name);
	free(r->bdev_name);
	free(r->cache_name);
}

static const struct spdk_json_object_decoder rpc_bdev_ocf_add_core_decoders[] = {
	{"core_name", offsetof(struct rpc_bdev_ocf_add_core, core_name), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_bdev_ocf_add_core, bdev_name), spdk_json_decode_string},
	{"cache_name", offsetof(struct rpc_bdev_ocf_add_core, cache_name), spdk_json_decode_string},
};

static void
rpc_bdev_ocf_add_core_cb(struct vbdev_ocf_core *vbdev_core, void *cb_arg, int error)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (error && error != -ENODEV) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Could not add core to OCF cache: %s",
						     spdk_strerror(-error));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, vbdev_core->name);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_ocf_add_core(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_add_core req = {};

	if (spdk_json_decode_object(params, rpc_bdev_ocf_add_core_decoders,
				    SPDK_COUNTOF(rpc_bdev_ocf_add_core_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_ocf_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parametes");
		goto cleanup;
	}

	vbdev_ocf_core_add(req.core_name, req.bdev_name, req.cache_name, rpc_bdev_ocf_add_core_cb, request);

cleanup:
	free_rpc_bdev_ocf_add_core(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_add_core", rpc_bdev_ocf_add_core, SPDK_RPC_RUNTIME)

struct rpc_bdev_ocf_remove_core {
	char *core_name;
};

static void
free_rpc_bdev_ocf_remove_core(struct rpc_bdev_ocf_remove_core *r) {
	free(r->core_name);
}

static const struct spdk_json_object_decoder rpc_bdev_ocf_remove_core_decoders[] = {
	{"core_name", offsetof(struct rpc_bdev_ocf_remove_core, core_name), spdk_json_decode_string},
};

static void
rpc_bdev_ocf_remove_core_cb(void *cb_arg, int error)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (error) {
		spdk_jsonrpc_send_error_response(request, error, spdk_strerror(-error));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_ocf_remove_core(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_remove_core req = {};

	if (spdk_json_decode_object(params, rpc_bdev_ocf_remove_core_decoders,
				    SPDK_COUNTOF(rpc_bdev_ocf_remove_core_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_ocf_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parametes");
		goto cleanup;
	}

	vbdev_ocf_core_remove(req.core_name, rpc_bdev_ocf_remove_core_cb, request);

cleanup:
	free_rpc_bdev_ocf_remove_core(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_remove_core", rpc_bdev_ocf_remove_core, SPDK_RPC_RUNTIME)

struct rpc_bdev_ocf_get_bdevs {
	char *name;
};

static void
free_rpc_bdev_ocf_get_bdevs(struct rpc_bdev_ocf_get_bdevs *r) {
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_ocf_get_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ocf_get_bdevs, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_ocf_get_bdevs_cb(void *cb_arg1, void *cb_arg2)
{
	struct spdk_json_write_ctx *w = cb_arg1;
	struct spdk_jsonrpc_request *request = cb_arg2;

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_ocf_get_bdevs(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdev_ocf_get_bdevs req = {};
	struct spdk_json_write_ctx *w;

	if (params && spdk_json_decode_object(params, rpc_bdev_ocf_get_bdevs_decoders,
					      SPDK_COUNTOF(rpc_bdev_ocf_get_bdevs_decoders),
					      &req)) {
		SPDK_DEBUGLOG(vbdev_ocf_rpc, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parametes");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	vbdev_ocf_get_bdevs(params ? req.name : NULL, rpc_bdev_ocf_get_bdevs_cb, w, request);

cleanup:
	free_rpc_bdev_ocf_get_bdevs(&req);
}
SPDK_RPC_REGISTER("bdev_ocf_get_bdevs", rpc_bdev_ocf_get_bdevs, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_set_cachemode(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
}
SPDK_RPC_REGISTER("bdev_ocf_set_cachemode", rpc_bdev_ocf_set_cachemode, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_set_cleaning(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
}
SPDK_RPC_REGISTER("bdev_ocf_set_cleaning", rpc_bdev_ocf_set_cleaning, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_set_seqcutoff(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
}
SPDK_RPC_REGISTER("bdev_ocf_set_seqcutoff", rpc_bdev_ocf_set_seqcutoff, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_get_stats(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
}
SPDK_RPC_REGISTER("bdev_ocf_get_stats", rpc_bdev_ocf_get_stats, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_reset_stats(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
}
SPDK_RPC_REGISTER("bdev_ocf_reset_stats", rpc_bdev_ocf_reset_stats, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_flush_start(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
}
SPDK_RPC_REGISTER("bdev_ocf_flush_start", rpc_bdev_ocf_flush_start, SPDK_RPC_RUNTIME)

static void
rpc_bdev_ocf_flush_status(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
}
SPDK_RPC_REGISTER("bdev_ocf_flush_status", rpc_bdev_ocf_flush_status, SPDK_RPC_RUNTIME)

SPDK_LOG_REGISTER_COMPONENT(vbdev_ocf_rpc)
