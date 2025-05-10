/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/sock.h"
#include "spdk_internal/sock_module.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/trace.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk_internal/trace_defs.h"

#define SPDK_SOCK_DEFAULT_PRIORITY 0
#define SPDK_SOCK_DEFAULT_ZCOPY true
#define SPDK_SOCK_DEFAULT_ACK_TIMEOUT 0
#define SPDK_SOCK_DEFAULT_CONNECT_TIMEOUT 0

#define PORTNUMLEN 32
#define MAX_TMPBUF 1024

#define SPDK_SOCK_OPTS_FIELD_OK(opts, field) (offsetof(struct spdk_sock_opts, field) + sizeof(opts->field) <= (opts->opts_size))

static STAILQ_HEAD(, spdk_net_impl) g_net_impls = STAILQ_HEAD_INITIALIZER(g_net_impls);
static struct spdk_net_impl *g_default_impl;

struct spdk_sock_placement_id_entry {
	int placement_id;
	uint32_t ref;
	struct spdk_sock_group_impl *group;
	STAILQ_ENTRY(spdk_sock_placement_id_entry) link;
};

static inline struct spdk_sock_group_impl *
sock_get_group_impl_from_group(struct spdk_sock *sock, struct spdk_sock_group *group)
{
	struct spdk_sock_group_impl *group_impl = NULL;

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		if (sock->net_impl == group_impl->net_impl) {
			return group_impl;
		}
	}
	return NULL;
}

/* Called under map->mtx lock */
static struct spdk_sock_placement_id_entry *
_sock_map_entry_alloc(struct spdk_sock_map *map, int placement_id)
{
	struct spdk_sock_placement_id_entry *entry;

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		SPDK_ERRLOG("Cannot allocate an entry for placement_id=%u\n", placement_id);
		return NULL;
	}

	entry->placement_id = placement_id;

	STAILQ_INSERT_TAIL(&map->entries, entry, link);

	return entry;
}

int
spdk_sock_map_insert(struct spdk_sock_map *map, int placement_id,
		     struct spdk_sock_group_impl *group)
{
	struct spdk_sock_placement_id_entry *entry;
	int rc = 0;

	pthread_mutex_lock(&map->mtx);
	STAILQ_FOREACH(entry, &map->entries, link) {
		if (placement_id == entry->placement_id) {
			/* Can't set group to NULL if it is already not-NULL */
			if (group == NULL) {
				rc = (entry->group == NULL) ? 0 : -EINVAL;
				goto end;
			}

			if (entry->group == NULL) {
				entry->group = group;
			} else if (entry->group != group) {
				rc = -EINVAL;
				goto end;
			}

			entry->ref++;
			goto end;
		}
	}

	entry = _sock_map_entry_alloc(map, placement_id);
	if (entry == NULL) {
		rc = -ENOMEM;
		goto end;
	}
	if (group) {
		entry->group = group;
		entry->ref++;
	}
end:
	pthread_mutex_unlock(&map->mtx);

	return rc;
}

void
spdk_sock_map_release(struct spdk_sock_map *map, int placement_id)
{
	struct spdk_sock_placement_id_entry *entry;

	pthread_mutex_lock(&map->mtx);
	STAILQ_FOREACH(entry, &map->entries, link) {
		if (placement_id == entry->placement_id) {
			assert(entry->ref > 0);
			entry->ref--;

			if (entry->ref == 0) {
				entry->group = NULL;
			}
			break;
		}
	}

	pthread_mutex_unlock(&map->mtx);
}

int
spdk_sock_map_lookup(struct spdk_sock_map *map, int placement_id,
		     struct spdk_sock_group_impl **group, struct spdk_sock_group_impl *hint)
{
	struct spdk_sock_placement_id_entry *entry;

	*group = NULL;
	pthread_mutex_lock(&map->mtx);
	STAILQ_FOREACH(entry, &map->entries, link) {
		if (placement_id == entry->placement_id) {
			*group = entry->group;
			if (*group != NULL) {
				/* Return previously assigned sock_group */
				pthread_mutex_unlock(&map->mtx);
				return 0;
			}
			break;
		}
	}

