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
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "spdk/sock.h"
#include "spdk/net.h"

#define ACCEPT_TIMEOUT_US 1000

static char *g_host;
static int g_port;
static bool g_is_server;

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
	bool is_server;
	char *host;
	int port;

	struct spdk_sock *sock;

	struct spdk_sock_group *group;
	struct spdk_poller *poller_in;
	struct spdk_poller *poller_out;
};

/*
 * Usage function for printing parameters that are specific to this application
 */
static void
hello_sock_usage(void)
{
	printf(" -H host_addr host address\n");
	printf(" -P port port number\n");
	printf(" -S start in listen mode\n");
}

/*
 * This function is called to parse the parameters that are specific to this application
 */
static void hello_sock_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'H':
		g_host = arg;
		break;
	case 'P':
		g_port = atoi(arg);
		break;
	case 'S':
		g_is_server = 1;
		break;
	}
}

static int
hello_sock_recv_poll(void *arg)
{
	struct hello_context_t		*ctx = arg;
	int				rc;
	char buf_in[1024];

	/*
	 * Get response
	 */
	rc = spdk_sock_recv(ctx->sock, buf_in, sizeof(buf_in) - 1);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
			    errno, spdk_strerror(errno));
		return -1;
	}

	if (rc > 0) {
		buf_in[rc] = '\0';
		printf("%s", buf_in);
	}

	return 0;
}

static int
hello_sock_writev_poll(void *arg)
{
	struct hello_context_t		*ctx = arg;
	int				rc = 0;
	char buf_out[1024];
	struct iovec iov;
	ssize_t n;


	n = read(0, buf_out, sizeof(buf_out));
	if (n == 0) {
		/* EOF */
		spdk_app_stop(0);
		return 0;
	}
	if (n > 0) {
		/*
		 * Send message to the server
		 */
		iov.iov_base = buf_out;
		iov.iov_len = n;
		rc = spdk_sock_writev(ctx->sock, &iov, 1);
	}
	return rc;
}

static int
hello_sock_connect(struct hello_context_t *ctx)
{
	int rc;
	char				saddr[1024], caddr[1024];

	SPDK_NOTICELOG("Connecting to the server on %s:%d\n", ctx->host, ctx->port);

	/*
	 * Connect to the server
	 */
	ctx->sock = spdk_sock_connect(ctx->host, ctx->port);
	if (ctx->sock == NULL) {
		SPDK_ERRLOG("connect error(%d): %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	rc = spdk_sock_getaddr(ctx->sock, saddr, sizeof(saddr), caddr, sizeof(caddr));
	if (rc < 0) {
		SPDK_ERRLOG("Cannot get connection addresses\n");
		spdk_sock_close(&ctx->sock);
		return -1;
	}

	SPDK_NOTICELOG("Connection accepted from %s to %s\n", caddr, saddr);

	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

	ctx->poller_in = spdk_poller_register(hello_sock_recv_poll, ctx, 0);
	ctx->poller_out = spdk_poller_register(hello_sock_writev_poll, ctx, 0);

	return 0;
}

static void
hello_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	ssize_t n;
	char buf[1024];
	struct iovec iov;

	n = spdk_sock_recv(sock, buf, sizeof(buf));
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
			return;
		}

		SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
			    errno, spdk_strerror(errno));
	}

	if (n > 0) {
		iov.iov_base = buf;
		iov.iov_len = n;
		spdk_sock_writev(sock, &iov, 1);
		return;
	}

	/* Connection closed */
	SPDK_NOTICELOG("Connection closed\n");
	spdk_sock_group_remove_sock(group, sock);
	spdk_sock_close(&sock);
}

static int
hello_sock_accept_poll(void *arg)
{
	struct hello_context_t		*ctx = arg;
	struct spdk_sock		*sock;
	int				rc;
	int				count = 0;

	char				saddr[1024], caddr[1024];

	while (1) {
		sock = spdk_sock_accept(ctx->sock);
		if (sock != NULL) {

			spdk_sock_getaddr(sock, saddr, sizeof(saddr), caddr, sizeof(caddr));

			SPDK_NOTICELOG("Accepting a new connection from %s to %s\n",
				       caddr, saddr);

			rc = spdk_sock_group_add_sock(ctx->group, sock,
						      hello_sock_cb, ctx);

			if (rc < 0) {
				spdk_sock_close(&sock);
				SPDK_ERRLOG("failed\n");
				break;
			}

			count++;
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("accept error(%d): %s\n", errno, spdk_strerror(errno));
			}
			break;
		}
	}

	return count;
}

static int
hello_sock_group_poll(void *arg)
{
	struct hello_context_t		*ctx = arg;
	int rc;

	rc = spdk_sock_group_poll(ctx->group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", ctx->group);
	}

	return -1;
}

static int
hello_sock_listen(struct hello_context_t *ctx)
{
	/*
	 * Listen
	 */
	ctx->sock = spdk_sock_listen(ctx->host, ctx->port);
	if (ctx->sock == NULL) {
		SPDK_ERRLOG("Cannot create server socket\n");
		return -1;
	}

	SPDK_NOTICELOG("Listening connection on %s:%d\n", ctx->host, ctx->port);

	/*
	 * Create sock group for server socket
	 */
	ctx->group = spdk_sock_group_create();

	/*
	 * Start acceptor
	 */
	ctx->poller_in = spdk_poller_register(hello_sock_accept_poll, ctx,
					      ACCEPT_TIMEOUT_US);
	ctx->poller_out = spdk_poller_register(hello_sock_group_poll, ctx, 0);

	return 0;
}

/*
 * Our initial event that kicks off everything from main().
 */
static void
hello_start(void *arg1, void *arg2)
{
	struct hello_context_t *ctx = arg1;
	int rc = 0;

	SPDK_NOTICELOG("Successfully started the application\n");

	if (ctx->is_server) {
		rc = hello_sock_listen(ctx);
	} else {
		rc = hello_sock_connect(ctx);
	}

	if (rc) {
		//SPDK_ERRLOG();
		spdk_app_stop(-1);
		return;
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	struct hello_context_t hello_context = {};

	/* Set default values in opts structure. */
	spdk_app_opts_init(&opts);
	opts.name = "hello_sock";
	opts.config_file = "sock.conf";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "H:P:S", hello_sock_parse_arg,
				      hello_sock_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}
	hello_context.is_server = g_is_server;
	hello_context.host = g_host;
	hello_context.port = g_port;

	rc = spdk_net_framework_start();
	if (rc) {
		SPDK_ERRLOG("ERROR starting application\n");
		goto end;
	}

	rc = spdk_app_start(&opts, hello_start, &hello_context, NULL);
	if (rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	}

end:
	SPDK_NOTICELOG("Exiting from application\n");
	if (hello_context.sock != NULL) {
		spdk_sock_close(&hello_context.sock);
	}

	spdk_net_framework_fini();

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
