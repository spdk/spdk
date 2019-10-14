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
#include "spdk/config.h"

#include <sys/epoll.h>
#include <liburing.h>

#include "spdk/barrier.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/sock.h"
#include "spdk_internal/assert.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define SO_RCVBUF_SIZE (2 * 1024 * 1024)
#define SO_SNDBUF_SIZE (2 * 1024 * 1024)
#define SPDK_SOCK_GROUP_QUEUE_DEPTH 4096
#define IOV_BATCH_SIZE 64

enum spdk_sock_task_type {
	SPDK_SOCK_TASK_POLLIN = 0,
	SPDK_SOCK_TASK_WRITE,
};

enum spdk_uring_sock_task_status {
	SPDK_URING_SOCK_TASK_NOT_IN_USE = 0,
	SPDK_URING_SOCK_TASK_IN_PROCESS,
};

struct spdk_uring_task {
	enum spdk_uring_sock_task_status	status;
	enum spdk_sock_task_type		type;
	struct spdk_uring_sock			*sock;
	struct msghdr				msg;
	struct iovec				iovs[IOV_BATCH_SIZE];
	int					iov_cnt;
	struct spdk_sock_request		*last_req;
	STAILQ_ENTRY(spdk_uring_task)		link;
};

struct spdk_uring_sock {
	struct spdk_sock			base;
	int					fd;
	struct spdk_uring_sock_group_impl	*group;
	struct spdk_uring_task			write_task;
	struct spdk_uring_task			pollin_task;
	int					outstanding_io;
};

struct spdk_uring_sock_group_impl {
	struct spdk_sock_group_impl		base;
	struct io_uring				uring;
	uint32_t				io_inflight;
	uint32_t				io_queued;
	uint32_t				io_avail;
};

#define SPDK_URING_SOCK_REQUEST_IOV(req) ((struct iovec *)((uint8_t *)req + sizeof(struct spdk_sock_request)))

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

#define __uring_sock(sock) (struct spdk_uring_sock *)sock
#define __uring_group_impl(group) (struct spdk_uring_sock_group_impl *)group

static int
spdk_uring_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
			char *caddr, int clen, uint16_t *cport)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
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

enum spdk_uring_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
spdk_uring_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
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
spdk_uring_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
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

static struct spdk_uring_sock *
_spdk_uring_sock_alloc(int fd)
{
	struct spdk_uring_sock *sock;

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;

	return sock;
}

static struct spdk_sock *
spdk_uring_sock_create(const char *ip, int port, enum spdk_uring_sock_create_type type)
{
	struct spdk_uring_sock *sock;
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

		val = SO_RCVBUF_SIZE;
		rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof val);
		if (rc) {
			/* Not fatal */
		}

		val = SO_SNDBUF_SIZE;
		rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof val);
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

	sock = _spdk_uring_sock_alloc(fd);
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(fd);
		return NULL;
	}

	return &sock->base;
}

static struct spdk_sock *
spdk_uring_sock_listen(const char *ip, int port)
{
	return spdk_uring_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN);
}

static struct spdk_sock *
spdk_uring_sock_connect(const char *ip, int port)
{
	return spdk_uring_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT);
}

static struct spdk_sock *
spdk_uring_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_uring_sock		*sock = __uring_sock(_sock);
	struct sockaddr_storage		sa;
	socklen_t			salen;
	int				rc, fd;
	struct spdk_uring_sock		*new_sock;
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

	new_sock = _spdk_uring_sock_alloc(fd);
	if (new_sock == NULL) {
		close(fd);
		return NULL;
	}

	return &new_sock->base;
}

static int
spdk_uring_sock_close(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int rc;

	/* defer the socket close if there is outstanding I/O */
	if (sock->outstanding_io) {
		return 0;
	}

	assert(TAILQ_EMPTY(&_sock->pending_reqs));
	assert(sock->group == NULL);
	rc = close(sock->fd);
	if (rc == 0) {
		free(sock);
	}

	return rc;
}

