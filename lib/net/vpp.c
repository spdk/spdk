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
#include "spdk/net.h"
#include <vcl/vppcom.h>

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

static int get_addr_str(struct sockaddr *sa, char *host, size_t hlen)
{
	const char *result = NULL;

	if (sa == NULL || host == NULL) {
		return -EFAULT;
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
getsockname_vpp(int sock, struct sockaddr *addr, socklen_t *len)
{
	vppcom_endpt_t ep;
	uint32_t size = sizeof(ep);
	int rc;

	if (!addr || !len) {
		return -EFAULT;
	}

	ep.ip = (uint8_t *) & ((const struct sockaddr_in *) addr)->sin_addr;

	rc = vppcom_session_attr(sock, VPPCOM_ATTR_GET_LCL_ADDR, &ep, &size);
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
spdk_sock_getaddr(int sock, char *saddr, int slen, char *caddr, int clen)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname_vpp(sock, (struct sockaddr *) &sa, &salen);
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
	rc = getpeername_vpp(sock, (struct sockaddr *) &sa, &salen);
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

static int
spdk_sock_create(const char *ip, int port, enum spdk_sock_create_type type)
{
	char *p;
	char buf[MAX_TMPBUF];
	int sock, rc;
	vppcom_endpt_t endpt;
	struct sockaddr_in servaddr;

	if (ip == NULL) {
		return -1;
	}
	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		ip = (const char *) &buf[0];
	}

	sock = vppcom_session_create(VPPCOM_VRF_DEFAULT, VPPCOM_PROTO_TCP, 1 /* is_nonblocking */);
	if (sock < 0) {
		errno = -sock;
		SPDK_ERRLOG("vppcom_session_create() failed, errno = %d\n", errno);
		return -1;
	}

	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(port);

	endpt.vrf = VPPCOM_VRF_DEFAULT;
	endpt.is_ip4 = (servaddr.sin_family == AF_INET);
	endpt.ip = (uint8_t *) & servaddr.sin_addr;
	endpt.port = (uint16_t) servaddr.sin_port;

	if (type == SPDK_SOCK_CREATE_LISTEN) {
		rc = vppcom_session_bind(sock, &endpt);
		if (rc != 0) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_bind() failed, errno = %d\n", errno);
			spdk_sock_close(sock);
			return -1;
		}

		rc = vppcom_session_listen(sock, 10);
		if (rc) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_listen() failed, errno = %d\n", errno);
			spdk_sock_close(sock);
			return -1;
		}
	} else if (type == SPDK_SOCK_CREATE_CONNECT) {
		rc = vppcom_session_connect(sock, &endpt);
		if (rc) {
			errno = -rc;
			SPDK_ERRLOG("vppcom_session_connect() failed, errno = %d\n", errno);
			spdk_sock_close(sock);
			return -1;
		}
	}

	return sock;
}

int
spdk_sock_listen(const char *ip, int port)
{
	return spdk_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN);
}

int
spdk_sock_connect(const char *ip, int port)
{
	return spdk_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT);
}

int
spdk_sock_accept(int sock)
{
	vppcom_endpt_t endpt;
	uint8_t ip[16];
	double wait_time = -1.0;
	int rc;

	endpt.ip = ip;

	rc = vppcom_session_accept(sock, &endpt, O_NONBLOCK, wait_time);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

int
spdk_sock_close(int sock)
{
	int rc;

	rc = vppcom_session_close(sock);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

ssize_t
spdk_sock_recv(int sock, void *buf, size_t len)
{
	int rc;
	rc = vppcom_session_read(sock, buf, len);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

ssize_t
spdk_sock_writev(int sock, struct iovec *__iov, int __iovcnt)
{
	int rc = -1;
	ssize_t total = 0;
	int i;

	if (__iov == 0 || __iovcnt == 0 || __iovcnt > IOV_MAX) {
		errno = EINVAL;
		return -1;
	}

	for (i = 0; i < __iovcnt; ++i) {
		rc = vppcom_session_write(sock, __iov[i].iov_base,
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
spdk_sock_set_recvlowat(int s, int nbytes)
{
	return 0;
}

int
spdk_sock_set_recvbuf(int sock, int sz)
{
	return 0;
}

int
spdk_sock_set_sendbuf(int sock, int sz)
{
	return 0;
}

int
spdk_epoll_create(int size)
{
	int rc;

	rc = vppcom_epoll_create();
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

int
spdk_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	int rc;

	rc = vppcom_epoll_ctl(epfd, op, fd, event);
	if (rc != 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

int
spdk_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
	int rc;

	rc = vppcom_epoll_wait(epfd, events, maxevents, timeout);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return rc;
}

bool
spdk_sock_is_ipv6(int sock)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname_vpp(sock, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		errno = -rc;
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);
}

bool
spdk_sock_is_ipv4(int sock)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname_vpp(sock, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		errno = -rc;
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
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
