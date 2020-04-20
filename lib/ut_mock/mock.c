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

#include "spdk_internal/mock.h"

DEFINE_WRAPPER(calloc, void *, (size_t nmemb, size_t size), (nmemb, size))

DEFINE_WRAPPER(pthread_mutex_init, int,
	       (pthread_mutex_t *mtx, const pthread_mutexattr_t *attr),
	       (mtx, attr))

DEFINE_WRAPPER(pthread_mutexattr_init, int,
	       (pthread_mutexattr_t *attr), (attr))

DEFINE_WRAPPER(recvmsg, ssize_t, (int sockfd, struct msghdr *msg, int flags), (sockfd, msg, flags))

DEFINE_WRAPPER(sendmsg, ssize_t, (int sockfd, const struct msghdr *msg, int flags), (sockfd, msg,
		flags))

DEFINE_WRAPPER(writev, ssize_t, (int fd, const struct iovec *iov, int iovcnt), (fd, iov, iovcnt))

char *g_unlink_path;
void (*g_unlink_callback)(void);

int
__attribute__((used))
__wrap_unlink(const char *path)
{
	if (g_unlink_path == NULL) {
		return ENOENT;
	}

	if (strcmp(g_unlink_path, path) != 0) {
		return ENOENT;
	}

	if (g_unlink_callback) {
		g_unlink_callback();
	}
	return 0;
}
