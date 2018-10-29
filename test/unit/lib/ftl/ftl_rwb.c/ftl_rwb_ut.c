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

#include "stdatomic.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#undef assert
#define assert(cond) SPDK_MOCK_ASSERT(cond)

#include "ftl/ftl_rwb.c"

#define RWB_SIZE	(1024 * 1024)
#define RWB_ENTRY_COUNT	(RWB_SIZE / FTL_BLOCK_SIZE)
#define XFER_SIZE	16
#define METADATA_SIZE	64

static struct ftl_rwb *g_rwb;

static void
setup_rwb(void)
{
	struct spdk_ftl_conf conf = { .rwb_size = RWB_SIZE };
	g_rwb = ftl_rwb_init(&conf, XFER_SIZE, METADATA_SIZE);
	CU_ASSERT_PTR_NOT_NULL_FATAL(g_rwb);
}

static void
cleanup_rwb(void)
{
	ftl_rwb_free(g_rwb);
	g_rwb = NULL;
}

static void
test_rwb_acquire(void)
{
	struct ftl_rwb_entry *entry;

	/* Verify that it's possible to acquire all of the entries */
	for (size_t i = 0; i < RWB_ENTRY_COUNT; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NULL(entry);
}

static void
test_rwb_pop(void)
{
	struct ftl_rwb_entry *entry;
	struct ftl_rwb_batch *batch;
	size_t entry_count;

	/* Acquire all entries */
	for (size_t i = 0; i < RWB_ENTRY_COUNT; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		entry->lba = i;
		ftl_rwb_push(entry);
	}

	/* Pop all batches and free them */
	for (size_t i = 0; i < RWB_ENTRY_COUNT / XFER_SIZE; ++i) {
		batch = ftl_rwb_pop(g_rwb);
		SPDK_CU_ASSERT_FATAL(batch);
		entry_count = 0;

		ftl_rwb_foreach(entry, batch) {
			CU_ASSERT_EQUAL(entry->lba, i * XFER_SIZE + entry_count);
			entry_count++;
		}

		CU_ASSERT_EQUAL(entry_count, XFER_SIZE);
		ftl_rwb_batch_release(batch);
	}

	/* Acquire all entries once more */
	for (size_t i = 0; i < RWB_ENTRY_COUNT; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	/* Pop one batch and check we can acquire XFER_SIZE entries */
	batch = ftl_rwb_pop(g_rwb);
	SPDK_CU_ASSERT_FATAL(batch);
	ftl_rwb_batch_release(batch);

	for (size_t i = 0; i < XFER_SIZE; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NULL(entry);
}

static void
test_rwb_batch_revert(void)
{
	struct ftl_rwb_batch *batch;
	struct ftl_rwb_entry *entry;

	for (size_t i = 0; i < RWB_ENTRY_COUNT; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	/* Pop one batch and revert it */
	batch = ftl_rwb_pop(g_rwb);
	SPDK_CU_ASSERT_FATAL(batch);

	ftl_rwb_batch_revert(batch);

	/* Verify all of the batches */
	for (size_t i = 0; i < RWB_ENTRY_COUNT / XFER_SIZE; ++i) {
		batch = ftl_rwb_pop(g_rwb);
		CU_ASSERT_PTR_NOT_NULL_FATAL(batch);
	}
}

static void
test_rwb_entry_from_offset(void)
{
	struct ftl_rwb_entry *entry;
	struct ftl_ppa ppa = { .cached = 1 };

	for (size_t i = 0; i < RWB_ENTRY_COUNT; ++i) {
		ppa.offset = i;

		entry = ftl_rwb_entry_from_offset(g_rwb, i);
		CU_ASSERT_EQUAL(ppa.offset, entry->pos);
	}
}

static void *
test_rwb_worker(void *ctx)
{
#define ENTRIES_PER_WORKER (16 * RWB_ENTRY_COUNT)
	struct ftl_rwb_entry *entry;
	atomic_bool *done = ctx;

	for (size_t i = 0; i < ENTRIES_PER_WORKER; ++i) {
		while (1) {
			entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
			if (entry) {
				entry->flags = 0;
				ftl_rwb_push(entry);
				break;
			} else {
				/* Allow other threads to run under valgrind */
				pthread_yield();
			}
		}
	}

	atomic_store(done, true);
	return NULL;
}

static void
test_rwb_parallel(void)
{
	struct ftl_rwb_batch *batch;
	struct ftl_rwb_entry *entry;
#define NUM_PARALLEL_WORKERS 4
	pthread_t workers[NUM_PARALLEL_WORKERS];
	atomic_bool done[NUM_PARALLEL_WORKERS];
	size_t num_entries = 0;

	for (size_t i = 0; i < NUM_PARALLEL_WORKERS; ++i) {
		atomic_store(&done[i], false);
		int rc = pthread_create(&workers[i], NULL, test_rwb_worker, (void *)&done[i]);
		CU_ASSERT_TRUE(rc == 0);
	}

	while (1) {
		batch = ftl_rwb_pop(g_rwb);
		if (batch) {
			ftl_rwb_foreach(entry, batch) {
				num_entries++;
			}

			ftl_rwb_batch_release(batch);
		} else {
			int num_done = 0;
			for (size_t i = 0; i < NUM_PARALLEL_WORKERS; ++i) {
				if (atomic_load(&done[i])) {
					num_done++;
					continue;
				}
			}

			if (num_done == NUM_PARALLEL_WORKERS) {
				for (size_t i = 0; i < NUM_PARALLEL_WORKERS; ++i) {
					pthread_join(workers[i], NULL);
				}

				break;
			}

			/* Allow other threads to run under valgrind */
			pthread_yield();
		}
	}

	CU_ASSERT_TRUE(num_entries == NUM_PARALLEL_WORKERS * ENTRIES_PER_WORKER);
}

static void
test_rwb_limits_base(void)
{
	struct ftl_rwb_entry *entry;
	size_t limits[FTL_RWB_TYPE_MAX];

	ftl_rwb_get_limits(g_rwb, limits);
	CU_ASSERT_TRUE(limits[FTL_RWB_TYPE_INTERNAL] == ftl_rwb_entry_cnt(g_rwb));
	CU_ASSERT_TRUE(limits[FTL_RWB_TYPE_USER] == ftl_rwb_entry_cnt(g_rwb));

	/* Verify it's possible to acquire both type of entries */
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
	CU_ASSERT_PTR_NOT_NULL_FATAL(entry);

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NOT_NULL_FATAL(entry);
}

static void
test_rwb_limits_set(void)
{
	size_t limits[FTL_RWB_TYPE_MAX], check[FTL_RWB_TYPE_MAX];

	/* Check that we can't set limit highest than the num of entries */
	for (volatile size_t i = 0; i < FTL_RWB_TYPE_MAX; ++i) {
		ftl_rwb_get_limits(g_rwb, limits);
		limits[i] = limits[i] + 1;

		SPDK_EXPECT_ASSERT_FAIL(ftl_rwb_set_limits(g_rwb, limits));
	}

	/* Check valid limits */
	for (size_t i = 0; i < FTL_RWB_TYPE_MAX; ++i) {
		ftl_rwb_get_limits(g_rwb, limits);
		limits[i] = limits[i] - 1;
	}

	memcpy(check, limits, sizeof(limits));
	ftl_rwb_set_limits(g_rwb, limits);
	ftl_rwb_get_limits(g_rwb, limits);
	SPDK_CU_ASSERT_MEMORY_EQUAL_FATAL(check, limits, sizeof(limits));

	for (size_t i = 0; i < FTL_RWB_TYPE_MAX; ++i) {
		ftl_rwb_get_limits(g_rwb, limits);
		limits[i] = 0;
	}

	memcpy(check, limits, sizeof(limits));
	ftl_rwb_set_limits(g_rwb, limits);
	ftl_rwb_get_limits(g_rwb, limits);
	SPDK_CU_ASSERT_MEMORY_EQUAL_FATAL(check, limits, sizeof(limits));
}

static void
test_rwb_limits_applied(void)
{
	struct ftl_rwb_entry *entry;
	struct ftl_rwb_batch *batch;
	size_t limits[FTL_RWB_TYPE_MAX];
	size_t i;

	/* Check that it's impossible to acquire any entries when the limits are */
	/* set to 0 */
	ftl_rwb_get_limits(g_rwb, limits);
	limits[FTL_RWB_TYPE_USER] = 0;
	ftl_rwb_set_limits(g_rwb, limits);
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NULL(entry);

	limits[FTL_RWB_TYPE_USER] = ftl_rwb_entry_cnt(g_rwb);
	limits[FTL_RWB_TYPE_INTERNAL] = 0;
	ftl_rwb_set_limits(g_rwb, limits);
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
	CU_ASSERT_PTR_NULL(entry);

	/* Check positive limits */
#define TEST_LIMIT XFER_SIZE
	limits[FTL_RWB_TYPE_USER] = ftl_rwb_entry_cnt(g_rwb);
	limits[FTL_RWB_TYPE_INTERNAL] = TEST_LIMIT;
	ftl_rwb_set_limits(g_rwb, limits);
	for (i = 0; i < TEST_LIMIT; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
		SPDK_CU_ASSERT_FATAL(entry);
		entry->flags = FTL_IO_INTERNAL;
		ftl_rwb_push(entry);
	}

	/* Now we expect null, since we've reached threshold */
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
	CU_ASSERT_PTR_NULL(entry);

	/* Complete the entries and check we can retrieve the entries once again */
	batch = ftl_rwb_pop(g_rwb);
	SPDK_CU_ASSERT_FATAL(batch);
	ftl_rwb_batch_release(batch);

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
	CU_ASSERT_PTR_NOT_NULL_FATAL(entry);
	entry->flags = FTL_IO_INTERNAL;

	/* Set the same limit but this time for user entries */
	limits[FTL_RWB_TYPE_USER] = TEST_LIMIT;
	limits[FTL_RWB_TYPE_INTERNAL] = ftl_rwb_entry_cnt(g_rwb);
	ftl_rwb_set_limits(g_rwb, limits);
	for (i = 0; i < TEST_LIMIT; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	/* Now we expect null, since we've reached threshold */
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NULL(entry);

	/* Check that we're still able to acquire a number of internal entries */
	/* while the user entires are being throttled */
	for (i = 0; i < TEST_LIMIT; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
		SPDK_CU_ASSERT_FATAL(entry);
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite_with_setup_and_teardown("ftl_rwb_suite", NULL, NULL,
			setup_rwb, cleanup_rwb);
	if (!suite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_rwb_acquire",
			    test_rwb_acquire) == NULL
		|| CU_add_test(suite, "test_rwb_pop",
			       test_rwb_pop) == NULL
		|| CU_add_test(suite, "test_rwb_batch_revert",
			       test_rwb_batch_revert) == NULL
		|| CU_add_test(suite, "test_rwb_entry_from_offset",
			       test_rwb_entry_from_offset) == NULL
		|| CU_add_test(suite, "test_rwb_parallel",
			       test_rwb_parallel) == NULL
		|| CU_add_test(suite, "test_rwb_limits_base",
			       test_rwb_limits_base) == NULL
		|| CU_add_test(suite, "test_rwb_limits_set",
			       test_rwb_limits_set) == NULL
		|| CU_add_test(suite, "test_rwb_limits_applied",
			       test_rwb_limits_applied) == NULL
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
