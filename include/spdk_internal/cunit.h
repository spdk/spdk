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

/** Extra option callback */
typedef int (*spdk_ut_option_cb)(int opt, const char *optarg, void *cb_arg);

/** Extra usage callback, called when user asks for --help */
typedef void (*spdk_ut_usage_cb)(void *cb_arg);

/** Init callback, called before tests are executed after parsing arguments */
typedef int (*spdk_ut_init_cb)(void *cb_arg);

struct spdk_ut_opts {
	/** Extra optstring */
	const char *optstring;
	/** Extra options */
	const struct option *opts;
	/** Number of extra options */
	size_t optlen;
	/** Callback argument */
	void *cb_arg;
	/** Extra option callback */
	spdk_ut_option_cb option_cb_fn;
	/** Init callback */
	spdk_ut_init_cb init_cb_fn;
	/** Usage callback */
	spdk_ut_usage_cb usage_cb_fn;
};

/**
 * Execute unit tests registered using CUnit.
 *
 * \param argc Size of the `argv` array.
 * \param argv Arguments to the test app.
 * \param opts Options.
 *
 * \return Number of test failures.
 */
int spdk_ut_run_tests(int argc, char **argv, const struct spdk_ut_opts *opts);

#endif /* SPDK_CUNIT_H */
