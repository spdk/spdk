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
#include "spdk/net.h"
#include "spdk/file.h"
#include "spdk_internal/sock_module.h"
#include "spdk/net.h"

#include "openssl/crypto.h"
#include "openssl/err.h"
#include "openssl/ssl.h"

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY)
#define SPDK_ZEROCOPY
#endif

struct posix_connect_ctx {
	int fd;
	bool ssl;
	struct addrinfo *first_res;
	struct addrinfo *next_res;
	struct spdk_sock_opts opts;
	struct spdk_sock_impl_opts impl_opts;
	uint64_t timeout_tsc;
	int set_recvlowat;
	int set_recvbuf;
	int set_sendbuf;
	spdk_sock_connect_cb_fn cb_fn;
	void *cb_arg;
};

struct spdk_posix_sock {
	struct spdk_sock	base;
	int			fd;

	uint32_t		sendmsg_idx;

	struct spdk_pipe	*recv_pipe;
	int			recv_buf_sz;
	bool			pipe_has_data;
	bool			socket_has_data;
	bool			zcopy;
	bool			ready;

	int			placement_id;

	SSL_CTX			*ssl_ctx;
	SSL			*ssl;

	TAILQ_ENTRY(spdk_posix_sock)	link;

	char			interface_name[IFNAMSIZ];

	struct posix_connect_ctx	*connect_ctx;
};

TAILQ_HEAD(spdk_has_data_list, spdk_posix_sock);

struct spdk_posix_sock_group_impl {
	struct spdk_sock_group_impl	base;
	int				fd;
	struct spdk_interrupt		*intr;
	struct spdk_has_data_list	socks_with_data;
	int				placement_id;
	struct spdk_pipe_group		*pipe_group;
};

static struct spdk_sock_impl_opts g_posix_impl_opts = {
	.recv_buf_size = DEFAULT_SO_RCVBUF_SIZE,
	.send_buf_size = DEFAULT_SO_SNDBUF_SIZE,
	.enable_recv_pipe = true,
	.enable_quickack = false,
	.enable_placement_id = PLACEMENT_NONE,
	.enable_zerocopy_send_server = true,
	.enable_zerocopy_send_client = false,
	.zerocopy_threshold = 0,
	.tls_version = 0,
	.enable_ktls = false,
	.psk_key = NULL,
	.psk_key_size = 0,
	.psk_identity = NULL,
	.get_key = NULL,
	.get_key_ctx = NULL,
	.tls_cipher_suites = NULL
};

static struct spdk_sock_impl_opts g_ssl_impl_opts = {
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
	SET_FIELD(psk_key_size);
	SET_FIELD(psk_identity);
	SET_FIELD(get_key);
	SET_FIELD(get_key_ctx);
	SET_FIELD(tls_cipher_suites);

#undef SET_FIELD
#undef FIELD_OK
}

static int
_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, struct spdk_sock_impl_opts *impl_opts,
		    size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= *len);
	memset(opts, 0, *len);

	posix_sock_copy_impl_opts(opts, impl_opts, *len);
	*len = spdk_min(*len, sizeof(*impl_opts));

	return 0;
}

static int
posix_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	return _sock_impl_get_opts(opts, &g_posix_impl_opts, len);
}

static int
ssl_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	return _sock_impl_get_opts(opts, &g_ssl_impl_opts, len);
}

static int
_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, struct spdk_sock_impl_opts *impl_opts,
		    size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

	assert(sizeof(*opts) >= len);
	posix_sock_copy_impl_opts(impl_opts, opts, len);

	return 0;
}

static int
posix_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	return _sock_impl_set_opts(opts, &g_posix_impl_opts, len);
}

static int
ssl_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	return _sock_impl_set_opts(opts, &g_ssl_impl_opts, len);
}

static void
_opts_get_impl_opts(const struct spdk_sock_opts *opts, struct spdk_sock_impl_opts *dest,
		    const struct spdk_sock_impl_opts *default_impl)
{
	/* Copy the default impl_opts first to cover cases when user's impl_opts is smaller */
	memcpy(dest, default_impl, sizeof(*dest));

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

	if (!sock->ready) {
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		errno = sock->connect_ctx ? EAGAIN : ENOTCONN;
		return -1;
	}

	assert(sock != NULL);
	return spdk_net_getaddr(sock->fd, saddr, slen, sport, caddr, clen, cport);
}

static const char *
posix_sock_get_interface_name(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	char saddr[64];
	int rc;

	rc = spdk_net_getaddr(sock->fd, saddr, sizeof(saddr), NULL, NULL, 0, NULL);
	if (rc != 0) {
		return NULL;
	}

	rc = spdk_net_get_interface_name(saddr, sock->interface_name,
					 sizeof(sock->interface_name));
	if (rc != 0) {
		return NULL;
	}

	return sock->interface_name;
}

