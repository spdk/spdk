/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"

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

static void
test_gcd_lcm(void)
{
	uint32_t lcm;

	CU_ASSERT(spdk_gcd32(0, 0) == 0);
	CU_ASSERT(spdk_gcd32(0, 7) == 7);
	CU_ASSERT(spdk_gcd32(7, 0) == 7);
	CU_ASSERT(spdk_gcd32(12, 18) == 6);
	CU_ASSERT(spdk_gcd32(18, 12) == 6);
	CU_ASSERT(spdk_gcd32(17, 5) == 1);
	CU_ASSERT(spdk_gcd32(4096, 512) == 512);
	CU_ASSERT(spdk_gcd32(UINT32_MAX, UINT32_MAX) == UINT32_MAX);

	CU_ASSERT(spdk_gcd64(0, 0) == 0);
	CU_ASSERT(spdk_gcd64(0, 1024 * 1024) == 1024 * 1024);
	CU_ASSERT(spdk_gcd64(1024 * 1024, 0) == 1024 * 1024);
	CU_ASSERT(spdk_gcd64(1024 * 1024, 6 * 4096) == 2 * 4096);
	CU_ASSERT(spdk_gcd64(UINT64_MAX, UINT64_MAX) == UINT64_MAX);

	/* Zero is the identity, so that a set of values can be accumulated
	 * starting from zero.
	 */
	CU_ASSERT(spdk_try_lcm32(0, 0, &lcm) && lcm == 0);
	CU_ASSERT(spdk_try_lcm32(0, 7, &lcm) && lcm == 7);
	CU_ASSERT(spdk_try_lcm32(7, 0, &lcm) && lcm == 7);

	CU_ASSERT(spdk_try_lcm32(4, 6, &lcm) && lcm == 12);
	CU_ASSERT(spdk_try_lcm32(6, 4, &lcm) && lcm == 12);
	CU_ASSERT(spdk_try_lcm32(8, 8, &lcm) && lcm == 8);
	CU_ASSERT(spdk_try_lcm32(2, 3, &lcm) && lcm == 6);
	CU_ASSERT(spdk_try_lcm32(4096, 512, &lcm) && lcm == 4096);
	CU_ASSERT(spdk_try_lcm32(1, UINT32_MAX, &lcm) && lcm == UINT32_MAX);

	/* UINT32_MAX is 3 x 5 x 17 x 257 x 65537, so it is a real result. */
	CU_ASSERT(spdk_try_lcm32(3 * 5 * 17, 257 * 65537, &lcm) && lcm == UINT32_MAX);

	/* A result that does not fit is reported, and the output is left untouched. */
	lcm = 42;
	CU_ASSERT(!spdk_try_lcm32(UINT32_MAX, UINT32_MAX - 1, &lcm));
	CU_ASSERT(!spdk_try_lcm32(1u << 31, (1u << 30) + 1, &lcm));
	CU_ASSERT(lcm == 42);
}

static void
test_memset_s(void)
{
	char secret[] = "0123456789abcdef";

	/* Zero length, nothing should be changed */
	spdk_memset_s(secret, sizeof(secret), 'b', 0);
	CU_ASSERT_EQUAL(memcmp(secret, "0123456789abcdef", sizeof(secret)), 0);

	/* Fill digits */
	spdk_memset_s(secret, sizeof(secret), 'x', 10);
	CU_ASSERT_EQUAL(memcmp(secret, "xxxxxxxxxxabcdef", sizeof(secret)), 0);

	/* Fill the whole string except of the NULL char */
	spdk_memset_s(secret, sizeof(secret), 'y', sizeof(secret) - 1);
	CU_ASSERT_EQUAL(memcmp(secret, "yyyyyyyyyyyyyyyy", sizeof(secret) - 1), 0);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite_math = NULL, suite_erase = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite_math = CU_add_suite("math", NULL, NULL);
	CU_ADD_TEST(suite_math, test_serial_number_arithmetic);
	CU_ADD_TEST(suite_math, test_gcd_lcm);

	suite_erase = CU_add_suite("erase", NULL, NULL);
	CU_ADD_TEST(suite_erase, test_memset_s);


	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
