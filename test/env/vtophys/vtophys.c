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

#include "spdk/env.h"

#include "CUnit/Basic.h"

static void
vtophys_malloc_test(void)
{
	void *p = NULL;
	int i;
	unsigned int size = 1;
	uint64_t paddr;

	/* Verify vtophys doesn't work on regular malloc memory */
	for (i = 0; i < 31; i++) {
		p = malloc(size);
		if (p == NULL) {
			continue;
		}

		paddr = spdk_vtophys(p);
		CU_ASSERT(paddr == SPDK_VTOPHYS_ERROR);

		free(p);
		size = size << 1;
	}

	/* Test addresses that are not in the valid x86-64 usermode range */
	paddr = spdk_vtophys((void *)0x0000800000000000ULL);
	CU_ASSERT(paddr == SPDK_VTOPHYS_ERROR)
}

static void
vtophys_spdk_malloc_test(void)
{
	void *p = NULL;
	int i;
	unsigned int size = 1;
	uint64_t paddr;

	/* Test vtophys on memory allocated through SPDK */
	for (i = 0; i < 31; i++) {
		p = spdk_dma_zmalloc(size, 512, NULL);
		if (p == NULL) {
			continue;
		}

		paddr = spdk_vtophys(p);
		CU_ASSERT(paddr != SPDK_VTOPHYS_ERROR);

		spdk_dma_free(p);
		size = size << 1;
	}
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	CU_pSuite suite = NULL;
	unsigned num_failures;

	spdk_env_opts_init(&opts);
	opts.name = "vtophys";
	opts.core_mask = "0x1";
	if (spdk_env_init(&opts) < 0) {
		printf("Err: Unable to initialize SPDK env\n");
		return 1;
	}

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("components_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "vtophys_malloc_test", vtophys_malloc_test) == NULL ||
		CU_add_test(suite, "vtophys_spdk_malloc_test", vtophys_spdk_malloc_test) == NULL
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