static int32_t
posix_sock_get_numa_id(struct spdk_sock *sock)
{
	const char *interface_name;
	uint32_t numa_id;
	int rc;

	interface_name = posix_sock_get_interface_name(sock);
	if (interface_name == NULL) {
		return SPDK_ENV_NUMA_ID_ANY;
	}

	rc = spdk_read_sysfs_attribute_uint32(&numa_id,
					      "/sys/class/net/%s/device/numa_node", interface_name);
	if (rc == 0 && numa_id <= INT32_MAX) {
		return (int32_t)numa_id;
	} else {
		return SPDK_ENV_NUMA_ID_ANY;
	}
}

static int
posix_sock_alloc_pipe(struct spdk_posix_sock *sock, int sz)
{
	uint8_t *new_buf, *old_buf;
	struct spdk_pipe *new_pipe;
	struct iovec siov[2];
	struct iovec diov[2];
	int sbytes;
	ssize_t bytes;
	int rc;

	if (sock->recv_buf_sz == sz) {
		return 0;
	}

	/* If the new size is 0, just free the pipe */
	if (sz == 0) {
		old_buf = spdk_pipe_destroy(sock->recv_pipe);
		free(old_buf);
		sock->recv_pipe = NULL;
		return 0;
	} else if (sz < MIN_SOCK_PIPE_SIZE) {
		SPDK_ERRLOG("The size of the pipe must be larger than %d\n", MIN_SOCK_PIPE_SIZE);
		return -1;
	}

	/* Round up to next 64 byte multiple */
	rc = posix_memalign((void **)&new_buf, 64, sz);
	if (rc != 0) {
		SPDK_ERRLOG("socket recv buf allocation failed\n");
		return -ENOMEM;
	}
	memset(new_buf, 0, sz);

	new_pipe = spdk_pipe_create(new_buf, sz);
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
			old_buf = spdk_pipe_destroy(new_pipe);
			free(old_buf);
			return -EINVAL;
		}

		sbytes = spdk_pipe_writer_get_buffer(new_pipe, sz, diov);
		assert(sbytes == sz);

		bytes = spdk_iovcpy(siov, 2, diov, 2);
		spdk_pipe_writer_advance(new_pipe, bytes);

		old_buf = spdk_pipe_destroy(sock->recv_pipe);
		free(old_buf);
	}

	sock->recv_buf_sz = sz;
	sock->recv_pipe = new_pipe;

	if (sock->base.group_impl) {
		struct spdk_posix_sock_group_impl *group;

		group = __posix_group_impl(sock->base.group_impl);
		spdk_pipe_group_add(group->pipe_group, sock->recv_pipe);
	}

	return 0;
}

static int
posix_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	int min_size;
	int rc;

	assert(sock != NULL);

	if (!sock->ready) {
		if (sock->connect_ctx) {
			sock->connect_ctx->set_recvbuf = sz;
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");
		errno = ENOTCONN;
		return -1;
	}

	if (_sock->impl_opts.enable_recv_pipe) {
		rc = posix_sock_alloc_pipe(sock, sz);
		if (rc) {
			errno = rc;
			return -1;
		}
	}

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE and
	 * _sock->impl_opts.recv_buf_size. */
	min_size = spdk_max(MIN_SO_RCVBUF_SIZE, _sock->impl_opts.recv_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc) {
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

	if (!sock->ready) {
		if (sock->connect_ctx) {
			sock->connect_ctx->set_sendbuf = sz;
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");
		errno = ENOTCONN;
		return -1;
	}

	/* Set kernel buffer size to be at least MIN_SO_SNDBUF_SIZE and
	 * _sock->impl_opts.send_buf_size. */
	min_size = spdk_max(MIN_SO_SNDBUF_SIZE, _sock->impl_opts.send_buf_size);

	if (sz < min_size) {
		sz = min_size;
	}

	rc = setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc) {
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
			/* Zcopy notification index from the kernel for first sendmsg is 0, so we need to start
			 * incrementing internal counter from UINT32_MAX. */
			sock->sendmsg_idx = UINT32_MAX;
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
	sock->ready = true;
}

static struct spdk_posix_sock *
posix_sock_alloc(int fd, struct spdk_sock_impl_opts *impl_opts)
{
	struct spdk_posix_sock *sock;

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;
	memcpy(&sock->base.impl_opts, impl_opts, sizeof(*impl_opts));
	return sock;
}

static int
posix_sock_psk_find_session_server_cb(SSL *ssl, const unsigned char *identity,
				      size_t identity_len, SSL_SESSION **sess)
{
	struct spdk_sock_impl_opts *impl_opts = SSL_get_app_data(ssl);
	uint8_t key[SSL_MAX_MASTER_KEY_LENGTH] = {};
	int keylen;
	int rc, i;
	STACK_OF(SSL_CIPHER) *ciphers;
	const SSL_CIPHER *cipher;
	const char *cipher_name;
	const char *user_cipher = NULL;
	bool found = false;

	if (impl_opts->get_key) {
		rc = impl_opts->get_key(key, sizeof(key), &user_cipher, identity, impl_opts->get_key_ctx);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to find PSK for identity: %s\n", identity);
			return 0;
		}
		keylen = rc;
	} else {
		if (impl_opts->psk_key == NULL) {
			SPDK_ERRLOG("PSK is not set\n");
			return 0;
		}

		SPDK_DEBUGLOG(sock_posix, "Length of Client's PSK ID %lu\n", strlen(impl_opts->psk_identity));
		if (strcmp(impl_opts->psk_identity, identity) != 0) {
			SPDK_ERRLOG("Unknown Client's PSK ID\n");
			return 0;
		}
		keylen = impl_opts->psk_key_size;

		memcpy(key, impl_opts->psk_key, keylen);
		user_cipher = impl_opts->tls_cipher_suites;
	}

	if (user_cipher == NULL) {
		SPDK_ERRLOG("Cipher suite not set\n");
		return 0;
	}

	*sess = SSL_SESSION_new();
	if (*sess == NULL) {
		SPDK_ERRLOG("Unable to allocate new SSL session\n");
		return 0;
	}

	ciphers = SSL_get_ciphers(ssl);
	for (i = 0; i < sk_SSL_CIPHER_num(ciphers); i++) {
		cipher = sk_SSL_CIPHER_value(ciphers, i);
		cipher_name = SSL_CIPHER_get_name(cipher);

		if (strcmp(user_cipher, cipher_name) == 0) {
			rc = SSL_SESSION_set_cipher(*sess, cipher);
			if (rc != 1) {
				SPDK_ERRLOG("Unable to set cipher: %s\n", cipher_name);
				goto err;
			}
			found = true;
			break;
		}
	}
	if (found == false) {
		SPDK_ERRLOG("No suitable cipher found\n");
		goto err;
	}

	SPDK_DEBUGLOG(sock_posix, "Cipher selected: %s\n", cipher_name);

	rc = SSL_SESSION_set_protocol_version(*sess, TLS1_3_VERSION);
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set TLS version: %d\n", TLS1_3_VERSION);
		goto err;
	}

	rc = SSL_SESSION_set1_master_key(*sess, key, keylen);
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set PSK for session\n");
		goto err;
	}

	return 1;

