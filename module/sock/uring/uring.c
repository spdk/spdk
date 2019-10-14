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

#include <sys/epoll.h>
#include <liburing.h>

#include "spdk/barrier.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/sock.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define SO_RCVBUF_SIZE (2 * 1024 * 1024)
#define SO_SNDBUF_SIZE (2 * 1024 * 1024)
#define SPDK_SOCK_GROUP_QUEUE_DEPTH 4096
#define SPDK_SOCK_QUEUE_DEPTH 128
#define SPDK_USE_MSG 0

enum spdk_sock_io_type {
	SPDK_SOCK_IO_UNUSED = 0,
	SPDK_SOCK_IO_READ,
	SPDK_SOCK_IO_WRITE,
};

struct spdk_uring_sock_task {
	spdk_sock_op_cb cb_fn;
	void *cb_arg;
	enum spdk_sock_io_type type;
	struct spdk_uring_sock *sock;
	struct msghdr msg;
	struct iovec iov[32];
	int iov_cnt;
	size_t len;
	size_t completed_len;
	size_t orig_len;
	STAILQ_ENTRY(spdk_uring_sock_task) link;

};

struct spdk_uring_sock {
	struct spdk_sock			base;
	int					fd;
	struct spdk_uring_sock_group_impl	*group;
	STAILQ_HEAD(, spdk_uring_sock_task)     task_list;
	struct spdk_uring_sock_task		tasks[SPDK_SOCK_QUEUE_DEPTH];
	uint32_t				avail_task_num;
};

struct spdk_uring_sock_group_impl {
	struct spdk_sock_group_impl		base;
	int					fd;
	struct io_uring				uring;
	STAILQ_HEAD(, spdk_uring_sock_task)	task_list;
	uint32_t				io_inflight;
	uint32_t				io_pending;
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

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(fd);
		return NULL;
	}

	sock->fd = fd;

	rc = spdk_uring_sock_set_recvbuf(&sock->base, SO_RCVBUF_SIZE);
	if (rc) {
		/* Not fatal */
	}

	rc = spdk_uring_sock_set_sendbuf(&sock->base, SO_SNDBUF_SIZE);
	if (rc) {
		/* Not fatal */
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

	new_sock = calloc(1, sizeof(*sock));
	if (new_sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(fd);
		return NULL;
	}

	new_sock->fd = fd;

	rc = spdk_uring_sock_set_recvbuf(&new_sock->base, SO_RCVBUF_SIZE);
	if (rc) {
		/* Not fatal */
	}

	rc = spdk_uring_sock_set_sendbuf(&new_sock->base, SO_SNDBUF_SIZE);
	if (rc) {
		/* Not fatal */
	}

	return &new_sock->base;
}

static int
spdk_uring_sock_close(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int rc;

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

	return writev(sock->fd, iov, iovcnt);
}

static void
spdk_uring_init_iov(struct spdk_uring_sock_task *task, struct iovec *iov, int iovcnt,
		    size_t len)
{
	int i, j;

	j = 0;
	task->len = 0;
	for (i = 0; i < iovcnt; i++) {
		if (len < iov[i].iov_len) {
			task->iov[j].iov_base = iov[i].iov_base + len;
			task->iov[j].iov_len = iov[i].iov_len - len;
			if (!len) {
				len = 0;
			}
			task->len += task->iov[j].iov_len;
			j++;
		} else {
			len -= iov[i].iov_len;
		}
	}

	task->iov_cnt = j;
	task->msg.msg_iov = task->iov;
	task->msg.msg_iovlen = task->iov_cnt;
	assert(task->type != SPDK_SOCK_IO_UNUSED);
}

static struct spdk_uring_sock_task *
spdk_uring_sock_allocate_and_queue_task(struct spdk_uring_sock *sock,  spdk_sock_op_cb cb_fn,
					void *cb_arg, enum spdk_sock_io_type type)
{

	struct spdk_uring_sock_task *task;

	if (spdk_unlikely(STAILQ_EMPTY(&sock->task_list))) {
		SPDK_ERRLOG("No resource to allocate task from sock=%p\n", sock);
		return NULL;
	}

	task = STAILQ_FIRST(&sock->task_list);
	STAILQ_REMOVE(&sock->task_list, task, spdk_uring_sock_task, link);
	assert(sock->group != NULL);
	assert(task->type == false);
	task->cb_fn = cb_fn;
	task->cb_arg = cb_arg;
	task->len = 0;
	task->completed_len = 0;
	task->type = type;
	STAILQ_INSERT_TAIL(&sock->group->task_list, task, link);
	assert(sock->avail_task_num > 0);
	sock->avail_task_num--;

	return task;
}

