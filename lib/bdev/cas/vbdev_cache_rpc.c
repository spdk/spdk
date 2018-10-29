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

#include "vbdev_cas.h"
#include <spdk/log.h>
#include <spdk/rpc.h>
#include <stats.h>

/* Structure to hold the parameters for this RPC method. */
struct rpc_construct_cache_bdev {
	char *name;		/* master vbdev */
	char *mode;		/* CAS mode (choose one) */
	char *cache_bdev_name;	/* sub bdev */
	char *core_bdev_name;	/* sub bdev */
};

static void
free_rpc_construct_cache_bdev(struct rpc_construct_cache_bdev *r)
{
	free(r->name);
	free(r->core_bdev_name);
	free(r->cache_bdev_name);
	free(r->mode);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_construct_cache_bdev_decoders[] = {
	{"name", offsetof(struct rpc_construct_cache_bdev, name), spdk_json_decode_string},
	{"mode", offsetof(struct rpc_construct_cache_bdev, mode), spdk_json_decode_string},
	{"cache_bdev_name", offsetof(struct rpc_construct_cache_bdev, cache_bdev_name), spdk_json_decode_string},
	{"core_bdev_name", offsetof(struct rpc_construct_cache_bdev, core_bdev_name), spdk_json_decode_string},
};

static void
spdk_rpc_construct_cache_bdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	int ret = 0;
	struct rpc_construct_cache_bdev req = {NULL};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_construct_cache_bdev_decoders,
				    SPDK_COUNTOF(rpc_construct_cache_bdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	ret = vbdev_cas_construct(req.name, req.mode, req.cache_bdev_name, req.core_bdev_name);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Could not create cache vbdev");
		goto end;
	}

	if ((w = spdk_jsonrpc_begin_result(request))) {
		spdk_json_write_string(w, req.name);
		spdk_jsonrpc_end_result(request, w);
	}

end:
	free_rpc_construct_cache_bdev(&req);
}
SPDK_RPC_REGISTER("construct_cache_bdev", spdk_rpc_construct_cache_bdev, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_get_cache_stats {
	char *name;		/* master vbdev */
	char *statname; /* < usage | reqs | blocks | errors > */
};

static void
free_rpc_get_cache_stats(struct rpc_get_cache_stats *r)
{
	free(r->name);
	free(r->statname);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_get_cache_stats_decoders[] = {
	{"name", offsetof(struct rpc_get_cache_stats, name), spdk_json_decode_string},
	{"statname", offsetof(struct rpc_get_cache_stats, statname), spdk_json_decode_string},
};

static void
rpc_print_stats(const char *text, void *ctx)
{
	struct spdk_json_write_ctx *w = ctx;
	spdk_json_write_string(w, text);
}

struct rpc_stat_call {
	cache_get_stats_fn_t fn;
	void *statsptr;
};

static struct rpc_stat_call
stats_get_method_by_name(struct cache_stats *stats, const char *statname)
{
	struct rpc_stat_call ret = { NULL, NULL };

	char *names[] = {
		"usage",
		"reqs",
		"blocks",
		"errors"
	};
	cache_get_stats_fn_t fns[] = {
		(cache_get_stats_fn_t)cache_stats_write_usage,
		(cache_get_stats_fn_t)cache_stats_write_reqs,
		(cache_get_stats_fn_t)cache_stats_write_blocks,
		(cache_get_stats_fn_t)cache_stats_write_errors
	};
	void *stat_ptrs[] = {
		&stats->usage,
		&stats->reqs,
		&stats->blocks,
		&stats->errors
	};

	for (size_t i = 0; i < (sizeof(names) / sizeof(*names)); i++) {
		if (0 == strcmp(names[i], statname)) {
			ret.fn = fns[i];
			ret.statsptr = stat_ptrs[i];
			break;
		}
	}
	return ret;
}

static void
spdk_rpc_get_cache_stats(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_get_cache_stats req = {NULL};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_get_cache_stats_decoders,
				    SPDK_COUNTOF(rpc_get_cache_stats_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	struct vbdev_cas *cache_dev = vbdev_cas_get_by_name(req.name);
	if (cache_dev) {
		struct cache_stats *stats = (struct cache_stats *) malloc(sizeof(*stats));
		int status = cache_get_stats(cache_dev->cache.id, cache_dev->core.id, stats);
		if (status) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Error on cache_get_stats");
		} else {
			struct rpc_stat_call print_call = stats_get_method_by_name(stats, req.statname);
			if (print_call.fn) {
				w = spdk_jsonrpc_begin_result(request);
				if (w) {
					print_call.fn(print_call.statsptr, rpc_print_stats, w);
					spdk_jsonrpc_end_result(request, w);
				} else {
					spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
									 "Could not initiate responce");
				}
			} else {
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
								 "Incorrect stat name");
			}
		}
		free(stats);
	} else {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Incorrect cache name");
	}

end:
	free_rpc_get_cache_stats(&req);
}
SPDK_RPC_REGISTER("get_cache_stats", spdk_rpc_get_cache_stats, SPDK_RPC_RUNTIME)

/* Structure to hold the parameters for this RPC method. */
struct rpc_delete_cache_bdev {
	char *name;		/* master vbdev */
};

static void
free_rpc_delete_cache_bdev(struct rpc_delete_cache_bdev *r)
{
	free(r->name);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_delete_cache_bdev_decoders[] = {
	{"name", offsetof(struct rpc_delete_cache_bdev, name), spdk_json_decode_string},
};

static void
spdk_rpc_delete_cache_bdev(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_delete_cache_bdev req = {NULL};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_delete_cache_bdev_decoders,
				    SPDK_COUNTOF(rpc_delete_cache_bdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	/*
	 * Deleting
	 */

	struct spdk_bdev *exp_bdev = spdk_bdev_get_by_name(req.name);
	if (NULL == exp_bdev) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Incorrect cache name");
		goto end;
	}

	spdk_bdev_unregister(exp_bdev, NULL, NULL);

	/*
	 * Write response
	 */
	if ((w = spdk_jsonrpc_begin_result(request))) {
		spdk_json_write_string(w, req.name);
		spdk_jsonrpc_end_result(request, w);
	}

end:
	free_rpc_delete_cache_bdev(&req);
}
SPDK_RPC_REGISTER("delete_cache_bdev", spdk_rpc_delete_cache_bdev, SPDK_RPC_RUNTIME)
