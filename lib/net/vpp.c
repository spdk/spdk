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
#include "spdk/queue.h"
#include <vcl/vppcom.h>

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

#define MAX_EVENTS_PER_POLL 32

struct spdk_sock {
	int			fd;
	spdk_sock_cb		cb_fn;
	void			*cb_arg;
	TAILQ_ENTRY(spdk_sock)	link;
};

struct spdk_sock_group {
	int			fd;
	TAILQ_HEAD(, spdk_sock)	socks;
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

static int
getsockname_vpp(int fd, struct sockaddr *addr, socklen_t *len)
{
	vppcom_endpt_t ep;
	uint32_t size = sizeof(ep);
	int rc;

	if (!addr || !len) {
		return -EFAULT;
	}

	ep.ip = (uint8_t *) & ((const struct sockaddr_in *) addr)->sin_addr;

	rc = vppcom_session_attr(fd, VPPCOM_ATTR_GET_LCL_ADDR, &ep, &size);
	if (rc == 0) {
		if (ep.vrf == VPPCOM_VRF_DEFAULT) {
			addr->sa_family = ep.is_ip4 == VPPCOM_IS_IP4 ? AF_INET : AF_INET6;
			switch (addr->sa_family) {
			case AF_INET:
				((struct sockaddr_in *) addr)->sin_port = ep.port;
				*len = sizeof(struct sockaddr_in);
				break;

			case AF_INET6:
				((struct sockaddr_in6 *) addr)->sin6_port = ep.port;
				*len = sizeof(struct sockaddr_in6);
				break;

			default:
				break;
			}
		}
	}

	return rc;
}

static inline int
vcom_socket_copy_ep_to_sockaddr(struct sockaddr *addr, socklen_t *len, vppcom_endpt_t *ep)
{
	int rc = 0;
	int sa_len, copy_len;

	addr->sa_family = (ep->is_ip4 == VPPCOM_IS_IP4) ? AF_INET : AF_INET6;
	switch (addr->sa_family) {
	case AF_INET:
		((struct sockaddr_in *) addr)->sin_port = ep->port;
		if (*len > sizeof(struct sockaddr_in)) {
			*len = sizeof(struct sockaddr_in);
		}
		sa_len = sizeof(struct sockaddr_in) - sizeof(struct in_addr);
		copy_len = *len - sa_len;
		if (copy_len > 0) {
			memcpy(&((struct sockaddr_in *) addr)->sin_addr, ep->ip, copy_len);
		}
		break;

	case AF_INET6:
		((struct sockaddr_in6 *) addr)->sin6_port = ep->port;
		if (*len > sizeof(struct sockaddr_in6)) {
			*len = sizeof(struct sockaddr_in6);
		}
		sa_len = sizeof(struct sockaddr_in6) - sizeof(struct in6_addr);
		copy_len = *len - sa_len;
		if (copy_len > 0)
			memcpy(((struct sockaddr_in6 *) addr)->sin6_addr.
			       __in6_u.__u6_addr8, ep->ip, copy_len);
		break;

	default:
		/* Not possible */
		rc = -EAFNOSUPPORT;
		break;
	}

	return rc;
}

static int
getpeername_vpp(int sock, struct sockaddr *addr, socklen_t *len)
{
	int rc = -1;
	uint8_t src_addr[sizeof(struct sockaddr_in6)];
	vppcom_endpt_t ep;
	uint32_t size = sizeof(ep);

	if (!addr || !len) {
		return -EFAULT;
	}

	ep.ip = src_addr;

	rc = vppcom_session_attr(sock, VPPCOM_ATTR_GET_PEER_ADDR, &ep, &size);
	if (rc != 0) {
		return rc;
	}

	return vcom_socket_copy_ep_to_sockaddr(addr, len, &ep);
}

int
spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, char *caddr, int clen)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname_vpp(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		errno = -rc;
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
	rc = getpeername_vpp(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		errno = -rc;
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

enum spdk_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static struct spdk_sock *
spdk_sock_create(const char *ip, int port, enum spdk_sock_create_type type)
{
	struct spdk_sock *sock;
	int fd, rc;
	vppcom_endpt_t endpt;
	struct sockaddr_in servaddr;

	if (ip == NULL) {
		return NULL;
	}

	fd = vppcom_session_create(VPPCOM_VRF_DEFAULT, VPPCOM_PROTO_TCP, 1 /* is_nonblocking */);
	if (fd < 0) {
		errno = -fd;
		SPDK_ERRLOG("vppcom_session_create() failed, errno = %d\n", errno);
		return NULL;
	}

	/* TODO: Check for IPv6 */
	servaddr.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &(servaddr.sin_addr));
	servaddr.sin_port = htons(port);

	endpt.vrf = VPPCOM_VRF_DEFAULT;
	endpt.is_ip4 = (servaddr.sin_family == AF_INET);
	endpt.ip = (uint8_t *) & servaddr.sin_addr;
	endpt.port = htons(port);

	if (type == SPDK_SOCK_CREATE_LISTEN) {
		rc = vppcom_session_bind(fd, &endpt);
		if (rc != 0) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_bind() failed, errno = %d\n", errno);
			vppcom_session_close(fd);
			fd = -1;
			goto end;
		}

		rc = vppcom_session_listen(fd, 10);
		if (rc) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_listen() failed, errno = %d\n", errno);
			vppcom_session_close(fd);
			fd = -1;
			goto end;
		}
	} else if (type == SPDK_SOCK_CREATE_CONNECT) {
		rc = vppcom_session_connect(fd, &endpt);
		if (rc) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_connect() failed, errno = %d\n", errno);
			vppcom_session_close(fd);
			fd = -1;
			goto end;
		}
	}
