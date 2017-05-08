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

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "spdk/log.h"

#include "log.c"

static void
log_test(void)
{
	int rc = 0;
	const char *buf;

	rc = spdk_set_log_facility("test");
	CU_ASSERT(rc == -1);
	CU_ASSERT_EQUAL(spdk_g_log_facility, LOG_DAEMON);
	rc = spdk_set_log_facility("local7");
	CU_ASSERT(rc == 0);
	CU_ASSERT_EQUAL(spdk_g_log_facility, LOG_LOCAL7);

	spdk_g_log_facility = -1;
	buf = spdk_get_log_facility();
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	CU_ASSERT_STRING_EQUAL(buf, "daemon");
	spdk_g_log_facility = LOG_LOCAL7;
	buf = spdk_get_log_facility();
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	CU_ASSERT_STRING_EQUAL(buf, "local7");

	rc = spdk_set_log_priority("test");
	CU_ASSERT(rc == -1);
	CU_ASSERT_EQUAL(spdk_g_log_priority, LOG_NOTICE);
	rc = spdk_set_log_priority("debug");
	CU_ASSERT(rc == 0);
	CU_ASSERT_EQUAL(spdk_g_log_priority, LOG_DEBUG);

#ifdef DEBUG
	CU_ASSERT(spdk_log_get_trace_flag("debug") == false);

	spdk_log_set_trace_flag("debug");
	CU_ASSERT(spdk_log_get_trace_flag("debug") == true);

	spdk_log_clear_trace_flag("debug");
	CU_ASSERT(spdk_log_get_trace_flag("debug") == false);
#endif

	spdk_open_log();
	spdk_log_set_trace_flag("debug");
	SPDK_WARNLOG("log warning unit test\n");
	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "log trace test\n");
	SPDK_TRACEDUMP(SPDK_TRACE_DEBUG, "log trace dump test:", "trace dump", 10);
	spdk_trace_dump("spdk dump test:", "spdk dump", 9);

	spdk_close_log();
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("log", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "log_ut", log_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
