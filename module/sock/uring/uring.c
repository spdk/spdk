/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/config.h"

#include <linux/errqueue.h>
#include <sys/epoll.h>
#include <liburing.h>

#include "spdk/barrier.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/pipe.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/sock.h"
#include "spdk_internal/assert.h"
#include "../sock_kernel.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define SPDK_SOCK_GROUP_QUEUE_DEPTH 4096
#define SPDK_SOCK_CMG_INFO_SIZE (sizeof(struct cmsghdr) + sizeof(struct sock_extended_err))

enum uring_task_type {
	URING_TASK_READ = 0,
	URING_TASK_ERRQUEUE,
	URING_TASK_WRITE,
	URING_TASK_CANCEL,
};

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY)
#define SPDK_ZEROCOPY
#endif

/* We don't know how big the buffers that the user posts will be, but this
 * is the maximum we'll ever allow it to receive in a single command.
 * If the user buffers are smaller, it will just receive less. */
#define URING_MAX_RECV_SIZE (128 * 1024)

/* We don't know how many buffers the user will post, but this is the
 * maximum number we'll take from the pool to post per group. */
#define URING_BUF_POOL_SIZE 128

/* We use 1 just so it's not zero and we can validate it's right. */
#define URING_BUF_GROUP_ID 1

enum spdk_uring_sock_task_status {
	SPDK_URING_SOCK_TASK_NOT_IN_USE = 0,
	SPDK_URING_SOCK_TASK_IN_PROCESS,
};

struct spdk_uring_task {
	enum spdk_uring_sock_task_status	status;
	enum uring_task_type		type;
	struct spdk_uring_sock			*sock;
	struct msghdr				msg;
	struct iovec				iovs[IOV_BATCH_SIZE];
	int					iov_cnt;
	struct spdk_sock_request		*last_req;
	bool					is_zcopy;
	STAILQ_ENTRY(spdk_uring_task)		link;
};

struct spdk_uring_sock {
	struct spdk_sock			base;
	int					fd;
	uint32_t				sendmsg_idx;
	struct spdk_uring_sock_group_impl	*group;
	STAILQ_HEAD(, spdk_uring_buf_tracker)	recv_stream;
	size_t					recv_offset;
	struct spdk_uring_task			write_task;
	struct spdk_uring_task			errqueue_task;
	struct spdk_uring_task			read_task;
	struct spdk_uring_task			cancel_task;
	struct spdk_pipe			*recv_pipe;
	void					*recv_buf;
	int					recv_buf_sz;
	bool					zcopy;
	bool					pending_recv;
	bool					pending_group_remove;
	int					zcopy_send_flags;
	int					connection_status;
	int					placement_id;
	uint8_t                                 reserved[4];
	uint8_t					buf[SPDK_SOCK_CMG_INFO_SIZE];
	TAILQ_ENTRY(spdk_uring_sock)		link;
};
/* 'struct cmsghdr' is mapped to the buffer 'buf', and while first element
 * of this control message header has a size of 8 bytes, 'buf'
 * must be 8-byte aligned.
 */
SPDK_STATIC_ASSERT(offsetof(struct spdk_uring_sock, buf) % 8 == 0,
		   "Incorrect alignment: `buf` must be aligned to 8 bytes");

TAILQ_HEAD(pending_recv_list, spdk_uring_sock);

struct spdk_uring_buf_tracker {
	void					*buf;
	size_t					buflen;
	size_t					len;
	void					*ctx;
	int					id;
	STAILQ_ENTRY(spdk_uring_buf_tracker)	link;
};

struct spdk_uring_sock_group_impl {
	struct spdk_sock_group_impl		base;
	struct io_uring				uring;
	uint32_t				io_inflight;
	uint32_t				io_queued;
	uint32_t				io_avail;
	struct pending_recv_list		pending_recv;

	struct io_uring_buf_ring		*buf_ring;
	uint32_t				buf_ring_count;
	struct spdk_uring_buf_tracker		*trackers;
	STAILQ_HEAD(, spdk_uring_buf_tracker)	free_trackers;
};

static struct spdk_sock_impl_opts g_spdk_uring_sock_impl_opts = {
	.recv_buf_size = DEFAULT_SO_RCVBUF_SIZE,
	.send_buf_size = DEFAULT_SO_SNDBUF_SIZE,
	.enable_recv_pipe = true,
	.enable_quickack = false,
	.enable_placement_id = PLACEMENT_NONE,
	.enable_zerocopy_send_server = false,
	.enable_zerocopy_send_client = false,
	.zerocopy_threshold = 0,
	.tls_version = 0,
	.enable_ktls = false,
	.psk_key = NULL,
	.psk_identity = NULL
};

static struct spdk_sock_map g_map = {
	.entries = STAILQ_HEAD_INITIALIZER(g_map.entries),
	.mtx = PTHREAD_MUTEX_INITIALIZER
};

__attribute((destructor)) static void
uring_sock_map_cleanup(void)
{
	spdk_sock_map_cleanup(&g_map);
}

#define SPDK_URING_SOCK_REQUEST_IOV(req) ((struct iovec *)((uint8_t *)req + sizeof(struct spdk_sock_request)))

#define __uring_sock(sock) (struct spdk_uring_sock *)sock
#define __uring_group_impl(group) (struct spdk_uring_sock_group_impl *)group

static void
uring_sock_copy_impl_opts(struct spdk_sock_impl_opts *dest, const struct spdk_sock_impl_opts *src,
			  size_t len)
{
#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(src->field) <= len

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		dest->field = src->field; \
	}

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);
	SET_FIELD(enable_zerocopy_send_server);
	SET_FIELD(enable_zerocopy_send_client);
	SET_FIELD(zerocopy_threshold);
	SET_FIELD(tls_version);
	SET_FIELD(enable_ktls);
	SET_FIELD(psk_key);
	SET_FIELD(psk_identity);

#undef SET_FIELD
#undef FIELD_OK
}

