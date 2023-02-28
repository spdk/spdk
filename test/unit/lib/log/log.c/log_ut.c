/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/string.h"

#include "log/log.c"
#include "log/log_flags.c"
#include "log/log_deprecated.c"

static void
log_test(void)
{
	spdk_log_set_level(SPDK_LOG_ERROR);
	CU_ASSERT_EQUAL(spdk_log_get_level(), SPDK_LOG_ERROR);
	spdk_log_set_level(SPDK_LOG_WARN);
	CU_ASSERT_EQUAL(spdk_log_get_level(), SPDK_LOG_WARN);
	spdk_log_set_level(SPDK_LOG_NOTICE);
	CU_ASSERT_EQUAL(spdk_log_get_level(), SPDK_LOG_NOTICE);
	spdk_log_set_level(SPDK_LOG_INFO);
	CU_ASSERT_EQUAL(spdk_log_get_level(), SPDK_LOG_INFO);
	spdk_log_set_level(SPDK_LOG_DEBUG);
	CU_ASSERT_EQUAL(spdk_log_get_level(), SPDK_LOG_DEBUG);

	spdk_log_set_print_level(SPDK_LOG_ERROR);
	CU_ASSERT_EQUAL(spdk_log_get_print_level(), SPDK_LOG_ERROR);
	spdk_log_set_print_level(SPDK_LOG_WARN);
	CU_ASSERT_EQUAL(spdk_log_get_print_level(), SPDK_LOG_WARN);
	spdk_log_set_print_level(SPDK_LOG_NOTICE);
	CU_ASSERT_EQUAL(spdk_log_get_print_level(), SPDK_LOG_NOTICE);
	spdk_log_set_print_level(SPDK_LOG_INFO);
	CU_ASSERT_EQUAL(spdk_log_get_print_level(), SPDK_LOG_INFO);
	spdk_log_set_print_level(SPDK_LOG_DEBUG);
	CU_ASSERT_EQUAL(spdk_log_get_print_level(), SPDK_LOG_DEBUG);

#ifdef DEBUG
	CU_ASSERT(spdk_log_get_flag("LOG") == false);

	spdk_log_set_flag("log");
	CU_ASSERT(spdk_log_get_flag("LOG") == true);

	spdk_log_clear_flag("LOG");
	CU_ASSERT(spdk_log_get_flag("LOG") == false);
#endif

	spdk_log_open(NULL);
	spdk_log_set_flag("log");
	SPDK_WARNLOG("log warning unit test\n");
	SPDK_DEBUGLOG(log, "log test\n");
	SPDK_LOGDUMP(log, "log dump test:", "log dump", 8);
	spdk_log_dump(stderr, "spdk dump test:", "spdk dump", 9);
	/* Test spdk_log_dump with more than 16 chars and less than 32 chars */
	spdk_log_dump(stderr, "spdk dump test:", "spdk dump 16 more chars", 23);

	spdk_log_close();
}

SPDK_LOG_DEPRECATION_REGISTER(unit_test_not_limited, "not rate limited", "never", 0)
SPDK_LOG_DEPRECATION_REGISTER(unit_test_limited, "with rate limit", "sometime", 1)
SPDK_LOG_DEPRECATION_REGISTER(unit_test_never_called, "not called", "maybe", 0)

int g_ut_dep_expect_line;
const char *g_ut_dep_expect_func;
const char *g_ut_dep_expect_msg;
uint32_t g_ut_dep_log_times;
bool g_ut_dep_saw_suppressed_log;

static void
log_deprecations(int level, const char *file, const int line, const char *func,
		 const char *format, va_list args)
{
	char *msg;

	g_ut_dep_log_times++;

	CU_ASSERT(level == SPDK_LOG_WARN);

	if (strcmp("spdk_log_deprecated", func) == 0) {
		g_ut_dep_saw_suppressed_log = true;
	} else {
		CU_ASSERT(strcmp(g_ut_dep_expect_func, func) == 0);
		CU_ASSERT(g_ut_dep_expect_line == line);
	}

	/* A "starts with" check */
	msg = spdk_vsprintf_alloc(format, args);
	SPDK_CU_ASSERT_FATAL(msg != NULL);
	CU_ASSERT(strncmp(g_ut_dep_expect_msg, msg, strlen(g_ut_dep_expect_msg)) == 0)

	free(msg);
}

bool g_found_not_limited;
bool g_found_limited;
bool g_found_never_called;

