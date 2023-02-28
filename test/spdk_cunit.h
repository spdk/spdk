/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_CUNIT_H
#define SPDK_CUNIT_H

#include "spdk/stdinc.h"

#include <CUnit/Basic.h>

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

#endif /* SPDK_CUNIT_H */
