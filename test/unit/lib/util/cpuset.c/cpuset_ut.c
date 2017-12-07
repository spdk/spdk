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

#include "cpuset.c"

static int
core_mask_check_range(spdk_cpuset_t *core_mask, uint32_t min, uint32_t max, int isset)
{
	uint32_t core;
	for (core = min; core <= max; core++) {
		if (isset) {
			if (!SPDK_CPU_ISSET(core, core_mask)) {
				return -1;
			}
		} else {
			if (SPDK_CPU_ISSET(core, core_mask)) {
				return -1;
			}
		}
	}
	return 0;
}

static void
test_parse_core_mask(void)
{
	int rc;
	spdk_cpuset_t core_mask;
	char buf[1024];

	/* Only core 0 should be set */
	rc = spdk_parse_core_mask("0x1", &core_mask);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 0, 0, 1) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 1, SPDK_CPU_SETSIZE - 1, 0) == 0);

	/* Only core 1 should be set */
	rc = spdk_parse_core_mask("[1]", &core_mask);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 0, 0, 0) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 1, 1, 1) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 2, SPDK_CPU_SETSIZE - 1, 0) == 0);

	/* Set cores 0-10,12,128-254 */
	rc = spdk_parse_core_mask("[0-10,12,128-254]", &core_mask);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 0, 10, 1) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 11, 11, 0) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 12, 12, 1) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 13, 127, 0) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 128, 254, 1) == 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 255, SPDK_CPU_SETSIZE - 1, 0) == 0);

	/* Set all cores */
	snprintf(buf, sizeof(buf), "[0-%d]", SPDK_CPU_SETSIZE - 1);
	rc = spdk_parse_core_mask(buf, &core_mask);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(core_mask_check_range(&core_mask, 0, SPDK_CPU_SETSIZE - 1, 1) == 0);

	/* Empty string is not allowed */
	rc = spdk_parse_core_mask("", &core_mask);
	CU_ASSERT(rc < 0);

	rc = spdk_parse_core_mask("[]", &core_mask);
	CU_ASSERT(rc < 0);

	rc = spdk_parse_core_mask("[10--11]", &core_mask);
	CU_ASSERT(rc < 0);

	rc = spdk_parse_core_mask("[11-10]", &core_mask);
	CU_ASSERT(rc < 0);

	rc = spdk_parse_core_mask("[10-11,]", &core_mask);
	CU_ASSERT(rc < 0);

	rc = spdk_parse_core_mask("[,10-11]", &core_mask);
	CU_ASSERT(rc < 0);

}

static void
test_core_mask_hex(void)
{
	int i;
	uint32_t lcore;
	spdk_cpuset_t core_mask;
	char hex_mask[SPDK_CPUSET_STR_MAX_LEN];
	char hex_mask_ref[SPDK_CPUSET_STR_MAX_LEN];

	/* Clear coremask. hex_mask should be "0" */
	SPDK_CPU_ZERO(&core_mask);
	spdk_core_mask_hex(&core_mask, hex_mask, sizeof(hex_mask));
	CU_ASSERT(strncmp("0", hex_mask, sizeof(hex_mask)) == 0);

	/* Set all cores */
	for (lcore = 0; lcore < SPDK_CPU_SETSIZE; lcore++) {
		SPDK_CPU_SET(lcore, &core_mask);
	}
	for (i = 0; i < SPDK_CPUSET_STR_MAX_LEN - 1; i++) {
		hex_mask_ref[i] = 'f';
	}
	hex_mask_ref[SPDK_CPUSET_STR_MAX_LEN - 1] = '\0';

	spdk_core_mask_hex(&core_mask, hex_mask, sizeof(hex_mask));

	CU_ASSERT(strncmp(hex_mask_ref, hex_mask, sizeof(hex_mask)) == 0);

	/* Try to write mask when buffer is too small */
	SPDK_CPU_ZERO(&core_mask);
	for (lcore = 0; lcore < 5; lcore++) {
		SPDK_CPU_SET(lcore, &core_mask);
	}
	spdk_core_mask_hex(&core_mask, hex_mask, 2);
	CU_ASSERT(strncmp("f", hex_mask, 2) == 0);
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
		CU_add_test(suite, "test_parse_core_mask", test_parse_core_mask) == NULL ||
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