static int
uring_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= *len);
	memset(opts, 0, *len);

	uring_sock_copy_impl_opts(opts, &g_spdk_uring_sock_impl_opts, *len);
	*len = spdk_min(*len, sizeof(g_spdk_uring_sock_impl_opts));

	return 0;
}

static int
uring_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= len);
	uring_sock_copy_impl_opts(&g_spdk_uring_sock_impl_opts, opts, len);

	return 0;
}

static void
uring_opts_get_impl_opts(const struct spdk_sock_opts *opts, struct spdk_sock_impl_opts *dest)
{
	/* Copy the default impl_opts first to cover cases when user's impl_opts is smaller */
	memcpy(dest, &g_spdk_uring_sock_impl_opts, sizeof(*dest));

	if (opts->impl_opts != NULL) {
		assert(sizeof(*dest) >= opts->impl_opts_size);
		uring_sock_copy_impl_opts(dest, opts->impl_opts, opts->impl_opts_size);
	}
}

static int
uring_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
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

enum uring_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
uring_sock_alloc_pipe(struct spdk_uring_sock *sock, int sz)
{
	uint8_t *new_buf;
	struct spdk_pipe *new_pipe;
	struct iovec siov[2];
	struct iovec diov[2];
	int sbytes;
	ssize_t bytes;
	int rc;

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
	rc = posix_memalign((void **)&new_buf, 64, sz);
	if (rc != 0) {
		SPDK_ERRLOG("socket recv buf allocation failed\n");
		return -ENOMEM;
	}
	memset(new_buf, 0, sz);

	new_pipe = spdk_pipe_create(new_buf, sz);
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
uring_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	if (_sock->impl_opts.enable_recv_pipe) {
		rc = uring_sock_alloc_pipe(sock, sz);
		if (rc) {
			SPDK_ERRLOG("unable to allocate sufficient recvbuf with sz=%d on sock=%p\n", sz, _sock);
			return rc;
		}
	}

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * g_spdk_uring_sock_impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, g_spdk_uring_sock_impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	_sock->impl_opts.recv_buf_size = sz;

	return 0;
}

static int
uring_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	/* Set kernel buffer size to be at least MIN_SO_SNDBUF_SIZE and
	 * g_spdk_uring_sock_impl_opts.seend_buf_size. */
	min_size = spdk_max(MIN_SO_SNDBUF_SIZE, g_spdk_uring_sock_impl_opts.send_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	_sock->impl_opts.send_buf_size = sz;

	return 0;
}

static struct spdk_uring_sock *
uring_sock_alloc(int fd, struct spdk_sock_impl_opts *impl_opts, bool enable_zero_copy)
{
	struct spdk_uring_sock *sock;
#if defined(__linux__)
	int flag;
	int rc;
#endif

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;
	memcpy(&sock->base.impl_opts, impl_opts, sizeof(*impl_opts));

	STAILQ_INIT(&sock->recv_stream);

#if defined(__linux__)
	flag = 1;

	if (sock->base.impl_opts.enable_quickack) {
		rc = setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}

	spdk_sock_get_placement_id(sock->fd, sock->base.impl_opts.enable_placement_id,
				   &sock->placement_id);
#ifdef SPDK_ZEROCOPY
	/* Try to turn on zero copy sends */
	flag = 1;

	if (enable_zero_copy) {
		rc = setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
		if (rc == 0) {
			sock->zcopy = true;
			sock->zcopy_send_flags = MSG_ZEROCOPY;
		}
	}
#endif
#endif

	return sock;
}

static struct spdk_sock *
uring_sock_create(const char *ip, int port,
		  enum uring_sock_create_type type,
		  struct spdk_sock_opts *opts)
{
	struct spdk_uring_sock *sock;
	struct spdk_sock_impl_opts impl_opts;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int fd, flag;
	int val = 1;
	int rc;
	bool enable_zcopy_impl_opts = false;
	bool enable_zcopy_user_opts = true;

	assert(opts != NULL);
	uring_opts_get_impl_opts(opts, &impl_opts);

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

		val = impl_opts.recv_buf_size;
		rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof val);
		if (rc) {
			/* Not fatal */
		}

		val = impl_opts.send_buf_size;
		rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof val);
		if (rc) {
			/* Not fatal */
		}

		rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			close(fd);
			fd = -1;
			/* error */
			continue;
		}
		rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
		if (rc != 0) {
			close(fd);
			fd = -1;
			/* error */
			continue;
		}

		if (opts->ack_timeout) {
#if defined(__linux__)
			val = opts->ack_timeout;
			rc = setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &val, sizeof val);
			if (rc != 0) {
				close(fd);
				fd = -1;
				/* error */
				continue;
			}
#else
			SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n");
#endif
		}



#if defined(SO_PRIORITY)
		if (opts != NULL && opts->priority) {
			rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
			if (rc != 0) {
				close(fd);
				fd = -1;
				/* error */
				continue;
			}
		}
#endif
		if (res->ai_family == AF_INET6) {
			rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
			if (rc != 0) {
				close(fd);
				fd = -1;
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

			flag = fcntl(fd, F_GETFL);
			if (fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0) {
				SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
				close(fd);
				fd = -1;
				break;
			}

			enable_zcopy_impl_opts = impl_opts.enable_zerocopy_send_server;
		} else if (type == SPDK_SOCK_CREATE_CONNECT) {
			rc = connect(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
				/* try next family */
				close(fd);
				fd = -1;
				continue;
			}

			flag = fcntl(fd, F_GETFL);
			if (fcntl(fd, F_SETFL, flag & ~O_NONBLOCK) < 0) {
				SPDK_ERRLOG("fcntl can't set blocking mode for socket, fd: %d (%d)\n", fd, errno);
				close(fd);
				fd = -1;
				break;
			}

			enable_zcopy_impl_opts = impl_opts.enable_zerocopy_send_client;
		}
		break;
	}
	freeaddrinfo(res0);

	if (fd < 0) {
		return NULL;
	}

	enable_zcopy_user_opts = opts->zcopy && !sock_is_loopback(fd);
	sock = uring_sock_alloc(fd, &impl_opts, enable_zcopy_user_opts && enable_zcopy_impl_opts);
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		close(fd);
		return NULL;
	}

	return &sock->base;
}

