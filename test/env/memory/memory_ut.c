/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "env_dpdk/memory.c"

#define UNIT_TEST_NO_VTOPHYS
#define UNIT_TEST_NO_ENV_MEMORY
#include "common/lib/test_env.c"
#include "spdk_internal/cunit.h"

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
DEFINE_STUB(rte_mem_event_callback_unregister, int, (const char *name, void *arg), 0);
DEFINE_STUB(rte_mem_virt2iova, rte_iova_t, (const void *virtaddr), 0);
DEFINE_STUB(rte_eal_iova_mode, enum rte_iova_mode, (void), RTE_IOVA_VA);
DEFINE_STUB(rte_vfio_is_enabled, int, (const char *modname), 0);
DEFINE_STUB(rte_vfio_noiommu_is_enabled, int, (void), 0);
DEFINE_STUB(rte_memseg_get_fd_thread_unsafe, int, (const struct rte_memseg *ms), 0);
DEFINE_STUB(rte_memseg_get_fd_offset_thread_unsafe, int,
	    (const struct rte_memseg *ms, size_t *offset), 0);
DEFINE_STUB(dpdk_pci_device_get_mem_resource, struct rte_mem_resource *,
	    (struct rte_pci_device *dev, uint32_t bar), 0);

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
	uint64_t reg_addr;
	uint64_t reg_size = size;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (vaddr == g_vaddr_to_fail) {
			/* Test the error handling. */
			return -1;
		}

		CU_ASSERT(spdk_mem_map_set_translation(map, (uint64_t)vaddr, (uint64_t)size, (uint64_t)vaddr) == 0);

		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		/* validate the start address */
		reg_addr = spdk_mem_map_translate(map, (uint64_t)vaddr, &reg_size);
		CU_ASSERT(reg_addr == (uint64_t)vaddr);
		spdk_mem_map_clear_translation(map, (uint64_t)vaddr, size);

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
test_mem_map_notify_nop(void *cb_ctx, struct spdk_mem_map *map,
			enum spdk_mem_map_notify_action action, void *vaddr, size_t size)
{
	return 0;
}

struct ut_memreg {
	uint64_t		start;
	size_t			len;
	TAILQ_ENTRY(ut_memreg)	tailq;
};

TAILQ_HEAD(ut_memreg_tailq, ut_memreg);

static int
ut_memreg_count(struct ut_memreg_tailq *regions)
{
	struct ut_memreg *memreg;
	int count = 0;

	TAILQ_FOREACH(memreg, regions, tailq) {
		count++;
	}

	return count;
}

static struct ut_memreg *
ut_memreg_find(struct ut_memreg_tailq *regions, uint64_t vaddr, size_t len)
{
	struct ut_memreg *memreg;

	TAILQ_FOREACH(memreg, regions, tailq) {
		if (memreg->start == vaddr && memreg->len == len) {
			return memreg;
		}
	}

	return NULL;
}

