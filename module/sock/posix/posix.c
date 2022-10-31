/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#if defined(__FreeBSD__)
#include <sys/event.h>
#define SPDK_KEVENT
#else
#include <sys/epoll.h>
#define SPDK_EPOLL
#endif

#if defined(__linux__)
#include <linux/errqueue.h>
#endif

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/pipe.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/sock.h"
#include "../sock_kernel.h"

#include "openssl/crypto.h"
#include "openssl/err.h"
#include "openssl/ssl.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY)
#define SPDK_ZEROCOPY
#endif

struct spdk_posix_sock {
	struct spdk_sock	base;
	int			fd;

	uint32_t		sendmsg_idx;

	struct spdk_pipe	*recv_pipe;
	void			*recv_buf;
	int			recv_buf_sz;
	bool			pipe_has_data;
	bool			socket_has_data;
	bool			zcopy;

	int			placement_id;

	SSL_CTX			*ctx;
	SSL			*ssl;

	TAILQ_ENTRY(spdk_posix_sock)	link;
};

TAILQ_HEAD(spdk_has_data_list, spdk_posix_sock);

struct spdk_posix_sock_group_impl {
	struct spdk_sock_group_impl	base;
	int				fd;
	struct spdk_has_data_list	socks_with_data;
	int				placement_id;
};

static struct spdk_sock_impl_opts g_spdk_posix_sock_impl_opts = {
	.recv_buf_size = MIN_SO_RCVBUF_SIZE,
	.send_buf_size = MIN_SO_SNDBUF_SIZE,
	.enable_recv_pipe = true,
	.enable_quickack = false,
	.enable_placement_id = PLACEMENT_NONE,
	.enable_zerocopy_send_server = true,
	.enable_zerocopy_send_client = false,
	.zerocopy_threshold = 0,
	.tls_version = 0,
	.enable_ktls = false,
	.psk_key = NULL,
	.psk_identity = NULL
};

static struct spdk_sock_map g_map = {
	.entries = STAILQ_HEAD_INITIALIZER(g_map.entries),
	.mtx = PTHREAD_MUTEX_INITIALIZER
};

__attribute((destructor)) static void
posix_sock_map_cleanup(void)
{
	spdk_sock_map_cleanup(&g_map);
}

#define __posix_sock(sock) (struct spdk_posix_sock *)sock
#define __posix_group_impl(group) (struct spdk_posix_sock_group_impl *)group

static void
posix_sock_copy_impl_opts(struct spdk_sock_impl_opts *dest, const struct spdk_sock_impl_opts *src,
			  size_t len)
{
#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(src->field) <= len

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		dest->field = src->field; \
	}

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_zerocopy_send);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);
	SET_FIELD(enable_zerocopy_send_server);
	SET_FIELD(enable_zerocopy_send_client);
	SET_FIELD(zerocopy_threshold);
	SET_FIELD(tls_version);
	SET_FIELD(enable_ktls);
	SET_FIELD(psk_key);
	SET_FIELD(psk_identity);

#undef SET_FIELD
#undef FIELD_OK
}

static int
posix_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= *len);
	memset(opts, 0, *len);

	posix_sock_copy_impl_opts(opts, &g_spdk_posix_sock_impl_opts, *len);
	*len = spdk_min(*len, sizeof(g_spdk_posix_sock_impl_opts));

	return 0;
}

static int
posix_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= len);
	posix_sock_copy_impl_opts(&g_spdk_posix_sock_impl_opts, opts, len);

	return 0;
}

static void
posix_opts_get_impl_opts(const struct spdk_sock_opts *opts, struct spdk_sock_impl_opts *dest)
{
	/* Copy the default impl_opts first to cover cases when user's impl_opts is smaller */
	memcpy(dest, &g_spdk_posix_sock_impl_opts, sizeof(*dest));

	if (opts->impl_opts != NULL) {
		assert(sizeof(*dest) >= opts->impl_opts_size);
		posix_sock_copy_impl_opts(dest, opts->impl_opts, opts->impl_opts_size);
	}
}

