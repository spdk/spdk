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
#include "spdk/log.h"
#include "spdk/barrier.h"
#include "spdk/thread.h"
#include "spdk/env.h"

#include "spdk_internal/log.h"
#include "notify_rpc.h"

#define SPDK_NOTIFY_RPC_TIMEDOUT_REQ_POLL_MS  50
#define SPDK_NOTIFY_RPC_DEFAULT_TIMEOUT_MS  100

struct rpc_notify_request {
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;

	/* Time when this request will be completed. */
	uint64_t timeout;

	/* Array of strings */
	struct spdk_json_val *notification_types;

	TAILQ_ENTRY(rpc_notify_request) link;
};

/* Aggregate connection and all notify requests for it */
struct rpc_notify_listener {
	struct spdk_jsonrpc_server_conn *conn;

	/* List of pending RPC requests. No need to lock as used only from RPC thread. */
	TAILQ_HEAD(, rpc_notify_request) requests;

	/* Listeners queue link. */
	TAILQ_ENTRY(rpc_notify_listener) link;
};

/* This might be accessed only from notification (RPC) thread. */
static TAILQ_HEAD(, rpc_notify_listener) g_rpc_notify_listeners = TAILQ_HEAD_INITIALIZER(
			g_rpc_notify_listeners);

static struct spdk_poller *g_rpc_notify_poller;
static struct spdk_thread *g_rpc_notify_thread;

static void rpc_notify_listener_put(struct rpc_notify_listener *listener);

/* Start writing request. */
static void
rpc_notify_request_begin_result(struct rpc_notify_request *req)
{
	req->w = spdk_jsonrpc_begin_result(req->request);
	if (req->w != NULL) {
		spdk_json_write_array_begin(req->w);
	}
}

/* Finish request */
static void
rpc_notify_request_end_result(struct rpc_notify_listener *listener, struct rpc_notify_request *req)
{
	if (req->w) {
		spdk_json_write_array_end(req->w);
		spdk_jsonrpc_end_result(req->request, req->w);
	}

	TAILQ_REMOVE(&listener->requests, req, link);
	if (TAILQ_EMPTY(&listener->requests)) {
		rpc_notify_listener_put(listener);
	}

	free(req);
}

static void
rpc_notify_process_notification(struct rpc_notify_listener *listener, struct spdk_notify *notify)
{
	struct rpc_notify_request *req = TAILQ_FIRST(&listener->requests);

	if (req == NULL) {
		return;
	}

	if (req->w == NULL) {
		rpc_notify_request_begin_result(req);
	}

	if (req->w != NULL) {
		spdk_notify_write_json(req->w, notify);
	}

	rpc_notify_request_end_result(listener, req);
}

static void
rpc_notify_process_expired_reqs(struct rpc_notify_listener *listener, uint64_t now)
{
	struct rpc_notify_request *req;

	/* Finish all requests that timed out for this listener. */
	while( (req = TAILQ_FIRST(&listener->requests)) != NULL) {
		if (now < req->timeout) {
			break;
		}

		SPDK_DEBUGLOG(SPDK_NOTIFY_RPC, "Listener %p (conn: %p): request %p timed out\n", listener, listener->conn, req);
		if (req->w == NULL) {
			rpc_notify_request_begin_result(req);
		}

		if (req->w != NULL) {
			rpc_notify_request_end_result(listener, req);
		}
	}
}

static int
rpc_notify_process(struct spdk_notify *notify)
{
	struct rpc_notify_listener *listener;
	uint64_t now = spdk_get_ticks();

	TAILQ_FOREACH(listener, &g_rpc_notify_listeners, link) {
		if (notify) {
			rpc_notify_process_notification(listener, notify);
		}

		rpc_notify_process_expired_reqs(listener, now);
	}

	if (notify) {
		spdk_notify_put(notify);
		return -1;
	}

	return -1;
}

static void
rpc_notify_msg_cb(void *arg)
{
	rpc_notify_process(arg);
}

/* Check for timed out notification requests. */
static int
rpc_notify_poller_cb(void *arg)
{
	rpc_notify_process(NULL);
	return -1;
}

static void
rpc_notify_conn_closed(struct spdk_jsonrpc_server_conn *conn, void *arg)
{
	struct rpc_notify_listener *listener = arg;
	SPDK_INFOLOG(SPDK_NOTIFY_RPC, "Listener %p (conn: %p): connection closed\n", listener, listener->conn);

	assert(conn == listener->conn);
	rpc_notify_listener_put(listener);
}

static void
rpc_notify_new_notification_cb(struct spdk_notify *notify, void *arg)
{
	/* This might race with unregister but should never be
	 * called if there is no thread. */
	assert (g_rpc_notify_thread != NULL);

	SPDK_DEBUGLOG(SPDK_NOTIFY_RPC, "Notification %p: sending to thread '%s'\n", notify, spdk_thread_get_name(g_rpc_notify_thread));
	spdk_notify_get(notify);
	spdk_thread_send_msg(g_rpc_notify_thread, rpc_notify_msg_cb, notify);
}

