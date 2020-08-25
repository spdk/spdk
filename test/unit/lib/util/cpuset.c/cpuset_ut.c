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
cpuset_check_range(struct spdk_cpuset *core_mask, uint32_t min, uint32_t max, bool isset)
{
	uint32_t core;
	for (core = min; core <= max; core++) {
		if (isset != spdk_cpuset_get_cpu(core_mask, core)) {
			return -1;
		}
	}
	return 0;
}

static void
test_cpuset(void)
{
	uint32_t cpu;
	struct spdk_cpuset *set = spdk_cpuset_alloc();

	SPDK_CU_ASSERT_FATAL(set != NULL);
	CU_ASSERT(spdk_cpuset_count(set) == 0);

	/* Set cpu 0 */
	spdk_cpuset_set_cpu(set, 0, true);
	CU_ASSERT(spdk_cpuset_get_cpu(set, 0) == true);
	CU_ASSERT(cpuset_check_range(set, 1, SPDK_CPUSET_SIZE - 1, false) == 0);
	CU_ASSERT(spdk_cpuset_count(set) == 1);

	/* Set last cpu (cpu 0 already set) */
	spdk_cpuset_set_cpu(set, SPDK_CPUSET_SIZE - 1, true);
	CU_ASSERT(spdk_cpuset_get_cpu(set, 0) == true);
	CU_ASSERT(spdk_cpuset_get_cpu(set, SPDK_CPUSET_SIZE - 1) == true);
	CU_ASSERT(cpuset_check_range(set, 1, SPDK_CPUSET_SIZE - 2, false) == 0);
	CU_ASSERT(spdk_cpuset_count(set) == 2);

	/* Clear cpu 0 (last cpu already set) */
	spdk_cpuset_set_cpu(set, 0, false);
	CU_ASSERT(spdk_cpuset_get_cpu(set, 0) == false);
	CU_ASSERT(cpuset_check_range(set, 1, SPDK_CPUSET_SIZE - 2, false) == 0);
	CU_ASSERT(spdk_cpuset_get_cpu(set, SPDK_CPUSET_SIZE - 1) == true);
	CU_ASSERT(spdk_cpuset_count(set) == 1);

	/* Set middle cpu (last cpu already set) */
	cpu = (SPDK_CPUSET_SIZE - 1) / 2;
	spdk_cpuset_set_cpu(set, cpu, true);
	CU_ASSERT(spdk_cpuset_get_cpu(set, cpu) == true);
	CU_ASSERT(spdk_cpuset_get_cpu(set, SPDK_CPUSET_SIZE - 1) == true);
	CU_ASSERT(cpuset_check_range(set, 1, cpu - 1, false) == 0);
	CU_ASSERT(cpuset_check_range(set, cpu + 1, SPDK_CPUSET_SIZE - 2, false) == 0);
	CU_ASSERT(spdk_cpuset_count(set) == 2);

	/* Set all cpus */
	for (cpu = 0; cpu < SPDK_CPUSET_SIZE; cpu++) {
		spdk_cpuset_set_cpu(set, cpu, true);
	}
	CU_ASSERT(cpuset_check_range(set, 0, SPDK_CPUSET_SIZE - 1, true) == 0);
	CU_ASSERT(spdk_cpuset_count(set) == SPDK_CPUSET_SIZE);

	/* Clear all cpus */
	spdk_cpuset_zero(set);
	CU_ASSERT(cpuset_check_range(set, 0, SPDK_CPUSET_SIZE - 1, false) == 0);
	CU_ASSERT(spdk_cpuset_count(set) == 0);

	spdk_cpuset_free(set);
}

