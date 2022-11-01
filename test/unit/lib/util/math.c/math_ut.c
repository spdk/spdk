/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
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
