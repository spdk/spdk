/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_core.h"

#define L2P_TABLE_SIZE 1024

static struct spdk_ftl_dev *g_dev;

static struct spdk_ftl_dev *
test_alloc_dev(size_t size)
{
	struct spdk_ftl_dev *dev;

	dev = calloc(1, sizeof(*dev));

	dev->num_lbas = L2P_TABLE_SIZE;
	dev->l2p = calloc(L2P_TABLE_SIZE, size);
	dev->layout.l2p.addr_size = size;

	if (size > sizeof(uint32_t)) {
		dev->layout.base.total_blocks = ~(~0ULL << 33);
	} else {
		dev->layout.base.total_blocks = 1024;
	}

	return dev;
}

static int
setup_l2p_64bit(void)
{
	g_dev = test_alloc_dev(sizeof(uint64_t));
	return 0;
}

static void
clean_l2p(void)
{
	size_t l2p_elem_size;

	if (ftl_addr_packed(g_dev)) {
		l2p_elem_size = sizeof(uint32_t);
	} else {
		l2p_elem_size = sizeof(uint64_t);
	}
	memset(g_dev->l2p, 0, g_dev->num_lbas * l2p_elem_size);
}

static int
cleanup(void)
{
	free(g_dev->l2p);
	free(g_dev);
	g_dev = NULL;
	return 0;
}

void
ftl_l2p_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	((uint64_t *)dev->l2p)[lba] = addr;
}

ftl_addr
ftl_l2p_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	return ((uint64_t *)dev->l2p)[lba];
}

static void
test_addr_cached(void)
{
	ftl_addr addr;
	size_t i;

	/* Set every other LBA is cached */
	for (i = 0; i < L2P_TABLE_SIZE; i += 2) {
		addr = ftl_addr_from_nvc_offset(g_dev, i);
		ftl_l2p_set(g_dev, i, addr);
	}

	/* Check every even LBA is cached while others are not */
	for (i = 0; i < L2P_TABLE_SIZE; ++i) {
		addr = ftl_l2p_get(g_dev, i);

		if (i % 2 == 0) {
			CU_ASSERT_TRUE(ftl_addr_in_nvc(g_dev, addr));
			CU_ASSERT_EQUAL(ftl_addr_to_nvc_offset(g_dev, addr), i);
		} else {
			CU_ASSERT_FALSE(ftl_addr_in_nvc(g_dev, addr));
		}
	}
	clean_l2p();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite64 = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite64 = CU_add_suite("ftl_addr64_suite", setup_l2p_64bit, cleanup);

	CU_ADD_TEST(suite64, test_addr_cached);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
