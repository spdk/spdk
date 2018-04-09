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

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_sock;
struct spdk_sock_group;

/**
 * Get addresses of the given socket.
 *
 * \param sock Socket to get addresses.
 * \param saddr A pointer to the buffer to hold the address of server.
 * \param slen Length of the buffer 'saddr'.
 * \param saddr A pointer to the buffer to hold the address of client.
 * \param slen Length of the buffer 'caddr'.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, char *caddr, int clen);

/**
 * Connect the remote socket server.
 *
 * \param ip IP address of the server.
 * \param port Port number of the server.
 *
 * \return a pointer to the connected socket on success, or NULL on failure.
 */
struct spdk_sock *spdk_sock_connect(const char *ip, int port);

/**
 * Listen on the given IP address and port to wait for the requests of remote client.
 *
 * \param ip IP address to listen on.
 * \param port Port number.
 *
 * \return a pointer to the listened socket on success, or NULL on failure.
 */
struct spdk_sock *spdk_sock_listen(const char *ip, int port);

/**
 * Accept requests from the remote client.
 *
 * \param sock Socket to accept requests from.
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
 * Receive message from the given socket.
 *
 * \param sock Socket to receive message.
 * \param buf Poiinter to a buffer to hold the data.
 * \param len Length of the buffer.
 *
 * \return the length of received message on success, -1 on failure.
 */
ssize_t spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len);

/**
 * Write message to the given socket from the vector of buffers.
 *
 * \param sock Socket to write to.
 * \param iov Vector of buffers.
 * \param iovcnt Number of buffers.
 *
 * \return the length of written message on success, -1 on failure.
 */
ssize_t spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

/**
 * Set the value of recvlowat for the given socket which is used to specify the
 * lowest bytes for receiving message.
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
 * Callback function for spdk_sock_group_add_sock().
 *
 * \param arg Argument for the callback function.
 * \param group Socket group.
 * \param sock Socket.
 */
typedef void (*spdk_sock_cb)(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock);

/**
 * Create a new group for sockets.
 *
 * \return a pointer to the created group on success, or NULL on failure.
 */
struct spdk_sock_group *spdk_sock_group_create(void);

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
 * Poll events of the given group.
 *
 * \param group Group to poll.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_group_poll(struct spdk_sock_group *group);

/**
 * Poll events of the given group with specified maximum events.
 *
 * \param group Group to poll.
 * \param max_events Number of maximum events to poll.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events);

/**
 * Close and clean the gievn group.
 *
 * \param group Group to close.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_sock_group_close(struct spdk_sock_group **group);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_SOCK_H */
