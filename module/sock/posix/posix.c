/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/pipe.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk_internal/sock.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY)
#define SPDK_ZEROCOPY
#endif

struct spdk_posix_sock {
	struct spdk_sock	base;
	int			fd;

	uint32_t		sendmsg_idx;

	struct spdk_pipe	*recv_pipe;
	void			*recv_buf;
	int			recv_buf_sz;
	bool			pending_recv;
	bool			zcopy;
	int			so_priority;

	TAILQ_ENTRY(spdk_posix_sock)	link;
};

struct spdk_posix_sock_group_impl {
	struct spdk_sock_group_impl	base;
	int				fd;
	TAILQ_HEAD(, spdk_posix_sock)	pending_recv;
};

static struct spdk_sock_impl_opts g_spdk_posix_sock_impl_opts = {
	.recv_buf_size = MIN_SO_RCVBUF_SIZE,
	.send_buf_size = MIN_SO_SNDBUF_SIZE,
	.enable_recv_pipe = true,
	.enable_zerocopy_send = false,
	.enable_quickack = false,
	.enable_placement_id = false,
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

static int
posix_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
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

enum posix_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
posix_sock_alloc_pipe(struct spdk_posix_sock *sock, int sz)
{
	uint8_t *new_buf;
	struct spdk_pipe *new_pipe;
	struct iovec siov[2];
	struct iovec diov[2];
	int sbytes;
	ssize_t bytes;

	if (sock->recv_buf_sz == sz) {
		return 0;
	}

	/* If the new size is 0, just free the pipe */
	if (sz == 0) {
		spdk_pipe_destroy(sock->recv_pipe);
		free(sock->recv_buf);
		sock->recv_pipe = NULL;
		sock->recv_buf = NULL;
		return 0;
	} else if (sz < MIN_SOCK_PIPE_SIZE) {
		SPDK_ERRLOG("The size of the pipe must be larger than %d\n", MIN_SOCK_PIPE_SIZE);
		return -1;
	}

	/* Round up to next 64 byte multiple */
	new_buf = calloc(SPDK_ALIGN_CEIL(sz + 1, 64), sizeof(uint8_t));
	if (!new_buf) {
		SPDK_ERRLOG("socket recv buf allocation failed\n");
		return -ENOMEM;
	}

	new_pipe = spdk_pipe_create(new_buf, sz + 1);
	if (new_pipe == NULL) {
		SPDK_ERRLOG("socket pipe allocation failed\n");
		free(new_buf);
		return -ENOMEM;
	}

	if (sock->recv_pipe != NULL) {
		/* Pull all of the data out of the old pipe */
		sbytes = spdk_pipe_reader_get_buffer(sock->recv_pipe, sock->recv_buf_sz, siov);
		if (sbytes > sz) {
			/* Too much data to fit into the new pipe size */
			spdk_pipe_destroy(new_pipe);
			free(new_buf);
			return -EINVAL;
		}

		sbytes = spdk_pipe_writer_get_buffer(new_pipe, sz, diov);
		assert(sbytes == sz);

		bytes = spdk_iovcpy(siov, 2, diov, 2);
		spdk_pipe_writer_advance(new_pipe, bytes);

		spdk_pipe_destroy(sock->recv_pipe);
		free(sock->recv_buf);
	}

	sock->recv_buf_sz = sz;
	sock->recv_buf = new_buf;
	sock->recv_pipe = new_pipe;

	return 0;
}

static int
posix_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	assert(sock != NULL);

