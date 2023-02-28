/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_core.c"
#include "ftl/ftl_band.c"
#include "../common/utils.c"

#define TEST_BAND_IDX		42
#define TEST_LBA		0x68676564
#define TEST_SEQ		0xDEADBEEF
#define G_GEO_ZONE_SIZE 10000
#define G_GEO_OPTIMAL_OPEN_ZONES 1

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
DEFINE_STUB_V(ftl_p2l_validate_ckpt, (struct ftl_band *band));
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
DEFINE_STUB(spdk_bdev_get_md_size, uint32_t, (const struct spdk_bdev *bdev), 8);
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
DEFINE_STUB_V(ftl_io_advance, (struct ftl_io *io, size_t num_blocks));
DEFINE_STUB(ftl_io_channel_get_ctx, struct ftl_io_channel *,
	    (struct spdk_io_channel *ioch), NULL);
DEFINE_STUB_V(ftl_io_complete, (struct ftl_io *io));
DEFINE_STUB(ftl_io_current_lba, uint64_t, (const struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_dec_req, (struct ftl_io *io));
DEFINE_STUB_V(ftl_io_fail, (struct ftl_io *io, int status));
DEFINE_STUB_V(ftl_io_free, (struct ftl_io *io));
DEFINE_STUB(ftl_io_get_lba, uint64_t,
	    (const struct ftl_io *io, size_t offset), 0);
DEFINE_STUB_V(ftl_io_inc_req, (struct ftl_io *io));
DEFINE_STUB(ftl_io_iovec_addr, void *, (struct ftl_io *io), NULL);
DEFINE_STUB(ftl_io_iovec_len_left, size_t, (struct ftl_io *io), 0);

DEFINE_STUB(ftl_iovec_num_blocks, size_t,
	    (struct iovec *iov, size_t iov_cnt), 0);
DEFINE_STUB_V(ftl_reloc, (struct ftl_reloc *reloc));

DEFINE_STUB_V(ftl_reloc_add, (struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
			      size_t num_blocks, int prio, bool defrag));
DEFINE_STUB(ftl_reloc_is_defrag_active, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB(ftl_reloc_is_halted, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB_V(ftl_reloc_halt, (struct ftl_reloc *reloc));
DEFINE_STUB(spdk_bdev_is_zoned, bool, (const struct spdk_bdev *bdev), true);
DEFINE_STUB(ftl_p2l_ckpt_acquire, struct ftl_p2l_ckpt *, (struct spdk_ftl_dev *dev), NULL);
DEFINE_STUB(ftl_mngt_unmap, int, (struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks,
				  spdk_ftl_fn cb, void *cb_cntx), 0);
DEFINE_STUB_V(ftl_p2l_ckpt_release, (struct spdk_ftl_dev *dev, struct ftl_p2l_ckpt *ckpt));

DEFINE_STUB_V(ftl_l2p_process, (struct spdk_ftl_dev *dev));
DEFINE_STUB_V(ftl_nv_cache_process, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_nv_cache_is_halted, bool, (struct ftl_nv_cache *nvc), true);
DEFINE_STUB(ftl_nv_cache_chunks_busy, int, (struct ftl_nv_cache *nvc), true);
DEFINE_STUB(ftl_nv_cache_full, bool, (struct ftl_nv_cache *nvc), true);
DEFINE_STUB(ftl_l2p_is_halted, bool, (struct spdk_ftl_dev *dev), true);
DEFINE_STUB(ftl_nv_cache_write, bool, (struct ftl_io *io), true);
DEFINE_STUB_V(ftl_nv_cache_halt, (struct ftl_nv_cache *nvc));
DEFINE_STUB_V(ftl_l2p_halt, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_io_init, int, (struct spdk_io_channel *_ioch, struct ftl_io *io, uint64_t lba,
			       size_t num_blocks,
			       struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_ctx, int type), 0);
DEFINE_STUB_V(ftl_mngt_next_step, (struct ftl_mngt_process *mngt));
DEFINE_STUB_V(ftl_mngt_fail_step, (struct ftl_mngt_process *mngt));
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *bdev_desc),
	    NULL);
DEFINE_STUB(spdk_bdev_io_type_supported, bool, (struct spdk_bdev *bdev,
		enum spdk_bdev_io_type io_type), true);
DEFINE_STUB_V(spdk_bdev_module_release_bdev, (struct spdk_bdev *bdev));
DEFINE_STUB(spdk_bdev_module_claim_bdev, int, (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_bdev_module *module), 0);
DEFINE_STUB(spdk_bdev_open_ext, int, (const char *bdev_name, bool write,
				      spdk_bdev_event_cb_t event_cb,
				      void *event_ctx, struct spdk_bdev_desc **desc), 0);
DEFINE_STUB(spdk_bdev_get_write_unit_size, uint32_t, (const struct spdk_bdev *bdev), 1);
DEFINE_STUB(spdk_bdev_is_md_separate, bool, (const struct spdk_bdev *bdev), true);
DEFINE_STUB(spdk_bdev_get_dif_type, enum spdk_dif_type, (const struct spdk_bdev *bdev),
	    SPDK_DIF_DISABLE);
DEFINE_STUB(ftl_md_xfer_blocks, uint64_t, (struct spdk_ftl_dev *dev), 4);
DEFINE_STUB_V(ftl_l2p_pin, (struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count,
			    ftl_l2p_pin_cb cb, void *cb_ctx,
			    struct ftl_l2p_pin_ctx *pin_ctx));
DEFINE_STUB_V(ftl_l2p_pin_skip, (struct spdk_ftl_dev *dev, ftl_l2p_pin_cb cb, void *cb_ctx,
				 struct ftl_l2p_pin_ctx *pin_ctx));
DEFINE_STUB(ftl_nv_cache_read, int, (struct ftl_io *io, ftl_addr addr, uint32_t num_blocks,
				     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB(ftl_l2p_get, ftl_addr, (struct spdk_ftl_dev *dev, uint64_t lba), 0);
DEFINE_STUB_V(ftl_writer_run, (struct ftl_writer *writer));
DEFINE_STUB(ftl_writer_is_halted, bool, (struct ftl_writer *writer), true);
DEFINE_STUB(ftl_mempool_claim_df, void *, (struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id),
	    NULL);
DEFINE_STUB(ftl_bitmap_count_set, uint64_t, (struct ftl_bitmap *bitmap), 0);
DEFINE_STUB(ftl_p2l_ckpt_region_type, enum ftl_layout_region_type,
	    (const struct ftl_p2l_ckpt *ckpt), 0);
DEFINE_STUB(ftl_md_get_buffer, void *, (struct ftl_md *md), NULL);
DEFINE_STUB(ftl_md_get_vss_buffer, union ftl_md_vss *, (struct ftl_md *md), NULL);
DEFINE_STUB(ftl_nv_cache_acquire_trim_seq_id, uint64_t, (struct ftl_nv_cache *nv_cache), 0);
DEFINE_STUB_V(ftl_md_persist, (struct ftl_md *md));
DEFINE_STUB_V(spdk_bdev_io_get_nvme_status, (const struct spdk_bdev_io *bdev_io, uint32_t *cdw0,
		int *sct, int *sc));
DEFINE_STUB(ftl_nv_cache_throttle, bool, (struct spdk_ftl_dev *dev), true);

static void
adjust_bitmap(struct ftl_bitmap **bitmap, uint64_t *bit)
{
	if (*bitmap == g_dev->valid_map) {
		*bitmap = g_band->p2l_map.valid;
		*bit = test_offset_from_addr(*bit, g_band);
	}
}

bool
ftl_bitmap_get(const struct ftl_bitmap *bitmap, uint64_t bit)
{
	adjust_bitmap((struct ftl_bitmap **)&bitmap, &bit);
	return spdk_bit_array_get((struct spdk_bit_array *)bitmap, bit);
}

void
ftl_bitmap_set(struct ftl_bitmap *bitmap, uint64_t bit)
{
	int ret;

	adjust_bitmap(&bitmap, &bit);
	ret = spdk_bit_array_set((struct spdk_bit_array *)bitmap, bit);
	CU_ASSERT_EQUAL(ret, 0);
}

void
ftl_bitmap_clear(struct ftl_bitmap *bitmap, uint64_t bit)
{
	adjust_bitmap(&bitmap, &bit);
	spdk_bit_array_clear((struct spdk_bit_array *)bitmap, bit);
}

static void
setup_band(void)
{
	int rc;

	g_dev = test_init_ftl_dev(&g_geo);
	g_band = test_init_ftl_band(g_dev, TEST_BAND_IDX, ftl_get_num_blocks_in_band(g_dev));
	rc = ftl_band_alloc_p2l_map(g_band);
	CU_ASSERT_EQUAL_FATAL(rc, 0);
}

static void
cleanup_band(void)
{
	ftl_band_release_p2l_map(g_band);
	test_free_ftl_band(g_band);
	test_free_ftl_dev(g_dev);
}

static ftl_addr
addr_from_zone_id(uint64_t zone_id)
{
	ftl_addr addr;

	addr = zone_id * g_geo.zone_size;
	return addr;
}

static void
test_band_block_offset_from_addr_base(void)
{
	ftl_addr addr;
	uint64_t offset;

	setup_band();
	addr = addr_from_zone_id(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);

	offset = ftl_band_block_offset_from_addr(g_band, addr);
	CU_ASSERT_EQUAL(offset, 0);
	cleanup_band();
}

static void
test_band_block_offset_from_addr_offset(void)
{
	ftl_addr addr;
	uint64_t offset, expect, j;

	setup_band();
	for (j = 0; j < g_geo.zone_size; ++j) {
		addr = addr_from_zone_id(0);
		addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev) + j;

		offset = ftl_band_block_offset_from_addr(g_band, addr);

		expect = test_offset_from_addr(addr, g_band);
		CU_ASSERT_EQUAL(offset, expect);
	}
	cleanup_band();
}

static void
test_band_addr_from_block_offset(void)
{
	ftl_addr addr, expect;
	uint64_t offset, j;

	setup_band();
	for (j = 0; j < g_geo.zone_size; ++j) {
		expect = addr_from_zone_id(0);
		expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev) + j;

		offset = ftl_band_block_offset_from_addr(g_band, expect);
		addr = ftl_band_addr_from_block_offset(g_band, offset);

		CU_ASSERT_EQUAL(addr, expect);
	}
	cleanup_band();
}

static void
test_band_set_addr(void)
{
	struct ftl_p2l_map *p2l_map;
	ftl_addr addr;
	uint64_t offset = 0;

	setup_band();
	p2l_map = &g_band->p2l_map;
	addr = addr_from_zone_id(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);

	CU_ASSERT_EQUAL(p2l_map->num_valid, 0);

	offset = test_offset_from_addr(addr, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, addr);
	ftl_band_set_p2l(g_band, TEST_LBA, addr, TEST_SEQ);
	CU_ASSERT_EQUAL(p2l_map->num_valid, 1);
	CU_ASSERT_EQUAL(p2l_map->band_map[offset].lba, TEST_LBA);
	CU_ASSERT_EQUAL(p2l_map->band_map[offset].seq_id, TEST_SEQ);
	CU_ASSERT_TRUE(ftl_bitmap_get(p2l_map->valid, offset));

	addr += g_geo.zone_size / 2;
	offset = test_offset_from_addr(addr, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, addr);
	ftl_band_set_p2l(g_band, TEST_LBA + 1, addr, TEST_SEQ + 1);
	CU_ASSERT_EQUAL(p2l_map->num_valid, 2);
	CU_ASSERT_EQUAL(p2l_map->band_map[offset].lba, TEST_LBA + 1);
	CU_ASSERT_EQUAL(p2l_map->band_map[offset].seq_id, TEST_SEQ + 1);
	CU_ASSERT_TRUE(ftl_bitmap_get(p2l_map->valid, offset));
	addr -= g_geo.zone_size / 2;
	offset = test_offset_from_addr(addr, g_band);
	CU_ASSERT_TRUE(ftl_bitmap_get(p2l_map->valid, offset));
	cleanup_band();
}

static void
test_invalidate_addr(void)
{
	struct ftl_p2l_map *p2l_map;
	ftl_addr addr;
	uint64_t offset[2];

	setup_band();
	p2l_map = &g_band->p2l_map;
	addr = addr_from_zone_id(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	offset[0] = test_offset_from_addr(addr, g_band);

	ftl_band_set_addr(g_band, TEST_LBA, addr);
	ftl_band_set_p2l(g_band, TEST_LBA, addr, TEST_SEQ);
	CU_ASSERT_EQUAL(p2l_map->num_valid, 1);
	CU_ASSERT_TRUE(ftl_bitmap_get(p2l_map->valid, offset[0]));
	ftl_invalidate_addr(g_band->dev, addr);
	CU_ASSERT_EQUAL(p2l_map->num_valid, 0);
	CU_ASSERT_FALSE(ftl_bitmap_get(p2l_map->valid, offset[0]));

	offset[0] = test_offset_from_addr(addr, g_band);
	ftl_band_set_addr(g_band, TEST_LBA, addr);
	ftl_band_set_p2l(g_band, TEST_LBA, addr, TEST_SEQ);
	addr += g_geo.zone_size / 2;
	offset[1] = test_offset_from_addr(addr, g_band);
	ftl_band_set_addr(g_band, TEST_LBA + 1, addr);
	ftl_band_set_p2l(g_band, TEST_LBA + 1, addr, TEST_SEQ);
	CU_ASSERT_EQUAL(p2l_map->num_valid, 2);
	CU_ASSERT_TRUE(ftl_bitmap_get(p2l_map->valid, offset[0]));
	CU_ASSERT_TRUE(ftl_bitmap_get(p2l_map->valid, offset[1]));
	ftl_invalidate_addr(g_band->dev, addr);
	CU_ASSERT_EQUAL(p2l_map->num_valid, 1);
	CU_ASSERT_TRUE(ftl_bitmap_get(p2l_map->valid, offset[0]));
	CU_ASSERT_FALSE(ftl_bitmap_get(p2l_map->valid, offset[1]));
	cleanup_band();
}

static void
test_next_xfer_addr(void)
{
	ftl_addr addr, result, expect;

	setup_band();
	/* Verify simple one block incremention */
	addr = addr_from_zone_id(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	expect = addr;
	expect += 1;

	result = ftl_band_next_xfer_addr(g_band, addr, 1);
	CU_ASSERT_EQUAL(result, expect);

	/* Verify jumping from last zone to the first one */
	expect = addr_from_zone_id(0);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev) + g_dev->xfer_size;
	addr = addr_from_zone_id(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	result = ftl_band_next_xfer_addr(g_band, addr, g_dev->xfer_size);
	CU_ASSERT_EQUAL(result, expect);

	/* Verify jumping from last zone to the first one with unaligned offset */
	expect = addr_from_zone_id(0);
	expect += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	expect += g_dev->xfer_size + 2;
	addr = addr_from_zone_id(0);
	addr += TEST_BAND_IDX * ftl_get_num_blocks_in_band(g_dev);
	result = ftl_band_next_xfer_addr(g_band, addr, g_dev->xfer_size + 2);
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
