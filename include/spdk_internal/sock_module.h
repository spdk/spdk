/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 */

/** \file
 * TCP network implementation abstraction layer
 */

#ifndef SPDK_INTERNAL_SOCK_MODULE_H
#define SPDK_INTERNAL_SOCK_MODULE_H

#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk/queue.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk_internal/trace_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_EVENTS_PER_POLL 32
#define DEFAULT_SOCK_PRIORITY 0
#define MIN_SOCK_PIPE_SIZE 1024
#define DEFAULT_SO_RCVBUF_SIZE (2 * 1024 * 1024)
#define DEFAULT_SO_SNDBUF_SIZE (2 * 1024 * 1024)
#define MIN_SO_RCVBUF_SIZE (4 * 1024)
#define MIN_SO_SNDBUF_SIZE (4 * 1024)
#define IOV_BATCH_SIZE 64

struct spdk_sock {
	struct spdk_net_impl		*net_impl;
	struct spdk_sock_opts		opts;
	struct spdk_sock_group_impl	*group_impl;
	TAILQ_ENTRY(spdk_sock)		link;

	TAILQ_HEAD(, spdk_sock_request)	queued_reqs;
	TAILQ_HEAD(, spdk_sock_request)	pending_reqs;
	struct spdk_sock_request	*read_req;
	int				queued_iovcnt;
	int				cb_cnt;
	spdk_sock_cb			cb_fn;
	void				*cb_arg;
	struct {
		uint8_t		closed		: 1;
		uint8_t		reserved	: 7;
	} flags;
	struct spdk_sock_impl_opts	impl_opts;
};

struct spdk_sock_group_provided_buf {
	size_t						len;
	void						*ctx;
	STAILQ_ENTRY(spdk_sock_group_provided_buf)	link;
};

struct spdk_sock_group {
	STAILQ_HEAD(, spdk_sock_group_impl)	group_impls;
	STAILQ_HEAD(, spdk_sock_group_provided_buf) pool;
	void					*ctx;
};

struct spdk_sock_group_impl {
	struct spdk_net_impl			*net_impl;
	struct spdk_sock_group			*group;
	TAILQ_HEAD(, spdk_sock)			socks;
	STAILQ_ENTRY(spdk_sock_group_impl)	link;
};

struct spdk_sock_map {
	STAILQ_HEAD(, spdk_sock_placement_id_entry) entries;
	pthread_mutex_t mtx;
};

struct spdk_net_impl {
	const char *name;