	/* No entry with assigned sock_group, nor hint to use */
	if (hint == NULL) {
		pthread_mutex_unlock(&map->mtx);
		return -EINVAL;
	}

	/* Create new entry if there is none with matching placement_id */
	if (entry == NULL) {
		entry = _sock_map_entry_alloc(map, placement_id);
		if (entry == NULL) {
			pthread_mutex_unlock(&map->mtx);
			return -ENOMEM;
		}
	}

	entry->group = hint;
	pthread_mutex_unlock(&map->mtx);

	return 0;
}

void
spdk_sock_map_cleanup(struct spdk_sock_map *map)
{
	struct spdk_sock_placement_id_entry *entry, *tmp;

	pthread_mutex_lock(&map->mtx);
	STAILQ_FOREACH_SAFE(entry, &map->entries, link, tmp) {
		STAILQ_REMOVE(&map->entries, entry, spdk_sock_placement_id_entry, link);
		free(entry);
	}
	pthread_mutex_unlock(&map->mtx);
}

int
spdk_sock_map_find_free(struct spdk_sock_map *map)
{
	struct spdk_sock_placement_id_entry *entry;
	int placement_id = -1;

	pthread_mutex_lock(&map->mtx);
	STAILQ_FOREACH(entry, &map->entries, link) {
		if (entry->group == NULL) {
			placement_id = entry->placement_id;
			break;
		}
	}

	pthread_mutex_unlock(&map->mtx);

	return placement_id;
}

int
spdk_sock_get_optimal_sock_group(struct spdk_sock *sock, struct spdk_sock_group **group,
				 struct spdk_sock_group *hint)
{
	struct spdk_sock_group_impl *group_impl;
	struct spdk_sock_group_impl *hint_group_impl = NULL;

	assert(group != NULL);

	if (hint != NULL) {
		hint_group_impl = sock_get_group_impl_from_group(sock, hint);
		if (hint_group_impl == NULL) {
			return -EINVAL;
		}
	}

	group_impl = sock->net_impl->group_impl_get_optimal(sock, hint_group_impl);

	if (group_impl) {
		*group = group_impl->group;
	}

	return 0;
}

int
spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
		  char *caddr, int clen, uint16_t *cport)
{
	return sock->net_impl->getaddr(sock, saddr, slen, sport, caddr, clen, cport);
}

const char *
spdk_sock_get_interface_name(struct spdk_sock *sock)
{
	if (sock->net_impl->get_interface_name) {
		return sock->net_impl->get_interface_name(sock);
	} else {
		return NULL;
	}
}

int32_t
spdk_sock_get_numa_id(struct spdk_sock *sock)
{
	if (sock->net_impl->get_numa_id) {
		return sock->net_impl->get_numa_id(sock);
	} else {
		return SPDK_ENV_NUMA_ID_ANY;
	}
}

const char *
spdk_sock_get_impl_name(struct spdk_sock *sock)
{
	return sock->net_impl->name;
}

void
spdk_sock_get_default_opts(struct spdk_sock_opts *opts)
{
	assert(opts);

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, priority)) {
		opts->priority = SPDK_SOCK_DEFAULT_PRIORITY;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, zcopy)) {
		opts->zcopy = SPDK_SOCK_DEFAULT_ZCOPY;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, ack_timeout)) {
		opts->ack_timeout = SPDK_SOCK_DEFAULT_ACK_TIMEOUT;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts)) {
		opts->impl_opts = NULL;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts_size)) {
		opts->impl_opts_size = 0;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_addr)) {
		opts->src_addr = NULL;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_port)) {
		opts->src_port = 0;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, connect_timeout)) {
		opts->connect_timeout = SPDK_SOCK_DEFAULT_CONNECT_TIMEOUT;
	}
}

/*
 * opts The opts allocated in the current library.
 * opts_user The opts passed by the caller.
 * */