static int
posix_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		   char *caddr, int clen, uint16_t *cport)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return -1;
	}

	switch (sa.ss_family) {
	case AF_UNIX:
		/* Acceptable connection types that don't have IPs */
		return 0;
	case AF_INET:
	case AF_INET6:
		/* Code below will get IP addresses */
		break;
	default:
		/* Unsupported socket family */
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, saddr, slen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	if (sport) {
		if (sa.ss_family == AF_INET) {
			*sport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*sport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getpeername(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, caddr, clen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	if (cport) {
		if (sa.ss_family == AF_INET) {
			*cport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*cport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	return 0;
}

enum posix_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
posix_sock_alloc_pipe(struct spdk_posix_sock *sock, int sz)
{
	uint8_t *new_buf;
	struct spdk_pipe *new_pipe;
	struct iovec siov[2];
	struct iovec diov[2];
	int sbytes;
	ssize_t bytes;

	if (sock->recv_buf_sz == sz) {
		return 0;
	}

	/* If the new size is 0, just free the pipe */
	if (sz == 0) {
		spdk_pipe_destroy(sock->recv_pipe);
		free(sock->recv_buf);
		sock->recv_pipe = NULL;
		sock->recv_buf = NULL;
		return 0;
	} else if (sz < MIN_SOCK_PIPE_SIZE) {
		SPDK_ERRLOG("The size of the pipe must be larger than %d\n", MIN_SOCK_PIPE_SIZE);
		return -1;
	}

	/* Round up to next 64 byte multiple */
	new_buf = calloc(SPDK_ALIGN_CEIL(sz + 1, 64), sizeof(uint8_t));
	if (!new_buf) {
		SPDK_ERRLOG("socket recv buf allocation failed\n");
		return -ENOMEM;
	}

	new_pipe = spdk_pipe_create(new_buf, sz + 1);
	if (new_pipe == NULL) {
		SPDK_ERRLOG("socket pipe allocation failed\n");
		free(new_buf);
		return -ENOMEM;
	}

	if (sock->recv_pipe != NULL) {
		/* Pull all of the data out of the old pipe */
		sbytes = spdk_pipe_reader_get_buffer(sock->recv_pipe, sock->recv_buf_sz, siov);
		if (sbytes > sz) {
			/* Too much data to fit into the new pipe size */
			spdk_pipe_destroy(new_pipe);
			free(new_buf);
			return -EINVAL;
		}

		sbytes = spdk_pipe_writer_get_buffer(new_pipe, sz, diov);
		assert(sbytes == sz);

		bytes = spdk_iovcpy(siov, 2, diov, 2);
		spdk_pipe_writer_advance(new_pipe, bytes);

		spdk_pipe_destroy(sock->recv_pipe);
		free(sock->recv_buf);
	}

	sock->recv_buf_sz = sz;
	sock->recv_buf = new_buf;
	sock->recv_pipe = new_pipe;

	return 0;
}

static int
posix_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	if (_sock->impl_opts.enable_recv_pipe) {
		rc = posix_sock_alloc_pipe(sock, sz);
		if (rc) {
			return rc;
		}
	}

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * g_spdk_posix_sock_impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, g_spdk_posix_sock_impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	_sock->impl_opts.recv_buf_size = sz;

	return 0;
}

static int
posix_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	/* Set kernel buffer size to be at least MIN_SO_SNDBUF_SIZE and
	 * g_spdk_posix_sock_impl_opts.send_buf_size. */
	min_size = spdk_max(MIN_SO_SNDBUF_SIZE, g_spdk_posix_sock_impl_opts.send_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	_sock->impl_opts.send_buf_size = sz;

	return 0;
}

static void
posix_sock_init(struct spdk_posix_sock *sock, bool enable_zero_copy)
{
#if defined(SPDK_ZEROCOPY) || defined(__linux__)
	int flag;
	int rc;
#endif

#if defined(SPDK_ZEROCOPY)
	flag = 1;

	if (enable_zero_copy) {
		/* Try to turn on zero copy sends */
		rc = setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
		if (rc == 0) {
			sock->zcopy = true;
		}
	}
#endif

#if defined(__linux__)
	flag = 1;

	if (sock->base.impl_opts.enable_quickack) {
		rc = setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}

	spdk_sock_get_placement_id(sock->fd, sock->base.impl_opts.enable_placement_id,
				   &sock->placement_id);

	if (sock->base.impl_opts.enable_placement_id == PLACEMENT_MARK) {
		/* Save placement_id */
		spdk_sock_map_insert(&g_map, sock->placement_id, NULL);
	}
#endif
}

static struct spdk_posix_sock *
posix_sock_alloc(int fd, struct spdk_sock_impl_opts *impl_opts, bool enable_zero_copy)
{
	struct spdk_posix_sock *sock;

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;
	memcpy(&sock->base.impl_opts, impl_opts, sizeof(*impl_opts));
	posix_sock_init(sock, enable_zero_copy);

	return sock;
}

static int
posix_fd_create(struct addrinfo *res, struct spdk_sock_opts *opts,
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
		/* error */
		return -1;
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
	if (rc != 0) {
		close(fd);
		/* error */
		return -1;
	}
	rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
	if (rc != 0) {
		close(fd);
		/* error */
		return -1;
	}

#if defined(SO_PRIORITY)
	if (opts->priority) {
		rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
		if (rc != 0) {
			close(fd);
			/* error */
			return -1;
		}
	}
#endif

	if (res->ai_family == AF_INET6) {
		rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
		if (rc != 0) {
			close(fd);
			/* error */
			return -1;
		}
	}

	if (opts->ack_timeout) {
#if defined(__linux__)
		to = opts->ack_timeout;
		rc = setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &to, sizeof(to));
		if (rc != 0) {
			close(fd);
			/* error */
			return -1;
		}
#else
		SPDK_WARNLOG("TCP_USER_TIMEOUT is not supported.\n");
#endif
	}

	return fd;
}

static unsigned int
posix_sock_tls_psk_server_cb(SSL *ssl,
			     const char *id,
			     unsigned char *psk,
			     unsigned int max_psk_len)
{
	long key_len;
	unsigned char *default_psk;
	struct spdk_sock_impl_opts *impl_opts;

	impl_opts = SSL_get_app_data(ssl);

	if (impl_opts->psk_key == NULL) {
		SPDK_ERRLOG("PSK is not set\n");
		goto err;
	}
	SPDK_DEBUGLOG(sock_posix, "Length of Client's PSK ID %lu\n", strlen(impl_opts->psk_identity));
	if (id == NULL) {
		SPDK_ERRLOG("Received empty PSK ID\n");
		goto err;
	}
	SPDK_DEBUGLOG(sock_posix,  "Received PSK ID '%s'\n", id);
	if (strcmp(impl_opts->psk_identity, id) != 0) {
		SPDK_ERRLOG("Unknown Client's PSK ID\n");
		goto err;
	}

	SPDK_DEBUGLOG(sock_posix, "Length of Client's PSK KEY %u\n", max_psk_len);
	default_psk = OPENSSL_hexstr2buf(impl_opts->psk_key, &key_len);
	if (default_psk == NULL) {
		SPDK_ERRLOG("Could not unhexlify PSK\n");
		goto err;
	}
	if (key_len > max_psk_len) {
		SPDK_ERRLOG("Insufficient buffer size to copy PSK\n");
		OPENSSL_free(default_psk);
		goto err;
	}

	memcpy(psk, default_psk, key_len);
	OPENSSL_free(default_psk);

	return key_len;

err:
	return 0;
}

static unsigned int
posix_sock_tls_psk_client_cb(SSL *ssl, const char *hint,
			     char *identity,
			     unsigned int max_identity_len,
			     unsigned char *psk,
			     unsigned int max_psk_len)
{
	long key_len;
	unsigned char *default_psk;
	struct spdk_sock_impl_opts *impl_opts;

	impl_opts = SSL_get_app_data(ssl);

	if (hint) {
		SPDK_DEBUGLOG(sock_posix,  "Received PSK identity hint '%s'\n", hint);
	}

	if (impl_opts->psk_key == NULL) {
		SPDK_ERRLOG("PSK is not set\n");
		goto err;
	}
	default_psk = OPENSSL_hexstr2buf(impl_opts->psk_key, &key_len);
	if (default_psk == NULL) {
		SPDK_ERRLOG("Could not unhexlify PSK\n");
		goto err;
	}
	if ((strlen(impl_opts->psk_identity) + 1 > max_identity_len)
	    || (key_len > max_psk_len)) {
		OPENSSL_free(default_psk);
		SPDK_ERRLOG("PSK ID or Key buffer is not sufficient\n");
		goto err;
	}
	spdk_strcpy_pad(identity, impl_opts->psk_identity, strlen(impl_opts->psk_identity), 0);
	SPDK_DEBUGLOG(sock_posix, "Sending PSK identity '%s'\n", identity);

	memcpy(psk, default_psk, key_len);
	SPDK_DEBUGLOG(sock_posix, "Provided out-of-band (OOB) PSK for TLS1.3 client\n");
	OPENSSL_free(default_psk);

	return key_len;

err:
	return 0;
}

static SSL_CTX *
posix_sock_create_ssl_context(const SSL_METHOD *method, struct spdk_sock_opts *opts,
			      struct spdk_sock_impl_opts *impl_opts)
{
	SSL_CTX *ctx;
	int tls_version = 0;
	bool ktls_enabled = false;
#ifdef SSL_OP_ENABLE_KTLS
	long options;
#endif

	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();
	/* Produce a SSL CTX in SSL V2 and V3 standards compliant way */
	ctx = SSL_CTX_new(method);
	if (!ctx) {
		SPDK_ERRLOG("SSL_CTX_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;
	}
	SPDK_DEBUGLOG(sock_posix, "SSL context created\n");

	switch (impl_opts->tls_version) {
	case 0:
		/* auto-negotioation */
		break;
	case SPDK_TLS_VERSION_1_1:
		tls_version = TLS1_1_VERSION;
		break;
	case SPDK_TLS_VERSION_1_2:
		tls_version = TLS1_2_VERSION;
		break;
	case SPDK_TLS_VERSION_1_3:
		tls_version = TLS1_3_VERSION;
		break;
	default:
		SPDK_ERRLOG("Incorrect TLS version provided: %d\n", impl_opts->tls_version);
		goto err;
	}

	if (tls_version) {
		SPDK_DEBUGLOG(sock_posix, "Hardening TLS version to '%d'='0x%X'\n", impl_opts->tls_version,
			      tls_version);
		if (!SSL_CTX_set_min_proto_version(ctx, tls_version)) {
			SPDK_ERRLOG("Unable to set Min TLS version to '%d'='0x%X\n", impl_opts->tls_version, tls_version);
			goto err;
		}
		if (!SSL_CTX_set_max_proto_version(ctx, tls_version)) {
			SPDK_ERRLOG("Unable to set Max TLS version to '%d'='0x%X\n", impl_opts->tls_version, tls_version);
			goto err;
		}
	}
	if (impl_opts->enable_ktls) {
		SPDK_DEBUGLOG(sock_posix, "Enabling kTLS offload\n");
#ifdef SSL_OP_ENABLE_KTLS
		options = SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
		ktls_enabled = options & SSL_OP_ENABLE_KTLS;
#else
		ktls_enabled = false;
#endif
		if (!ktls_enabled) {
			SPDK_ERRLOG("Unable to set kTLS offload via SSL_CTX_set_options(). Configure openssl with 'enable-ktls'\n");
			goto err;
		}
	}

	return ctx;

err:
	SSL_CTX_free(ctx);
	return NULL;
}

static SSL *
ssl_sock_connect_loop(SSL_CTX *ctx, int fd, struct spdk_sock_impl_opts *impl_opts)
{
	int rc;
	SSL *ssl;
	int ssl_get_error;

	ssl = SSL_new(ctx);
	if (!ssl) {
		SPDK_ERRLOG("SSL_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;
	}
	SSL_set_fd(ssl, fd);
	SSL_set_app_data(ssl, impl_opts);
	SSL_set_psk_client_callback(ssl, posix_sock_tls_psk_client_cb);
	SPDK_DEBUGLOG(sock_posix, "SSL object creation finished: %p\n", ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	while ((rc = SSL_connect(ssl)) != 1) {
		SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
		ssl_get_error = SSL_get_error(ssl, rc);
		SPDK_DEBUGLOG(sock_posix, "SSL_connect failed %d = SSL_connect(%p), %d = SSL_get_error(%p, %d)\n",
			      rc, ssl, ssl_get_error, ssl, rc);
		switch (ssl_get_error) {
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
			continue;
		default:
			break;
		}
		SPDK_ERRLOG("SSL_connect() failed, errno = %d\n", errno);
		SSL_free(ssl);
		return NULL;
	}
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "Negotiated Cipher suite:%s\n",
		      SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
	return ssl;
}

static SSL *
ssl_sock_accept_loop(SSL_CTX *ctx, int fd, struct spdk_sock_impl_opts *impl_opts)
{
	int rc;
	SSL *ssl;
	int ssl_get_error;

	ssl = SSL_new(ctx);
	if (!ssl) {
		SPDK_ERRLOG("SSL_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;
	}
	SSL_set_fd(ssl, fd);
	SSL_set_app_data(ssl, impl_opts);
	SSL_set_psk_server_callback(ssl, posix_sock_tls_psk_server_cb);
	SPDK_DEBUGLOG(sock_posix, "SSL object creation finished: %p\n", ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	while ((rc = SSL_accept(ssl)) != 1) {
		SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
		ssl_get_error = SSL_get_error(ssl, rc);
		SPDK_DEBUGLOG(sock_posix, "SSL_accept failed %d = SSL_accept(%p), %d = SSL_get_error(%p, %d)\n", rc,
			      ssl, ssl_get_error, ssl, rc);
		switch (ssl_get_error) {
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
			continue;
		default:
			break;
		}
		SPDK_ERRLOG("SSL_accept() failed, errno = %d\n", errno);
		SSL_free(ssl);
		return NULL;
	}
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "Negotiated Cipher suite:%s\n",
		      SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
	return ssl;
}

static ssize_t
SSL_readv(SSL *ssl, const struct iovec *iov, int iovcnt)
{
	int i, rc = 0;
	ssize_t total = 0;

	for (i = 0; i < iovcnt; i++) {
		rc = SSL_read(ssl, iov[i].iov_base, iov[i].iov_len);

		if (rc > 0) {
			total += rc;
		}
		if (rc != (int)iov[i].iov_len) {
			break;
		}
	}
	if (total > 0) {
		errno = 0;
		return total;
	}
	switch (SSL_get_error(ssl, rc)) {
	case SSL_ERROR_ZERO_RETURN:
		errno = ENOTCONN;
		return 0;
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
	case SSL_ERROR_WANT_CONNECT:
	case SSL_ERROR_WANT_ACCEPT:
	case SSL_ERROR_WANT_X509_LOOKUP:
	case SSL_ERROR_WANT_ASYNC:
	case SSL_ERROR_WANT_ASYNC_JOB:
	case SSL_ERROR_WANT_CLIENT_HELLO_CB:
		errno = EAGAIN;
		return -1;
	case SSL_ERROR_SYSCALL:
	case SSL_ERROR_SSL:
		errno = ENOTCONN;
		return -1;
	default:
		errno = ENOTCONN;
		return -1;
	}
}

static ssize_t
SSL_writev(SSL *ssl, struct iovec *iov, int iovcnt)
{
	int i, rc = 0;
	ssize_t total = 0;

	for (i = 0; i < iovcnt; i++) {
		rc = SSL_write(ssl, iov[i].iov_base, iov[i].iov_len);

		if (rc > 0) {
			total += rc;
		}
		if (rc != (int)iov[i].iov_len) {
			break;
		}
	}
	if (total > 0) {
		errno = 0;
		return total;
	}
	switch (SSL_get_error(ssl, rc)) {
	case SSL_ERROR_ZERO_RETURN:
		errno = ENOTCONN;
		return 0;
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
	case SSL_ERROR_WANT_CONNECT:
	case SSL_ERROR_WANT_ACCEPT:
	case SSL_ERROR_WANT_X509_LOOKUP:
	case SSL_ERROR_WANT_ASYNC:
	case SSL_ERROR_WANT_ASYNC_JOB:
	case SSL_ERROR_WANT_CLIENT_HELLO_CB:
		errno = EAGAIN;
		return -1;
	case SSL_ERROR_SYSCALL:
	case SSL_ERROR_SSL:
		errno = ENOTCONN;
		return -1;
	default:
		errno = ENOTCONN;
		return -1;
	}
}

static struct spdk_sock *
posix_sock_create(const char *ip, int port,
		  enum posix_sock_create_type type,
		  struct spdk_sock_opts *opts,
		  bool enable_ssl)
{
	struct spdk_posix_sock *sock;
	struct spdk_sock_impl_opts impl_opts;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int fd, flag;
	int rc;
	bool enable_zcopy_user_opts = true;
	bool enable_zcopy_impl_opts = true;
	SSL_CTX *ctx = 0;
	SSL *ssl = 0;

	assert(opts != NULL);
	posix_opts_get_impl_opts(opts, &impl_opts);

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
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", gai_strerror(rc), rc);
		return NULL;
	}

	/* try listen */
	fd = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
retry:
		fd = posix_fd_create(res, opts, &impl_opts);
		if (fd < 0) {
			continue;
		}
		if (type == SPDK_SOCK_CREATE_LISTEN) {
			rc = bind(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("bind() failed at port %d, errno = %d\n", port, errno);
				switch (errno) {
				case EINTR:
					/* interrupted? */
					close(fd);
					goto retry;
				case EADDRNOTAVAIL:
					SPDK_ERRLOG("IP address %s not available. "
						    "Verify IP address in config file "
						    "and make sure setup script is "
						    "run before starting spdk app.\n", ip);
				/* FALLTHROUGH */
				default:
					/* try next family */
					close(fd);
					fd = -1;
					continue;
				}
			}
			/* bind OK */
			rc = listen(fd, 512);
			if (rc != 0) {
				SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
				close(fd);
				fd = -1;
				break;
			}
			enable_zcopy_impl_opts = impl_opts.enable_zerocopy_send_server;
		} else if (type == SPDK_SOCK_CREATE_CONNECT) {
			rc = connect(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
				/* try next family */
				close(fd);
				fd = -1;
				continue;
			}
			enable_zcopy_impl_opts = impl_opts.enable_zerocopy_send_client;
			if (enable_ssl) {
				ctx = posix_sock_create_ssl_context(TLS_client_method(), opts, &impl_opts);
				if (!ctx) {
					SPDK_ERRLOG("posix_sock_create_ssl_context() failed, errno = %d\n", errno);
					close(fd);
					fd = -1;
					break;
				}
				ssl = ssl_sock_connect_loop(ctx, fd, &impl_opts);
				if (!ssl) {
					SPDK_ERRLOG("ssl_sock_connect_loop() failed, errno = %d\n", errno);
					close(fd);
					fd = -1;
					SSL_CTX_free(ctx);
					break;
				}
			}
		}

		flag = fcntl(fd, F_GETFL);
		if (fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0) {
			SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
			SSL_free(ssl);
			SSL_CTX_free(ctx);
			close(fd);
			fd = -1;
			break;
		}
		break;
	}
	freeaddrinfo(res0);

	if (fd < 0) {
		return NULL;
	}

	/* Only enable zero copy for non-loopback and non-ssl sockets. */
	enable_zcopy_user_opts = opts->zcopy && !sock_is_loopback(fd) && !enable_ssl;

	sock = posix_sock_alloc(fd, &impl_opts, enable_zcopy_user_opts && enable_zcopy_impl_opts);
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		close(fd);
		return NULL;
	}

	if (ctx) {
		sock->ctx = ctx;
	}

	if (ssl) {
		sock->ssl = ssl;
	}

	return &sock->base;
}

static struct spdk_sock *
posix_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return posix_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN, opts, false);
}

static struct spdk_sock *
posix_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return posix_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT, opts, false);
}

static struct spdk_sock *
_posix_sock_accept(struct spdk_sock *_sock, bool enable_ssl)
{
	struct spdk_posix_sock		*sock = __posix_sock(_sock);
	struct sockaddr_storage		sa;
	socklen_t			salen;
	int				rc, fd;
	struct spdk_posix_sock		*new_sock;
	int				flag;
	SSL_CTX *ctx = 0;
	SSL *ssl = 0;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);

	assert(sock != NULL);

	rc = accept(sock->fd, (struct sockaddr *)&sa, &salen);

	if (rc == -1) {
		return NULL;
	}

	fd = rc;

	flag = fcntl(fd, F_GETFL);
	if ((!(flag & O_NONBLOCK)) && (fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0)) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
		close(fd);
		return NULL;
	}