end:
	if (fd < 0) {
		return NULL;
	}

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		vppcom_session_close(fd);
		return NULL;
	}

	sock->fd = fd;
	return sock;
}

struct spdk_sock *
spdk_sock_listen(const char *ip, int port)
{
	return spdk_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN);
}

struct spdk_sock *
spdk_sock_connect(const char *ip, int port)
{
	return spdk_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT);
}

struct spdk_sock *
spdk_sock_accept(struct spdk_sock *sock)
{
	vppcom_endpt_t		endpt;
	uint8_t			ip[16];
	int			rc;
	struct spdk_sock	*new_sock;
	double			wait_time = -1.0;

	endpt.ip = ip;

	rc = vppcom_session_accept(sock->fd, &endpt, O_NONBLOCK, wait_time);
	if (rc < 0) {
		errno = -rc;
		return NULL;
	}

	new_sock = calloc(1, sizeof(*sock));
	if (new_sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(rc);
		return NULL;
	}

	new_sock->fd = rc;
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

	rc = vppcom_session_close((*sock)->fd);

	if (rc == 0) {
		free(*sock);
		*sock = NULL;
	}

	return rc;
}

ssize_t
spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	int rc;

	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	rc = vppcom_session_read(sock->fd, buf, len);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

ssize_t
spdk_sock_writev(struct spdk_sock *sock, struct iovec *__iov, int __iovcnt)
{
	int rc = -1;
	ssize_t total = 0;
	int i;

	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}


	if (__iov == 0 || __iovcnt == 0 || __iovcnt > IOV_MAX) {
		errno = EINVAL;
		return -1;
	}

	for (i = 0; i < __iovcnt; ++i) {
		rc = vppcom_session_write(sock->fd, __iov[i].iov_base,
					  __iov[i].iov_len);
		if (rc < 0) {
			if (total > 0) {
				break;
			} else {
				errno = -rc;
				return -1;
			}
		} else {
			total += rc;
		}
	}
	return total;
}

int
spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes)
{
	assert(sock != NULL);

	return 0;
}

int
spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz)
{
	assert(sock != NULL);

	return 0;
}

int
spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz)
{
	assert(sock != NULL);

	return 0;
}

bool
spdk_sock_is_ipv6(struct spdk_sock *sock)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname_vpp(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		errno = -rc;
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);
}

bool
spdk_sock_is_ipv4(struct spdk_sock *sock)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname_vpp(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		errno = -rc;
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}

struct spdk_sock_group *
spdk_sock_group_create(void)
{
	struct spdk_sock_group *sock_group;
	int fd;

	fd = vppcom_epoll_create();
	if (fd == -1) {
		return NULL;
	}

	sock_group = calloc(1, sizeof(*sock_group));
	if (sock_group == NULL) {
		SPDK_ERRLOG("sock_group allocation failed\n");
		close(fd);
		return NULL;
	}

	sock_group->fd = fd;
	TAILQ_INIT(&sock_group->socks);

	return sock_group;
}

int
spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			 spdk_sock_cb cb_fn, void *cb_arg)
{
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

	struct epoll_event event;

	event.events = EPOLLIN;
	event.data.ptr = sock;

	rc = vppcom_epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);
	if (rc == 0) {
		TAILQ_INSERT_TAIL(&group->socks, sock, link);
		sock->cb_fn = cb_fn;
		sock->cb_arg = cb_arg;
	}

	return rc;
}

int
spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock)
{
	int rc;
	struct epoll_event event;

	/* Event parameter is ignored but some old kernel version still require it. */
	rc = vppcom_epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event);
	if (rc == 0) {
		TAILQ_REMOVE(&group->socks, sock, link);
		sock->cb_fn = NULL;
		sock->cb_arg = NULL;
	}

	return rc;
}

int
spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events)
{
	struct spdk_sock *sock;
	int num_events, i;
	struct epoll_event events[MAX_EVENTS_PER_POLL];

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

	num_events = vppcom_epoll_wait(group->fd, events, max_events, 0);

	if (num_events == -1) {
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		sock = events[i].data.ptr;

		assert(sock->cb_fn != NULL);
		sock->cb_fn(sock->cb_arg, group, sock);
	}

	return 0;
}

int
spdk_sock_group_poll(struct spdk_sock_group *group)
{
	return spdk_sock_group_poll_count(group, MAX_EVENTS_PER_POLL);
}

int
spdk_sock_group_close(struct spdk_sock_group **group)
{
	int rc;

	if (*group == NULL) {
		errno = EBADF;
		return -1;
	}

	if (!TAILQ_EMPTY(&(*group)->socks)) {
		errno = EBUSY;
		return -1;
	}

	rc = vppcom_session_close((*group)->fd);

	if (rc == 0) {
		free(*group);
		*group = NULL;
	}

	return rc;
}

void
spdk_sock_init(void)
{
	vppcom_app_create("SPDK_APP");
}

void
spdk_sock_fini(void)
{
	vppcom_app_destroy();
}
