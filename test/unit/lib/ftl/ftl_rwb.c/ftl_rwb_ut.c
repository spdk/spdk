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

#include "ftl/ftl_rwb.c"

struct ftl_rwb_ut {
	/* configurations */
	struct	spdk_ftl_conf conf;
	size_t	metadata_size;
	size_t	num_punits;
	size_t	xfer_size;

	/* the fields below are calculated by the configurations */
	size_t	max_batches;
	size_t	max_active_batches;
	size_t	max_entries;
	size_t	max_allocable_entries;
	size_t	interleave_offset;
	size_t	num_entries_per_worker;
};

static struct ftl_rwb *g_rwb;
static struct ftl_rwb_ut g_ut;

static int _init_suite(void);

static int
init_suite1(void)
{
	g_ut.conf.rwb_size = 1024 * 1024;
	g_ut.conf.num_interleave_units = 1;
	g_ut.metadata_size = 64;
	g_ut.num_punits = 4;
	g_ut.xfer_size = 16;

	return _init_suite();
}

static int
init_suite2(void)
{
	g_ut.conf.rwb_size = 2 * 1024 * 1024;
	g_ut.conf.num_interleave_units = 4;
	g_ut.metadata_size = 64;
	g_ut.num_punits = 8;
	g_ut.xfer_size = 16;

	return _init_suite();
}

static int
_init_suite(void)
{
	struct spdk_ftl_conf *conf = &g_ut.conf;

	if (conf->num_interleave_units == 0 ||
	    g_ut.xfer_size % conf->num_interleave_units ||
	    g_ut.num_punits == 0) {
		return -1;
	}

	g_ut.max_batches = conf->rwb_size / (FTL_BLOCK_SIZE * g_ut.xfer_size);
	if (conf->num_interleave_units > 1) {
		g_ut.max_batches += g_ut.num_punits;
		g_ut.max_active_batches = g_ut.num_punits;
	} else {
		g_ut.max_batches++;
		g_ut.max_active_batches = 1;
	}

	g_ut.max_entries = g_ut.max_batches * g_ut.xfer_size;
	g_ut.max_allocable_entries = (g_ut.max_batches / g_ut.max_active_batches) *
				     g_ut.max_active_batches * g_ut.xfer_size;

	g_ut.interleave_offset = g_ut.xfer_size / conf->num_interleave_units;

	/* if max_batches is less than max_active_batches * 2, */
	/* test_rwb_limits_applied will be failed. */
	if (g_ut.max_batches < g_ut.max_active_batches * 2) {
		return -1;
	}

	g_ut.num_entries_per_worker = 16 * g_ut.max_allocable_entries;

	return 0;
}

