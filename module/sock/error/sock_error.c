/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Nutanix Inc. All rights reserved.
 */

/*
 * This is a test module that injects errors into socket operations.
 */

#include "sock_error.h"

#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk/log.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/sock_module.h"

struct sock_error_inject_state {
	struct sock_error_inject_opts opts;
	/* Number of errors already injected */
	uint64_t count;
	/* Number of operations executed since last error injection */
	uint64_t interval;
};

static pthread_mutex_t g_inject_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct sock_error_inject_state g_inject[SPDK_SOCK_ERROR_OPERATION_MAX];

static struct spdk_net_impl *g_base_impl;
static struct spdk_net_impl g_error_net_impl;

int
sock_error_inject_error(struct sock_error_inject_opts *opts)
{
	struct sock_error_inject_state *state;

	const bool is_io_op =
		opts->operation == SPDK_SOCK_ERROR_OPERATION_READV ||
		opts->operation == SPDK_SOCK_ERROR_OPERATION_RECV ||
		opts->operation == SPDK_SOCK_ERROR_OPERATION_WRITEV ||
		opts->operation == SPDK_SOCK_ERROR_OPERATION_WRITEV_ASYNC;


	if (!g_base_impl) {
		SPDK_ERRLOG("sock_error is not registered, call sock_error_register first\n");
		return -ENODEV;
	}

	/* Treat negative errcode as a caller mistake */
	if (opts->errcode < 0) {
		SPDK_ERRLOG("errcode must be non-negative, got %d\n", opts->errcode);
		return -EINVAL;
	}

	if (opts->type == SPDK_SOCK_ERROR_TYPE_CORRUPT && !is_io_op) {
		SPDK_ERRLOG("corrupt is only supported on byte-moving ops\n");
		return -EINVAL;
	}

	if (opts->match_len != 0 && !is_io_op) {
		SPDK_ERRLOG("length matching is only supported on byte-moving ops\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&g_inject_mutex);

	state = &g_inject[opts->operation];
	state->opts = *opts;
	state->count = 0;
	state->interval = 0;

	/* Use EIO as the default failure errno when none was provided */
	if (state->opts.errcode == 0) {
		state->opts.errcode = EIO;
	}

	pthread_mutex_unlock(&g_inject_mutex);

	return 0;
}

struct sock_error_action {
	enum spdk_sock_error_type type;
	int errcode;
};

/* Begin error injection action, acquires g_inject_mutex and reports the
 * pending action without advancing the state. The caller must pair every
 * sock_error_action_begin() with exactly one sock_error_action_end() on every
 * exit path. The lock is held for the whole window in between.
 *
 * req_len is the number of bytes the caller asked the underlying op to move.
 * When opts.match_len is non-zero the action only fires if req_len is equal to it.
 */
static struct sock_error_action
sock_error_action_begin(enum spdk_sock_error_operation op, size_t req_len)
{
	struct sock_error_action action = { .type = SPDK_SOCK_ERROR_TYPE_DISABLE };
	struct sock_error_inject_state *state = &g_inject[op];

	pthread_mutex_lock(&g_inject_mutex);

	if (state->opts.type == SPDK_SOCK_ERROR_TYPE_DISABLE) {
		return action;
	}

	if (state->opts.match_len != 0 && req_len != state->opts.match_len) {
		return action;
	}

	/* +1 accounts for this uncommitted operation. */
	if (state->interval + 1 >= state->opts.interval &&
	    state->count + 1 <= state->opts.count) {
		action.type = state->opts.type;
		action.errcode = state->opts.errcode;
	}

	return action;
}

/* End an error injection action, releases g_inject_mutex. When committed is true
 * the state machine is advanced (the action took effect), disabling the
 * injection once its budget is spent. Must be called exactly once per
 * sock_error_action_begin() on every exit path.
 */
static void
sock_error_action_end(enum spdk_sock_error_operation op, bool committed)
{
	struct sock_error_inject_state *state = &g_inject[op];

	if (!committed || state->opts.type == SPDK_SOCK_ERROR_TYPE_DISABLE) {
		goto unlock;
	}

	state->interval++;
	if (state->interval >= state->opts.interval) {
		state->interval = 0;
		state->count++;

		if (state->count >= state->opts.count) {
			state->opts.type = SPDK_SOCK_ERROR_TYPE_DISABLE;
			state->count = 0;
		}
	}

unlock:
	pthread_mutex_unlock(&g_inject_mutex);
}

static void
sock_error_corrupt_buf(void *buf, size_t len)
{
	if (len > 0) {
		*((uint8_t *)buf) ^= 0xff;
		return;
	}
}

static void
sock_error_corrupt_iov(struct iovec *iov, int iovcnt, size_t nbytes)
{
	int i;

	for (i = 0; i < iovcnt && nbytes > 0; i++) {
		size_t len = spdk_min(iov[i].iov_len, nbytes);

		sock_error_corrupt_buf(iov[i].iov_base, len);
		nbytes -= len;
	}
}

static void
sock_error_corrupt_req(struct spdk_sock_request *req)
{
	int i;

	for (i = 0; i < req->iovcnt; i++) {
		struct iovec *iov = SPDK_SOCK_REQUEST_IOV(req, i);
		sock_error_corrupt_buf(iov->iov_base, iov->iov_len);
	}
}

static size_t
sock_error_req_len(struct spdk_sock_request *req)
{
	size_t len = 0;
	int i;

	for (i = 0; i < req->iovcnt; i++) {
		len += SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
	}

	return len;
}

static size_t
sock_error_iov_len(const struct iovec *iov, int iovcnt)
{
	size_t len = 0;
	int i;

	for (i = 0; i < iovcnt; i++) {
		len += iov[i].iov_len;
	}

	return len;
}

static ssize_t
sock_error_readv(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	ssize_t rc;
	bool committed = false;
	struct sock_error_action action;

	action = sock_error_action_begin(SPDK_SOCK_ERROR_OPERATION_READV,
					 sock_error_iov_len(iov, iovcnt));

	if (action.type == SPDK_SOCK_ERROR_TYPE_FAILURE) {
		sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_READV, true);
		return -action.errcode;
	}

	rc = g_base_impl->readv(sock, iov, iovcnt);

	if (action.type == SPDK_SOCK_ERROR_TYPE_CORRUPT && rc > 0) {
		sock_error_corrupt_iov(iov, iovcnt, rc);
		committed = true;
	}

	sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_READV, committed);

	return rc;
}

