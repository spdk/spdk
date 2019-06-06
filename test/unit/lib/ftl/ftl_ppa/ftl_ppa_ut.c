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

static struct spdk_ftl_dev *
test_alloc_dev(size_t size)
{
	struct spdk_ftl_dev *dev;

	dev = calloc(1, sizeof(*dev));

	dev->num_lbas = L2P_TABLE_SIZE;
	dev->l2p = calloc(L2P_TABLE_SIZE, size);

	return dev;
}

static void
clean_l2p(void)
{
	size_t l2p_elem_size;

	if (ftl_ppa_packed(g_dev)) {
		l2p_elem_size = sizeof(uint32_t);
	} else {
		l2p_elem_size = sizeof(uint64_t);
	}
	memset(g_dev->l2p, 0, g_dev->num_lbas * l2p_elem_size);
}

static int
setup_l2p_32bit(void)
{
	g_dev = test_alloc_dev(sizeof(uint32_t));
	g_dev->ppaf.lbk_offset	= 0;
	g_dev->ppaf.lbk_mask	= (1 << 8) - 1;
	g_dev->ppaf.chk_offset	= 8;
	g_dev->ppaf.chk_mask	= (1 << 4) - 1;
	g_dev->ppaf.pu_offset	= g_dev->ppaf.chk_offset + 4;
	g_dev->ppaf.pu_mask	= (1 << 3) - 1;
	g_dev->ppaf.grp_offset	= g_dev->ppaf.pu_offset + 3;
	g_dev->ppaf.grp_mask	= (1 << 2) - 1;
	g_dev->ppa_len		= g_dev->ppaf.grp_offset + 2;

	return 0;
}