	if (g_spdk_posix_sock_impl_opts.enable_recv_pipe) {
		rc = posix_sock_alloc_pipe(sock, sz);
		if (rc) {
			return rc;
		}
	}

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE */
	if (sz < MIN_SO_RCVBUF_SIZE) {
		sz = MIN_SO_RCVBUF_SIZE;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static int
posix_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	assert(sock != NULL);

	if (sz < MIN_SO_SNDBUF_SIZE) {
		sz = MIN_SO_SNDBUF_SIZE;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static struct spdk_posix_sock *
posix_sock_alloc(int fd, bool enable_zero_copy)
{
	struct spdk_posix_sock *sock;
#if defined(SPDK_ZEROCOPY) || defined(__linux__)
	int flag;
	int rc;
#endif

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;

#if defined(SPDK_ZEROCOPY)
	flag = 1;

	if (!enable_zero_copy || !g_spdk_posix_sock_impl_opts.enable_zerocopy_send) {
		return sock;
	}

	/* Try to turn on zero copy sends */
	rc = setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
	if (rc == 0) {
		sock->zcopy = true;
	}
#endif

#if defined(__linux__)
	flag = 1;

	if (g_spdk_posix_sock_impl_opts.enable_quickack) {
		rc = setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}
#endif

	return sock;
}

static bool
sock_is_loopback(int fd)
{
	struct ifaddrs *addrs, *tmp;
	struct sockaddr_storage sa = {};
	socklen_t salen;
	struct ifreq ifr = {};
	char ip_addr[256], ip_addr_tmp[256];
	int rc;
	bool is_loopback = false;

	salen = sizeof(sa);
	rc = getsockname(fd, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		return is_loopback;
	}

	memset(ip_addr, 0, sizeof(ip_addr));
	rc = get_addr_str((struct sockaddr *)&sa, ip_addr, sizeof(ip_addr));
	if (rc != 0) {
		return is_loopback;
	}

	getifaddrs(&addrs);
	for (tmp = addrs; tmp != NULL; tmp = tmp->ifa_next) {
		if (tmp->ifa_addr && (tmp->ifa_flags & IFF_UP) &&
		    (tmp->ifa_addr->sa_family == sa.ss_family)) {
			memset(ip_addr_tmp, 0, sizeof(ip_addr_tmp));
			rc = get_addr_str(tmp->ifa_addr, ip_addr_tmp, sizeof(ip_addr_tmp));
			if (rc != 0) {
				continue;
			}

			if (strncmp(ip_addr, ip_addr_tmp, sizeof(ip_addr)) == 0) {
				memcpy(ifr.ifr_name, tmp->ifa_name, sizeof(ifr.ifr_name));
				ioctl(fd, SIOCGIFFLAGS, &ifr);
				if (ifr.ifr_flags & IFF_LOOPBACK) {
					is_loopback = true;
				}
				goto end;
			}
		}
	}

end:
	freeifaddrs(addrs);
	return is_loopback;
}

static struct spdk_sock *
posix_sock_create(const char *ip, int port,
		  enum posix_sock_create_type type,
		  struct spdk_sock_opts *opts)
{
	struct spdk_posix_sock *sock;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int fd, flag;
	int val = 1;
	int rc, sz;
	bool enable_zero_copy = true;

	assert(opts != NULL);

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
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", gai_strerror(rc), rc);
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

		sz = g_spdk_posix_sock_impl_opts.recv_buf_size;
		rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		sz = g_spdk_posix_sock_impl_opts.send_buf_size;
		rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
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

#if defined(SO_PRIORITY)
		if (opts->priority) {
			rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
			if (rc != 0) {
				close(fd);
				/* error */
				continue;
			}
		}
#endif

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

	/* Only enable zero copy for non-loopback sockets. */
	enable_zero_copy = opts->zcopy && !sock_is_loopback(fd);

	sock = posix_sock_alloc(fd, enable_zero_copy);
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(fd);
		return NULL;
	}

	if (opts != NULL) {
		sock->so_priority = opts->priority;
	}
	return &sock->base;
}

static struct spdk_sock *
posix_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return posix_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN, opts);
}

static struct spdk_sock *
posix_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return posix_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT, opts);
}

static struct spdk_sock *
posix_sock_accept(struct spdk_sock *_sock)
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

