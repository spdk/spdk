/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "spdk/sock.h"
#include "spdk/hexlify.h"
#include "spdk/nvmf.h"

#define ACCEPT_TIMEOUT_US 1000
#define CLOSE_TIMEOUT_US 1000000
#define BUFFER_SIZE 1024
#define ADDR_STR_LEN INET6_ADDRSTRLEN

static bool g_is_running;

static char *g_host;
static char *g_sock_impl_name;
static int g_port;
static bool g_is_server;
static int g_zcopy;
static int g_ktls;
static int g_tls_version;
static bool g_verbose;
static uint8_t g_psk_key[SPDK_TLS_PSK_MAX_LEN];
static uint32_t g_psk_key_size;
static char *g_psk_identity;

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
	bool is_server;
	char *host;
	const char *sock_impl_name;
	int port;
	int zcopy;
	int ktls;
	int tls_version;
	uint8_t *psk_key;
	uint32_t psk_key_size;
	char *psk_identity;

	bool verbose;
	int bytes_in;
	int bytes_out;

	struct spdk_sock *sock;

	struct spdk_sock_group *group;
	void *buf;
	struct spdk_poller *poller_in;
	struct spdk_poller *poller_out;
	struct spdk_poller *time_out;

	int rc;
	ssize_t n;
};

/*
 * Usage function for printing parameters that are specific to this application
 */
static void
hello_sock_usage(void)
{
	printf(" -E psk_key    Default PSK KEY in hexadecimal digits, e.g. 1234567890ABCDEF (only applies when sock_impl == ssl)\n");
	printf(" -H host_addr  host address\n");
	printf(" -I psk_id     Default PSK ID, e.g. psk.spdk.io (only applies when sock_impl == ssl)\n");
	printf(" -P port       port number\n");
	printf(" -N sock_impl  socket implementation, e.g., -N posix or -N uring\n");
	printf(" -S            start in server mode\n");
	printf(" -T tls_ver    TLS version, e.g., -T 12 or -T 13. If omitted, auto-negotiation will take place\n");
	printf(" -k            disable KTLS for the given sock implementation (default)\n");
	printf(" -K            enable KTLS for the given sock implementation\n");
	printf(" -V            print out additional information\n");
	printf(" -z            disable zero copy send for the given sock implementation\n");
	printf(" -Z            enable zero copy send for the given sock implementation\n");
}

/*
 * This function is called to parse the parameters that are specific to this application
 */
static int
hello_sock_parse_arg(int ch, char *arg)
{
	char *unhexlified;

	switch (ch) {
	case 'E':
		g_psk_key_size = strlen(arg) / 2;
		if (g_psk_key_size > SPDK_TLS_PSK_MAX_LEN) {
			fprintf(stderr, "Invalid PSK: too long (%"PRIu32")\n", g_psk_key_size);
			return -EINVAL;
		}
		unhexlified = spdk_unhexlify(arg);
		if (unhexlified == NULL) {
			fprintf(stderr, "Invalid PSK: not in a hex format\n");
			return -EINVAL;
		}
		memcpy(g_psk_key, unhexlified, g_psk_key_size);
		free(unhexlified);
		break;
	case 'H':
		g_host = arg;
		break;
	case 'I':
		g_psk_identity = arg;
		break;
	case 'N':
		g_sock_impl_name = arg;
		break;
	case 'P':
		g_port = spdk_strtol(arg, 10);
		if (g_port < 0) {
			fprintf(stderr, "Invalid port ID\n");
			return g_port;
		}
		break;
	case 'S':
		g_is_server = 1;
		break;
	case 'K':
		g_ktls = 1;
		break;
	case 'k':
		g_ktls = 0;
		break;
	case 'T':
		g_tls_version = spdk_strtol(arg, 10);
		if (g_tls_version < 0) {
			fprintf(stderr, "Invalid TLS version\n");
			return g_tls_version;
		}
		break;
	case 'V':
		g_verbose = true;
		break;
	case 'Z':
		g_zcopy = 1;
		break;
	case 'z':
		g_zcopy = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
hello_sock_close_timeout_poll(void *arg)
{
	struct hello_context_t *ctx = arg;
	SPDK_NOTICELOG("Connection closed\n");

	spdk_poller_unregister(&ctx->time_out);
	spdk_poller_unregister(&ctx->poller_in);
	spdk_sock_close(&ctx->sock);
	spdk_sock_group_close(&ctx->group);

	spdk_app_stop(ctx->rc);
	return SPDK_POLLER_BUSY;
}

static int
hello_sock_quit(struct hello_context_t *ctx, int rc)
{
	ctx->rc = rc;
	spdk_poller_unregister(&ctx->poller_out);
	if (!ctx->time_out) {
		ctx->time_out = SPDK_POLLER_REGISTER(hello_sock_close_timeout_poll, ctx,
						     CLOSE_TIMEOUT_US);
	}
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
			return SPDK_POLLER_IDLE;
		}

		hello_sock_quit(ctx, -1);
		SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
			    errno, spdk_strerror(errno));
		return SPDK_POLLER_BUSY;
	}

	if (rc > 0) {
		ctx->bytes_in += rc;
		buf_in[rc] = '\0';
		printf("%s", buf_in);
	}

	return SPDK_POLLER_BUSY;
}

