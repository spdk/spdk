/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"

#include "spdk_cunit.h"

#include "spdk_internal/sock.h"

#include "sock/sock.c"
#include "sock/posix/posix.c"

#include "spdk_internal/mock.h"
#include "common/lib/test_env.c"

#include "unit/lib/json_mock.c"

#define UT_IP	"test_ip"
#define UT_PORT	1234

bool g_read_data_called;
ssize_t g_bytes_read;
char g_buf[256];
struct spdk_sock *g_server_sock_read;
int g_ut_accept_count;
struct spdk_ut_sock *g_ut_listen_sock;
struct spdk_ut_sock *g_ut_client_sock;

struct spdk_ut_sock {
	struct spdk_sock	base;
	struct spdk_ut_sock	*peer;
	size_t			bytes_avail;
	char			buf[256];
};

struct spdk_ut_sock_group_impl {
	struct spdk_sock_group_impl	base;
	struct spdk_ut_sock		*sock;
};

#define __ut_sock(sock) (struct spdk_ut_sock *)sock
#define __ut_group(group) (struct spdk_ut_sock_group_impl *)group

static int
spdk_ut_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		     char *caddr, int clen, uint16_t *cport)
{
	return 0;
}

static struct spdk_sock *
spdk_ut_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	struct spdk_ut_sock *sock;

	if (strcmp(ip, UT_IP) || port != UT_PORT) {
		return NULL;
	}

	CU_ASSERT(g_ut_listen_sock == NULL);

	sock = calloc(1, sizeof(*sock));
	SPDK_CU_ASSERT_FATAL(sock != NULL);
	g_ut_listen_sock = sock;

	return &sock->base;
}

static struct spdk_sock *
spdk_ut_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	struct spdk_ut_sock *sock;

	if (strcmp(ip, UT_IP) || port != UT_PORT) {
		return NULL;
	}

	sock = calloc(1, sizeof(*sock));
	SPDK_CU_ASSERT_FATAL(sock != NULL);
	g_ut_accept_count++;
	CU_ASSERT(g_ut_client_sock == NULL);
	g_ut_client_sock = sock;

	return &sock->base;
}

static struct spdk_sock *
spdk_ut_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_ut_sock *sock = __ut_sock(_sock);
	struct spdk_ut_sock *new_sock;

	CU_ASSERT(sock == g_ut_listen_sock);

	if (g_ut_accept_count == 0) {
		errno = EAGAIN;
		return NULL;
	}

	g_ut_accept_count--;
	new_sock = calloc(1, sizeof(*sock));
	if (new_sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	SPDK_CU_ASSERT_FATAL(g_ut_client_sock != NULL);
	g_ut_client_sock->peer = new_sock;
	new_sock->peer = g_ut_client_sock;

	return &new_sock->base;
}

static int
spdk_ut_sock_close(struct spdk_sock *_sock)
{
	struct spdk_ut_sock *sock = __ut_sock(_sock);

	if (sock == g_ut_listen_sock) {
		g_ut_listen_sock = NULL;
	}
	if (sock == g_ut_client_sock) {
		g_ut_client_sock = NULL;
	}

	if (sock->peer != NULL) {
		sock->peer->peer = NULL;
	}

	free(_sock);

	return 0;
}

static ssize_t
spdk_ut_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct spdk_ut_sock *sock = __ut_sock(_sock);
	char tmp[256];

	len = spdk_min(len, sock->bytes_avail);

	if (len == 0) {
		errno = EAGAIN;
		return -1;
	}

	memcpy(buf, sock->buf, len);
	memcpy(tmp, &sock->buf[len], sock->bytes_avail - len);
	memcpy(sock->buf, tmp, sock->bytes_avail - len);
	sock->bytes_avail -= len;

	return len;
}

static ssize_t
spdk_ut_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_ut_sock *sock = __ut_sock(_sock);
	size_t len;
	char tmp[256];

	/* Test implementation only supports single iov for now. */
	CU_ASSERT(iovcnt == 1);

	len = spdk_min(iov[0].iov_len, sock->bytes_avail);

	if (len == 0) {
		errno = EAGAIN;
		return -1;
	}

	memcpy(iov[0].iov_base, sock->buf, len);
	memcpy(tmp, &sock->buf[len], sock->bytes_avail - len);
	memcpy(sock->buf, tmp, sock->bytes_avail - len);
	sock->bytes_avail -= len;

	return len;
}