static void
setup_rwb(void)
{
	g_rwb = ftl_rwb_init(&g_ut.conf, g_ut.xfer_size,
			     g_ut.metadata_size, g_ut.num_punits);
	SPDK_CU_ASSERT_FATAL(g_rwb != NULL);
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
	size_t i;

	setup_rwb();
	/* Verify that it's possible to acquire all of the entries */
	for (i = 0; i < g_ut.max_allocable_entries; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NULL(entry);
	cleanup_rwb();
}

static void
test_rwb_pop(void)
{
	struct ftl_rwb_entry *entry;
	struct ftl_rwb_batch *batch;
	size_t entry_count, i, i_reset = 0, i_offset = 0;
	uint64_t expected_lba;

	setup_rwb();

	/* Acquire all entries */
	for (i = 0; i < g_ut.max_allocable_entries; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);

		SPDK_CU_ASSERT_FATAL(entry);
		entry->lba = i;
		ftl_rwb_push(entry);
	}

	/* Pop all batches and free them */
	for (i = 0; i < g_ut.max_allocable_entries / g_ut.xfer_size; ++i) {
		batch = ftl_rwb_pop(g_rwb);
		SPDK_CU_ASSERT_FATAL(batch);
		entry_count = 0;

		ftl_rwb_foreach(entry, batch) {
			if (i % g_ut.max_active_batches == 0) {
				i_offset = i * g_ut.xfer_size;
			}

			if (entry_count % g_ut.interleave_offset == 0) {
				i_reset = i % g_ut.max_active_batches +
					  (entry_count / g_ut.interleave_offset) *
					  g_ut.max_active_batches;
			}

			expected_lba = i_offset +
				       i_reset * g_ut.interleave_offset +
				       entry_count % g_ut.interleave_offset;

			CU_ASSERT_EQUAL(entry->lba, expected_lba);
			entry_count++;
		}

		CU_ASSERT_EQUAL(entry_count, g_ut.xfer_size);
		ftl_rwb_batch_release(batch);
	}

	/* Acquire all entries once more */
	for (i = 0; i < g_ut.max_allocable_entries; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	/* Pop one batch and check we can acquire xfer_size entries */
	for (i = 0; i < g_ut.max_active_batches; i++) {
		batch = ftl_rwb_pop(g_rwb);
		SPDK_CU_ASSERT_FATAL(batch);
		ftl_rwb_batch_release(batch);
	}

	for (i = 0; i < g_ut.xfer_size * g_ut.max_active_batches; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);

		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NULL(entry);

	/* Pop and Release all batches */
	for (i = 0; i < g_ut.max_allocable_entries / g_ut.xfer_size; ++i) {
		batch = ftl_rwb_pop(g_rwb);
		SPDK_CU_ASSERT_FATAL(batch);
		ftl_rwb_batch_release(batch);
	}

	cleanup_rwb();
}

static void
test_rwb_disable_interleaving(void)
{
	struct ftl_rwb_entry *entry;
	struct ftl_rwb_batch *batch;
	size_t entry_count, i;

	setup_rwb();

	ftl_rwb_disable_interleaving(g_rwb);

	/* Acquire all entries and assign sequential lbas */
	for (i = 0; i < g_ut.max_allocable_entries; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);

		SPDK_CU_ASSERT_FATAL(entry);
		entry->lba = i;
		ftl_rwb_push(entry);
	}

	/* Check for expected lbas */
	for (i = 0; i < g_ut.max_allocable_entries / g_ut.xfer_size; ++i) {
		batch = ftl_rwb_pop(g_rwb);
		SPDK_CU_ASSERT_FATAL(batch);
		entry_count = 0;

		ftl_rwb_foreach(entry, batch) {
			CU_ASSERT_EQUAL(entry->lba, i * g_ut.xfer_size + entry_count);
			entry_count++;
		}

		CU_ASSERT_EQUAL(entry_count, g_ut.xfer_size);
		ftl_rwb_batch_release(batch);
	}

	cleanup_rwb();
}

