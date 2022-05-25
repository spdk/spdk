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
#define TEST_SEQ		0xDEADBEEF
#define G_GEO_ZONE_SIZE 10000
#define G_GEO_OPTIMAL_OPEN_ZONES 9

struct base_bdev_geometry g_geo = {
	.write_unit_size    = FTL_NUM_LBA_IN_BLOCK,
	.optimal_open_zones = G_GEO_OPTIMAL_OPEN_ZONES,
	.zone_size	    = G_GEO_ZONE_SIZE,
	.blockcnt	    = (TEST_BAND_IDX + 1) * G_GEO_ZONE_SIZE * G_GEO_OPTIMAL_OPEN_ZONES,
};

static struct spdk_ftl_dev *g_dev;
static struct ftl_band	*g_band;

#if defined(DEBUG)
DEFINE_STUB_V(ftl_band_validate_md, (struct ftl_band *band, ftl_band_validate_md_cb cb));
DEFINE_STUB_V(ftl_trace_limits, (struct spdk_ftl_dev *dev, int limit, size_t num_free));
DEFINE_STUB_V(ftl_trace_completion, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     enum ftl_trace_completion completion));
DEFINE_STUB_V(ftl_trace_defrag_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_wbuf_fill, (struct spdk_ftl_dev *dev, const struct ftl_io *io));
DEFINE_STUB_V(ftl_trace_wbuf_pop, (struct spdk_ftl_dev *dev, const struct ftl_wbuf_entry *entry));
DEFINE_STUB_V(ftl_trace_write_band, (struct spdk_ftl_dev *dev, const struct ftl_band *band));
DEFINE_STUB_V(ftl_trace_submission, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     ftl_addr addr, size_t addr_cnt));
#endif
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 512);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test");
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_media_events, size_t,
	    (struct spdk_bdev_desc *bdev_desc, struct spdk_bdev_media_event *events,
	     size_t max_events), 0);
DEFINE_STUB(spdk_bdev_get_md_size, uint32_t, (const struct spdk_bdev *bdev), 8);
DEFINE_STUB(spdk_bdev_io_get_append_location, uint64_t, (struct spdk_bdev_io *bdev_io), 0);
DEFINE_STUB(spdk_bdev_write_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_write_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, void *buf, void *md, uint64_t offset_blocks,
		uint64_t num_blocks, spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_read_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_write_zeroes_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_zone_appendv, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t zone_id, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_zone_management, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		uint64_t zone_id, enum spdk_bdev_zone_action action,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);

DEFINE_STUB_V(ftl_io_advance, (struct ftl_io *io, size_t num_blocks));
DEFINE_STUB_V(ftl_io_call_foreach_child,
	      (struct ftl_io *io, void (*callback)(struct ftl_io *)));
DEFINE_STUB(ftl_io_channel_get_ctx, struct ftl_io_channel *,
	    (struct spdk_io_channel *ioch), NULL);