static ssize_t
spdk_ut_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_ut_sock *sock = __ut_sock(_sock);
	struct spdk_ut_sock *peer;

	SPDK_CU_ASSERT_FATAL(sock->peer != NULL);
	peer = sock->peer;

	/* Test implementation only supports single iov for now. */
	CU_ASSERT(iovcnt == 1);

	memcpy(&peer->buf[peer->bytes_avail], iov[0].iov_base, iov[0].iov_len);
	peer->bytes_avail += iov[0].iov_len;

	return iov[0].iov_len;
}

static int
spdk_ut_sock_flush(struct spdk_sock *sock)
{
	return -1;
}

static int
spdk_ut_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	return 0;
}

static int
spdk_ut_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	return 0;
}

static int
spdk_ut_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	return 0;
}

static bool
spdk_ut_sock_is_ipv6(struct spdk_sock *_sock)
{
	return false;
}

static bool
spdk_ut_sock_is_ipv4(struct spdk_sock *_sock)
{
	return true;
}

static bool
spdk_ut_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_ut_sock *sock = __ut_sock(_sock);

	return (sock->peer != NULL);
}

static struct spdk_sock_group_impl *
spdk_ut_sock_group_impl_get_optimal(struct spdk_sock *_sock, struct spdk_sock_group_impl *hint)
{
	return NULL;
}

static struct spdk_sock_group_impl *
spdk_ut_sock_group_impl_create(void)
{
	struct spdk_ut_sock_group_impl *group_impl;

	group_impl = calloc(1, sizeof(*group_impl));
	SPDK_CU_ASSERT_FATAL(group_impl != NULL);

	return &group_impl->base;
}

static int
spdk_ut_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_ut_sock_group_impl *group = __ut_group(_group);
	struct spdk_ut_sock *sock = __ut_sock(_sock);

	group->sock = sock;

	return 0;
}

static int
spdk_ut_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_ut_sock_group_impl *group = __ut_group(_group);
	struct spdk_ut_sock *sock = __ut_sock(_sock);

	CU_ASSERT(group->sock == sock);
	group->sock = NULL;

	return 0;
}

static int
spdk_ut_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			     struct spdk_sock **socks)
{
	struct spdk_ut_sock_group_impl *group = __ut_group(_group);

	if (group->sock != NULL && group->sock->bytes_avail > 0) {
		socks[0] = &group->sock->base;
		return 1;
	}

	return 0;
}

static int
spdk_ut_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_ut_sock_group_impl *group = __ut_group(_group);

	CU_ASSERT(group->sock == NULL);
	free(_group);

	return 0;
}

static struct spdk_net_impl g_ut_net_impl = {
	.name		= "ut",
	.getaddr	= spdk_ut_sock_getaddr,
	.connect	= spdk_ut_sock_connect,
	.listen		= spdk_ut_sock_listen,
	.accept		= spdk_ut_sock_accept,
	.close		= spdk_ut_sock_close,
	.recv		= spdk_ut_sock_recv,
	.readv		= spdk_ut_sock_readv,
	.writev		= spdk_ut_sock_writev,
	.flush          = spdk_ut_sock_flush,
	.set_recvlowat	= spdk_ut_sock_set_recvlowat,
	.set_recvbuf	= spdk_ut_sock_set_recvbuf,
	.set_sendbuf	= spdk_ut_sock_set_sendbuf,
	.is_ipv6	= spdk_ut_sock_is_ipv6,
	.is_ipv4	= spdk_ut_sock_is_ipv4,
	.is_connected	= spdk_ut_sock_is_connected,
	.group_impl_get_optimal	= spdk_ut_sock_group_impl_get_optimal,
	.group_impl_create	= spdk_ut_sock_group_impl_create,
	.group_impl_add_sock	= spdk_ut_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_ut_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_ut_sock_group_impl_poll,
	.group_impl_close	= spdk_ut_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(ut, &g_ut_net_impl, DEFAULT_SOCK_PRIORITY + 2);

static void
_sock(const char *ip, int port, char *impl_name)
{
	struct spdk_sock *listen_sock;
	struct spdk_sock *server_sock;
	struct spdk_sock *client_sock;
	char *test_string = "abcdef";
	char buffer[64];
	ssize_t bytes_read, bytes_written;
	struct iovec iov;
	int nbytes;
	int rc;

	listen_sock = spdk_sock_listen(ip, port, impl_name);
	SPDK_CU_ASSERT_FATAL(listen_sock != NULL);

	server_sock = spdk_sock_accept(listen_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

	client_sock = spdk_sock_connect(ip, port, impl_name);
	SPDK_CU_ASSERT_FATAL(client_sock != NULL);

	/*
	 * Delay a bit here before checking if server socket is
	 *  ready.
	 */
	usleep(1000);

	server_sock = spdk_sock_accept(listen_sock);
	SPDK_CU_ASSERT_FATAL(server_sock != NULL);
	CU_ASSERT(spdk_sock_is_connected(client_sock) == true);
	CU_ASSERT(spdk_sock_is_connected(server_sock) == true);

	/* Test spdk_sock_set_default_impl */
	rc = spdk_sock_set_default_impl(impl_name);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_default_impl != NULL);

	/* Test spdk_sock_set_default_impl when name is NULL */
	rc = spdk_sock_set_default_impl(NULL);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);

