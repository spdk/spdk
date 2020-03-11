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

#include "util/bit_array.c"
#include "common/lib/test_env.c"

static void
test_1bit(void)
{
	struct spdk_bit_array *ba;

	ba = spdk_bit_array_create(1);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 1);

	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == UINT32_MAX);

	/* Set bit 0 */
	CU_ASSERT(spdk_bit_array_set(ba, 0) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == true);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == 0);

	/* Clear bit 0 */
	spdk_bit_array_clear(ba, 0);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == UINT32_MAX);

	spdk_bit_array_free(&ba);
	CU_ASSERT(ba == NULL);
}

static void
test_64bit(void)
{
	struct spdk_bit_array *ba;

	ba = spdk_bit_array_create(64);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 64);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_get(ba, 63) == false);
	CU_ASSERT(spdk_bit_array_get(ba, 64) == false);
	CU_ASSERT(spdk_bit_array_get(ba, 1000) == false);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == UINT32_MAX);

	/* Set bit 1 */
	CU_ASSERT(spdk_bit_array_set(ba, 1) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == true);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == 1);

	/* Set bit 63 (1 still set) */
	CU_ASSERT(spdk_bit_array_set(ba, 63) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == true);
	CU_ASSERT(spdk_bit_array_get(ba, 63) == true);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == 1);

	/* Clear bit 1 (63 still set) */
	spdk_bit_array_clear(ba, 1);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == false);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == 63);

	/* Clear bit 63 (no bits set) */
	spdk_bit_array_clear(ba, 63);
	CU_ASSERT(spdk_bit_array_get(ba, 63) == false);
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 0) == UINT32_MAX);

	spdk_bit_array_free(&ba);
}

static void
test_find(void)
{
	struct spdk_bit_array *ba;
	uint32_t i;

	ba = spdk_bit_array_create(256);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 256);

	/* Set all bits */
	for (i = 0; i < 256; i++) {
		CU_ASSERT(spdk_bit_array_set(ba, i) == 0);
	}

	/* Verify that find_first_set and find_first_clear work for each starting position */
	for (i = 0; i < 256; i++) {
		CU_ASSERT(spdk_bit_array_find_first_set(ba, i) == i);
		CU_ASSERT(spdk_bit_array_find_first_clear(ba, i) == UINT32_MAX);
	}
	CU_ASSERT(spdk_bit_array_find_first_set(ba, 256) == UINT32_MAX);
	CU_ASSERT(spdk_bit_array_find_first_clear(ba, 256) == UINT32_MAX);

	/* Clear bits 0 through 31 */
	for (i = 0; i < 32; i++) {
		spdk_bit_array_clear(ba, i);
	}

	for (i = 0; i < 32; i++) {
		CU_ASSERT(spdk_bit_array_find_first_set(ba, i) == 32);
		CU_ASSERT(spdk_bit_array_find_first_clear(ba, i) == i);
	}

	for (i = 32; i < 256; i++) {
		CU_ASSERT(spdk_bit_array_find_first_set(ba, i) == i);
		CU_ASSERT(spdk_bit_array_find_first_clear(ba, i) == UINT32_MAX);
	}

	/* Clear bit 255 */
	spdk_bit_array_clear(ba, 255);

	for (i = 0; i < 32; i++) {
		CU_ASSERT(spdk_bit_array_find_first_set(ba, i) == 32);
		CU_ASSERT(spdk_bit_array_find_first_clear(ba, i) == i);
	}

	for (i = 32; i < 255; i++)  {
		CU_ASSERT(spdk_bit_array_find_first_set(ba, i) == i);
		CU_ASSERT(spdk_bit_array_find_first_clear(ba, i) == 255);
	}

	CU_ASSERT(spdk_bit_array_find_first_clear(ba, 256) == UINT32_MAX);

	spdk_bit_array_free(&ba);
}

static void
test_resize(void)
{
	struct spdk_bit_array *ba;

	/* Start with a 0 bit array */
	ba = spdk_bit_array_create(0);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_set(ba, 0) == -EINVAL);
	spdk_bit_array_clear(ba, 0);

	/* Increase size to 1 bit */
	SPDK_CU_ASSERT_FATAL(spdk_bit_array_resize(&ba, 1) == 0);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 1);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_set(ba, 0) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == true);

	/* Increase size to 2 bits */
	SPDK_CU_ASSERT_FATAL(spdk_bit_array_resize(&ba, 2) == 0);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 2);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == false);
	CU_ASSERT(spdk_bit_array_set(ba, 1) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == true);

	/* Shrink size back to 1 bit */
	SPDK_CU_ASSERT_FATAL(spdk_bit_array_resize(&ba, 1) == 0);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 1);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == true);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == false);

	/* Increase size to 65 bits */
	SPDK_CU_ASSERT_FATAL(spdk_bit_array_resize(&ba, 65) == 0);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 65);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == true);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == false);
	CU_ASSERT(spdk_bit_array_set(ba, 64) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 64) == true);

	/* Shrink size back to 0 bits */
	SPDK_CU_ASSERT_FATAL(spdk_bit_array_resize(&ba, 0) == 0);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_capacity(ba) == 0);
	CU_ASSERT(spdk_bit_array_get(ba, 0) == false);
	CU_ASSERT(spdk_bit_array_get(ba, 1) == false);

	spdk_bit_array_free(&ba);
}