static struct spdk_sock *
uring_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	if (spdk_interrupt_mode_is_enabled()) {
		SPDK_ERRLOG("Interrupt mode is not supported in the uring sock implementation.");
		return NULL;
	}

	return uring_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN, opts);
}

static struct spdk_sock *
uring_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	if (spdk_interrupt_mode_is_enabled()) {
		SPDK_ERRLOG("Interrupt mode is not supported in the uring sock implementation.");
		return NULL;
	}

	return uring_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT, opts);
}

static struct spdk_sock *
uring_sock_accept(struct spdk_sock *_sock)
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
	if ((flag & O_NONBLOCK) && (fcntl(fd, F_SETFL, flag & ~O_NONBLOCK) < 0)) {
		SPDK_ERRLOG("fcntl can't set blocking mode for socket, fd: %d (%d)\n", fd, errno);
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

	new_sock = uring_sock_alloc(fd, &sock->base.impl_opts, sock->zcopy);
	if (new_sock == NULL) {
		close(fd);
		return NULL;
	}

	return &new_sock->base;
}

static int
uring_sock_close(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);

	assert(TAILQ_EMPTY(&_sock->pending_reqs));
	assert(sock->group == NULL);

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	close(sock->fd);

	spdk_pipe_destroy(sock->recv_pipe);
	free(sock->recv_buf);
	free(sock);

	return 0;
}

static ssize_t
uring_sock_recv_from_pipe(struct spdk_uring_sock *sock, struct iovec *diov, int diovcnt)
{
	struct iovec siov[2];
	int sbytes;
	ssize_t bytes;
	struct spdk_uring_sock_group_impl *group;

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
		group = __uring_group_impl(sock->base.group_impl);
		TAILQ_REMOVE(&group->pending_recv, sock, link);
		sock->pending_recv = false;
	}

	return bytes;
}

static inline ssize_t
sock_readv(int fd, struct iovec *iov, int iovcnt)
{
	struct msghdr msg = {
		.msg_iov = iov,
		.msg_iovlen = iovcnt,
	};

	return recvmsg(fd, &msg, MSG_DONTWAIT);
}

static inline ssize_t
uring_sock_read(struct spdk_uring_sock *sock)
{
	struct iovec iov[2];
	int bytes;
	struct spdk_uring_sock_group_impl *group;

	bytes = spdk_pipe_writer_get_buffer(sock->recv_pipe, sock->recv_buf_sz, iov);

	if (bytes > 0) {
		bytes = sock_readv(sock->fd, iov, 2);
		if (bytes > 0) {
			spdk_pipe_writer_advance(sock->recv_pipe, bytes);
			if (sock->base.group_impl && !sock->pending_recv) {
				group = __uring_group_impl(sock->base.group_impl);
				TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
				sock->pending_recv = true;
			}
		}
	}

	return bytes;
}

static int
uring_sock_recv_next(struct spdk_sock *_sock, void **_buf, void **ctx)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_sock_group_impl *group;
	struct spdk_uring_buf_tracker *tr;

	if (sock->connection_status < 0) {
		errno = -sock->connection_status;
		return -1;
	}

	if (sock->recv_pipe != NULL) {
		errno = ENOTSUP;
		return -1;
	}

	group = __uring_group_impl(_sock->group_impl);

	tr = STAILQ_FIRST(&sock->recv_stream);
	if (tr == NULL) {
		if (sock->group->buf_ring_count > 0) {
			/* There are buffers posted, but data hasn't arrived. */
			errno = EAGAIN;
		} else {
			/* There are no buffers posted, so this won't ever
			 * make forward progress. */
			errno = ENOBUFS;
		}
		return -1;
	}
	assert(sock->pending_recv == true);
	assert(tr->buf != NULL);

	*_buf = tr->buf + sock->recv_offset;
	*ctx = tr->ctx;

	STAILQ_REMOVE_HEAD(&sock->recv_stream, link);
	STAILQ_INSERT_HEAD(&group->free_trackers, tr, link);

	if (STAILQ_EMPTY(&sock->recv_stream)) {
		sock->pending_recv = false;
		TAILQ_REMOVE(&group->pending_recv, sock, link);
	}

	return tr->len - sock->recv_offset;
}

static ssize_t
uring_sock_readv_no_pipe(struct spdk_sock *_sock, struct iovec *iovs, int iovcnt)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_buf_tracker *tr;
	struct iovec iov;
	ssize_t total, len;
	int i;

	if (sock->connection_status < 0) {
		errno = -sock->connection_status;
		return -1;
	}

	if (_sock->group_impl == NULL) {
		/* If not in a group just read from the socket the regular way. */
		return sock_readv(sock->fd, iovs, iovcnt);
	}

	if (STAILQ_EMPTY(&sock->recv_stream)) {
		if (sock->group->buf_ring_count == 0) {
			/* If the user hasn't posted any buffers, read from the socket
			 * directly. */

			if (sock->pending_recv) {
				sock->pending_recv = false;
				TAILQ_REMOVE(&(__uring_group_impl(_sock->group_impl))->pending_recv, sock, link);
			}

			return sock_readv(sock->fd, iovs, iovcnt);
		}

		errno = EAGAIN;
		return -1;
	}

	total = 0;
	for (i = 0; i < iovcnt; i++) {
		/* Copy to stack so we can change it */
		iov = iovs[i];

		tr = STAILQ_FIRST(&sock->recv_stream);
		while (tr != NULL) {
			len = spdk_min(iov.iov_len, tr->len - sock->recv_offset);
			memcpy(iov.iov_base, tr->buf + sock->recv_offset, len);

			total += len;
			sock->recv_offset += len;
			iov.iov_base += len;
			iov.iov_len -= len;

			if (sock->recv_offset == tr->len) {
				sock->recv_offset = 0;
				STAILQ_REMOVE_HEAD(&sock->recv_stream, link);
				STAILQ_INSERT_HEAD(&sock->group->free_trackers, tr, link);
				spdk_sock_group_provide_buf(sock->group->base.group, tr->buf, tr->buflen, tr->ctx);
				tr = STAILQ_FIRST(&sock->recv_stream);
			}

			if (iov.iov_len == 0) {
				break;
			}
		}
	}

	if (STAILQ_EMPTY(&sock->recv_stream)) {
		struct spdk_uring_sock_group_impl *group;

		group = __uring_group_impl(_sock->group_impl);
		sock->pending_recv = false;
		TAILQ_REMOVE(&group->pending_recv, sock, link);
	}

	assert(total > 0);
	return total;
}

