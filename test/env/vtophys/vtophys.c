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

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include "CUnit/Basic.h"

#define __SPDK_ENV_NAME(path)	(strrchr(#path, '/') + 1)
#define _SPDK_ENV_NAME(path)	__SPDK_ENV_NAME(path)
#define SPDK_ENV_NAME		_SPDK_ENV_NAME(SPDK_CONFIG_ENV)

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

		paddr = spdk_vtophys(p, NULL);
		CU_ASSERT(paddr == SPDK_VTOPHYS_ERROR);

		free(p);
		size = size << 1;
	}

	/* Test addresses that are not in the valid x86-64 usermode range */
	paddr = spdk_vtophys((void *)0x0000800000000000ULL, NULL);
	CU_ASSERT(paddr == SPDK_VTOPHYS_ERROR);
}

static void
vtophys_spdk_malloc_test(void)
{
	void *buf = NULL, *p = NULL;
	size_t buf_align = 512;
	int i;
	unsigned int size = 1;
	uint64_t paddr, tmpsize;

	/* Test vtophys on memory allocated through SPDK */
	for (i = 0; i < 31; i++) {
		buf = spdk_zmalloc(size, buf_align, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (buf == NULL) {
			continue;
		}

		/* test vtophys translation with no length parameter */
		paddr = spdk_vtophys(buf, NULL);
		CU_ASSERT(paddr != SPDK_VTOPHYS_ERROR);

		/* translate the entire buffer; it's not necessarily contiguous */
		p = buf;
		tmpsize = size;
		while (p < buf + size) {
			paddr = spdk_vtophys(p, &tmpsize);
			CU_ASSERT(paddr != SPDK_VTOPHYS_ERROR);
			CU_ASSERT(tmpsize >= spdk_min(size, buf_align));
			p += tmpsize;
			tmpsize = buf + size - p;
		}
		CU_ASSERT(tmpsize == 0);

		/* translate a valid vaddr, but with length 0 */
		p = buf;
		tmpsize = 0;
		paddr = spdk_vtophys(p, &tmpsize);
		CU_ASSERT(paddr != SPDK_VTOPHYS_ERROR);
		CU_ASSERT(tmpsize == 0);

		/* translate the first half of the buffer */
		p = buf;
		tmpsize = size / 2;
		while (p < buf + size / 2) {
			paddr = spdk_vtophys(p, &tmpsize);
			CU_ASSERT(paddr != SPDK_VTOPHYS_ERROR);
			CU_ASSERT(tmpsize >= spdk_min(size / 2, buf_align));
			p += tmpsize;
			tmpsize = buf + size / 2 - p;
		}
		CU_ASSERT(tmpsize == 0);

		/* translate the second half of the buffer */
		p = buf + size / 2;
		tmpsize = size / 2;
		while (p < buf + size) {
			paddr = spdk_vtophys(p, &tmpsize);
			CU_ASSERT(paddr != SPDK_VTOPHYS_ERROR);
			CU_ASSERT(tmpsize >= spdk_min(size / 2, buf_align));
			p += tmpsize;
			tmpsize = buf + size - p;
		}
		CU_ASSERT(tmpsize == 0);

		/* translate a region that's not entirely registered */
		p = buf;
		tmpsize = UINT64_MAX;
		while (p < buf + size) {
			paddr = spdk_vtophys(p, &tmpsize);
			CU_ASSERT(paddr != SPDK_VTOPHYS_ERROR);
			CU_ASSERT(tmpsize >= buf_align);
			p += tmpsize;
			/* verify our region is really contiguous */
			CU_ASSERT(paddr + tmpsize - 1 == spdk_vtophys(p - 1, &tmpsize));
			tmpsize = UINT64_MAX;
		}

		spdk_free(buf);
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
	if (strcmp(SPDK_ENV_NAME, "env_dpdk") == 0) {
		opts.env_context = "--log-level=lib.eal:8";
	}

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
