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

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__FreeBSD__)
#include <sys/event.h>
#endif

#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk_internal/sock.h"
#include "spdk/queue.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

#define MAX_EVENTS_PER_POLL 32

static STAILQ_HEAD(, spdk_net_impl) g_net_impls = STAILQ_HEAD_INITIALIZER(g_net_impls);

struct spdk_sock {
	struct spdk_net_impl	*net_impl;
	spdk_sock_cb		cb_fn;
	void			*cb_arg;
	TAILQ_ENTRY(spdk_sock)	link;
};

struct spdk_posix_sock {
	struct spdk_sock	base;
	int			fd;
};

struct spdk_sock_group {
	STAILQ_HEAD(, spdk_sock_group_impl)	group_impls;
};

struct spdk_sock_group_impl {
	struct spdk_net_impl			*net_impl;
	TAILQ_HEAD(, spdk_sock)			socks;
	STAILQ_ENTRY(spdk_sock_group_impl)	link;
};

struct spdk_posix_sock_group_impl {
	struct spdk_sock_group_impl	base;
	int				fd;
};

static int get_addr_str(struct sockaddr *sa, char *host, size_t hlen)
{
	const char *result = NULL;

	if (sa == NULL || host == NULL) {
		return -1;
	}

	switch (sa->sa_family) {
	case AF_INET:
		result = inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				   host, hlen);
		break;
	case AF_INET6:
		result = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				   host, hlen);
		break;
	default:
		break;
	}

	if (result != NULL) {
		return 0;
	} else {
		return -1;
	}
}

#define __posix_sock(sock) (struct spdk_posix_sock *)sock
#define __posix_group_impl(group) (struct spdk_posix_sock_group_impl *)group

static int
spdk_posix_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, char *caddr, int clen)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return -1;
	}

	switch (sa.ss_family) {
	case AF_UNIX:
		/* Acceptable connection types that don't have IPs */
		return 0;
	case AF_INET:
	case AF_INET6:
		/* Code below will get IP addresses */
		break;
	default:
		/* Unsupported socket family */
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, saddr, slen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getpeername(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, caddr, clen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	return 0;
}

enum spdk_posix_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static struct spdk_sock *
spdk_posix_sock_create(const char *ip, int port, enum spdk_posix_sock_create_type type)
{
	struct spdk_posix_sock *sock;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int fd, flag;
	int val = 1;
	int rc;

	if (ip == NULL) {
		return NULL;
	}
	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		ip = (const char *) &buf[0];
	}

	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed (errno=%d)\n", errno);
		return NULL;
	}

	/* try listen */
	fd = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
retry:
		fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd < 0) {
			/* error */
			continue;
		}
		rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			close(fd);
			/* error */
			continue;
		}
		rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
		if (rc != 0) {
			close(fd);
			/* error */
			continue;
		}

		if (type == SPDK_SOCK_CREATE_LISTEN) {
			rc = bind(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("bind() failed, errno = %d\n", errno);
				switch (errno) {
				case EINTR:
					/* interrupted? */
					close(fd);
					goto retry;
				case EADDRNOTAVAIL:
					SPDK_ERRLOG("IP address %s not available. "
						    "Verify IP address in config file "
						    "and make sure setup script is "
						    "run before starting spdk app.\n", ip);
				/* FALLTHROUGH */
				default:
					/* try next family */
					close(fd);
					fd = -1;
					continue;
				}
			}
			/* bind OK */
			rc = listen(fd, 512);
			if (rc != 0) {
				SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
				close(fd);
				fd = -1;
				break;
			}
		} else if (type == SPDK_SOCK_CREATE_CONNECT) {
			rc = connect(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
				/* try next family */
				close(fd);
				fd = -1;
				continue;
			}
		}

		flag = fcntl(fd, F_GETFL);
		if (fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0) {
			SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
			close(fd);
			fd = -1;
			break;
		}
		break;
	}
	freeaddrinfo(res0);

	if (fd < 0) {
		return NULL;
	}

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(fd);
		return NULL;
	}

	sock->fd = fd;
	return &sock->base;
}

static struct spdk_sock *
spdk_posix_sock_listen(const char *ip, int port)
{
	return spdk_posix_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN);
}

static struct spdk_sock *
spdk_posix_sock_connect(const char *ip, int port)
{
	return spdk_posix_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT);
}