static ssize_t
uring_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int rc, i;
	size_t len;

	if (sock->connection_status < 0) {
		errno = -sock->connection_status;
		return -1;
	}

	if (sock->recv_pipe == NULL) {
		return uring_sock_readv_no_pipe(_sock, iov, iovcnt);
	}

	len = 0;
	for (i = 0; i < iovcnt; i++) {
		len += iov[i].iov_len;
	}

	if (spdk_pipe_reader_bytes_available(sock->recv_pipe) == 0) {
		/* If the user is receiving a sufficiently large amount of data,
		 * receive directly to their buffers. */
		if (len >= MIN_SOCK_PIPE_SIZE) {
			return sock_readv(sock->fd, iov, iovcnt);
		}

		/* Otherwise, do a big read into our pipe */
		rc = uring_sock_read(sock);
		if (rc <= 0) {
			return rc;
		}
	}

	return uring_sock_recv_from_pipe(sock, iov, iovcnt);
}

static ssize_t
uring_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	struct iovec iov[1];

	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	return uring_sock_readv(sock, iov, 1);
}

static ssize_t
uring_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct msghdr msg = {
		.msg_iov = iov,
		.msg_iovlen = iovcnt,
	};

	if (sock->write_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		errno = EAGAIN;
		return -1;
	}

	return sendmsg(sock->fd, &msg, MSG_DONTWAIT);
}

static ssize_t
sock_request_advance_offset(struct spdk_sock_request *req, ssize_t rc)
{
	unsigned int offset;
	size_t len;
	int i;

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
			req->internal.offset += rc;
			return -1;
		}

		offset = 0;
		req->internal.offset += len;
		rc -= len;
	}

	return rc;
}

static int
sock_complete_write_reqs(struct spdk_sock *_sock, ssize_t rc, bool is_zcopy)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_sock_request *req;
	int retval;

	if (is_zcopy) {
		/* Handling overflow case, because we use psock->sendmsg_idx - 1 for the
		 * req->internal.offset, so sendmsg_idx should not be zero */
		if (spdk_unlikely(sock->sendmsg_idx == UINT32_MAX)) {
			sock->sendmsg_idx = 1;
		} else {
			sock->sendmsg_idx++;
		}
	}

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&_sock->queued_reqs);
	while (req) {
		/* req->internal.is_zcopy is true when the whole req or part of it is sent with zerocopy */
		req->internal.is_zcopy = is_zcopy;

		rc = sock_request_advance_offset(req, rc);
		if (rc < 0) {
			/* This element was partially sent. */
			return 0;
		}

		/* Handled a full request. */
		spdk_sock_request_pend(_sock, req);

		if (!req->internal.is_zcopy && req == TAILQ_FIRST(&_sock->pending_reqs)) {
			retval = spdk_sock_request_put(_sock, req, 0);
			if (retval) {
				return retval;
			}
		} else {
			/* Re-use the offset field to hold the sendmsg call index. The
			 * index is 0 based, so subtract one here because we've already
			 * incremented above. */
			req->internal.offset = sock->sendmsg_idx - 1;
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&_sock->queued_reqs);
	}

	return 0;
}

#ifdef SPDK_ZEROCOPY
static int
_sock_check_zcopy(struct spdk_sock *_sock, int status)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;
	struct spdk_sock_request *req, *treq;
	bool found;

	assert(sock->zcopy == true);
	if (spdk_unlikely(status) < 0) {
		if (!TAILQ_EMPTY(&_sock->pending_reqs)) {
			SPDK_ERRLOG("Attempting to receive from ERRQUEUE yielded error, but pending list still has orphaned entries, status =%d\n",
				    status);
		} else {
			SPDK_WARNLOG("Recvmsg yielded an error!\n");
		}
		return 0;
	}

	cm = CMSG_FIRSTHDR(&sock->errqueue_task.msg);
	if (!((cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR) ||
	      (cm->cmsg_level == SOL_IPV6 && cm->cmsg_type == IPV6_RECVERR))) {
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
		TAILQ_FOREACH_SAFE(req, &_sock->pending_reqs, internal.link, treq) {
			if (!req->internal.is_zcopy) {
				/* This wasn't a zcopy request. It was just waiting in line to complete */
				rc = spdk_sock_request_put(_sock, req, 0);
				if (rc < 0) {
					return rc;
				}
			} else if (req->internal.offset == idx) {
				found = true;
				rc = spdk_sock_request_put(_sock, req, 0);
				if (rc < 0) {
					return rc;
				}
			} else if (found) {
				break;
			}
		}
	}

	return 0;
}

static void
_sock_prep_errqueue(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_task *task = &sock->errqueue_task;
	struct io_uring_sqe *sqe;

	if (task->status == SPDK_URING_SOCK_TASK_IN_PROCESS) {
		return;
	}

	if (sock->pending_group_remove) {
		return;
	}

	assert(sock->group != NULL);
	sock->group->io_queued++;

	sqe = io_uring_get_sqe(&sock->group->uring);
	io_uring_prep_recvmsg(sqe, sock->fd, &task->msg, MSG_ERRQUEUE);
	io_uring_sqe_set_data(sqe, task);
	task->status = SPDK_URING_SOCK_TASK_IN_PROCESS;
}

#endif

