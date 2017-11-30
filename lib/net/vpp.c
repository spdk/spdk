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
#include </home/tzawadzk/vpp/src/vcl/vppcom.h>

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

static int get_addr_str(struct sockaddr *sa, char *host, size_t hlen)
{
	const char *result = NULL;

	if (sa == NULL || host == NULL)
		return -1;

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

	if (result != NULL)
		return 0;
	else
		return -1;
}

static inline int
vpp_session_getsockname(int sid, vppcom_endpt_t *ep)
{
	int rv;
	uint32_t size = sizeof(*ep);

	rv = vppcom_session_attr(sid, VPPCOM_ATTR_GET_LCL_ADDR, ep, &size);
	return rv;
}

static int
getsockname_vpp(int sock, struct sockaddr *__addr, socklen_t *__len)
{
	int rv = -1;

	if (!__addr || !__len)
		return -EFAULT;

	vppcom_endpt_t ep;
	ep.ip = (uint8_t *) & ((const struct sockaddr_in *) __addr)->sin_addr;
	rv = vpp_session_getsockname(sock, &ep);
	if (rv == 0) {
		if (ep.vrf == VPPCOM_VRF_DEFAULT) {
			__addr->sa_family = ep.is_ip4 == VPPCOM_IS_IP4 ? AF_INET : AF_INET6;
			switch (__addr->sa_family) {
			case AF_INET:
				((struct sockaddr_in *) __addr)->sin_port = ep.port;
				*__len = sizeof(struct sockaddr_in);
				break;

			case AF_INET6:
				((struct sockaddr_in6 *) __addr)->sin6_port = ep.port;
				*__len = sizeof(struct sockaddr_in6);
				break;

			default:
				break;
			}
		}
	}

	return rv;
}

static inline int
vcom_socket_copy_ep_to_sockaddr(struct sockaddr *__addr,
				socklen_t *__len,
				vppcom_endpt_t *ep)
{
	int rv = 0;
	int sa_len, copy_len;

	__addr->sa_family = (ep->is_ip4 == VPPCOM_IS_IP4) ? AF_INET : AF_INET6;
	switch (__addr->sa_family) {
	case AF_INET:
		((struct sockaddr_in *) __addr)->sin_port = ep->port;
		if (*__len > sizeof(struct sockaddr_in))
			*__len = sizeof(struct sockaddr_in);
		sa_len = sizeof(struct sockaddr_in) - sizeof(struct in_addr);
		copy_len = *__len - sa_len;
		if (copy_len > 0)
			memcpy(&((struct sockaddr_in *) __addr)->sin_addr, ep->ip, copy_len);
		break;

	case AF_INET6:
		((struct sockaddr_in6 *) __addr)->sin6_port = ep->port;
		if (*__len > sizeof(struct sockaddr_in6))
			*__len = sizeof(struct sockaddr_in6);
		sa_len = sizeof(struct sockaddr_in6) - sizeof(struct in6_addr);
		copy_len = *__len - sa_len;
		if (copy_len > 0)
			memcpy(((struct sockaddr_in6 *) __addr)->sin6_addr.
			       __in6_u.__u6_addr8, ep->ip, copy_len);
		break;

	default:
		/* Not possible */
		rv = -EAFNOSUPPORT;
		break;
	}

	return rv;
}

static inline int
vpp_session_getpeername(int sid, vppcom_endpt_t *ep)
{
	int rv;
	uint32_t size = sizeof(*ep);

	rv = vppcom_session_attr(sid, VPPCOM_ATTR_GET_PEER_ADDR, ep, &size);
	return rv;
}

static int
getpeername_vpp(int sock, struct sockaddr *__addr,
		socklen_t *__len)
{
	int rv = -1;
	uint8_t src_addr[sizeof(struct sockaddr_in6)];
	vppcom_endpt_t ep;

	if (!__addr || !__len)
		return -EFAULT;

	ep.ip = src_addr;
	rv = vpp_session_getpeername(sock, &ep);
	if (rv == 0)
		rv = vcom_socket_copy_ep_to_sockaddr(__addr, __len, &ep);

	return rv;
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
#ifdef NOPE
	char portnum[PORTNUMLEN];
	struct addrinfo hints, *res, *res0;
	int flag;
	int val = 1;
#endif
	char *p;
	char buf[MAX_TMPBUF];
	int sock, rc;
	vppcom_endpt_t endpt;
	struct sockaddr_in servaddr;

	if (ip == NULL)
		return -1;
	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL)
			*p = '\0';
		ip = (const char *) &buf[0];
	}

	sock = vppcom_session_create(VPPCOM_VRF_DEFAULT, VPPCOM_PROTO_TCP, 1 /* is_nonblocking */);
	if (sock < 0) {
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
		if (rc) {
			return -1;
		}

		rc = vppcom_session_listen(sock, 10);
		if (rc) {
			return -1;
		}
	} else if (type == SPDK_SOCK_CREATE_CONNECT) {
		rc = vppcom_session_connect(sock, &endpt);
		if (rc) {
			return -1;
		}
	}

	return sock;


#ifdef NOPE

	if (ip == NULL)
		return -1;
	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL)
			*p = '\0';
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
		return -1;
	}

	/* try listen */
	sock = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
retry:
		sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (sock < 0) {
			/* error */
			continue;
		}
		rc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			close(sock);
			/* error */
			continue;
		}
		rc = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
		if (rc != 0) {
			close(sock);
			/* error */
			continue;
		}

		if (type == SPDK_SOCK_CREATE_LISTEN) {
			rc = bind(sock, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("bind() failed, errno = %d\n", errno);
				switch (errno) {
				case EINTR:
					/* interrupted? */
					close(sock);
					goto retry;
				case EADDRNOTAVAIL:
					SPDK_ERRLOG("IP address %s not available. "
						    "Verify IP address in config file "
						    "and make sure setup script is "
						    "run before starting spdk app.\n", ip);
				/* FALLTHROUGH */
				default:
					/* try next family */
					close(sock);
					sock = -1;
					continue;
				}
			}
			/* bind OK */
			rc = listen(sock, 512);
			if (rc != 0) {
				SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
				close(sock);
				sock = -1;
				break;
			}
		} else if (type == SPDK_SOCK_CREATE_CONNECT) {
			rc = connect(sock, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
				/* try next family */
				close(sock);
				sock = -1;
				continue;
			}
		}

		flag = fcntl(sock, F_GETFL);
		if (fcntl(sock, F_SETFL, flag | O_NONBLOCK) < 0) {
			SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", sock, errno);
			close(sock);
			sock = -1;
			break;
		}
		break;
	}
	freeaddrinfo(res0);

	if (sock < 0) {
		return -1;
	}
	return sock;
#endif
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

	endpt.ip = ip;

	return vppcom_session_accept(sock, &endpt, O_NONBLOCK, wait_time);
}

int
spdk_sock_close(int sock)
{
	return close(sock);
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
	int rv = -1;
	ssize_t total = 0;
	int i;

	if (__iov == 0 || __iovcnt == 0 || __iovcnt > IOV_MAX)
		return -EINVAL;

	for (i = 0; i < __iovcnt; ++i) {
		rv = vppcom_session_write(sock, __iov[i].iov_base,
					  __iov[i].iov_len);
		if (rv < 0) {
			if (total > 0)
				break;
			else
				return rv;
		} else
			total += rv;
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
spdk_epoll_create1(int size)
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
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}