	/* Test spdk_sock_is _ipv6 */
	CU_ASSERT(!spdk_sock_is_ipv6(client_sock));

	/* Test spdk_sock_is _ipv4 */
	CU_ASSERT(spdk_sock_is_ipv4(client_sock));

	nbytes = 2048;

	/* Test spdk_sock_set_recvlowat */
	rc = spdk_sock_set_recvlowat(client_sock, nbytes);
	CU_ASSERT(rc == 0);

	/* Test spdk_sock_set_recvbuf */
	rc = spdk_sock_set_recvbuf(client_sock, nbytes);
	CU_ASSERT(rc == 0);

	/* Test spdk_sock_set_sendbuf */
	rc = spdk_sock_set_sendbuf(client_sock, nbytes);
	CU_ASSERT(rc == 0);

	/* Test spdk_sock_recv */
	iov.iov_base = test_string;
	iov.iov_len = 7;
	bytes_written = spdk_sock_writev(client_sock, &iov, 1);
	CU_ASSERT(bytes_written == 7);

	usleep(1000);

	bytes_read = spdk_sock_recv(server_sock, buffer, 2);
	CU_ASSERT(bytes_read == 2);

	usleep(1000);

	bytes_read += spdk_sock_recv(server_sock, buffer + 2, 5);
	CU_ASSERT(bytes_read == 7);

	CU_ASSERT(strncmp(test_string, buffer, 7) == 0);

	/* Test spdk_sock_readv */
	iov.iov_base = test_string;
	iov.iov_len = 7;
	bytes_written = spdk_sock_writev(client_sock, &iov, 1);
	CU_ASSERT(bytes_written == 7);

	usleep(1000);

	iov.iov_base = buffer;
	iov.iov_len = 2;
	bytes_read = spdk_sock_readv(server_sock, &iov, 1);
	CU_ASSERT(bytes_read == 2);

	usleep(1000);

	iov.iov_base = buffer + 2;
	iov.iov_len = 5;
	bytes_read += spdk_sock_readv(server_sock, &iov, 1);
	CU_ASSERT(bytes_read == 7);

	usleep(1000);

	CU_ASSERT(strncmp(test_string, buffer, 7) == 0);

	rc = spdk_sock_close(&client_sock);
	CU_ASSERT(client_sock == NULL);
	CU_ASSERT(rc == 0);

#if defined(__FreeBSD__)
	/* On FreeBSD, it takes a small amount of time for a close to propagate to the
	 * other side, even in loopback. Introduce a small sleep. */
	sleep(1);
#endif
	CU_ASSERT(spdk_sock_is_connected(server_sock) == false);

	rc = spdk_sock_close(&server_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&listen_sock);
	CU_ASSERT(listen_sock == NULL);
	CU_ASSERT(rc == 0);
}

static void
posix_sock(void)
{
	_sock("127.0.0.1", UT_PORT, "posix");
}

static void
ut_sock(void)
{
	_sock(UT_IP, UT_PORT, "ut");
}

static void
read_data(void *cb_arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_sock *server_sock = cb_arg;

	CU_ASSERT(server_sock == sock);

	g_read_data_called = true;
	g_bytes_read += spdk_sock_recv(server_sock, g_buf + g_bytes_read, sizeof(g_buf) - g_bytes_read);
}