static int
test_mem_map_notify_memreg(void *cb_ctx, struct spdk_mem_map *map,
			   enum spdk_mem_map_notify_action action,
			   void *vaddr, size_t len)
{
	struct ut_memreg_tailq *tailq = cb_ctx;
	struct ut_memreg *memreg;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (vaddr == g_vaddr_to_fail) {
			return -1;
		}
		TAILQ_FOREACH(memreg, tailq, tailq) {
			CU_ASSERT((uint64_t)vaddr + len <= memreg->start ||
				  (uint64_t)vaddr >= memreg->start + memreg->len);
		}

		memreg = calloc(1, sizeof(*memreg));
		SPDK_CU_ASSERT_FATAL(memreg != NULL);

		memreg->start = (uint64_t)vaddr;
		memreg->len = len;
		TAILQ_INSERT_TAIL(tailq, memreg, tailq);
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		TAILQ_FOREACH(memreg, tailq, tailq) {
			if (memreg->start == (uint64_t)vaddr && memreg->len == len) {
				break;
			}
		}
		SPDK_CU_ASSERT_FATAL(memreg != NULL);
		TAILQ_REMOVE(tailq, memreg, tailq);
		free(memreg);
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

struct spdk_mem_map_ops test_map_ops_notify_nop = {
	.notify_cb = test_mem_map_notify_nop,
	.are_contiguous = test_check_regions_contiguous
};

struct spdk_mem_map_ops test_map_ops_notify_nop_no_contig = {
	.notify_cb = test_mem_map_notify_nop,
	.are_contiguous = NULL
};

struct spdk_mem_map_ops test_map_ops_notify_memreg = {
	.notify_cb = test_mem_map_notify_memreg,
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
	g_vaddr_to_fail = (void *)UINT64_MAX;
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

static void
test_mem_map_4kb(void)
{
	struct spdk_mem_map *map;
	const uint64_t default_translation = 0xDEADBEEF0BADF00D;
	uint64_t i, addr, traddr, size;
	int rc;

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_nop_no_contig, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Check single 4KB page translation */
	addr = 0;
	rc = spdk_mem_map_set_translation(map, addr, VALUE_4KB, 0xfeedbeeff00d0);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d0);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Set the next 4KB page */
	addr = VALUE_4KB;
	rc = spdk_mem_map_set_translation(map, addr, VALUE_4KB, 0xfeedbeeff00d1);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, 0, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d0);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d1);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Clear the second page */
	rc = spdk_mem_map_clear_translation(map, addr, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, 0, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d0);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	traddr = spdk_mem_map_translate(map, addr, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check two 4KB pages spanning across 2MB boundary */
	addr = VALUE_2MB - VALUE_4KB;
	rc = spdk_mem_map_set_translation(map, addr, 2 * VALUE_4KB, 0xfeedbeeff00d2);
	CU_ASSERT_EQUAL(rc, 0);

	traddr = spdk_mem_map_translate(map, addr - VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d2);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d2);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check one 4KB page + full 2MB page */
	addr = 3 * VALUE_2MB - VALUE_4KB;
	rc = spdk_mem_map_set_translation(map, addr, VALUE_4KB + VALUE_2MB, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(rc, 0);

	traddr = spdk_mem_map_translate(map, addr - VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(size, VALUE_2MB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB - VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB + VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check the same, but switch the order (i.e. 4KB + 2MB -> 2MB + 4KB) */
	addr = 5 * VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr, VALUE_2MB + VALUE_4KB, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(rc, 0);

	traddr = spdk_mem_map_translate(map, addr - VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(size, VALUE_2MB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(size, VALUE_2MB - VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB - VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB + VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check 2 4KB pages with one full 2MB page in the middle  */
	addr = 7 * VALUE_2MB - VALUE_4KB;
	rc = spdk_mem_map_set_translation(map, addr, 2 * VALUE_4KB + VALUE_2MB, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(rc, 0);

	traddr = spdk_mem_map_translate(map, addr - VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(size, VALUE_2MB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB - VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB + VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB + 2 * VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check multiple pages (2x4KB + 2x2MB + 2x4KB) */
	addr = 9 * VALUE_2MB - 2 * VALUE_4KB;
	rc = spdk_mem_map_set_translation(map, addr, 4 * VALUE_4KB + 2 * VALUE_2MB, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(rc, 0);

	traddr = spdk_mem_map_translate(map, addr - VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, VALUE_2MB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB + 2 * VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, VALUE_2MB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_2MB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_2MB + 2 * VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_2MB + 3 * VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_2MB + 4 * VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Set 4KB translation in the middle of an already translated 2MB page */
	addr = 13 * VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr, VALUE_2MB, 0xfeedbeeff00d7);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr + VALUE_4KB, VALUE_4KB, 0xfeedbeeff00d8);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d7);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d8);
	CU_ASSERT_EQUAL(size, VALUE_4KB);

	for (i = 2 * VALUE_4KB; i < VALUE_2MB; i += VALUE_4KB) {
		size = VALUE_1GB;
		traddr = spdk_mem_map_translate(map, addr + i, &size);
		CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d7);
		CU_ASSERT_EQUAL(size, VALUE_4KB);
	}
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Set 2MB translation on an area with existing 4KB translation */
	addr = 14 * VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr + VALUE_4KB, VALUE_4KB, 0xfeedbeeff00d9);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr, VALUE_2MB, 0xfeedbeeff00da);
	CU_ASSERT_EQUAL(rc, 0);
	for (i = 0; i < VALUE_2MB; i += VALUE_4KB) {
		size = VALUE_1GB;
		traddr = spdk_mem_map_translate(map, addr + i, &size);
		CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00da);
		CU_ASSERT_EQUAL(size, VALUE_2MB - i);
	}

	/* Set 4KB + 2MB translation and then clear the 2MB containing the 4KB */
	addr = 16 * VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr + VALUE_2MB - VALUE_4KB, VALUE_2MB + VALUE_4KB,
					  0xfeedbeeff00da);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB - 2 * VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, default_translation);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB - VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00da);
	CU_ASSERT_EQUAL(size, VALUE_4KB);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00da);
	CU_ASSERT_EQUAL(size, VALUE_2MB);

	rc = spdk_mem_map_clear_translation(map, addr, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, 0);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB - VALUE_4KB, &size);
	CU_ASSERT_EQUAL(traddr, default_translation);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_2MB, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00da);
	CU_ASSERT_EQUAL(size, VALUE_2MB);

	spdk_mem_map_free(&map);
	CU_ASSERT(map == NULL);
}

