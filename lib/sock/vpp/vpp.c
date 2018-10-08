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
#include "spdk/net.h"
#include "spdk/string.h"
#include "spdk_internal/sock.h"
#include <vcl/vppcom.h>

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

static bool g_vpp_initialized = false;

struct spdk_vpp_sock {
	struct spdk_sock	base;
	int			fd;
};

struct spdk_vpp_sock_group_impl {
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

	if (sa->sa_family == AF_INET) {
		result = inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				   host, hlen);
	} else {
		result = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				   host, hlen);
	}

	if (result == NULL) {
		return -1;
	}

	return 0;
}

#define __vpp_sock(sock) (struct spdk_vpp_sock *)sock
#define __vpp_group_impl(group) (struct spdk_vpp_sock_group_impl *)group

static inline void
vcom_socket_copy_ep_to_sockaddr(struct sockaddr *addr, socklen_t *len, vppcom_endpt_t *ep)
{
	int sa_len, copy_len;

	assert(ep->vrf == VPPCOM_VRF_DEFAULT);

	if (ep->is_ip4 == VPPCOM_IS_IP4) {
		addr->sa_family = AF_INET;
		((struct sockaddr_in *) addr)->sin_port = ep->port;
		if (*len > sizeof(struct sockaddr_in)) {
			*len = sizeof(struct sockaddr_in);
		}
		sa_len = sizeof(struct sockaddr_in) - sizeof(struct in_addr);
		copy_len = *len - sa_len;
		if (copy_len > 0) {
			memcpy(&((struct sockaddr_in *) addr)->sin_addr, ep->ip, copy_len);
		}
	} else {
		addr->sa_family = AF_INET6;
		((struct sockaddr_in6 *) addr)->sin6_port = ep->port;
		if (*len > sizeof(struct sockaddr_in6)) {
			*len = sizeof(struct sockaddr_in6);
		}
		sa_len = sizeof(struct sockaddr_in6) - sizeof(struct in6_addr);
		copy_len = *len - sa_len;
		if (copy_len > 0) {
			memcpy(&((struct sockaddr_in6 *) addr)->sin6_addr, ep->ip, copy_len);
		}
	}
}

static int
getsockname_vpp(int fd, struct sockaddr *addr, socklen_t *len)
{
	vppcom_endpt_t ep;
	uint32_t size = sizeof(ep);
	uint8_t addr_buf[sizeof(struct in6_addr)];
	int rc;

	if (!addr || !len) {
		return -EFAULT;
	}

	ep.ip = addr_buf;

	rc = vppcom_session_attr(fd, VPPCOM_ATTR_GET_LCL_ADDR, &ep, &size);
	if (rc == VPPCOM_OK) {
		vcom_socket_copy_ep_to_sockaddr(addr, len, &ep);
	}

	return rc;
}


static int
getpeername_vpp(int sock, struct sockaddr *addr, socklen_t *len)
{
	vppcom_endpt_t ep;
	uint32_t size = sizeof(ep);
	uint8_t addr_buf[sizeof(struct in6_addr)];
	int rc;

	if (!addr || !len) {
		return -EFAULT;
	}

	ep.ip = addr_buf;

	rc = vppcom_session_attr(sock, VPPCOM_ATTR_GET_PEER_ADDR, &ep, &size);
	if (rc == VPPCOM_OK) {
		vcom_socket_copy_ep_to_sockaddr(addr, len, &ep);
	}

	return rc;
}