static void
_sock_group(const char *ip, int port, char *impl_name)
{
	struct spdk_sock_group *group;
	struct spdk_sock_group *hint;
	struct spdk_sock *listen_sock;
	struct spdk_sock *server_sock;
	struct spdk_sock *client_sock;
	char *test_string = "abcdef";
	ssize_t bytes_written;
	struct iovec iov;
	int rc;

	listen_sock = spdk_sock_listen(ip, port, impl_name);
	SPDK_CU_ASSERT_FATAL(listen_sock != NULL);

	server_sock = spdk_sock_accept(listen_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

	client_sock = spdk_sock_connect(ip, port, impl_name);
	SPDK_CU_ASSERT_FATAL(client_sock != NULL);

	usleep(1000);

	server_sock = spdk_sock_accept(listen_sock);
	SPDK_CU_ASSERT_FATAL(server_sock != NULL);

	group = spdk_sock_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);

	hint = spdk_sock_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(hint != NULL);

	/* pass null cb_fn */
	rc = spdk_sock_group_add_sock(group, server_sock, NULL, NULL);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);

	rc = spdk_sock_group_add_sock(group, server_sock, read_data, server_sock);
	CU_ASSERT(rc == 0);

	/* try adding sock a second time */
	rc = spdk_sock_group_add_sock(group, server_sock, read_data, server_sock);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);

	g_read_data_called = false;
	g_bytes_read = 0;
	rc = spdk_sock_group_poll(group);

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_read_data_called == false);

	iov.iov_base = test_string;
	iov.iov_len = 7;
	bytes_written = spdk_sock_writev(client_sock, &iov, 1);
	CU_ASSERT(bytes_written == 7);

	usleep(1000);

	g_read_data_called = false;
	g_bytes_read = 0;
	rc = spdk_sock_group_poll(group);

	CU_ASSERT(rc == 1);
	CU_ASSERT(g_read_data_called == true);
	CU_ASSERT(g_bytes_read == 7);

	CU_ASSERT(strncmp(test_string, g_buf, 7) == 0);

	rc = spdk_sock_close(&client_sock);
	CU_ASSERT(client_sock == NULL);
	CU_ASSERT(rc == 0);

	/* Test get_optimal_sock_group */
	rc = spdk_sock_get_optimal_sock_group(server_sock, &group, hint);
	CU_ASSERT(rc == 0);

	/* Try to close sock_group while it still has sockets. */
	rc = spdk_sock_group_close(&group);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EBUSY);

	/* Try to close sock while it is still part of a sock_group. */
	rc = spdk_sock_close(&server_sock);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EBUSY);

	rc = spdk_sock_group_remove_sock(group, server_sock);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_group_close(&group);
	CU_ASSERT(group == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_group_close(&hint);
	CU_ASSERT(hint == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&server_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&listen_sock);
	CU_ASSERT(listen_sock == NULL);
	CU_ASSERT(rc == 0);
}

static void
posix_sock_group(void)
{
	_sock_group("127.0.0.1", UT_PORT, "posix");
}

static void
ut_sock_group(void)
{
	_sock_group(UT_IP, UT_PORT, "ut");
}

static void
read_data_fairness(void *cb_arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_sock *server_sock = cb_arg;
	ssize_t bytes_read;
	char buf[1];

	CU_ASSERT(g_server_sock_read == NULL);
	CU_ASSERT(server_sock == sock);

	g_server_sock_read = server_sock;
	bytes_read = spdk_sock_recv(server_sock, buf, 1);
	CU_ASSERT(bytes_read == 1);
}

static void
posix_sock_group_fairness(void)
{
	struct spdk_sock_group *group;
	struct spdk_sock *listen_sock;
	struct spdk_sock *server_sock[3];
	struct spdk_sock *client_sock[3];
	char test_char = 'a';
	ssize_t bytes_written;
	struct iovec iov;
	int i, rc;

	listen_sock = spdk_sock_listen("127.0.0.1", UT_PORT, "posix");
	SPDK_CU_ASSERT_FATAL(listen_sock != NULL);

	group = spdk_sock_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);

	for (i = 0; i < 3; i++) {
		client_sock[i] = spdk_sock_connect("127.0.0.1", UT_PORT, "posix");
		SPDK_CU_ASSERT_FATAL(client_sock[i] != NULL);

		usleep(1000);

		server_sock[i] = spdk_sock_accept(listen_sock);
		SPDK_CU_ASSERT_FATAL(server_sock[i] != NULL);

		rc = spdk_sock_group_add_sock(group, server_sock[i],
					      read_data_fairness, server_sock[i]);
		CU_ASSERT(rc == 0);
	}

	iov.iov_base = &test_char;
	iov.iov_len = 1;

	for (i = 0; i < 3; i++) {
		bytes_written = spdk_sock_writev(client_sock[i], &iov, 1);
		CU_ASSERT(bytes_written == 1);
	}

	usleep(1000);

	/*
	 * Poll for just one event - this should be server sock 0, since that
	 *  is the peer of the first client sock that we wrote to.
	 */
	g_server_sock_read = NULL;
	rc = spdk_sock_group_poll_count(group, 1);
	CU_ASSERT(rc == 1);
	CU_ASSERT(g_server_sock_read == server_sock[0]);

	/*
	 * Now write another byte to client sock 0.  We want to ensure that
	 *  the sock group does not unfairly process the event for this sock
	 *  before the socks that were written to earlier.
	 */
	bytes_written = spdk_sock_writev(client_sock[0], &iov, 1);
	CU_ASSERT(bytes_written == 1);

	usleep(1000);

	g_server_sock_read = NULL;
	rc = spdk_sock_group_poll_count(group, 1);
	CU_ASSERT(rc == 1);
	CU_ASSERT(g_server_sock_read == server_sock[1]);

	g_server_sock_read = NULL;
	rc = spdk_sock_group_poll_count(group, 1);
	CU_ASSERT(rc == 1);
	CU_ASSERT(g_server_sock_read == server_sock[2]);

	g_server_sock_read = NULL;
	rc = spdk_sock_group_poll_count(group, 1);
	CU_ASSERT(rc == 1);
	CU_ASSERT(g_server_sock_read == server_sock[0]);

	for (i = 0; i < 3; i++) {
		rc = spdk_sock_group_remove_sock(group, server_sock[i]);
		CU_ASSERT(rc == 0);

		rc = spdk_sock_close(&client_sock[i]);
		CU_ASSERT(client_sock[i] == NULL);
		CU_ASSERT(rc == 0);

		rc = spdk_sock_close(&server_sock[i]);
		CU_ASSERT(server_sock[i] == NULL);
		CU_ASSERT(rc == 0);
	}

	rc = spdk_sock_group_close(&group);
	CU_ASSERT(group == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&listen_sock);
	CU_ASSERT(listen_sock == NULL);
	CU_ASSERT(rc == 0);
}