static ssize_t
sock_error_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	ssize_t rc;
	bool committed = false;
	struct sock_error_action action;

	action = sock_error_action_begin(SPDK_SOCK_ERROR_OPERATION_RECV, len);

	if (action.type == SPDK_SOCK_ERROR_TYPE_FAILURE) {
		sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_RECV, true);
		return -action.errcode;
	}

	rc = g_base_impl->recv(sock, buf, len);

	if (action.type == SPDK_SOCK_ERROR_TYPE_CORRUPT && rc > 0) {
		sock_error_corrupt_buf(buf, rc);
		committed = true;
	}

	sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_RECV, committed);

	return rc;
}

static ssize_t
sock_error_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	struct sock_error_action action;
	bool committed = false;
	ssize_t rc;

	action = sock_error_action_begin(SPDK_SOCK_ERROR_OPERATION_WRITEV,
					 sock_error_iov_len(iov, iovcnt));

	if (action.type == SPDK_SOCK_ERROR_TYPE_FAILURE) {
		sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_WRITEV, true);
		return -action.errcode;
	}

	if (action.type == SPDK_SOCK_ERROR_TYPE_CORRUPT) {
		sock_error_corrupt_iov(iov, iovcnt, SIZE_MAX);
		committed = true;
	}

	/*
	 * End the section (commit + drop the lock) before the base call. writev
	 * can flush queued async requests, whose completion callbacks may
	 * re-enter a wrapped sock op on this thread and deadlock on this
	 * non-recursive mutex.
	 */
	sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_WRITEV, committed);
	rc = g_base_impl->writev(sock, iov, iovcnt);

	return rc;
}