static void
test_rwb_batch_revert(void)
{
	struct ftl_rwb_batch *batch;
	struct ftl_rwb_entry *entry;
	size_t i;

	setup_rwb();
	for (i = 0; i < g_ut.max_allocable_entries; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	/* Pop one batch and revert it */
	batch = ftl_rwb_pop(g_rwb);
	SPDK_CU_ASSERT_FATAL(batch);

	ftl_rwb_batch_revert(batch);

	/* Verify all of the batches */
	for (i = 0; i < g_ut.max_allocable_entries / g_ut.xfer_size; ++i) {
		batch = ftl_rwb_pop(g_rwb);
		CU_ASSERT_PTR_NOT_NULL_FATAL(batch);
	}
	cleanup_rwb();
}

static void
test_rwb_entry_from_offset(void)
{
	struct ftl_rwb_entry *entry;
	struct ftl_addr addr = { .cached = 1 };
	size_t i;

	setup_rwb();
	for (i = 0; i < g_ut.max_allocable_entries; ++i) {
		addr.cache_offset = i;

		entry = ftl_rwb_entry_from_offset(g_rwb, i);
		CU_ASSERT_EQUAL(addr.cache_offset, entry->pos);
	}
	cleanup_rwb();
}

static void *
test_rwb_worker(void *ctx)
{
	struct ftl_rwb_entry *entry;
	unsigned int *num_done = ctx;
	size_t i;

	for (i = 0; i < g_ut.num_entries_per_worker; ++i) {
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

	__atomic_fetch_add(num_done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void
test_rwb_parallel(void)
{
	struct ftl_rwb_batch *batch;
	struct ftl_rwb_entry *entry;
#define NUM_PARALLEL_WORKERS 4
	pthread_t workers[NUM_PARALLEL_WORKERS];
	unsigned int num_done = 0;
	size_t i, num_entries = 0;
	bool all_done = false;
	int rc;

	setup_rwb();
	for (i = 0; i < NUM_PARALLEL_WORKERS; ++i) {
		rc = pthread_create(&workers[i], NULL, test_rwb_worker, (void *)&num_done);
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
			if (NUM_PARALLEL_WORKERS == __atomic_load_n(&num_done, __ATOMIC_SEQ_CST)) {
				if (!all_done) {
					/*  Pop all left entries from rwb */
					all_done = true;
					continue;
				}

				for (i = 0; i < NUM_PARALLEL_WORKERS; ++i) {
					pthread_join(workers[i], NULL);
				}

				break;
			}

			/* Allow other threads to run under valgrind */
			pthread_yield();
		}
	}

	CU_ASSERT_TRUE(num_entries == NUM_PARALLEL_WORKERS * g_ut.num_entries_per_worker);
	cleanup_rwb();
}

static void
test_rwb_limits_base(void)
{
	struct ftl_rwb_entry *entry;
	size_t limits[FTL_RWB_TYPE_MAX];

	setup_rwb();
	ftl_rwb_get_limits(g_rwb, limits);
	CU_ASSERT_TRUE(limits[FTL_RWB_TYPE_INTERNAL] == ftl_rwb_entry_cnt(g_rwb));
	CU_ASSERT_TRUE(limits[FTL_RWB_TYPE_USER] == ftl_rwb_entry_cnt(g_rwb));

	/* Verify it's possible to acquire both type of entries */
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
	CU_ASSERT_PTR_NOT_NULL_FATAL(entry);

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NOT_NULL_FATAL(entry);
	cleanup_rwb();
}

static void
test_rwb_limits_set(void)
{
	size_t limits[FTL_RWB_TYPE_MAX], check[FTL_RWB_TYPE_MAX];
	size_t i;

	setup_rwb();

	/* Check valid limits */
	ftl_rwb_get_limits(g_rwb, limits);
	memcpy(check, limits, sizeof(limits));
	ftl_rwb_set_limits(g_rwb, limits);
	ftl_rwb_get_limits(g_rwb, limits);
	CU_ASSERT(memcmp(check, limits, sizeof(limits)) == 0);

	for (i = 0; i < FTL_RWB_TYPE_MAX; ++i) {
		ftl_rwb_get_limits(g_rwb, limits);
		limits[i] = 0;
	}

	memcpy(check, limits, sizeof(limits));
	ftl_rwb_set_limits(g_rwb, limits);
	ftl_rwb_get_limits(g_rwb, limits);
	CU_ASSERT(memcmp(check, limits, sizeof(limits)) == 0);
	cleanup_rwb();
}

static void
test_rwb_limits_applied(void)
{
	struct ftl_rwb_entry *entry;
	struct ftl_rwb_batch *batch;
	size_t limits[FTL_RWB_TYPE_MAX];
	const size_t test_limit = g_ut.xfer_size * g_ut.max_active_batches;
	size_t i;

	setup_rwb();

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
	limits[FTL_RWB_TYPE_USER] = ftl_rwb_entry_cnt(g_rwb);
	limits[FTL_RWB_TYPE_INTERNAL] = test_limit;
	ftl_rwb_set_limits(g_rwb, limits);
	for (i = 0; i < test_limit; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
		SPDK_CU_ASSERT_FATAL(entry);
		entry->flags = FTL_IO_INTERNAL;
		ftl_rwb_push(entry);
	}

	/* Now we expect null, since we've reached threshold */
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
	CU_ASSERT_PTR_NULL(entry);

	for (i = 0; i < test_limit / g_ut.xfer_size; ++i) {
		/* Complete the entries and check we can retrieve the entries once again */
		batch = ftl_rwb_pop(g_rwb);
		SPDK_CU_ASSERT_FATAL(batch);
		ftl_rwb_batch_release(batch);
	}

	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
	SPDK_CU_ASSERT_FATAL(entry);
	entry->flags = FTL_IO_INTERNAL;

	/* Set the same limit but this time for user entries */
	limits[FTL_RWB_TYPE_USER] = test_limit;
	limits[FTL_RWB_TYPE_INTERNAL] = ftl_rwb_entry_cnt(g_rwb);
	ftl_rwb_set_limits(g_rwb, limits);
	for (i = 0; i < test_limit; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
		SPDK_CU_ASSERT_FATAL(entry);
		ftl_rwb_push(entry);
	}

	/* Now we expect null, since we've reached threshold */
	entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_USER);
	CU_ASSERT_PTR_NULL(entry);

	/* Check that we're still able to acquire a number of internal entries */
	/* while the user entires are being throttled */
	for (i = 0; i < g_ut.xfer_size; ++i) {
		entry = ftl_rwb_acquire(g_rwb, FTL_RWB_TYPE_INTERNAL);
		SPDK_CU_ASSERT_FATAL(entry);
	}

	cleanup_rwb();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite1, suite2;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite1 = CU_add_suite("suite1", init_suite1, NULL);
	if (!suite1) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	suite2 = CU_add_suite("suite2", init_suite2, NULL);
	if (!suite2) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite1, "test_rwb_acquire",
			    test_rwb_acquire) == NULL
		|| CU_add_test(suite1, "test_rwb_pop",
			       test_rwb_pop) == NULL
		|| CU_add_test(suite1, "test_rwb_disable_interleaving",
			       test_rwb_disable_interleaving) == NULL
		|| CU_add_test(suite1, "test_rwb_batch_revert",
			       test_rwb_batch_revert) == NULL
		|| CU_add_test(suite1, "test_rwb_entry_from_offset",
			       test_rwb_entry_from_offset) == NULL
		|| CU_add_test(suite1, "test_rwb_parallel",
			       test_rwb_parallel) == NULL
		|| CU_add_test(suite1, "test_rwb_limits_base",
			       test_rwb_limits_base) == NULL
		|| CU_add_test(suite1, "test_rwb_limits_set",
			       test_rwb_limits_set) == NULL
		|| CU_add_test(suite1, "test_rwb_limits_applied",
			       test_rwb_limits_applied) == NULL
		|| CU_add_test(suite2, "test_rwb_acquire",
			       test_rwb_acquire) == NULL
		|| CU_add_test(suite2, "test_rwb_pop",
			       test_rwb_pop) == NULL
		|| CU_add_test(suite2, "test_rwb_disable_interleaving",
			       test_rwb_disable_interleaving) == NULL
		|| CU_add_test(suite2, "test_rwb_batch_revert",
			       test_rwb_batch_revert) == NULL
		|| CU_add_test(suite2, "test_rwb_entry_from_offset",
			       test_rwb_entry_from_offset) == NULL
		|| CU_add_test(suite2, "test_rwb_parallel",
			       test_rwb_parallel) == NULL
		|| CU_add_test(suite2, "test_rwb_limits_base",
			       test_rwb_limits_base) == NULL
		|| CU_add_test(suite2, "test_rwb_limits_set",
			       test_rwb_limits_set) == NULL
		|| CU_add_test(suite2, "test_rwb_limits_applied",
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
