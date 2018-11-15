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

/* MOC
 * TODO: imeplement
*/
static void
spdk_notify_unlisten(uint32_t listen_id)
{

}

/* MOC
 * TODO: imeplement - get connection from request
 */
static struct spdk_jsonrpc_server_conn *
spdk_jsonrpc_get_conn(struct spdk_jsonrpc_request *request)
{
	return NULL;
}

/* MOC
 * TODO: implement - register connection close hook
 */
static int
spdk_jsonrpc_add_conn_close_hook(struct spdk_jsonrpc_server_conn *conn, void (*cb)(void *), void *ctx)
{
	return -ENOSYS;
}

/* MOC
 * TODO: implement - remove connection close hook
 */
static int
spdk_jsonrpc_del_conn_close_hook(struct spdk_jsonrpc_server_conn *conn, void (*cb)(void *), void *ctx)
{
	return -ENOSYS;
}

struct notify_request {
	struct spdk_jsonrpc_request *request;
	STAILQ_ENTRY(notify_request) link;
};

struct notify_ctx {
	/* Notification listen ID for unregister */
	uint32_t listen_id;

	struct spdk_jsonrpc_server_conn *conn;

	STAILQ_HEAD(, notify_request) requests;

	STAILQ_ENTRY(notify_ctx) link;
};

static STAILQ_HEAD(, notify_ctx) notify_listeners = STAILQ_HEAD_INITIALIZER(notify_listeners);


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

static void
rpc_notication_conn_close_cb(struct spdk_jsonrpc_server_conn *srv, void *arg)
{
	struct notify_ctx *ctx = arg;

	/* TODO:
	 * 1. undo spdk_notify_listen(spdk_notify_listen, ctx);
	 * 2. abort all ctx->requests
	 * 3. free ctx
	 */

	spdk_notify_unlisten(ctx->listen_id);
}

static void
rpc_notication_cb(struct spdk_notify_type *notify, void *arg)
{
	struct notify_ctx *ctx = arg;

	(void)ctx;
	/* TODO: write notification */
}

static void
spdk_rpc_listen_notifications(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct notify_ctx *ctx = NULL;
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters required (yet)");
		return;
	}

	/* TODO:
	 * Check if we are already listening for events on this connection. If yes,
	 * just modify or throw error.
	 */

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto invalid;
	}

	ctx->conn = spdk_jsonrpc_get_conn(request);
	rc = spdk_jsonrpc_add_conn_close_hook(ctx->conn, rpc_notication_conn_close_cb, ctx);
	if (rc < 0) {
		goto invalid;
	}

	rc = spdk_notify_listen(NULL, spdk_notify_listen, ctx);
	if (rc < 0) {
		spdk_jsonrpc_del_conn_close_hook(ctx->conn, rpc_notication_conn_close_cb, ctx);
		goto invalid;
	}

	STAILQ_INIT(&ctx->requests);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
	free(ctx);
}
SPDK_RPC_REGISTER("listen_notifications", spdk_rpc_listen_notifications,
		  SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)

static void
spdk_rpc_get_notifications(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct notify_ctx *ctx = NULL;
	int rc;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "No parameters required (yet)");
		return;
	}

	/* TODO:
	 * 0. Check if notification are listen on this connection
	 * 1. alloc struct notify_request
	 * 2. add it to list of requests for this connection
	 * 3. Done, wait for connections.
	 */

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("get_notifications", spdk_rpc_get_notifications,
		  SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)
