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

#include "env_dpdk/memory.c"

#define UNIT_TEST_NO_VTOPHYS
#define UNIT_TEST_NO_PCI_ADDR
#include "common/lib/test_env.c"
#include "spdk_cunit.h"

#include "spdk/bit_array.h"

#define PAGE_ARRAY_SIZE (100)
static struct spdk_bit_array *g_page_array;
static void *g_vaddr_to_fail = (void *)UINT64_MAX;

DEFINE_STUB(rte_memseg_contig_walk, int, (rte_memseg_contig_walk_t func, void *arg), 0);
DEFINE_STUB(rte_mem_virt2memseg, struct rte_memseg *,
	    (const void *virt, const struct rte_memseg_list *msl), NULL);
DEFINE_STUB(spdk_env_dpdk_external_init, bool, (void), true);
DEFINE_STUB(rte_mem_event_callback_register, int,
	    (const char *name, rte_mem_event_callback_t clb, void *arg), 0);
DEFINE_STUB(rte_mem_virt2iova, rte_iova_t, (const void *virtaddr), 0);
DEFINE_STUB(rte_eal_iova_mode, enum rte_iova_mode, (void), RTE_IOVA_VA);
DEFINE_STUB(rte_vfio_is_enabled, int, (const char *modname), 0);
DEFINE_STUB(rte_vfio_noiommu_is_enabled, int, (void), 0);
DEFINE_STUB(rte_memseg_get_fd_thread_unsafe, int, (const struct rte_memseg *ms), 0);
DEFINE_STUB(rte_memseg_get_fd_offset_thread_unsafe, int,
	    (const struct rte_memseg *ms, size_t *offset), 0);

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

static int
test_mem_map_notify_fail(void *cb_ctx, struct spdk_mem_map *map,
			 enum spdk_mem_map_notify_action action, void *vaddr, size_t size)
{
	struct spdk_mem_map *reg_map = cb_ctx;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (vaddr == g_vaddr_to_fail) {
			/* Test the error handling. */
			return -1;
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		/* Clear the same region in the other mem_map to be able to
		 * verify that there was no memory left still registered after
		 * the mem_map creation failure.
		 */
		spdk_mem_map_clear_translation(reg_map, (uint64_t)vaddr, size);
		break;
	}

	return 0;
}

static int
test_mem_map_notify_checklen(void *cb_ctx, struct spdk_mem_map *map,
			     enum spdk_mem_map_notify_action action, void *vaddr, size_t size)
{
	size_t *len_arr = cb_ctx;

	/*
	 * This is a test requirement - the len array we use to verify
	 * pages are valid is only so large.
	 */
	SPDK_CU_ASSERT_FATAL((uintptr_t)vaddr < (VALUE_2MB * PAGE_ARRAY_SIZE));

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		assert(size == len_arr[(uintptr_t)vaddr / VALUE_2MB]);
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		CU_ASSERT(size == len_arr[(uintptr_t)vaddr / VALUE_2MB]);
		break;
	}

	return 0;
}

static int
test_check_regions_contiguous(uint64_t addr1, uint64_t addr2)
{
	return addr1 == addr2;
}

const struct spdk_mem_map_ops test_mem_map_ops = {
	.notify_cb = test_mem_map_notify,
	.are_contiguous = test_check_regions_contiguous
};

const struct spdk_mem_map_ops test_mem_map_ops_no_contig = {
	.notify_cb = test_mem_map_notify,
	.are_contiguous = NULL
};

struct spdk_mem_map_ops test_map_ops_notify_fail = {
	.notify_cb = test_mem_map_notify_fail,
	.are_contiguous = NULL
};

struct spdk_mem_map_ops test_map_ops_notify_checklen = {
	.notify_cb = test_mem_map_notify_checklen,
	.are_contiguous = NULL
};

static void
test_mem_map_alloc_free(void)
{
	struct spdk_mem_map *map, *failed_map;
	uint64_t default_translation = 0xDEADBEEF0BADF00D;
	int i;

	map = spdk_mem_map_alloc(default_translation, &test_mem_map_ops, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);

	map = spdk_mem_map_alloc(default_translation, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Register some memory for the initial memory walk in
	 * spdk_mem_map_alloc(). We'll fail registering the last region
	 * and will check if the mem_map cleaned up all its previously
	 * initialized translations.
	 */
	for (i = 0; i < 5; i++) {
		spdk_mem_register((void *)(uintptr_t)(2 * i * VALUE_2MB), VALUE_2MB);
	}

	/* The last region */
	g_vaddr_to_fail = (void *)(8 * VALUE_2MB);
	failed_map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_fail, map);
	CU_ASSERT(failed_map == NULL);

	for (i = 0; i < 4; i++) {
		uint64_t reg, size = VALUE_2MB;

		reg = spdk_mem_map_translate(map, 2 * i * VALUE_2MB, &size);
		/* check if `failed_map` didn't leave any translations behind */
		CU_ASSERT(reg == default_translation);
	}

	for (i = 0; i < 5; i++) {
		spdk_mem_unregister((void *)(uintptr_t)(2 * i * VALUE_2MB), VALUE_2MB);
	}

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);
}