#if defined(SO_PRIORITY)
	/* The priority is not inherited, so call this function again */
	if (sock->base.opts.priority) {
		rc = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &sock->base.opts.priority, sizeof(int));
		if (rc != 0) {
			close(fd);
			return NULL;
		}
	}
#endif

	/* Establish SSL connection */
	if (enable_ssl) {
		ctx = posix_sock_create_ssl_context(TLS_server_method(), &sock->base.opts, &sock->base.impl_opts);
		if (!ctx) {
			SPDK_ERRLOG("posix_sock_create_ssl_context() failed, errno = %d\n", errno);
			close(fd);
			return NULL;
		}
		ssl = ssl_sock_accept_loop(ctx, fd, &sock->base.impl_opts);
		if (!ssl) {
			SPDK_ERRLOG("ssl_sock_accept_loop() failed, errno = %d\n", errno);
			close(fd);
			SSL_CTX_free(ctx);
			return NULL;
		}
	}

	/* Inherit the zero copy feature from the listen socket */
	new_sock = posix_sock_alloc(fd, &sock->base.impl_opts, sock->zcopy);
	if (new_sock == NULL) {
		close(fd);
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		return NULL;
	}

	if (ctx) {
		new_sock->ctx = ctx;
	}

	if (ssl) {
		new_sock->ssl = ssl;
	}

	return &new_sock->base;
}

