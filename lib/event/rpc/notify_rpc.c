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
#include "spdk/queue.h"
#include "spdk/notify.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "notify_rpc.h"

/* MOC
 * TODO: imeplement
*/
static void
spdk_notify_unlisten(uint32_t listen_id)
{

}

static struct spdk_jsonrpc_server_conn *
spdk_jsonrpc_get_conn(struct spdk_jsonrpc_request *request)
{
	return NULL;
}

struct notify_request {
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;

	STAILQ_ENTRY(notify_request) link;
};

struct notify_ctx {
	/* Notification listen ID for unregister */
	uint32_t listen_id;

	struct spdk_jsonrpc_server_conn *conn;
	bool conn_closed;
	struct notify_request *current_request;

	STAILQ_HEAD(, notify_request) requests;
	STAILQ_ENTRY(notify_ctx) link;
};

static STAILQ_HEAD(, notify_ctx) g_rpc_notify_listeners = STAILQ_HEAD_INITIALIZER(
			g_rpc_notify_listeners);

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
	TAILQ_FOREACH(ntype, &g_notify_types, tailq) {
		spdk_json_write_string(w, ntype->name);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_notification_types", spdk_rpc_get_notification_types,
		  SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)

static int
rpc_notication_start(struct notify_ctx *ctx)
{
	assert(ctx->current_request == NULL);
	struct notify_request *req;

	while ((req = STAILQ_FIRST(&ctx->requests)) != NULL) {
		STAILQ_REMOVE_HEAD(&ctx->requests, link);

		req->w = spdk_jsonrpc_begin_result(req->request);
		if (!req->w) {
			free(req);
			continue;
		}

		ctx->current_request = req;
		spdk_json_write_array_begin(req->w);
		return 0;
	}

	return -ENOBUFS;
}

static void
rpc_notication_end(struct notify_ctx *ctx)
{
	struct notify_request *req = ctx->current_request;

	assert(req);
	ctx->current_request = NULL;
	spdk_json_write_array_end(req->w);
	spdk_jsonrpc_end_result(req->request, req->w);
	free(req);
}

static void
rpc_notication_cb(struct spdk_notify_type *notify, void *arg)
{
	struct notify_ctx *ctx = arg;

	SPDK_ERRLOG("Connection %p: notification %s\n", ctx->conn, notify->name);
}

static void
spdk_rpc_notify_close_connection(struct spdk_jsonrpc_server_conn *conn, void *arg)
{
	struct notify_ctx *ctx = arg;

	SPDK_ERRLOG("Connection %p: closed\n", conn);
	spdk_notify_unlisten(ctx->listen_id);

	while (!STAILQ_EMPTY(&ctx->requests)) {
		rpc_notication_start(ctx);
		rpc_notication_end(ctx);
	}

	STAILQ_REMOVE(&g_rpc_notify_listeners, ctx, notify_ctx, link);
	free(ctx);
}

static void
spdk_rpc_get_notifications(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct notify_request *nreq = NULL;
	struct spdk_jsonrpc_server_conn *conn;
	struct notify_ctx *ctx;
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters required (yet)");
		return;
	}

	conn = spdk_jsonrpc_get_conn(request);
	STAILQ_FOREACH(ctx, &g_rpc_notify_listeners, link) {
		if (ctx->conn == conn) {
			break;
		}
	}

	if (ctx == NULL) {
		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			rc = -ENOMEM;
			goto invalid;
		}

		rc = spdk_notify_listen(NULL, rpc_notication_cb, ctx);
		if (rc) {
			free(ctx);
			goto invalid;
		}

		spdk_jsonrpc_conn_set_close_cb(conn, spdk_rpc_notify_close_connection, ctx);
		ctx->conn = conn;
		STAILQ_INIT(&ctx->requests);
		STAILQ_INSERT_TAIL(&g_rpc_notify_listeners, ctx, link);
	}

	nreq = calloc(1, sizeof(*nreq));
	nreq->request = request;

	ctx->current_request = nreq;
	STAILQ_INSERT_TAIL(&ctx->requests, nreq, link);

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("get_notifications", spdk_rpc_get_notifications,
		  SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)
