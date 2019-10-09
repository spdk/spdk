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
#include <linux/errqueue.h>
#elif defined(__FreeBSD__)
#include <sys/event.h>
#endif

#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk_internal/sock.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define SO_RCVBUF_SIZE (2 * 1024 * 1024)
#define SO_SNDBUF_SIZE (2 * 1024 * 1024)
#define DEFAULT_NUM_SOCK_REQS 256
#define DEFAULT_MAX_IOV 64

struct spdk_posix_sock_request {
	spdk_sock_op_cb				cb_fn;
	void					*cb_arg;

	TAILQ_ENTRY(spdk_posix_sock_request)	link;

	unsigned int				offset;

	int					iovcnt;
	struct iovec				iov[];
};

#define SENDMSG_RING_SIZE 64

struct spdk_posix_sock {
	struct spdk_sock	base;
	int			fd;

	int					max_iovcnt;
	uint32_t				num_reqs;
	struct spdk_posix_sock_request		*req_mem;
	TAILQ_HEAD(, spdk_posix_sock_request)	free_reqs;
	TAILQ_HEAD(, spdk_posix_sock_request)	queued_reqs;
	TAILQ_HEAD(, spdk_posix_sock_request)	pending_reqs;
	int					queued_iovcnt;

	uint32_t				sendmsg_head;
	uint32_t				sendmsg_tail;
	ssize_t					sendmsg_calls[SENDMSG_RING_SIZE];
	bool					zcopy;
};

struct spdk_posix_sock_group_impl {
	struct spdk_sock_group_impl	base;
	int				fd;
};

static int
get_addr_str(struct sockaddr *sa, char *host, size_t hlen)
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

static inline struct spdk_posix_sock_request *
spdk_posix_sock_request_get(struct spdk_posix_sock *sock)
{
	struct spdk_posix_sock_request *req;

	req = TAILQ_FIRST(&sock->free_reqs);

	if (req == NULL) {
		return NULL;
	}

	req->offset = 0;
	req->iovcnt = 0;

	TAILQ_REMOVE(&sock->free_reqs, req, link);

	return req;
}

static inline void
spdk_posix_sock_request_queue(struct spdk_posix_sock *sock, struct spdk_posix_sock_request *req)
{
	TAILQ_INSERT_TAIL(&sock->queued_reqs, req, link);
	sock->queued_iovcnt += req->iovcnt;
}

static inline void
spdk_posix_sock_request_pend(struct spdk_posix_sock *sock, struct spdk_posix_sock_request *req)
{
	TAILQ_REMOVE(&sock->queued_reqs, req, link);
	assert(sock->queued_iovcnt >= req->iovcnt);
	sock->queued_iovcnt -= req->iovcnt;
	TAILQ_INSERT_TAIL(&sock->pending_reqs, req, link);
}

static inline void
spdk_posix_sock_request_put(struct spdk_posix_sock *sock,
			    struct spdk_posix_sock_request *req)
{
	TAILQ_REMOVE(&sock->pending_reqs, req, link);
	TAILQ_INSERT_HEAD(&sock->free_reqs, req, link);
}