static struct spdk_sock *
posix_sock_accept(struct spdk_sock *_sock)
{
	return _posix_sock_accept(_sock, false);
}

static int
posix_sock_close(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);

	assert(TAILQ_EMPTY(&_sock->pending_reqs));

	if (sock->ssl != NULL) {
		SSL_shutdown(sock->ssl);
	}

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	close(sock->fd);

	SSL_free(sock->ssl);
	SSL_CTX_free(sock->ctx);

	spdk_pipe_destroy(sock->recv_pipe);
	free(sock->recv_buf);
	free(sock);

	return 0;
}

#ifdef SPDK_ZEROCOPY
static int
_sock_check_zcopy(struct spdk_sock *sock)
{
	struct spdk_posix_sock *psock = __posix_sock(sock);
	struct msghdr msgh = {};
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err)];
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;
	struct spdk_sock_request *req, *treq;
	bool found;

	msgh.msg_control = buf;
	msgh.msg_controllen = sizeof(buf);

	while (true) {
		rc = recvmsg(psock->fd, &msgh, MSG_ERRQUEUE);

		if (rc < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return 0;
			}

			if (!TAILQ_EMPTY(&sock->pending_reqs)) {
				SPDK_ERRLOG("Attempting to receive from ERRQUEUE yielded error, but pending list still has orphaned entries\n");
			} else {
				SPDK_WARNLOG("Recvmsg yielded an error!\n");
			}
			return 0;
		}

		cm = CMSG_FIRSTHDR(&msgh);
		if (!(cm &&
		      ((cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR) ||
		       (cm->cmsg_level == SOL_IPV6 && cm->cmsg_type == IPV6_RECVERR)))) {
			SPDK_WARNLOG("Unexpected cmsg level or type!\n");
			return 0;
		}

		serr = (struct sock_extended_err *)CMSG_DATA(cm);
		if (serr->ee_errno != 0 || serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
			SPDK_WARNLOG("Unexpected extended error origin\n");
			return 0;
		}

		/* Most of the time, the pending_reqs array is in the exact
		 * order we need such that all of the requests to complete are
		 * in order, in the front. It is guaranteed that all requests
		 * belonging to the same sendmsg call are sequential, so once
		 * we encounter one match we can stop looping as soon as a
		 * non-match is found.
		 */
		for (idx = serr->ee_info; idx <= serr->ee_data; idx++) {
			found = false;
			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, internal.link, treq) {
				if (!req->internal.is_zcopy) {
					/* This wasn't a zcopy request. It was just waiting in line to complete */
					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}
				} else if (req->internal.offset == idx) {
					found = true;
					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}
				} else if (found) {
					break;
				}
			}
		}
	}

	return 0;
}
#endif

