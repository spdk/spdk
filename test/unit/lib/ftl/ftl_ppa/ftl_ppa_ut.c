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

#include "ftl/ftl_core.h"

#define L2P_TABLE_SIZE 1024

static struct spdk_ftl_dev *g_dev;

DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);

uint64_t
spdk_bdev_get_zone_size(const struct spdk_bdev *bdev)
{
	if (g_dev->addr_len > 32) {
		return 1ULL << 32;
	}

	return 1024;
}

uint32_t
spdk_bdev_get_optimal_open_zones(const struct spdk_bdev *bdev)
{
	return 100;
}

static struct spdk_ftl_dev *
test_alloc_dev(size_t size)
{
	struct spdk_ftl_dev *dev;

	dev = calloc(1, sizeof(*dev));

	dev->num_lbas = L2P_TABLE_SIZE;
	dev->l2p = calloc(L2P_TABLE_SIZE, size);

	return dev;
}

static int
setup_l2p_32bit(void)
{
	g_dev = test_alloc_dev(sizeof(uint32_t));
	g_dev->addr_len = 24;
	return 0;
}

static int
setup_l2p_64bit(void)
{
	g_dev = test_alloc_dev(sizeof(uint64_t));
	g_dev->addr_len = 63;
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

static void
test_addr_pack32(void)
{
	struct ftl_addr orig = {}, addr;

	/* Check valid address transformation */
	orig.offset = 4;
	addr = ftl_addr_to_packed(g_dev, orig);
	CU_ASSERT_TRUE(addr.offset <= UINT32_MAX);
	CU_ASSERT_FALSE(addr.pack.cached);
	addr = ftl_addr_from_packed(g_dev, addr);
	CU_ASSERT_FALSE(ftl_addr_invalid(addr));
	CU_ASSERT_EQUAL(addr.offset, orig.offset);

	/* Check invalid address transformation */
	orig = ftl_to_addr(FTL_ADDR_INVALID);
	addr = ftl_addr_to_packed(g_dev, orig);
	CU_ASSERT_TRUE(addr.offset <= UINT32_MAX);
	addr = ftl_addr_from_packed(g_dev, addr);
	CU_ASSERT_TRUE(ftl_addr_invalid(addr));

	/* Check cached entry offset transformation */
	orig.cached = 1;
	orig.cache_offset = 1024;
	addr = ftl_addr_to_packed(g_dev, orig);
	CU_ASSERT_TRUE(addr.offset <= UINT32_MAX);
	CU_ASSERT_TRUE(addr.pack.cached);
	addr = ftl_addr_from_packed(g_dev, addr);
	CU_ASSERT_FALSE(ftl_addr_invalid(addr));
	CU_ASSERT_TRUE(ftl_addr_cached(addr));
	CU_ASSERT_EQUAL(addr.offset, orig.offset);
	clean_l2p();
}

static void
test_addr_invalid(void)
{
	struct ftl_addr addr;
	size_t i;

	/* Set every other LBA as invalid */
	for (i = 0; i < L2P_TABLE_SIZE; i += 2) {
		ftl_l2p_set(g_dev, i, ftl_to_addr(FTL_ADDR_INVALID));
	}

	/* Check every even LBA is invalid while others are fine */
	for (i = 0; i < L2P_TABLE_SIZE; ++i) {
		addr = ftl_l2p_get(g_dev, i);

		if (i % 2 == 0) {
			CU_ASSERT_TRUE(ftl_addr_invalid(addr));
		} else {
			CU_ASSERT_FALSE(ftl_addr_invalid(addr));
		}
	}
	clean_l2p();
}

static void
test_addr_cached(void)
{
	struct ftl_addr addr;
	size_t i;

	/* Set every other LBA is cached */
	for (i = 0; i < L2P_TABLE_SIZE; i += 2) {
		addr.cached = 1;
		addr.cache_offset = i;
		ftl_l2p_set(g_dev, i, addr);
	}

	/* Check every even LBA is cached while others are not */
	for (i = 0; i < L2P_TABLE_SIZE; ++i) {
		addr = ftl_l2p_get(g_dev, i);

		if (i % 2 == 0) {
			CU_ASSERT_TRUE(ftl_addr_cached(addr));
			CU_ASSERT_EQUAL(addr.cache_offset, i);
		} else {
			CU_ASSERT_FALSE(ftl_addr_cached(addr));
		}
	}
	clean_l2p();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite32 = NULL, suite64 = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite32 = CU_add_suite("ftl_addr32_suite", setup_l2p_32bit, cleanup);


	suite64 = CU_add_suite("ftl_addr64_suite", setup_l2p_64bit, cleanup);


	CU_ADD_TEST(suite32, test_addr_pack32);
	CU_ADD_TEST(suite32, test_addr_invalid);
	CU_ADD_TEST(suite32, test_addr_cached);
	CU_ADD_TEST(suite64, test_addr_invalid);
	CU_ADD_TEST(suite64, test_addr_cached);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
