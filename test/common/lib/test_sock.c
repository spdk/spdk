/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/sock_module.h"
#include "spdk_internal/mock.h"

#define MAX_SOCK_GROUP_ENTRIES 4

struct test_sock_group {
	struct spdk_sock		*entries[MAX_SOCK_GROUP_ENTRIES];
	int				num_entries;
	spdk_sock_cb			rx_cb;
};

DEFINE_STUB(spdk_sock_getaddr, int, (struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
				     char *caddr, int clen, uint16_t *cport), 0);
DEFINE_STUB_V(spdk_sock_get_default_opts, (struct spdk_sock_opts *opts));
DEFINE_STUB(spdk_sock_impl_get_opts, int, (const char *impl_name, struct spdk_sock_impl_opts *opts,
		size_t *len), 0);
DEFINE_STUB(spdk_sock_recv, ssize_t, (struct spdk_sock *sock, void *buf, size_t len), 1);
DEFINE_STUB(spdk_sock_writev, ssize_t, (struct spdk_sock *sock, struct iovec *iov, int iovcnt), 0);
DEFINE_STUB(spdk_sock_readv, ssize_t, (struct spdk_sock *sock, struct iovec *iov, int iovcnt), 0);
DEFINE_STUB(spdk_sock_set_recvlowat, int, (struct spdk_sock *sock, int nbytes), 0);
DEFINE_STUB(spdk_sock_set_recvbuf, int, (struct spdk_sock *sock, int sz), 0);
DEFINE_STUB(spdk_sock_set_sendbuf, int, (struct spdk_sock *sock, int sz), 0);
DEFINE_STUB_V(spdk_sock_writev_async, (struct spdk_sock *sock, struct spdk_sock_request *req));
DEFINE_STUB_V(spdk_sock_writev_async_ext, (struct spdk_sock *sock, struct spdk_sock_request *req,
		const struct spdk_sock_request_opts *opts));
DEFINE_STUB(spdk_sock_flush, int, (struct spdk_sock *sock), 0);
DEFINE_STUB(spdk_sock_is_ipv6, bool, (struct spdk_sock *sock), false);
DEFINE_STUB(spdk_sock_is_ipv4, bool, (struct spdk_sock *sock), true);
DEFINE_STUB(spdk_sock_is_connected, bool, (struct spdk_sock *sock), true);

DEFINE_RETURN_MOCK(spdk_sock_group_create, struct spdk_sock_group *);
struct spdk_sock_group *
spdk_sock_group_create(const struct spdk_sock_group_opts *opts)
{
	struct test_sock_group *group;

	HANDLE_RETURN_MOCK(spdk_sock_group_create);

	group = calloc(1, sizeof(*group));
	SPDK_CU_ASSERT_FATAL(group != NULL);

	if (opts) {
		group->rx_cb = opts->rx_cb;
	}

	return (struct spdk_sock_group *)group;
}

DEFINE_RETURN_MOCK(spdk_sock_group_add_sock, int);
int
spdk_sock_group_add_sock(struct spdk_sock_group *_group, struct spdk_sock *sock)
{
	struct test_sock_group *group;

	HANDLE_RETURN_MOCK(spdk_sock_group_add_sock);

	group = (struct test_sock_group *)_group;

	/* a bit of a hack, but it's useful during spdk_sock_close below */
	sock->group_impl = (struct spdk_sock_group_impl *)_group;

	SPDK_CU_ASSERT_FATAL(group->num_entries < MAX_SOCK_GROUP_ENTRIES);

	group->entries[group->num_entries++] = sock;

	return 0;
}

DEFINE_RETURN_MOCK(spdk_sock_group_remove_sock, int);
int
spdk_sock_group_remove_sock(struct spdk_sock_group *_group, struct spdk_sock *sock)
{
	struct test_sock_group *group;
	struct spdk_sock *entries[MAX_SOCK_GROUP_ENTRIES];
	int num_entries, i;

	HANDLE_RETURN_MOCK(spdk_sock_group_remove_sock);

	sock->group_impl = NULL;

	group = (struct test_sock_group *)_group;
	num_entries = 0;

	for (i = 0; i < group->num_entries; i++) {
		if (group->entries[i] != sock) {
			entries[num_entries++] = group->entries[i];
		}
	}

	memcpy(group->entries, entries, sizeof(struct spdk_sock *) * num_entries);
	group->num_entries = num_entries;

	return 0;
}