static int
_sock_flush(struct spdk_sock *sock)
{
	struct spdk_posix_sock *psock = __posix_sock(sock);
	struct msghdr msg = {};
	int flags;
	struct iovec iovs[IOV_BATCH_SIZE];
	int iovcnt;
	int retval;
	struct spdk_sock_request *req;
	int i;
	ssize_t rc, sent;
	unsigned int offset;
	size_t len;
	bool is_zcopy = false;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (sock->cb_cnt > 0) {
		errno = EAGAIN;
		return -1;
	}

#ifdef SPDK_ZEROCOPY
	if (psock->zcopy) {
		flags = MSG_ZEROCOPY | MSG_NOSIGNAL;
	} else
#endif
	{
		flags = MSG_NOSIGNAL;
	}

	iovcnt = spdk_sock_prep_reqs(sock, iovs, 0, NULL, &flags);
	if (iovcnt == 0) {
		return 0;
	}

#ifdef SPDK_ZEROCOPY
	is_zcopy = flags & MSG_ZEROCOPY;
#endif

	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;

	if (psock->ssl) {
		rc = SSL_writev(psock->ssl, iovs, iovcnt);
	} else {
		rc = sendmsg(psock->fd, &msg, flags);
	}
	if (rc <= 0) {
		if (rc == 0 || errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && psock->zcopy)) {
			errno = EAGAIN;
		}
		return -1;
	}

	sent = rc;

	if (is_zcopy) {
		/* Handling overflow case, because we use psock->sendmsg_idx - 1 for the
		 * req->internal.offset, so sendmsg_idx should not be zero  */
		if (spdk_unlikely(psock->sendmsg_idx == UINT32_MAX)) {
			psock->sendmsg_idx = 1;
		} else {
			psock->sendmsg_idx++;
		}
	}

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		offset = req->internal.offset;

		/* req->internal.is_zcopy is true when the whole req or part of it is sent with zerocopy */
		req->internal.is_zcopy = is_zcopy;

		for (i = 0; i < req->iovcnt; i++) {
			/* Advance by the offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;

			if (len > (size_t)rc) {
				/* This element was partially sent. */
				req->internal.offset += rc;
				return sent;
			}

			offset = 0;
			req->internal.offset += len;
			rc -= len;
		}

		/* Handled a full request. */
		spdk_sock_request_pend(sock, req);

		if (!req->internal.is_zcopy && req == TAILQ_FIRST(&sock->pending_reqs)) {
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			retval = spdk_sock_request_put(sock, req, 0);
			if (retval) {
				break;
			}
		} else {
			/* Re-use the offset field to hold the sendmsg call index. The
			 * index is 0 based, so subtract one here because we've already
			 * incremented above. */
			req->internal.offset = psock->sendmsg_idx - 1;
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	return sent;
}

static int
posix_sock_flush(struct spdk_sock *sock)
{
#ifdef SPDK_ZEROCOPY
	struct spdk_posix_sock *psock = __posix_sock(sock);

	if (psock->zcopy && !TAILQ_EMPTY(&sock->pending_reqs)) {
		_sock_check_zcopy(sock);
	}
#endif

	return _sock_flush(sock);
}

static ssize_t
posix_sock_recv_from_pipe(struct spdk_posix_sock *sock, struct iovec *diov, int diovcnt)
{
	struct iovec siov[2];
	int sbytes;
	ssize_t bytes;
	struct spdk_posix_sock_group_impl *group;

	sbytes = spdk_pipe_reader_get_buffer(sock->recv_pipe, sock->recv_buf_sz, siov);
	if (sbytes < 0) {
		errno = EINVAL;
		return -1;
	} else if (sbytes == 0) {
		errno = EAGAIN;
		return -1;
	}

	bytes = spdk_iovcpy(siov, 2, diov, diovcnt);

	if (bytes == 0) {
		/* The only way this happens is if diov is 0 length */
		errno = EINVAL;
		return -1;
	}

	spdk_pipe_reader_advance(sock->recv_pipe, bytes);

	/* If we drained the pipe, mark it appropriately */
	if (spdk_pipe_reader_bytes_available(sock->recv_pipe) == 0) {
		assert(sock->pipe_has_data == true);

		group = __posix_group_impl(sock->base.group_impl);
		if (group && !sock->socket_has_data) {
			TAILQ_REMOVE(&group->socks_with_data, sock, link);
		}

		sock->pipe_has_data = false;
	}

	return bytes;
}