static void
sock_init_opts(struct spdk_sock_opts *opts, struct spdk_sock_opts *opts_user)
{
	assert(opts);
	assert(opts_user);

	opts->opts_size = sizeof(*opts);
	spdk_sock_get_default_opts(opts);

	/* reset the size according to the user */
	opts->opts_size = opts_user->opts_size;
	if (SPDK_SOCK_OPTS_FIELD_OK(opts, priority)) {
		opts->priority = opts_user->priority;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, zcopy)) {
		opts->zcopy = opts_user->zcopy;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, ack_timeout)) {
		opts->ack_timeout = opts_user->ack_timeout;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts)) {
		opts->impl_opts = opts_user->impl_opts;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, impl_opts_size)) {
		opts->impl_opts_size = opts_user->impl_opts_size;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_addr)) {
		opts->src_addr = opts_user->src_addr;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, src_port)) {
		opts->src_port = opts_user->src_port;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, connect_timeout)) {
		opts->connect_timeout = opts_user->connect_timeout;
	}
}

struct addrinfo *
spdk_sock_posix_getaddrinfo(const char *ip, int port)
{
	struct addrinfo *res, hints = {};
	char portnum[PORTNUMLEN];
	char buf[MAX_TMPBUF];
	char *p;
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
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = getaddrinfo(ip, portnum, &hints, &res);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", gai_strerror(rc), rc);
		return NULL;
	}

	return res;
}

int
spdk_sock_posix_fd_create(struct addrinfo *res, struct spdk_sock_opts *opts,
			  struct spdk_sock_impl_opts *impl_opts)
{
	int fd;
	int val = 1;
	int rc, sz;
#if defined(__linux__)
	int to;
#endif

	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		return -errno;
	}

	sz = impl_opts->recv_buf_size;
	rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc) {
		/* Not fatal */
	}

	sz = impl_opts->send_buf_size;
	rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc) {
		/* Not fatal */
	}

	rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
	if (rc < 0) {
		goto err;
	}

	rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
	if (rc < 0) {
		goto err;
	}

#if defined(SO_PRIORITY)
	if (opts->priority) {
		rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
		if (rc < 0) {
			goto err;
		}
	}
#endif

	if (res->ai_family == AF_INET6) {
		rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
		if (rc < 0) {
			goto err;
		}
	}

	if (opts->ack_timeout) {
#if defined(__linux__)
		to = opts->ack_timeout;
		rc = setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof(to));
		if (rc < 0) {
			goto err;
		}
#else
		SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n");
#endif
	}

	return fd;

err:
	close(fd);
	return -errno;
}

static int
sock_posix_fd_connect_poll(int fd, struct spdk_sock_opts *opts, bool block)
{
	int rc, err, timeout = 0;
	struct pollfd pfd = {.fd = fd, .events = POLLOUT};
	socklen_t len = sizeof(err);

	if (opts && block) {
		assert(opts->connect_timeout <= INT_MAX);
		timeout = opts->connect_timeout ? (int)opts->connect_timeout : -1;
	}

	rc = poll(&pfd, 1, timeout);
	if (rc < 0) {
		SPDK_ERRLOG("poll() failed, errno = %d\n", errno);
		return -errno;
	}

	if (rc == 0) {
		if (block) {
			SPDK_ERRLOG("poll() timeout after %d ms\n", timeout);
			return -ETIMEDOUT;
		}

		return -EAGAIN;
	}

	rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
	if (rc < 0) {
		SPDK_ERRLOG("getsockopt() failed, errno = %d\n", errno);
		return -errno;
	}

	if (err) {
		SPDK_ERRLOG("connect() failed, err = %d\n", err);
		return -err;
	}

	if (!(pfd.revents & POLLOUT)) {
		SPDK_ERRLOG("poll() returned %d event(s) %s%s%sbut not POLLOUT\n", rc,
			    pfd.revents & POLLERR ? "POLLERR, " : "", pfd.revents & POLLHUP ? "POLLHUP, " : "",
			    pfd.revents & POLLNVAL ? "POLLNVAL, " : "");
	}

	if (!(pfd.revents & POLLOUT)) {
		return -EIO;
	}

	return 0;
}