	int (*getaddr)(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport, char *caddr,
		       int clen, uint16_t *cport);
	const char *(*get_interface_name)(struct spdk_sock *sock);
	int32_t (*get_numa_id)(struct spdk_sock *sock);
	struct spdk_sock *(*connect)(const char *ip, int port, struct spdk_sock_opts *opts);
	struct spdk_sock *(*connect_async)(const char *ip, int port, struct spdk_sock_opts *opts,
					   spdk_sock_connect_cb_fn cb_fn, void *cb_arg);
	struct spdk_sock *(*listen)(const char *ip, int port, struct spdk_sock_opts *opts);
	struct spdk_sock *(*accept)(struct spdk_sock *sock);
	int (*close)(struct spdk_sock *sock);
	ssize_t (*recv)(struct spdk_sock *sock, void *buf, size_t len);
	ssize_t (*readv)(struct spdk_sock *sock, struct iovec *iov, int iovcnt);
	ssize_t (*writev)(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

	int (*recv_next)(struct spdk_sock *sock, void **buf, void **ctx);
	void (*writev_async)(struct spdk_sock *sock, struct spdk_sock_request *req);
	void (*readv_async)(struct spdk_sock *sock, struct spdk_sock_request *req);
	int (*flush)(struct spdk_sock *sock);

	int (*set_recvlowat)(struct spdk_sock *sock, int nbytes);
	int (*set_recvbuf)(struct spdk_sock *sock, int sz);
	int (*set_sendbuf)(struct spdk_sock *sock, int sz);

	bool (*is_ipv6)(struct spdk_sock *sock);
	bool (*is_ipv4)(struct spdk_sock *sock);
	bool (*is_connected)(struct spdk_sock *sock);

	struct spdk_sock_group_impl *(*group_impl_get_optimal)(struct spdk_sock *sock,
			struct spdk_sock_group_impl *hint);
	struct spdk_sock_group_impl *(*group_impl_create)(void);
	int (*group_impl_add_sock)(struct spdk_sock_group_impl *group, struct spdk_sock *sock);
	int (*group_impl_remove_sock)(struct spdk_sock_group_impl *group, struct spdk_sock *sock);
	int (*group_impl_poll)(struct spdk_sock_group_impl *group, int max_events,
			       struct spdk_sock **socks);
	int (*group_impl_register_interrupt)(struct spdk_sock_group_impl *group, uint32_t events,
					     spdk_interrupt_fn fn, void *arg, const char *name);
	void (*group_impl_unregister_interrupt)(struct spdk_sock_group_impl *group);
	int (*group_impl_close)(struct spdk_sock_group_impl *group);

	int (*get_opts)(struct spdk_sock_impl_opts *opts, size_t *len);
	int (*set_opts)(const struct spdk_sock_impl_opts *opts, size_t len);

	STAILQ_ENTRY(spdk_net_impl) link;
};

void spdk_net_impl_register(struct spdk_net_impl *impl);

#define SPDK_NET_IMPL_REGISTER(name, impl) \
static void __attribute__((constructor)) net_impl_register_##name(void) \
{ \
	spdk_net_impl_register(impl); \
}

#define SPDK_NET_IMPL_REGISTER_DEFAULT(name, impl) \
static void __attribute__((constructor)) net_impl_register_default_##name(void) \
{ \
	spdk_net_impl_register(impl); \
	spdk_sock_set_default_impl(SPDK_STRINGIFY(name)); \
}

size_t spdk_sock_group_get_buf(struct spdk_sock_group *group, void **buf, void **ctx);

static inline void
spdk_sock_request_queue(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	assert(req->internal.curr_list == NULL);
	if (spdk_trace_tpoint_enabled(TRACE_SOCK_REQ_QUEUE)) {
		uint64_t len = 0;
		int i;

		for (i = 0; i < req->iovcnt; i++) {
			len += SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
		}
		spdk_trace_record(TRACE_SOCK_REQ_QUEUE, 0, len, (uintptr_t)req, (uintptr_t)req->cb_arg);
	}
	TAILQ_INSERT_TAIL(&sock->queued_reqs, req, internal.link);
#ifdef DEBUG
	req->internal.curr_list = &sock->queued_reqs;
#endif
	sock->queued_iovcnt += req->iovcnt;
}

static inline void
spdk_sock_request_pend(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	assert(req->internal.curr_list == &sock->queued_reqs);
	spdk_trace_record(TRACE_SOCK_REQ_PEND, 0, 0, (uintptr_t)req, (uintptr_t)req->cb_arg);
	TAILQ_REMOVE(&sock->queued_reqs, req, internal.link);
	assert(sock->queued_iovcnt >= req->iovcnt);
	sock->queued_iovcnt -= req->iovcnt;
	TAILQ_INSERT_TAIL(&sock->pending_reqs, req, internal.link);
#ifdef DEBUG
	req->internal.curr_list = &sock->pending_reqs;
#endif
}

static inline int
spdk_sock_request_complete(struct spdk_sock *sock, struct spdk_sock_request *req, int err)
{
	bool closed;
	int rc = 0;

	spdk_trace_record(TRACE_SOCK_REQ_COMPLETE, 0, 0, (uintptr_t)req, (uintptr_t)req->cb_arg);
	req->internal.offset = 0;
	req->internal.zcopy_idx = 0;
	req->internal.pending_zcopy = false;

	closed = sock->flags.closed;
	sock->cb_cnt++;
	req->cb_fn(req->cb_arg, err);
	assert(sock->cb_cnt > 0);
	sock->cb_cnt--;

	if (sock->cb_cnt == 0 && !closed && sock->flags.closed) {
		/* The user closed the socket in response to a callback above. */
		rc = -1;
		spdk_sock_close(&sock);
	}

	return rc;
}

static inline int
spdk_sock_request_put(struct spdk_sock *sock, struct spdk_sock_request *req, int err)
{
	assert(req->internal.curr_list == &sock->pending_reqs);
	TAILQ_REMOVE(&sock->pending_reqs, req, internal.link);
#ifdef DEBUG
	req->internal.curr_list = NULL;
#endif
	return spdk_sock_request_complete(sock, req, err);
}

static inline int
spdk_sock_abort_requests(struct spdk_sock *sock)
{
	struct spdk_sock_request *req;
	bool closed;
	int rc = 0;

	closed = sock->flags.closed;
	sock->cb_cnt++;

	req = TAILQ_FIRST(&sock->pending_reqs);
	while (req) {
		assert(req->internal.curr_list == &sock->pending_reqs);
		TAILQ_REMOVE(&sock->pending_reqs, req, internal.link);
#ifdef DEBUG
		req->internal.curr_list = NULL;
#endif

		req->cb_fn(req->cb_arg, -ECANCELED);

		req = TAILQ_FIRST(&sock->pending_reqs);
	}

	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		assert(req->internal.curr_list == &sock->queued_reqs);
		TAILQ_REMOVE(&sock->queued_reqs, req, internal.link);
#ifdef DEBUG
		req->internal.curr_list = NULL;
#endif

		assert(sock->queued_iovcnt >= req->iovcnt);
		sock->queued_iovcnt -= req->iovcnt;

		req->cb_fn(req->cb_arg, -ECANCELED);

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	req = sock->read_req;
	if (req != NULL) {
		sock->read_req = NULL;
		req->cb_fn(req->cb_arg, -ECANCELED);
	}
	assert(sock->cb_cnt > 0);
	sock->cb_cnt--;

	assert(TAILQ_EMPTY(&sock->queued_reqs));
	assert(TAILQ_EMPTY(&sock->pending_reqs));

	if (sock->cb_cnt == 0 && !closed && sock->flags.closed) {
		/* The user closed the socket in response to a callback above. */
		rc = -1;
		spdk_sock_close(&sock);
	}

	return rc;
}

static inline int
spdk_sock_prep_req(struct spdk_sock_request *req, struct iovec *iovs, int index,
		   uint64_t *num_bytes)
{
	unsigned int offset;
	int iovcnt, i;

	assert(index < IOV_BATCH_SIZE);
	offset = req->internal.offset;
	iovcnt = index;

	for (i = 0; i < req->iovcnt; i++) {
		/* Consume any offset first */
		if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
			offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
			continue;
		}

		iovs[iovcnt].iov_base = (uint8_t *)SPDK_SOCK_REQUEST_IOV(req, i)->iov_base + offset;
		iovs[iovcnt].iov_len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;
		if (num_bytes != NULL) {
			*num_bytes += iovs[iovcnt].iov_len;
		}

		iovcnt++;
		offset = 0;

		if (iovcnt >= IOV_BATCH_SIZE) {
			break;
		}
	}

