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
