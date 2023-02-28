/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "util/crc32.c"
#include "util/crc32c.c"

static void
test_crc32c(void)
{
	uint32_t crc;
	char buf[1024], buf1[1024];
	struct iovec iov[2] = {};

	/* Verify a string's CRC32-C value against the known correct result. */
	snprintf(buf, sizeof(buf), "%s", "Hello world!");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7b98e751);

	crc = 0xFFFFFFFFu;
	iov[0].iov_base = buf;
	iov[0].iov_len = strlen(buf);
	crc = spdk_crc32c_iov_update(iov, 1, crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7b98e751);

	crc = 0xFFFFFFFFu;
	snprintf(buf, sizeof(buf), "%s", "Hello");
	iov[0].iov_base = buf;
	iov[0].iov_len = strlen(buf);

	snprintf(buf1, sizeof(buf1), "%s", " world!");
	iov[1].iov_base = buf1;
	iov[1].iov_len = strlen(buf1);

	crc = spdk_crc32c_iov_update(iov, 2, crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7b98e751);

	/*
	 * The main loop of the optimized CRC32-C implementation processes data in 8-byte blocks,
	 * followed by a loop to handle the 0-7 trailing bytes.
	 * Test all buffer sizes from 0 to 7 in order to hit all possible trailing byte counts.
	 */

	/* 0-byte buffer should not modify CRC at all, so final result should be ~0 ^ ~0 == 0 */
	snprintf(buf, sizeof(buf), "%s", "");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0);

	/* 1-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "1");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x90F599E3);

	/* 2-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "12");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x7355C460);

	/* 3-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "123");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x107B2FB2);

	/* 4-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "1234");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0xF63AF4EE);

	/* 5-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "12345");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x18D12335);

	/* 6-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "123456");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x41357186);

	/* 7-byte buffer */
	snprintf(buf, sizeof(buf), "%s", "1234567");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x124297EA);

	/* Test a buffer of exactly 8 bytes (one block in the main CRC32-C loop). */
	snprintf(buf, sizeof(buf), "%s", "12345678");
	crc = 0xFFFFFFFFu;
	crc = spdk_crc32c_update(buf, strlen(buf), crc);
	crc ^= 0xFFFFFFFFu;
	CU_ASSERT(crc == 0x6087809A);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("crc32c", NULL, NULL);

	CU_ADD_TEST(suite, test_crc32c);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