static void
sock_error_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	struct sock_error_action action;
	bool committed = false;

	action = sock_error_action_begin(SPDK_SOCK_ERROR_OPERATION_WRITEV_ASYNC,
					 sock_error_req_len(req));

	if (action.type == SPDK_SOCK_ERROR_TYPE_FAILURE) {
		sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_WRITEV_ASYNC, true);

		/* Invokes callbacks, need the mutex to be unlocked here as well */
		spdk_sock_request_queue(sock, req);
		spdk_sock_abort_requests(sock);
		return;
	}

	if (action.type == SPDK_SOCK_ERROR_TYPE_CORRUPT) {
		sock_error_corrupt_req(req);
		committed = true;
	}

	/* Same as writev, end the section (commit + unlock) before the base call */
	sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_WRITEV_ASYNC, committed);
	g_base_impl->writev_async(sock, req);
}

static int
sock_error_flush(struct spdk_sock *sock)
{
	struct sock_error_action action;
	ssize_t rc;

	action = sock_error_action_begin(SPDK_SOCK_ERROR_OPERATION_FLUSH, 0);

	if (action.type == SPDK_SOCK_ERROR_TYPE_FAILURE) {
		sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_FLUSH, true);
		return -action.errcode;
	}

	/* Same as writev, end the section (unlock) before the base call */
	sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_FLUSH, false);
	rc = g_base_impl->flush(sock);

	return rc;
}

struct sock_error_connect_ctx {
	spdk_sock_connect_cb_fn cb_fn;
	void *cb_arg;
	int errcode;
};

static void
sock_error_connect_cb(void *cb_arg, int status)
{
	struct sock_error_connect_ctx *ctx = cb_arg;

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, -ctx->errcode);
	}

	free(ctx);
}

static struct spdk_sock *
sock_error_connect(const char *ip, int port, struct spdk_sock_opts *opts,
		   spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct sock_error_action action;
	struct sock_error_connect_ctx *ctx;
	struct spdk_sock *sock;

	action = sock_error_action_begin(SPDK_SOCK_ERROR_OPERATION_CONNECT, 0);
	sock_error_action_end(SPDK_SOCK_ERROR_OPERATION_CONNECT,
			      action.type == SPDK_SOCK_ERROR_TYPE_FAILURE);

	if (action.type != SPDK_SOCK_ERROR_TYPE_FAILURE) {
		return g_base_impl->connect(ip, port, opts, cb_fn, cb_arg);
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return NULL;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->errcode = action.errcode;

	sock = g_base_impl->connect(ip, port, opts, sock_error_connect_cb, ctx);
	if (!sock) {
		free(ctx);
	}

	return sock;
}

int
sock_error_register_impl(const char *base_impl_name)
{
	static bool registered = false;
	struct spdk_net_impl *base;

	if (registered) {
		return 0;
	}

	base = spdk_net_impl_get_by_name(base_impl_name);
	if (!base) {
		return -ENOTSUP;
	}
	g_base_impl = base;

	/* Init the net_impl here */
	g_error_net_impl = *base;
	g_error_net_impl.name = "sock_error";

	/* Override the callbacks we implement */
	g_error_net_impl.connect        = sock_error_connect;
	g_error_net_impl.recv           = sock_error_recv;
	g_error_net_impl.readv          = sock_error_readv;
	g_error_net_impl.writev         = sock_error_writev;
	g_error_net_impl.writev_async   = sock_error_writev_async;
	g_error_net_impl.flush          = sock_error_flush;

	spdk_net_impl_register(&g_error_net_impl);
	registered = true;

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(sock_error)