static ssize_t
spdk_uring_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);

	return recv(sock->fd, buf, len, MSG_DONTWAIT);
}

static ssize_t
spdk_uring_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);

	return readv(sock->fd, iov, iovcnt);
}

static ssize_t
spdk_uring_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);

	if (sock->write_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		errno = EAGAIN;
		return -1;
	}

	return writev(sock->fd, iov, iovcnt);
}

static int
spdk_sock_prep_reqs(struct spdk_sock *_sock, struct iovec *iovs, int index,
		    struct spdk_sock_request **last_req)
{
	int iovcnt, i;
	struct spdk_sock_request *req;
	unsigned int offset;

	/* Gather an iov */
	iovcnt = index;
	if (spdk_unlikely(iovcnt >= IOV_BATCH_SIZE)) {
		goto end;
	}

	if (last_req != NULL && *last_req != NULL) {
		req = TAILQ_NEXT(*last_req, internal.link);
	} else {
		req = TAILQ_FIRST(&_sock->queued_reqs);
	}

	while (req) {
		offset = req->internal.offset;

		for (i = 0; i < req->iovcnt; i++) {
			/* Consume any offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			iovs[iovcnt].iov_base = SPDK_SOCK_REQUEST_IOV(req, i)->iov_base + offset;
			iovs[iovcnt].iov_len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;
			iovcnt++;

			offset = 0;

			if (iovcnt >= IOV_BATCH_SIZE) {
				break;
			}
		}
		if (iovcnt >= IOV_BATCH_SIZE) {
			break;
		}

		if (last_req != NULL) {
			*last_req = req;
		}
		req = TAILQ_NEXT(req, internal.link);
	}

end:
	return iovcnt;
}

static int
spdk_sock_complete_reqs(struct spdk_sock *_sock, ssize_t rc)
{
	struct spdk_sock_request *req;
	int i, retval;
	unsigned int offset;
	size_t len;

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&_sock->queued_reqs);
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
		spdk_sock_request_pend(_sock, req);

		retval = spdk_sock_request_put(_sock, req, 0);
		if (retval) {
			return retval;
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&_sock->queued_reqs);
	}

	return 0;
}

static void
_sock_flush(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_task *task = &sock->write_task;
	uint32_t iovcnt;
	struct io_uring_sqe *sqe;

	if (task->status == SPDK_URING_SOCK_TASK_IN_PROCESS) {
		return;
	}

	iovcnt = spdk_sock_prep_reqs(&sock->base, task->iovs, task->iov_cnt, &task->last_req);
	if (!iovcnt) {
		return;
	}

	task->iov_cnt = iovcnt;
	assert(sock->group != NULL);
	task->msg.msg_iov = task->iovs;
	task->msg.msg_iovlen = task->iov_cnt;

	sock->group->io_queued++;

	sqe = io_uring_get_sqe(&sock->group->uring);
	io_uring_prep_sendmsg(sqe, sock->fd, &sock->write_task.msg, 0);
	io_uring_sqe_set_data(sqe, task);
	task->status = SPDK_URING_SOCK_TASK_IN_PROCESS;
}

static void
_sock_prep_pollin(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_task *task = &sock->pollin_task;
	struct io_uring_sqe *sqe;

	if (task->status == SPDK_URING_SOCK_TASK_IN_PROCESS) {
		return;
	}

	assert(sock->group != NULL);
	sock->group->io_queued++;

	sqe = io_uring_get_sqe(&sock->group->uring);
	io_uring_prep_poll_add(sqe, sock->fd, POLLIN);
	io_uring_sqe_set_data(sqe, task);
	task->status = SPDK_URING_SOCK_TASK_IN_PROCESS;
}

static int
spdk_sock_uring_group_reap(struct io_uring *ring, int max,
			   struct spdk_sock **socks)
{
	int i, count, ret;
	struct io_uring_cqe *cqe;
	struct spdk_uring_sock *sock;
	struct spdk_uring_task *task;
	int status;

	count = 0;
	for (i = 0; i < max; i++) {
		ret = io_uring_peek_cqe(ring, &cqe);
		if (ret != 0) {
			break;
		}

		if (cqe == NULL) {
			break;
		}

		task = (struct spdk_uring_task *)cqe->user_data;
		assert(task != NULL);
		sock = task->sock;
		assert(sock != NULL);
		assert(sock->group != NULL);
		sock->group->io_inflight--;
		sock->group->io_avail++;
		status = cqe->res;
		io_uring_cqe_seen(ring, cqe);

		task->status = SPDK_URING_SOCK_TASK_NOT_IN_USE;

		if (spdk_unlikely(status <= 0)) {
			if (status == -EAGAIN || status == -EWOULDBLOCK) {
				continue;
			}
		}

		switch (task->type) {
		case SPDK_SOCK_TASK_POLLIN:
			if ((status & POLLIN) == POLLIN) {
				if ((socks != NULL) && (sock->base.cb_fn != NULL)) {
					socks[count] = &sock->base;
					count++;
				}
			} else {
				SPDK_UNREACHABLE();
			}
			break;
		case SPDK_SOCK_TASK_WRITE:
			assert(TAILQ_EMPTY(&sock->base.pending_reqs));
			task->last_req = NULL;
			task->iov_cnt = 0;
			spdk_sock_complete_reqs(&sock->base, status);

			/* For socket is removed from the group but having outstanding I/O */
			if (spdk_unlikely(task->sock->outstanding_io > 0 &&
					  TAILQ_EMPTY(&sock->base.pending_reqs))) {
				if (--sock->outstanding_io == 0) {
					sock->group = NULL;
					/* Just for sock close case */
					if (sock->base.flags.closed) {
						spdk_uring_sock_close(&sock->base);
					}
				}
			}

			break;
		default:
			SPDK_UNREACHABLE();
		}
	}

