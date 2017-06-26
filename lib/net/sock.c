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

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

static int get_addr_str(struct sockaddr_in *paddr, char *host, size_t hlen)
{
	uint8_t *pa;

	if (paddr == NULL || host == NULL)
		return -1;

	pa = (uint8_t *)&paddr->sin_addr.s_addr;
	snprintf(host, hlen, "%u.%u.%u.%u", pa[0], pa[1], pa[2], pa[3]);

	return 0;
}

int
spdk_sock_getaddr(int sock, char *saddr, int slen, char *caddr, int clen)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock, (struct sockaddr *) &sa, &salen);
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

	rc = get_addr_str((struct sockaddr_in *)&sa, saddr, slen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getpeername(sock, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = get_addr_str((struct sockaddr_in *)&sa, caddr, clen);
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
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int sock, nonblock;
	int val = 1;
	int rc;

	if (ip == NULL)
		return -1;
	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL)
			*p = '\0';
		ip = (const char *) &buf[0];
		if (strcasecmp(ip, "*") == 0) {
			snprintf(buf, sizeof(buf), "::");
			ip = (const char *) &buf[0];
		}
	} else if (strcasecmp(ip, "*") == 0) {
		snprintf(buf, sizeof(buf), "0.0.0.0");
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

		nonblock = 1;
		rc = ioctl(sock, FIONBIO, &nonblock);
		if (rc != 0) {
			SPDK_ERRLOG("ioctl(FIONBIO) failed\n");
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
	struct sockaddr_storage		sa;
	socklen_t			salen;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);
	return accept(sock, (struct sockaddr *)&sa, &salen);
}

int
spdk_sock_close(int sock)
{
	return close(sock);
}

ssize_t
spdk_sock_recv(int sock, void *buf, size_t len)
{
	return recv(sock, buf, len, MSG_DONTWAIT);
}

ssize_t
spdk_sock_writev(int sock, struct iovec *iov, int iovcnt)
{
	return writev(sock, iov, iovcnt);
}

int
spdk_sock_set_recvlowat(int s, int nbytes)
{
	int val;
	int rc;

	val = nbytes;
	rc = setsockopt(s, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
	if (rc != 0)
		return -1;
	return 0;
}

int
spdk_sock_set_recvbuf(int sock, int sz)
{
	return setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
			  &sz, sizeof(sz));
}

int
spdk_sock_set_sendbuf(int sock, int sz)
{
	return setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
			  &sz, sizeof(sz));
}

bool
spdk_sock_is_ipv6(int sock)
{
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock, (struct sockaddr *) &sa, &salen);
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
	rc = getsockname(sock, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}
