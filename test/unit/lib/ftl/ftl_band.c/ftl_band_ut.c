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

#include "ftl/ftl_core.c"
#include "ftl/ftl_band.c"
#include "../common/utils.c"

#define TEST_BAND_IDX		68
#define TEST_LBA		0x68676564

static struct spdk_ocssd_geometry_data g_geo = {
	.num_grp	= 4,
	.num_pu		= 3,
	.num_chk	= 1500,
	.clba		= 100,
	.ws_opt		= 16,
	.ws_min		= 4,
};

static struct spdk_ftl_punit_range g_range = {
	.begin		= 2,
	.end		= 9,
};

static struct spdk_ftl_dev		*g_dev;
static struct ftl_band	*g_band;

static void
setup_band(void)
{
	int rc;

	g_dev = test_init_ftl_dev(&g_geo, &g_range);
	g_band = test_init_ftl_band(g_dev, TEST_BAND_IDX);
	printf("pool %p", g_dev->lba_pool);

	rc = ftl_band_alloc_lba_map(g_band);
	CU_ASSERT_EQUAL_FATAL(rc, 0);
}

static void
cleanup_band(void)
{
	test_free_ftl_band(g_band);
	test_free_ftl_dev(g_dev);
}

static struct ftl_ppa
ppa_from_punit(uint64_t punit)
{
	struct ftl_ppa ppa = {};

	ppa.grp = punit % g_geo.num_grp;
	ppa.pu = punit / g_geo.num_grp;
	return ppa;
}

static void
test_band_lbkoff_from_ppa_base(void)
{
	struct ftl_ppa ppa;
	uint64_t offset, i, flat_lun = 0;

	setup_band();
	for (i = g_range.begin; i < g_range.end; ++i) {
		ppa = ppa_from_punit(i);
		ppa.chk = TEST_BAND_IDX;

		offset = ftl_band_lbkoff_from_ppa(g_band, ppa);
		CU_ASSERT_EQUAL(offset, flat_lun * ftl_dev_lbks_in_chunk(g_dev));
		flat_lun++;
	}
	cleanup_band();
}

static void
test_band_lbkoff_from_ppa_lbk(void)
{
	struct ftl_ppa ppa;
	uint64_t offset, expect, i, j;

	setup_band();
	for (i = g_range.begin; i < g_range.end; ++i) {
		for (j = 0; j < g_geo.clba; ++j) {
			ppa = ppa_from_punit(i);
			ppa.chk = TEST_BAND_IDX;
			ppa.lbk = j;

			offset = ftl_band_lbkoff_from_ppa(g_band, ppa);

			expect = test_offset_from_ppa(ppa, g_band);
			CU_ASSERT_EQUAL(offset, expect);
		}
	}
	cleanup_band();
}

static void
test_band_ppa_from_lbkoff(void)
{
	struct ftl_ppa ppa, expect;
	uint64_t offset, i, j;

	setup_band();
	for (i = g_range.begin; i < g_range.end; ++i) {
		for (j = 0; j < g_geo.clba; ++j) {
			expect = ppa_from_punit(i);
			expect.chk = TEST_BAND_IDX;
			expect.lbk = j;

			offset = ftl_band_lbkoff_from_ppa(g_band, expect);
			ppa = ftl_band_ppa_from_lbkoff(g_band, offset);

			CU_ASSERT_EQUAL(ppa.ppa, expect.ppa);
		}
	}
	cleanup_band();
}

static void
test_band_set_addr(void)
{
	struct ftl_lba_map *lba_map;
	struct ftl_ppa ppa;
	uint64_t offset = 0;

	setup_band();
	lba_map = &g_band->lba_map;
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;

	CU_ASSERT_EQUAL(lba_map->num_vld, 0);

	offset = test_offset_from_ppa(ppa, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, ppa);
	CU_ASSERT_EQUAL(lba_map->num_vld, 1);
	CU_ASSERT_EQUAL(lba_map->map[offset], TEST_LBA);
	CU_ASSERT_TRUE(spdk_bit_array_get(lba_map->vld, offset));

	ppa.pu++;
	offset = test_offset_from_ppa(ppa, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, ppa);
	CU_ASSERT_EQUAL(lba_map->num_vld, 2);
	CU_ASSERT_EQUAL(lba_map->map[offset], TEST_LBA + 1);
	CU_ASSERT_TRUE(spdk_bit_array_get(lba_map->vld, offset));
	ppa.pu--;
	offset = test_offset_from_ppa(ppa, g_band);
	CU_ASSERT_TRUE(spdk_bit_array_get(lba_map->vld, offset));
	cleanup_band();
}

