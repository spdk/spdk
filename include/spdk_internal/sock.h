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

/** \file
 * TCP network implementation abstraction layer
 */

#ifndef SPDK_INTERNAL_SOCK_H
#define SPDK_INTERNAL_SOCK_H

#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_EVENTS_PER_POLL 32
#define DEFAULT_SOCK_PRIORITY 0

struct spdk_sock {
	struct spdk_net_impl		*net_impl;
	int				cb_cnt;
	spdk_sock_cb			cb_fn;
	void				*cb_arg;
	struct spdk_sock_group_impl	*group_impl;
	TAILQ_ENTRY(spdk_sock)		link;

	int				max_iovcnt;
	TAILQ_HEAD(, spdk_sock_request)	queued_reqs;
	TAILQ_HEAD(, spdk_sock_request)	pending_reqs;
	int				queued_iovcnt;

	struct {
		uint8_t		closed		: 1;
		uint8_t		reserved	: 7;
	} flags;
};

struct spdk_sock_group {
	STAILQ_HEAD(, spdk_sock_group_impl)	group_impls;
	void					*ctx;
};

struct spdk_sock_group_impl {
	struct spdk_net_impl			*net_impl;
	TAILQ_HEAD(, spdk_sock)			socks;
	STAILQ_ENTRY(spdk_sock_group_impl)	link;
};

struct spdk_net_impl {
	const char *name;
	int priority;

	int (*getaddr)(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport, char *caddr,
		       int clen, uint16_t *cport);
	struct spdk_sock *(*connect)(const char *ip, int port);
	struct spdk_sock *(*listen)(const char *ip, int port);
	struct spdk_sock *(*accept)(struct spdk_sock *sock);
	int (*close)(struct spdk_sock *sock);
	ssize_t (*recv)(struct spdk_sock *sock, void *buf, size_t len);
	ssize_t (*readv)(struct spdk_sock *sock, struct iovec *iov, int iovcnt);
	ssize_t (*writev)(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

	void (*writev_async)(struct spdk_sock *sock, struct spdk_sock_request *req);

	int (*set_recvlowat)(struct spdk_sock *sock, int nbytes);
	int (*set_recvbuf)(struct spdk_sock *sock, int sz);
	int (*set_sendbuf)(struct spdk_sock *sock, int sz);
	int (*set_priority)(struct spdk_sock *sock, int priority);

	bool (*is_ipv6)(struct spdk_sock *sock);
	bool (*is_ipv4)(struct spdk_sock *sock);
	bool (*is_connected)(struct spdk_sock *sock);

	int (*get_placement_id)(struct spdk_sock *sock, int *placement_id);
	struct spdk_sock_group_impl *(*group_impl_create)(void);
	int (*group_impl_add_sock)(struct spdk_sock_group_impl *group, struct spdk_sock *sock);
	int (*group_impl_remove_sock)(struct spdk_sock_group_impl *group, struct spdk_sock *sock);
	int (*group_impl_poll)(struct spdk_sock_group_impl *group, int max_events,
			       struct spdk_sock **socks);
	int (*group_impl_close)(struct spdk_sock_group_impl *group);

	STAILQ_ENTRY(spdk_net_impl) link;
};

void spdk_net_impl_register(struct spdk_net_impl *impl, int priority);

#define SPDK_NET_IMPL_REGISTER(name, impl, priority) \
static void __attribute__((constructor)) net_impl_register_##name(void) \
{ \
	spdk_net_impl_register(impl, priority); \
}

static inline void
spdk_sock_request_queue(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	TAILQ_INSERT_TAIL(&sock->queued_reqs, req, internal.link);
	sock->queued_iovcnt += req->iovcnt;
}

static inline void
spdk_sock_request_pend(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	TAILQ_REMOVE(&sock->queued_reqs, req, internal.link);
	assert(sock->queued_iovcnt >= req->iovcnt);
	sock->queued_iovcnt -= req->iovcnt;
	TAILQ_INSERT_TAIL(&sock->pending_reqs, req, internal.link);
}

static inline int
spdk_sock_request_put(struct spdk_sock *sock, struct spdk_sock_request *req, int err)
{
	bool closed;
	int rc = 0;

	TAILQ_REMOVE(&sock->pending_reqs, req, internal.link);

	closed = sock->flags.closed;
	sock->cb_cnt++;
	req->cb_fn(req->cb_arg, err);
	assert(sock->cb_cnt > 0);
	sock->cb_cnt--;

	if (sock->cb_cnt == 0 && !closed && sock->flags.closed) {
		/* The user closed the socket in response to a callback above. */
		rc = -1;
		spdk_sock_close(&sock);
	}

	return rc;
}

static inline int
spdk_sock_abort_requests(struct spdk_sock *sock)
{
	struct spdk_sock_request *req;
	bool closed;
	int rc = 0;

	closed = sock->flags.closed;
	sock->cb_cnt++;

	req = TAILQ_FIRST(&sock->pending_reqs);
	while (req) {
		TAILQ_REMOVE(&sock->pending_reqs, req, internal.link);

		req->cb_fn(req->cb_arg, -ECANCELED);

		req = TAILQ_FIRST(&sock->pending_reqs);
	}

	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		TAILQ_REMOVE(&sock->queued_reqs, req, internal.link);

		assert(sock->queued_iovcnt >= req->iovcnt);
		sock->queued_iovcnt -= req->iovcnt;

		req->cb_fn(req->cb_arg, -ECANCELED);

		req = TAILQ_FIRST(&sock->queued_reqs);
	}
	assert(sock->cb_cnt > 0);
	sock->cb_cnt--;

	assert(TAILQ_EMPTY(&sock->queued_reqs));
	assert(TAILQ_EMPTY(&sock->pending_reqs));

	if (sock->cb_cnt == 0 && !closed && sock->flags.closed) {
		/* The user closed the socket in response to a callback above. */
		rc = -1;
		spdk_sock_close(&sock);
	}

	return rc;
}

#ifdef __cplusplus
}
#endif

#endif /* SPDK_INTERNAL_SOCK_H */