	return count;
}

static int
_sock_flush_client(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct msghdr msg = {};
	struct iovec iovs[IOV_BATCH_SIZE];
	int iovcnt;
	ssize_t rc;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (_sock->cb_cnt > 0) {
		return 0;
	}

	/* Gather an iov */
	iovcnt = spdk_sock_prep_reqs(_sock, iovs, 0, NULL);
	if (iovcnt == 0) {
		return 0;
	}

	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;
	rc = sendmsg(sock->fd, &msg, 0);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		return rc;
	}

	spdk_sock_complete_reqs(_sock, rc);

	return 0;
}

static void
spdk_uring_sock_writev_async(struct spdk_sock *_sock, struct spdk_sock_request *req)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int rc;

	spdk_sock_request_queue(_sock, req);

	if (!sock->group) {
		if (_sock->queued_iovcnt >= IOV_BATCH_SIZE) {
			rc = _sock_flush_client(_sock);
			if (rc) {
				spdk_sock_abort_requests(_sock);
			}
		}
	}
}

static int
spdk_uring_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
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
spdk_uring_sock_set_priority(struct spdk_sock *_sock, int priority)
{
	int rc = 0;

#if defined(SO_PRIORITY)
	struct spdk_uring_sock *sock = __uring_sock(_sock);

	assert(sock != NULL);

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_PRIORITY,
			&priority, sizeof(priority));
#endif
	return rc;
}

static bool
spdk_uring_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
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
spdk_uring_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
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
spdk_uring_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
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
spdk_uring_sock_get_placement_id(struct spdk_sock *_sock, int *placement_id)
{
	int rc = -1;

#if defined(SO_INCOMING_NAPI_ID)
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	socklen_t salen = sizeof(int);

	rc = getsockopt(sock->fd, SOL_SOCKET, SO_INCOMING_NAPI_ID, placement_id, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockopt() failed (errno=%d)\n", errno);
	}

#endif
	return rc;
}