static int
spdk_vpp_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		      char *caddr, int clen, uint16_t *cport)
{
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	struct sockaddr sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);
	assert(g_vpp_initialized);

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);
	rc = getsockname_vpp(sock->fd, &sa, &salen);
	if (rc != 0) {
		errno = -rc;
		SPDK_ERRLOG("getsockname_vpp() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = get_addr_str(&sa, saddr, slen);
	if (rc != 0) {
		/* Errno already set by get_addr_str() */
		SPDK_ERRLOG("get_addr_str() failed (errno=%d)\n", errno);
		return -1;
	}

	if (sport) {
		if (sa.ss_family == AF_INET) {
			*sport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*sport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);
	rc = getpeername_vpp(sock->fd, &sa, &salen);
	if (rc != 0) {
		errno = -rc;
		SPDK_ERRLOG("getpeername_vpp() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = get_addr_str(&sa, caddr, clen);
	if (rc != 0) {
		/* Errno already set by get_addr_str() */
		SPDK_ERRLOG("get_addr_str() failed (errno=%d)\n", errno);
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

enum spdk_vpp_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static struct spdk_sock *
spdk_vpp_sock_create(const char *ip, int port, enum spdk_vpp_create_type type)
{
	struct spdk_vpp_sock *sock;
	int fd, rc;
	vppcom_endpt_t endpt;
	uint8_t addr_buf[sizeof(struct in6_addr)];

	if (ip == NULL) {
		return NULL;
	}

	/* Check address family */
	if (inet_pton(AF_INET, ip, &addr_buf)) {
		endpt.is_ip4 = VPPCOM_IS_IP4;
	} else if (inet_pton(AF_INET6, ip, &addr_buf)) {
		endpt.is_ip4 = VPPCOM_IS_IP6;
	} else {
		SPDK_ERRLOG("IP address with invalid format\n");
		return NULL;
	}
	endpt.vrf = VPPCOM_VRF_DEFAULT;
	endpt.ip = (uint8_t *)&addr_buf;
	endpt.port = htons(port);

	fd = vppcom_session_create(VPPCOM_VRF_DEFAULT, VPPCOM_PROTO_TCP, 1 /* is_nonblocking */);
	if (fd < 0) {
		errno = -fd;
		SPDK_ERRLOG("vppcom_session_create() failed, errno = %d\n", errno);
		return NULL;
	}

	if (type == SPDK_SOCK_CREATE_LISTEN) {
		rc = vppcom_session_bind(fd, &endpt);
		if (rc != VPPCOM_OK) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_bind() failed, errno = %d\n", errno);
			vppcom_session_close(fd);
			return NULL;
		}

		rc = vppcom_session_listen(fd, 512);
		if (rc != VPPCOM_OK) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_listen() failed, errno = %d\n", errno);
			vppcom_session_close(fd);
			return NULL;
		}
	} else if (type == SPDK_SOCK_CREATE_CONNECT) {
		rc = vppcom_session_connect(fd, &endpt);
		if (rc != VPPCOM_OK) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_connect() failed, errno = %d\n", errno);
			vppcom_session_close(fd);
			return NULL;
		}
	}

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		errno = -ENOMEM;
		SPDK_ERRLOG("sock allocation failed\n");
		vppcom_session_close(fd);
		return NULL;
	}

	sock->fd = fd;
	return &sock->base;
}

static struct spdk_sock *
spdk_vpp_sock_listen(const char *ip, int port)
{
	if (!g_vpp_initialized) {
		return NULL;
	}

	return spdk_vpp_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN);
}

static struct spdk_sock *
spdk_vpp_sock_connect(const char *ip, int port)
{
	if (!g_vpp_initialized) {
		return NULL;
	}

	return spdk_vpp_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT);
}

static struct spdk_sock *
spdk_vpp_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	vppcom_endpt_t		endpt;
	uint8_t			ip[16];
	int			rc;
	struct spdk_vpp_sock	*new_sock;
	double			wait_time = -1.0;

	endpt.ip = ip;

	assert(sock != NULL);
	assert(g_vpp_initialized);

	rc = vppcom_session_accept(sock->fd, &endpt, O_NONBLOCK, wait_time);
	if (rc < 0) {
		errno = -rc;
		return NULL;
	}

	new_sock = calloc(1, sizeof(*sock));
	if (new_sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		vppcom_session_close(rc);
		return NULL;
	}

	new_sock->fd = rc;
	return &new_sock->base;
}

static int
spdk_vpp_sock_close(struct spdk_sock *_sock)
{
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	int rc;

	assert(sock != NULL);
	assert(g_vpp_initialized);

	rc = vppcom_session_close(sock->fd);
	if (rc != VPPCOM_OK) {
		errno = -rc;
		return -1;
	}
	free(sock);

	return 0;
}

static ssize_t
spdk_vpp_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	int rc;

	assert(sock != NULL);
	assert(g_vpp_initialized);

	rc = vppcom_session_read(sock->fd, buf, len);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

static ssize_t
spdk_vpp_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	ssize_t total = 0;
	int i, rc;

	assert(sock != NULL);
	assert(g_vpp_initialized);

	for (i = 0; i < iovcnt; ++i) {
		rc = vppcom_session_write(sock->fd, iov[i].iov_base, iov[i].iov_len);
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


/*
 * TODO: Check if there are similar parameters to configure in VPP
 * to three below.
 */
static int
spdk_vpp_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	assert(g_vpp_initialized);

	return 0;
}

static int
spdk_vpp_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	assert(g_vpp_initialized);

	return 0;
}

static int
spdk_vpp_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	assert(g_vpp_initialized);

	return 0;
}

static bool
spdk_vpp_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	vppcom_endpt_t ep;
	uint32_t size = sizeof(ep);
	uint8_t addr_buf[sizeof(struct in6_addr)];
	int rc;

	assert(sock != NULL);
	assert(g_vpp_initialized);

	ep.ip = addr_buf;

	rc = vppcom_session_attr(sock->fd, VPPCOM_ATTR_GET_LCL_ADDR, &ep, &size);
	if (rc != VPPCOM_OK) {
		errno = -rc;
		return false;
	}

	return (ep.is_ip4 == VPPCOM_IS_IP6);
}