static void
_sock_flush(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_task *task = &sock->write_task;
	uint32_t iovcnt;
	struct io_uring_sqe *sqe;
	int flags;

	if (task->status == SPDK_URING_SOCK_TASK_IN_PROCESS) {
		return;
	}

#ifdef SPDK_ZEROCOPY
	if (sock->zcopy) {
		flags = MSG_DONTWAIT | sock->zcopy_send_flags;
	} else
#endif
	{
		flags = MSG_DONTWAIT;
	}

	iovcnt = spdk_sock_prep_reqs(&sock->base, task->iovs, task->iov_cnt, &task->last_req, &flags);
	if (!iovcnt) {
		return;
	}

	task->iov_cnt = iovcnt;
	assert(sock->group != NULL);
	task->msg.msg_iov = task->iovs;
	task->msg.msg_iovlen = task->iov_cnt;
#ifdef SPDK_ZEROCOPY
	task->is_zcopy = (flags & MSG_ZEROCOPY) ? true : false;
#endif
	sock->group->io_queued++;

	sqe = io_uring_get_sqe(&sock->group->uring);
	io_uring_prep_sendmsg(sqe, sock->fd, &sock->write_task.msg, flags);
	io_uring_sqe_set_data(sqe, task);
	task->status = SPDK_URING_SOCK_TASK_IN_PROCESS;
}

static void
_sock_prep_read(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_task *task = &sock->read_task;
	struct io_uring_sqe *sqe;

	/* Do not prepare read event */
	if (task->status == SPDK_URING_SOCK_TASK_IN_PROCESS) {
		return;
	}

	if (sock->pending_group_remove) {
		return;
	}

	assert(sock->group != NULL);
	sock->group->io_queued++;

	sqe = io_uring_get_sqe(&sock->group->uring);
	io_uring_prep_recv(sqe, sock->fd, NULL, URING_MAX_RECV_SIZE, 0);
	sqe->buf_group = URING_BUF_GROUP_ID;
	sqe->flags |= IOSQE_BUFFER_SELECT;
	io_uring_sqe_set_data(sqe, task);
	task->status = SPDK_URING_SOCK_TASK_IN_PROCESS;
}

static void
_sock_prep_cancel_task(struct spdk_sock *_sock, void *user_data)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_task *task = &sock->cancel_task;
	struct io_uring_sqe *sqe;

	if (task->status == SPDK_URING_SOCK_TASK_IN_PROCESS) {
		return;
	}

	assert(sock->group != NULL);
	sock->group->io_queued++;

	sqe = io_uring_get_sqe(&sock->group->uring);
	io_uring_prep_cancel(sqe, user_data, 0);
	io_uring_sqe_set_data(sqe, task);
	task->status = SPDK_URING_SOCK_TASK_IN_PROCESS;
}

static void
uring_sock_fail(struct spdk_uring_sock *sock, int status)
{
	struct spdk_uring_sock_group_impl *group = sock->group;
	int rc;

	sock->connection_status = status;
	rc = spdk_sock_abort_requests(&sock->base);

	/* The user needs to be notified that this socket is dead. */
	if (rc == 0 && sock->base.cb_fn != NULL &&
	    sock->pending_recv == false) {
		sock->pending_recv = true;
		TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
	}
}

static int
sock_uring_group_reap(struct spdk_uring_sock_group_impl *group, int max, int max_read_events,
		      struct spdk_sock **socks)
{
	int i, count, ret;
	struct io_uring_cqe *cqe;
	struct spdk_uring_sock *sock, *tmp;
	struct spdk_uring_task *task;
	int status, bid, flags;
	bool is_zcopy;

	for (i = 0; i < max; i++) {
		ret = io_uring_peek_cqe(&group->uring, &cqe);
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
		assert(sock->group == group);
		sock->group->io_inflight--;
		sock->group->io_avail++;
		status = cqe->res;
		flags = cqe->flags;
		io_uring_cqe_seen(&group->uring, cqe);

		task->status = SPDK_URING_SOCK_TASK_NOT_IN_USE;

		switch (task->type) {
		case URING_TASK_READ:
			if (status == -EAGAIN || status == -EWOULDBLOCK) {
				/* This likely shouldn't happen, but would indicate that the
				 * kernel didn't have enough resources to queue a task internally. */
				_sock_prep_read(&sock->base);
			} else if (status == -ECANCELED) {
				continue;
			} else if (status == -ENOBUFS) {
				/* There's data in the socket but the user hasn't provided any buffers.
				 * We need to notify the user that the socket has data pending. */
				if (sock->base.cb_fn != NULL &&
				    sock->pending_recv == false) {
					sock->pending_recv = true;
					TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
				}

				_sock_prep_read(&sock->base);
			} else if (spdk_unlikely(status <= 0)) {
				uring_sock_fail(sock, status < 0 ? status : -ECONNRESET);
			} else {
				struct spdk_uring_buf_tracker *tracker;

				assert((flags & IORING_CQE_F_BUFFER) != 0);

				bid = flags >> IORING_CQE_BUFFER_SHIFT;
				tracker = &group->trackers[bid];

				assert(tracker->buf != NULL);
				assert(tracker->len != 0);

				/* Append this data to the stream */
				tracker->len = status;
				STAILQ_INSERT_TAIL(&sock->recv_stream, tracker, link);
				assert(group->buf_ring_count > 0);
				group->buf_ring_count--;

				if (sock->base.cb_fn != NULL &&
				    sock->pending_recv == false) {
					sock->pending_recv = true;
					TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
				}

				_sock_prep_read(&sock->base);
			}
			break;
		case URING_TASK_WRITE:
			if (status == -EAGAIN || status == -EWOULDBLOCK ||
			    (status == -ENOBUFS && sock->zcopy) ||
			    status == -ECANCELED) {
				continue;
			} else if (spdk_unlikely(status) < 0) {
				uring_sock_fail(sock, status);
			} else {
				task->last_req = NULL;
				task->iov_cnt = 0;
				is_zcopy = task->is_zcopy;
				task->is_zcopy = false;
				sock_complete_write_reqs(&sock->base, status, is_zcopy);
			}

			break;
#ifdef SPDK_ZEROCOPY
		case URING_TASK_ERRQUEUE:
			if (status == -EAGAIN || status == -EWOULDBLOCK) {
				_sock_prep_errqueue(&sock->base);
			} else if (status == -ECANCELED) {
				continue;
			} else if (spdk_unlikely(status < 0)) {
				uring_sock_fail(sock, status);
			} else {
				_sock_check_zcopy(&sock->base, status);
				_sock_prep_errqueue(&sock->base);
			}
			break;
#endif
		case URING_TASK_CANCEL:
			/* Do nothing */
			break;
		default:
			SPDK_UNREACHABLE();
		}
	}

