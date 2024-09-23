/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef __SPDK_XLIO_H__
#define __SPDK_XLIO_H__

#include <sys/epoll.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#include <mellanox/xlio_extra.h>
#pragma GCC diagnostic pop
#include <mellanox/xlio.h>

#include "spdk/config.h"

#ifdef __cplusplus
extern "C" {
#endif

int spdk_xlio_init(void);
void spdk_xlio_fini(void);
bool spdk_xlio_is_initialized(void);

#ifdef SPDK_CONFIG_STATIC_XLIO

/* Statically linked libxlio */

#define xlio_getaddrinfo(...) getaddrinfo(__VA_ARGS__)
#define xlio_freeaddrinfo(...) freeaddrinfo(__VA_ARGS__)
#define xlio_gai_strerror(...) gai_strerror(__VA_ARGS__)

#else

/* Dynamically loaded shared libxlio */

struct spdk_sock_xlio_ops {
	int (*xlio_init_ex)(const struct xlio_init_attr *attr);
	int (*xlio_poll_group_create)(const struct xlio_poll_group_attr *attr,
				      xlio_poll_group_t *group_out);
	int (*xlio_poll_group_destroy)(xlio_poll_group_t group);
	int (*xlio_poll_group_update)(xlio_poll_group_t group, const struct xlio_poll_group_attr *attr);
	void (*xlio_poll_group_poll)(xlio_poll_group_t group);
	int (*xlio_socket_create)(const struct xlio_socket_attr *attr, xlio_socket_t *sock_out);
	int (*xlio_socket_destroy)(xlio_socket_t sock);
	int (*xlio_socket_update)(xlio_socket_t sock, unsigned flags, uintptr_t userdata_sq);

	int (*xlio_socket_setsockopt)(xlio_socket_t sock, int level, int optname, const void *optval,
				      socklen_t optlen);
	int (*xlio_socket_getsockname)(xlio_socket_t sock, struct sockaddr *addr, socklen_t *addrlen);
	int (*xlio_socket_getpeername)(xlio_socket_t sock, struct sockaddr *addr, socklen_t *addrlen);

	int (*xlio_socket_bind)(xlio_socket_t sock, const struct sockaddr *addr, socklen_t addrlen);
	int (*xlio_socket_connect)(xlio_socket_t sock, const struct sockaddr *to, socklen_t tolen);
	int (*xlio_socket_listen)(xlio_socket_t sock);
	struct ibv_pd *(*xlio_socket_get_pd)(xlio_socket_t sock);
	int (*xlio_socket_detach_group)(xlio_socket_t sock);
	int (*xlio_socket_attach_group)(xlio_socket_t sock, xlio_poll_group_t group);
	int (*xlio_socket_send)(xlio_socket_t sock, const void *data, size_t len,
				const struct xlio_socket_send_attr *attr);
	int (*xlio_socket_sendv)(xlio_socket_t sock, const struct iovec *iov, unsigned iovcnt,
				 const struct xlio_socket_send_attr *attr);
	void (*xlio_poll_group_flush)(xlio_poll_group_t group);
	void (*xlio_socket_flush)(xlio_socket_t sock);
	void (*xlio_socket_buf_free)(xlio_socket_t sock, struct xlio_buf *buf);
	void (*xlio_poll_group_buf_free)(xlio_poll_group_t group, struct xlio_buf *buf);
};

extern struct xlio_api_t *g_xlio_api;

#define xlio_getaddrinfo(...) getaddrinfo(__VA_ARGS__)
#define xlio_freeaddrinfo(...) freeaddrinfo(__VA_ARGS__)
#define xlio_gai_strerror(...) gai_strerror(__VA_ARGS__)
#define xlio_init_ex(...) g_xlio_api->xlio_init_ex(__VA_ARGS__)
#define xlio_poll_group_create(...) g_xlio_api->xlio_poll_group_create(__VA_ARGS__)
#define xlio_poll_group_destroy(...) g_xlio_api->xlio_poll_group_destroy(__VA_ARGS__)
#define xlio_poll_group_update(...) g_xlio_api->xlio_poll_group_update(__VA_ARGS__)
#define xlio_poll_group_poll(...) g_xlio_api->xlio_poll_group_poll(__VA_ARGS__)
#define xlio_socket_create(...) g_xlio_api->xlio_socket_create(__VA_ARGS__)
#define xlio_socket_destroy(...) g_xlio_api->xlio_socket_destroy(__VA_ARGS__)
#define xlio_socket_update(...) g_xlio_api->xlio_socket_update(__VA_ARGS__)
#define xlio_socket_setsockopt(...) g_xlio_api->xlio_socket_setsockopt(__VA_ARGS__)
#define xlio_socket_getsockname(...) g_xlio_api->xlio_socket_getsockname(__VA_ARGS__)
#define xlio_socket_getpeername(...) g_xlio_api->xlio_socket_getpeername(__VA_ARGS__)
#define xlio_socket_bind(...) g_xlio_api->xlio_socket_bind(__VA_ARGS__)
#define xlio_socket_connect(...) g_xlio_api->xlio_socket_connect(__VA_ARGS__)
#define xlio_socket_listen(...) g_xlio_api->xlio_socket_listen(__VA_ARGS__)
#define xlio_socket_get_pd(...) g_xlio_api->xlio_socket_get_pd(__VA_ARGS__)
#define xlio_socket_detach_group(...) g_xlio_api->xlio_socket_detach_group(__VA_ARGS__)
#define xlio_socket_attach_group(...) g_xlio_api->xlio_socket_attach_group(__VA_ARGS__)
#define xlio_socket_send(...) g_xlio_api->xlio_socket_send(__VA_ARGS__)
#define xlio_socket_sendv(...) g_xlio_api->xlio_socket_sendv(__VA_ARGS__)
#define xlio_poll_group_flush(...) g_xlio_api->xlio_poll_group_flush(__VA_ARGS__)
#define xlio_socket_flush(...) g_xlio_api->xlio_socket_flush(__VA_ARGS__)
#define xlio_socket_buf_free(...) g_xlio_api->xlio_socket_buf_free(__VA_ARGS__)
#define xlio_poll_group_buf_free(...) g_xlio_api->xlio_poll_group_buf_free(__VA_ARGS__)

#endif

#ifdef __cplusplus
}
#endif

#endif /* __SPDK_XLIO_H__ */
