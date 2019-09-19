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
#include "spdk/likely.h"
#include "bdev_ocssd.h"

#define BDEV_OCSSD_DEFAULT_NSID 1

struct rpc_create_ocssd_bdev {
	char		*ctrlr_name;
	char		*bdev_name;
	uint32_t	nsid;
};

static const struct spdk_json_object_decoder rpc_create_ocssd_bdev_decoders[] = {
	{"ctrlr_name", offsetof(struct rpc_create_ocssd_bdev, ctrlr_name), spdk_json_decode_string},
	{"bdev_name", offsetof(struct rpc_create_ocssd_bdev, bdev_name), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_create_ocssd_bdev, nsid), spdk_json_decode_uint32, true},
};

static void
free_rpc_create_ocssd_bdev(struct rpc_create_ocssd_bdev *rpc)
{
	free(rpc->ctrlr_name);
	free(rpc->bdev_name);
}

static void
rpc_bdev_ocssd_create_done(struct spdk_bdev *bdev, int status, void *ctx)
{
	struct spdk_jsonrpc_request *request = ctx;
	struct spdk_json_write_ctx *w;

	if (status != 0) {
		spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_ocssd_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_create_ocssd_bdev req = { .nsid = BDEV_OCSSD_DEFAULT_NSID };
	int rc;

	if (spdk_json_decode_object(params, rpc_create_ocssd_bdev_decoders,
				    SPDK_COUNTOF(rpc_create_ocssd_bdev_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed to parse the request");
		goto out;
	}

	rc = spdk_bdev_ocssd_create_bdev(req.ctrlr_name, req.bdev_name, req.nsid,
					 rpc_bdev_ocssd_create_done, request);
	if (spdk_unlikely(rc != 0)) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;

	}
out:
	free_rpc_create_ocssd_bdev(&req);
}

SPDK_RPC_REGISTER("bdev_ocssd_create", rpc_bdev_ocssd_create, SPDK_RPC_RUNTIME)

struct rpc_delete_ocssd_bdev {
	char *name;
};

static const struct spdk_json_object_decoder rpc_delete_ocssd_bdev_decoders[] = {
	{"name", offsetof(struct rpc_delete_ocssd_bdev, name), spdk_json_decode_string},
};

static void
free_rpc_delete_ocssd_bdev(struct rpc_delete_ocssd_bdev *rpc)
{
	free(rpc->name);
}

static void
rpc_bdev_ocssd_delete_done(int status, void *ctx)
{
	struct spdk_jsonrpc_request *request = ctx;
	struct spdk_json_write_ctx *w;

	if (status != 0) {
		spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_int32(w, 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_ocssd_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_delete_ocssd_bdev req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_ocssd_bdev_decoders,
				    SPDK_COUNTOF(rpc_delete_ocssd_bdev_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, -EINVAL, "Failed to parse the request");
		goto out;
	}

	rc = spdk_bdev_ocssd_delete_bdev(req.name, rpc_bdev_ocssd_delete_done, request);
	if (spdk_unlikely(rc != 0)) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}
out:
	free_rpc_delete_ocssd_bdev(&req);
}

SPDK_RPC_REGISTER("bdev_ocssd_delete", rpc_bdev_ocssd_delete, SPDK_RPC_RUNTIME)