#if defined(SO_PRIORITY)
	/* The priority is not inherited, so call this function again */
	if (sock->base.opts.priority) {
		rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &sock->base.opts.priority, sizeof(int));
		if (rc != 0) {
			close(fd);
			return NULL;
		}
	}
#endif

	/* Inherit the zero copy feature from the listen socket */
	new_sock = posix_sock_alloc(fd, sock->zcopy);
	if (new_sock == NULL) {
		close(fd);
		return NULL;
	}
	new_sock->so_priority = sock->base.opts.priority;

	return &new_sock->base;
}

static int
posix_sock_close(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	assert(TAILQ_EMPTY(&_sock->pending_reqs));

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	close(sock->fd);

	spdk_pipe_destroy(sock->recv_pipe);
	free(sock->recv_buf);
	free(sock);

	return 0;
}

#ifdef SPDK_ZEROCOPY
static int
_sock_check_zcopy(struct spdk_sock *sock)
{
	struct spdk_posix_sock *psock = __posix_sock(sock);
	struct msghdr msgh = {};
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err)];
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;
	struct spdk_sock_request *req, *treq;
	bool found;

	msgh.msg_control = buf;
	msgh.msg_controllen = sizeof(buf);

	while (true) {
		rc = recvmsg(psock->fd, &msgh, MSG_ERRQUEUE);

		if (rc < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return 0;
			}

			if (!TAILQ_EMPTY(&sock->pending_reqs)) {
				SPDK_ERRLOG("Attempting to receive from ERRQUEUE yielded error, but pending list still has orphaned entries\n");
			} else {
				SPDK_WARNLOG("Recvmsg yielded an error!\n");
			}
			return 0;
		}

		cm = CMSG_FIRSTHDR(&msgh);
		if (!cm || cm->cmsg_level != SOL_IP || cm->cmsg_type != IP_RECVERR) {
			SPDK_WARNLOG("Unexpected cmsg level or type!\n");
			return 0;
		}

		serr = (struct sock_extended_err *)CMSG_DATA(cm);
		if (serr->ee_errno != 0 || serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
			SPDK_WARNLOG("Unexpected extended error origin\n");
			return 0;
		}

		/* Most of the time, the pending_reqs array is in the exact
		 * order we need such that all of the requests to complete are
		 * in order, in the front. It is guaranteed that all requests
		 * belonging to the same sendmsg call are sequential, so once
		 * we encounter one match we can stop looping as soon as a
		 * non-match is found.
		 */
		for (idx = serr->ee_info; idx <= serr->ee_data; idx++) {
			found = false;
			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, internal.link, treq) {
				if (req->internal.offset == idx) {
					found = true;

					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}

				} else if (found) {
					break;
				}
			}

		}
	}

	return 0;
}
#endif

static int
_sock_flush(struct spdk_sock *sock)
{
	struct spdk_posix_sock *psock = __posix_sock(sock);
	struct msghdr msg = {};
	int flags;
	struct iovec iovs[IOV_BATCH_SIZE];
	int iovcnt;
	int retval;
	struct spdk_sock_request *req;
	int i;
	ssize_t rc;
	unsigned int offset;
	size_t len;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (sock->cb_cnt > 0) {
		return 0;
	}

	iovcnt = spdk_sock_prep_reqs(sock, iovs, 0, NULL);

	if (iovcnt == 0) {
		return 0;
	}

	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;
#ifdef SPDK_ZEROCOPY
	if (psock->zcopy) {
		flags = MSG_ZEROCOPY;
	} else
#endif
	{
		flags = 0;
	}
	rc = sendmsg(psock->fd, &msg, flags);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && psock->zcopy)) {
			return 0;
		}
		return rc;
	}

	/* Handling overflow case, because we use psock->sendmsg_idx - 1 for the
	 * req->internal.offset, so sendmsg_idx should not be zero  */
	if (spdk_unlikely(psock->sendmsg_idx == UINT32_MAX)) {
		psock->sendmsg_idx = 1;
	} else {
		psock->sendmsg_idx++;
	}

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		offset = req->internal.offset;

		for (i = 0; i < req->iovcnt; i++) {
			/* Advance by the offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;

			if (len > (size_t)rc) {
				/* This element was partially sent. */
				req->internal.offset += rc;
				return 0;
			}

			offset = 0;
			req->internal.offset += len;
			rc -= len;
		}

		/* Handled a full request. */
		spdk_sock_request_pend(sock, req);

		if (!psock->zcopy) {
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			retval = spdk_sock_request_put(sock, req, 0);
			if (retval) {
				break;
			}
		} else {
			/* Re-use the offset field to hold the sendmsg call index. The
			 * index is 0 based, so subtract one here because we've already
			 * incremented above. */
			req->internal.offset = psock->sendmsg_idx - 1;
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	return 0;
}