static int
spdk_posix_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
			char *caddr, int clen, uint16_t *cport)
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

	if (sport) {
		if (sa.ss_family == AF_INET) {
			*sport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*sport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
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

	if (cport) {
		if (sa.ss_family == AF_INET) {
			*cport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*cport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	return 0;
}

enum spdk_posix_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
spdk_posix_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	assert(sock != NULL);

	if (sz < SO_RCVBUF_SIZE) {
		sz = SO_RCVBUF_SIZE;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static int
spdk_posix_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	assert(sock != NULL);

	if (sz < SO_SNDBUF_SIZE) {
		sz = SO_SNDBUF_SIZE;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

#define SOCK_REQUEST_SIZE(max) (sizeof(struct spdk_posix_sock_request) + (max * sizeof(struct iovec)))

static int
_posix_sock_alloc_requests(struct spdk_posix_sock *sock, uint32_t num_reqs, int iovcnt)
{
	struct spdk_posix_sock_request *req;
	uint32_t i;
	uint8_t *buf;

	if (iovcnt == sock->max_iovcnt && num_reqs == sock->num_reqs) {
		return 0;
	}

	if (!TAILQ_EMPTY(&sock->queued_reqs)) {
		return -EAGAIN;
	}

	if (!TAILQ_EMPTY(&sock->pending_reqs)) {
		return -EAGAIN;
	}

	buf = realloc(sock->req_mem, num_reqs * SOCK_REQUEST_SIZE(iovcnt));
	if (buf == NULL) {
		return -ENOMEM;
	}
	memset(buf, 0, num_reqs * SOCK_REQUEST_SIZE(iovcnt));

	sock->req_mem = (struct spdk_posix_sock_request *)buf;
	sock->max_iovcnt = iovcnt;
	sock->num_reqs = num_reqs;
	TAILQ_INIT(&sock->free_reqs);
	TAILQ_INIT(&sock->queued_reqs);
	TAILQ_INIT(&sock->pending_reqs);

	for (i = 0; i < sock->num_reqs; i++) {
		req = (struct spdk_posix_sock_request *)buf;
		TAILQ_INSERT_TAIL(&sock->free_reqs, req, link);
		buf += SOCK_REQUEST_SIZE(sock->max_iovcnt);
	}

	return 0;
}

static int
spdk_posix_sock_set_max_iovcnt(struct spdk_sock *_sock, int *num)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	assert(num != NULL);

	return _posix_sock_alloc_requests(sock, sock->num_reqs, *num);
}

static int
spdk_posix_sock_set_max_async_ops(struct spdk_sock *_sock, int *num)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	assert(num != NULL);

	return _posix_sock_alloc_requests(sock, *num, sock->max_iovcnt);
}

static struct spdk_posix_sock *
_spdk_posix_sock_alloc(int fd, int num_reqs, int max_iovcnt)
{
	struct spdk_posix_sock *sock;
	int rc, flag;

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;

	rc = _posix_sock_alloc_requests(sock, num_reqs, max_iovcnt);
	if (rc) {
		close(fd);
		free(sock);
		return NULL;
	}

	rc = spdk_posix_sock_set_recvbuf(&sock->base, SO_RCVBUF_SIZE);
	if (rc) {
		/* Not fatal */
	}

	rc = spdk_posix_sock_set_sendbuf(&sock->base, SO_SNDBUF_SIZE);
	if (rc) {
		/* Not fatal */
	}

	/* Try to turn on zero copy sends */
	flag = 1;
	rc = setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
	if (rc == 0) {
		sock->zcopy = true;
	}

	return sock;
}

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

		if (res->ai_family == AF_INET6) {
			rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
			if (rc != 0) {
				close(fd);
				/* error */
				continue;
			}
		}

		if (type == SPDK_SOCK_CREATE_LISTEN) {
			rc = bind(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("bind() failed at port %d, errno = %d\n", port, errno);
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

	sock = _spdk_posix_sock_alloc(fd, DEFAULT_NUM_SOCK_REQS, DEFAULT_MAX_IOV);
	if (sock == NULL) {
		close(fd);
		return NULL;
	}

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
	int				rc, fd;
	struct spdk_posix_sock		*new_sock;
	int				flag;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);

	assert(sock != NULL);

	rc = accept(sock->fd, (struct sockaddr *)&sa, &salen);

	if (rc == -1) {
		return NULL;
	}

	fd = rc;

	flag = fcntl(fd, F_GETFL);
	if ((!(flag & O_NONBLOCK)) && (fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0)) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
		close(fd);
		return NULL;
	}

	new_sock = _spdk_posix_sock_alloc(fd, sock->num_reqs, sock->max_iovcnt);
	if (new_sock == NULL) {
		close(fd);
		return NULL;
	}

	return &new_sock->base;
}

static int
spdk_posix_sock_close(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	/* If the socket fails to close, the best choice is to
	 * leak it but continue to free the rest of the sock
	 * memory. */
	close(sock->fd);

	assert(TAILQ_EMPTY(&sock->queued_reqs));
	assert(TAILQ_EMPTY(&sock->pending_reqs));

	free(sock->req_mem);
	free(sock);

	return 0;
}

static ssize_t
spdk_posix_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	return recv(sock->fd, buf, len, MSG_DONTWAIT);
}

static ssize_t
spdk_posix_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	return readv(sock->fd, iov, iovcnt);
}

