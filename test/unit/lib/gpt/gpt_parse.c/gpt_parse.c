/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2016 FUJITSU LIMITED, All rights reserved.
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
 *     * Neither the name of the copyright holder nor the names of its
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

#include "spdk_cunit.h"
#include "lib/test_env.c"
#undef SPDK_CONFIG_VTUNE

#include "gpt/gpt.c"


static int
null_init(void)
{
	return 0;
}

static int
null_clean(void)
{
	return 0;
}


static void
spdk_gpt_parse_test(void)
{
	struct spdk_gpt *gpt;
	int rc;

	gpt = calloc(1, sizeof(*gpt));
	gpt->buf = "abc";

	if (!gpt || !gpt->buf) {
		SPDK_ERRLOG("Gpt and the related buffer should not be NULL\n");
	}

	rc = spdk_gpt_check_mbr(gpt);
	if (rc) {
		SPDK_TRACELOG(SPDK_TRACE_GPT_PARSE, "Failed to detect gpt in MBR\n");
	}

	rc = spdk_gpt_read_header(gpt);
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt header\n");
	}

	rc = spdk_gpt_read_partitions(gpt);
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt partitions\n");
	}

	free(gpt);

}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("gpt_parse", null_init, null_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "gpt_parse", spdk_gpt_parse_test) == NULL
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