static void
test_mem_map_4kb_contig_pages(void)
{
	struct spdk_mem_map *map;
	const uint64_t default_translation = 0xDEADBEEF0BADF00D;
	uint64_t i, addr, traddr, size;
	int rc;

	/* The ops treats adjescent pages with the same translation as contiguous */
	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_nop, NULL);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Check two regions: 2x4KB pages + 3x4KB pages immediatelly following it */
	addr = 0;
	rc = spdk_mem_map_set_translation(map, addr, 2 * VALUE_4KB, 0xfeedbeeff00d0);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr + 2 * VALUE_4KB, 3 * VALUE_4KB, 0xfeedbeeff00d1);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d0);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB - 1);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_4KB + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d1);
	CU_ASSERT_EQUAL(size, 3 * VALUE_4KB - 1);
	traddr = spdk_mem_map_translate(map, addr + 5 * VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check 2x4KB contiguous pages created via two set_translation() calls  */
	addr = VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr, VALUE_4KB, 0xfeedbeeff00d2);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr + VALUE_4KB, VALUE_4KB, 0xfeedbeeff00d2);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d2);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB - 1);
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_4KB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check contiguous region consisting of 2x4KB pages + 2x2MB pages */
	addr = 2 * VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr - 2 * VALUE_4KB, 2 * VALUE_4KB, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr, 2 * VALUE_2MB, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr - 2 * VALUE_4KB + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d3);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB + 2 * VALUE_2MB - 1);
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_4KB + 2 * VALUE_2MB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check the same, but switch the order (i.e. 2x4KB + 2x2MB -> 2x2MB + 2x4KB) */
	addr = 4 * VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr, 2 * VALUE_2MB, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr + 2 * VALUE_2MB, 2 * VALUE_4KB, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB + 2 * VALUE_2MB - 1);
	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d4);
	CU_ASSERT_EQUAL(size, VALUE_4KB + 2 * VALUE_2MB - 1);
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_4KB + 2 * VALUE_2MB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check 4KB + 2MB + 4KB */
	addr = 7 * VALUE_2MB;
	rc = spdk_mem_map_set_translation(map, addr - VALUE_4KB, VALUE_4KB, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr, VALUE_2MB, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_map_set_translation(map, addr + VALUE_2MB, VALUE_4KB, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr - VALUE_4KB + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d5);
	CU_ASSERT_EQUAL(size, 2 * VALUE_4KB + VALUE_2MB - 1);
	traddr = spdk_mem_map_translate(map, addr + VALUE_4KB + VALUE_2MB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	/* Check a region consisting of a 2MB page created via 4KB mappings plus a 2MB page */
	addr = 9 * VALUE_2MB;
	for (i = 0; i < VALUE_2MB; i += VALUE_4KB) {
		rc = spdk_mem_map_set_translation(map, addr + i, VALUE_4KB, 0xfeedbeeff00d6);
		CU_ASSERT_EQUAL(rc, 0);
	}
	rc = spdk_mem_map_set_translation(map, addr + VALUE_2MB, VALUE_2MB, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(rc, 0);

	size = VALUE_1GB;
	traddr = spdk_mem_map_translate(map, addr + 1, &size);
	CU_ASSERT_EQUAL(traddr, 0xfeedbeeff00d6);
	CU_ASSERT_EQUAL(size, 2 * VALUE_2MB - 1);
	traddr = spdk_mem_map_translate(map, addr + 2 * VALUE_2MB, NULL);
	CU_ASSERT_EQUAL(traddr, default_translation);

	spdk_mem_map_free(&map);
}

static void
test_mem_4kb_register_notify(void)
{
	struct spdk_mem_map *map;
	uint64_t off, default_translation = 0xDEADBEEF0BADF00D;
	struct ut_memreg_tailq regions = TAILQ_HEAD_INITIALIZER(regions);
	int rc;

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);

	/* Register a single 4KB page */
	rc = spdk_mem_register((void *)0, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, 0, VALUE_4KB));
	rc = spdk_mem_unregister((void *)0, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Register two 4KB pages spanning across 2MB boundary */
	rc = spdk_mem_register((void *)(VALUE_2MB - VALUE_4KB), 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, VALUE_2MB - VALUE_4KB, 2 * VALUE_4KB));
	rc = spdk_mem_unregister((void *)(VALUE_2MB - VALUE_4KB), 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Register a region consisting of one 4KB page and one 2MB page */
	rc = spdk_mem_register((void *)(VALUE_2MB - VALUE_4KB), VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, VALUE_2MB - VALUE_4KB, VALUE_2MB + VALUE_4KB));
	rc = spdk_mem_unregister((void *)(VALUE_2MB - VALUE_4KB), VALUE_2MB +  VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Register a region consisting of: 4KB page, 2MB page, 4KB page */
	rc = spdk_mem_register((void *)(VALUE_2MB - VALUE_4KB), VALUE_2MB + 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, VALUE_2MB - VALUE_4KB, VALUE_2MB + 2 * VALUE_4KB));
	rc = spdk_mem_unregister((void *)(VALUE_2MB - VALUE_4KB), VALUE_2MB +  2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Check that registration fails when it includes a registered 4KB page */
	rc = spdk_mem_register((void *)(VALUE_2MB - VALUE_4KB), VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	/* Try to register a range consisting of two 4KB pages including the already registered one */
	rc = spdk_mem_register((void *)(VALUE_2MB - VALUE_4KB), 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, -EBUSY);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	/* Try to register a 2MB page including the registered 4KB page */
	rc = spdk_mem_register((void *)0, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, -EBUSY);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	/* Try to register a range consisting of a 4KB page and 2MB page including the already
	 * registered 4KB page
	 */
	rc = spdk_mem_register((void *)(VALUE_2MB - VALUE_4KB), VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, -EBUSY);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	rc = spdk_mem_unregister((void *)(VALUE_2MB - VALUE_4KB), VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Try to unregister a region including unregistered pages */
	rc = spdk_mem_register((void *)0, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);

	rc = spdk_mem_unregister((void *)0, 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, -EINVAL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	rc = spdk_mem_unregister((void *)0, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, -EINVAL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	rc = spdk_mem_unregister((void *)0, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, -EINVAL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);

	rc = spdk_mem_unregister((void *)0, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Do the same but change the 4KB page's offset to the end of the 2MB page */
	rc = spdk_mem_register((void *)(VALUE_2MB - VALUE_4KB), VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);

	rc = spdk_mem_unregister((void *)(VALUE_2MB - VALUE_4KB), 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, -EINVAL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	rc = spdk_mem_unregister((void *)0, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, -EINVAL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	rc = spdk_mem_unregister((void *)(VALUE_2MB - VALUE_4KB), VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, -EINVAL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);

	rc = spdk_mem_unregister((void *)(VALUE_2MB - VALUE_4KB), VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Register two 4KB pages indivdually and unregister them both at once */
	rc = spdk_mem_register((void *)0, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_register((void *)VALUE_4KB, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_unregister((void *)0, 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Register a full 2MB page via multiple 4KB registrations and unregister it all at once */
	for (off = 0; off < VALUE_2MB; off += VALUE_4KB) {
		rc = spdk_mem_register((void *)off, VALUE_4KB);
		CU_ASSERT_EQUAL(rc, 0);
	}
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), VALUE_2MB / VALUE_4KB);
	rc = spdk_mem_unregister((void *)0, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	spdk_mem_map_free(&map);
}

static void
test_mem_4kb_register_create(void)
{
	struct spdk_mem_map *map;
	uint64_t offset, default_translation = 0xDEADBEEF0BADF00D;
	struct ut_memreg_tailq regions = TAILQ_HEAD_INITIALIZER(regions);
	int rc;

	/* Register a single 4KB page, create a map, and verify the map is correctly notified  */
	offset = 0 * VALUE_2MB;
	rc = spdk_mem_register((void *)offset, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset, VALUE_4KB));

	rc = spdk_mem_unregister((void *)offset, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	/* Register a page at the end of a 2MB region */
	offset = 1 * VALUE_2MB;
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_register((void *)offset - VALUE_4KB, VALUE_4KB);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset - VALUE_4KB, VALUE_4KB));

	rc = spdk_mem_unregister((void *)offset - VALUE_4KB, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	/* Register two 4KB pages spanning across 2MB boundary */
	offset = 1 * VALUE_2MB;
	rc = spdk_mem_register((void *)offset - VALUE_4KB, 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset - VALUE_4KB, 2 * VALUE_4KB));

	rc = spdk_mem_unregister((void *)offset - VALUE_4KB, 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	/* Register the same region (4KB + 4KB), but register the pages separately */
	offset = 1 * VALUE_2MB;
	rc = spdk_mem_register((void *)offset - VALUE_4KB, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_register((void *)offset, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 2);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset - VALUE_4KB, VALUE_4KB));
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset, VALUE_4KB));

	rc = spdk_mem_unregister((void *)offset - VALUE_4KB, 2 * VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	/* Check a region of 2MB + 4KB */
	offset = 3 * VALUE_2MB;
	rc = spdk_mem_register((void *)offset, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset, VALUE_2MB + VALUE_4KB));

	rc = spdk_mem_unregister((void *)offset, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	/* Check the same region (2MB + 4KB), but register the pages separately */
	offset = 5 * VALUE_2MB;
	rc = spdk_mem_register((void *)offset, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_register((void *)(offset + VALUE_2MB), VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 2);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset, VALUE_2MB));
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset + VALUE_2MB, VALUE_4KB));

	rc = spdk_mem_unregister((void *)offset, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	/* Do the same as above, but change the order of the pages (i.e. 2MB + 4KB -> 4KB + 2MB) */
	offset = 8 * VALUE_2MB;
	rc = spdk_mem_register((void *)offset - VALUE_4KB, VALUE_4KB + VALUE_2MB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 1);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset - VALUE_4KB, VALUE_4KB + VALUE_2MB));

	rc = spdk_mem_unregister((void *)offset - VALUE_4KB, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	offset = 10 * VALUE_2MB;
	rc = spdk_mem_register((void *)offset - VALUE_4KB, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_register((void *)offset, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map != NULL);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 2);
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset - VALUE_4KB, VALUE_4KB));
	CU_ASSERT_PTR_NOT_NULL(ut_memreg_find(&regions, offset, VALUE_2MB));

	rc = spdk_mem_unregister((void *)offset - VALUE_4KB, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);
	spdk_mem_map_free(&map);

	/* Check failure from notify_cb() */
	offset = 11 * VALUE_2MB;
	g_vaddr_to_fail = (void *)offset;

	rc = spdk_mem_register((void *)offset - VALUE_4KB, VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_register((void *)offset, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map == NULL);

	rc = spdk_mem_unregister((void *)offset - VALUE_4KB, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	/* Check the same, but choose a different region (4K + 2MB -> 2MB + 4KB) */
	offset = 13 * VALUE_2MB;
	g_vaddr_to_fail = (void *)(offset + VALUE_2MB);

	rc = spdk_mem_register((void *)offset, VALUE_2MB);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_mem_register((void *)(offset + VALUE_2MB), VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);

	map = spdk_mem_map_alloc(default_translation, &test_map_ops_notify_memreg, &regions);
	SPDK_CU_ASSERT_FATAL(map == NULL);

	rc = spdk_mem_unregister((void *)offset, VALUE_2MB + VALUE_4KB);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(ut_memreg_count(&regions), 0);

	g_vaddr_to_fail = (void *)UINT64_MAX;
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
		CU_add_test(suite, "alloc_free_mem_map", test_mem_map_alloc_free) == NULL ||
		CU_add_test(suite, "mem_map_translation", test_mem_map_translation) == NULL ||
		CU_add_test(suite, "mem_map_registration", test_mem_map_registration) == NULL ||
		CU_add_test(suite, "mem_map_adjacent_registrations", test_mem_map_registration_adjacent) == NULL ||
		CU_add_test(suite, "mem_map_4kb", test_mem_map_4kb) == NULL ||
		CU_add_test(suite, "mem_map_4kb_contig_pages", test_mem_map_4kb_contig_pages) == NULL ||
		CU_add_test(suite, "mem_4kb_register_notify", test_mem_4kb_register_notify) == NULL ||
		CU_add_test(suite, "mem_4kb_register_create", test_mem_4kb_register_create) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	spdk_bit_array_free(&g_page_array);

	return num_failures;
}
