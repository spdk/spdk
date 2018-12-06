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
// #include "spdk/notify.h"
struct spdk_notication {
	void (*write_json)(struct spdk_json_write_ctx *w, struct spdk_notication *n);
	void *ctx;


	/* void *ctx; */
	char *method;
	char *bdev_name;


};

#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "notify_rpc.h"

/* XXX: For debug only */
#include "spdk/thread.h"

struct rpc_notify_request {
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;

	uint32_t timeout_ms;
	/* Array of strings */
	struct spdk_json_val *notification_types;
	uint32_t min_events;
	uint32_t max_events;

	struct rpc_notify_listener *notify_listener; /* this connection notify ctx */
	TAILQ_ENTRY(rpc_notify_request) link;
};

/* Agregate connection and all notify requests for it */
struct rpc_notify_listener {

	struct spdk_jsonrpc_server_conn *conn;

	bool conn_closed;

	TAILQ_HEAD(, rpc_notify_request) requests;
	TAILQ_ENTRY(rpc_notify_listener) link;

	/* XXX: this is for testing */
	struct spdk_poller *poller;
};

static TAILQ_HEAD(, rpc_notify_listener) g_rpc_notify_listeners = TAILQ_HEAD_INITIALIZER(
			g_rpc_notify_listeners);

static void rpc_notify_listener_connection_closed(struct spdk_jsonrpc_server_conn *conn, void *arg);
static int rpc_notify_simulate(void *arg);