	if (!socks) {
		return 0;
	}
	count = 0;
	TAILQ_FOREACH_SAFE(sock, &group->pending_recv, link, tmp) {
		if (count == max_read_events) {
			break;
		}

		/* If the socket's cb_fn is NULL, do not add it to socks array */
		if (spdk_unlikely(sock->base.cb_fn == NULL)) {
			assert(sock->pending_recv == true);
			sock->pending_recv = false;
			TAILQ_REMOVE(&group->pending_recv, sock, link);
			continue;
		}

		socks[count++] = &sock->base;
	}


	/* Cycle the pending_recv list so that each time we poll things aren't
	 * in the same order. Say we have 6 sockets in the list, named as follows:
	 * A B C D E F
	 * And all 6 sockets had the poll events, but max_events is only 3. That means
	 * psock currently points at D. We want to rearrange the list to the following:
	 * D E F A B C
	 *
	 * The variables below are named according to this example to make it easier to
	 * follow the swaps.
	 */
	if (sock != NULL) {
		struct spdk_uring_sock *ua, *uc, *ud, *uf;

		/* Capture pointers to the elements we need */
		ud = sock;

		ua = TAILQ_FIRST(&group->pending_recv);
		if (ua == ud) {
			goto end;
		}

		uf = TAILQ_LAST(&group->pending_recv, pending_recv_list);
		if (uf == ud) {
			TAILQ_REMOVE(&group->pending_recv, ud, link);
			TAILQ_INSERT_HEAD(&group->pending_recv, ud, link);
			goto end;
		}

		uc = TAILQ_PREV(ud, pending_recv_list, link);
		assert(uc != NULL);

		/* Break the link between C and D */
		uc->link.tqe_next = NULL;

		/* Connect F to A */
		uf->link.tqe_next = ua;
		ua->link.tqe_prev = &uf->link.tqe_next;

		/* Fix up the list first/last pointers */
		group->pending_recv.tqh_first = ud;
		group->pending_recv.tqh_last = &uc->link.tqe_next;

		/* D is in front of the list, make tqe prev pointer point to the head of list */
		ud->link.tqe_prev = &group->pending_recv.tqh_first;
	}

end:
	return count;
}

static int uring_sock_flush(struct spdk_sock *_sock);

static void
uring_sock_writev_async(struct spdk_sock *_sock, struct spdk_sock_request *req)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	int rc;

	if (spdk_unlikely(sock->connection_status)) {
		req->cb_fn(req->cb_arg, sock->connection_status);
		return;
	}

	spdk_sock_request_queue(_sock, req);

	if (!sock->group) {
		if (_sock->queued_iovcnt >= IOV_BATCH_SIZE) {
			rc = uring_sock_flush(_sock);
			if (rc < 0 && errno != EAGAIN) {
				spdk_sock_abort_requests(_sock);
			}
		}
	}
}

static int
uring_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
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

static bool
uring_sock_is_ipv6(struct spdk_sock *_sock)
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
uring_sock_is_ipv4(struct spdk_sock *_sock)
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
uring_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	uint8_t byte;
	int rc;

	rc = recv(sock->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
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

static struct spdk_sock_group_impl *
uring_sock_group_impl_get_optimal(struct spdk_sock *_sock, struct spdk_sock_group_impl *hint)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_sock_group_impl *group;

	if (sock->placement_id != -1) {
		spdk_sock_map_lookup(&g_map, sock->placement_id, &group, hint);
		return group;
	}

	return NULL;
}

static int
uring_sock_group_impl_buf_pool_free(struct spdk_uring_sock_group_impl *group_impl)
{
	if (group_impl->buf_ring) {
		io_uring_unregister_buf_ring(&group_impl->uring, URING_BUF_GROUP_ID);
		free(group_impl->buf_ring);
	}

	free(group_impl->trackers);

	return 0;
}

static int
uring_sock_group_impl_buf_pool_alloc(struct spdk_uring_sock_group_impl *group_impl)
{
	struct io_uring_buf_reg buf_reg = {};
	struct io_uring_buf_ring *buf_ring;
	int i, rc;

	rc = posix_memalign((void **)&buf_ring, 0x1000, URING_BUF_POOL_SIZE * sizeof(struct io_uring_buf));
	if (rc != 0) {
		/* posix_memalign returns positive errno values */
		return -rc;
	}

	buf_reg.ring_addr = (unsigned long long)buf_ring;
	buf_reg.ring_entries = URING_BUF_POOL_SIZE;
	buf_reg.bgid = URING_BUF_GROUP_ID;

	rc = io_uring_register_buf_ring(&group_impl->uring, &buf_reg, 0);
	if (rc != 0) {
		free(buf_ring);
		return rc;
	}

	group_impl->buf_ring = buf_ring;
	io_uring_buf_ring_init(group_impl->buf_ring);
	group_impl->buf_ring_count = 0;

	group_impl->trackers = calloc(URING_BUF_POOL_SIZE, sizeof(struct spdk_uring_buf_tracker));
	if (group_impl->trackers == NULL) {
		uring_sock_group_impl_buf_pool_free(group_impl);
		return -ENOMEM;
	}

	STAILQ_INIT(&group_impl->free_trackers);

	for (i = 0; i < URING_BUF_POOL_SIZE; i++) {
		struct spdk_uring_buf_tracker *tracker = &group_impl->trackers[i];

		tracker->buf = NULL;
		tracker->len = 0;
		tracker->ctx = NULL;
		tracker->id = i;

		STAILQ_INSERT_TAIL(&group_impl->free_trackers, tracker, link);
	}

	return 0;
}