static int
posix_sock_flush(struct spdk_sock *_sock)
{
	return _sock_flush(_sock);
}

static ssize_t
posix_sock_recv_from_pipe(struct spdk_posix_sock *sock, struct iovec *diov, int diovcnt)
{
	struct iovec siov[2];
	int sbytes;
	ssize_t bytes;
	struct spdk_posix_sock_group_impl *group;

	sbytes = spdk_pipe_reader_get_buffer(sock->recv_pipe, sock->recv_buf_sz, siov);
	if (sbytes < 0) {
		errno = EINVAL;
		return -1;
	} else if (sbytes == 0) {
		errno = EAGAIN;
		return -1;
	}

	bytes = spdk_iovcpy(siov, 2, diov, diovcnt);

	if (bytes == 0) {
		/* The only way this happens is if diov is 0 length */
		errno = EINVAL;
		return -1;
	}

	spdk_pipe_reader_advance(sock->recv_pipe, bytes);

	/* If we drained the pipe, take it off the level-triggered list */
	if (sock->base.group_impl && spdk_pipe_reader_bytes_available(sock->recv_pipe) == 0) {
		group = __posix_group_impl(sock->base.group_impl);
		TAILQ_REMOVE(&group->pending_recv, sock, link);
		sock->pending_recv = false;
	}

	return bytes;
}

static inline ssize_t
posix_sock_read(struct spdk_posix_sock *sock)
{
	struct iovec iov[2];
	int bytes;
	struct spdk_posix_sock_group_impl *group;

	bytes = spdk_pipe_writer_get_buffer(sock->recv_pipe, sock->recv_buf_sz, iov);

	if (bytes > 0) {
		bytes = readv(sock->fd, iov, 2);
		if (bytes > 0) {
			spdk_pipe_writer_advance(sock->recv_pipe, bytes);
			if (sock->base.group_impl) {
				group = __posix_group_impl(sock->base.group_impl);
				TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
				sock->pending_recv = true;
			}
		}
	}

	return bytes;
}

static ssize_t
posix_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc, i;
	size_t len;

	if (sock->recv_pipe == NULL) {
		return readv(sock->fd, iov, iovcnt);
	}

	len = 0;
	for (i = 0; i < iovcnt; i++) {
		len += iov[i].iov_len;
	}

	if (spdk_pipe_reader_bytes_available(sock->recv_pipe) == 0) {
		/* If the user is receiving a sufficiently large amount of data,
		 * receive directly to their buffers. */
		if (len >= MIN_SOCK_PIPE_SIZE) {
			return readv(sock->fd, iov, iovcnt);
		}

		/* Otherwise, do a big read into our pipe */
		rc = posix_sock_read(sock);
		if (rc <= 0) {
			return rc;
		}
	}

	return posix_sock_recv_from_pipe(sock, iov, iovcnt);
}

static ssize_t
posix_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	struct iovec iov[1];

	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	return posix_sock_readv(sock, iov, 1);
}

static ssize_t
posix_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	/* In order to process a writev, we need to flush any asynchronous writes
	 * first. */
	rc = _sock_flush(_sock);
	if (rc < 0) {
		return rc;
	}

	if (!TAILQ_EMPTY(&_sock->queued_reqs)) {
		/* We weren't able to flush all requests */
		errno = EAGAIN;
		return -1;
	}

	return writev(sock->fd, iov, iovcnt);
}

