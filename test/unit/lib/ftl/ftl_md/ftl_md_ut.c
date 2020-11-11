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

#if defined(DEBUG)
DEFINE_STUB(ftl_band_validate_md, bool, (struct ftl_band *band), true);
#endif
DEFINE_STUB_V(ftl_apply_limits, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_io_init_internal, struct ftl_io *,
	    (const struct ftl_io_init_opts *opts), NULL);
DEFINE_STUB_V(ftl_io_read, (struct ftl_io *io));
DEFINE_STUB_V(ftl_io_write, (struct ftl_io *io));
DEFINE_STUB_V(ftl_reloc_add, (struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
			      size_t num_blocks, int prio, bool defrag));

struct base_bdev_geometry g_geo = {
	.write_unit_size    = 16,
	.optimal_open_zones = 12,
	.zone_size	    = 100,
	.blockcnt	    = 1500 * 100 * 12,
};

static void
setup_band(struct ftl_band **band, const struct base_bdev_geometry *geo)
{
	int rc;
	struct spdk_ftl_dev *dev;

	dev = test_init_ftl_dev(&g_geo);
	*band = test_init_ftl_band(dev, 0, geo->zone_size);
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

	setup_band(&band, &g_geo);

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

	setup_band(&band, &g_geo);

	lba_map = &band->lba_map;
	SPDK_CU_ASSERT_FATAL(lba_map->dma_buf);

	/* check crc */
	ftl_pack_tail_md(band);
	/* flip last bit of lba_map */
	*((char *)lba_map->dma_buf + ftl_tail_md_num_blocks(band->dev) * FTL_BLOCK_SIZE - 1) ^= 0x1;
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
	g_geo.zone_size--;
	CU_ASSERT_EQUAL(ftl_unpack_tail_md(band), FTL_MD_INVALID_SIZE);

	cleanup_band(band);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_meta_suite", NULL, NULL);


	CU_ADD_TEST(suite, test_md_unpack);
	CU_ADD_TEST(suite, test_md_unpack_fail);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
