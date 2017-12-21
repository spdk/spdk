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
#include "spdk/cpuset.h"

#include "spdk_cunit.h"

#include "util/cpuset.c"

static int
core_mask_check_range(spdk_cpuset *core_mask, uint32_t min, uint32_t max, int isset)
{
	uint32_t core;
	for (core = min; core <= max; core++) {
		if (isset) {
			if (!spdk_cpuset_get_cpu(core_mask, core)) {
				return -1;
			}
		} else {
			if (spdk_cpuset_get_cpu(core_mask, core)) {
				return -1;
			}
		}
	}
	return 0;
}

static void
test_cpuset_parse(void)
{
	int rc;
	spdk_cpuset *core_mask = spdk_cpuset_alloc();
	char buf[1024];

	/* Only core 0 should be set */
	rc = spdk_cpuset_parse(core_mask, "0x1");
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(core_mask, 0, 0, 1) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 1, SPDK_CPUSET_SIZE - 1, 0) == 0);

	/* Only core 1 should be set */
	rc = spdk_cpuset_parse(core_mask, "[1]");
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(core_mask, 0, 0, 0) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 1, 1, 1) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 2, SPDK_CPUSET_SIZE - 1, 0) == 0);

	/* Set cores 0-10,12,128-254 */
	rc = spdk_cpuset_parse(core_mask, "[0-10,12,128-254]");
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(core_mask, 0, 10, 1) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 11, 11, 0) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 12, 12, 1) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 13, 127, 0) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 128, 254, 1) == 0);
	CU_ASSERT(core_mask_check_range(core_mask, 255, SPDK_CPUSET_SIZE - 1, 0) == 0);

	/* Set all cores */
	snprintf(buf, sizeof(buf), "[0-%d]", SPDK_CPUSET_SIZE - 1);
	rc = spdk_cpuset_parse(core_mask, buf);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(core_mask, 0, SPDK_CPUSET_SIZE - 1, 1) == 0);

	/* Null parameters not allowed */
	rc = spdk_cpuset_parse(core_mask, NULL);
	CU_ASSERT(rc < 0);

	rc = spdk_cpuset_parse(NULL, "[1]");
	CU_ASSERT(rc < 0);

	/* Wrong formated core lists */
	rc = spdk_cpuset_parse(core_mask, "");
	CU_ASSERT(rc < 0);

	rc = spdk_cpuset_parse(core_mask, "[");
	CU_ASSERT(rc < 0);

	rc = spdk_cpuset_parse(core_mask, "[]");
	CU_ASSERT(rc < 0);

	rc = spdk_cpuset_parse(core_mask, "[10--11]");
	CU_ASSERT(rc < 0);

	rc = spdk_cpuset_parse(core_mask, "[11-10]");
	CU_ASSERT(rc < 0);

	rc = spdk_cpuset_parse(core_mask, "[10-11,]");
	CU_ASSERT(rc < 0);

	rc = spdk_cpuset_parse(core_mask, "[,10-11]");
	CU_ASSERT(rc < 0);

	/* Out of range value */
	snprintf(buf, sizeof(buf), "[%d]", SPDK_CPUSET_SIZE + 1);
	rc = spdk_cpuset_parse(core_mask, buf);
	CU_ASSERT(rc < 0);

	/* Overflow value (UINT64_MAX * 10) */
	rc = spdk_cpuset_parse(core_mask, "[184467440737095516150]");
	CU_ASSERT(rc < 0);

	spdk_cpuset_free(core_mask);
}

static void
test_core_mask_hex(void)
{
	int i, rc;
	uint32_t lcore;
	spdk_cpuset *core_mask = spdk_cpuset_alloc();
	char hex_mask[SPDK_CPUSET_STR_MAX_LEN];
	char hex_mask_ref[SPDK_CPUSET_STR_MAX_LEN];

	/* Clear coremask. hex_mask should be "0" */
	spdk_cpuset_zero(core_mask);
	rc = spdk_cpuset_fmt(hex_mask, sizeof(hex_mask), core_mask);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(rc == 1);
	CU_ASSERT(strncmp("0", hex_mask, sizeof(hex_mask)) == 0);

	/* Set coremask 0x51234. Result should be "51234" */
	spdk_cpuset_zero(core_mask);
	spdk_cpuset_set_cpu(core_mask, 2, 1);
	spdk_cpuset_set_cpu(core_mask, 4, 1);
	spdk_cpuset_set_cpu(core_mask, 5, 1);
	spdk_cpuset_set_cpu(core_mask, 9, 1);
	spdk_cpuset_set_cpu(core_mask, 12, 1);
	spdk_cpuset_set_cpu(core_mask, 16, 1);
	spdk_cpuset_set_cpu(core_mask, 18, 1);
	rc = spdk_cpuset_fmt(hex_mask, sizeof(hex_mask), core_mask);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(rc == 5);
	CU_ASSERT(strncmp("51234", hex_mask, sizeof(hex_mask)) == 0);

	/* Set all cores */
	spdk_cpuset_zero(core_mask);
	for (lcore = 0; lcore < SPDK_CPUSET_SIZE; lcore++) {
		spdk_cpuset_set_cpu(core_mask, lcore, 1);
	}
	for (i = 0; i < SPDK_CPUSET_STR_MAX_LEN - 1; i++) {
		hex_mask_ref[i] = 'f';
	}
	hex_mask_ref[SPDK_CPUSET_STR_MAX_LEN - 1] = '\0';

	rc = spdk_cpuset_fmt(hex_mask, sizeof(hex_mask), core_mask);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(rc == SPDK_CPUSET_STR_MAX_LEN - 1);
	CU_ASSERT(strncmp(hex_mask_ref, hex_mask, sizeof(hex_mask)) == 0);

	/* Try to write mask when buffer is too small */
	spdk_cpuset_zero(core_mask);
	for (lcore = 0; lcore < 5; lcore++) {
		spdk_cpuset_set_cpu(core_mask, lcore, 1);
	}
	rc = spdk_cpuset_fmt(hex_mask, 2, core_mask);
	CU_ASSERT(rc < 0);

	spdk_cpuset_free(core_mask);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("cpuset", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_cpuset_parse", test_cpuset_parse) == NULL ||
		CU_add_test(suite, "test_core_mask_hex", test_core_mask_hex) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
