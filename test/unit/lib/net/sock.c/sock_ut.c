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

#include "spdk_cunit.h"

#include "sock.c"

bool g_read_data_called;
ssize_t g_bytes_read;
char g_buf[256];
struct spdk_sock *g_server_sock_read;

static void
sock(void)
{
	struct spdk_sock *listen_sock;
	struct spdk_sock *server_sock;
	struct spdk_sock *client_sock;
	char *test_string = "abcdef";
	char buffer[64];
	ssize_t bytes_read, bytes_written;
	struct iovec iov;
	int rc;

	listen_sock = spdk_sock_listen("127.0.0.1", 3260);
	SPDK_CU_ASSERT_FATAL(listen_sock != NULL);

	server_sock = spdk_sock_accept(listen_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

	client_sock = spdk_sock_connect("127.0.0.1", 3260);
	SPDK_CU_ASSERT_FATAL(client_sock != NULL);

	/*
	 * Delay a bit here before checking if server socket is
	 *  ready.
	 */
	usleep(1000);

	server_sock = spdk_sock_accept(listen_sock);
	SPDK_CU_ASSERT_FATAL(server_sock != NULL);

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

	rc = spdk_sock_close(&client_sock);
	CU_ASSERT(client_sock == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&server_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&listen_sock);
	CU_ASSERT(listen_sock == NULL);
	CU_ASSERT(rc == 0);
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
sock_group(void)
{
	struct spdk_sock_group *group;
	struct spdk_sock *listen_sock;
	struct spdk_sock *server_sock;
	struct spdk_sock *client_sock;
	char *test_string = "abcdef";
	ssize_t bytes_written;
	struct iovec iov;
	int rc;

	listen_sock = spdk_sock_listen("127.0.0.1", 3260);
	SPDK_CU_ASSERT_FATAL(listen_sock != NULL);

	server_sock = spdk_sock_accept(listen_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

	client_sock = spdk_sock_connect("127.0.0.1", 3260);
	SPDK_CU_ASSERT_FATAL(client_sock != NULL);

	usleep(1000);

	server_sock = spdk_sock_accept(listen_sock);
	SPDK_CU_ASSERT_FATAL(server_sock != NULL);

	group = spdk_sock_group_create();
	SPDK_CU_ASSERT_FATAL(group != NULL);

	/* pass null cb_fn */
	rc = spdk_sock_group_add_sock(group, server_sock, NULL, NULL);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EINVAL);

	rc = spdk_sock_group_add_sock(group, server_sock, read_data, server_sock);
	CU_ASSERT(rc == 0);

	/* try adding sock a second time */
	rc = spdk_sock_group_add_sock(group, server_sock, read_data, server_sock);
	CU_ASSERT(rc == -1);
	CU_ASSERT(errno == EBUSY);

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

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_read_data_called == true);
	CU_ASSERT(g_bytes_read == 7);

	CU_ASSERT(strncmp(test_string, g_buf, 7) == 0);

	rc = spdk_sock_close(&client_sock);
	CU_ASSERT(client_sock == NULL);
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

	rc = spdk_sock_close(&server_sock);
	CU_ASSERT(server_sock == NULL);
	CU_ASSERT(rc == 0);

	rc = spdk_sock_close(&listen_sock);
	CU_ASSERT(listen_sock == NULL);
	CU_ASSERT(rc == 0);
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
sock_group_fairness(void)
{
	struct spdk_sock_group *group;
	struct spdk_sock *listen_sock;
	struct spdk_sock *server_sock[3];
	struct spdk_sock *client_sock[3];
	char test_char = 'a';
	ssize_t bytes_written;
	struct iovec iov;
	int i, rc;

	listen_sock = spdk_sock_listen("127.0.0.1", 3260);
	SPDK_CU_ASSERT_FATAL(listen_sock != NULL);

	group = spdk_sock_group_create();
	SPDK_CU_ASSERT_FATAL(group != NULL);

	for (i = 0; i < 3; i++) {
		client_sock[i] = spdk_sock_connect("127.0.0.1", 3260);
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
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_server_sock_read == server_sock[0]);

	/*
	 * Now write another byte to client sock 0.  We want to ensure that
	 *  the sock group does not unfairly process the event for this sock
	 *  before the socks that were written to earlier.
	 */
	bytes_written = spdk_sock_writev(client_sock[0], &iov, 1);
	CU_ASSERT(bytes_written == 1);

	g_server_sock_read = NULL;
	rc = spdk_sock_group_poll_count(group, 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_server_sock_read == server_sock[1]);

	g_server_sock_read = NULL;
	rc = spdk_sock_group_poll_count(group, 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_server_sock_read == server_sock[2]);

	g_server_sock_read = NULL;
	rc = spdk_sock_group_poll_count(group, 1);
	CU_ASSERT(rc == 0);
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

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("sock", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "sock", sock) == NULL ||
		CU_add_test(suite, "sock_group", sock_group) == NULL ||
		CU_add_test(suite, "sock_group_fairness", sock_group_fairness) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