static int
setup_l2p_64bit(void)
{
	g_dev = test_alloc_dev(sizeof(uint64_t));
	g_dev->ppaf.lbk_offset	= 0;
	g_dev->ppaf.lbk_mask	= (1UL << 31) - 1;
	g_dev->ppaf.chk_offset	= 31;
	g_dev->ppaf.chk_mask	= (1 << 4) - 1;
	g_dev->ppaf.pu_offset	= g_dev->ppaf.chk_offset + 4;
	g_dev->ppaf.pu_mask	= (1 << 3) - 1;
	g_dev->ppaf.grp_offset	= g_dev->ppaf.pu_offset + 3;
	g_dev->ppaf.grp_mask	= (1 << 2) - 1;
	g_dev->ppa_len		= g_dev->ppaf.grp_offset + 2;

	return 0;
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
test_ppa_pack32(void)
{
	struct ftl_ppa orig = {}, ppa;

	/* Check valid address transformation */
	orig.lbk = 4;
	orig.chk = 3;
	orig.pu = 2;
	orig.grp = 1;
	ppa = ftl_ppa_to_packed(g_dev, orig);
	CU_ASSERT_TRUE(ppa.ppa <= UINT32_MAX);
	CU_ASSERT_FALSE(ppa.pack.cached);
	ppa = ftl_ppa_from_packed(g_dev, ppa);
	CU_ASSERT_FALSE(ftl_ppa_invalid(ppa));
	CU_ASSERT_EQUAL(ppa.ppa, orig.ppa);

	/* Check invalid address transformation */
	orig = ftl_to_ppa(FTL_PPA_INVALID);
	ppa = ftl_ppa_to_packed(g_dev, orig);
	CU_ASSERT_TRUE(ppa.ppa <= UINT32_MAX);
	ppa = ftl_ppa_from_packed(g_dev, ppa);
	CU_ASSERT_TRUE(ftl_ppa_invalid(ppa));

	/* Check cached entry offset transformation */
	orig.cached = 1;
	orig.offset = 1024;
	ppa = ftl_ppa_to_packed(g_dev, orig);
	CU_ASSERT_TRUE(ppa.ppa <= UINT32_MAX);
	CU_ASSERT_TRUE(ppa.pack.cached);
	ppa = ftl_ppa_from_packed(g_dev, ppa);
	CU_ASSERT_FALSE(ftl_ppa_invalid(ppa));
	CU_ASSERT_TRUE(ftl_ppa_cached(ppa));
	CU_ASSERT_EQUAL(ppa.ppa, orig.ppa);
	clean_l2p();
}

static void
test_ppa_pack64(void)
{
	struct ftl_ppa orig = {}, ppa;

	orig.lbk = 4;
	orig.chk = 3;
	orig.pu = 2;
	orig.grp = 1;

	/* Check valid address transformation */
	ppa.ppa = ftl_ppa_addr_pack(g_dev, orig);
	ppa = ftl_ppa_addr_unpack(g_dev, ppa.ppa);
	CU_ASSERT_FALSE(ftl_ppa_invalid(ppa));
	CU_ASSERT_EQUAL(ppa.ppa, orig.ppa);

	orig.lbk = 0x7ea0be0f;
	orig.chk = 0x6;
	orig.pu = 0x4;
	orig.grp = 0x2;

	ppa.ppa = ftl_ppa_addr_pack(g_dev, orig);
	ppa = ftl_ppa_addr_unpack(g_dev, ppa.ppa);
	CU_ASSERT_FALSE(ftl_ppa_invalid(ppa));
	CU_ASSERT_EQUAL(ppa.ppa, orig.ppa);

	/* Check maximum valid address for ppaf */
	orig.lbk = 0x7fffffff;
	orig.chk = 0xf;
	orig.pu = 0x7;
	orig.grp = 0x3;

	ppa.ppa = ftl_ppa_addr_pack(g_dev, orig);
	ppa = ftl_ppa_addr_unpack(g_dev, ppa.ppa);
	CU_ASSERT_FALSE(ftl_ppa_invalid(ppa));
	CU_ASSERT_EQUAL(ppa.ppa, orig.ppa);
	clean_l2p();
}

static void
test_ppa_trans(void)
{
	struct ftl_ppa ppa = {}, orig = {};
	size_t i;

	for (i = 0; i < L2P_TABLE_SIZE; ++i) {
		ppa.lbk = i % (g_dev->ppaf.lbk_mask + 1);
		ppa.chk = i % (g_dev->ppaf.chk_mask + 1);
		ppa.pu = i % (g_dev->ppaf.pu_mask + 1);
		ppa.grp = i % (g_dev->ppaf.grp_mask + 1);
		ftl_l2p_set(g_dev, i, ppa);
	}

	for (i = 0; i < L2P_TABLE_SIZE; ++i) {
		orig.lbk = i % (g_dev->ppaf.lbk_mask + 1);
		orig.chk = i % (g_dev->ppaf.chk_mask + 1);
		orig.pu = i % (g_dev->ppaf.pu_mask + 1);
		orig.grp = i % (g_dev->ppaf.grp_mask + 1);
		ppa = ftl_l2p_get(g_dev, i);
		CU_ASSERT_EQUAL(ppa.ppa, orig.ppa);
	}
	clean_l2p();
}

static void
test_ppa_invalid(void)
{
	struct ftl_ppa ppa;
	size_t i;

	/* Set every other LBA as invalid */
	for (i = 0; i < L2P_TABLE_SIZE; i += 2) {
		ftl_l2p_set(g_dev, i, ftl_to_ppa(FTL_PPA_INVALID));
	}

	/* Check every even LBA is invalid while others are fine */
	for (i = 0; i < L2P_TABLE_SIZE; ++i) {
		ppa = ftl_l2p_get(g_dev, i);

		if (i % 2 == 0) {
			CU_ASSERT_TRUE(ftl_ppa_invalid(ppa));
		} else {
			CU_ASSERT_FALSE(ftl_ppa_invalid(ppa));
		}
	}
	clean_l2p();
}

static void
test_ppa_cached(void)
{
	struct ftl_ppa ppa;
	size_t i;

	/* Set every other LBA is cached */
	for (i = 0; i < L2P_TABLE_SIZE; i += 2) {
		ppa.cached = 1;
		ppa.offset = i;
		ftl_l2p_set(g_dev, i, ppa);
	}

	/* Check every even LBA is cached while others are not */
	for (i = 0; i < L2P_TABLE_SIZE; ++i) {
		ppa = ftl_l2p_get(g_dev, i);

		if (i % 2 == 0) {
			CU_ASSERT_TRUE(ftl_ppa_cached(ppa));
			CU_ASSERT_EQUAL(ppa.offset, i);
		} else {
			CU_ASSERT_FALSE(ftl_ppa_cached(ppa));
		}
	}
	clean_l2p();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite32 = NULL, suite64 = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite32 = CU_add_suite("ftl_ppa32_suite", setup_l2p_32bit, cleanup);
	if (!suite32) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	suite64 = CU_add_suite("ftl_ppa64_suite", setup_l2p_64bit, cleanup);
	if (!suite64) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite32, "test_ppa_pack",
			    test_ppa_pack32) == NULL
		|| CU_add_test(suite32, "test_ppa32_invalid",
			       test_ppa_invalid) == NULL
		|| CU_add_test(suite32, "test_ppa32_trans",
			       test_ppa_trans) == NULL
		|| CU_add_test(suite32, "test_ppa32_cached",
			       test_ppa_cached) == NULL
		|| CU_add_test(suite64, "test_ppa64_invalid",
			       test_ppa_invalid) == NULL
		|| CU_add_test(suite64, "test_ppa64_trans",
			       test_ppa_trans) == NULL
		|| CU_add_test(suite64, "test_ppa64_cached",
			       test_ppa_cached) == NULL
		|| CU_add_test(suite64, "test_ppa64_pack",
			       test_ppa_pack64) == NULL
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