static ssize_t
spdk_uring_sock_recv_async(struct spdk_sock *_sock, void *buf, size_t len, spdk_sock_op_cb cb_fn,
			   void *cb_arg)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct io_uring_sqe *sqe;
	int rc;

	if (spdk_unlikely(cb_fn == NULL)) {
		SPDK_ERRLOG("call back function need to be provided\n");
		return -1;
	}

	if (sock->group) {
		struct spdk_uring_sock_task *task;

		task = spdk_uring_sock_allocate_and_queue_task(sock, cb_fn, cb_arg, SPDK_SOCK_IO_READ);
		if (spdk_unlikely(!task)) {
			SPDK_ERRLOG("No resource to allocate task from sock=%p\n", sock);
			return -ENOMEM;
		}

		sqe = io_uring_get_sqe(&sock->group->uring);
		task->iov[0].iov_base = buf;
		task->iov[0].iov_len = len;
#if SPDK_USE_MSG
		io_uring_prep_recvmsg(sqe, sock->fd, &task->msg, 0);
#else
		io_uring_prep_readv(sqe, sock->fd, task->iov, task->iov_cnt, 0);
#endif
		io_uring_sqe_set_data(sqe, task);
		sock->group->io_pending++;
		task->orig_len = task->len;

	} else {
		rc = recv(sock->fd, buf, len, MSG_DONTWAIT);
		cb_fn(cb_arg, rc);
	}
	return 0;
}

static ssize_t
spdk_uring_sock_readv_async(struct spdk_sock *_sock, struct iovec *iov, int iovcnt,
			    spdk_sock_op_cb cb_fn,
			    void *cb_arg)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct io_uring_sqe *sqe;
	int rc;

	if (spdk_unlikely(cb_fn == NULL)) {
		SPDK_ERRLOG("call back function need to be provided\n");
		return -1;
	}

	if (sock->group) {
		struct spdk_uring_sock_task *task;

		task = spdk_uring_sock_allocate_and_queue_task(sock, cb_fn, cb_arg, SPDK_SOCK_IO_READ);
		if (spdk_unlikely(!task)) {
			SPDK_ERRLOG("No resource to allocate task from sock=%p\n", sock);
			return -ENOMEM;
		}

		spdk_uring_init_iov(task, iov, iovcnt, 0);
		sqe = io_uring_get_sqe(&sock->group->uring);
#if SPDK_USE_MSG
		io_uring_prep_recvmsg(sqe, sock->fd, &task->msg, 0);
#else
		io_uring_prep_readv(sqe, sock->fd, iov, iovcnt, 0);
#endif

		io_uring_sqe_set_data(sqe, task);
		sock->group->io_pending++;
		task->orig_len = task->len;
		sock->group->io_pending++;

	} else {
		rc = readv(sock->fd, iov, iovcnt);
		cb_fn(cb_arg, rc);
	}

	return 0;
}

static ssize_t
spdk_uring_sock_writev_async(struct spdk_sock *_sock, struct iovec *iov, int iovcnt,
			     spdk_sock_op_cb cb_fn, void *cb_arg)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct io_uring_sqe *sqe;
	int rc;

	if (spdk_unlikely(cb_fn == NULL)) {
		SPDK_ERRLOG("call back function need to be provided\n");
		return -1;
	}

	if (sock->group) {
		struct spdk_uring_sock_task *task;

		if (spdk_unlikely(STAILQ_EMPTY(&sock->task_list))) {
			SPDK_ERRLOG("No resource to allocate task from sock=%p\n", sock);
			return -ENOMEM;
		}

		task = spdk_uring_sock_allocate_and_queue_task(sock, cb_fn, cb_arg, SPDK_SOCK_IO_WRITE);
		if (spdk_unlikely(!task)) {
			SPDK_ERRLOG("No resource to allocate task from sock=%p\n", sock);
			return -ENOMEM;
		}


		spdk_uring_init_iov(task, iov, iovcnt, 0);
		sqe = io_uring_get_sqe(&sock->group->uring);
#if SPDK_USE_MSG
		io_uring_prep_sendmsg(sqe, sock->fd, &task->msg, 0);
#else
		io_uring_prep_writev(sqe, sock->fd, iov, iovcnt, 0);
#endif

		io_uring_sqe_set_data(sqe, task);
		task->orig_len = task->len;
		sock->group->io_pending++;

	} else {
		rc = writev(sock->fd, iov, iovcnt);
		cb_fn(cb_arg, rc);
	}

	return 0;
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
	int fd;

	fd = epoll_create1(0);
	if (fd == -1) {
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		close(fd);
		return NULL;
	}

	STAILQ_INIT(&group_impl->task_list);
	if (io_uring_queue_init(SPDK_SOCK_GROUP_QUEUE_DEPTH, &group_impl->uring, 0) < 0) {
		SPDK_ERRLOG("uring I/O context setup failure\n");
		close(fd);
		return NULL;
	}

	group_impl->fd = fd;

	return &group_impl->base;
}