int
spdk_sock_posix_fd_connect_poll_async(int fd)
{
	return sock_posix_fd_connect_poll(fd, NULL, false);
}

static int
sock_posix_fd_connect(int fd, struct addrinfo *res, struct spdk_sock_opts *opts, bool block)
{
	char portnum[PORTNUMLEN];
	const char *src_addr;
	uint16_t src_port;
	struct addrinfo hints, *src_ai;
	int rc;

	/* Socket address may be not assigned immediately during bind() and
	 * can return EINPROGRESS if function is invoked with O_NONBLOCK set. */
	rc = spdk_fd_clear_nonblock(fd);
	if (rc < 0) {
		return rc;
	}

	src_addr = SPDK_GET_FIELD(opts, src_addr, NULL, opts->opts_size);
	src_port = SPDK_GET_FIELD(opts, src_port, 0, opts->opts_size);
	if (src_addr != NULL || src_port != 0) {
		snprintf(portnum, sizeof(portnum), "%"PRIu16, src_port);
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICSERV | AI_NUMERICHOST | AI_PASSIVE;
		rc = getaddrinfo(src_addr, src_port > 0 ? portnum : NULL, &hints, &src_ai);
		if (rc != 0 || src_ai == NULL) {
			SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", rc != 0 ? gai_strerror(rc) : "", rc);
			return -EINVAL;
		}

		rc = bind(fd, src_ai->ai_addr, src_ai->ai_addrlen);
		if (rc < 0) {
			SPDK_ERRLOG("bind() failed errno %d (%s:%s)\n", errno, src_addr ? src_addr : "", portnum);
			freeaddrinfo(src_ai);
			return -errno;
		}

		freeaddrinfo(src_ai);
	}

	rc = spdk_fd_set_nonblock(fd);
	if (rc < 0) {
		return rc;
	}

	rc = connect(fd, res->ai_addr, res->ai_addrlen);
	if (rc < 0 && errno != EINPROGRESS) {
		SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
		return -errno;
	}

	if (!block) {
		return 0;
	}

	rc = sock_posix_fd_connect_poll(fd, opts, block);
	if (rc < 0) {
		return rc;
	}

	rc = spdk_fd_clear_nonblock(fd);
	if (rc < 0) {
		return rc;
	}

	return 0;
}

int
spdk_sock_posix_fd_connect_async(int fd, struct addrinfo *res, struct spdk_sock_opts *opts)
{
	return sock_posix_fd_connect(fd, res, opts, false);
}

int
spdk_sock_posix_fd_connect(int fd, struct addrinfo *res, struct spdk_sock_opts *opts)
{
	return sock_posix_fd_connect(fd, res, opts, true);
}

struct spdk_sock *
spdk_sock_connect(const char *ip, int port, const char *impl_name)
{
	struct spdk_sock_opts opts;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	return spdk_sock_connect_ext(ip, port, impl_name, &opts);
}

static struct spdk_sock *
sock_connect_ext(const char *ip, int port, const char *_impl_name, struct spdk_sock_opts *opts,
		 bool async, spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock *sock;
	struct spdk_sock_opts opts_local;
	const char *impl_name = NULL;

	assert(async || (!cb_fn && !cb_arg));

	if (opts == NULL) {
		SPDK_ERRLOG("the opts should not be NULL pointer\n");
		return NULL;
	}

	if (_impl_name) {
		impl_name = _impl_name;
	} else if (g_default_impl) {
		impl_name = g_default_impl->name;
	}

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		if (impl_name && strncmp(impl_name, impl->name, strlen(impl->name) + 1) == 0) {
			break;
		}
	}

	if (!impl) {
		SPDK_ERRLOG("Cannot find %s sock implementation\n", impl_name ? impl_name : "any");
		return NULL;
	}

	SPDK_DEBUGLOG(sock, "Creating a client socket using impl %s\n", impl->name);
	sock_init_opts(&opts_local, opts);
	if (opts_local.connect_timeout > INT_MAX) {
		SPDK_ERRLOG("connect_timeout opt cannot exceed INT_MAX\n");
		return NULL;
	}

	if (async && impl->connect_async) {
		sock = impl->connect_async(ip, port, &opts_local, cb_fn, cb_arg);
	} else {
		sock = impl->connect(ip, port, &opts_local);
	}

	if (!sock) {
		return NULL;
	}

	/* Copy the contents, both the two structures are the same ABI version */
	memcpy(&sock->opts, &opts_local, sizeof(sock->opts));
	/* Clear out impl_opts to make sure we don't keep reference to a dangling
	 * pointer */
	sock->opts.impl_opts = NULL;
	sock->net_impl = impl;
	TAILQ_INIT(&sock->queued_reqs);
	TAILQ_INIT(&sock->pending_reqs);

	/* Invoke cb_fn only in case of fallback to sync version. */
	if (cb_fn && async && !impl->connect_async) {
		cb_fn(cb_arg, 0);
	}

	return sock;
}

