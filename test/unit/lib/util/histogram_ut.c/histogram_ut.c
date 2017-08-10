/*-

 *   BSD LICENSE
 *
 *   Copyright (c) NetApp, Inc.
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
#include "histogram.c"
#include "spdk/util.h"
#include "json_write.c"
#include "lib/test_env.c"

static void
test_histogram_functions(void)
{
	struct spdk_histogram_data *hg1, *hg2, *hg3, *hg4;

	/* Histogram Register */
	hg1 = spdk_histogram_alloc(true, "test_histogram1", "test", "nsec");
	SPDK_CU_ASSERT_FATAL(hg1 != NULL);
	hg2 = spdk_histogram_alloc(false, "test_histogram2", "test", "nsec");
	SPDK_CU_ASSERT_FATAL(hg2 != NULL);
	/* Histogram Find */
	hg3 = spdk_histogram_find(hg1->hist_id);
	CU_ASSERT(hg3 == hg1);
	hg4 = spdk_histogram_find(15);
	CU_ASSERT(hg4 == NULL);

	/* Histogram Enable/Disable */
	CU_ASSERT(spdk_histogram_is_enabled(hg1) == true);
	spdk_histogram_enable(hg2);
	spdk_histogram_disable(hg1);
	CU_ASSERT(spdk_histogram_is_enabled(hg1) == false);
	CU_ASSERT(spdk_histogram_is_enabled(hg2) == true);

	/* spdk_histogram_data_tally For insert value to histogram (CLEAR TEST) */
	spdk_histogram_data_tally(hg2, 800);
	spdk_histogram_data_tally(hg2, 850);
	CU_ASSERT(spdk_histogram_cleared(hg2) == false);
	spdk_histogram_data_reset(hg2);
	CU_ASSERT(spdk_histogram_cleared(hg2) == true);
	spdk_histogram_data_tally(hg1, 800);
	spdk_histogram_data_tally(hg1, 850);
	CU_ASSERT(spdk_histogram_cleared(hg1) == true); //because hg1 is disabled

	/* Histogram Show */
	/* add unit tests for show with json : TODO */

	spdk_histogram_free(hg1);
	spdk_histogram_free(hg2);
}

int
main(int argc, char **argv)
{
	CU_pSuite   suite = NULL;
	unsigned int    num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("histogram", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_histogram_functions", test_histogram_functions) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();


	return num_failures;
}