static int
spdk_uring_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int rc;

	struct epoll_event event;
	int i;

	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN;
	event.data.ptr = sock;

	rc = epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);

	sock->group = group;
	STAILQ_INIT(&sock->task_list);
	for (i = 0; i < SPDK_SOCK_QUEUE_DEPTH; i++) {
		sock->tasks[i].sock = sock;
		STAILQ_INSERT_TAIL(&sock->task_list, &sock->tasks[i], link);
		sock->tasks[i].msg.msg_namelen = sizeof(struct sockaddr_in);
	}

	sock->avail_task_num = SPDK_SOCK_QUEUE_DEPTH;

	return rc;
}

static int
spdk_uring_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int rc;
	struct epoll_event event;
	struct spdk_uring_sock_task *task;

	STAILQ_FOREACH(task, &group->task_list, link) {
		if (task->sock == sock) {
			STAILQ_REMOVE(&group->task_list, task, spdk_uring_sock_task, link);
			STAILQ_INSERT_TAIL(&sock->task_list, task, link);
			task->cb_fn(task->cb_arg, -1);
			sock->avail_task_num++;
			group->io_inflight--;
		}
	}

	assert(sock->avail_task_num == SPDK_SOCK_QUEUE_DEPTH);
	sock->group = NULL;
	/* Event parameter is ignored but some old kernel version still require it. */
	rc = epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event);
	return rc;
}

static int
spdk_sock_uring_group_reap(struct io_uring *ring, int max)
{
	int i, count, ret;
	struct io_uring_cqe *cqe;
	struct spdk_uring_sock_task *uring_task;
	int status;

	count = 0;
	for (i = 0; i < max; i++) {
		ret = io_uring_peek_cqe(ring, &cqe);
		if (ret != 0) {
			return ret;
		}

		if (cqe == NULL) {
			return count;
		}

		uring_task = (struct spdk_uring_sock_task *)cqe->user_data;
		assert(uring_task->sock != NULL);
		assert(uring_task->sock->group != NULL);
		uring_task->sock->group->io_inflight--;
		status = cqe->res;
		io_uring_cqe_seen(ring, cqe);
		uring_task->type = SPDK_SOCK_IO_UNUSED;
		STAILQ_REMOVE(&uring_task->sock->group->task_list, uring_task, spdk_uring_sock_task, link);
		STAILQ_INSERT_TAIL(&uring_task->sock->task_list, uring_task, link);
		uring_task->sock->avail_task_num++;
		uring_task->cb_fn(uring_task->cb_arg, status);
		count++;
	}

	return count;
}

static int
spdk_uring_group_impl_io_poll(struct spdk_uring_sock_group_impl *group)
{
	int to_complete, to_submit;
	int count, ret;

	to_submit = group->io_pending;
	to_complete = group->io_inflight;

	ret = 0;
	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call io_uring_enter appropriately. */
		ret = io_uring_submit(&group->uring);
		group->io_pending = 0;
		group->io_inflight += to_submit;
	} else if (to_complete > 0) {
		/* If there are I/O in flight but none to submit, we need to
		 * call io_uring_enter ourselves. */
		ret = io_uring_enter(group->uring.ring_fd, 0, 0,
				     IORING_ENTER_GETEVENTS, NULL);
	}

	if (ret < 0) {
		return 1;
	}

	count = 0;
	if (to_complete > 0) {
		count = spdk_sock_uring_group_reap(&group->uring, to_complete);
	}

	return (count + to_submit);
}

static int
spdk_uring_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
				struct spdk_sock **socks)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);
	int num_events, i;

	struct epoll_event events[MAX_EVENTS_PER_POLL];

	num_events = epoll_wait(group->fd, events, max_events, 0);
	if (num_events == -1) {
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		socks[i] = events[i].data.ptr;
	}

	spdk_uring_group_impl_io_poll(group);
	return num_events;
}

static int
spdk_uring_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);
	int rc;

	assert(group->io_inflight == 0);
	close(group->uring.ring_fd);
	io_uring_queue_exit(&group->uring);

	rc = close(group->fd);
	free(group);
	return rc;
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
	.recv_async     = spdk_uring_sock_recv_async,
	.readv_async	= spdk_uring_sock_readv_async,
	.writev_async	= spdk_uring_sock_writev_async,
	.set_recvlowat	= spdk_uring_sock_set_recvlowat,
	.set_recvbuf	= spdk_uring_sock_set_recvbuf,
	.set_sendbuf	= spdk_uring_sock_set_sendbuf,
	.set_priority	= spdk_uring_sock_set_priority,
	.is_ipv6	= spdk_uring_sock_is_ipv6,
	.is_ipv4	= spdk_uring_sock_is_ipv4,
	.get_placement_id	= spdk_uring_sock_get_placement_id,
	.group_impl_create	= spdk_uring_sock_group_impl_create,
	.group_impl_add_sock	= spdk_uring_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_uring_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_uring_sock_group_impl_poll,
	.group_impl_close	= spdk_uring_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(uring, &g_uring_net_impl);