err:
	SSL_SESSION_free(*sess);
	*sess = NULL;
	return 0;
}

static int
posix_sock_psk_use_session_client_cb(SSL *ssl, const EVP_MD *md, const unsigned char **identity,
				     size_t *identity_len, SSL_SESSION **sess)
{
	struct spdk_sock_impl_opts *impl_opts = SSL_get_app_data(ssl);
	int rc, i;
	STACK_OF(SSL_CIPHER) *ciphers;
	const SSL_CIPHER *cipher;
	const char *cipher_name;
	long keylen;
	bool found = false;

	if (impl_opts->psk_key == NULL) {
		SPDK_ERRLOG("PSK is not set\n");
		return 0;
	}
	if (impl_opts->psk_key_size > SSL_MAX_MASTER_KEY_LENGTH) {
		SPDK_ERRLOG("PSK too long\n");
		return 0;
	}
	keylen = impl_opts->psk_key_size;

	if (impl_opts->tls_cipher_suites == NULL) {
		SPDK_ERRLOG("Cipher suite not set\n");
		return 0;
	}
	*sess = SSL_SESSION_new();
	if (*sess == NULL) {
		SPDK_ERRLOG("Unable to allocate new SSL session\n");
		return 0;
	}

	ciphers = SSL_get_ciphers(ssl);
	for (i = 0; i < sk_SSL_CIPHER_num(ciphers); i++) {
		cipher = sk_SSL_CIPHER_value(ciphers, i);
		cipher_name = SSL_CIPHER_get_name(cipher);

		if (strcmp(impl_opts->tls_cipher_suites, cipher_name) == 0) {
			rc = SSL_SESSION_set_cipher(*sess, cipher);
			if (rc != 1) {
				SPDK_ERRLOG("Unable to set cipher: %s\n", cipher_name);
				goto err;
			}
			found = true;
			break;
		}
	}
	if (found == false) {
		SPDK_ERRLOG("No suitable cipher found\n");
		goto err;
	}

	SPDK_DEBUGLOG(sock_posix, "Cipher selected: %s\n", cipher_name);

	rc = SSL_SESSION_set_protocol_version(*sess, TLS1_3_VERSION);
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set TLS version: %d\n", TLS1_3_VERSION);
		goto err;
	}

	rc = SSL_SESSION_set1_master_key(*sess, impl_opts->psk_key, keylen);
	if (rc != 1) {
		SPDK_ERRLOG("Unable to set PSK for session\n");
		goto err;
	}

	*identity_len = strlen(impl_opts->psk_identity);
	*identity = impl_opts->psk_identity;

	return 1;

err:
	SSL_SESSION_free(*sess);
	*sess = NULL;
	return 0;
}

static SSL_CTX *
posix_sock_create_ssl_context(const SSL_METHOD *method, struct spdk_sock_impl_opts *impl_opts)
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
		/* auto-negotiation */
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

	/* SSL_CTX_set_ciphersuites() return 1 if the requested
	 * cipher suite list was configured, and 0 otherwise. */
	if (impl_opts->tls_cipher_suites != NULL &&
	    SSL_CTX_set_ciphersuites(ctx, impl_opts->tls_cipher_suites) != 1) {
		SPDK_ERRLOG("Unable to set TLS cipher suites for SSL'\n");
		goto err;
	}

	return ctx;

