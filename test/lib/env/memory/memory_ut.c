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

#include "memory.c"

#include "test_env.c"
#include "spdk_cunit.h"

#include "spdk/bit_array.h"

static struct rte_mem_config g_mcfg = {};

static struct rte_config g_cfg = {
	.mem_config = &g_mcfg,
};

struct rte_config *
rte_eal_get_configuration(void)
{
	return &g_cfg;
}

#define PAGE_ARRAY_SIZE (100)
static struct spdk_bit_array *g_page_array;

static int
test_mem_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		    enum spdk_mem_map_notify_action action,
		    void *vaddr, size_t len)
{
	uint32_t i, end;

	SPDK_CU_ASSERT_FATAL(((uintptr_t)vaddr & MASK_2MB) == 0);
	SPDK_CU_ASSERT_FATAL((len & MASK_2MB) == 0);

	/*
	 * This is a test requirement - the bit array we use to verify
	 * pages are valid is only so large.
	 */
	SPDK_CU_ASSERT_FATAL((uintptr_t)vaddr < (VALUE_2MB * PAGE_ARRAY_SIZE));

	i = (uintptr_t)vaddr >> SHIFT_2MB;
	end = i + (len >> SHIFT_2MB);
	for (; i < end; i++) {
		switch (action) {
		case SPDK_MEM_MAP_NOTIFY_REGISTER:
			/* This page should not already be registered */
			SPDK_CU_ASSERT_FATAL(spdk_bit_array_get(g_page_array, i) == false);
			SPDK_CU_ASSERT_FATAL(spdk_bit_array_set(g_page_array, i) == 0);
			break;
		case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
			SPDK_CU_ASSERT_FATAL(spdk_bit_array_get(g_page_array, i) == true);
			spdk_bit_array_clear(g_page_array, i);
			break;
		default:
			SPDK_UNREACHABLE();
		}
	}

	return 0;
}

static void
test_mem_map_alloc_free(void)
{
	struct spdk_mem_map *map;
	uint64_t default_translation = 0xDEADBEEF0BADF00D;

	map = spdk_mem_map_alloc(default_translation, test_mem_map_notify, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);
}

static void
test_mem_map_translation(void)
{
	struct spdk_mem_map *map;
	uint64_t default_translation = 0xDEADBEEF0BADF00D;
	uint64_t addr;
	int rc;

	map = spdk_mem_map_alloc(default_translation, test_mem_map_notify, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Try to get translation for address with no translation */
	addr = spdk_mem_map_translate(map, 10);
	CU_ASSERT(addr == default_translation);

	/* Set translation for region of non-2MB multiple size */
	rc = spdk_mem_map_set_translation(map, VALUE_2MB, 1234, VALUE_2MB);
	CU_ASSERT(rc == -EINVAL);

	/* Set translation for vaddr that isn't 2MB aligned */
	rc = spdk_mem_map_set_translation(map, 1234, VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == -EINVAL);

	/* Set translation for one 2MB page */
	rc = spdk_mem_map_set_translation(map, VALUE_2MB, VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Set translation for region that overlaps the previous translation */
	rc = spdk_mem_map_set_translation(map, 0, 3 * VALUE_2MB, 0);
	CU_ASSERT(rc == 0);

	/* Clear translation for the middle page of the larger region. */
	rc = spdk_mem_map_clear_translation(map, VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Get translation for first page */
	addr = spdk_mem_map_translate(map, 0);
	CU_ASSERT(addr == 0);

	/* Verify translation for 2nd page is the default */
	addr = spdk_mem_map_translate(map, VALUE_2MB);
	CU_ASSERT(addr == default_translation);

	/* Get translation for third page */
	addr = spdk_mem_map_translate(map, 2 * VALUE_2MB);
	/*
	 * Note that addr should be 0, not 4MB. When we set the
	 * translation above, we said the whole 6MB region
	 * should translate to 0.
	 */
	CU_ASSERT(addr == 0);

	/* Clear translation for the first page */
	rc = spdk_mem_map_clear_translation(map, 0, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Get translation for the first page */
	addr = spdk_mem_map_translate(map, 0);
	CU_ASSERT(addr == default_translation);

	/* Clear translation for the third page */
	rc = spdk_mem_map_clear_translation(map, 2 * VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Get translation for the third page */
	addr = spdk_mem_map_translate(map, 2 * VALUE_2MB);
	CU_ASSERT(addr == default_translation);

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);
}

static void
test_mem_map_registration(void)
{
	int rc;
	struct spdk_mem_map *map;
	uint64_t default_translation = 0xDEADBEEF0BADF00D;

	map = spdk_mem_map_alloc(default_translation, test_mem_map_notify, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Unregister memory region that wasn't previously registered */
	rc =  spdk_mem_unregister((void *)VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == -EINVAL);

	/* Register non-2MB multiple size */
	rc = spdk_mem_register((void *)VALUE_2MB, 1234, 0);
	CU_ASSERT(rc == -EINVAL);

	/* Register region that isn't 2MB aligned */
	rc = spdk_mem_register((void *)1234, VALUE_2MB, 0);
	CU_ASSERT(rc == -EINVAL);

	/* Register one 2MB page */
	rc = spdk_mem_register((void *)VALUE_2MB, VALUE_2MB, 0);
	CU_ASSERT(rc == 0);

	/* Register an overlapping address range */
	rc = spdk_mem_register((void *)0, 3 * VALUE_2MB, 0);
	CU_ASSERT(rc == 0);

	/*
	 * Unregister the middle page of the larger region.
	 * It was set twice, so unregister it twice.
	 */
	rc = spdk_mem_unregister((void *)VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);
	rc = spdk_mem_unregister((void *)VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Unregister the first page */
	rc = spdk_mem_unregister((void *)0, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Unregister the third page */
	rc = spdk_mem_unregister((void *)(2 * VALUE_2MB), VALUE_2MB);
	CU_ASSERT(rc == 0);

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	/*
	 * These tests can use PAGE_ARRAY_SIZE 2MB pages of memory.
	 * Note that the tests just verify addresses - this memory
	 * is not actually allocated.
	  */
	g_page_array = spdk_bit_array_create(PAGE_ARRAY_SIZE);

	/* Initialize the memory map */
	if (spdk_mem_map_init() < 0) {
		return CUE_NOMEMORY;
	}

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("memory", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "alloc and free memory map", test_mem_map_alloc_free) == NULL ||
		CU_add_test(suite, "mem map translation", test_mem_map_translation) == NULL ||
		CU_add_test(suite, "mem map registration", test_mem_map_registration) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	spdk_bit_array_free(&g_page_array);

	return num_failures;
}