struct spdk_sock *
spdk_sock_connect_ext(const char *ip, int port, const char *_impl_name, struct spdk_sock_opts *opts)
{
	return sock_connect_ext(ip, port, _impl_name, opts, false, NULL, NULL);
}

struct spdk_sock *
spdk_sock_connect_async(const char *ip, int port, const char *_impl_name,
			struct spdk_sock_opts *opts, spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	return sock_connect_ext(ip, port, _impl_name, opts, true, cb_fn, cb_arg);
}

struct spdk_sock *
spdk_sock_listen(const char *ip, int port, const char *impl_name)
{
	struct spdk_sock_opts opts;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	return spdk_sock_listen_ext(ip, port, impl_name, &opts);
}

struct spdk_sock *
spdk_sock_listen_ext(const char *ip, int port, const char *_impl_name, struct spdk_sock_opts *opts)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock *sock;
	struct spdk_sock_opts opts_local;
	const char *impl_name = NULL;

	if (opts == NULL) {
		SPDK_ERRLOG("the opts should not be NULL pointer\n");
		return NULL;
	}

	if (_impl_name) {
		impl_name = _impl_name;
	} else if (g_default_impl) {
		impl_name = g_default_impl->name;
	}

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		if (impl_name && strncmp(impl_name, impl->name, strlen(impl->name) + 1) == 0) {
			break;
		}
	}

	if (!impl) {
		SPDK_ERRLOG("Cannot find %s sock implementation\n", impl_name ? impl_name : "any");
		return NULL;
	}

	SPDK_DEBUGLOG(sock, "Creating a listening socket using impl %s\n", impl->name);
	sock_init_opts(&opts_local, opts);
	sock = impl->listen(ip, port, &opts_local);
	if (!sock) {
		return NULL;
	}

	/* Copy the contents, both the two structures are the same ABI version */
	memcpy(&sock->opts, &opts_local, sizeof(sock->opts));
	/* Clear out impl_opts to make sure we don't keep reference to a dangling
	 * pointer */
	sock->opts.impl_opts = NULL;
	sock->net_impl = impl;
	/* Don't need to initialize the request queues for listen
	 * sockets. */
	return sock;
}

struct spdk_sock *
spdk_sock_accept(struct spdk_sock *sock)
{
	struct spdk_sock *new_sock;

	new_sock = sock->net_impl->accept(sock);
	if (new_sock != NULL) {
		/* Inherit the opts from the "accept sock" */
		new_sock->opts = sock->opts;
		memcpy(&new_sock->opts, &sock->opts, sizeof(new_sock->opts));
		new_sock->net_impl = sock->net_impl;
		TAILQ_INIT(&new_sock->queued_reqs);
		TAILQ_INIT(&new_sock->pending_reqs);
	}

	return new_sock;
}

int
spdk_sock_close(struct spdk_sock **_sock)
{
	struct spdk_sock *sock = *_sock;

	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	if (sock->cb_fn != NULL) {
		/* This sock is still part of a sock_group. */
		errno = EBUSY;
		return -1;
	}

	/* Beyond this point the socket is considered closed. */
	*_sock = NULL;

	sock->flags.closed = true;

	if (sock->cb_cnt > 0) {
		/* Let the callback unwind before destroying the socket */
		return 0;
	}

	spdk_sock_abort_requests(sock);

	return sock->net_impl->close(sock);
}