static void
posix_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	int rc;

	spdk_sock_request_queue(sock, req);

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (sock->queued_iovcnt >= IOV_BATCH_SIZE) {
		rc = _sock_flush(sock);
		if (rc) {
			spdk_sock_abort_requests(sock);
		}
	}
}

static int
posix_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
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

static bool
posix_sock_is_ipv6(struct spdk_sock *_sock)
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
posix_sock_is_ipv4(struct spdk_sock *_sock)
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
posix_sock_is_connected(struct spdk_sock *_sock)
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
posix_sock_get_placement_id(struct spdk_sock *_sock, int *placement_id)
{
	int rc = -1;

	if (!g_spdk_posix_sock_impl_opts.enable_placement_id) {
		return rc;
	}

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
posix_sock_group_impl_create(void)
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
	TAILQ_INIT(&group_impl->pending_recv);

	return &group_impl->base;
}

static int
posix_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

#if defined(__linux__)
	struct epoll_event event;

	memset(&event, 0, sizeof(event));
	/* EPOLLERR is always on even if we don't set it, but be explicit for clarity */
	event.events = EPOLLIN | EPOLLERR;
	event.data.ptr = sock;

	rc = epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);
#elif defined(__FreeBSD__)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_ADD, 0, 0, sock);

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
#endif

	/* switched from another polling group due to scheduling */
	if (spdk_unlikely(sock->recv_pipe != NULL  &&
			  (spdk_pipe_reader_bytes_available(sock->recv_pipe) > 0))) {
		assert(sock->pending_recv == false);
		sock->pending_recv = true;
		TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
	}

	return rc;
}

static int
posix_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	if (sock->recv_pipe != NULL) {
		if (spdk_pipe_reader_bytes_available(sock->recv_pipe) > 0) {
			TAILQ_REMOVE(&group->pending_recv, sock, link);
			sock->pending_recv = false;
		}
		assert(sock->pending_recv == false);
	}

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

	spdk_sock_abort_requests(_sock);

	return rc;
}

static int
posix_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			   struct spdk_sock **socks)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_sock *sock, *tmp;
	int num_events, i, rc;
	struct spdk_posix_sock *psock, *ptmp;
#if defined(__linux__)
	struct epoll_event events[MAX_EVENTS_PER_POLL];
#elif defined(__FreeBSD__)
	struct kevent events[MAX_EVENTS_PER_POLL];
	struct timespec ts = {0};
#endif

	/* This must be a TAILQ_FOREACH_SAFE because while flushing,
	 * a completion callback could remove the sock from the
	 * group. */
	TAILQ_FOREACH_SAFE(sock, &_group->socks, link, tmp) {
		rc = _sock_flush(sock);
		if (rc) {
			spdk_sock_abort_requests(sock);
		}
	}

#if defined(__linux__)
	num_events = epoll_wait(group->fd, events, max_events, 0);
#elif defined(__FreeBSD__)
	num_events = kevent(group->fd, NULL, 0, events, max_events, &ts);
