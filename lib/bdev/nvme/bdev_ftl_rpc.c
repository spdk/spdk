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
#include "spdk_internal/log.h"

#include "bdev_ftl.h"

struct rpc_construct_ftl {
	char *name;
	char *trtype;
	char *traddr;
	char *punits;
	char *uuid;
	char *cache_bdev;
	bool allow_open_bands;
};

static void
free_rpc_construct_ftl(struct rpc_construct_ftl *req)
{
	free(req->name);
	free(req->trtype);
	free(req->traddr);
	free(req->punits);
	free(req->uuid);
	free(req->cache_bdev);
}

static const struct spdk_json_object_decoder rpc_construct_ftl_decoders[] = {
	{"name", offsetof(struct rpc_construct_ftl, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_construct_ftl, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_construct_ftl, traddr), spdk_json_decode_string},
	{"punits", offsetof(struct rpc_construct_ftl, punits), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_construct_ftl, uuid), spdk_json_decode_string, true},
	{"cache", offsetof(struct rpc_construct_ftl, cache_bdev), spdk_json_decode_string, true},
	{
		"allow_open_bands", offsetof(struct rpc_construct_ftl, allow_open_bands),
		spdk_json_decode_bool, true
	},
};

#define FTL_RANGE_MAX_LENGTH 32

static void
_spdk_rpc_construct_ftl_bdev_cb(const struct ftl_bdev_info *bdev_info, void *ctx, int status)
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
spdk_rpc_construct_ftl_bdev(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_construct_ftl req = {};
	struct ftl_bdev_init_opts opts = {};
	char range[FTL_RANGE_MAX_LENGTH];
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_ftl_decoders,
				    SPDK_COUNTOF(rpc_construct_ftl_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto invalid;
	}

	if (req.cache_bdev && !spdk_bdev_get_by_name(req.cache_bdev)) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "No such bdev: %s", req.cache_bdev);
		goto invalid;
	}

	opts.name = req.name;
	opts.mode = SPDK_FTL_MODE_CREATE;
	opts.cache_bdev = req.cache_bdev;
	opts.allow_open_bands = req.allow_open_bands;

	/* Parse trtype */
	rc = spdk_nvme_transport_id_parse_trtype(&opts.trid.trtype, req.trtype);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to parse trtype: %s, rc: %s",
						     req.trtype, spdk_strerror(-rc));
		goto invalid;
	}

	if (opts.trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid trtype: %s. Only PCIe is supported",
						     req.trtype);
		goto invalid;
	}

	/* Parse traddr */
	snprintf(opts.trid.traddr, sizeof(opts.trid.traddr), "%s", req.traddr);
	snprintf(range, sizeof(range), "%s", req.punits);

	if (bdev_ftl_parse_punits(&opts.range, req.punits)) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to parse parallel unit range: %s",
						     req.punits);
		goto invalid;
	}

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

	rc = bdev_ftl_init_bdev(&opts, _spdk_rpc_construct_ftl_bdev_cb, request);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create FTL bdev: %s",
						     spdk_strerror(-rc));
		goto invalid;
	}

invalid:
	free_rpc_construct_ftl(&req);
}

SPDK_RPC_REGISTER("construct_ftl_bdev", spdk_rpc_construct_ftl_bdev, SPDK_RPC_RUNTIME)

struct rpc_delete_ftl {
	char *name;
};

static const struct spdk_json_object_decoder rpc_delete_ftl_decoders[] = {
	{"name", offsetof(struct rpc_construct_ftl, name), spdk_json_decode_string},
};

static void
_spdk_rpc_delete_ftl_bdev_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_delete_ftl_bdev(struct spdk_jsonrpc_request *request,
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

	bdev_ftl_delete_bdev(attrs.name, _spdk_rpc_delete_ftl_bdev_cb, request);
invalid:
	free(attrs.name);
}

SPDK_RPC_REGISTER("delete_ftl_bdev", spdk_rpc_delete_ftl_bdev, SPDK_RPC_RUNTIME)
