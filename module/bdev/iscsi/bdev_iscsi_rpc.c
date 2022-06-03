/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_iscsi_delete(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct rpc_delete_iscsi req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_iscsi_decoders,
				    SPDK_COUNTOF(rpc_delete_iscsi_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_iscsi_disk(req.name, rpc_bdev_iscsi_delete_cb, request);

cleanup:
	free_rpc_delete_iscsi(&req);
}
SPDK_RPC_REGISTER("bdev_iscsi_delete", rpc_bdev_iscsi_delete, SPDK_RPC_RUNTIME)