#endif

	if (num_events == -1) {
		return -1;
	} else if (num_events == 0 && !TAILQ_EMPTY(&_group->socks)) {
		uint8_t byte;

		sock = TAILQ_FIRST(&_group->socks);
		psock = __posix_sock(sock);
		/* a recv is done here to busy poll the queue associated with
		 * first socket in list and potentially reap incoming data.
		 */
		if (psock->so_priority) {
			recv(psock->fd, &byte, 1, MSG_PEEK);
		}
	}

	for (i = 0; i < num_events; i++) {
#if defined(__linux__)
		sock = events[i].data.ptr;
		psock = __posix_sock(sock);

#ifdef SPDK_ZEROCOPY
		if (events[i].events & EPOLLERR) {
			rc = _sock_check_zcopy(sock);
			/* If the socket was closed or removed from
			 * the group in response to a send ack, don't
			 * add it to the array here. */
			if (rc || sock->cb_fn == NULL) {
				continue;
			}
		}
#endif
		if ((events[i].events & EPOLLIN) == 0) {
			continue;
		}

#elif defined(__FreeBSD__)
		sock = events[i].udata;
		psock = __posix_sock(sock);
#endif

		/* If the socket does not already have recv pending, add it now */
		if (!psock->pending_recv) {
			psock->pending_recv = true;
			TAILQ_INSERT_TAIL(&group->pending_recv, psock, link);
		}
	}

	num_events = 0;

	TAILQ_FOREACH_SAFE(psock, &group->pending_recv, link, ptmp) {
		if (num_events == max_events) {
			break;
		}

		socks[num_events++] = &psock->base;
	}

	/* Cycle the pending_recv list so that each time we poll things aren't
	 * in the same order. */
	for (i = 0; i < num_events; i++) {
		psock = __posix_sock(socks[i]);

		TAILQ_REMOVE(&group->pending_recv, psock, link);

		if (psock->recv_pipe == NULL || spdk_pipe_reader_bytes_available(psock->recv_pipe) == 0) {
			psock->pending_recv = false;
		} else {
			TAILQ_INSERT_TAIL(&group->pending_recv, psock, link);
		}

	}

	return num_events;
}

static int
posix_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	int rc;

	rc = close(group->fd);
	free(group);
	return rc;
}

static int
posix_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}
	memset(opts, 0, *len);

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= *len

#define GET_FIELD(field) \
	if (FIELD_OK(field)) { \
		opts->field = g_spdk_posix_sock_impl_opts.field; \
	}

	GET_FIELD(recv_buf_size);
	GET_FIELD(send_buf_size);
	GET_FIELD(enable_recv_pipe);
	GET_FIELD(enable_zerocopy_send);
	GET_FIELD(enable_quickack);
	GET_FIELD(enable_placement_id);

#undef GET_FIELD
#undef FIELD_OK

	*len = spdk_min(*len, sizeof(g_spdk_posix_sock_impl_opts));
	return 0;
}

static int
posix_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= len

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		g_spdk_posix_sock_impl_opts.field = opts->field; \
	}

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_zerocopy_send);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);

#undef SET_FIELD
#undef FIELD_OK

	return 0;
}


static struct spdk_net_impl g_posix_net_impl = {
	.name		= "posix",
	.getaddr	= posix_sock_getaddr,
	.connect	= posix_sock_connect,
	.listen		= posix_sock_listen,
	.accept		= posix_sock_accept,
	.close		= posix_sock_close,
	.recv		= posix_sock_recv,
	.readv		= posix_sock_readv,
	.writev		= posix_sock_writev,
	.writev_async	= posix_sock_writev_async,
	.flush		= posix_sock_flush,
	.set_recvlowat	= posix_sock_set_recvlowat,
	.set_recvbuf	= posix_sock_set_recvbuf,
	.set_sendbuf	= posix_sock_set_sendbuf,
	.is_ipv6	= posix_sock_is_ipv6,
	.is_ipv4	= posix_sock_is_ipv4,
	.is_connected	= posix_sock_is_connected,
	.get_placement_id	= posix_sock_get_placement_id,
	.group_impl_create	= posix_sock_group_impl_create,
	.group_impl_add_sock	= posix_sock_group_impl_add_sock,
	.group_impl_remove_sock = posix_sock_group_impl_remove_sock,
	.group_impl_poll	= posix_sock_group_impl_poll,
	.group_impl_close	= posix_sock_group_impl_close,
	.get_opts	= posix_sock_impl_get_opts,
	.set_opts	= posix_sock_impl_set_opts,
};

SPDK_NET_IMPL_REGISTER(posix, &g_posix_net_impl, DEFAULT_SOCK_PRIORITY);