static struct spdk_sock_group_impl *
uring_sock_group_impl_create(void)
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

	TAILQ_INIT(&group_impl->pending_recv);

	if (uring_sock_group_impl_buf_pool_alloc(group_impl) < 0) {
		SPDK_ERRLOG("Failed to create buffer ring."
			    "uring sock implementation is likely not supported on this kernel.\n");
		io_uring_queue_exit(&group_impl->uring);
		free(group_impl);
		return NULL;
	}

	if (g_spdk_uring_sock_impl_opts.enable_placement_id == PLACEMENT_CPU) {
		spdk_sock_map_insert(&g_map, spdk_env_get_current_core(), &group_impl->base);
	}

	return &group_impl->base;
}

static int
uring_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group,
			       struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);
	int rc;

	sock->group = group;
	sock->write_task.sock = sock;
	sock->write_task.type = URING_TASK_WRITE;

	sock->read_task.sock = sock;
	sock->read_task.type = URING_TASK_READ;

	sock->errqueue_task.sock = sock;
	sock->errqueue_task.type = URING_TASK_ERRQUEUE;
	sock->errqueue_task.msg.msg_control = sock->buf;
	sock->errqueue_task.msg.msg_controllen = sizeof(sock->buf);

	sock->cancel_task.sock = sock;
	sock->cancel_task.type = URING_TASK_CANCEL;

	/* switched from another polling group due to scheduling */
	if (spdk_unlikely(sock->recv_pipe != NULL &&
			  (spdk_pipe_reader_bytes_available(sock->recv_pipe) > 0))) {
		assert(sock->pending_recv == false);
		sock->pending_recv = true;
		TAILQ_INSERT_TAIL(&group->pending_recv, sock, link);
	}

	if (sock->placement_id != -1) {
		rc = spdk_sock_map_insert(&g_map, sock->placement_id, &group->base);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to insert sock group into map: %d", rc);
			/* Do not treat this as an error. The system will continue running. */
		}
	}

	/* We get an async read going immediately */
	_sock_prep_read(&sock->base);
#ifdef SPDK_ZEROCOPY
	if (sock->zcopy) {
		_sock_prep_errqueue(_sock);
	}
#endif

	return 0;
}

static void
uring_sock_group_populate_buf_ring(struct spdk_uring_sock_group_impl *group)
{
	struct spdk_uring_buf_tracker *tracker;
	int count, mask;

	if (g_spdk_uring_sock_impl_opts.enable_recv_pipe) {
		/* If recv_pipe is enabled, we do not post buffers. */
		return;
	}

	/* Try to re-populate the io_uring's buffer pool using user-provided buffers */
	tracker = STAILQ_FIRST(&group->free_trackers);
	count = 0;
	mask = io_uring_buf_ring_mask(URING_BUF_POOL_SIZE);
	while (tracker != NULL) {
		tracker->buflen = spdk_sock_group_get_buf(group->base.group, &tracker->buf, &tracker->ctx);
		if (tracker->buflen == 0) {
			break;
		}

		assert(tracker->buf != NULL);
		STAILQ_REMOVE_HEAD(&group->free_trackers, link);
		assert(STAILQ_FIRST(&group->free_trackers) != tracker);

		io_uring_buf_ring_add(group->buf_ring, tracker->buf, tracker->buflen, tracker->id, mask, count);
		count++;
		tracker = STAILQ_FIRST(&group->free_trackers);
	}

	if (count > 0) {
		group->buf_ring_count += count;
		io_uring_buf_ring_advance(group->buf_ring, count);
	}
}

static int
uring_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			   struct spdk_sock **socks)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);
	int count, ret;
	int to_complete, to_submit;
	struct spdk_sock *_sock, *tmp;
	struct spdk_uring_sock *sock;

	if (spdk_likely(socks)) {
		TAILQ_FOREACH_SAFE(_sock, &group->base.socks, link, tmp) {
			sock = __uring_sock(_sock);
			if (spdk_unlikely(sock->connection_status)) {
				continue;
			}
			_sock_flush(_sock);
		}
	}

	/* Try to re-populate the io_uring's buffer pool using user-provided buffers */
	uring_sock_group_populate_buf_ring(group);

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
	if (to_complete > 0 || !TAILQ_EMPTY(&group->pending_recv)) {
		count = sock_uring_group_reap(group, to_complete, max_events, socks);
	}

	return count;
}

static int
uring_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group,
				  struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);

	sock->pending_group_remove = true;

	if (sock->write_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		_sock_prep_cancel_task(_sock, &sock->write_task);
		/* Since spdk_sock_group_remove_sock is not asynchronous interface, so
		 * currently can use a while loop here. */
		while ((sock->write_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) ||
		       (sock->cancel_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE)) {
			uring_sock_group_impl_poll(_group, 32, NULL);
		}
	}

	if (sock->read_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		_sock_prep_cancel_task(_sock, &sock->read_task);
		/* Since spdk_sock_group_remove_sock is not asynchronous interface, so
		 * currently can use a while loop here. */
		while ((sock->read_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) ||
		       (sock->cancel_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE)) {
			uring_sock_group_impl_poll(_group, 32, NULL);
		}
	}

	if (sock->errqueue_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		_sock_prep_cancel_task(_sock, &sock->errqueue_task);
		/* Since spdk_sock_group_remove_sock is not asynchronous interface, so
		 * currently can use a while loop here. */
		while ((sock->errqueue_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) ||
		       (sock->cancel_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE)) {
			uring_sock_group_impl_poll(_group, 32, NULL);
		}
	}

	/* Make sure the cancelling the tasks above didn't cause sending new requests */
	assert(sock->write_task.status == SPDK_URING_SOCK_TASK_NOT_IN_USE);
	assert(sock->read_task.status == SPDK_URING_SOCK_TASK_NOT_IN_USE);
	assert(sock->errqueue_task.status == SPDK_URING_SOCK_TASK_NOT_IN_USE);

	if (sock->pending_recv) {
		TAILQ_REMOVE(&group->pending_recv, sock, link);
		sock->pending_recv = false;
	}
	assert(sock->pending_recv == false);

	/* We have no way to handle this case. We could let the user read this
	 * buffer, but the buffer came from a group and we have lost the association
	 * to that so we couldn't release it. */
	assert(STAILQ_EMPTY(&sock->recv_stream));

	if (sock->placement_id != -1) {
		spdk_sock_map_release(&g_map, sock->placement_id);
	}

	sock->pending_group_remove = false;
	sock->group = NULL;
	return 0;
}

