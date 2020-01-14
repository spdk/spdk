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

/** \file
 * TCP socket abstraction layer
 */

#ifndef SPDK_SOCK_H
#define SPDK_SOCK_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_sock;
struct spdk_sock_group;

/**
 * Anywhere this struct is used, an iovec array is assumed to
 * immediately follow the last member in memory, without any
 * padding.
 *
 * A simpler implementation would be to place a 0-length array
 * of struct iovec at the end of this request. However, embedding
 * a structure that ends with a variable length array inside of
 * another structure is a GNU C extension and not standard.
 */
struct spdk_sock_request {
	/* When the request is completed, this callback will be called.
	 * err will be 0 on success or a negated errno value on failure. */
	void	(*cb_fn)(void *cb_arg, int err);
	void				*cb_arg;

	/**
	 * These fields are used by the socket layer and should not be modified
	 */
	struct __sock_request_internal {
		TAILQ_ENTRY(spdk_sock_request)	link;
		unsigned int			offset;
	} internal;

	int				iovcnt;
	/* struct iovec			iov[]; */
};

#define SPDK_SOCK_REQUEST_IOV(req, i) ((struct iovec *)(((uint8_t *)req + sizeof(struct spdk_sock_request)) + (sizeof(struct iovec) * i)))

/**
 * Get client and server addresses of the given socket.
 *
 * \param sock Socket to get address.
 * \param saddr A pointer to the buffer to hold the address of server.
 * \param slen Length of the buffer 'saddr'.
 * \param sport A pointer(May be NULL) to the buffer to hold the port info of server.
 * \param caddr A pointer to the buffer to hold the address of client.
 * \param clen Length of the buffer 'caddr'.
 * \param cport A pointer(May be NULL) to the buffer to hold the port info of server.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
		      char *caddr, int clen, uint16_t *cport);

/**
 * Create a socket using the specific sock implementation, connect the socket
 * to the specified address and port (of the server), and then return the socket.
 * This function is used by client.
 *
 * \param ip IP address of the server.
 * \param port Port number of the server.
 * \param impl_name The sock_implementation to use, such as "posix". If impl_name is
 * specified, it will *only* try to listen on that impl. If it is NULL, it will try
 * all the sock implementations in order and uses the first sock implementation which
 * can connect. For example, it will try vpp, posix as an example.
 *
 * \return a pointer to the connected socket on success, or NULL on failure.
 */
struct spdk_sock *spdk_sock_connect(const char *ip, int port, char *impl_name);

/**
 * Create a socket using the specific sock implementation, bind the socket to
 * the specified address and port and listen on the socket, and then return the socket.
 * This function is used by server.
 *
 * \param ip IP address to listen on.
 * \param port Port number.
 * \param impl_name The sock_implementation to use, such as "posix". If impl_name is
 * specified, it will *only* try to listen on that impl. If it is NULL, it will try
 * all the sock implementations in order and uses the first sock implementation which
 * can listen. For example, it will try vpp, posix as an example.
 *
 * \return a pointer to the listened socket on success, or NULL on failure.
 */
struct spdk_sock *spdk_sock_listen(const char *ip, int port, char *impl_name);

/**
 * Accept a new connection from a client on the specified socket and return a
 * socket structure which holds the connection.
 *
 * \param sock Listening socket.
 *
 * \return a pointer to the accepted socket on success, or NULL on failure.
 */
struct spdk_sock *spdk_sock_accept(struct spdk_sock *sock);

/**
 * Close a socket.
 *
 * \param sock Socket to close.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_close(struct spdk_sock **sock);

/**
 * Receive a message from the given socket.
 *
 * \param sock Socket to receive message.
 * \param buf Pointer to a buffer to hold the data.
 * \param len Length of the buffer.
 *
 * \return the length of the received message on success, -1 on failure.
 */
ssize_t spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len);

/**
 * Write message to the given socket from the I/O vector array.
 *
 * \param sock Socket to write to.
 * \param iov I/O vector.
 * \param iovcnt Number of I/O vectors in the array.
 *
 * \return the length of written message on success, -1 on failure.
 */