static void
test_cpuset_parse(void)
{
	int rc;
	struct spdk_cpuset *core_mask;
	char buf[1024];

	core_mask = spdk_cpuset_alloc();
	SPDK_CU_ASSERT_FATAL(core_mask != NULL);

	/* Only core 0 should be set */
	rc = spdk_cpuset_parse(core_mask, "0x1");
	CU_ASSERT(rc >= 0);
	CU_ASSERT(cpuset_check_range(core_mask, 0, 0, true) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 1, SPDK_CPUSET_SIZE - 1, false) == 0);

	/* Only core 1 should be set */
	rc = spdk_cpuset_parse(core_mask, "[1]");
	CU_ASSERT(rc >= 0);
	CU_ASSERT(cpuset_check_range(core_mask, 0, 0, false) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 1, 1, true) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 2, SPDK_CPUSET_SIZE - 1, false) == 0);

	/* Set cores 0-10,12,128-254 */
	rc = spdk_cpuset_parse(core_mask, "[0-10,12,128-254]");
	CU_ASSERT(rc >= 0);
	CU_ASSERT(cpuset_check_range(core_mask, 0, 10, true) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 11, 11, false) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 12, 12, true) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 13, 127, false) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 128, 254, true) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 255, SPDK_CPUSET_SIZE - 1, false) == 0);

	/* Set all cores */
	snprintf(buf, sizeof(buf), "[0-%d]", SPDK_CPUSET_SIZE - 1);
	rc = spdk_cpuset_parse(core_mask, buf);
	CU_ASSERT(rc >= 0);
	CU_ASSERT(cpuset_check_range(core_mask, 0, SPDK_CPUSET_SIZE - 1, true) == 0);

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

	/* Test mask with cores 4-7 and 168-171 set. */
	rc = spdk_cpuset_parse(core_mask, "0xF0000000000000000000000000000000000000000F0");
	CU_ASSERT(rc == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 0, 3, false) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 4, 7, true) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 8, 167, false) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 168, 171, true) == 0);
	CU_ASSERT(cpuset_check_range(core_mask, 172, SPDK_CPUSET_SIZE - 1, false) == 0);

	spdk_cpuset_free(core_mask);
}

static void
test_cpuset_fmt(void)
{
	int i;
	uint32_t lcore;
	struct spdk_cpuset *core_mask = spdk_cpuset_alloc();
	const char *hex_mask;
	char hex_mask_ref[SPDK_CPUSET_SIZE / 4 + 1];

	/* Clear coremask. hex_mask should be "0" */
	spdk_cpuset_zero(core_mask);
	hex_mask = spdk_cpuset_fmt(core_mask);
	SPDK_CU_ASSERT_FATAL(hex_mask != NULL);
	CU_ASSERT(strcmp("0", hex_mask) == 0);

	/* Set coremask 0x51234. Result should be "51234" */
	spdk_cpuset_zero(core_mask);
	spdk_cpuset_set_cpu(core_mask, 2, true);
	spdk_cpuset_set_cpu(core_mask, 4, true);
	spdk_cpuset_set_cpu(core_mask, 5, true);
	spdk_cpuset_set_cpu(core_mask, 9, true);
	spdk_cpuset_set_cpu(core_mask, 12, true);
	spdk_cpuset_set_cpu(core_mask, 16, true);
	spdk_cpuset_set_cpu(core_mask, 18, true);
	hex_mask = spdk_cpuset_fmt(core_mask);
	SPDK_CU_ASSERT_FATAL(hex_mask != NULL);
	CU_ASSERT(strcmp("51234", hex_mask) == 0);

	/* Set all cores */
	spdk_cpuset_zero(core_mask);
	CU_ASSERT(cpuset_check_range(core_mask, 0, SPDK_CPUSET_SIZE - 1, false) == 0);

	for (lcore = 0; lcore < SPDK_CPUSET_SIZE; lcore++) {
		spdk_cpuset_set_cpu(core_mask, lcore, true);
	}
	for (i = 0; i < SPDK_CPUSET_SIZE / 4; i++) {
		hex_mask_ref[i] = 'f';
	}
	hex_mask_ref[SPDK_CPUSET_SIZE / 4] = '\0';

	/* Check data before format */
	CU_ASSERT(cpuset_check_range(core_mask, 0, SPDK_CPUSET_SIZE - 1, true) == 0);

	hex_mask = spdk_cpuset_fmt(core_mask);
	SPDK_CU_ASSERT_FATAL(hex_mask != NULL);
	CU_ASSERT(strcmp(hex_mask_ref, hex_mask) == 0);

	/* Check data integrity after format */
	CU_ASSERT(cpuset_check_range(core_mask, 0, SPDK_CPUSET_SIZE - 1, true) == 0);

	spdk_cpuset_free(core_mask);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("cpuset", NULL, NULL);

	CU_ADD_TEST(suite, test_cpuset);
	CU_ADD_TEST(suite, test_cpuset_parse);
	CU_ADD_TEST(suite, test_cpuset_fmt);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
