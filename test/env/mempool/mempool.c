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
#include "spdk_cunit.h"

#define align512 0x200ULL
#define align512_mask 0x1FFULL
#define align4k 0x1000ULL
#define align4k_mask 0xFFFULL

#define MEMPOOL_SIZE 16ULL

static void
aligned_mempool_test(void)
{
	struct spdk_mempool *mp;
	void *items[MEMPOOL_SIZE];
	/* Try two sizes, one a multiple of the */
	size_t item_size[] = {8192, 416};
	size_t i, j;

	for (i = 0; i < 2; i++) {
		mp = spdk_mempool_create_aligned("mempool_512", MEMPOOL_SIZE, item_size[i], align512, 0,
						 SPDK_ENV_SOCKET_ID_ANY);

		for (j = 0; j < MEMPOOL_SIZE; j++) {
			items[j] = spdk_mempool_get(mp);
			CU_ASSERT(items[j] != NULL);
			CU_ASSERT(((uintptr_t)items[j] & ~align512_mask) == (uintptr_t)items[j]);
		}
		spdk_mempool_put_bulk(mp, items, MEMPOOL_SIZE);
		spdk_mempool_free(mp);
	}

	for (i = 0; i < 2; i++) {
		mp = spdk_mempool_create_aligned("mempool_512", MEMPOOL_SIZE, item_size[i], align4k, 0,
						 SPDK_ENV_SOCKET_ID_ANY);

		for (j = 0; j < MEMPOOL_SIZE; j++) {
			items[j] = spdk_mempool_get(mp);
			CU_ASSERT(((uintptr_t)items[j] & ~align4k_mask) == (uintptr_t)items[j]);
		}
		spdk_mempool_put_bulk(mp, items, MEMPOOL_SIZE);
		spdk_mempool_free(mp);
	}
}

static void
unaligned_mempool_test(void)
{
	struct spdk_mempool *mp;
	void *items[MEMPOOL_SIZE];
	/* Try two sizes, one a multiple of the */
	size_t j;

	mp = spdk_mempool_create_aligned("mempool_unaligned", MEMPOOL_SIZE, 8192, 0, 0,
					 SPDK_ENV_SOCKET_ID_ANY);

	for (j = 0; j < MEMPOOL_SIZE; j++) {
		items[j] = spdk_mempool_get(mp);
		CU_ASSERT(items[j] != NULL);
	}
	spdk_mempool_put_bulk(mp, items, MEMPOOL_SIZE);
	spdk_mempool_free(mp);
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	CU_pSuite suite = NULL;
	unsigned num_failures;

	spdk_env_opts_init(&opts);
	opts.name = "mempool_test";
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
		CU_add_test(suite, "mempool_allocate_aligned", aligned_mempool_test) == NULL ||
		CU_add_test(suite, "mempool_allocate_unaligned", unaligned_mempool_test) == NULL
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
