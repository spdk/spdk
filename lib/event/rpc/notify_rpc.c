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
#include "spdk/notify.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

static void
spdk_rpc_get_notification_types(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_notify_type *ntype;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters required");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	ntype = spdk_notify_type_first();
	while (ntype) {
		spdk_json_write_string(w, spdk_notify_type_get_name(ntype));
		ntype = spdk_notify_type_next(ntype);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_notification_types", spdk_rpc_get_notification_types, SPDK_RPC_RUNTIME)

struct rpc_get_notifications {
	uint64_t id;
};

static const struct spdk_json_object_decoder rpc_get_notifications_decoders[] = {
	{"id", offsetof(struct rpc_get_notifications, id), spdk_json_decode_uint64, true},
};

static void
spdk_rpc_get_notifications(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_get_notifications req = {0};
	const struct spdk_notify_event *ev;
	struct spdk_json_write_ctx *w;
	int rc;

	if (params &&
	    spdk_json_decode_object(params, rpc_get_notifications_decoders,
				    SPDK_COUNTOF(rpc_get_notifications_decoders), &req)) {
		SPDK_DEBUGLOG(SPDK_NOTIFY_RPC, "spdk_json_decode_object failed\n");
		rc =  -EINVAL;
		goto invalid;
	}


	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	while ((ev = spdk_notify_get_event(req.id)) != NULL) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "type", ev->type);
		spdk_json_write_named_string(w, "ctx", ev->ctx);
		spdk_json_write_named_uint64(w, "id", req.id);
		spdk_json_write_object_end(w);
		req.id++;
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("get_notifications", spdk_rpc_get_notifications, SPDK_RPC_RUNTIME)

SPDK_LOG_REGISTER_COMPONENT("notify_rpc", SPDK_NOTIFY_RPC)