err:
	SSL_CTX_free(ctx);
	return NULL;
}

static SSL *
ssl_sock_setup_connect(SSL_CTX *ctx, int fd)
{
	SSL *ssl;

	ssl = SSL_new(ctx);
	if (!ssl) {
		SPDK_ERRLOG("SSL_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;
	}
	SSL_set_fd(ssl, fd);
	SSL_set_connect_state(ssl);
	SSL_set_psk_use_session_callback(ssl, posix_sock_psk_use_session_client_cb);
	SPDK_DEBUGLOG(sock_posix, "SSL object creation finished: %p\n", ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "Negotiated Cipher suite:%s\n",
		      SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
	return ssl;
}

static SSL *
ssl_sock_setup_accept(SSL_CTX *ctx, int fd)
{
	SSL *ssl;

	ssl = SSL_new(ctx);
	if (!ssl) {
		SPDK_ERRLOG("SSL_new() failed, msg = %s\n", ERR_error_string(ERR_peek_last_error(), NULL));
		return NULL;
	}
	SSL_set_fd(ssl, fd);
	SSL_set_accept_state(ssl);
	SSL_set_psk_find_session_callback(ssl, posix_sock_psk_find_session_server_cb);
	SPDK_DEBUGLOG(sock_posix, "SSL object creation finished: %p\n", ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "%s = SSL_state_string_long(%p)\n", SSL_state_string_long(ssl), ssl);
	SPDK_DEBUGLOG(sock_posix, "Negotiated Cipher suite:%s\n",
		      SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
	return ssl;
}

static int
posix_sock_configure_ssl(struct spdk_posix_sock *sock, bool client)
{
	SSL* (*setup_fn)(SSL_CTX *, int) = client ? ssl_sock_setup_connect : ssl_sock_setup_accept;

	sock->ssl_ctx = posix_sock_create_ssl_context(client ? TLS_client_method() : TLS_server_method(),
			&sock->base.impl_opts);
	if (!sock->ssl_ctx) {
		SPDK_ERRLOG("posix_sock_create_ssl_context() failed\n");
		return -EPROTO;
	}

	sock->ssl = setup_fn(sock->ssl_ctx, sock->fd);
	if (!sock->ssl) {
		SPDK_ERRLOG("ssl_sock_setup_%s() failed\n", client ? "connect" : "accept");
		SSL_CTX_free(sock->ssl_ctx);
		sock->ssl_ctx = NULL;
		return -EPROTO;
	}

	SSL_set_app_data(sock->ssl, &sock->base.impl_opts);
	return 0;
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
_posix_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts, bool enable_ssl)
{
	struct spdk_sock_impl_opts impl_opts;
	struct spdk_posix_sock *sock;
	struct addrinfo *res0;
	int rc, fd = -1;

	assert(opts != NULL);
	if (enable_ssl) {
		_opts_get_impl_opts(opts, &impl_opts, &g_ssl_impl_opts);
	} else {
		_opts_get_impl_opts(opts, &impl_opts, &g_posix_impl_opts);
	}

	res0 = spdk_sock_posix_getaddrinfo(ip, port);
	if (!res0) {
		return NULL;
	}

	for (struct addrinfo *res = res0; res != NULL; res = res->ai_next) {
retry:
		fd = spdk_sock_posix_fd_create(res, opts, &impl_opts);
		if (fd < 0) {
			continue;
		}

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

		rc = listen(fd, 512);
		if (rc != 0) {
			SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
			close(fd);
			fd = -1;
			break;
		}

		if (spdk_fd_set_nonblock(fd)) {
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

	sock = posix_sock_alloc(fd, &impl_opts);
	if (sock == NULL) {
		close(fd);
		return NULL;
	}

	/* Only enable zero copy for non-loopback and non-ssl sockets. */
	posix_sock_init(sock, opts->zcopy && !spdk_net_is_loopback(fd) && !enable_ssl &&
			impl_opts.enable_zerocopy_send_server);
	return &sock->base;
}

static int
_sock_posix_connect_async(struct posix_connect_ctx *ctx)
{
	int rc = -ENOENT, fd;

	/* It is either first execution or continuation; in that case invalid fd is expected. */
	assert(ctx->fd == -1);
	for (; ctx->next_res != NULL; ctx->next_res = ctx->next_res->ai_next) {
		rc = spdk_sock_posix_fd_create(ctx->next_res, &ctx->opts, &ctx->impl_opts);
		if (rc < 0) {
			continue;
		}

		fd = rc;
		rc = spdk_sock_posix_fd_connect_async(fd, ctx->next_res, &ctx->opts);
		if (rc < 0) {
			close(fd);
			continue;
		}

		ctx->next_res = ctx->next_res->ai_next;
		break;
	}

	if (rc < 0) {
		return rc;
	}

	ctx->fd = fd;
	ctx->timeout_tsc = !ctx->opts.connect_timeout ? 0 : spdk_get_ticks() + ctx->opts.connect_timeout *
			   spdk_get_ticks_hz() / 1000;
	return 0;
}

static void
sock_posix_connect_ctx_cleanup(struct posix_connect_ctx **_ctx, int rc)
{
	struct posix_connect_ctx *ctx = *_ctx;

	*_ctx = NULL;
	if (!ctx) {
		return;
	}

	freeaddrinfo(ctx->first_res);
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, rc);
	}

	free(ctx);
}

static int
sock_posix_connect_async(struct addrinfo *res, struct spdk_sock_opts *opts,
			 struct spdk_sock_impl_opts *impl_opts, bool ssl, spdk_sock_connect_cb_fn cb_fn, void *cb_arg,
			 struct posix_connect_ctx **_ctx)
{
	struct posix_connect_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->first_res = ctx->next_res = res;
	ctx->opts = *opts;
	ctx->impl_opts = *impl_opts;
	ctx->ssl = ssl;
	ctx->fd = -1;
	ctx->set_recvlowat = -1;
	ctx->set_recvbuf = -1;
	ctx->set_sendbuf = -1;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = _sock_posix_connect_async(ctx);
	if (rc < 0) {
		free(ctx);
		return rc;
	}

	*_ctx = ctx;
	return 0;
}

static int posix_connect_poller(struct spdk_posix_sock *sock);

static struct spdk_sock *
_posix_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts, bool async,
		    bool enable_ssl, spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct spdk_sock_impl_opts impl_opts;
	struct spdk_posix_sock *sock = NULL;
	struct addrinfo *res0 = NULL;
	int rc;

	assert(opts != NULL);
	if (enable_ssl) {
		_opts_get_impl_opts(opts, &impl_opts, &g_ssl_impl_opts);
	} else {
		_opts_get_impl_opts(opts, &impl_opts, &g_posix_impl_opts);
	}

	res0 = spdk_sock_posix_getaddrinfo(ip, port);
	if (!res0) {
		rc = -EIO;
		goto err;
	}

	sock = posix_sock_alloc(-1, &impl_opts);
	if (!sock) {
		rc = -ENOMEM;
		goto err;
	}

	rc = sock_posix_connect_async(res0, opts, &impl_opts, enable_ssl, cb_fn, cb_arg,
				      &sock->connect_ctx);
	if (rc < 0) {
		goto err;
	}

	sock->fd = sock->connect_ctx->fd;
	if (async) {
		return &sock->base;
	}

	do {
		rc = posix_connect_poller(sock);
	} while (rc == -EAGAIN);

	if (!sock->ready) {
		free(sock);
		return NULL;
	}

	return &sock->base;

err:
	free(sock);
	if (res0) {
		freeaddrinfo(res0);
	}

	if (cb_fn) {
		cb_fn(cb_arg, rc);
	}

	return NULL;
}

static struct spdk_sock *
posix_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_listen(ip, port, opts, false);
}

static struct spdk_sock *
posix_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_connect(ip, port, opts, false, false, NULL, NULL);
}