struct close_ctx {
	struct spdk_sock_group *group;
	struct spdk_sock *sock;
	bool called;
};

static void
_first_close_cb(void *cb_arg, int err)
{
	struct close_ctx *ctx = cb_arg;
	int rc;

	ctx->called = true;

	/* Always close the socket here */
	rc = spdk_sock_group_remove_sock(ctx->group, ctx->sock);
	CU_ASSERT(rc == 0);
	spdk_sock_close(&ctx->sock);

	CU_ASSERT(err == 0);
}

static void
_second_close_cb(void *cb_arg, int err)
{
	*(bool *)cb_arg = true;
	CU_ASSERT(err == -ECANCELED);
}

static void
_sock_close(const char *ip, int port, char *impl_name)
{
	struct spdk_sock_group *group;
	struct spdk_sock *listen_sock;
	struct spdk_sock *server_sock;
	struct spdk_sock *client_sock;
	uint8_t data_buf[64] = {};
	struct spdk_sock_request *req1, *req2;
	struct close_ctx ctx = {};
	bool cb_arg2 = false;
	int rc;

	listen_sock = spdk_sock_listen(ip, port, impl_name);
	SPDK_CU_ASSERT_FATAL(listen_sock != NULL);

	client_sock = spdk_sock_connect(ip, port, impl_name);
	SPDK_CU_ASSERT_FATAL(client_sock != NULL);

	usleep(1000);

	server_sock = spdk_sock_accept(listen_sock);
	SPDK_CU_ASSERT_FATAL(server_sock != NULL);

	group = spdk_sock_group_create(NULL);
	SPDK_CU_ASSERT_FATAL(group != NULL);

	rc = spdk_sock_group_add_sock(group, server_sock, read_data, server_sock);
	CU_ASSERT(rc == 0);

	/* Submit multiple async writevs on the server sock */

	req1 = calloc(1, sizeof(struct spdk_sock_request) + sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req1 != NULL);
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_base = data_buf;
	SPDK_SOCK_REQUEST_IOV(req1, 0)->iov_len = 64;
	ctx.group = group;
	ctx.sock = server_sock;
	ctx.called = false;
	req1->iovcnt = 1;
	req1->cb_fn = _first_close_cb;
	req1->cb_arg = &ctx;
	spdk_sock_writev_async(server_sock, req1);
	CU_ASSERT(ctx.called == false);

	req2 = calloc(1, sizeof(struct spdk_sock_request) + sizeof(struct iovec));
	SPDK_CU_ASSERT_FATAL(req2 != NULL);
	SPDK_SOCK_REQUEST_IOV(req2, 0)->iov_base = data_buf;
	SPDK_SOCK_REQUEST_IOV(req2, 0)->iov_len = 64;
	req2->iovcnt = 1;
	req2->cb_fn = _second_close_cb;
	req2->cb_arg = &cb_arg2;
	spdk_sock_writev_async(server_sock, req2);
	CU_ASSERT(cb_arg2 == false);

	/* Test spdk_sock_flush when sock is NULL */
	rc = spdk_sock_flush(NULL);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EBADF);

	/* Test spdk_sock_flush when sock is not NULL */
	rc = spdk_sock_flush(client_sock);
	CU_ASSERT(rc == 0);

	/* Poll the socket so the writev_async's send. The first one's
	 * callback will close the socket. */
	spdk_sock_group_poll(group);
	if (ctx.called == false) {
		/* Sometimes the zerocopy completion isn't posted immediately. Delay slightly
		* and poll one more time. */
		usleep(1000);
		spdk_sock_group_poll(group);
	}
	CU_ASSERT(ctx.called == true);
	CU_ASSERT(cb_arg2 == true);

	rc = spdk_sock_group_close(&group);
	CU_ASSERT(group == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&client_sock);
	CU_ASSERT(client_sock == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&listen_sock);
	CU_ASSERT(listen_sock == NULL);
	CU_ASSERT(rc == 0);

	free(req1);
	free(req2);
}