ssize_t
spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	if (sock == NULL || sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->recv(sock, buf, len);
}

ssize_t
spdk_sock_readv(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	if (sock == NULL || sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->readv(sock, iov, iovcnt);
}

ssize_t
spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	if (sock == NULL || sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->writev(sock, iov, iovcnt);
}

void
spdk_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	assert(req->cb_fn != NULL);

	if (sock == NULL || sock->flags.closed) {
		req->cb_fn(req->cb_arg, -EBADF);
		return;
	}

	sock->net_impl->writev_async(sock, req);
}

int
spdk_sock_recv_next(struct spdk_sock *sock, void **buf, void **ctx)
{
	if (sock == NULL || sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	if (sock->group_impl == NULL) {
		errno = ENOTSUP;
		return -1;
	}

	return sock->net_impl->recv_next(sock, buf, ctx);
}

int
spdk_sock_flush(struct spdk_sock *sock)
{
	if (sock == NULL || sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->flush(sock);
}

int
spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes)
{
	return sock->net_impl->set_recvlowat(sock, nbytes);
}

int
spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_recvbuf(sock, sz);
}

int
spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_sendbuf(sock, sz);
}

bool
spdk_sock_is_ipv6(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv6(sock);
}

bool
spdk_sock_is_ipv4(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv4(sock);
}

bool
spdk_sock_is_connected(struct spdk_sock *sock)
{
	return sock->net_impl->is_connected(sock);
}

struct spdk_sock_group *
spdk_sock_group_create(void *ctx)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock_group *group;
	struct spdk_sock_group_impl *group_impl;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	STAILQ_INIT(&group->group_impls);
	STAILQ_INIT(&group->pool);

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		group_impl = impl->group_impl_create();
		if (group_impl != NULL) {
			STAILQ_INSERT_TAIL(&group->group_impls, group_impl, link);
			TAILQ_INIT(&group_impl->socks);
			group_impl->net_impl = impl;
			group_impl->group = group;
		}
	}

	group->ctx = ctx;

	return group;
}

void *
spdk_sock_group_get_ctx(struct spdk_sock_group *group)
{
	if (group == NULL) {
		return NULL;
	}

	return group->ctx;
}

int
spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			 spdk_sock_cb cb_fn, void *cb_arg)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc;

	if (cb_fn == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (sock->group_impl != NULL) {
		/*
		 * This sock is already part of a sock_group.
		 */
		errno = EINVAL;
		return -1;
	}

	group_impl = sock_get_group_impl_from_group(sock, group);
	if (group_impl == NULL) {
		errno = EINVAL;
		return -1;
	}

	rc = group_impl->net_impl->group_impl_add_sock(group_impl, sock);
	if (rc != 0) {
		return rc;
	}

	TAILQ_INSERT_TAIL(&group_impl->socks, sock, link);
	sock->group_impl = group_impl;
	sock->cb_fn = cb_fn;
	sock->cb_arg = cb_arg;

	return 0;
}

int
spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc;

	group_impl = sock_get_group_impl_from_group(sock, group);
	if (group_impl == NULL) {
		errno = EINVAL;
		return -1;
	}

	assert(group_impl == sock->group_impl);

	rc = group_impl->net_impl->group_impl_remove_sock(group_impl, sock);
	if (rc == 0) {
		TAILQ_REMOVE(&group_impl->socks, sock, link);
		sock->group_impl = NULL;
		sock->cb_fn = NULL;
		sock->cb_arg = NULL;
	}

	return rc;
}

int
spdk_sock_group_provide_buf(struct spdk_sock_group *group, void *buf, size_t len, void *ctx)
{
	struct spdk_sock_group_provided_buf *provided;

	provided = (struct spdk_sock_group_provided_buf *)buf;

	provided->len = len;
	provided->ctx = ctx;
	STAILQ_INSERT_HEAD(&group->pool, provided, link);

	return 0;
}