static struct spdk_sock *
posix_sock_connect_async(const char *ip, int port, struct spdk_sock_opts *opts,
			 spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	return _posix_sock_connect(ip, port, opts, true, false, cb_fn, cb_arg);
}

static struct spdk_sock *
_posix_sock_accept(struct spdk_sock *_sock, bool enable_ssl)
{
	struct spdk_posix_sock		*sock = __posix_sock(_sock);
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(sock->base.group_impl);
	struct sockaddr_storage		sa;
	socklen_t			salen;
	int				rc, fd;
	struct spdk_posix_sock		*new_sock;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);

	assert(sock != NULL);

	/* epoll_wait will trigger again if there is more than one request */
	if (group && sock->socket_has_data) {
		sock->socket_has_data = false;
		TAILQ_REMOVE(&group->socks_with_data, sock, link);
	}

	rc = accept(sock->fd, (struct sockaddr *)&sa, &salen);

	if (rc == -1) {
		return NULL;
	}

	fd = rc;

	if (spdk_fd_set_nonblock(fd)) {
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

	new_sock = posix_sock_alloc(fd, &sock->base.impl_opts);
	if (new_sock == NULL) {
		close(fd);
		return NULL;
	}

	if (enable_ssl) {
		rc = posix_sock_configure_ssl(new_sock, false);
		if (rc < 0) {
			free(new_sock);
			close(fd);
			return NULL;
		}
	}

	/* Inherit the zero copy feature from the listen socket */
	posix_sock_init(new_sock, sock->zcopy);
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
	void *pipe_buf;

	assert(TAILQ_EMPTY(&_sock->pending_reqs));

	sock_posix_connect_ctx_cleanup(&sock->connect_ctx, -ECONNRESET);

	if (sock->ssl != NULL) {
		SSL_shutdown(sock->ssl);
	}

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	if (sock->fd != -1) {
		close(sock->fd);
	}

	SSL_free(sock->ssl);
	SSL_CTX_free(sock->ssl_ctx);

	if (sock->recv_pipe) {
		pipe_buf = spdk_pipe_destroy(sock->recv_pipe);
		free(pipe_buf);
	}

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
		idx = serr->ee_info;
		while (true) {
			found = false;
			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, internal.link, treq) {
				if (!req->internal.pending_zcopy) {
					/* This wasn't a zcopy request. It was just waiting in line
					 * to complete. */
					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}
				} else if (req->internal.zcopy_idx == idx) {
					found = true;
					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}
				} else if (found) {
					break;
				}
			}

			if (idx == serr->ee_data) {
				break;
			}

			idx++;
		}

		/* If the req is sent partially (still queued) and we just received its zcopy
		 * notification, next chunk may be sent without zcopy and should result in the req
		 * completion if it is the last chunk. Clear the pending flag to allow it.
		 * Checking the first queued req and the last index is enough, because only one req
		 * can be partially sent and it is the last one we can get notification for. */
		req = TAILQ_FIRST(&sock->queued_reqs);
		if (req && req->internal.pending_zcopy &&
		    req->internal.zcopy_idx == serr->ee_data) {
			req->internal.pending_zcopy = false;
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
	ssize_t rc;
	unsigned int offset;
	size_t len;
	bool is_zcopy = false;

	rc = posix_connect_poller(psock);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}

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

	if (is_zcopy) {
		psock->sendmsg_idx++;
	}

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		offset = req->internal.offset;

		if (is_zcopy) {
			/* Cache sendmsg_idx because full request might not be handled and next
			 * chunk may be sent without zero copy. */
			req->internal.pending_zcopy = true;
			req->internal.zcopy_idx = psock->sendmsg_idx;
		}

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
				return 0;
			}

			offset = 0;
			req->internal.offset += len;
			rc -= len;
		}

		/* Handled a full request. */
		spdk_sock_request_pend(sock, req);

		/* We can't put the req if zero-copy is not completed or it is not first
		 * in the line. */
		if (!req->internal.pending_zcopy && req == TAILQ_FIRST(&sock->pending_reqs)) {
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			retval = spdk_sock_request_put(sock, req, 0);
			if (retval) {
				break;
			}
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	return 0;
}