static inline ssize_t
posix_sock_read(struct spdk_posix_sock *sock)
{
	struct iovec iov[2];
	int bytes_avail, bytes_recvd;
	struct spdk_posix_sock_group_impl *group;

	bytes_avail = spdk_pipe_writer_get_buffer(sock->recv_pipe, sock->recv_buf_sz, iov);

	if (bytes_avail <= 0) {
		return bytes_avail;
	}

	if (sock->ssl) {
		bytes_recvd = SSL_readv(sock->ssl, iov, 2);
	} else {
		bytes_recvd = readv(sock->fd, iov, 2);
	}

	assert(sock->pipe_has_data == false);

	if (bytes_recvd <= 0) {
		/* Errors count as draining the socket data */
		if (sock->base.group_impl && sock->socket_has_data) {
			group = __posix_group_impl(sock->base.group_impl);
			TAILQ_REMOVE(&group->socks_with_data, sock, link);
		}

		sock->socket_has_data = false;

		return bytes_recvd;
	}

	spdk_pipe_writer_advance(sock->recv_pipe, bytes_recvd);

#if DEBUG
	if (sock->base.group_impl) {
		assert(sock->socket_has_data == true);
	}
#endif

	sock->pipe_has_data = true;
	if (bytes_recvd < bytes_avail) {
		/* We drained the kernel socket entirely. */
		sock->socket_has_data = false;
	}

	return bytes_recvd;
}

static ssize_t
posix_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(sock->base.group_impl);
	int rc, i;
	size_t len;

	if (sock->recv_pipe == NULL) {
		assert(sock->pipe_has_data == false);
		if (group && sock->socket_has_data) {
			sock->socket_has_data = false;
			TAILQ_REMOVE(&group->socks_with_data, sock, link);
		}
		if (sock->ssl) {
			return SSL_readv(sock->ssl, iov, iovcnt);
		} else {
			return readv(sock->fd, iov, iovcnt);
		}
	}

	/* If the socket is not in a group, we must assume it always has
	 * data waiting for us because it is not epolled */
	if (!sock->pipe_has_data && (group == NULL || sock->socket_has_data)) {
		/* If the user is receiving a sufficiently large amount of data,
		 * receive directly to their buffers. */
		len = 0;
		for (i = 0; i < iovcnt; i++) {
			len += iov[i].iov_len;
		}

		if (len >= MIN_SOCK_PIPE_SIZE) {
			/* TODO: Should this detect if kernel socket is drained? */
			if (sock->ssl) {
				return SSL_readv(sock->ssl, iov, iovcnt);
			} else {
				return readv(sock->fd, iov, iovcnt);
			}
		}

		/* Otherwise, do a big read into our pipe */
		rc = posix_sock_read(sock);
		if (rc <= 0) {
			return rc;
		}
	}

	return posix_sock_recv_from_pipe(sock, iov, iovcnt);
}

static ssize_t
posix_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	struct iovec iov[1];

	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	return posix_sock_readv(sock, iov, 1);
}

static void
posix_sock_readv_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	req->cb_fn(req->cb_arg, -ENOTSUP);
}

static ssize_t
posix_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	/* In order to process a writev, we need to flush any asynchronous writes
	 * first. */
	rc = _sock_flush(_sock);
	if (rc < 0) {
		return rc;
	}

	if (!TAILQ_EMPTY(&_sock->queued_reqs)) {
		/* We weren't able to flush all requests */
		errno = EAGAIN;
		return -1;
	}

	if (sock->ssl) {
		return SSL_writev(sock->ssl, iov, iovcnt);
	} else {
		return writev(sock->fd, iov, iovcnt);
	}
}

static void
posix_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	int rc;

	spdk_sock_request_queue(sock, req);

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (sock->queued_iovcnt >= IOV_BATCH_SIZE) {
		rc = _sock_flush(sock);
		if (rc < 0 && errno != EAGAIN) {
			spdk_sock_abort_requests(sock);
		}
	}
}

static int
posix_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int val;
	int rc;

	assert(sock != NULL);

	val = nbytes;
	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
	if (rc != 0) {
		return -1;
	}
	return 0;
}

static bool
posix_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);
}

static bool
posix_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}

static bool
posix_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	uint8_t byte;
	int rc;

	rc = recv(sock->fd, &byte, 1, MSG_PEEK);
	if (rc == 0) {
		return false;
	}

	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return true;
		}

		return false;
	}

	return true;
}

static struct spdk_sock_group_impl *
posix_sock_group_impl_get_optimal(struct spdk_sock *_sock, struct spdk_sock_group_impl *hint)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct spdk_sock_group_impl *group_impl;

	if (sock->placement_id != -1) {
		spdk_sock_map_lookup(&g_map, sock->placement_id, &group_impl, hint);
		return group_impl;
	}

	return NULL;
}

static struct spdk_sock_group_impl *
posix_sock_group_impl_create(void)
{
	struct spdk_posix_sock_group_impl *group_impl;
	int fd;

#if defined(SPDK_EPOLL)
	fd = epoll_create1(0);
#elif defined(SPDK_KEVENT)
	fd = kqueue();
#endif
	if (fd == -1) {
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		close(fd);
		return NULL;
	}

	group_impl->fd = fd;
	TAILQ_INIT(&group_impl->socks_with_data);
	group_impl->placement_id = -1;

	if (g_spdk_posix_sock_impl_opts.enable_placement_id == PLACEMENT_CPU) {
		spdk_sock_map_insert(&g_map, spdk_env_get_current_core(), &group_impl->base);
		group_impl->placement_id = spdk_env_get_current_core();
	}

	return &group_impl->base;
}

static void
posix_sock_mark(struct spdk_posix_sock_group_impl *group, struct spdk_posix_sock *sock,
		int placement_id)
{
#if defined(SO_MARK)
	int rc;

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_MARK,
			&placement_id, sizeof(placement_id));
	if (rc != 0) {
		/* Not fatal */
		SPDK_ERRLOG("Error setting SO_MARK\n");
		return;
	}

	rc = spdk_sock_map_insert(&g_map, placement_id, &group->base);
	if (rc != 0) {
		/* Not fatal */
		SPDK_ERRLOG("Failed to insert sock group into map: %d\n", rc);
		return;
	}

	sock->placement_id = placement_id;
