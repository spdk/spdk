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

/* used to signify pass through */
#define MOCK_PASS_THRU (0xdeadbeef)
#define MOCK_PASS_THRU_P (void*)0xdeadbeef
/* helper for initializing struct value with mock macros */
#define MOCK_STRUCT_INIT(...) \
	{ __VA_ARGS__ }

/*
 * For controlling mocked function behavior, setting
 * and getting values from the stub, the _P macros are
 * for mocking functions that return pointer values.
 */
#define MOCK_SET(fn, ret, val) \
	ut_ ## fn = (ret)val

#define MOCK_SET_P(fn, ret, val) \
	ut_p_ ## fn = (ret)val

#define MOCK_GET(fn) \
	ut_ ## fn

#define MOCK_GET_P(fn) \
	ut_p_ ## fn

/* for declaring function protoypes for wrappers */
#define DECLARE_WRAPPER(fn, ret, args) \
	extern ret ut_ ## fn; \
	ret __wrap_ ## fn args; ret __real_ ## fn args;

/* for defining the implmentation of wrappers for syscalls */
#define DEFINE_WRAPPER(fn, ret, dargs, pargs, val) \
	ret ut_ ## fn = val; \
	ret __wrap_ ## fn dargs \
	{ \
		if (ut_ ## fn == (ret)MOCK_PASS_THRU) { \
			return __real_ ## fn pargs; \
		} else { \
			return MOCK_GET(fn); \
		} \
	}

/* DEFINE_STUB is for defining the implmentation of stubs for SPDK funcs. */
#define DEFINE_STUB(fn, ret, dargs, val) \
	ret ut_ ## fn = val; \
	ret fn dargs; \
	ret fn dargs \
	{ \
		return MOCK_GET(fn); \
	}

/* DEFINE_STUB_P macro is for stubs that return pointer values */
#define DEFINE_STUB_P(fn, ret, dargs, val) \
	ret ut_ ## fn = val; \
	ret* ut_p_ ## fn = &(ut_ ## fn); \
	ret* fn dargs; \
	ret* fn dargs \
	{ \
		return MOCK_GET_P(fn); \
	}

/* DEFINE_STUB_V macro is for stubs that don't have a return value */
#define DEFINE_STUB_V(fn, dargs) \
	void fn dargs; \
	void fn dargs \
	{ \
	}

/* DEFINE_STUB_VP macro is for stubs that return void pointer values */
#define DEFINE_STUB_VP(fn, dargs, val) \
	void* ut_p_ ## fn = val; \
	void* fn dargs; \
	void* fn dargs \
	{ \
		return MOCK_GET_P(fn); \
	}

/* declare wrapper protos (alphabetically please) here */
DECLARE_WRAPPER(calloc, void *, (size_t nmemb, size_t size));

DECLARE_WRAPPER(pthread_mutex_init, int,
		(pthread_mutex_t *mtx, const pthread_mutexattr_t *attr));

DECLARE_WRAPPER(pthread_mutexattr_init, int,
		(pthread_mutexattr_t *attr));

#endif /* SPDK_INTERNAL_MOCK_H */