static int
posix_sock_flush(struct spdk_sock *sock)
{
#ifdef SPDK_ZEROCOPY
	struct spdk_posix_sock *psock = __posix_sock(sock);
	int rc, _errno;

	rc = _sock_flush(sock);
	_errno = errno;

	if (psock->zcopy && !TAILQ_EMPTY(&sock->pending_reqs)) {
		_sock_check_zcopy(sock);
	}

	/* Restore errno to prevent potential change when executing zcopy check. */
	errno = _errno;
	return rc;
#else
	return _sock_flush(sock);
#endif
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

	rc = posix_connect_poller(sock);
	if (rc < 0) {
		errno = -rc;
		return -1;
	}

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

static int
posix_sock_recv_next(struct spdk_sock *_sock, void **buf, void **ctx)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct iovec iov;
	ssize_t rc;

	if (sock->recv_pipe != NULL) {
		errno = ENOTSUP;
		return -1;
	}

	iov.iov_len = spdk_sock_group_get_buf(_sock->group_impl->group, &iov.iov_base, ctx);
	if (iov.iov_len == 0) {
		errno = ENOBUFS;
		return -1;
	}

	rc = posix_sock_readv(_sock, &iov, 1);
	if (rc <= 0) {
		spdk_sock_group_provide_buf(_sock->group_impl->group, iov.iov_base, iov.iov_len, *ctx);
		return rc;
	}

	*buf = iov.iov_base;

	return rc;
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

	assert(sock != NULL);

	if (!sock->ready) {
		if (sock->connect_ctx) {
			sock->connect_ctx->set_recvlowat = nbytes;
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");
		errno = ENOTCONN;
		return -1;
	}

	val = nbytes;
	return setsockopt(sock->fd, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
}

static bool
posix_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_posix_sock *sock = __posix_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	if (!sock->ready) {
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		errno = sock->connect_ctx ? EAGAIN : ENOTCONN;
		return -1;
	}

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

	if (!sock->ready) {
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		errno = sock->connect_ctx ? EAGAIN : ENOTCONN;
		return -1;
	}

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

	rc = posix_connect_poller(sock);
	if (rc < 0) {
		errno = -rc;
		return false;
	}

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

	if (!sock->ready) {
		SPDK_ERRLOG("Connection %s.\n", sock->connect_ctx ? "in progress" : "failed");
		errno = sock->connect_ctx ? EAGAIN : ENOTCONN;
		return NULL;
	}

	if (sock->placement_id != -1) {
		spdk_sock_map_lookup(&g_map, sock->placement_id, &group_impl, hint);
		return group_impl;
	}

	return NULL;
}

static struct spdk_sock_group_impl *
_sock_group_impl_create(uint32_t enable_placement_id)
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

	group_impl->pipe_group = spdk_pipe_group_create();
	if (group_impl->pipe_group == NULL) {
		SPDK_ERRLOG("pipe_group allocation failed\n");
		free(group_impl);
		close(fd);
		return NULL;
	}

	group_impl->fd = fd;
	TAILQ_INIT(&group_impl->socks_with_data);
	group_impl->placement_id = -1;

	if (enable_placement_id == PLACEMENT_CPU) {
		spdk_sock_map_insert(&g_map, spdk_env_get_current_core(), &group_impl->base);
		group_impl->placement_id = spdk_env_get_current_core();
	}

	return &group_impl->base;
}

