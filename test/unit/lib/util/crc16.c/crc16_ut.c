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

#include "util/crc16.c"

static void
test_crc16_t10dif(void)
{
	uint16_t crc;
	char buf[] = "123456789";

	crc = spdk_crc16_t10dif(0, buf, strlen(buf));
	CU_ASSERT(crc == 0xd0db);
}

static void
test_crc16_t10dif_seed(void)
{
	uint16_t crc = 0;
	char buf1[] = "1234";
	char buf2[] = "56789";

	crc = spdk_crc16_t10dif(crc, buf1, strlen(buf1));
	crc = spdk_crc16_t10dif(crc, buf2, strlen(buf2));
	CU_ASSERT(crc == 0xd0db);
}

static void
test_crc16_t10dif_copy(void)
{
	uint16_t crc1 = 0, crc2;
	char buf1[] = "1234";
	char buf2[] = "56789";
	char *buf3 = calloc(1, strlen(buf1) + strlen(buf2) + 1);
	SPDK_CU_ASSERT_FATAL(buf3 != NULL);

	crc1 = spdk_crc16_t10dif_copy(crc1, buf3, buf1, strlen(buf1));
	crc1 = spdk_crc16_t10dif_copy(crc1, buf3 + strlen(buf1), buf2, strlen(buf2));
	CU_ASSERT(crc1 == 0xd0db);

	crc2  = spdk_crc16_t10dif(0, buf3, strlen(buf3));
	CU_ASSERT(crc2 == 0xd0db);

	free(buf3);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("crc16", NULL, NULL);

	CU_ADD_TEST(suite, test_crc16_t10dif);
	CU_ADD_TEST(suite, test_crc16_t10dif_seed);
	CU_ADD_TEST(suite, test_crc16_t10dif_copy);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
