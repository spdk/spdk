/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
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
