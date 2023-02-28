/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/utils/ftl_mempool.c"

#define COUNT 16
#define ALIGNMENT 64
#define SIZE (ALIGNMENT * 2)
#define SOCKET_ID_ANY -1

DEFINE_STUB(ftl_bitmap_create, struct ftl_bitmap *, (void *buf, size_t size), NULL);
DEFINE_STUB_V(ftl_bitmap_destroy, (struct ftl_bitmap *bitmap));
DEFINE_STUB(ftl_bitmap_get, bool, (const struct ftl_bitmap *bitmap, uint64_t bit), true);
DEFINE_STUB_V(ftl_bitmap_set, (struct ftl_bitmap *bitmap, uint64_t bit));
DEFINE_STUB_V(ftl_bitmap_clear, (struct ftl_bitmap *bitmap, uint64_t bit));

static struct ftl_mempool *g_mpool;

static void
test_ftl_mempool_create(void)
{
	struct ftl_mempool *mpool;

	/* improper value of alignment */
	mpool = ftl_mempool_create(COUNT, SIZE, ALIGNMENT + 1, SOCKET_ID_ANY);
	CU_ASSERT_EQUAL(mpool, NULL);
}

static void
test_ftl_mempool_get_put(void)
{
	void *elem[COUNT];
	void *elem_empty;
	void *elem_first = SLIST_FIRST(&g_mpool->list);
	struct ftl_mempool_element *ftl_elem;
	int i;
	for (i = 0; i < COUNT; i++) {
		elem[i] = ftl_mempool_get(g_mpool);
		ftl_elem = elem[i];
		CU_ASSERT_EQUAL(ftl_elem->entry.sle_next, SLIST_FIRST(&g_mpool->list));
	}

	CU_ASSERT(SLIST_EMPTY(&g_mpool->list));

	elem_empty = ftl_mempool_get(g_mpool);
	CU_ASSERT_EQUAL(elem_empty, NULL);

	for (i = COUNT - 1; i >= 0; i--) {
		ftl_mempool_put(g_mpool, elem[i]);
		CU_ASSERT_EQUAL(SLIST_FIRST(&g_mpool->list), elem[i]);
	}

	CU_ASSERT_EQUAL(SLIST_FIRST(&g_mpool->list), elem_first);
}

static int
test_setup(void)
{
	g_mpool = ftl_mempool_create(COUNT, SIZE, ALIGNMENT, SOCKET_ID_ANY);
	if (!g_mpool) {
		return -ENOMEM;
	}

	return 0;
}

static int
test_cleanup(void)
{
	ftl_mempool_destroy(g_mpool);
	g_mpool = NULL;
	return 0;
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_mempool", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_ftl_mempool_create);
	CU_ADD_TEST(suite, test_ftl_mempool_get_put);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