static void
_posix_sock_close(void)
{
	_sock_close("127.0.0.1", UT_PORT, "posix");
}

static void
sock_get_default_opts(void)
{
	struct spdk_sock_opts opts;

	/* opts_size is 0 */
	opts.opts_size = 0;
	opts.priority = 3;
	spdk_sock_get_default_opts(&opts);
	CU_ASSERT(opts.priority == 3);
	CU_ASSERT(opts.opts_size == 0);

	/* opts_size is less than sizeof(opts) */
	opts.opts_size = 4;
	opts.priority = 3;
	spdk_sock_get_default_opts(&opts);
	CU_ASSERT(opts.priority == 3);
	CU_ASSERT(opts.opts_size == 4);

	/* opts_size is equal to sizeof(opts) */
	opts.opts_size = sizeof(opts);
	opts.priority = 3;
	spdk_sock_get_default_opts(&opts);
	CU_ASSERT(opts.priority == SPDK_SOCK_DEFAULT_PRIORITY);
	CU_ASSERT(opts.opts_size == sizeof(opts));

	/* opts_size is larger then sizeof(opts) */
	opts.opts_size = sizeof(opts) + 1;
	opts.priority = 3;
	spdk_sock_get_default_opts(&opts);
	CU_ASSERT(opts.priority == SPDK_SOCK_DEFAULT_PRIORITY);
	CU_ASSERT(opts.opts_size == (sizeof(opts) + 1));
}

static void
ut_sock_impl_get_set_opts(void)
{
	int rc;
	size_t len = 0;
	/* Use any pointer value for opts. It is never dereferenced in this test */
	struct spdk_sock_impl_opts *opts = (struct spdk_sock_impl_opts *)0x123456789;

	rc = spdk_sock_impl_get_opts("ut", NULL, &len);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);
	rc = spdk_sock_impl_get_opts("ut", opts, NULL);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);
	rc = spdk_sock_impl_get_opts("ut", opts, &len);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == ENOTSUP);

	rc = spdk_sock_impl_set_opts("ut", NULL, len);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);
	rc = spdk_sock_impl_set_opts("ut", opts, len);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == ENOTSUP);
}

