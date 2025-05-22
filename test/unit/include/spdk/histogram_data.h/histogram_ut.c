/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"

#include "spdk/histogram_data.h"
#include "spdk/util.h"

uint64_t g_values[] = {
	1,
	10,
	1000,
	50000,
	(1ULL << 63),
	UINT64_MAX
};

uint64_t *g_values_end = &g_values[SPDK_COUNTOF(g_values)];
uint64_t g_total;
uint64_t g_number_of_merged_histograms;

static void
check_values(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	uint64_t **values = ctx;

	if (count == 0) {
		return;
	}

	CU_ASSERT(so_far == (g_total + count));

	/*
	 * The bucket for this iteration does not include end, but
	 *  subtract one anyways to account for the last bucket
	 *  which will have end = 0x0 (UINT64_MAX + 1).
	 */
	end--;

	while (1) {
		CU_ASSERT(**values >= start);
		/*
		 * We subtracted one from end above, so it's OK here for
		 *  **values to equal end.
		 */
		CU_ASSERT(**values <= end);
		g_total += g_number_of_merged_histograms;
		count -= g_number_of_merged_histograms;
		(*values)++;
		if (*values == g_values_end || **values > end) {
			break;
		}
	}
	CU_ASSERT(count == 0);
}

static void
histogram_test(void)
{
	struct spdk_histogram_data *h;
	uint64_t *values = g_values;
	uint32_t i;

	h = spdk_histogram_data_alloc();

	for (i = 0; i < SPDK_COUNTOF(g_values); i++) {
		spdk_histogram_data_tally(h, g_values[i]);
	}
	g_total = 0;
	g_number_of_merged_histograms = 1;
	spdk_histogram_data_iterate(h, check_values, &values);

	spdk_histogram_data_free(h);
}

static void
histogram_merge(void)
{
	struct spdk_histogram_data *h1, *h2;
	uint64_t *values = g_values;
	uint32_t i;
	int rc;

	h1 = spdk_histogram_data_alloc();
	h2 = spdk_histogram_data_alloc();

	for (i = 0; i < SPDK_COUNTOF(g_values); i++) {
		spdk_histogram_data_tally(h1, g_values[i]);
		spdk_histogram_data_tally(h2, g_values[i]);
	}

	rc = spdk_histogram_data_merge(h1, h2);
	CU_ASSERT(rc == 0);

	g_total = 0;
	g_number_of_merged_histograms = 2;
	spdk_histogram_data_iterate(h1, check_values, &values);

	spdk_histogram_data_free(h1);
	spdk_histogram_data_free(h2);

	h1 = spdk_histogram_data_alloc_sized(SPDK_HISTOGRAM_GRANULARITY_DEFAULT);
	h2 = spdk_histogram_data_alloc_sized(SPDK_HISTOGRAM_GRANULARITY_DEFAULT - 1);

	rc = spdk_histogram_data_merge(h1, h2);
	CU_ASSERT(rc == -EINVAL);

	spdk_histogram_data_free(h1);
	spdk_histogram_data_free(h2);
}

struct value_with_count {
	uint64_t value;
	uint64_t count;
};

static void
check_values_with_count(void *ctx, uint64_t start, uint64_t end, uint64_t count,
			uint64_t total, uint64_t so_far)
{
	struct value_with_count **values = ctx;

	if (count == 0) {
		return;
	}

	CU_ASSERT((**values).count == count);

	/*
	 * The bucket for this iteration does not include end, but
	 *  subtract one anyways to account for the last bucket
	 *  which will have end = 0x0 (UINT64_MAX + 1).
	 */
	end--;

	CU_ASSERT((**values).value >= start);
	/*
	 * We subtracted one from end above, so it's OK here for
	 *  value to equal end.
	 */
	CU_ASSERT((*values)->value <= end);
	(*values)++;
}

#define TEST_TALLY_COUNT 3
#define TEST_MIN_VAL (1ULL << 9)
#define TEST_MAX_VAL (1ULL << 30)
#define TEST_BELOW_MIN_VAL (TEST_MIN_VAL >> 1)
#define TEST_IN_MIDDLE_VAL ((TEST_MIN_VAL + TEST_MAX_VAL) >> 2)
#define TEST_ABOVE_MAX_VAL (TEST_MAX_VAL << 1)

struct value_with_count g_value_with_count[] = {
	{.value = TEST_MIN_VAL, .count = 2 * TEST_TALLY_COUNT},
	{.value = TEST_IN_MIDDLE_VAL, .count = TEST_TALLY_COUNT},
	{.value = TEST_MAX_VAL - 1, .count = 2 * TEST_TALLY_COUNT},
};

static void
histogram_min_max_range_test(void)
{
	struct spdk_histogram_data *h1, *h2;
	struct value_with_count *values = g_value_with_count;
	int i;

	h1 = spdk_histogram_data_alloc();

	CU_ASSERT(h1->min_range == 0);
	CU_ASSERT(h1->max_range == SPDK_HISTOGRAM_BUCKET_LSB(h1));

	h2 = spdk_histogram_data_alloc_sized_ext(SPDK_HISTOGRAM_GRANULARITY_DEFAULT, TEST_MIN_VAL,
			TEST_MAX_VAL);

	for (i = 0; i < TEST_TALLY_COUNT; i++) {
		spdk_histogram_data_tally(h2, TEST_BELOW_MIN_VAL);
		spdk_histogram_data_tally(h2, TEST_MIN_VAL);
		spdk_histogram_data_tally(h2, TEST_IN_MIDDLE_VAL);
		spdk_histogram_data_tally(h2, TEST_MAX_VAL);
		spdk_histogram_data_tally(h2, TEST_ABOVE_MAX_VAL);
	}

	spdk_histogram_data_iterate(h2, check_values_with_count, &values);

	spdk_histogram_data_free(h1);
	spdk_histogram_data_free(h2);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("histogram", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "histogram_test", histogram_test) == NULL ||
		CU_add_test(suite, "histogram_merge", histogram_merge) == NULL ||
		CU_add_test(suite, "histogram_min_max_range_test", histogram_min_max_range_test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
