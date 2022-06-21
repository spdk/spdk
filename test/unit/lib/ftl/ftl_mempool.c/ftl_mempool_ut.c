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
#include "common/lib/test_env.c"

#include "ftl/utils/ftl_mempool.c"

#define COUNT 16
#define ALIGNMENT 64
#define SIZE (ALIGNMENT * 2)
#define SOCKET_ID_ANY -1

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

	CU_ASSERT_NOT_EQUAL(SLIST_EMPTY(&g_mpool->list), 0);

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
