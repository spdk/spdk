/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "util/crc64.c"


static void
test_crc64_nvme(void)
{
	unsigned int buf_size = 4096;
	char buf[buf_size];
	uint64_t crc;
	unsigned int i, j;

	/* All the expected CRC values are compliant with
	* the NVM Command Set Specification 1.0c */

	/* Input buffer = 0s */
	memset(buf, 0, buf_size);
	crc = spdk_crc64_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0x6482D367EB22B64E);

	/* Input buffer = 1s */
	memset(buf, 0xFF, buf_size);
	crc = spdk_crc64_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0xC0DDBA7302ECA3AC);

	/* Input buffer = 0x00, 0x01, 0x02, ... */
	memset(buf, 0, buf_size);
	j = 0;
	for (i = 0; i < buf_size; i++) {
		buf[i] = (char)j;
		if (j == 0xFF) {
			j = 0;
		} else {
			j++;
		}
	}
	crc = spdk_crc64_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0x3E729F5F6750449C);

	/* Input buffer = 0xFF, 0xFE, 0xFD, ... */
	memset(buf, 0, buf_size);
	j = 0xFF;
	for (i = 0; i < buf_size ; i++) {
		buf[i] = (char)j;
		if (j == 0) {
			j = 0xFF;
		} else {
			j--;
		}
	}
	crc = spdk_crc64_nvme(buf, buf_size, 0);
	CU_ASSERT(crc == 0x9A2DF64B8E9E517E);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("crc64", NULL, NULL);

	CU_ADD_TEST(suite, test_crc64_nvme);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
