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

#include "ftl/ftl_band.c"
#include "../common/utils.c"

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

static void
setup_band(struct ftl_band **band, const struct spdk_ocssd_geometry_data *geo,
	   const struct spdk_ftl_punit_range *range)
{
	int rc;
	struct spdk_ftl_dev *dev;

	dev = test_init_ftl_dev(geo, range);
	*band = test_init_ftl_band(dev, 0);
	rc = ftl_band_alloc_lba_map(*band);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	(*band)->state = FTL_BAND_STATE_PREP;
	ftl_band_clear_lba_map(*band);
}

static void
cleanup_band(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	test_free_ftl_band(band);
	test_free_ftl_dev(dev);
}

static void
test_md_unpack(void)
{
	struct ftl_band *band;
	struct ftl_lba_map *lba_map;

	setup_band(&band, &g_geo, &g_range);

	lba_map = &band->lba_map;
	SPDK_CU_ASSERT_FATAL(lba_map->dma_buf);

	ftl_pack_head_md(band);
	CU_ASSERT_EQUAL(ftl_unpack_head_md(band), FTL_MD_SUCCESS);

	ftl_pack_tail_md(band);
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band), FTL_MD_SUCCESS);

	cleanup_band(band);
}

static void
test_md_unpack_fail(void)
{
	struct ftl_band *band;
	struct ftl_lba_map *lba_map;
	struct ftl_md_hdr *hdr;

	setup_band(&band, &g_geo, &g_range);

	lba_map = &band->lba_map;
	SPDK_CU_ASSERT_FATAL(lba_map->dma_buf);

	/* check crc */
	ftl_pack_tail_md(band);
	/* flip last bit of lba_map */
	*((char *)lba_map->dma_buf + ftl_tail_md_num_lbks(band->dev) * FTL_BLOCK_SIZE - 1) ^= 0x1;
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band), FTL_MD_INVALID_CRC);

	/* check invalid version */
	hdr = lba_map->dma_buf;
	ftl_pack_tail_md(band);
	hdr->ver++;
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band), FTL_MD_INVALID_VER);

	/* check wrong UUID */
	ftl_pack_head_md(band);
	hdr->uuid.u.raw[0] ^= 0x1;
	CU_ASSERT_EQUAL(ftl_unpack_head_md(band), FTL_MD_NO_MD);

	/* check invalid size */
	ftl_pack_tail_md(band);
	band->dev->geo.clba--;
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band), FTL_MD_INVALID_SIZE);

	cleanup_band(band);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("ftl_meta_suite", NULL, NULL);
	if (!suite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_md_unpack",
			    test_md_unpack) == NULL
		|| CU_add_test(suite, "test_md_unpack_fail",
			       test_md_unpack_fail) == NULL
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
