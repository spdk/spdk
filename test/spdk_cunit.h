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

#ifndef SPDK_CUNIT_H
#define SPDK_CUNIT_H

#include "spdk/stdinc.h"

#include <CUnit/Basic.h>
#include <setjmp.h>

extern jmp_buf	g_ut_jmpbuf;
extern int	g_ut_expect_assert_fail;

/*
 * CU_ASSERT_FATAL calls a function that does a longjmp() internally, but only for fatal asserts,
 * so the function itself is not marked as noreturn.  Add an abort() after the assert to help
 * static analyzers figure out that it really doesn't return.
 * The abort() will never actually execute.
 */
#define SPDK_CU_ASSERT_FATAL(cond)		\
	do {					\
		int result_ = !!(cond);		\
		CU_ASSERT_FATAL(result_);	\
		if (!result_) {			\
			abort();		\
		}				\
	} while (0)

#define SPDK_EXPECT_ASSERT_FAIL(func)							\
	do {										\
		if (!setjmp(g_ut_jmpbuf)) {						\
			g_ut_expect_assert_fail = 1;					\
			func;								\
			g_ut_expect_assert_fail = 0;					\
			CU_FAIL_FATAL("Expected assertion failure did not occur");	\
		} else {								\
			g_ut_expect_assert_fail = 0;					\
		}									\
	} while (0)

#define SPDK_MOCK_ASSERT(cond)					\
	do {							\
		if (!(cond)) {					\
			if (g_ut_expect_assert_fail) {		\
				longjmp(g_ut_jmpbuf, 1);	\
			} else {				\
				CU_FAIL_FATAL(cond);		\
			}					\
		}						\
	} while(0)

#define _SPDK_CU_ASSERT_MEMORY_EQUAL(actual, expected, len, fatal) \
	CU_ASSERT##fatal(!memcmp(actual, expected, len))

#define SPDK_CU_ASSERT_MEMORY_EQUAL(actual, expected, len) \
	_SPDK_CU_ASSERT_MEMORY_EQUAL(actual, expected, len, )

#define SPDK_CU_ASSERT_MEMORY_EQUAL_FATAL(actual, expected, len) \
	_SPDK_CU_ASSERT_MEMORY_EQUAL(actual, expected, len, _FATAL)

#endif /* SPDK_CUNIT_H */
