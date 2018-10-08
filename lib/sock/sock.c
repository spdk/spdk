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

#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk_internal/sock.h"
#include "spdk/queue.h"

static STAILQ_HEAD(, spdk_net_impl) g_net_impls = STAILQ_HEAD_INITIALIZER(g_net_impls);

int
spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
		  char *caddr, int clen, uint16_t *cport)
{
	return sock->net_impl->getaddr(sock, saddr, slen, sport, caddr, clen, cport);
}

struct spdk_sock *
spdk_sock_connect(const char *ip, int port)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock *sock;

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		sock = impl->connect(ip, port);
		if (sock != NULL) {
			sock->net_impl = impl;
			return sock;
		}
	}

	return NULL;
}

struct spdk_sock *
spdk_sock_listen(const char *ip, int port)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock *sock;

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		sock = impl->listen(ip, port);
		if (sock != NULL) {
			sock->net_impl = impl;
			return sock;
		}
	}

	return NULL;
}

struct spdk_sock *
spdk_sock_accept(struct spdk_sock *sock)
{
	struct spdk_sock *new_sock;

	new_sock = sock->net_impl->accept(sock);
	if (new_sock != NULL) {
		new_sock->net_impl = sock->net_impl;
	}

	return new_sock;
}

int
spdk_sock_close(struct spdk_sock **sock)
{
	int rc;

	if (*sock == NULL) {
		errno = EBADF;
		return -1;
	}

	if ((*sock)->cb_fn != NULL) {
		/* This sock is still part of a sock_group. */
		errno = EBUSY;
		return -1;
	}

	rc = (*sock)->net_impl->close(*sock);
	if (rc == 0) {
		*sock = NULL;
	}

	return rc;
}

ssize_t
spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->recv(sock, buf, len);
}

ssize_t
spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->writev(sock, iov, iovcnt);
}


int
spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes)
{
	return sock->net_impl->set_recvlowat(sock, nbytes);
}

int
spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_recvbuf(sock, sz);
}

int
spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_sendbuf(sock, sz);
}

bool
spdk_sock_is_ipv6(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv6(sock);
}

bool
spdk_sock_is_ipv4(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv4(sock);
}

struct spdk_sock_group *
spdk_sock_group_create(void)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock_group *group;
	struct spdk_sock_group_impl *group_impl;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	STAILQ_INIT(&group->group_impls);

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		group_impl = impl->group_impl_create();
		if (group_impl != NULL) {
			STAILQ_INSERT_TAIL(&group->group_impls, group_impl, link);
			TAILQ_INIT(&group_impl->socks);
			group_impl->net_impl = impl;
		}
	}

	return group;
}

int
spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			 spdk_sock_cb cb_fn, void *cb_arg)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc;

	if (cb_fn == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (sock->cb_fn != NULL) {
		/*
		 * This sock is already part of a sock_group.  Currently we don't
		 *  support this.
		 */
		errno = EBUSY;
		return -1;
	}

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		if (sock->net_impl == group_impl->net_impl) {
			break;
		}
	}

	if (group_impl == NULL) {
		errno = EINVAL;
		return -1;
	}

	rc = group_impl->net_impl->group_impl_add_sock(group_impl, sock);
	if (rc == 0) {
		TAILQ_INSERT_TAIL(&group_impl->socks, sock, link);
		sock->cb_fn = cb_fn;
		sock->cb_arg = cb_arg;
	}

	return rc;
}

int
spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc;

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		if (sock->net_impl == group_impl->net_impl) {
			break;
		}
	}

	if (group_impl == NULL) {
		errno = EINVAL;
		return -1;
	}

	rc = group_impl->net_impl->group_impl_remove_sock(group_impl, sock);
	if (rc == 0) {
		TAILQ_REMOVE(&group_impl->socks, sock, link);
		sock->cb_fn = NULL;
		sock->cb_arg = NULL;
	}

	return rc;
}

int
spdk_sock_group_poll(struct spdk_sock_group *group)
{
	return spdk_sock_group_poll_count(group, MAX_EVENTS_PER_POLL);
}

static int
spdk_sock_group_impl_poll_count(struct spdk_sock_group_impl *group_impl,
				struct spdk_sock_group *group,
				int max_events)
{
	struct spdk_sock *socks[MAX_EVENTS_PER_POLL];
	int num_events, i;

	if (TAILQ_EMPTY(&group_impl->socks)) {
		return 0;
	}

	num_events = group_impl->net_impl->group_impl_poll(group_impl, max_events, socks);
	if (num_events == -1) {
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		struct spdk_sock *sock = socks[i];

		assert(sock->cb_fn != NULL);
		sock->cb_fn(sock->cb_arg, group, sock);
	}
	return 0;
}

int
spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc, final_rc = 0;

	if (max_events < 1) {
		errno = -EINVAL;
		return -1;
	}

	/*
	 * Only poll for up to 32 events at a time - if more events are pending,
	 *  the next call to this function will reap them.
	 */
	if (max_events > MAX_EVENTS_PER_POLL) {
		max_events = MAX_EVENTS_PER_POLL;
	}

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		rc = spdk_sock_group_impl_poll_count(group_impl, group, max_events);
		if (rc != 0) {
			final_rc = rc;
			SPDK_ERRLOG("group_impl_poll_count for net(%s) failed\n",
				    group_impl->net_impl->name);
		}
	}

	return final_rc;
}

int
spdk_sock_group_close(struct spdk_sock_group **group)
{
	struct spdk_sock_group_impl *group_impl = NULL, *tmp;
	int rc;

	if (*group == NULL) {
		errno = EBADF;
		return -1;
	}

	STAILQ_FOREACH_SAFE(group_impl, &(*group)->group_impls, link, tmp) {
		if (!TAILQ_EMPTY(&group_impl->socks)) {
			errno = EBUSY;
			return -1;
		}
	}

	STAILQ_FOREACH_SAFE(group_impl, &(*group)->group_impls, link, tmp) {
		rc = group_impl->net_impl->group_impl_close(group_impl);
		if (rc != 0) {
			SPDK_ERRLOG("group_impl_close for net(%s) failed\n",
				    group_impl->net_impl->name);
		}
		free(group_impl);
	}

	free(*group);
	*group = NULL;

	return 0;
}

void
spdk_net_impl_register(struct spdk_net_impl *impl)
{
	if (!strcmp("posix", impl->name)) {
		STAILQ_INSERT_TAIL(&g_net_impls, impl, link);
	} else {
		STAILQ_INSERT_HEAD(&g_net_impls, impl, link);
	}
}
