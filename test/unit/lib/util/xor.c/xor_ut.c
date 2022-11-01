/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "util/xor.c"
#include "common/lib/test_env.c"

#define BUF_COUNT 8
#define SRC_BUF_COUNT (BUF_COUNT - 1)
#define BUF_SIZE 4096

static void
test_xor_gen(void)
{
	void *bufs[BUF_COUNT];
	void *bufs2[SRC_BUF_COUNT];
	uint8_t *ref, *dest;
	int ret;
	size_t i, j;
	uint32_t *tmp;

	/* alloc and fill the buffers with a pattern */
	for (i = 0; i < BUF_COUNT; i++) {
		ret = posix_memalign(&bufs[i], spdk_xor_get_optimal_alignment(), BUF_SIZE);
		SPDK_CU_ASSERT_FATAL(ret == 0);

		tmp = bufs[i];
		for (j = 0; j < BUF_SIZE / sizeof(*tmp); j++) {
			tmp[j] = (i << 16) + j;
		}
	}
	dest = bufs[SRC_BUF_COUNT];

	/* prepare the reference buffer */
	ref = malloc(BUF_SIZE);
	SPDK_CU_ASSERT_FATAL(ref != NULL);

	memset(ref, 0, BUF_SIZE);
	for (i = 0; i < SRC_BUF_COUNT; i++) {
		for (j = 0; j < BUF_SIZE; j++) {
			ref[j] ^= ((uint8_t *)bufs[i])[j];
		}
	}

	/* generate xor, compare the dest and reference buffers */
	ret = spdk_xor_gen(dest, bufs, SRC_BUF_COUNT, BUF_SIZE);
	CU_ASSERT(ret == 0);
	ret = memcmp(ref, dest, BUF_SIZE);
	CU_ASSERT(ret == 0);

	/* len not multiple of alignment */
	memset(dest, 0xba, BUF_SIZE);
	ret = spdk_xor_gen(dest, bufs, SRC_BUF_COUNT, BUF_SIZE - 1);
	CU_ASSERT(ret == 0);
	ret = memcmp(ref, dest, BUF_SIZE - 1);
	CU_ASSERT(ret == 0);

	/* unaligned buffer */
	memcpy(bufs2, bufs, sizeof(bufs2));
	bufs2[1] += 1;
	bufs2[2] += 2;
	bufs2[3] += 3;

	memset(ref, 0, BUF_SIZE);
	for (i = 0; i < SRC_BUF_COUNT; i++) {
		for (j = 0; j < BUF_SIZE - SRC_BUF_COUNT; j++) {
			ref[j] ^= ((uint8_t *)bufs2[i])[j];
		}
	}

	memset(dest, 0xba, BUF_SIZE);
	ret = spdk_xor_gen(dest, bufs2, SRC_BUF_COUNT, BUF_SIZE - SRC_BUF_COUNT);
	CU_ASSERT(ret == 0);
	ret = memcmp(ref, dest, BUF_SIZE - SRC_BUF_COUNT);
	CU_ASSERT(ret == 0);

	/* xoring a buffer with itself should result in all zeros */
	memset(ref, 0, BUF_SIZE);
	bufs2[0] = bufs[0];
	bufs2[1] = bufs[0];
	dest = bufs[0];

	ret = spdk_xor_gen(dest, bufs2, 2, BUF_SIZE);
	CU_ASSERT(ret == 0);
	ret = memcmp(ref, dest, BUF_SIZE);
	CU_ASSERT(ret == 0);

	/* cleanup */
	for (i = 0; i < BUF_COUNT; i++) {
		free(bufs[i]);
	}
	free(ref);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("xor", NULL, NULL);

	CU_ADD_TEST(suite, test_xor_gen);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