static struct spdk_sock *
spdk_posix_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_posix_sock		*sock = __posix_sock(_sock);
	struct sockaddr_storage		sa;
	socklen_t			salen;
	int				rc;
	struct spdk_posix_sock		*new_sock;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);

	assert(sock != NULL);

	rc = accept(sock->fd, (struct sockaddr *)&sa, &salen);

	if (rc == -1) {
		return NULL;
	}

	new_sock = calloc(1, sizeof(*sock));
	if (new_sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(rc);
		return NULL;
	}

	new_sock->fd = rc;
	return &new_sock->base;
}

static int
spdk_posix_sock_close(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	return close(sock->fd);
}

static ssize_t
spdk_posix_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	return recv(sock->fd, buf, len, MSG_DONTWAIT);
}

static ssize_t
spdk_posix_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	return writev(sock->fd, iov, iovcnt);
}

static int
spdk_posix_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int val;
	int rc;

	assert(sock != NULL);

	val = nbytes;
	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
	if (rc != 0) {
		return -1;
	}
	return 0;
}

static int
spdk_posix_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	assert(sock != NULL);

	return setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF,
			  &sz, sizeof(sz));
}

static int
spdk_posix_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	assert(sock != NULL);

	return setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF,
			  &sz, sizeof(sz));
}

static bool
spdk_posix_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);
}

static bool
spdk_posix_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}

static struct spdk_sock_group_impl *
spdk_posix_sock_group_impl_create(void)
{
	struct spdk_posix_sock_group_impl *group_impl;
	int fd;

#if defined(__linux__)
	fd = epoll_create1(0);
#elif defined(__FreeBSD__)
	fd = kqueue();
#endif
	if (fd == -1) {
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		close(fd);
		return NULL;
	}

	group_impl->fd = fd;

	return &group_impl->base;
}

static int
spdk_posix_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

#if defined(__linux__)
	struct epoll_event event;

	event.events = EPOLLIN;
	event.data.ptr = sock;

	rc = epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);
#elif defined(__FreeBSD__)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_ADD, 0, 0, sock);

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
#endif
	return rc;
}

static int
spdk_posix_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;
#if defined(__linux__)
	struct epoll_event event;

	/* Event parameter is ignored but some old kernel version still require it. */
	rc = epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event);
#elif defined(__FreeBSD__)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
	if (rc == 0 && event.flags & EV_ERROR) {
		rc = -1;
		errno = event.data;
	}
#endif
	return rc;
}

static int
spdk_posix_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
				struct spdk_sock **socks)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	int num_events, i;

#if defined(__linux__)
	struct epoll_event events[MAX_EVENTS_PER_POLL];

	num_events = epoll_wait(group->fd, events, max_events, 0);
#elif defined(__FreeBSD__)
	struct kevent events[MAX_EVENTS_PER_POLL];
	struct timespec ts = {0};

	num_events = kevent(group->fd, NULL, 0, events, max_events, &ts);
#endif

	if (num_events == -1) {
		return -1;
	}

	for (i = 0; i < num_events; i++) {
#if defined(__linux__)
		socks[i] = events[i].data.ptr;
#elif defined(__FreeBSD__)
		socks[i] = events[i].udata;
#endif
	}

	return num_events;
}

static int
spdk_posix_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);

	return close(group->fd);
}

static struct spdk_net_impl g_posix_net_impl = {
	.name		= "posix",
	.getaddr	= spdk_posix_sock_getaddr,
	.connect	= spdk_posix_sock_connect,
	.listen		= spdk_posix_sock_listen,
	.accept		= spdk_posix_sock_accept,
	.close		= spdk_posix_sock_close,
	.recv		= spdk_posix_sock_recv,
	.writev		= spdk_posix_sock_writev,
	.set_recvlowat	= spdk_posix_sock_set_recvlowat,
	.set_recvbuf	= spdk_posix_sock_set_recvbuf,
	.set_sendbuf	= spdk_posix_sock_set_sendbuf,
	.is_ipv6	= spdk_posix_sock_is_ipv6,
	.is_ipv4	= spdk_posix_sock_is_ipv4,
	.group_impl_create	= spdk_posix_sock_group_impl_create,
	.group_impl_add_sock	= spdk_posix_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_posix_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_posix_sock_group_impl_poll,
	.group_impl_close	= spdk_posix_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(posix, &g_posix_net_impl);

int
spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, char *caddr, int clen)
{
	return sock->net_impl->getaddr(sock, saddr, slen, caddr, clen);
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
		free(*sock);
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
		assert(group_impl != NULL);
		STAILQ_INSERT_TAIL(&group->group_impls, group_impl, link);
		TAILQ_INIT(&group_impl->socks);
		group_impl->net_impl = impl;
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