static void
posix_sock_impl_get_set_opts(void)
{
	int rc;
	size_t len = 0;
	struct spdk_sock_impl_opts opts = {};

	rc = spdk_sock_impl_get_opts("posix", NULL, &len);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);
	rc = spdk_sock_impl_get_opts("posix", &opts, NULL);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);

	/* Check default opts */
	len = sizeof(opts);
	rc = spdk_sock_impl_get_opts("posix", &opts, &len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(len == sizeof(opts));
	CU_ASSERT(opts.recv_buf_size == MIN_SO_RCVBUF_SIZE);
	CU_ASSERT(opts.send_buf_size == MIN_SO_SNDBUF_SIZE);

	/* Try to request zero opts */
	len = 0;
	rc = spdk_sock_impl_get_opts("posix", &opts, &len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(len == 0);

	rc = spdk_sock_impl_set_opts("posix", NULL, len);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);

	opts.recv_buf_size = 16;
	opts.send_buf_size = 4;
	rc = spdk_sock_impl_set_opts("posix", &opts, sizeof(opts));
	CU_ASSERT(rc == 0);
	len = sizeof(opts);
	memset(&opts, 0, sizeof(opts));
	rc = spdk_sock_impl_get_opts("posix", &opts, &len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(opts.recv_buf_size == 16);
	CU_ASSERT(opts.send_buf_size == 4);

	/* Try to set less opts. Opts in the end should be untouched */
	opts.recv_buf_size = 5;
	opts.send_buf_size = 10;
	rc = spdk_sock_impl_set_opts("posix", &opts, sizeof(opts.recv_buf_size));
	CU_ASSERT(rc == 0);
	len = sizeof(opts);
	memset(&opts, 0, sizeof(opts));
	rc = spdk_sock_impl_get_opts("posix", &opts, &len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(opts.recv_buf_size == 5);
	CU_ASSERT(opts.send_buf_size == 4);

	/* Try to set partial option. It should not be changed */
	opts.recv_buf_size = 1000;
	rc = spdk_sock_impl_set_opts("posix", &opts, 1);
	CU_ASSERT(rc == 0);
	len = sizeof(opts);
	memset(&opts, 0, sizeof(opts));
	rc = spdk_sock_impl_get_opts("posix", &opts, &len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(opts.recv_buf_size == 5);
}

static void
ut_sock_map(void)
{
	struct spdk_sock_map map = {
		.entries = STAILQ_HEAD_INITIALIZER(map.entries),
		.mtx = PTHREAD_MUTEX_INITIALIZER
	};
	struct spdk_sock_group_impl *group_1, *group_2, *test_group;
	int rc;
	int test_id;

	group_1 = spdk_ut_sock_group_impl_create();
	group_2 = spdk_ut_sock_group_impl_create();

	/* Test 1
	 * Sanity check when sock_map is empty */
	test_id = spdk_sock_map_find_free(&map);
	CU_ASSERT(test_id == -1);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(test_group == NULL);

	/* Test 2
	 * Insert single entry */
	rc = spdk_sock_map_insert(&map, 1, group_1);
	CU_ASSERT(rc == 0);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_1);

	/* There is single entry allocated, but it is not free */
	test_id = spdk_sock_map_find_free(&map);
	CU_ASSERT(test_id == -1);

	/* Free the entry and verify */
	spdk_sock_map_release(&map, 1);
	test_id = spdk_sock_map_find_free(&map);
	CU_ASSERT(test_id == 1);

	spdk_sock_map_cleanup(&map);

	/* Test 3
	 * Insert sock_group into placement_id multiple times */
	rc = spdk_sock_map_insert(&map, 1, group_1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(STAILQ_FIRST(&map.entries)->ref == 1);
	rc = spdk_sock_map_insert(&map, 1, group_1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(STAILQ_FIRST(&map.entries)->ref == 2);

	/* Release entry once and see that it still exists. */
	spdk_sock_map_release(&map, 1);
	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_1);

	/* Release entry second and final time. */
	spdk_sock_map_release(&map, 1);
	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(test_group == NULL);

	spdk_sock_map_cleanup(&map);

	/* Test 4
	 * Test multiple entries */
	rc = spdk_sock_map_insert(&map, 1, group_1);
	CU_ASSERT(rc == 0);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_1);

	rc = spdk_sock_map_insert(&map, 2, group_2);
	CU_ASSERT(rc == 0);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 2, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_2);

	spdk_sock_map_cleanup(&map);

	/* Test 5
	 * Attempt inserting multiple entries into single placement_id */
	rc = spdk_sock_map_insert(&map, 1, group_1);
	CU_ASSERT(rc == 0);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_1);

	rc = spdk_sock_map_insert(&map, 1, group_2);
	CU_ASSERT(rc == -EINVAL);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_1);

	spdk_sock_map_cleanup(&map);

	/* Test 6
	 * Insert single entry without a sock_group */
	rc = spdk_sock_map_insert(&map, 1, NULL);
	CU_ASSERT(rc == 0);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(test_group == NULL);

	test_id = spdk_sock_map_find_free(&map);
	CU_ASSERT(test_id == 1);

	rc = spdk_sock_map_insert(&map, test_id, group_1);
	CU_ASSERT(rc == 0);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, test_id, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_1);

	spdk_sock_map_cleanup(&map);

	/* Test 6
	 * Use hint sock_group for for placement_id */
	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, group_1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == NULL);

	test_group = NULL;
	rc = spdk_sock_map_lookup(&map, 1, &test_group, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(test_group == group_1);

	test_id = spdk_sock_map_find_free(&map);
	CU_ASSERT(test_id == -1);

	rc = spdk_sock_map_insert(&map, 1, group_2);
	CU_ASSERT(rc == -EINVAL);

	rc = spdk_sock_map_insert(&map, 1, group_1);
	CU_ASSERT(rc == 0);

	spdk_sock_map_cleanup(&map);

	spdk_ut_sock_group_impl_close(group_2);
	spdk_ut_sock_group_impl_close(group_1);
}