static int
iter_dep_cb(void *ctx, struct spdk_deprecation *dep)
{
	/* The getters work from the callback. */
	if (dep == _deprecated_unit_test_not_limited) {
		CU_ASSERT(!g_found_not_limited);
		g_found_not_limited = true;
		CU_ASSERT(strcmp(spdk_deprecation_get_tag(dep), "unit_test_not_limited") == 0);
		CU_ASSERT(strcmp(spdk_deprecation_get_description(dep), "not rate limited") == 0);
		CU_ASSERT(strcmp(spdk_deprecation_get_remove_release(dep), "never") == 0);
		CU_ASSERT(spdk_deprecation_get_hits(dep) != 0);
	} else if (dep == _deprecated_unit_test_limited) {
		CU_ASSERT(!g_found_limited);
		g_found_limited = true;
		CU_ASSERT(strcmp(spdk_deprecation_get_tag(dep), "unit_test_limited") == 0);
		CU_ASSERT(strcmp(spdk_deprecation_get_description(dep), "with rate limit") == 0);
		CU_ASSERT(strcmp(spdk_deprecation_get_remove_release(dep), "sometime") == 0);
		CU_ASSERT(spdk_deprecation_get_hits(dep) != 0);
	} else if (dep == _deprecated_unit_test_never_called) {
		CU_ASSERT(!g_found_never_called);
		g_found_never_called = true;
		CU_ASSERT(strcmp(spdk_deprecation_get_tag(dep), "unit_test_never_called") == 0);
		CU_ASSERT(strcmp(spdk_deprecation_get_description(dep), "not called") == 0);
		CU_ASSERT(strcmp(spdk_deprecation_get_remove_release(dep), "maybe") == 0);
		CU_ASSERT(spdk_deprecation_get_hits(dep) == 0);
	} else {
		CU_ASSERT(false);
	}

	return 0;
}

static void
deprecation(void)
{
	int rc;

	spdk_log_open(log_deprecations);

	/* A log message is emitted for every message without rate limiting. */
	g_ut_dep_saw_suppressed_log = false;
	g_ut_dep_log_times = 0;
	g_ut_dep_expect_func = __func__;
	g_ut_dep_expect_msg = "unit_test_not_limited:";
	g_ut_dep_expect_line = __LINE__ + 1;
	SPDK_LOG_DEPRECATED(unit_test_not_limited);
	CU_ASSERT(_deprecated_unit_test_not_limited->hits == 1);
	CU_ASSERT(_deprecated_unit_test_not_limited->deferred == 0);
	CU_ASSERT(g_ut_dep_log_times == 1);
	g_ut_dep_expect_line = __LINE__ + 1;
	SPDK_LOG_DEPRECATED(unit_test_not_limited);
	CU_ASSERT(_deprecated_unit_test_not_limited->hits == 2);
	CU_ASSERT(_deprecated_unit_test_not_limited->deferred == 0);
	CU_ASSERT(g_ut_dep_log_times == 2);
	CU_ASSERT(!g_ut_dep_saw_suppressed_log);

	/* Rate limiting keeps track of deferred messages */
	g_ut_dep_saw_suppressed_log = false;
	g_ut_dep_log_times = 0;
	g_ut_dep_expect_msg = "unit_test_limited:";
	g_ut_dep_expect_line = __LINE__ + 1;
	SPDK_LOG_DEPRECATED(unit_test_limited);
	CU_ASSERT(_deprecated_unit_test_limited->hits == 1);
	CU_ASSERT(_deprecated_unit_test_limited->deferred == 0);
	CU_ASSERT(g_ut_dep_log_times == 1);
	SPDK_LOG_DEPRECATED(unit_test_limited);
	CU_ASSERT(_deprecated_unit_test_limited->hits == 2);
	CU_ASSERT(_deprecated_unit_test_limited->deferred == 1);
	CU_ASSERT(g_ut_dep_log_times == 1);
	CU_ASSERT(!g_ut_dep_saw_suppressed_log);

	/* After a delay, the next log message prints the normal message followed by one that says
	 * that some messages were suppressed.
	 */
	g_ut_dep_saw_suppressed_log = false;
	sleep(1);
	g_ut_dep_expect_line = __LINE__ + 1;
	SPDK_LOG_DEPRECATED(unit_test_limited);
	CU_ASSERT(_deprecated_unit_test_limited->hits == 3);
	CU_ASSERT(_deprecated_unit_test_limited->deferred == 0);
	CU_ASSERT(g_ut_dep_log_times == 3);
	CU_ASSERT(g_ut_dep_saw_suppressed_log);

	/* spdk_log_for_each_deprecation() visits each registered deprecation */
	rc = spdk_log_for_each_deprecation(NULL, iter_dep_cb);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_found_not_limited);
	CU_ASSERT(g_found_limited);
	CU_ASSERT(g_found_never_called);

	g_log = NULL;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("log", NULL, NULL);

	CU_ADD_TEST(suite, log_test);
	CU_ADD_TEST(suite, deprecation);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