size_t
spdk_sock_group_get_buf(struct spdk_sock_group *group, void **buf, void **ctx)
{
	struct spdk_sock_group_provided_buf *provided;

	provided = STAILQ_FIRST(&group->pool);
	if (provided == NULL) {
		*buf = NULL;
		return 0;
	}
	STAILQ_REMOVE_HEAD(&group->pool, link);

	*buf = provided;
	*ctx = provided->ctx;
	return provided->len;
}

int
spdk_sock_group_poll(struct spdk_sock_group *group)
{
	return spdk_sock_group_poll_count(group, MAX_EVENTS_PER_POLL);
}

static int
sock_group_impl_poll_count(struct spdk_sock_group_impl *group_impl,
			   struct spdk_sock_group *group,
			   int max_events)
{
	struct spdk_sock *socks[MAX_EVENTS_PER_POLL];
	int num_events, i;

	if (TAILQ_EMPTY(&group_impl->socks)) {
		return 0;
	}

	num_events = group_impl->net_impl->group_impl_poll(group_impl, max_events, socks);
	if (num_events == -1) {
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		struct spdk_sock *sock = socks[i];
		assert(sock->cb_fn != NULL);
		sock->cb_fn(sock->cb_arg, group, sock);
	}

	return num_events;
}

int
spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc, num_events = 0;

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

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		rc = sock_group_impl_poll_count(group_impl, group, max_events);
		if (rc < 0) {
			num_events = -1;
			SPDK_ERRLOG("group_impl_poll_count for net(%s) failed\n",
				    group_impl->net_impl->name);
		} else if (num_events >= 0) {
			num_events += rc;
		}
	}

	return num_events;
}

int
spdk_sock_group_close(struct spdk_sock_group **group)
{
	struct spdk_sock_group_impl *group_impl = NULL, *tmp;
	int rc;

	if (*group == NULL) {
		errno = EBADF;
		return -1;
	}

	STAILQ_FOREACH_SAFE(group_impl, &(*group)->group_impls, link, tmp) {
		if (!TAILQ_EMPTY(&group_impl->socks)) {
			errno = EBUSY;
			return -1;
		}
	}

	STAILQ_FOREACH_SAFE(group_impl, &(*group)->group_impls, link, tmp) {
		rc = group_impl->net_impl->group_impl_close(group_impl);
		if (rc != 0) {
			SPDK_ERRLOG("group_impl_close for net failed\n");
		}
	}

	free(*group);
	*group = NULL;

	return 0;
}

static inline struct spdk_net_impl *
sock_get_impl_by_name(const char *impl_name)
{
	struct spdk_net_impl *impl;

	assert(impl_name != NULL);
	STAILQ_FOREACH(impl, &g_net_impls, link) {
		if (0 == strcmp(impl_name, impl->name)) {
			return impl;
		}
	}

	return NULL;
}

int
spdk_sock_impl_get_opts(const char *impl_name, struct spdk_sock_impl_opts *opts, size_t *len)
{
	struct spdk_net_impl *impl;

	if (!impl_name || !opts || !len) {
		errno = EINVAL;
		return -1;
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		errno = EINVAL;
		return -1;
	}

	if (!impl->get_opts) {
		errno = ENOTSUP;
		return -1;
	}

	return impl->get_opts(opts, len);
}

int
spdk_sock_impl_set_opts(const char *impl_name, const struct spdk_sock_impl_opts *opts, size_t len)
{
	struct spdk_net_impl *impl;

	if (!impl_name || !opts) {
		errno = EINVAL;
		return -1;
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		errno = EINVAL;
		return -1;
	}

	if (!impl->set_opts) {
		errno = ENOTSUP;
		return -1;
	}

	return impl->set_opts(opts, len);
}

