/*
 * INTEL CONFIDENTIAL
 *
 * Copyright 2018 Intel Corporation
 *
 * This software and the related documents are Intel copyrighted materials, and
 * your use of them is governed by the express license under which they were
 * provided to you (License). Unless the License provides otherwise, you may not
 * use, modify, copy, publish, distribute, disclose or transmit this software or
 * the related documents without Intel's prior written permission.
 * This software and the related documents are provided as is, with no express or
 * implied warranties, other than those that are expressly stated in the License.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_core.c"
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
setup_band(struct ftl_band **_band, const struct spdk_ocssd_geometry_data *geo,
	   const struct spdk_ftl_punit_range *range)
{
	int rc;
	struct spdk_ftl_dev *dev;
	struct ftl_band *band;

	dev = test_init_ftl_dev(geo, range);
	band = test_init_ftl_band(dev, 0);
	rc = ftl_band_alloc_md(band);
	CU_ASSERT_EQUAL_FATAL(rc, 0);

	*_band = band;
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
	struct ftl_md *md;
	void *data;

	setup_band(&band, &g_geo, &g_range);

	md = &band->md;

	data = malloc(ftl_tail_md_num_lbks(band->dev) * PAGE_SIZE);
	SPDK_CU_ASSERT_FATAL(data);

	ftl_pack_head_md(band->dev, md, data);
	CU_ASSERT_EQUAL(ftl_unpack_head_md(band->dev, md, data), FTL_MD_SUCCESS);

	ftl_pack_tail_md(band->dev, md, data);
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band->dev, md, data), FTL_MD_SUCCESS);

	free(data);
	cleanup_band(band);
}

static void
test_md_unpack_crc_fail(void)
{
	struct ftl_band *band;
	struct ftl_md *md;
	void *data;

	setup_band(&band, &g_geo, &g_range);

	md = &band->md;

	data = malloc(ftl_tail_md_num_lbks(band->dev) * PAGE_SIZE);
	SPDK_CU_ASSERT_FATAL(data);

	ftl_pack_tail_md(band->dev, md, data);

	/* flip last bit of lba_map */
	*((char *)data + ftl_tail_md_num_lbks(band->dev) * FTL_BLOCK_SIZE - 1) ^= 0x1;
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band->dev, md, data), FTL_MD_INVALID_CRC);

	free(data);
	cleanup_band(band);
}

static void
test_md_unpack_ver_fail(void)
{
	struct ftl_band *band;
	struct ftl_md *md;
	struct ftl_md_hdr *hdr;
	void *data;

	setup_band(&band, &g_geo, &g_range);

	md = &band->md;

	data = malloc(ftl_tail_md_num_lbks(band->dev) * PAGE_SIZE);
	SPDK_CU_ASSERT_FATAL(data);

	hdr = data;
	ftl_pack_tail_md(band->dev, md, data);

	hdr->ver++;
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band->dev, md, data), FTL_MD_INVALID_VER);

	free(data);
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
		|| CU_add_test(suite, "test_md_unpack_crc_fail",
			       test_md_unpack_crc_fail) == NULL
		|| CU_add_test(suite, "test_md_unpack_ver_fail",
			       test_md_unpack_ver_fail) == NULL
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