	return iovcnt;
}

static inline int
spdk_sock_prep_reqs(struct spdk_sock *_sock, struct iovec *iovs, int index,
		    struct spdk_sock_request **last_req, int *flags)
{
	int iovcnt;
	struct spdk_sock_request *req;
	uint64_t total = 0;

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
		iovcnt = spdk_sock_prep_req(req, iovs, iovcnt, &total);
		if (iovcnt >= IOV_BATCH_SIZE) {
			break;
		}

		if (last_req != NULL) {
			*last_req = req;
		}
		req = TAILQ_NEXT(req, internal.link);
	}

end:

#if defined(MSG_ZEROCOPY)
	/* if data size < zerocopy_threshold, remove MSG_ZEROCOPY flag */
	if (total < _sock->impl_opts.zerocopy_threshold && flags != NULL) {
		*flags = *flags & (~MSG_ZEROCOPY);
	}
#endif

	return iovcnt;
}

static inline void
spdk_sock_get_placement_id(int fd, enum spdk_placement_mode mode, int *placement_id)
{
	*placement_id = -1;

	switch (mode) {
	case PLACEMENT_NONE:
		break;
	case PLACEMENT_MARK:
	case PLACEMENT_NAPI: {
#if defined(SO_INCOMING_NAPI_ID)
		socklen_t len = sizeof(int);

		int rc = getsockopt(fd, SOL_SOCKET, SO_INCOMING_NAPI_ID, placement_id, &len);
		if (rc == -1) {
			SPDK_ERRLOG("getsockopt() failed: %s\n", strerror(errno));
			assert(false);
		}
#endif
		break;
	}
	case PLACEMENT_CPU: {
#if defined(SO_INCOMING_CPU)
		socklen_t len = sizeof(int);

		int rc = getsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, placement_id, &len);
		if (rc == -1) {
			SPDK_ERRLOG("getsockopt() failed: %s\n", strerror(errno));
			assert(false);
		}
#endif
		break;
	}
	default:
		break;
	}
}