void
spdk_sock_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_net_impl *impl;
	struct spdk_sock_impl_opts opts;
	size_t len;

	assert(w != NULL);

	spdk_json_write_array_begin(w);

	if (g_default_impl) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "sock_set_default_impl");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "impl_name", g_default_impl->name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	STAILQ_FOREACH(impl, &g_net_impls, link) {
		if (!impl->get_opts) {
			continue;
		}

		len = sizeof(opts);
		if (impl->get_opts(&opts, &len) == 0) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "sock_impl_set_options");
			spdk_json_write_named_object_begin(w, "params");
			spdk_json_write_named_string(w, "impl_name", impl->name);
			spdk_json_write_named_uint32(w, "recv_buf_size", opts.recv_buf_size);
			spdk_json_write_named_uint32(w, "send_buf_size", opts.send_buf_size);
			spdk_json_write_named_bool(w, "enable_recv_pipe", opts.enable_recv_pipe);
			spdk_json_write_named_bool(w, "enable_quickack", opts.enable_quickack);
			spdk_json_write_named_uint32(w, "enable_placement_id", opts.enable_placement_id);
			spdk_json_write_named_bool(w, "enable_zerocopy_send_server", opts.enable_zerocopy_send_server);
			spdk_json_write_named_bool(w, "enable_zerocopy_send_client", opts.enable_zerocopy_send_client);
			spdk_json_write_named_uint32(w, "zerocopy_threshold", opts.zerocopy_threshold);
			spdk_json_write_named_uint32(w, "tls_version", opts.tls_version);
			spdk_json_write_named_bool(w, "enable_ktls", opts.enable_ktls);
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		} else {
			SPDK_ERRLOG("Failed to get socket options for socket implementation %s\n", impl->name);
		}
	}

	spdk_json_write_array_end(w);
}

void
spdk_net_impl_register(struct spdk_net_impl *impl)
{
	STAILQ_INSERT_HEAD(&g_net_impls, impl, link);
}

int
spdk_sock_set_default_impl(const char *impl_name)
{
	struct spdk_net_impl *impl;

	if (!impl_name) {
		errno = EINVAL;
		return -1;
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		errno = EINVAL;
		return -1;
	}

	if (impl == g_default_impl) {
		return 0;
	}

	if (g_default_impl) {
		SPDK_DEBUGLOG(sock, "Change the default sock impl from %s to %s\n", g_default_impl->name,
			      impl->name);
	} else {
		SPDK_DEBUGLOG(sock, "Set default sock implementation to %s\n", impl_name);
	}

	g_default_impl = impl;

	return 0;
}

const char *
spdk_sock_get_default_impl(void)
{
	if (g_default_impl) {
		return g_default_impl->name;
	}

	return NULL;
}

int
spdk_sock_group_register_interrupt(struct spdk_sock_group *group, uint32_t events,
				   spdk_interrupt_fn fn,
				   void *arg, const char *name)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc;

	assert(group != NULL);
	assert(fn != NULL);

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		rc = group_impl->net_impl->group_impl_register_interrupt(group_impl, events, fn, arg, name);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

void
spdk_sock_group_unregister_interrupt(struct spdk_sock_group *group)
{
	struct spdk_sock_group_impl *group_impl = NULL;

	assert(group != NULL);

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		group_impl->net_impl->group_impl_unregister_interrupt(group_impl);
	}
}

SPDK_LOG_REGISTER_COMPONENT(sock)

static void
sock_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"SOCK_REQ_QUEUE", TRACE_SOCK_REQ_QUEUE,
			OWNER_TYPE_SOCK, OBJECT_SOCK_REQ, 1,
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
			}
		},
		{
			"SOCK_REQ_PEND", TRACE_SOCK_REQ_PEND,
			OWNER_TYPE_SOCK, OBJECT_SOCK_REQ, 0,
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
			}
		},
		{
			"SOCK_REQ_COMPLETE", TRACE_SOCK_REQ_COMPLETE,
			OWNER_TYPE_SOCK, OBJECT_SOCK_REQ, 0,
			{
				{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
			}
		},
	};

	spdk_trace_register_owner_type(OWNER_TYPE_SOCK, 's');
	spdk_trace_register_object(OBJECT_SOCK_REQ, 's');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
SPDK_TRACE_REGISTER_FN(sock_trace, "sock", TRACE_GROUP_SOCK)