static ssize_t
spdk_posix_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	return writev(sock->fd, iov, iovcnt);
}

static int spdk_posix_sock_flush(struct spdk_posix_sock *sock);

static void
spdk_posix_sock_writev_async(struct spdk_sock *_sock, struct iovec *iov, int iovcnt,
			     spdk_sock_op_cb cb_fn, void *cb_arg)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct spdk_posix_sock_request *req, *tmp;
	int rc;

	assert(cb_fn != NULL);

	if (_sock->group_impl == NULL) {
		cb_fn(cb_arg, -ENOTSUP);
		return;
	}

	if (iovcnt > sock->max_iovcnt) {
		cb_fn(cb_arg, -ENOTSUP);
		return;
	}

	req = spdk_posix_sock_request_get(sock);
	if (req == NULL) {
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	memcpy(req->iov, iov, sizeof(*iov) * iovcnt);
	req->iovcnt = iovcnt;
	spdk_posix_sock_request_queue(sock, req);

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (sock->queued_iovcnt > DEFAULT_MAX_IOV) {
		rc = spdk_posix_sock_flush(sock);
		if (rc) {
			/* TODO: Likely need to check recvmsg once here to figure out what actually sent. */

			TAILQ_FOREACH_SAFE(req, &sock->queued_reqs, link, tmp) {
				spdk_posix_sock_request_pend(sock, req);
			}

			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, link, tmp) {
				cb_fn = req->cb_fn;
				cb_arg = req->cb_arg;
				spdk_posix_sock_request_put(sock, req);
				cb_fn(cb_arg, -rc);
			}

			memset(sock->sendmsg_calls, 0, sizeof(sock->sendmsg_calls));
			sock->sendmsg_head = 0;
			sock->sendmsg_tail = 0;
		}
	}
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
spdk_posix_sock_set_priority(struct spdk_sock *_sock, int priority)
{
	int rc = 0;

#if defined(SO_PRIORITY)
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	assert(sock != NULL);

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_PRIORITY,
			&priority, sizeof(priority));
#endif
	return rc;
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

static bool
spdk_posix_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	uint8_t byte;
	int rc;

	rc = recv(sock->fd, &byte, 1, MSG_PEEK);
	if (rc == 0) {
		return false;
	}

	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return true;
		}

		return false;
	}

	return true;
}

static int
spdk_posix_sock_get_placement_id(struct spdk_sock *_sock, int *placement_id)
{
	int rc = -1;

#if defined(SO_INCOMING_NAPI_ID)
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	socklen_t salen = sizeof(int);

	rc = getsockopt(sock->fd, SOL_SOCKET, SO_INCOMING_NAPI_ID, placement_id, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockopt() failed (errno=%d)\n", errno);
	}

#endif
	return rc;
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

	memset(&event, 0, sizeof(event));
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
	struct spdk_posix_sock_request *req, *tmp;
	spdk_sock_op_cb cb_fn;
	void *cb_arg;
	int rc;

	assert(_sock->group_impl == _group);

	/* TODO: Need to do a zcopy reap to figure out what already sent */
	TAILQ_FOREACH_SAFE(req, &sock->queued_reqs, link, tmp) {
		spdk_posix_sock_request_pend(sock, req);

	}

	TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, link, tmp) {
		cb_fn = req->cb_fn;
		cb_arg = req->cb_arg;
		spdk_posix_sock_request_put(sock, req);
		cb_fn(cb_arg, -ECANCELED);

	}

	memset(sock->sendmsg_calls, 0, sizeof(sock->sendmsg_calls));
	sock->sendmsg_head = 0;
	sock->sendmsg_tail = 0;

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