#endif
}

static void
posix_sock_update_mark(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);

	if (group->placement_id == -1) {
		group->placement_id = spdk_sock_map_find_free(&g_map);

		/* If a free placement id is found, update existing sockets in this group */
		if (group->placement_id != -1) {
			struct spdk_sock  *sock, *tmp;

			TAILQ_FOREACH_SAFE(sock, &_group->socks, link, tmp) {
				posix_sock_mark(group, __posix_sock(sock), group->placement_id);
			}
		}
	}

	if (group->placement_id != -1) {
		/*
		 * group placement id is already determined for this poll group.
		 * Mark socket with group's placement id.
		 */
		posix_sock_mark(group, __posix_sock(_sock), group->placement_id);
	}
}

static int
posix_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

#if defined(SPDK_EPOLL)
	struct epoll_event event;

	memset(&event, 0, sizeof(event));
	/* EPOLLERR is always on even if we don't set it, but be explicit for clarity */
	event.events = EPOLLIN | EPOLLERR;
	event.data.ptr = sock;

	rc = epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);
#elif defined(SPDK_KEVENT)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_ADD, 0, 0, sock);

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
#endif

	if (rc != 0) {
		return rc;
	}

	/* switched from another polling group due to scheduling */
	if (spdk_unlikely(sock->recv_pipe != NULL  &&
			  (spdk_pipe_reader_bytes_available(sock->recv_pipe) > 0))) {
		sock->pipe_has_data = true;
		sock->socket_has_data = false;
		TAILQ_INSERT_TAIL(&group->socks_with_data, sock, link);
	}

	if (g_spdk_posix_sock_impl_opts.enable_placement_id == PLACEMENT_MARK) {
		posix_sock_update_mark(_group, _sock);
	} else if (sock->placement_id != -1) {
		rc = spdk_sock_map_insert(&g_map, sock->placement_id, &group->base);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to insert sock group into map: %d\n", rc);
			/* Do not treat this as an error. The system will continue running. */
		}
	}

	return rc;
}

static int
posix_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int rc;

	if (sock->pipe_has_data || sock->socket_has_data) {
		TAILQ_REMOVE(&group->socks_with_data, sock, link);
		sock->pipe_has_data = false;
		sock->socket_has_data = false;
	}

	if (sock->placement_id != -1) {
		spdk_sock_map_release(&g_map, sock->placement_id);
	}

#if defined(SPDK_EPOLL)
	struct epoll_event event;

	/* Event parameter is ignored but some old kernel version still require it. */
	rc = epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event);
#elif defined(SPDK_KEVENT)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
	if (rc == 0 && event.flags & EV_ERROR) {
		rc = -1;
		errno = event.data;
	}
#endif

	spdk_sock_abort_requests(_sock);

	return rc;
}

static int
posix_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			   struct spdk_sock **socks)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	struct spdk_sock *sock, *tmp;
	int num_events, i, rc;
	struct spdk_posix_sock *psock, *ptmp;
#if defined(SPDK_EPOLL)
	struct epoll_event events[MAX_EVENTS_PER_POLL];
#elif defined(SPDK_KEVENT)
	struct kevent events[MAX_EVENTS_PER_POLL];
	struct timespec ts = {0};
#endif

#ifdef SPDK_ZEROCOPY
	/* When all of the following conditions are met
	 * - non-blocking socket
	 * - zero copy is enabled
	 * - interrupts suppressed (i.e. busy polling)
	 * - the NIC tx queue is full at the time sendmsg() is called
	 * - epoll_wait determines there is an EPOLLIN event for the socket
	 * then we can get into a situation where data we've sent is queued
	 * up in the kernel network stack, but interrupts have been suppressed
	 * because other traffic is flowing so the kernel misses the signal
	 * to flush the software tx queue. If there wasn't incoming data
	 * pending on the socket, then epoll_wait would have been sufficient
	 * to kick off the send operation, but since there is a pending event
	 * epoll_wait does not trigger the necessary operation.
	 *
	 * We deal with this by checking for all of the above conditions and
	 * additionally looking for EPOLLIN events that were not consumed from
	 * the last poll loop. We take this to mean that the upper layer is
	 * unable to consume them because it is blocked waiting for resources
	 * to free up, and those resources are most likely freed in response
	 * to a pending asynchronous write completing.
	 *
	 * Additionally, sockets that have the same placement_id actually share
	 * an underlying hardware queue. That means polling one of them is
	 * equivalent to polling all of them. As a quick mechanism to avoid
	 * making extra poll() calls, stash the last placement_id during the loop
	 * and only poll if it's not the same. The overwhelmingly common case
	 * is that all sockets in this list have the same placement_id because
	 * SPDK is intentionally grouping sockets by that value, so even
	 * though this won't stop all extra calls to poll(), it's very fast
	 * and will catch all of them in practice.
	 */
	int last_placement_id = -1;

	TAILQ_FOREACH(psock, &group->socks_with_data, link) {
		if (psock->zcopy && psock->placement_id >= 0 &&
		    psock->placement_id != last_placement_id) {
			struct pollfd pfd = {psock->fd, POLLIN | POLLERR, 0};

			poll(&pfd, 1, 0);
			last_placement_id = psock->placement_id;
		}
	}
#endif

	/* This must be a TAILQ_FOREACH_SAFE because while flushing,
	 * a completion callback could remove the sock from the
	 * group. */
	TAILQ_FOREACH_SAFE(sock, &_group->socks, link, tmp) {
		rc = _sock_flush(sock);
		if (rc < 0 && errno != EAGAIN) {
			spdk_sock_abort_requests(sock);
		}
	}

	assert(max_events > 0);

#if defined(SPDK_EPOLL)
	num_events = epoll_wait(group->fd, events, max_events, 0);
#elif defined(SPDK_KEVENT)
	num_events = kevent(group->fd, NULL, 0, events, max_events, &ts);