DEFINE_STUB_V(ftl_io_complete, (struct ftl_io *io));
DEFINE_STUB(ftl_io_current_lba, uint64_t, (const struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_dec_req, (struct ftl_io *io));
DEFINE_STUB(ftl_io_erase_init, struct ftl_io *,
	    (struct ftl_band *band, size_t num_blocks, ftl_io_fn cb), NULL);
DEFINE_STUB_V(ftl_io_fail, (struct ftl_io *io, int status));
DEFINE_STUB_V(ftl_io_free, (struct ftl_io *io));
DEFINE_STUB(ftl_io_get_lba, uint64_t,
	    (const struct ftl_io *io, size_t offset), 0);
DEFINE_STUB_V(ftl_io_inc_req, (struct ftl_io *io));
DEFINE_STUB(ftl_io_init_internal, struct ftl_io *,
	    (const struct ftl_io_init_opts *opts), NULL);
DEFINE_STUB_V(ftl_io_reset, (struct ftl_io *io));
DEFINE_STUB(ftl_io_iovec_addr, void *, (struct ftl_io *io), NULL);
DEFINE_STUB(ftl_io_iovec_len_left, size_t, (struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_shrink_iovec, (struct ftl_io *io, size_t num_blocks));

DEFINE_STUB(ftl_io_user_init, int,
	    (struct spdk_io_channel *ioch, struct ftl_io *io, uint64_t lba,
	     size_t num_blocks, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
	     void *cb_arg, int type), 0);

DEFINE_STUB(ftl_iovec_num_blocks, size_t,
	    (struct iovec *iov, size_t iov_cnt), 0);
DEFINE_STUB_V(ftl_reloc, (struct ftl_reloc *reloc));

DEFINE_STUB_V(ftl_reloc_add, (struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
			      size_t num_blocks, int prio, bool defrag));
DEFINE_STUB(ftl_reloc_is_defrag_active, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB(ftl_reloc_is_halted, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB(spdk_bdev_is_zoned, bool, (const struct spdk_bdev *bdev), true);
DEFINE_STUB(ftl_p2l_ckpt_acquire, struct ftl_p2l_ckpt *, (struct spdk_ftl_dev *dev), NULL);
DEFINE_STUB(ftl_mngt_unmap, int, (struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks,
				  spdk_ftl_fn cb, void *cb_cntx), 0);
DEFINE_STUB_V(ftl_p2l_ckpt_release, (struct spdk_ftl_dev *dev, struct ftl_p2l_ckpt *ckpt));

#ifdef SPDK_CONFIG_PMDK
DEFINE_STUB_V(pmem_persist, (const void *addr, size_t len));
#endif

static void adjust_bitmap(struct ftl_bitmap **bitmap, uint64_t *bit)
{
	if (*bitmap == g_dev->valid_map) {
		*bitmap = g_band->lba_map.vld;
		*bit = test_offset_from_addr(*bit, g_band);
	}
}

bool ftl_bitmap_get(const struct ftl_bitmap *bitmap, uint64_t bit)
{
	adjust_bitmap((struct ftl_bitmap **)&bitmap, &bit);
	return spdk_bit_array_get((struct spdk_bit_array *)bitmap, bit);
}

void ftl_bitmap_set(struct ftl_bitmap *bitmap, uint64_t bit)
{
	int ret;

	adjust_bitmap(&bitmap, &bit);
	ret = spdk_bit_array_set((struct spdk_bit_array *)bitmap, bit);
	CU_ASSERT_EQUAL(ret, 0);
}

void ftl_bitmap_clear(struct ftl_bitmap *bitmap, uint64_t bit)
{
	adjust_bitmap(&bitmap, &bit);
	spdk_bit_array_clear((struct spdk_bit_array *)bitmap, bit);
}

static void
setup_band(void)
{
	int rc;

	g_dev = test_init_ftl_dev(&g_geo);
	g_band = test_init_ftl_band(g_dev, TEST_BAND_IDX, g_geo.zone_size);
	rc = ftl_band_alloc_lba_map(g_band);
	CU_ASSERT_EQUAL_FATAL(rc, 0);
}

static void
cleanup_band(void)
{
	test_free_ftl_band(g_band);
	test_free_ftl_dev(g_dev);
}

static ftl_addr
addr_from_punit(uint64_t punit)
{
	ftl_addr addr;

	addr = punit * g_geo.zone_size;
	return addr;
}

static void
test_band_block_offset_from_addr_base(void)
{
	ftl_addr addr;
	uint64_t offset, i, flat_lun = 0;

	setup_band();
	for (i = 0; i < ftl_get_num_punits(g_dev); ++i) {
		addr = addr_from_punit(i);
		addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);

		offset = ftl_band_block_offset_from_addr(g_band, addr);
		CU_ASSERT_EQUAL(offset, flat_lun * ftl_get_num_blocks_in_zone(g_dev));
		flat_lun++;
	}
	cleanup_band();
}

static void
test_band_block_offset_from_addr_offset(void)
{
	ftl_addr addr;
	uint64_t offset, expect, i, j;

	setup_band();
	for (i = 0; i < ftl_get_num_punits(g_dev); ++i) {
		for (j = 0; j < g_geo.zone_size; ++j) {
			addr = addr_from_punit(i);
			addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev) + j;

			offset = ftl_band_block_offset_from_addr(g_band, addr);

			expect = test_offset_from_addr(addr, g_band);
			CU_ASSERT_EQUAL(offset, expect);
		}
	}
	cleanup_band();
}

static void
test_band_addr_from_block_offset(void)
{
	ftl_addr addr, expect;
	uint64_t offset, i, j;

	setup_band();
	for (i = 0; i < ftl_get_num_punits(g_dev); ++i) {
		for (j = 0; j < g_geo.zone_size; ++j) {
			expect = addr_from_punit(i);
			expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev) + j;

			offset = ftl_band_block_offset_from_addr(g_band, expect);
			addr = ftl_band_addr_from_block_offset(g_band, offset);

			CU_ASSERT_EQUAL(addr, expect);
		}
	}
	cleanup_band();
}

static void
test_band_set_addr(void)
{
	struct ftl_lba_map *lba_map;
	ftl_addr addr;
	uint64_t offset = 0;

	setup_band();
	lba_map = &g_band->lba_map;
	addr = addr_from_punit(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);

	CU_ASSERT_EQUAL(lba_map->num_vld, 0);

	offset = test_offset_from_addr(addr, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, addr);
	ftl_band_set_p2l(g_band, TEST_LBA, addr, TEST_SEQ);
	CU_ASSERT_EQUAL(lba_map->num_vld, 1);
	CU_ASSERT_EQUAL(lba_map->band_map[offset].lba, TEST_LBA);
	CU_ASSERT_EQUAL(lba_map->band_map[offset].seq_id, TEST_SEQ);
	CU_ASSERT_TRUE(ftl_bitmap_get(lba_map->vld, offset));

	addr += g_geo.zone_size;
	offset = test_offset_from_addr(addr, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, addr);
	ftl_band_set_p2l(g_band, TEST_LBA + 1, addr, TEST_SEQ + 1);
	CU_ASSERT_EQUAL(lba_map->num_vld, 2);
	CU_ASSERT_EQUAL(lba_map->band_map[offset].lba, TEST_LBA + 1);
	CU_ASSERT_EQUAL(lba_map->band_map[offset].seq_id, TEST_SEQ + 1);
	CU_ASSERT_TRUE(ftl_bitmap_get(lba_map->vld, offset));
	addr -= g_geo.zone_size;
	offset = test_offset_from_addr(addr, g_band);
	CU_ASSERT_TRUE(ftl_bitmap_get(lba_map->vld, offset));
	cleanup_band();
}

static void
test_invalidate_addr(void)
{
	struct ftl_lba_map *lba_map;
	ftl_addr addr;
	uint64_t offset[2];

	setup_band();
	lba_map = &g_band->lba_map;
	addr = addr_from_punit(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	offset[0] = test_offset_from_addr(addr, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, addr);
	ftl_band_set_p2l(g_band, TEST_LBA, addr, TEST_SEQ);
	CU_ASSERT_EQUAL(lba_map->num_vld, 1);
	CU_ASSERT_TRUE(ftl_bitmap_get(lba_map->vld, offset[0]));
	ftl_invalidate_addr(g_band->dev, addr);
	CU_ASSERT_EQUAL(lba_map->num_vld, 0);
	CU_ASSERT_FALSE(ftl_bitmap_get(lba_map->vld, offset[0]));

	offset[0] = test_offset_from_addr(addr, g_band);
	ftl_band_set_addr(g_band, TEST_LBA, addr);
	ftl_band_set_p2l(g_band, TEST_LBA, addr, TEST_SEQ);
	addr += g_geo.zone_size;
	offset[1] = test_offset_from_addr(addr, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, addr);
	ftl_band_set_p2l(g_band, TEST_LBA + 1, addr, TEST_SEQ);
	CU_ASSERT_EQUAL(lba_map->num_vld, 2);
	CU_ASSERT_TRUE(ftl_bitmap_get(lba_map->vld, offset[0]));
	CU_ASSERT_TRUE(ftl_bitmap_get(lba_map->vld, offset[1]));
	ftl_invalidate_addr(g_band->dev, addr);
	CU_ASSERT_EQUAL(lba_map->num_vld, 1);
	CU_ASSERT_TRUE(ftl_bitmap_get(lba_map->vld, offset[0]));
	CU_ASSERT_FALSE(ftl_bitmap_get(lba_map->vld, offset[1]));
	cleanup_band();
}

static void
test_next_xfer_addr(void)
{
	ftl_addr addr, result, expect;

	setup_band();
	/* Verify simple one block incremention */
	addr = addr_from_punit(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	expect = addr;
	expect += 1;

	result = ftl_band_next_xfer_addr(g_band, addr, 1);
	CU_ASSERT_EQUAL(result, expect);

	/* Verify jumping between zones */
	expect = addr_from_punit(1);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	result = ftl_band_next_xfer_addr(g_band, addr, g_dev->xfer_size);
	CU_ASSERT_EQUAL(result, expect);

	/* Verify jumping works with unaligned offsets */
	expect = addr_from_punit(1);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev) + 3;
	result = ftl_band_next_xfer_addr(g_band, addr, g_dev->xfer_size + 3);
	CU_ASSERT_EQUAL(result, expect);

	/* Verify jumping from last zone to the first one */
	expect = addr_from_punit(0);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev) + g_dev->xfer_size;
	addr = addr_from_punit(ftl_get_num_punits(g_dev) - 1);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	result = ftl_band_next_xfer_addr(g_band, addr, g_dev->xfer_size);
	CU_ASSERT_EQUAL(result, expect);

	/* Verify jumping from last zone to the first one with unaligned offset */
	expect = addr_from_punit(0);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	expect += g_dev->xfer_size + 2;
	addr = addr_from_punit(ftl_get_num_punits(g_dev) - 1);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	result = ftl_band_next_xfer_addr(g_band, addr, g_dev->xfer_size + 2);
	CU_ASSERT_EQUAL(result, expect);

	/* Verify large offset spanning across the whole band multiple times */
	expect = addr_from_punit(0);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	expect += g_dev->xfer_size * 5 + 4;
	addr = addr_from_punit(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	addr += g_dev->xfer_size * 2 + 1;
	result = ftl_band_next_xfer_addr(g_band, addr, 3 * g_dev->xfer_size *
					 ftl_get_num_punits(g_dev) + 3);
	CU_ASSERT_EQUAL(result, expect);

	/* Remove one zone and verify it's skipped properly */
	g_band->zone_buf[1].info.state = SPDK_BDEV_ZONE_STATE_OFFLINE;
	CIRCLEQ_REMOVE(&g_band->zones, &g_band->zone_buf[1], circleq);
	g_band->num_zones--;
	expect = addr_from_punit(2);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	expect += g_dev->xfer_size * 5 + 4;
	addr = addr_from_punit(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	addr += g_dev->xfer_size * 2 + 1;
	result = ftl_band_next_xfer_addr(g_band, addr, 3 * g_dev->xfer_size *
					 (ftl_get_num_punits(g_dev) - 1) + g_dev->xfer_size + 3);
	CU_ASSERT_EQUAL(result, expect);
	cleanup_band();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_band_suite", NULL, NULL);


	CU_ADD_TEST(suite, test_band_block_offset_from_addr_base);
	CU_ADD_TEST(suite, test_band_block_offset_from_addr_offset);
	CU_ADD_TEST(suite, test_band_addr_from_block_offset);
	CU_ADD_TEST(suite, test_band_set_addr);
	CU_ADD_TEST(suite, test_invalidate_addr);
	CU_ADD_TEST(suite, test_next_xfer_addr);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