static void
spdk_posix_sock_reap_reqs(struct spdk_posix_sock *sock, ssize_t bytes)
{
	struct spdk_posix_sock_request *req;
	unsigned int offset;
	size_t len;
	int i;
	spdk_sock_op_cb cb_fn;
	void *cb_arg;

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&sock->pending_reqs);
	while (req) {
		offset = req->offset;

		if (sock->zcopy) {
			assert(offset == 0);
		}

		for (i = 0; i < req->iovcnt; i++) {
			/* Advance by the offset first */
			if (offset >= req->iov[i].iov_len) {
				offset -= req->iov[i].iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = req->iov[i].iov_len - offset;

			if (len > (size_t)bytes) {
				/* This element was partially sent. */
				req->offset += bytes;
				return;
			}

			offset = 0;
			req->offset += len;
			bytes -= len;
		}

		req->offset = 0;
		cb_fn = req->cb_fn;
		cb_arg = req->cb_arg;
		spdk_posix_sock_request_put(sock, req);
		cb_fn(cb_arg, 0);

		if (bytes == 0) {
			break;
		}

		req = TAILQ_FIRST(&sock->pending_reqs);
	}
}

static int
spdk_posix_sock_flush(struct spdk_posix_sock *sock)
{
	struct msghdr msg = {};
	int flags;
	struct iovec iovs[DEFAULT_MAX_IOV];
	int iovcnt;
	struct spdk_posix_sock_request *req;
	int i;
	ssize_t rc;
	unsigned int offset;

	if (sock->sendmsg_head > (sock->sendmsg_tail + SENDMSG_RING_SIZE - 1)) {
		/* No more room in the ring */
		return 0;
	}

	/* Gather an iov */
	iovcnt = 0;
	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		offset = req->offset;

		if (sock->zcopy) {
			assert(offset == 0);
		}

		if (iovcnt + req->iovcnt > DEFAULT_MAX_IOV) {
			/* Don't process a request if there isn't space for all of its
			 * elements. This means in zcopy mode, we won't ever have any partial
			 * sends. */
			break;
		}

		for (i = 0; i < req->iovcnt; i++) {
			/* Consume any offset first */
			if (offset >= req->iov[i].iov_len) {
				offset -= req->iov[i].iov_len;
				continue;
			}

			iovs[iovcnt].iov_base = req->iov[i].iov_base + offset;
			iovs[iovcnt].iov_len = req->iov[i].iov_len - offset;
			iovcnt++;

			offset = 0;

			if (iovcnt >= DEFAULT_MAX_IOV) {
				break;
			}
		}

		if (iovcnt >= DEFAULT_MAX_IOV) {
			break;
		}

		spdk_posix_sock_request_pend(sock, req);

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	if (iovcnt == 0) {
		return 0;
	}

	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;
	if (sock->zcopy) {
		flags = MSG_ZEROCOPY;
	} else {
		flags = 0;
	}
	rc = sendmsg(sock->fd, &msg, flags);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		return rc;
	}

	if (sock->zcopy) {
		sock->sendmsg_calls[sock->sendmsg_head % SENDMSG_RING_SIZE] = rc;
		sock->sendmsg_head++;
	} else {
		spdk_posix_sock_reap_reqs(sock, rc);
	}

	return 0;
}