static int
hello_sock_writev_poll(void *arg)
{
	struct hello_context_t *ctx = arg;
	int rc = 0;
	struct iovec iov;
	ssize_t n;

	/* If previously we could not send any bytes, we should try again with the same buffer. */
	if (ctx->n != 0) {
		iov.iov_base = ctx->buf;
		iov.iov_len = ctx->n;
		errno = 0;
		rc = spdk_sock_writev(ctx->sock, &iov, 1);
		if (rc < 0) {
			if (errno == EAGAIN) {
				return SPDK_POLLER_BUSY;
			}
			SPDK_ERRLOG("Write to socket failed. Closing connection...\n");
			hello_sock_quit(ctx, -1);
			return SPDK_POLLER_IDLE;
		}
		ctx->bytes_out += rc;
		ctx->n = 0;
	}

	n = read(STDIN_FILENO, ctx->buf, BUFFER_SIZE);
	if (n == 0 || !g_is_running) {
		/* EOF */
		SPDK_NOTICELOG("Closing connection...\n");
		hello_sock_quit(ctx, 0);
		return SPDK_POLLER_IDLE;
	}
	if (n > 0) {
		/*
		 * Send message to the server
		 */
		iov.iov_base = ctx->buf;
		iov.iov_len = n;
		errno = 0;
		rc = spdk_sock_writev(ctx->sock, &iov, 1);
		if (rc < 0) {
			if (errno == EAGAIN) {
				ctx->n = n;
			} else {
				SPDK_ERRLOG("Write to socket failed. Closing connection...\n");
				hello_sock_quit(ctx, -1);
				return SPDK_POLLER_IDLE;
			}
		}
		if (rc > 0) {
			ctx->bytes_out += rc;
		}
	}

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
hello_sock_connect(struct hello_context_t *ctx)
{
	int rc;
	char saddr[ADDR_STR_LEN], caddr[ADDR_STR_LEN];
	uint16_t cport, sport;
	struct spdk_sock_impl_opts impl_opts;
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;

	spdk_sock_impl_get_opts(ctx->sock_impl_name, &impl_opts, &impl_opts_size);
	impl_opts.enable_ktls = ctx->ktls;
	impl_opts.tls_version = ctx->tls_version;
	impl_opts.psk_identity = ctx->psk_identity;
	impl_opts.tls_cipher_suites = "TLS_AES_128_GCM_SHA256";
	impl_opts.psk_key = ctx->psk_key;
	impl_opts.psk_key_size = ctx->psk_key_size;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.zcopy = ctx->zcopy;
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = sizeof(impl_opts);

	SPDK_NOTICELOG("Connecting to the server on %s:%d with sock_impl(%s)\n", ctx->host, ctx->port,
		       ctx->sock_impl_name);

	ctx->sock = spdk_sock_connect_ext(ctx->host, ctx->port, ctx->sock_impl_name, &opts);
	if (ctx->sock == NULL) {
		SPDK_ERRLOG("connect error(%d): %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	rc = spdk_sock_getaddr(ctx->sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);
	if (rc < 0) {
		SPDK_ERRLOG("Cannot get connection addresses\n");
		goto err;
	}

	SPDK_NOTICELOG("Connection accepted from (%s, %hu) to (%s, %hu)\n", caddr, cport, saddr, sport);

	rc = fcntl(STDIN_FILENO, F_GETFL);
	if (rc == -1) {
		SPDK_ERRLOG("Getting file status flag failed: %s\n", strerror(errno));
		goto err;
	}

	if (fcntl(STDIN_FILENO, F_SETFL, rc | O_NONBLOCK) == -1) {
		SPDK_ERRLOG("Setting file status flag failed: %s\n", strerror(errno));
		goto err;
	}

	g_is_running = true;
	ctx->poller_in = SPDK_POLLER_REGISTER(hello_sock_recv_poll, ctx, 0);
	ctx->poller_out = SPDK_POLLER_REGISTER(hello_sock_writev_poll, ctx, 0);

	return 0;
err:
	spdk_sock_close(&ctx->sock);
	return -1;
}

static void
hello_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	int rc;
	struct hello_context_t *ctx = arg;
	struct iovec iov = {};
	ssize_t n;
	void *user_ctx;

	rc = spdk_sock_recv_next(sock, &iov.iov_base, &user_ctx);
	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		}

		if (errno != ENOTCONN && errno != ECONNRESET) {
			SPDK_ERRLOG("spdk_sock_recv_zcopy() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	if (rc > 0) {
		iov.iov_len = rc;
		ctx->bytes_in += iov.iov_len;
		n = spdk_sock_writev(sock, &iov, 1);
		if (n > 0) {
			assert(n == rc);
			ctx->bytes_out += n;
		}

		spdk_sock_group_provide_buf(ctx->group, iov.iov_base, BUFFER_SIZE, NULL);
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
		hello_sock_quit(ctx, 0);
		return SPDK_POLLER_IDLE;
	}

	while (1) {
		sock = spdk_sock_accept(ctx->sock);
		if (sock != NULL) {
			rc = spdk_sock_getaddr(sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);
			if (rc < 0) {
				SPDK_ERRLOG("Cannot get connection addresses\n");
				spdk_sock_close(&sock);
				return SPDK_POLLER_IDLE;
			}

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

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
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

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
hello_sock_listen(struct hello_context_t *ctx)
{
	struct spdk_sock_impl_opts impl_opts;
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;

	spdk_sock_impl_get_opts(ctx->sock_impl_name, &impl_opts, &impl_opts_size);
	impl_opts.enable_ktls = ctx->ktls;
	impl_opts.tls_version = ctx->tls_version;
	impl_opts.psk_identity = ctx->psk_identity;
	impl_opts.tls_cipher_suites = "TLS_AES_128_GCM_SHA256";
	impl_opts.psk_key = ctx->psk_key;
	impl_opts.psk_key_size = ctx->psk_key_size;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.zcopy = ctx->zcopy;
	opts.impl_opts = &impl_opts;
	opts.impl_opts_size = sizeof(impl_opts);

	ctx->sock = spdk_sock_listen_ext(ctx->host, ctx->port, ctx->sock_impl_name, &opts);
	if (ctx->sock == NULL) {
		SPDK_ERRLOG("Cannot create server socket\n");
		return -1;
	}

	SPDK_NOTICELOG("Listening connection on %s:%d with sock_impl(%s)\n", ctx->host, ctx->port,
		       ctx->sock_impl_name);

	/*
	 * Create sock group for server socket
	 */
	ctx->group = spdk_sock_group_create(NULL);
	if (ctx->group == NULL) {
		SPDK_ERRLOG("Cannot create sock group\n");
		spdk_sock_close(&ctx->sock);
		return -1;
	}

	spdk_sock_group_provide_buf(ctx->group, ctx->buf, BUFFER_SIZE, NULL);

	g_is_running = true;

	/*
	 * Start acceptor and group poller
	 */
	ctx->poller_in = SPDK_POLLER_REGISTER(hello_sock_accept_poll, ctx,
					      ACCEPT_TIMEOUT_US);
	ctx->poller_out = SPDK_POLLER_REGISTER(hello_sock_group_poll, ctx, 0);

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
hello_start(void *arg1)
{
	struct hello_context_t *ctx = arg1;
	int rc;

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
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "hello_sock";
	opts.shutdown_cb = hello_sock_shutdown_cb;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "E:H:I:kKN:P:ST:VzZ", NULL, hello_sock_parse_arg,
				      hello_sock_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}
	hello_context.is_server = g_is_server;
	hello_context.host = g_host;
	hello_context.sock_impl_name = g_sock_impl_name;
	hello_context.port = g_port;
	hello_context.zcopy = g_zcopy;
	hello_context.ktls = g_ktls;
	hello_context.tls_version = g_tls_version;
	hello_context.psk_key = g_psk_key;
	hello_context.psk_key_size = g_psk_key_size;
	hello_context.psk_identity = g_psk_identity;
	hello_context.verbose = g_verbose;

	if (hello_context.sock_impl_name == NULL) {
		hello_context.sock_impl_name = spdk_sock_get_default_impl();

		if (hello_context.sock_impl_name == NULL) {
			SPDK_ERRLOG("No sock implementations available!\n");
			exit(-1);
		}
	}

	hello_context.buf = calloc(1, BUFFER_SIZE);
	if (hello_context.buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory for hello_context buffer\n");
		exit(-1);
	}
	hello_context.n = 0;

	if (hello_context.is_server) {
		struct spdk_sock_impl_opts impl_opts = {};
		size_t len = sizeof(impl_opts);

		rc = spdk_sock_impl_get_opts(hello_context.sock_impl_name, &impl_opts, &len);
		if (rc < 0) {
			free(hello_context.buf);
			exit(rc);
		}

		/* Our applications will post buffers to be used for receiving. That feature
		 * is mutually exclusive with the recv pipe, so we need to disable it. */
		impl_opts.enable_recv_pipe = false;
		spdk_sock_impl_set_opts(hello_context.sock_impl_name, &impl_opts, len);
	}

	rc = spdk_app_start(&opts, hello_start, &hello_context);
	if (rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	}

	SPDK_NOTICELOG("Exiting from application\n");

	if (hello_context.verbose) {
		printf("** %d bytes received, %d bytes sent **\n",
		       hello_context.bytes_in, hello_context.bytes_out);
	}

	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	free(hello_context.buf);
	return rc;
}