static void
test_errors(void)
{
	/* Passing NULL to resize should fail. */
	CU_ASSERT(spdk_bit_array_resize(NULL, 0) == -EINVAL);

	/* Passing NULL to free is a no-op. */
	spdk_bit_array_free(NULL);
}

static void
test_count(void)
{
	struct spdk_bit_array *ba;
	uint32_t i;

	/* 0-bit array should have 0 bits set and 0 bits clear */
	ba = spdk_bit_array_create(0);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 0);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 0);
	spdk_bit_array_free(&ba);

	/* 1-bit array */
	ba = spdk_bit_array_create(1);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 0);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 1);
	spdk_bit_array_set(ba, 0);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 1);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 0);
	spdk_bit_array_free(&ba);

	/* 65-bit array */
	ba = spdk_bit_array_create(65);
	SPDK_CU_ASSERT_FATAL(ba != NULL);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 0);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 65);
	spdk_bit_array_set(ba, 0);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 1);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 64);
	spdk_bit_array_set(ba, 5);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 2);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 63);
	spdk_bit_array_set(ba, 13);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 3);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 62);
	spdk_bit_array_clear(ba, 0);
	CU_ASSERT(spdk_bit_array_count_set(ba) == 2);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 63);
	for (i = 0; i < 65; i++) {
		spdk_bit_array_set(ba, i);
	}
	CU_ASSERT(spdk_bit_array_count_set(ba) == 65);
	CU_ASSERT(spdk_bit_array_count_clear(ba) == 0);
	for (i = 0; i < 65; i++) {
		spdk_bit_array_clear(ba, i);
		CU_ASSERT(spdk_bit_array_count_set(ba) == 65 - i - 1);
		CU_ASSERT(spdk_bit_array_count_clear(ba) == i + 1);
	}
	spdk_bit_array_free(&ba);
}

#define TEST_MASK_SIZE 128
#define TEST_BITS_NUM (TEST_MASK_SIZE * 8 - 3)
static void
test_mask_store_load(void)
{
	struct spdk_bit_array *ba;
	uint8_t mask[TEST_MASK_SIZE] = { 0 };
	uint32_t i;

	ba = spdk_bit_array_create(TEST_BITS_NUM);

	/* Check if stored mask is consistent with bit array mask */
	spdk_bit_array_set(ba, 0);
	spdk_bit_array_set(ba, TEST_BITS_NUM / 2);
	spdk_bit_array_set(ba, TEST_BITS_NUM - 1);

	spdk_bit_array_store_mask(ba, mask);

	for (i = 0; i < TEST_BITS_NUM; i++) {
		if (i == 0 || i == TEST_BITS_NUM / 2 || i == TEST_BITS_NUM - 1) {
			CU_ASSERT((mask[i / 8] & (1U << (i % 8))));
		} else {
			CU_ASSERT(!(mask[i / 8] & (1U << (i % 8))));
		}
	}

	/* Check if loaded mask is consistent with bit array mask */
	memset(mask, 0, TEST_MASK_SIZE);
	mask[0] = 1;
	mask[TEST_MASK_SIZE - 1] = 1U << 4;

	spdk_bit_array_load_mask(ba, mask);

	CU_ASSERT(spdk_bit_array_get(ba, 0));
	CU_ASSERT(spdk_bit_array_get(ba, TEST_BITS_NUM - 1));

	spdk_bit_array_clear(ba, 0);
	spdk_bit_array_clear(ba, TEST_BITS_NUM - 1);

	for (i = 0; i < TEST_BITS_NUM; i++) {
		CU_ASSERT(!spdk_bit_array_get(ba, i));
	}

	spdk_bit_array_free(&ba);
}

static void
test_mask_clear(void)
{
	struct spdk_bit_array *ba;
	uint32_t i;

	ba = spdk_bit_array_create(TEST_BITS_NUM);

	for (i = 0; i < TEST_BITS_NUM; i++) {
		spdk_bit_array_set(ba, i);
	}

	spdk_bit_array_clear_mask(ba);

	for (i = 0; i < TEST_BITS_NUM; i++) {
		CU_ASSERT(!spdk_bit_array_get(ba, i));
	}

	spdk_bit_array_free(&ba);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("bit_array", NULL, NULL);

	CU_ADD_TEST(suite, test_1bit);
	CU_ADD_TEST(suite, test_64bit);
	CU_ADD_TEST(suite, test_find);
	CU_ADD_TEST(suite, test_resize);
	CU_ADD_TEST(suite, test_errors);
	CU_ADD_TEST(suite, test_count);
	CU_ADD_TEST(suite, test_mask_store_load);
	CU_ADD_TEST(suite, test_mask_clear);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