static void
spdk_posix_sock_check_zcopy(struct spdk_posix_sock *sock)
{
	struct msghdr msgh = { 0 };
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err) + 64];
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;

	msgh.msg_control = buf;
	msgh.msg_controllen = sizeof(buf);

	rc = recvmsg(sock->fd, &msgh, MSG_ERRQUEUE);

	if (rc < 0) {
		return;
	}

	cm = CMSG_FIRSTHDR(&msgh);
	if (cm == NULL) {
		return;
	}
	if (cm->cmsg_level != SOL_IP || cm->cmsg_type != IP_RECVERR) {
		return;
	}

	serr = (struct sock_extended_err *)CMSG_DATA(cm);
	if (serr->ee_errno != 0 || serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
		return;
	}

	for (idx = serr->ee_info; idx <= serr->ee_data; idx++) {
		sock->sendmsg_tail++;
		assert(sock->sendmsg_tail <= sock->sendmsg_head);
		spdk_posix_sock_reap_reqs(sock, sock->sendmsg_calls[idx % SENDMSG_RING_SIZE]);
		sock->sendmsg_calls[idx % SENDMSG_RING_SIZE] = 0;

	}
}

static int
spdk_posix_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
				struct spdk_sock **socks)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_sock *_sock, *tmp;
	struct spdk_posix_sock *sock;
	struct spdk_posix_sock_request *req, *rtmp;
	int num_events, i, rc;
	spdk_sock_op_cb cb_fn;
	void *cb_arg;
#if defined(__linux__)
	struct epoll_event events[MAX_EVENTS_PER_POLL];
#elif defined(__FreeBSD__)
	struct kevent events[MAX_EVENTS_PER_POLL];
	struct timespec ts = {0};
#endif

	TAILQ_FOREACH_SAFE(_sock, &_group->socks, link, tmp) {
		sock = __posix_sock(_sock);

		rc = spdk_posix_sock_flush(sock);
		if (rc) {
			TAILQ_FOREACH_SAFE(req, &sock->queued_reqs, link, rtmp) {
				spdk_posix_sock_request_pend(sock, req);
			}

			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, link, rtmp) {
				cb_fn = req->cb_fn;
				cb_arg = req->cb_arg;
				spdk_posix_sock_request_put(sock, req);
				cb_fn(cb_arg, -rc);
			}

			memset(sock->sendmsg_calls, 0, sizeof(sock->sendmsg_calls));
			sock->sendmsg_head = 0;
			sock->sendmsg_tail = 0;
			continue;
		}

		if (sock->zcopy && sock->sendmsg_tail != sock->sendmsg_head) {
			spdk_posix_sock_check_zcopy(sock);
		}
	}

#if defined(__linux__)
	num_events = epoll_wait(group->fd, events, max_events, 0);
#elif defined(__FreeBSD__)
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
	int rc;

	rc = close(group->fd);
	free(group);
	return rc;
}

static struct spdk_net_impl g_posix_net_impl = {
	.name		= "posix",
	.getaddr	= spdk_posix_sock_getaddr,
	.connect	= spdk_posix_sock_connect,
	.listen		= spdk_posix_sock_listen,
	.accept		= spdk_posix_sock_accept,
	.close		= spdk_posix_sock_close,
	.recv		= spdk_posix_sock_recv,
	.readv		= spdk_posix_sock_readv,
	.writev		= spdk_posix_sock_writev,
	.writev_async	= spdk_posix_sock_writev_async,
	.set_recvlowat	= spdk_posix_sock_set_recvlowat,
	.set_recvbuf	= spdk_posix_sock_set_recvbuf,
	.set_sendbuf	= spdk_posix_sock_set_sendbuf,
	.set_max_iovcnt	= spdk_posix_sock_set_max_iovcnt,
	.set_max_async_ops	= spdk_posix_sock_set_max_async_ops,
	.set_priority	= spdk_posix_sock_set_priority,
	.is_ipv6	= spdk_posix_sock_is_ipv6,
	.is_ipv4	= spdk_posix_sock_is_ipv4,
	.is_connected	= spdk_posix_sock_is_connected,
	.get_placement_id	= spdk_posix_sock_get_placement_id,
	.group_impl_create	= spdk_posix_sock_group_impl_create,
	.group_impl_add_sock	= spdk_posix_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_posix_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_posix_sock_group_impl_poll,
	.group_impl_close	= spdk_posix_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(posix, &g_posix_net_impl);
