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
#include "common.h"

static void
spdk_rpc_construct_ftl_bdev(struct spdk_bdev_nvme_construct_opts *opts,
			    spdk_rpc_construct_bdev_cb_fn cb_fn, void *cb_arg)
{
	struct ftl_bdev_init_opts ftl_opts = {};
	int rc;

	ftl_opts.name = opts->name;
	ftl_opts.mode = SPDK_FTL_MODE_CREATE;
	ftl_opts.cache_bdev = opts->cache_bdev;
	ftl_opts.ftl_conf = opts->ftl_conf;
	ftl_opts.trid = opts->trid;
	ftl_opts.range = opts->range;
	if (opts->uuid) {
		ftl_opts.uuid = *opts->uuid;

		if (!spdk_mem_all_zero(&opts->uuid, sizeof(opts->uuid))) {
			ftl_opts.mode &= ~SPDK_FTL_MODE_CREATE;
		}
	}

	rc = bdev_ftl_init_bdev(&ftl_opts, cb_fn, cb_arg);
	if (rc) {
		cb_fn(NULL, cb_arg, rc);
	}
	free(opts->uuid);
}

static int
spdk_rpc_parse_ftl_bdev_args(struct rpc_construct_nvme *req,
			     struct spdk_bdev_nvme_construct_opts *opts)
{
	if (req->cache_bdev && !spdk_bdev_get_by_name(req->cache_bdev)) {
		return -EINVAL;
	}

	if (bdev_ftl_parse_punits(&opts->range, req->punits)) {
		return -EINVAL;
	}

	if (req->uuid) {
		opts->uuid = calloc(1, sizeof(struct spdk_uuid));
		if (spdk_uuid_parse(opts->uuid, req->uuid) < 0) {
			return -EINVAL;
		}
	}

	memcpy(&opts->ftl_conf, &req->ftl_conf, sizeof(struct spdk_ftl_conf));

	return 0;
}
SPDK_RPC_REGISTER_CONSTRUCT_FNS("ftl", spdk_rpc_construct_ftl_bdev, spdk_rpc_parse_ftl_bdev_args)

struct rpc_delete_ftl {
	char *name;
};

static const struct spdk_json_object_decoder rpc_delete_ftl_decoders[] = {
	{"name", offsetof(struct rpc_delete_ftl, name), spdk_json_decode_string},
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