/**
 * Converts ip and port into address.
 *
 * Use freeaddrinfo() when returned object is no longer needed.
 *
 * \return addrinfo object or NULL in case of any failures.
 */
struct addrinfo *spdk_sock_posix_getaddrinfo(const char *ip, int port);

/**
 * Creates sock file descriptor.
 *
 * Use close() when returned fd is no longer needed.
 *
 * \return 0 on success, negative errno value on failure.
 */
int spdk_sock_posix_fd_create(struct addrinfo *res, struct spdk_sock_opts *opts,
			      struct spdk_sock_impl_opts *impl_opts);

/**
 * Connects the socket to the address.
 *
 * On success O_NONBLOCK is cleared otherwise property value is undefined.
 *
 * \return 0 on success, negative errno value on failure.
 */
int spdk_sock_posix_fd_connect(int fd, struct addrinfo *res, struct spdk_sock_opts *opts);

/**
 * Initiates the socket connection.
 *
 * On success O_NONBLOCK is set otherwise property value is undefined.
 *
 * User must use \ref spdk_sock_posix_fd_connect_poll_async to determine connection status.
 *
 * \return 0 on success, negative errno value on failure.
 */
int spdk_sock_posix_fd_connect_async(int fd, struct addrinfo *res, struct spdk_sock_opts *opts);

/**
 * Polls the socket connection status.
 *
 * \return 0 on success, negative errno value on failure.
 */
int spdk_sock_posix_fd_connect_poll_async(int fd);

/**
 * Insert a group into the placement map.
 * If the group is already in the map, take a reference.
 */
int spdk_sock_map_insert(struct spdk_sock_map *map, int placement_id,
			 struct spdk_sock_group_impl *group_impl);

/**
 * Release a reference for the given placement_id. If the reference count goes to 0, the
 * entry will no longer be associated with a group.
 */
void spdk_sock_map_release(struct spdk_sock_map *map, int placement_id);

/**
 * Look up the group for the given placement_id.
 */
int spdk_sock_map_lookup(struct spdk_sock_map *map, int placement_id,
			 struct spdk_sock_group_impl **group_impl, struct spdk_sock_group_impl *hint);

/**
 * Find a placement id with no associated group
 */
int spdk_sock_map_find_free(struct spdk_sock_map *map);

/**
 * Clean up all memory associated with the given map
 */
void spdk_sock_map_cleanup(struct spdk_sock_map *map);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_INTERNAL_SOCK_MODULE_H */