static int
uring_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_uring_sock_group_impl *group = __uring_group_impl(_group);

	/* try to reap all the active I/O */
	while (group->io_inflight) {
		uring_sock_group_impl_poll(_group, 32, NULL);
	}
	assert(group->io_inflight == 0);
	assert(group->io_avail == SPDK_SOCK_GROUP_QUEUE_DEPTH);

	uring_sock_group_impl_buf_pool_free(group);

	io_uring_queue_exit(&group->uring);

	if (g_spdk_uring_sock_impl_opts.enable_placement_id == PLACEMENT_CPU) {
		spdk_sock_map_release(&g_map, spdk_env_get_current_core());
	}

	free(group);
	return 0;
}

static int
uring_sock_flush(struct spdk_sock *_sock)
{
	struct spdk_uring_sock *sock = __uring_sock(_sock);
	struct msghdr msg = {};
	struct iovec iovs[IOV_BATCH_SIZE];
	int iovcnt;
	ssize_t rc;
	int flags = sock->zcopy_send_flags;
	int retval;
	bool is_zcopy = false;
	struct spdk_uring_task *task = &sock->errqueue_task;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (_sock->cb_cnt > 0) {
		errno = EAGAIN;
		return -1;
	}

	/* Can't flush while a write is already outstanding */
	if (sock->write_task.status != SPDK_URING_SOCK_TASK_NOT_IN_USE) {
		errno = EAGAIN;
		return -1;
	}

	/* Gather an iov */
	iovcnt = spdk_sock_prep_reqs(_sock, iovs, 0, NULL, &flags);
	if (iovcnt == 0) {
		/* Nothing to send */
		return 0;
	}

	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;
	rc = sendmsg(sock->fd, &msg, flags | MSG_DONTWAIT);
	if (rc <= 0) {
		if (rc == 0 || errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && sock->zcopy)) {
			errno = EAGAIN;
		}
		return -1;
	}

#ifdef SPDK_ZEROCOPY
	is_zcopy = flags & MSG_ZEROCOPY;
#endif
	retval = sock_complete_write_reqs(_sock, rc, is_zcopy);
	if (retval < 0) {
		/* if the socket is closed, return to avoid heap-use-after-free error */
		errno = ENOTCONN;
		return -1;
	}

#ifdef SPDK_ZEROCOPY
	/* At least do once to check zero copy case */
	if (sock->zcopy && !TAILQ_EMPTY(&_sock->pending_reqs)) {
		retval = recvmsg(sock->fd, &task->msg, MSG_ERRQUEUE);
		if (retval < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return rc;
			}
		}
		_sock_check_zcopy(_sock, retval);;
	}
#endif

	return rc;
}

static int
uring_sock_group_impl_register_interrupt(struct spdk_sock_group_impl *_group, uint32_t events,
		spdk_interrupt_fn fn, void *arg, const char *name)
{
	SPDK_ERRLOG("Interrupt mode is not supported in the uring sock implementation.");

	return -ENOTSUP;
}

static void
uring_sock_group_impl_unregister_interrupt(struct spdk_sock_group_impl *_group)
{
}

static struct spdk_net_impl g_uring_net_impl = {
	.name		= "uring",
	.getaddr	= uring_sock_getaddr,
	.connect	= uring_sock_connect,
	.listen		= uring_sock_listen,
	.accept		= uring_sock_accept,
	.close		= uring_sock_close,
	.recv		= uring_sock_recv,
	.readv		= uring_sock_readv,
	.writev		= uring_sock_writev,
	.recv_next	= uring_sock_recv_next,
	.writev_async	= uring_sock_writev_async,
	.flush          = uring_sock_flush,
	.set_recvlowat	= uring_sock_set_recvlowat,
	.set_recvbuf	= uring_sock_set_recvbuf,
	.set_sendbuf	= uring_sock_set_sendbuf,
	.is_ipv6	= uring_sock_is_ipv6,
	.is_ipv4	= uring_sock_is_ipv4,
	.is_connected   = uring_sock_is_connected,
	.group_impl_get_optimal	= uring_sock_group_impl_get_optimal,
	.group_impl_create	= uring_sock_group_impl_create,
	.group_impl_add_sock	= uring_sock_group_impl_add_sock,
	.group_impl_remove_sock	= uring_sock_group_impl_remove_sock,
	.group_impl_poll	= uring_sock_group_impl_poll,
	.group_impl_register_interrupt    = uring_sock_group_impl_register_interrupt,
	.group_impl_unregister_interrupt  = uring_sock_group_impl_unregister_interrupt,
	.group_impl_close	= uring_sock_group_impl_close,
	.get_opts		= uring_sock_impl_get_opts,
	.set_opts		= uring_sock_impl_set_opts,
};

__attribute__((constructor)) static void
net_impl_register_uring(void)
{
	struct spdk_sock_group_impl *impl;

	/* Check if we can create a uring sock group before we register
	 * it as a valid impl. */
	impl = uring_sock_group_impl_create();
	if (impl) {
		uring_sock_group_impl_close(impl);
		spdk_net_impl_register(&g_uring_net_impl);
	}
}
