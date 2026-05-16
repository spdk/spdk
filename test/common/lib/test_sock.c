/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/sock_module.h"
#include "spdk_internal/mock.h"

struct test_sock_group_entry {
	struct spdk_sock	*sock;
	spdk_sock_cb		cb_fn;
	void			*cb_arg;
};

#define MAX_SOCK_GROUP_ENTRIES 4

struct test_sock_group {
	struct test_sock_group_entry	entries[MAX_SOCK_GROUP_ENTRIES];
	int				num_entries;
};

DEFINE_STUB(spdk_sock_getaddr, int, (struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
				     char *caddr, int clen, uint16_t *cport), 0);
DEFINE_STUB(spdk_sock_connect, struct spdk_sock *, (const char *ip, int port,
		const char *impl_name), NULL);
DEFINE_STUB(spdk_sock_listen, struct spdk_sock *, (const char *ip, int port, const char *impl_name),
	    NULL);
DEFINE_STUB(spdk_sock_listen_ext, struct spdk_sock *, (const char *ip, int port,
		const char *impl_name, struct spdk_sock_opts *opts), NULL);
DEFINE_STUB_V(spdk_sock_get_default_opts, (struct spdk_sock_opts *opts));
DEFINE_STUB(spdk_sock_impl_get_opts, int, (const char *impl_name, struct spdk_sock_impl_opts *opts,
		size_t *len), 0);
DEFINE_STUB(spdk_sock_accept, struct spdk_sock *, (struct spdk_sock *sock), NULL);
DEFINE_STUB(spdk_sock_close, int, (struct spdk_sock **sock), 0);
DEFINE_STUB(spdk_sock_recv, ssize_t, (struct spdk_sock *sock, void *buf, size_t len), 1);
DEFINE_STUB(spdk_sock_writev, ssize_t, (struct spdk_sock *sock, struct iovec *iov, int iovcnt), 0);
DEFINE_STUB(spdk_sock_readv, ssize_t, (struct spdk_sock *sock, struct iovec *iov, int iovcnt), 0);
DEFINE_STUB(spdk_sock_set_recvlowat, int, (struct spdk_sock *sock, int nbytes), 0);
DEFINE_STUB(spdk_sock_set_recvbuf, int, (struct spdk_sock *sock, int sz), 0);
DEFINE_STUB(spdk_sock_set_sendbuf, int, (struct spdk_sock *sock, int sz), 0);
DEFINE_STUB_V(spdk_sock_writev_async, (struct spdk_sock *sock, struct spdk_sock_request *req));
DEFINE_STUB(spdk_sock_flush, int, (struct spdk_sock *sock), 0);
DEFINE_STUB(spdk_sock_is_ipv6, bool, (struct spdk_sock *sock), false);
DEFINE_STUB(spdk_sock_is_ipv4, bool, (struct spdk_sock *sock), true);
DEFINE_STUB(spdk_sock_is_connected, bool, (struct spdk_sock *sock), true);
DEFINE_STUB(spdk_sock_group_provide_buf, int, (struct spdk_sock_group *group, void *buf, size_t len,
		void *ctx), 0);

static uint8_t g_buf[0x1000] = {};

DEFINE_RETURN_MOCK(spdk_sock_recv_next, int);
int
spdk_sock_recv_next(struct spdk_sock *sock, void **buf, void **ctx)
{
	HANDLE_RETURN_MOCK(spdk_sock_recv_next);

	*buf = g_buf;
	*ctx = NULL;

	return 0x1000;
}

DEFINE_RETURN_MOCK(spdk_sock_group_create, struct spdk_sock_group *);
struct spdk_sock_group *
spdk_sock_group_create(void *ctx)
{
	struct test_sock_group *group;

	HANDLE_RETURN_MOCK(spdk_sock_group_create);

	group = calloc(1, sizeof(*group));
	SPDK_CU_ASSERT_FATAL(group != NULL);

	return (struct spdk_sock_group *)group;
}

DEFINE_RETURN_MOCK(spdk_sock_group_add_sock, int);
int
spdk_sock_group_add_sock(struct spdk_sock_group *_group, struct spdk_sock *sock, spdk_sock_cb cb_fn,
			 void *cb_arg)
{
	struct test_sock_group *group;
	struct test_sock_group_entry *entry;

	HANDLE_RETURN_MOCK(spdk_sock_group_add_sock);

	group = (struct test_sock_group *)_group;

	SPDK_CU_ASSERT_FATAL(group->num_entries < MAX_SOCK_GROUP_ENTRIES);

	entry = &group->entries[group->num_entries++];

	entry->cb_arg = cb_arg;
	entry->cb_fn = cb_fn;
	entry->sock = sock;

	return 0;
}

DEFINE_RETURN_MOCK(spdk_sock_group_remove_sock, int);
int
spdk_sock_group_remove_sock(struct spdk_sock_group *_group, struct spdk_sock *sock)
{
	struct test_sock_group *group;
	struct test_sock_group_entry entries[MAX_SOCK_GROUP_ENTRIES];
	int num_entries, i;

	HANDLE_RETURN_MOCK(spdk_sock_group_remove_sock);

	group = (struct test_sock_group *)_group;
	num_entries = 0;

	for (i = 0; i < group->num_entries; i++) {
		if (group->entries[i].sock != sock) {
			memcpy(&entries[num_entries], &group->entries[i], sizeof(struct test_sock_group_entry));
			num_entries++;
		}
	}

	memcpy(group->entries, entries, sizeof(struct test_sock_group_entry) * num_entries);
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
		group->entries[i].cb_fn(group->entries[i].cb_arg, _group, group->entries[i].sock);
	}

	return 0;
}

DEFINE_RETURN_MOCK(spdk_sock_group_poll_count, int);
int
spdk_sock_group_poll_count(struct spdk_sock_group *_group, int max_events)
{
	struct test_sock_group *group;
	int i;

	HANDLE_RETURN_MOCK(spdk_sock_group_poll_count);

	group = (struct test_sock_group *)_group;

	for (i = 0; i < group->num_entries; i++) {
		group->entries[i].cb_fn(group->entries[i].cb_arg, _group, group->entries[i].sock);
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
