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

#ifndef SPDK_INTERNAL_MOCK_H
#define SPDK_INTERNAL_MOCK_H

#include "spdk/stdinc.h"

#define MOCK_STRUCT_INIT(...) \
	{ __VA_ARGS__ }

#define DEFINE_RETURN_MOCK(fn, ret) \
	bool ut_ ## fn ## _mocked = false; \
	ret ut_ ## fn

/*
 * For controlling mocked function behavior, setting
 * and getting values from the stub, the _P macros are
 * for mocking functions that return pointer values.
 */
#define MOCK_SET(fn, val) \
	ut_ ## fn ## _mocked = true; \
	ut_ ## fn = val

#define MOCK_GET(fn) \
	ut_ ## fn

#define MOCK_CLEAR(fn) \
	ut_ ## fn ## _mocked = false

#define MOCK_CLEAR_P(fn) \
	ut_ ## fn ## _mocked = false; \
	ut_ ## fn = NULL

/* for proving to *certain* static analysis tools that we didn't reset the mock function. */
#define MOCK_CLEARED_ASSERT(fn) \
	SPDK_CU_ASSERT_FATAL(ut_ ## fn ## _mocked == false)

/* for declaring function protoypes for wrappers */
#define DECLARE_WRAPPER(fn, ret, args) \
	extern bool ut_ ## fn ## _mocked; \
	extern ret ut_ ## fn; \
	ret __wrap_ ## fn args; ret __real_ ## fn args

/*
 * For defining the implementation of wrappers for syscalls.
 * Avoid nested macro calls to prevent macro expansion of fn.
 */
#define DEFINE_WRAPPER(fn, ret, dargs, pargs) \
	bool ut_ ## fn ## _mocked = false; \
	ret ut_ ## fn; \
	__attribute__((used)) ret __wrap_ ## fn dargs \
	{ \
		if (!ut_ ## fn ## _mocked) { \
			return __real_ ## fn pargs; \
		} else { \
			return ut_ ## fn; \
		} \
	}

/* DEFINE_STUB is for defining the implementation of stubs for SPDK funcs. */
#define DEFINE_STUB(fn, ret, dargs, val) \
	bool ut_ ## fn ## _mocked = true; \
	ret ut_ ## fn = val; \
	ret fn dargs; \
	ret fn dargs \
	{ \
		return MOCK_GET(fn); \
	}

/* DEFINE_STUB_V macro is for stubs that don't have a return value */
#define DEFINE_STUB_V(fn, dargs) \
	void fn dargs; \
	void fn dargs \
	{ \
	}

#define HANDLE_RETURN_MOCK(fn) \
	if (ut_ ## fn ## _mocked) { \
		return ut_ ## fn; \
	}


/* declare wrapper protos (alphabetically please) here */
DECLARE_WRAPPER(calloc, void *, (size_t nmemb, size_t size));

DECLARE_WRAPPER(pthread_mutex_init, int,
		(pthread_mutex_t *mtx, const pthread_mutexattr_t *attr));

DECLARE_WRAPPER(pthread_mutexattr_init, int,
		(pthread_mutexattr_t *attr));

DECLARE_WRAPPER(recvmsg, ssize_t, (int sockfd, struct msghdr *msg, int flags));

DECLARE_WRAPPER(sendmsg, ssize_t, (int sockfd, const struct msghdr *msg, int flags));

DECLARE_WRAPPER(writev, ssize_t, (int fd, const struct iovec *iov, int iovcnt));

/* unlink is done a bit differently. */
extern char *g_unlink_path;
extern void (*g_unlink_callback)(void);
/* If g_unlink_path is NULL, __wrap_unlink will return ENOENT.
 * If the __wrap_unlink() parameter does not match g_unlink_path, it will return ENOENT.
 * If g_unlink_path does match, and g_unlink_callback has been set, g_unlink_callback will
 * be called before returning 0.
 */
int __wrap_unlink(const char *path);

#endif /* SPDK_INTERNAL_MOCK_H */