static void
override_impl_opts(void)
{
	struct spdk_sock *lsock, *csock, *asock;
	struct spdk_sock_opts opts;
	struct spdk_sock_impl_opts impl_opts;
	uint32_t send_buf_size;
	size_t opts_size;
	int rc;

	opts_size = sizeof(impl_opts);
	rc = spdk_sock_impl_get_opts("posix", &impl_opts, &opts_size);
	CU_ASSERT_EQUAL(rc, 0);
	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = sizeof(impl_opts);

	/* Use send_buf_size to verify that impl_opts get overridden */
	send_buf_size = impl_opts.send_buf_size;
	impl_opts.send_buf_size = send_buf_size + 1;

	lsock = spdk_sock_listen_ext("127.0.0.1", UT_PORT, "posix", &opts);
	SPDK_CU_ASSERT_FATAL(lsock != NULL);
	CU_ASSERT_EQUAL(lsock->impl_opts.send_buf_size, send_buf_size + 1);

	/* Check the same for connect() */
	opts_size = sizeof(impl_opts);
	rc = spdk_sock_impl_get_opts("posix", &impl_opts, &opts_size);
	CU_ASSERT_EQUAL(rc, 0);
	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = sizeof(impl_opts);

	impl_opts.send_buf_size = send_buf_size + 2;

	csock = spdk_sock_connect_ext("127.0.0.1", UT_PORT, "posix", &opts);
	SPDK_CU_ASSERT_FATAL(csock != NULL);
	CU_ASSERT_EQUAL(csock->impl_opts.send_buf_size, send_buf_size + 2);

	/* Check that accept() inherits impl_opts from listen socket */
	asock = spdk_sock_accept(lsock);
	SPDK_CU_ASSERT_FATAL(asock != NULL);
	CU_ASSERT_EQUAL(asock->impl_opts.send_buf_size, send_buf_size + 1);

	spdk_sock_close(&asock);
	spdk_sock_close(&csock);
	spdk_sock_close(&lsock);

	/* Check that impl_opts_size is verified by setting it to the offset of send_buf_size  */
	opts_size = sizeof(impl_opts);
	rc = spdk_sock_impl_get_opts("posix", &impl_opts, &opts_size);
	CU_ASSERT_EQUAL(rc, 0);
	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = offsetof(struct spdk_sock_impl_opts, send_buf_size);

	send_buf_size = impl_opts.send_buf_size;
	impl_opts.send_buf_size = send_buf_size + 1;

	lsock = spdk_sock_listen_ext("127.0.0.1", UT_PORT, "posix", &opts);
	SPDK_CU_ASSERT_FATAL(lsock != NULL);
	CU_ASSERT_EQUAL(lsock->impl_opts.send_buf_size, send_buf_size);

	/* Check the same for connect() */
	opts_size = sizeof(impl_opts);
	rc = spdk_sock_impl_get_opts("posix", &impl_opts, &opts_size);
	CU_ASSERT_EQUAL(rc, 0);
	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = offsetof(struct spdk_sock_impl_opts, send_buf_size);

	impl_opts.send_buf_size = send_buf_size + 2;

	csock = spdk_sock_connect_ext("127.0.0.1", UT_PORT, "posix", &opts);
	SPDK_CU_ASSERT_FATAL(csock != NULL);
	CU_ASSERT_EQUAL(csock->impl_opts.send_buf_size, send_buf_size);

	spdk_sock_close(&lsock);
	spdk_sock_close(&csock);
}

static void
ut_sock_group_get_ctx(void)
{
	void *test_ctx = (void *)0xff0000000;
	void *test_ctx1 = (void *)0xfff000000;
	void *test_ctx2 = (void *)0xffff00000;
	struct spdk_sock_group group;

	/* The return should be NULL */
	test_ctx = spdk_sock_group_get_ctx(NULL);
	CU_ASSERT(test_ctx == NULL);

	/* The group.ctx should be changed */
	group.ctx = test_ctx1;
	test_ctx2 = spdk_sock_group_get_ctx(&group);

	CU_ASSERT(test_ctx1 == test_ctx2);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("sock", NULL, NULL);

	CU_ADD_TEST(suite, posix_sock);
	CU_ADD_TEST(suite, ut_sock);
	CU_ADD_TEST(suite, posix_sock_group);
	CU_ADD_TEST(suite, ut_sock_group);
	CU_ADD_TEST(suite, posix_sock_group_fairness);
	CU_ADD_TEST(suite, _posix_sock_close);
	CU_ADD_TEST(suite, sock_get_default_opts);
	CU_ADD_TEST(suite, ut_sock_impl_get_set_opts);
	CU_ADD_TEST(suite, posix_sock_impl_get_set_opts);
	CU_ADD_TEST(suite, ut_sock_map);
	CU_ADD_TEST(suite, override_impl_opts);
	CU_ADD_TEST(suite, ut_sock_group_get_ctx);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