static struct spdk_sock_group_impl *
posix_sock_group_impl_create(void)
{
	return _sock_group_impl_create(g_posix_impl_opts.enable_placement_id);
}

static struct spdk_sock_group_impl *
ssl_sock_group_impl_create(void)
{
	return _sock_group_impl_create(g_ssl_impl_opts.enable_placement_id);
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

	if (!sock->ready) {
		/* Defer adding the sock to the group;
		 * the group is cached in the base object by the upper layer. */
		if (sock->connect_ctx) {
			return 0;
		}

		SPDK_ERRLOG("Connection failed.\n");
		errno = ENOTCONN;
		return -1;
	}

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
	} else if (sock->recv_pipe != NULL) {
		rc = spdk_pipe_group_add(group->pipe_group, sock->recv_pipe);
		assert(rc == 0);
	}

	if (_sock->impl_opts.enable_placement_id == PLACEMENT_MARK) {
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

	if (sock->connect_ctx || !sock->ready) {
		spdk_sock_abort_requests(_sock);
		return 0;
	}

	if (sock->pipe_has_data || sock->socket_has_data) {
		TAILQ_REMOVE(&group->socks_with_data, sock, link);
		sock->pipe_has_data = false;
		sock->socket_has_data = false;
	} else if (sock->recv_pipe != NULL) {
		rc = spdk_pipe_group_remove(group->pipe_group, sock->recv_pipe);
		assert(rc == 0);
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
posix_sock_group_impl_register_interrupt(struct spdk_sock_group_impl *_group, uint32_t events,
		spdk_interrupt_fn fn, void *arg, const char *name)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);

	group->intr = spdk_interrupt_register_for_events(group->fd, events, fn, arg, name);

	return group->intr ? 0 : -1;
}

static void
posix_sock_group_impl_unregister_interrupt(struct spdk_sock_group_impl *_group)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);

	spdk_interrupt_unregister(&group->intr);
}

static int
_sock_group_impl_close(struct spdk_sock_group_impl *_group, uint32_t enable_placement_id)
{
	struct spdk_posix_sock_group_impl *group = __posix_group_impl(_group);
	int rc;

	if (enable_placement_id == PLACEMENT_CPU) {
		spdk_sock_map_release(&g_map, spdk_env_get_current_core());
	}

	spdk_pipe_group_destroy(group->pipe_group);
	rc = close(group->fd);
	free(group);
	return rc;
}

static int
posix_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	return _sock_group_impl_close(_group, g_posix_impl_opts.enable_placement_id);
}

static int
ssl_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	return _sock_group_impl_close(_group, g_ssl_impl_opts.enable_placement_id);
}

static int
posix_connect_poller(struct spdk_posix_sock *sock)
{
	struct posix_connect_ctx *ctx = sock->connect_ctx;
	int rc;

	if (sock->ready) {
		return 0;
	} else if (!ctx) {
		return -ENOTCONN;
	}

	if (ctx->opts.connect_timeout && ctx->timeout_tsc < spdk_get_ticks()) {
		rc = -ETIMEDOUT;
		goto err;
	}

	rc = spdk_sock_posix_fd_connect_poll_async(ctx->fd);
	if (rc == -EAGAIN) {
		return -EAGAIN;;
	}

	if (rc < 0) {
		int _rc = rc;

		close(ctx->fd);
		ctx->fd = -1;
		rc = _sock_posix_connect_async(ctx);
		if (rc < 0) {
			rc = _rc;
			goto err;
		}

		return -EAGAIN;
	}

	/* Connection established, proceed to deferred initialization. */
	sock->fd = ctx->fd;

	/* Only enable zero copy for non-loopback and non-ssl sockets. */
	posix_sock_init(sock, sock->base.opts.zcopy && !spdk_net_is_loopback(sock->fd) && !ctx->ssl &&
			sock->base.impl_opts.enable_zerocopy_send_client);

	if (ctx->ssl) {
		rc = posix_sock_configure_ssl(sock, true);
		if (rc < 0) {
			goto err;
		}
	}

	if (ctx->set_recvlowat != -1) {
		rc = posix_sock_set_recvlowat(&sock->base, ctx->set_recvlowat);
		if (rc < 0) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_set_recvlowat() failed %d (errno=%d).\n",
				    rc, errno);
			rc = -errno;
			goto err;
		}
	}

	if (ctx->set_recvbuf != -1) {
		rc = posix_sock_set_recvbuf(&sock->base, ctx->set_recvbuf);
		if (rc < 0) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_set_recvbuf() failed %d (errno=%d).\n",
				    rc, errno);
			rc = -errno;
			goto err;
		}
	}

	if (ctx->set_sendbuf != -1) {
		rc = posix_sock_set_sendbuf(&sock->base, ctx->set_sendbuf);
		if (rc < 0) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_set_sendbuf() failed %d (errno=%d).\n",
				    rc, errno);
			rc = -errno;
			goto err;
		}
	}

	if (sock->base.group_impl) {
		rc = posix_sock_group_impl_add_sock(sock->base.group_impl, &sock->base);
		if (rc) {
			SPDK_ERRLOG("Connection was established but delayed posix_sock_group_impl_add_sock() failed %d (errno=%d).\n",
				    rc, errno);
			rc = -errno;
			goto err;
		}
	}

	goto out;

