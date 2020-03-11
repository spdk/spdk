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

#include "util/math.c"

static void
test_serial_number_arithmetic(void)
{
	CU_ASSERT(spdk_sn32_add(0, 1) == 1);
	CU_ASSERT(spdk_sn32_add(1, 1) == 2);
	CU_ASSERT(spdk_sn32_add(1, 2) == 3);
	CU_ASSERT(spdk_sn32_add(1, UINT32_MAX) == 0);
	CU_ASSERT(spdk_sn32_add(UINT32_MAX, UINT32_MAX) == UINT32_MAX - 1);
	CU_ASSERT(spdk_sn32_gt(1, 0) == true);
	CU_ASSERT(spdk_sn32_gt(2, 1) == true);
	CU_ASSERT(spdk_sn32_gt(UINT32_MAX, UINT32_MAX - 1) == true);
	CU_ASSERT(spdk_sn32_gt(0, UINT32_MAX) == true);
	CU_ASSERT(spdk_sn32_gt(100, UINT32_MAX - 100) == true);
	CU_ASSERT(spdk_sn32_lt(1, 0) == false);
	CU_ASSERT(spdk_sn32_lt(2, 1) == false);
	CU_ASSERT(spdk_sn32_lt(UINT32_MAX, UINT32_MAX - 1) == false);
	CU_ASSERT(spdk_sn32_lt(0, UINT32_MAX) == false);
	CU_ASSERT(spdk_sn32_lt(100, UINT32_MAX - 100) == false);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("math", NULL, NULL);

	CU_ADD_TEST(suite, test_serial_number_arithmetic);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