/*
 * Return existing or create new lister for connection of given request.
 * Called only only on RPC thread.
 */
static struct rpc_notify_listener *
rpc_notify_listener_get(struct spdk_jsonrpc_server_conn *conn)
{
	struct rpc_notify_listener *listener;

	TAILQ_FOREACH(listener, &g_rpc_notify_listeners, link) {
		if (listener->conn == conn) {
			assert(spdk_get_thread() == g_rpc_notify_thread);
			return listener;
		}
	}

	/* Listener not found - create new one for this connection. */
	listener = calloc(1, sizeof(*listener));
	if (listener == NULL) {
		SPDK_ERRLOG("calloc() failed\n");
		return NULL;
	}

	listener->conn = conn;
	TAILQ_INIT(&listener->requests);

	/* Start poller if this is first listener. */
	if (TAILQ_EMPTY(&g_rpc_notify_listeners)) {
		g_rpc_notify_poller = spdk_poller_register(rpc_notify_poller_cb, NULL, SPDK_NOTIFY_RPC_TIMEDOUT_REQ_POLL_MS);
	}

	if (g_rpc_notify_thread == NULL) {
		g_rpc_notify_thread = spdk_get_thread();
	}

	TAILQ_INSERT_TAIL(&g_rpc_notify_listeners, listener, link);

	spdk_jsonrpc_conn_add_close_cb(conn, rpc_notify_conn_closed, listener);
	spdk_notify_listen(rpc_notify_new_notification_cb, NULL);

	SPDK_INFOLOG(SPDK_NOTIFY_RPC, "New notification listener: %p (conn: %p)\n", listener, listener->conn);
	return listener;
}

static void
rpc_notify_listener_put(struct rpc_notify_listener *listener)
{
	SPDK_INFOLOG(SPDK_NOTIFY_RPC, "Listener %p (conn: %p): deleting\n", listener, listener->conn);

	spdk_notify_unlisten(rpc_notify_new_notification_cb, listener);
	spdk_jsonrpc_conn_del_close_cb(listener->conn, rpc_notify_conn_closed, listener);

	TAILQ_REMOVE(&g_rpc_notify_listeners, listener, link);
	if (TAILQ_EMPTY(&g_rpc_notify_listeners)) {
		spdk_poller_unregister(&g_rpc_notify_poller);
	}

	SPDK_INFOLOG(SPDK_NOTIFY_RPC, "Listener %p (conn: %p): finishing pending requests\n", listener, listener->conn);
	while (!TAILQ_EMPTY(&listener->requests)) {
		/* TODO: */
		SPDK_ERRLOG("Implement me\n");
		assert(false);
	}

	SPDK_INFOLOG(SPDK_NOTIFY_RPC, "Listener %p (conn: %p): listener removed\n", listener, listener->conn);
	free(listener);
}

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
		spdk_notify_type_write_json(w, ntype);
		ntype = spdk_notify_type_next(ntype);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_notification_types", spdk_rpc_get_notification_types,
		  SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)

static const struct spdk_json_object_decoder rpc_get_notifications_decoders[] = {
	{"timeout_ms", offsetof(struct rpc_notify_request, timeout), spdk_json_decode_uint64, true},
};

static void
spdk_rpc_get_notifications(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_notify_request *it, *req = NULL;
	struct rpc_notify_listener *listener;
	int rc;

	req = calloc(1, sizeof(*req));
	req->timeout = SPDK_NOTIFY_RPC_DEFAULT_TIMEOUT_MS;

	if (params &&
	    spdk_json_decode_object(params, rpc_get_notifications_decoders,
				    SPDK_COUNTOF(rpc_get_notifications_decoders), req)) {
		/* XXX: remove error log */
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc =  -EINVAL;
		goto invalid;
	}

	listener = rpc_notify_listener_get(spdk_jsonrpc_get_conn(request));
	if (listener == NULL) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->request = request;
	/* Convert ms to ticks */
	req->timeout = spdk_get_ticks() + spdk_get_ticks_hz() * req->timeout / 1000UL;

	SPDK_DEBUGLOG(SPDK_NOTIFY_RPC, "Listener %p (conn: %p): new notification request %p.\n", listener, listener->conn, req);

	/* Insert at proper timeout position. */
	TAILQ_FOREACH(it, &listener->requests, link) {
		if (req->timeout < it->timeout) {
			TAILQ_INSERT_BEFORE(it, req, link);
			return;
		}
	}

	/* Queue is empty or timeout > the last request timeout so insert tail. */
	TAILQ_INSERT_TAIL(&listener->requests, req, link);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
	free(req);
}
SPDK_RPC_REGISTER("get_notifications", spdk_rpc_get_notifications, SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)

SPDK_LOG_REGISTER_COMPONENT("notify_rpc", SPDK_NOTIFY_RPC)