err:
	/* It is safe to pass NULL to SSL free functions. */
	SSL_free(sock->ssl);
	SSL_CTX_free(sock->ssl_ctx);
	if (ctx->fd != -1) {
		close(ctx->fd);
	}

	sock->fd = -1;
	sock->ready = false;

out:
	sock_posix_connect_ctx_cleanup(&sock->connect_ctx, rc);
	return rc;
}

static struct spdk_net_impl g_posix_net_impl = {
	.name		= "posix",
	.getaddr	= posix_sock_getaddr,
	.get_interface_name = posix_sock_get_interface_name,
	.get_numa_id	= posix_sock_get_numa_id,
	.connect	= posix_sock_connect,
	.connect_async	= posix_sock_connect_async,
	.listen		= posix_sock_listen,
	.accept		= posix_sock_accept,
	.close		= posix_sock_close,
	.recv		= posix_sock_recv,
	.readv		= posix_sock_readv,
	.writev		= posix_sock_writev,
	.recv_next	= posix_sock_recv_next,
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
	.group_impl_register_interrupt     = posix_sock_group_impl_register_interrupt,
	.group_impl_unregister_interrupt  = posix_sock_group_impl_unregister_interrupt,
	.group_impl_close	= posix_sock_group_impl_close,
	.get_opts	= posix_sock_impl_get_opts,
	.set_opts	= posix_sock_impl_set_opts,
};

SPDK_NET_IMPL_REGISTER_DEFAULT(posix, &g_posix_net_impl);

static struct spdk_sock *
ssl_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_listen(ip, port, opts, true);
}

static struct spdk_sock *
ssl_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return _posix_sock_connect(ip, port, opts, false, true, NULL, NULL);
}

static struct spdk_sock *
ssl_sock_connect_async(const char *ip, int port, struct spdk_sock_opts *opts,
		       spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	return _posix_sock_connect(ip, port, opts, true, true, cb_fn, cb_arg);
}

static struct spdk_sock *
ssl_sock_accept(struct spdk_sock *_sock)
{
	return _posix_sock_accept(_sock, true);
}

static struct spdk_net_impl g_ssl_net_impl = {
	.name		= "ssl",
	.getaddr	= posix_sock_getaddr,
	.get_interface_name = posix_sock_get_interface_name,
	.get_numa_id	= posix_sock_get_numa_id,
	.connect	= ssl_sock_connect,
	.connect_async	= ssl_sock_connect_async,
	.listen		= ssl_sock_listen,
	.accept		= ssl_sock_accept,
	.close		= posix_sock_close,
	.recv		= posix_sock_recv,
	.readv		= posix_sock_readv,
	.writev		= posix_sock_writev,
	.recv_next	= posix_sock_recv_next,
	.writev_async	= posix_sock_writev_async,
	.flush		= posix_sock_flush,
	.set_recvlowat	= posix_sock_set_recvlowat,
	.set_recvbuf	= posix_sock_set_recvbuf,
	.set_sendbuf	= posix_sock_set_sendbuf,
	.is_ipv6	= posix_sock_is_ipv6,
	.is_ipv4	= posix_sock_is_ipv4,
	.is_connected	= posix_sock_is_connected,
	.group_impl_get_optimal	= posix_sock_group_impl_get_optimal,
	.group_impl_create	= ssl_sock_group_impl_create,
	.group_impl_add_sock	= posix_sock_group_impl_add_sock,
	.group_impl_remove_sock = posix_sock_group_impl_remove_sock,
	.group_impl_poll	= posix_sock_group_impl_poll,
	.group_impl_register_interrupt    = posix_sock_group_impl_register_interrupt,
	.group_impl_unregister_interrupt  = posix_sock_group_impl_unregister_interrupt,
	.group_impl_close	= ssl_sock_group_impl_close,
	.get_opts	= ssl_sock_impl_get_opts,
	.set_opts	= ssl_sock_impl_set_opts,
};

SPDK_NET_IMPL_REGISTER(ssl, &g_ssl_net_impl);
SPDK_LOG_REGISTER_COMPONENT(sock_posix)