DEFINE_RETURN_MOCK(spdk_sock_group_poll, int);
int
spdk_sock_group_poll(struct spdk_sock_group *_group)
{
	struct test_sock_group *group;
	int i;

	HANDLE_RETURN_MOCK(spdk_sock_group_poll);

	group = (struct test_sock_group *)_group;

	for (i = 0; i < group->num_entries; i++) {
		group->rx_cb(group->entries[i]->cb_arg, _group, group->entries[i]);
	}

	return 0;
}

DEFINE_RETURN_MOCK(spdk_sock_group_close, int);
int
spdk_sock_group_close(struct spdk_sock_group **group)
{
	HANDLE_RETURN_MOCK(spdk_sock_group_close);

	free(*group);
	*group = NULL;

	return 0;
}

DEFINE_RETURN_MOCK(spdk_sock_connect, struct spdk_sock *);
struct spdk_sock *
spdk_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts,
		  spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct spdk_sock *sock;
	int rc;

	HANDLE_RETURN_MOCK(spdk_sock_connect);

	SPDK_CU_ASSERT_FATAL(opts->group != NULL);

	CU_ASSERT(port == 23);
	CU_ASSERT(opts->opts_size == sizeof(*opts));
	CU_ASSERT(!strcmp(ip, "192.168.1.78"));

	sock = calloc(1, sizeof(*sock));
	SPDK_CU_ASSERT_FATAL(sock != NULL);

	sock->cb_arg = opts->user_ctx;

	rc = spdk_sock_group_add_sock(opts->group, sock);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	return sock;
}

DEFINE_RETURN_MOCK(spdk_sock_close, int);
int
spdk_sock_close(struct spdk_sock **_sock)
{
	struct spdk_sock *sock;

	HANDLE_RETURN_MOCK(spdk_sock_close);

	sock = *_sock;

	if (sock == (struct spdk_sock *)0xDEADBEEF) {
		/* To simplify tests, we allow this special value and just don't close it. */
		*_sock = NULL;
		return 0;
	}

	SPDK_CU_ASSERT_FATAL(sock != NULL);

	if (sock->group_impl != NULL) {
		spdk_sock_group_remove_sock((struct spdk_sock_group *)(sock->group_impl), sock);
	}

	free(sock);
	*_sock = NULL;

	return 0;
}

DEFINE_RETURN_MOCK(spdk_sock_listen, struct spdk_sock *);
struct spdk_sock *
spdk_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	struct spdk_sock *sock;
	int rc;

	HANDLE_RETURN_MOCK(spdk_sock_listen);

	sock = calloc(1, sizeof(*sock));
	SPDK_CU_ASSERT_FATAL(sock != NULL);

	sock->cb_arg = opts->user_ctx;

	rc = spdk_sock_group_add_sock(opts->group, sock);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	return sock;
}

DEFINE_RETURN_MOCK(spdk_sock_accept, struct spdk_sock *);
struct spdk_sock *
spdk_sock_accept(struct spdk_sock *listen_sock)
{
	struct spdk_sock *sock;
	struct spdk_sock_group *group;
	int rc;

	HANDLE_RETURN_MOCK(spdk_sock_accept);

	group = (struct spdk_sock_group *)listen_sock->group_impl;

	sock = calloc(1, sizeof(*sock));
	SPDK_CU_ASSERT_FATAL(sock != NULL);

	rc = spdk_sock_group_add_sock(group, sock);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	return sock;
}

void
spdk_sock_set_user_ctx(struct spdk_sock *sock, void *cb_arg)
{
	sock->cb_arg = cb_arg;
}