ssize_t spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

/**
 * Write data to the given socket asynchronously, calling
 * the provided callback when the data has been written.
 *
 * \param sock Socket to write to.
 * \param req The write request to submit.
 */
void spdk_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req);

/**
 * Read message from the given socket to the I/O vector array.
 *
 * \param sock Socket to receive message.
 * \param iov I/O vector.
 * \param iovcnt Number of I/O vectors in the array.
 *
 * \return the length of the received message on success, -1 on failure.
 */
ssize_t spdk_sock_readv(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

/**
 * Set the value used to specify the low water mark (in bytes) for this socket.
 *
 * \param sock Socket to set for.
 * \param nbytes Value for recvlowat.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes);

/**
 * Set receive buffer size for the given socket.
 *
 * \param sock Socket to set buffer size for.
 * \param sz Buffer size in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz);

/**
 * Set priority for the given socket.
 *
 * \param sock Socket to set the priority.
 * \param priority Priority given by the user.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_set_priority(struct spdk_sock *sock, int priority);

/**
 * Set send buffer size for the given socket.
 *
 * \param sock Socket to set buffer size for.
 * \param sz Buffer size in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz);

/**
 * Check whether the address of socket is ipv6.
 *
 * \param sock Socket to check.
 *
 * \return true if the address of socket is ipv6, or false otherwise.
 */
bool spdk_sock_is_ipv6(struct spdk_sock *sock);

/**
 * Check whether the address of socket is ipv4.
 *
 * \param sock Socket to check.
 *
 * \return true if the address of socket is ipv4, or false otherwise.
 */
bool spdk_sock_is_ipv4(struct spdk_sock *sock);

/**
 * Check whether the socket is currently connected.
 *
 * \param sock Socket to check
 *
 * \return true if the socket is connected or false otherwise.
 */
bool spdk_sock_is_connected(struct spdk_sock *sock);

/**
 * Callback function for spdk_sock_group_add_sock().
 *
 * \param arg Argument for the callback function.
 * \param group Socket group.
 * \param sock Socket.
 */
typedef void (*spdk_sock_cb)(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock);

/**
 * Create a new socket group with user provided pointer
 *
 * \param ctx the context provided by user.
 * \return a pointer to the created group on success, or NULL on failure.
 */
struct spdk_sock_group *spdk_sock_group_create(void *ctx);

/**
 * Get the ctx of the sock group
 *
 * \param sock_group Socket group.
 * \return a pointer which is ctx of the sock_group.
 */
void *spdk_sock_group_get_ctx(struct spdk_sock_group *sock_group);


/**
 * Add a socket to the group.
 *
 * \param group Socket group.
 * \param sock Socket to add.
 * \param cb_fn Called when the operation completes.
 * \param cb_arg Argument passed to the callback function.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			     spdk_sock_cb cb_fn, void *cb_arg);

/**
 * Remove a socket from the group.
 *
 * \param group Socket group.
 * \param sock Socket to remove.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock);

/**
 * Poll incoming events for each registered socket.
 *
 * \param group Group to poll.
 *
 * \return the number of events on success, -1 on failure.
 */
int spdk_sock_group_poll(struct spdk_sock_group *group);

/**
 * Poll incoming events up to max_events for each registered socket.
 *
 * \param group Group to poll.
 * \param max_events Number of maximum events to poll for each socket.
 *
 * \return the number of events on success, -1 on failure.
 */
int spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events);

/**
 * Close all registered sockets of the group and then remove the group.
 *
 * \param group Group to close.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_group_close(struct spdk_sock_group **group);

/**
 * Get the optimal sock group for this sock.
 *
 * \param sock The socket
 * \param group Returns the optimal sock group. If there is no optimal sock group, returns NULL.
 *
 * \return 0 on success. Negated errno on failure.
 */
int spdk_sock_get_optimal_sock_group(struct spdk_sock *sock, struct spdk_sock_group **group);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_SOCK_H */