#endif

	if (num_events == -1) {
		return -1;
	} else if (num_events == 0 && !TAILQ_EMPTY(&_group->socks)) {
		sock = TAILQ_FIRST(&_group->socks);
		psock = __posix_sock(sock);
		/* poll() is called here to busy poll the queue associated with
		 * first socket in list and potentially reap incoming data.
		 */
		if (sock->opts.priority) {
			struct pollfd pfd = {0, 0, 0};

			pfd.fd = psock->fd;
			pfd.events = POLLIN | POLLERR;
			poll(&pfd, 1, 0);
		}
	}

	for (i = 0; i < num_events; i++) {
#if defined(SPDK_EPOLL)
		sock = events[i].data.ptr;
		psock = __posix_sock(sock);

#ifdef SPDK_ZEROCOPY
		if (events[i].events & EPOLLERR) {
			rc = _sock_check_zcopy(sock);
			/* If the socket was closed or removed from
			 * the group in response to a send ack, don't
			 * add it to the array here. */
			if (rc || sock->cb_fn == NULL) {
				continue;
			}
		}
#endif
		if ((events[i].events & EPOLLIN) == 0) {
			continue;
		}

#elif defined(SPDK_KEVENT)
		sock = events[i].udata;
		psock = __posix_sock(sock);
#endif

		/* If the socket is not already in the list, add it now */
		if (!psock->socket_has_data && !psock->pipe_has_data) {
			TAILQ_INSERT_TAIL(&group->socks_with_data, psock, link);
		}
		psock->socket_has_data = true;
	}

	num_events = 0;

	TAILQ_FOREACH_SAFE(psock, &group->socks_with_data, link, ptmp) {
		if (num_events == max_events) {
			break;
		}

		/* If the socket's cb_fn is NULL, just remove it from the
		 * list and do not add it to socks array */
		if (spdk_unlikely(psock->base.cb_fn == NULL)) {
			psock->socket_has_data = false;
			psock->pipe_has_data = false;
			TAILQ_REMOVE(&group->socks_with_data, psock, link);
			continue;
		}

		socks[num_events++] = &psock->base;
	}

	/* Cycle the has_data list so that each time we poll things aren't
	 * in the same order. Say we have 6 sockets in the list, named as follows:
	 * A B C D E F
	 * And all 6 sockets had epoll events, but max_events is only 3. That means
	 * psock currently points at D. We want to rearrange the list to the following:
	 * D E F A B C
	 *
	 * The variables below are named according to this example to make it easier to
	 * follow the swaps.
	 */
	if (psock != NULL) {
		struct spdk_posix_sock *pa, *pc, *pd, *pf;

		/* Capture pointers to the elements we need */
		pd = psock;
		pc = TAILQ_PREV(pd, spdk_has_data_list, link);
		pa = TAILQ_FIRST(&group->socks_with_data);
		pf = TAILQ_LAST(&group->socks_with_data, spdk_has_data_list);

		/* Break the link between C and D */
		pc->link.tqe_next = NULL;

		/* Connect F to A */
		pf->link.tqe_next = pa;
		pa->link.tqe_prev = &pf->link.tqe_next;

		/* Fix up the list first/last pointers */
		group->socks_with_data.tqh_first = pd;
		group->socks_with_data.tqh_last = &pc->link.tqe_next;

		/* D is in front of the list, make tqe prev pointer point to the head of list */
		pd->link.tqe_prev = &group->socks_with_data.tqh_first;
	}

	return num_events;
}

static int
posix_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	int rc;

	if (g_spdk_posix_sock_impl_opts.enable_placement_id == PLACEMENT_CPU) {
		spdk_sock_map_release(&g_map, spdk_env_get_current_core());
	}

	rc = close(group->fd);
	free(group);
	return rc;
}

static struct spdk_net_impl g_posix_net_impl = {
	.name		= "posix",
	.getaddr	= posix_sock_getaddr,
	.connect	= posix_sock_connect,
	.listen		= posix_sock_listen,
	.accept		= posix_sock_accept,
	.close		= posix_sock_close,
	.recv		= posix_sock_recv,
	.readv		= posix_sock_readv,
	.readv_async	= posix_sock_readv_async,
	.writev		= posix_sock_writev,
	.writev_async	= posix_sock_writev_async,
	.flush		= posix_sock_flush,
	.set_recvlowat	= posix_sock_set_recvlowat,
	.set_recvbuf	= posix_sock_set_recvbuf,
	.set_sendbuf	= posix_sock_set_sendbuf,
	.is_ipv6	= posix_sock_is_ipv6,
	.is_ipv4	= posix_sock_is_ipv4,
	.is_connected	= posix_sock_is_connected,
	.group_impl_get_optimal	= posix_sock_group_impl_get_optimal,
	.group_impl_create	= posix_sock_group_impl_create,
	.group_impl_add_sock	= posix_sock_group_impl_add_sock,
	.group_impl_remove_sock = posix_sock_group_impl_remove_sock,
	.group_impl_poll	= posix_sock_group_impl_poll,
	.group_impl_close	= posix_sock_group_impl_close,
	.get_opts	= posix_sock_impl_get_opts,
	.set_opts	= posix_sock_impl_set_opts,
};

SPDK_NET_IMPL_REGISTER(posix, &g_posix_net_impl, DEFAULT_SOCK_PRIORITY + 1);

static struct spdk_sock *
ssl_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return posix_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN, opts, true);
}

static struct spdk_sock *
ssl_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return posix_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT, opts, true);
}

static struct spdk_sock *
ssl_sock_accept(struct spdk_sock *_sock)
{
	return _posix_sock_accept(_sock, true);
}

static struct spdk_net_impl g_ssl_net_impl = {
	.name		= "ssl",
	.getaddr	= posix_sock_getaddr,
	.connect	= ssl_sock_connect,
	.listen		= ssl_sock_listen,
	.accept		= ssl_sock_accept,
	.close		= posix_sock_close,
	.recv		= posix_sock_recv,
	.readv		= posix_sock_readv,
	.writev		= posix_sock_writev,
	.writev_async	= posix_sock_writev_async,
	.flush		= posix_sock_flush,
	.set_recvlowat	= posix_sock_set_recvlowat,
	.set_recvbuf	= posix_sock_set_recvbuf,
	.set_sendbuf	= posix_sock_set_sendbuf,
	.is_ipv6	= posix_sock_is_ipv6,
	.is_ipv4	= posix_sock_is_ipv4,
	.is_connected	= posix_sock_is_connected,
	.group_impl_get_optimal	= posix_sock_group_impl_get_optimal,
	.group_impl_create	= posix_sock_group_impl_create,
	.group_impl_add_sock	= posix_sock_group_impl_add_sock,
	.group_impl_remove_sock = posix_sock_group_impl_remove_sock,
	.group_impl_poll	= posix_sock_group_impl_poll,
	.group_impl_close	= posix_sock_group_impl_close,
	.get_opts	= posix_sock_impl_get_opts,
	.set_opts	= posix_sock_impl_set_opts,
};

SPDK_NET_IMPL_REGISTER(ssl, &g_ssl_net_impl, DEFAULT_SOCK_PRIORITY);
SPDK_LOG_REGISTER_COMPONENT(sock_posix)
