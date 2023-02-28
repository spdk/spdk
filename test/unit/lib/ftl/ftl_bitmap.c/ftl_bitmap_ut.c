/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "ftl/utils/ftl_bitmap.c"

#define BITMAP_SIZE	64
#define BITMAP_CAPACITY	(BITMAP_SIZE * 8)

#define TEST_BIT(bi, bbi) \
{ .byte_idx = (bi), .byte_bit_idx = (bbi), .bit_idx = (((bi) * 8) + (bbi)) }

static struct {
	size_t byte_idx;
	uint8_t byte_bit_idx;
	uint64_t bit_idx;
} g_test_bits[] = {
	TEST_BIT(0, 0),
	TEST_BIT(0, 1),
	TEST_BIT(0, 2),
	TEST_BIT(1, 3),
	TEST_BIT(2, 4),
	TEST_BIT(3, 5),
	TEST_BIT(15, 7),
	TEST_BIT(42, 6),
	TEST_BIT(BITMAP_SIZE - 1, 0),
	TEST_BIT(BITMAP_SIZE - 1, 7),
};
static const size_t g_test_bits_count = sizeof(g_test_bits) / sizeof(*g_test_bits);

static unsigned long g_buf[BITMAP_SIZE / sizeof(unsigned long)];
static struct ftl_bitmap *g_bitmap;

static uint64_t
count_set_bits(const struct ftl_bitmap *bitmap)
{
	uint64_t n = 0;
	uint64_t i;

	for (i = 0; i < BITMAP_CAPACITY; i++) {
		if (ftl_bitmap_get(bitmap, i)) {
			n++;
		}
	}

	return n;
}

static void
test_ftl_bitmap_create(void)
{
	struct ftl_bitmap *ret;

	/* unaligned buffer */
	ret = ftl_bitmap_create(((uint8_t *)g_buf) + 1, BITMAP_SIZE);
	CU_ASSERT_EQUAL(ret, NULL);

	/* wrong size */
	ret = ftl_bitmap_create(g_buf, BITMAP_SIZE - 1);
	CU_ASSERT_EQUAL(ret, NULL);
}

static void
test_ftl_bitmap_get(void)
{
	uint8_t *buf = (uint8_t *)g_buf;
	size_t i;

	memset(g_buf, 0, BITMAP_SIZE);

	for (i = 0; i < g_test_bits_count; i++) {
		buf[g_test_bits[i].byte_idx] += (1 << g_test_bits[i].byte_bit_idx);
	}

	CU_ASSERT_EQUAL(count_set_bits(g_bitmap), g_test_bits_count);

	for (i = 0; i < g_test_bits_count; i++) {
		CU_ASSERT_TRUE(ftl_bitmap_get(g_bitmap, g_test_bits[i].bit_idx));
	}
}

static void
test_ftl_bitmap_set(void)
{
	size_t i;

	memset(g_buf, 0, BITMAP_SIZE);

	for (i = 0; i < g_test_bits_count; i++) {
		ftl_bitmap_set(g_bitmap, g_test_bits[i].bit_idx);
	}

	CU_ASSERT_EQUAL(count_set_bits(g_bitmap), g_test_bits_count);

	for (i = 0; i < g_test_bits_count; i++) {
		CU_ASSERT_TRUE(ftl_bitmap_get(g_bitmap, g_test_bits[i].bit_idx));
	}
}

static void
test_ftl_bitmap_clear(void)
{
	size_t i;

	memset(g_buf, 0xff, BITMAP_SIZE);

	for (i = 0; i < g_test_bits_count; i++) {
		ftl_bitmap_clear(g_bitmap, g_test_bits[i].bit_idx);
	}

	CU_ASSERT_EQUAL(count_set_bits(g_bitmap), BITMAP_CAPACITY - g_test_bits_count);

	for (i = 0; i < g_test_bits_count; i++) {
		CU_ASSERT_FALSE(ftl_bitmap_get(g_bitmap, g_test_bits[i].bit_idx));
	}
}

