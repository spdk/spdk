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

#include "bdev_iscsi.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk/log.h"

struct rpc_bdev_iscsi_create {
	char *name;
	char *initiator_iqn;
	char *url;
};

static const struct spdk_json_object_decoder rpc_bdev_iscsi_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_iscsi_create, name), spdk_json_decode_string},
	{"initiator_iqn", offsetof(struct rpc_bdev_iscsi_create, initiator_iqn), spdk_json_decode_string},
	{"url", offsetof(struct rpc_bdev_iscsi_create, url), spdk_json_decode_string},
};

static void
free_rpc_bdev_iscsi_create(struct rpc_bdev_iscsi_create *req)
{
	free(req->name);
	free(req->initiator_iqn);
	free(req->url);
}

static void
bdev_iscsi_create_cb(void *cb_arg, struct spdk_bdev *bdev, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (status > 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "iSCSI error (%d).", status);
	} else if (status < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(-status));
	} else {
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_string(w, spdk_bdev_get_name(bdev));
		spdk_jsonrpc_end_result(request, w);
	}
}

static void
rpc_bdev_iscsi_create(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_bdev_iscsi_create req = {};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_iscsi_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_iscsi_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = create_iscsi_disk(req.name, req.url, req.initiator_iqn, bdev_iscsi_create_cb, request);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}

cleanup:
	free_rpc_bdev_iscsi_create(&req);
}
SPDK_RPC_REGISTER("bdev_iscsi_create", rpc_bdev_iscsi_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_iscsi_create, construct_iscsi_bdev)

struct rpc_delete_iscsi {
	char *name;
};

static void
free_rpc_delete_iscsi(struct rpc_delete_iscsi *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_iscsi_decoders[] = {
	{"name", offsetof(struct rpc_delete_iscsi, name), spdk_json_decode_string},
};

static void
rpc_bdev_iscsi_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_iscsi_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_iscsi req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_iscsi_decoders,
				    SPDK_COUNTOF(rpc_delete_iscsi_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	delete_iscsi_disk(bdev, rpc_bdev_iscsi_delete_cb, request);

cleanup:
	free_rpc_delete_iscsi(&req);
}
SPDK_RPC_REGISTER("bdev_iscsi_delete", rpc_bdev_iscsi_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_iscsi_delete, delete_iscsi_bdev)