static struct spdk_sock_group_impl *
spdk_uring_sock_group_impl_create(void)
{
	struct spdk_uring_sock_group_impl *group_impl;

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		return NULL;
	}

	group_impl->io_avail = SPDK_SOCK_GROUP_QUEUE_DEPTH;

	if (io_uring_queue_init(SPDK_SOCK_GROUP_QUEUE_DEPTH, &group_impl->uring, 0) < 0) {
		SPDK_ERRLOG("uring I/O context setup failure\n");
		free(group_impl);
		return NULL;
	}

	return &group_impl->base;
}

static int
spdk_uring_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group,
				    struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);

	sock->group = group;
	sock->write_task.sock = sock;
	sock->write_task.type = SPDK_SOCK_TASK_WRITE;

	sock->pollin_task.sock = sock;
	sock->pollin_task.type = SPDK_SOCK_TASK_POLLIN;

	return 0;
}

static int
spdk_uring_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group,
				       struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);

	if (sock->write_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		sock->outstanding_io++;
	}

	if (sock->pollin_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		sock->outstanding_io++;
	}

	if (!sock->outstanding_io) {
		sock->group = NULL;
	}

	return 0;
}

static int
spdk_uring_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
				struct spdk_sock **socks)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);
	int count, ret;
	int to_complete, to_submit;
	struct spdk_sock *_sock, *tmp;

	TAILQ_FOREACH_SAFE(_sock, &group->base.socks, link, tmp) {
		_sock_flush(_sock);
		_sock_prep_pollin(_sock);
	}

	to_submit = group->io_queued;

	/* For network I/O, it cannot be set with O_DIRECT, so we do not need to call spdk_io_uring_enter */
	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call io_uring_enter appropriately. */
		ret = io_uring_submit(&group->uring);
		if (ret < 0) {
			return 1;
		}
		group->io_queued = 0;
		group->io_inflight += to_submit;
		group->io_avail -= to_submit;
	}

	count = 0;
	to_complete = group->io_inflight;
	if (to_complete > 0) {
		to_complete = spdk_min(to_complete, max_events);
		count = spdk_sock_uring_group_reap(&group->uring, to_complete, socks);
	}

	return count;
}

static int
spdk_uring_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);

	/* try to reap all the active I/O */
	while (group->io_inflight) {
		spdk_uring_sock_group_impl_poll(_group, 32, NULL);
	}
	assert(group->io_inflight == 0);
	assert(group->io_avail == SPDK_SOCK_GROUP_QUEUE_DEPTH);

	close(group->uring.ring_fd);
	io_uring_queue_exit(&group->uring);

	free(group);
	return 0;
}

static int
spdk_uring_sock_flush(struct spdk_sock *_sock)
{
	return _sock_flush_client(_sock);
}

static struct spdk_net_impl g_uring_net_impl = {
	.name		= "uring",
	.getaddr	= spdk_uring_sock_getaddr,
	.connect	= spdk_uring_sock_connect,
	.listen		= spdk_uring_sock_listen,
	.accept		= spdk_uring_sock_accept,
	.close		= spdk_uring_sock_close,
	.recv		= spdk_uring_sock_recv,
	.readv		= spdk_uring_sock_readv,
	.writev		= spdk_uring_sock_writev,
	.writev_async	= spdk_uring_sock_writev_async,
	.flush          = spdk_uring_sock_flush,
	.set_recvlowat	= spdk_uring_sock_set_recvlowat,
	.set_recvbuf	= spdk_uring_sock_set_recvbuf,
	.set_sendbuf	= spdk_uring_sock_set_sendbuf,
	.set_priority	= spdk_uring_sock_set_priority,
	.is_ipv6	= spdk_uring_sock_is_ipv6,
	.is_ipv4	= spdk_uring_sock_is_ipv4,
	.is_connected   = spdk_uring_sock_is_connected,
	.get_placement_id	= spdk_uring_sock_get_placement_id,
	.group_impl_create	= spdk_uring_sock_group_impl_create,
	.group_impl_add_sock	= spdk_uring_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_uring_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_uring_sock_group_impl_poll,
	.group_impl_close	= spdk_uring_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(uring, &g_uring_net_impl, DEFAULT_SOCK_PRIORITY + 1);
