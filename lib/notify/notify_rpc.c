/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */


#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/notify.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include "spdk_internal/rpc_autogen.h"

#include "spdk/log.h"

static int
notify_get_types_cb(const struct spdk_notify_type *type, void *ctx)
{
	spdk_json_write_string((struct spdk_json_write_ctx *)ctx, spdk_notify_type_get_name(type));
	return 0;
}

static void
rpc_notify_get_types(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters required");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	spdk_notify_foreach_type(notify_get_types_cb, w);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("notify_get_types", rpc_notify_get_types, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_notify_get_notifications_decoders[] = {
	{"id", offsetof(struct rpc_notify_get_notifications_ctx, id), spdk_json_decode_uint64, true},
	{"max", offsetof(struct rpc_notify_get_notifications_ctx, max), spdk_json_decode_uint64, true},
};

static int
notify_get_notifications_cb(uint64_t id, const struct spdk_notify_event *ev, void *ctx)
{
	struct spdk_json_write_ctx *w = ctx;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "type", ev->type);
	spdk_json_write_named_string(w, "ctx", ev->ctx);
	spdk_json_write_named_uint64(w, "id", id);
	spdk_json_write_object_end(w);
	return 0;
}

static void
rpc_notify_get_notifications(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_notify_get_notifications_ctx req = {.max = UINT64_MAX};
	struct spdk_json_write_ctx *w;

	if (params &&
	    spdk_json_decode_object(params, rpc_notify_get_notifications_decoders,
				    SPDK_COUNTOF(rpc_notify_get_notifications_decoders), &req)) {
		SPDK_DEBUGLOG(notify_rpc, "spdk_json_decode_object failed\n");

		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(EINVAL));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_array_begin(w);
	spdk_notify_foreach_event(req.id, req.max, notify_get_notifications_cb, w);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("notify_get_notifications", rpc_notify_get_notifications, SPDK_RPC_RUNTIME)

SPDK_LOG_REGISTER_COMPONENT(notify_rpc)