static void
test_invalidate_addr(void)
{
	struct ftl_lba_map *lba_map;
	struct ftl_ppa ppa;
	uint64_t offset[2];

	setup_band();
	lba_map = &g_band->lba_map;
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	offset[0] = test_offset_from_ppa(ppa, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, ppa);
	CU_ASSERT_EQUAL(lba_map->num_vld, 1);
	CU_ASSERT_TRUE(spdk_bit_array_get(lba_map->vld, offset[0]));
	ftl_invalidate_addr(g_band->dev, ppa);
	CU_ASSERT_EQUAL(lba_map->num_vld, 0);
	CU_ASSERT_FALSE(spdk_bit_array_get(lba_map->vld, offset[0]));

	offset[0] = test_offset_from_ppa(ppa, g_band);
	ftl_band_set_addr(g_band, TEST_LBA, ppa);
	ppa.pu++;
	offset[1] = test_offset_from_ppa(ppa, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, ppa);
	CU_ASSERT_EQUAL(lba_map->num_vld, 2);
	CU_ASSERT_TRUE(spdk_bit_array_get(lba_map->vld, offset[0]));
	CU_ASSERT_TRUE(spdk_bit_array_get(lba_map->vld, offset[1]));
	ftl_invalidate_addr(g_band->dev, ppa);
	CU_ASSERT_EQUAL(lba_map->num_vld, 1);
	CU_ASSERT_TRUE(spdk_bit_array_get(lba_map->vld, offset[0]));
	CU_ASSERT_FALSE(spdk_bit_array_get(lba_map->vld, offset[1]));
	cleanup_band();
}

static void
test_next_xfer_ppa(void)
{
	struct ftl_ppa ppa, result, expect;

	setup_band();
	/* Verify simple one lbk incremention */
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	ppa.lbk = 0;
	expect = ppa;
	expect.lbk = 1;

	result = ftl_band_next_xfer_ppa(g_band, ppa, 1);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping between chunks */
	expect = ppa_from_punit(g_range.begin + 1);
	expect.chk = TEST_BAND_IDX;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping works with unaligned offsets */
	expect = ppa_from_punit(g_range.begin + 1);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = 3;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size + 3);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping from last chunk to the first one */
	expect = ppa_from_punit(g_range.begin);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size;
	ppa = ppa_from_punit(g_range.end);
	ppa.chk = TEST_BAND_IDX;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify jumping from last chunk to the first one with unaligned offset */
	expect = ppa_from_punit(g_range.begin);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size + 2;
	ppa = ppa_from_punit(g_range.end);
	ppa.chk = TEST_BAND_IDX;
	result = ftl_band_next_xfer_ppa(g_band, ppa, g_dev->xfer_size + 2);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Verify large offset spanning across the whole band multiple times */
	expect = ppa_from_punit(g_range.begin);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size * 5 + 4;
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	ppa.lbk = g_dev->xfer_size * 2 + 1;
	result = ftl_band_next_xfer_ppa(g_band, ppa, 3 * g_dev->xfer_size *
					ftl_dev_num_punits(g_dev) + 3);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);

	/* Remove one chunk and verify it's skipped properly */
	g_band->chunk_buf[1].state = FTL_CHUNK_STATE_BAD;
	CIRCLEQ_REMOVE(&g_band->chunks, &g_band->chunk_buf[1], circleq);
	g_band->num_chunks--;
	expect = ppa_from_punit(g_range.begin + 2);
	expect.chk = TEST_BAND_IDX;
	expect.lbk = g_dev->xfer_size * 5 + 4;
	ppa = ppa_from_punit(g_range.begin);
	ppa.chk = TEST_BAND_IDX;
	ppa.lbk = g_dev->xfer_size * 2 + 1;
	result = ftl_band_next_xfer_ppa(g_band, ppa, 3 * g_dev->xfer_size *
					(ftl_dev_num_punits(g_dev) - 1) + g_dev->xfer_size + 3);
	CU_ASSERT_EQUAL(result.ppa, expect.ppa);
	cleanup_band();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ftl_band_suite", NULL, NULL);
	if (!suite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_band_lbkoff_from_ppa_base",
			    test_band_lbkoff_from_ppa_base) == NULL
		|| CU_add_test(suite, "test_band_lbkoff_from_ppa_lbk",
			       test_band_lbkoff_from_ppa_lbk) == NULL
		|| CU_add_test(suite, "test_band_ppa_from_lbkoff",
			       test_band_ppa_from_lbkoff) == NULL
		|| CU_add_test(suite, "test_band_set_addr",
			       test_band_set_addr) == NULL
		|| CU_add_test(suite, "test_invalidate_addr",
			       test_invalidate_addr) == NULL
		|| CU_add_test(suite, "test_next_xfer_ppa",
			       test_next_xfer_ppa) == NULL
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