static bool
spdk_vpp_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	vppcom_endpt_t ep;
	uint32_t size = sizeof(ep);
	uint8_t addr_buf[sizeof(struct in6_addr)];
	int rc;

	assert(sock != NULL);
	assert(g_vpp_initialized);

	ep.ip = addr_buf;

	rc = vppcom_session_attr(sock->fd, VPPCOM_ATTR_GET_LCL_ADDR, &ep, &size);
	if (rc != VPPCOM_OK) {
		errno = -rc;
		return false;
	}

	return (ep.is_ip4 == VPPCOM_IS_IP4);
}

static struct spdk_sock_group_impl *
spdk_vpp_sock_group_impl_create(void)
{
	struct spdk_vpp_sock_group_impl *group_impl;
	int fd;

	if (!g_vpp_initialized) {
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("sock_group allocation failed\n");
		return NULL;
	}

	fd = vppcom_epoll_create();
	if (fd < 0) {
		free(group_impl);
		return NULL;
	}

	group_impl->fd = fd;

	return &group_impl->base;
}

static int
spdk_vpp_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_vpp_sock_group_impl *group = __vpp_group_impl(_group);
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	int rc;
	struct epoll_event event;

	assert(sock != NULL);
	assert(group != NULL);
	assert(g_vpp_initialized);

	event.events = EPOLLIN;
	event.data.ptr = sock;

	rc = vppcom_epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);
	if (rc != VPPCOM_OK) {
		errno = -rc;
		return -1;
	}

	return 0;
}

static int
spdk_vpp_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_vpp_sock_group_impl *group = __vpp_group_impl(_group);
	struct spdk_vpp_sock *sock = __vpp_sock(_sock);
	int rc;
	struct epoll_event event;

	assert(sock != NULL);
	assert(group != NULL);
	assert(g_vpp_initialized);

	rc = vppcom_epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event);
	if (rc != VPPCOM_OK) {
		errno = -rc;
		return -1;
	}

	return 0;
}

static int
spdk_vpp_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			      struct spdk_sock **socks)
{
	struct spdk_vpp_sock_group_impl *group = __vpp_group_impl(_group);
	int num_events, i;
	struct epoll_event events[MAX_EVENTS_PER_POLL];

	assert(group != NULL);
	assert(socks != NULL);
	assert(g_vpp_initialized);

	num_events = vppcom_epoll_wait(group->fd, events, max_events, 0);
	if (num_events < 0) {
		errno = -num_events;
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		socks[i] = events[i].data.ptr;
	}

	return num_events;
}

static int
spdk_vpp_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_vpp_sock_group_impl *group = __vpp_group_impl(_group);
	int rc;

	assert(group != NULL);
	assert(g_vpp_initialized);

	rc = vppcom_session_close(group->fd);
	if (rc != VPPCOM_OK) {
		errno = -rc;
		return -1;
	}

	return 0;
}

static struct spdk_net_impl g_vpp_net_impl = {
	.name		= "vpp",
	.getaddr	= spdk_vpp_sock_getaddr,
	.connect	= spdk_vpp_sock_connect,
	.listen		= spdk_vpp_sock_listen,
	.accept		= spdk_vpp_sock_accept,
	.close		= spdk_vpp_sock_close,
	.recv		= spdk_vpp_sock_recv,
	.writev		= spdk_vpp_sock_writev,
	.set_recvlowat	= spdk_vpp_sock_set_recvlowat,
	.set_recvbuf	= spdk_vpp_sock_set_recvbuf,
	.set_sendbuf	= spdk_vpp_sock_set_sendbuf,
	.is_ipv6	= spdk_vpp_sock_is_ipv6,
	.is_ipv4	= spdk_vpp_sock_is_ipv4,
	.group_impl_create	= spdk_vpp_sock_group_impl_create,
	.group_impl_add_sock	= spdk_vpp_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_vpp_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_vpp_sock_group_impl_poll,
	.group_impl_close	= spdk_vpp_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(vpp, &g_vpp_net_impl);

static int
spdk_vpp_net_framework_init(void)
{
	int rc;
	char *app_name;

	app_name = spdk_sprintf_alloc("SPDK_%d", getpid());
	if (app_name == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for SPDK app name\n");
		return -ENOMEM;
	}

	rc = vppcom_app_create(app_name);
	if (rc == 0) {
		g_vpp_initialized = true;
	}

	free(app_name);

	return 0;
}

static void
spdk_vpp_net_framework_fini(void)
{
	if (g_vpp_initialized) {
		vppcom_app_destroy();
	}
}

static struct spdk_net_framework g_vpp_net_framework = {
	.name	= "vpp",
	.init	= spdk_vpp_net_framework_init,
	.fini	= spdk_vpp_net_framework_fini,
};

SPDK_NET_FRAMEWORK_REGISTER(vpp, &g_vpp_net_framework);
