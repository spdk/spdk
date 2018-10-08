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
#define CLOSE_TIMEOUT_US 1000000
#define BUFFER_SIZE 1024
#define ADDR_STR_LEN INET6_ADDRSTRLEN

static bool g_is_running;

static char *g_host;
static int g_port;
static bool g_is_server;
static bool g_verbose;

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
	bool is_server;
	char *host;
	int port;

	bool verbose;
	int bytes_in;
	int bytes_out;

	struct spdk_sock *sock;

	struct spdk_sock_group *group;
	struct spdk_poller *poller_in;
	struct spdk_poller *poller_out;
	struct spdk_poller *time_out;
};

/*
 * Usage function for printing parameters that are specific to this application
 */
static void
hello_sock_usage(void)
{
	printf(" -H host_addr  host address\n");
	printf(" -P port       port number\n");
	printf(" -S            start in server mode\n");
	printf(" -V            print out additional informations");
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
	case 'V':
		g_verbose = true;
	}
}

static int
hello_sock_close_timeout_poll(void *arg)
{
	struct hello_context_t *ctx = arg;
	SPDK_NOTICELOG("Connection closed\n");

	spdk_poller_unregister(&ctx->time_out);
	spdk_poller_unregister(&ctx->poller_in);
	spdk_sock_close(&ctx->sock);

	spdk_app_stop(0);
	return 0;
}

static int
hello_sock_recv_poll(void *arg)
{
	struct hello_context_t *ctx = arg;
	int rc;
	char buf_in[BUFFER_SIZE];

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
		ctx->bytes_in += rc;
		buf_in[rc] = '\0';
		printf("%s", buf_in);
	}

	return 0;
}

static int
hello_sock_writev_poll(void *arg)
{
	struct hello_context_t *ctx = arg;
	int rc = 0;
	char buf_out[BUFFER_SIZE];
	struct iovec iov;
	ssize_t n;

	n = read(STDIN_FILENO, buf_out, sizeof(buf_out));
	if (n == 0 || !g_is_running) {
		/* EOF */
		SPDK_NOTICELOG("Closing connection...\n");

		ctx->time_out = spdk_poller_register(hello_sock_close_timeout_poll, ctx,
						     CLOSE_TIMEOUT_US);

		spdk_poller_unregister(&ctx->poller_out);
		return 0;
	}
	if (n > 0) {
		/*
		 * Send message to the server
		 */
		iov.iov_base = buf_out;
		iov.iov_len = n;
		rc = spdk_sock_writev(ctx->sock, &iov, 1);
		if (rc > 0) {
			ctx->bytes_out += rc;
		}
	}
	return rc;
}

static int
hello_sock_connect(struct hello_context_t *ctx)
{
	int rc;
	char saddr[ADDR_STR_LEN], caddr[ADDR_STR_LEN];
	uint16_t cport, sport;

	SPDK_NOTICELOG("Connecting to the server on %s:%d\n", ctx->host, ctx->port);

	ctx->sock = spdk_sock_connect(ctx->host, ctx->port);
	if (ctx->sock == NULL) {
		SPDK_ERRLOG("connect error(%d): %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	rc = spdk_sock_getaddr(ctx->sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot get connection addresses\n");
		spdk_sock_close(&ctx->sock);
		return -1;
	}

	SPDK_NOTICELOG("Connection accepted from (%s, %hu) to (%s, %hu)\n", caddr, cport, saddr, sport);

	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

	g_is_running = true;
	ctx->poller_in = spdk_poller_register(hello_sock_recv_poll, ctx, 0);
	ctx->poller_out = spdk_poller_register(hello_sock_writev_poll, ctx, 0);

	return 0;
}

static void
hello_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	ssize_t n;
	char buf[BUFFER_SIZE];
	struct iovec iov;
	struct hello_context_t *ctx = arg;

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
		ctx->bytes_in += n;
		iov.iov_base = buf;
		iov.iov_len = n;
		n = spdk_sock_writev(sock, &iov, 1);
		if (n > 0) {
			ctx->bytes_out += n;
		}
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
	struct hello_context_t *ctx = arg;
	struct spdk_sock *sock;
	int rc;
	int count = 0;
	char saddr[ADDR_STR_LEN], caddr[ADDR_STR_LEN];
	uint16_t cport, sport;

	if (!g_is_running) {
		spdk_poller_unregister(&ctx->poller_in);
		spdk_poller_unregister(&ctx->poller_out);
		spdk_sock_close(&ctx->sock);
		spdk_sock_group_close(&ctx->group);
		spdk_app_stop(0);
		return 0;
	}

	while (1) {
		sock = spdk_sock_accept(ctx->sock);
		if (sock != NULL) {
			spdk_sock_getaddr(sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);

			SPDK_NOTICELOG("Accepting a new connection from (%s, %hu) to (%s, %hu)\n",
				       caddr, cport, saddr, sport);

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
	struct hello_context_t *ctx = arg;
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

	g_is_running = true;

	/*
	 * Start acceptor and group poller
	 */
	ctx->poller_in = spdk_poller_register(hello_sock_accept_poll, ctx,
					      ACCEPT_TIMEOUT_US);
	ctx->poller_out = spdk_poller_register(hello_sock_group_poll, ctx, 0);

	return 0;
}

static void
hello_sock_shutdown_cb(void)
{
	g_is_running = false;
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
	opts.shutdown_cb = hello_sock_shutdown_cb;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "H:P:SV", NULL, hello_sock_parse_arg,
				      hello_sock_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}
	hello_context.is_server = g_is_server;
	hello_context.host = g_host;
	hello_context.port = g_port;
	hello_context.verbose = g_verbose;

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

	if (hello_context.verbose) {
		printf("** %d bytes received, %d bytes sent **\n",
		       hello_context.bytes_in, hello_context.bytes_out);
	}

	spdk_net_framework_fini();

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}
