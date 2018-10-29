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

#include <spdk/rpc.h>
#include <spdk/util.h>
#include <spdk/bdev_module.h>
#include <spdk_internal/log.h>
#include "bdev_ocssd.h"

struct rpc_construct_ocssd {
	char *name;
	char *trtype;
	char *traddr;
	char *punits;
	unsigned int mode;
	char *uuid;
};

static void
free_rpc_construct_ocssd(struct rpc_construct_ocssd *req)
{
	free(req->name);
	free(req->trtype);
	free(req->traddr);
	free(req->punits);
	free(req->uuid);
}

static const struct spdk_json_object_decoder rpc_construct_ocssd_decoders[] = {
	{"name", offsetof(struct rpc_construct_ocssd, name), spdk_json_decode_string},
	{"trtype", offsetof(struct rpc_construct_ocssd, trtype), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_construct_ocssd, traddr), spdk_json_decode_string},
	{"punits", offsetof(struct rpc_construct_ocssd, punits), spdk_json_decode_string},
	{"mode", offsetof(struct rpc_construct_ocssd, mode), spdk_json_decode_uint32},
	{"uuid", offsetof(struct rpc_construct_ocssd, uuid), spdk_json_decode_string},
};

#define OCSSD_RANGE_MAX_LENGTH 32

static void
_spdk_rpc_construct_ocssd_bdev_cb(const struct ocssd_bdev_info *bdev_info, void *ctx, int status)
{
	struct spdk_jsonrpc_request *request = ctx;
	char bdev_uuid[SPDK_UUID_STRING_LEN];
	struct spdk_json_write_ctx *w;

	if (status) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (!w) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "spdk_json_begin_result failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		return;
	}

	spdk_json_write_array_begin(w);
	spdk_uuid_fmt_lower(bdev_uuid, sizeof(bdev_uuid), &bdev_info->uuid);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", bdev_info->name);
	spdk_json_write_named_string(w, "uuid", bdev_uuid);
	spdk_json_write_object_end(w);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_construct_ocssd_bdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_construct_ocssd req = {};
	struct ocssd_bdev_init_opts opts = {};
	char range[OCSSD_RANGE_MAX_LENGTH];

	if (spdk_json_decode_object(params, rpc_construct_ocssd_decoders,
				    SPDK_COUNTOF(rpc_construct_ocssd_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_OCSSD, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	opts.name = req.name;

	/* Parse trtype */
	if (spdk_nvme_transport_id_parse_trtype(&opts.trid.trtype, req.trtype) < 0) {
		SPDK_ERRLOG("Failed to parse trtype: %s\n", req.trtype);
		goto invalid;
	}

	if (opts.trid.trtype != SPDK_NVME_TRANSPORT_PCIE) {
		SPDK_ERRLOG("Devices other than PCIe not supported %s\n", opts.trid.traddr);
		goto invalid;
	}

	/* Parse traddr */
	snprintf(opts.trid.traddr, sizeof(opts.trid.traddr), "%s", req.traddr);
	snprintf(range, sizeof(range), "%s", req.punits);

	if (bdev_ocssd_parse_punits(&opts.range, req.punits)) {
		SPDK_ERRLOG("Failed to parse parallel unit range\n");
		goto invalid;
	}

	opts.mode = req.mode;
	if (!(opts.mode & SPDK_FTL_MODE_CREATE)) {
		if (spdk_uuid_parse(&opts.uuid, req.uuid) < 0) {
			SPDK_ERRLOG("Failed to parse uuid: %s\n", req.uuid);
			goto invalid;
		}
	}

	if (bdev_ocssd_init_bdev(&opts, _spdk_rpc_construct_ocssd_bdev_cb, request)) {
		SPDK_ERRLOG("Failed to create OCSSD bdev\n");
		goto invalid;
	}

	goto free_rpc;
invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
free_rpc:
	free_rpc_construct_ocssd(&req);
}

SPDK_RPC_REGISTER("construct_ocssd_bdev", spdk_rpc_construct_ocssd_bdev, SPDK_RPC_RUNTIME)

struct rpc_delete_ocssd {
	char *name;
};

static const struct spdk_json_object_decoder rpc_delete_ocssd_decoders[] = {
	{"name", offsetof(struct rpc_construct_ocssd, name), spdk_json_decode_string},
};

static void
_spdk_rpc_delete_ocssd_bdev_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_delete_ocssd_bdev(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_delete_ocssd attrs = {};

	if (spdk_json_decode_object(params, rpc_delete_ocssd_decoders,
				    SPDK_COUNTOF(rpc_delete_ocssd_decoders),
				    &attrs)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
	}

	bdev_ocssd_delete_bdev(attrs.name, _spdk_rpc_delete_ocssd_bdev_cb, request);
	free(attrs.name);
}

SPDK_RPC_REGISTER("delete_ocssd_bdev", spdk_rpc_delete_ocssd_bdev, SPDK_RPC_RUNTIME)