static void
test_ftl_bitmap_find_first_set(void)
{
	size_t i;
	uint64_t bit;

	memset(g_buf, 0, BITMAP_SIZE);

	CU_ASSERT_EQUAL(ftl_bitmap_find_first_set(g_bitmap, 0, UINT64_MAX), UINT64_MAX);

	for (i = 1; i <= g_test_bits_count; i++) {
		bit = g_test_bits[g_test_bits_count - i].bit_idx;

		ftl_bitmap_set(g_bitmap, bit);

		CU_ASSERT_EQUAL(ftl_bitmap_find_first_set(g_bitmap, 0, UINT64_MAX), bit);
		CU_ASSERT_EQUAL(ftl_bitmap_find_first_set(g_bitmap, 0, bit), bit);
		if (bit > 0) {
			CU_ASSERT_EQUAL(ftl_bitmap_find_first_set(g_bitmap, 0, bit - 1), UINT64_MAX);
		}
	}

	for (i = 0; i < g_test_bits_count; i++) {
		bit = g_test_bits[i].bit_idx;

		CU_ASSERT_EQUAL(ftl_bitmap_find_first_set(g_bitmap, bit, UINT64_MAX), bit);
		CU_ASSERT_EQUAL(ftl_bitmap_find_first_set(g_bitmap, bit, bit), bit);
	}
}

static void
test_ftl_bitmap_find_first_clear(void)
{
	size_t i;
	uint64_t bit;

	memset(g_buf, 0xff, BITMAP_SIZE);

	CU_ASSERT_EQUAL(ftl_bitmap_find_first_clear(g_bitmap, 0, UINT64_MAX), UINT64_MAX);

	for (i = 1; i <= g_test_bits_count; i++) {
		bit = g_test_bits[g_test_bits_count - i].bit_idx;

		ftl_bitmap_clear(g_bitmap, bit);

		CU_ASSERT_EQUAL(ftl_bitmap_find_first_clear(g_bitmap, 0, UINT64_MAX), bit);
		CU_ASSERT_EQUAL(ftl_bitmap_find_first_clear(g_bitmap, 0, bit), bit);
		if (bit > 0) {
			CU_ASSERT_EQUAL(ftl_bitmap_find_first_clear(g_bitmap, 0, bit - 1), UINT64_MAX);
		}
	}

	for (i = 0; i < g_test_bits_count; i++) {
		bit = g_test_bits[i].bit_idx;

		CU_ASSERT_EQUAL(ftl_bitmap_find_first_clear(g_bitmap, bit, UINT64_MAX), bit);
		CU_ASSERT_EQUAL(ftl_bitmap_find_first_clear(g_bitmap, bit, bit), bit);
	}
}

static void
test_ftl_bitmap_count_set(void)
{
	size_t i;

	memset(g_buf, 0, BITMAP_SIZE);

	for (i = 0; i < g_test_bits_count; i++) {
		ftl_bitmap_set(g_bitmap, g_test_bits[i].bit_idx);
	}

	CU_ASSERT_EQUAL(g_test_bits_count, ftl_bitmap_count_set(g_bitmap));
	CU_ASSERT_EQUAL(count_set_bits(g_bitmap), ftl_bitmap_count_set(g_bitmap));
}

static int
test_setup(void)
{
	g_bitmap = ftl_bitmap_create(g_buf, BITMAP_SIZE);
	if (!g_bitmap) {
		return -ENOMEM;
	}

	return 0;
}

static int
test_cleanup(void)
{
	free(g_bitmap);
	g_bitmap = NULL;
	return 0;
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_bitmap", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_ftl_bitmap_create);
	CU_ADD_TEST(suite, test_ftl_bitmap_get);
	CU_ADD_TEST(suite, test_ftl_bitmap_set);
	CU_ADD_TEST(suite, test_ftl_bitmap_clear);
	CU_ADD_TEST(suite, test_ftl_bitmap_find_first_set);
	CU_ADD_TEST(suite, test_ftl_bitmap_find_first_clear);
	CU_ADD_TEST(suite, test_ftl_bitmap_count_set);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
