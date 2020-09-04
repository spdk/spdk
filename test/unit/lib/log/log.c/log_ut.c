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

#include "log/log.c"
#include "log/log_flags.c"

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

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("log", NULL, NULL);

	CU_ADD_TEST(suite, log_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