/* return existing or create new lister for connection of given request. */
static struct rpc_notify_listener *
rpc_notify_listener_get(struct spdk_jsonrpc_request *request)
{
	struct rpc_notify_listener *ctx;

	struct spdk_jsonrpc_server_conn *conn = spdk_jsonrpc_get_conn(request);
	TAILQ_FOREACH(ctx, &g_rpc_notify_listeners, link) {
		if (ctx->conn == conn) {
			return ctx;
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx) {
		ctx->conn = conn;
		TAILQ_INIT(&ctx->requests);
		TAILQ_INSERT_TAIL(&g_rpc_notify_listeners, ctx, link);

		/* TODO: register in spdk_notify_listen */
		spdk_jsonrpc_conn_add_close_cb(conn, rpc_notify_listener_connection_closed, ctx);

		/* Simulate event(s) every 1s */
		ctx->poller = spdk_poller_register(rpc_notify_simulate, ctx, 1000000);
	}

	return ctx;
}

static void
rpc_notify_listener_put(struct rpc_notify_listener *listener)
{
	SPDK_ERRLOG("Removing listener %p\n", listener);
	spdk_poller_unregister(&listener->poller);
	spdk_jsonrpc_conn_del_close_cb(listener->conn, rpc_notify_listener_connection_closed, listener);
	TAILQ_REMOVE(&g_rpc_notify_listeners, listener, link);
	free(listener);
}

static void
rpc_notify_listener_add_request(struct rpc_notify_request *req)
{
	struct rpc_notify_listener *listener = rpc_notify_listener_get(req->request);

	req->notify_listener = listener;
	/* Add this request to queue */
	TAILQ_INSERT_TAIL(&listener->requests, req, link);
}

static void
rpc_notify_listener_put_request(struct rpc_notify_request *req)
{
	struct rpc_notify_listener *listener = req->notify_listener;

	TAILQ_REMOVE(&listener->requests, req, link);
	if (TAILQ_EMPTY(&listener->requests)) {
		rpc_notify_listener_put(listener);
	}

	free(req);
}


static struct rpc_notify_request *
rpc_notify_listener_get_request(struct rpc_notify_listener *listener)
{
	struct rpc_notify_request *req;

	while ((req = TAILQ_FIRST(&listener->requests)) != NULL) {
		req->w = spdk_jsonrpc_begin_result(req->request);
		if (!req->w) {
			rpc_notify_listener_put_request(req);
			continue;
		}

		spdk_json_write_array_begin(req->w);
		break;
	}

	return req;
}

static void
rpc_notify_listener_end_request(struct rpc_notify_request *req)
{
	spdk_json_write_array_end(req->w);
	spdk_jsonrpc_end_result(req->request, req->w);

	rpc_notify_listener_put_request(req);
}

/* Called on new notification */
static void
rpc_notify_listener_cb(struct spdk_notication *notify, void *arg)
{
	struct rpc_notify_listener *ctx = arg;
	struct rpc_notify_request *req;

	SPDK_ERRLOG("Connection %p: notification %p\n", ctx->conn, notify);
	req = rpc_notify_listener_get_request(ctx);
	if (req) {
		notify->write_json(req->w, notify);
		rpc_notify_listener_end_request(req);
	} else {
		/* Wanring: ingored as all of the requests have JSON ID */
		assert(false);
	}
}

static void
rpc_notify_simulate_write(struct spdk_json_write_ctx *w, struct spdk_notication *n)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", n->method);

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", n->bdev_name);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
rpc_notify_simulate(void *arg)
{
	struct rpc_notify_listener *ctx = arg;



	struct spdk_notication n = {
		.write_json = rpc_notify_simulate_write,
		.method = "delete_malloc_bdev",
		.bdev_name = "Malloc0",
	};

	assert(!TAILQ_EMPTY(&ctx->requests));

	rpc_notify_listener_cb(&n, arg);

	return -1;
}

static void
rpc_notify_listener_connection_closed(struct spdk_jsonrpc_server_conn *conn, void *arg)
{
	struct rpc_notify_listener *listener = arg;

	SPDK_ERRLOG("Connection %p: closed\n", conn);
	/* TODO: spdk_notify_unlisten() */

	assert(!TAILQ_EMPTY(&listener->requests));

	while (!TAILQ_EMPTY(&listener->requests)) {
		rpc_notify_listener_put_request(TAILQ_FIRST(&listener->requests));
	}
}

static void
spdk_rpc_get_notification_types(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	/* struct spdk_notify_type *ntype; */

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
	/*
	TAILQ_FOREACH(ntype, &g_notify_types, tailq) {
		spdk_json_write_string(w, ntype->name);
	}
	*/

	spdk_json_write_string(w, "fake_type1");
	spdk_json_write_string(w, "fake_type2");
	spdk_json_write_string(w, "fake_type3");

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_notification_types", spdk_rpc_get_notification_types,
		  SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)

static int
cap_array(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **out_array = out;

	if (val->type == SPDK_JSON_VAL_NULL) {
		*out_array = NULL;
		return 0;
	}

	if (val->type != SPDK_JSON_VAL_ARRAY_BEGIN) {
		SPDK_ERRLOG("Not an array: '%*s'\n", val->len, (const char *)val->start);
		return SPDK_JSON_PARSE_INVALID;
	}

	*out_array = val;
	return 0;
}

static const struct spdk_json_object_decoder rpc_get_notifications_decoders[] = {
	{"timeout_ms", offsetof(struct rpc_notify_request, timeout_ms), spdk_json_decode_uint32, true},
	{"notification_types", offsetof(struct rpc_notify_request, notification_types), cap_array, true},
	{"min_events", offsetof(struct rpc_notify_request, min_events), spdk_json_decode_uint32, true},
	{"max_events", offsetof(struct rpc_notify_request, min_events), spdk_json_decode_uint32, true},
};

static void
spdk_rpc_get_notifications(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_notify_request *req = NULL;
	int rc;

	req = calloc(1, sizeof(*req));
	if (params &&
	    spdk_json_decode_object(params, rpc_get_notifications_decoders,
				    SPDK_COUNTOF(rpc_get_notifications_decoders), req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	SPDK_ERRLOG("New notification request\n");
	req->request = request;

	rpc_notify_listener_add_request(req);
	return;
invalid:
	free(req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("get_notifications", spdk_rpc_get_notifications,
		  SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)
