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

int spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, char *caddr, int clen);
struct spdk_sock *spdk_sock_connect(const char *ip, int port);
struct spdk_sock *spdk_sock_listen(const char *ip, int port);
struct spdk_sock *spdk_sock_accept(struct spdk_sock *sock);
int spdk_sock_close(struct spdk_sock **sock);
ssize_t spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len);
ssize_t spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt);

int spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes);
int spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz);
int spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz);

bool spdk_sock_is_ipv6(struct spdk_sock *sock);
bool spdk_sock_is_ipv4(struct spdk_sock *sock);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_SOCK_H */