static void
test_mem_map_translation(void)
{
	struct spdk_mem_map *map;
	uint64_t default_translation = 0xDEADBEEF0BADF00D;
	uint64_t addr;
	uint64_t mapping_length;
	int rc;

	map = spdk_mem_map_alloc(default_translation, &test_mem_map_ops, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Try to get translation for address with no translation */
	addr = spdk_mem_map_translate(map, 10, NULL);
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

	/* Make sure we indicate that the three regions are contiguous */
	mapping_length = VALUE_2MB * 3;
	addr = spdk_mem_map_translate(map, 0, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == VALUE_2MB * 3);

	/* Translate an unaligned address */
	mapping_length = VALUE_2MB * 3;
	addr = spdk_mem_map_translate(map, VALUE_4KB, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == VALUE_2MB * 3 - VALUE_4KB);

	/* Clear translation for the middle page of the larger region. */
	rc = spdk_mem_map_clear_translation(map, VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Get translation for first page */
	addr = spdk_mem_map_translate(map, 0, NULL);
	CU_ASSERT(addr == 0);

	/* Make sure we indicate that the three regions are no longer contiguous */
	mapping_length = VALUE_2MB * 3;
	addr = spdk_mem_map_translate(map, 0, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == VALUE_2MB);

	/* Get translation for an unallocated block. Make sure size is 0 */
	mapping_length = VALUE_2MB * 3;
	addr = spdk_mem_map_translate(map, VALUE_2MB, &mapping_length);
	CU_ASSERT(addr == default_translation);
	CU_ASSERT(mapping_length == VALUE_2MB);

	/* Verify translation for 2nd page is the default */
	addr = spdk_mem_map_translate(map, VALUE_2MB, NULL);
	CU_ASSERT(addr == default_translation);

	/* Get translation for third page */
	addr = spdk_mem_map_translate(map, 2 * VALUE_2MB, NULL);
	/*
	 * Note that addr should be 0, not 4MB. When we set the
	 * translation above, we said the whole 6MB region
	 * should translate to 0.
	 */
	CU_ASSERT(addr == 0);

	/* Translate only a subset of a 2MB page */
	mapping_length = 543;
	addr = spdk_mem_map_translate(map, 0, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == 543);

	/* Translate another subset of a 2MB page */
	mapping_length = 543;
	addr = spdk_mem_map_translate(map, VALUE_4KB, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == 543);

	/* Try to translate an unaligned region that is only partially registered */
	mapping_length = 543;
	addr = spdk_mem_map_translate(map, 3 * VALUE_2MB - 196, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == 196);

	/* Clear translation for the first page */
	rc = spdk_mem_map_clear_translation(map, 0, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Get translation for the first page */
	addr = spdk_mem_map_translate(map, 0, NULL);
	CU_ASSERT(addr == default_translation);

	/* Clear translation for the third page */
	rc = spdk_mem_map_clear_translation(map, 2 * VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Get translation for the third page */
	addr = spdk_mem_map_translate(map, 2 * VALUE_2MB, NULL);
	CU_ASSERT(addr == default_translation);

	/* Set translation for the last valid 2MB region */
	rc = spdk_mem_map_set_translation(map, 0xffffffe00000ULL, VALUE_2MB, 0x1234);
	CU_ASSERT(rc == 0);

	/* Verify translation for last valid 2MB region */
	addr = spdk_mem_map_translate(map, 0xffffffe00000ULL, NULL);
	CU_ASSERT(addr == 0x1234);

	/* Attempt to set translation for the first invalid address */
	rc = spdk_mem_map_set_translation(map, 0x1000000000000ULL, VALUE_2MB, 0x5678);
	CU_ASSERT(rc == -EINVAL);

	/* Attempt to set translation starting at a valid address but exceeding the valid range */
	rc = spdk_mem_map_set_translation(map, 0xffffffe00000ULL, VALUE_2MB * 2, 0x123123);
	CU_ASSERT(rc != 0);

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);

	/* Allocate a map without a contiguous region checker */
	map = spdk_mem_map_alloc(default_translation, &test_mem_map_ops_no_contig, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* map three contiguous regions */
	rc = spdk_mem_map_set_translation(map, 0, 3 * VALUE_2MB, 0);
	CU_ASSERT(rc == 0);

	/* Since we can't check their contiguity, make sure we only return the size of one page */
	mapping_length = VALUE_2MB * 3;
	addr = spdk_mem_map_translate(map, 0, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == VALUE_2MB);

	/* Translate only a subset of a 2MB page */
	mapping_length = 543;
	addr = spdk_mem_map_translate(map, 0, &mapping_length);
	CU_ASSERT(addr == 0);
	CU_ASSERT(mapping_length == 543);

	/* Clear the translation */
	rc = spdk_mem_map_clear_translation(map, 0, VALUE_2MB * 3);
	CU_ASSERT(rc == 0);

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);
}

static void
test_mem_map_registration(void)
{
	int rc;
	struct spdk_mem_map *map;
	uint64_t default_translation = 0xDEADBEEF0BADF00D;

	map = spdk_mem_map_alloc(default_translation, &test_mem_map_ops, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Unregister memory region that wasn't previously registered */
	rc =  spdk_mem_unregister((void *)VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == -EINVAL);

	/* Register non-2MB multiple size */
	rc = spdk_mem_register((void *)VALUE_2MB, 1234);
	CU_ASSERT(rc == -EINVAL);

	/* Register region that isn't 2MB aligned */
	rc = spdk_mem_register((void *)1234, VALUE_2MB);
	CU_ASSERT(rc == -EINVAL);

	/* Register one 2MB page */
	rc = spdk_mem_register((void *)VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Register an overlapping address range */
	rc = spdk_mem_register((void *)0, 3 * VALUE_2MB);
	CU_ASSERT(rc == -EBUSY);

	/* Unregister a 2MB page */
	rc = spdk_mem_unregister((void *)VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Register non overlapping address range */
	rc = spdk_mem_register((void *)0, 3 * VALUE_2MB);
	CU_ASSERT(rc == 0);

	/* Unregister the middle page of the larger region. */
	rc = spdk_mem_unregister((void *)VALUE_2MB, VALUE_2MB);
	CU_ASSERT(rc == -ERANGE);

	/* Unregister the first page */
	rc = spdk_mem_unregister((void *)0, VALUE_2MB);
	CU_ASSERT(rc == -ERANGE);

	/* Unregister the third page */
	rc = spdk_mem_unregister((void *)(2 * VALUE_2MB), VALUE_2MB);
	CU_ASSERT(rc == -ERANGE);

	/* Unregister the entire address range */
	rc = spdk_mem_unregister((void *)0, 3 * VALUE_2MB);
	CU_ASSERT(rc == 0);

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);
}

static void
test_mem_map_registration_adjacent(void)
{
	struct spdk_mem_map *map, *newmap;
	uint64_t default_translation = 0xDEADBEEF0BADF00D;
	uintptr_t vaddr;
	unsigned i;
	size_t notify_len[PAGE_ARRAY_SIZE] = {0};
	size_t chunk_len[] = { 2, 1, 3, 2, 1, 1 };

	map = spdk_mem_map_alloc(default_translation,
				 &test_map_ops_notify_checklen, notify_len);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	vaddr = 0;
	for (i = 0; i < SPDK_COUNTOF(chunk_len); i++) {
		notify_len[vaddr / VALUE_2MB] = chunk_len[i] * VALUE_2MB;
		spdk_mem_register((void *)vaddr, notify_len[vaddr / VALUE_2MB]);
		vaddr += notify_len[vaddr / VALUE_2MB];
	}

	/* Verify the memory is translated in the same chunks it was registered */
	newmap = spdk_mem_map_alloc(default_translation,
				    &test_map_ops_notify_checklen, notify_len);
	SPDK_CU_ASSERT_FATAL(newmap != NULL);
	spdk_mem_map_free(&newmap);
	CU_ASSERT(newmap == NULL);

	vaddr = 0;
	for (i = 0; i < SPDK_COUNTOF(chunk_len); i++) {
		notify_len[vaddr / VALUE_2MB] = chunk_len[i] * VALUE_2MB;
		spdk_mem_unregister((void *)vaddr, notify_len[vaddr / VALUE_2MB]);
		vaddr += notify_len[vaddr / VALUE_2MB];
	}

	/* Register all chunks again just to unregister them again, but this
	 * time with only a single unregister() call.
	 */
	vaddr = 0;
	for (i = 0; i < SPDK_COUNTOF(chunk_len); i++) {
		notify_len[vaddr / VALUE_2MB] = chunk_len[i] * VALUE_2MB;
		spdk_mem_register((void *)vaddr, notify_len[vaddr / VALUE_2MB]);
		vaddr += notify_len[vaddr / VALUE_2MB];
	}
	spdk_mem_unregister(0, vaddr);

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
	if (mem_map_init(false) < 0) {
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
		CU_add_test(suite, "mem map registration", test_mem_map_registration) == NULL ||
		CU_add_test(suite, "mem map adjacent registrations", test_mem_map_registration_adjacent) == NULL
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
