/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"

#include "util/crc32.c"
#include "util/crc32_ieee.c"

static void
test_crc32_ieee(void)
{
	uint32_t crc;
	char buf[] = "Hello world!";

	crc = 0xFFFFFFFFu;
	crc = spdk_crc32_ieee_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x1b851995);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("crc32_ieee", NULL, NULL);

	CU_ADD_TEST(suite, test_crc32_ieee);


	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
