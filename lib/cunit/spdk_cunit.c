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

static int
spdk_cunit_get_test_result(CU_pTest test)
{
	CU_pFailureRecord failure = CU_get_failure_list();

	while (failure != NULL) {
		if (failure->pTest == test) {
			return 1;
		}
		failure = failure->pNext;
	}

	return 0;
}

static void
spdk_cunit_print_test_result(FILE *out, CU_pTest test)
{
	fprintf(out, "    {\n");
	fprintf(out, "      \"Name\" : \"%s\",\n", test->pName);
	fprintf(out, "      \"Result\" : \"%s\"\n",
		spdk_cunit_get_test_result(test) ? "FAIL" : "PASS");
	fprintf(out, "    }\n");
}

static void
spdk_cunit_print_suite_result(FILE *out, CU_pSuite suite)
{
	CU_pTest test = suite->pTest;

	while (test != NULL) {
		spdk_cunit_print_test_result(out, test);
		test = test->pNext;
		if (test != NULL) {
			fprintf(out, "    ,\n");
		}
	}
}

static void
spdk_cunit_print_registry_result(FILE *out, CU_pTestRegistry registry)
{
	CU_pSuite suite = registry->pSuite;

	if (suite == NULL) {
		return;
	}

	fprintf(out, "{\n");
	fprintf(out, "  \"%s unit tests\": [\n", suite->pName);

	while (suite != NULL) {
		spdk_cunit_print_suite_result(out, suite);
		suite = suite->pNext;
	}

	fprintf(out, "  ]\n");
	fprintf(out, "}\n");
}

int
spdk_cunit_print_results(const char *filename)
{
	FILE *out;

	out = fopen(filename, "w");
	if (out == NULL) {
		fprintf(stderr, "could not open results file %s\n", filename);
		return -1;
	}

	spdk_cunit_print_registry_result(out, CU_get_registry());
	fclose(out);
	return 0;
}
