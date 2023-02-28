/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/sock.h"
#include "spdk_internal/mock.h"

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
DEFINE_STUB(spdk_sock_group_create, struct spdk_sock_group *, (void *ctx), NULL);
DEFINE_STUB(spdk_sock_group_add_sock, int, (struct spdk_sock_group *group, struct spdk_sock *sock,
		spdk_sock_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_sock_group_remove_sock, int, (struct spdk_sock_group *group,
		struct spdk_sock *sock), 0);
DEFINE_STUB(spdk_sock_group_poll, int, (struct spdk_sock_group *group), 0);
DEFINE_STUB(spdk_sock_group_poll_count, int, (struct spdk_sock_group *group, int max_events), 0);
DEFINE_STUB(spdk_sock_group_close, int, (struct spdk_sock_group **group), 0);
