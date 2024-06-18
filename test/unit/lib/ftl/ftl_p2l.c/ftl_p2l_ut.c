/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_core.c"
#include "ftl/ftl_p2l.c"

struct spdk_ftl_dev g_dev;
struct ftl_band g_band;
void *md_buffer;

DEFINE_STUB(ftl_bitmap_create, struct ftl_bitmap *, (void *buf, size_t size), (void *)1);
DEFINE_STUB_V(ftl_bitmap_destroy, (struct ftl_bitmap *bitmap));
DEFINE_STUB_V(ftl_bitmap_set, (struct ftl_bitmap *bitmap, uint64_t bit));
DEFINE_STUB(ftl_bitmap_get, bool, (const struct ftl_bitmap *bitmap, uint64_t bit), false);
DEFINE_STUB_V(ftl_bitmap_clear, (struct ftl_bitmap *bitmap, uint64_t bit));
DEFINE_STUB(ftl_md_vss_buf_alloc, union ftl_md_vss *, (struct ftl_layout_region *region,
		uint32_t count), NULL);
DEFINE_STUB_V(ftl_band_set_p2l, (struct ftl_band *band, uint64_t lba, ftl_addr addr,
				 uint64_t seq_id));
DEFINE_STUB_V(ftl_md_persist, (struct ftl_md *md));
DEFINE_STUB_V(ftl_md_persist_entries, (struct ftl_md *md, uint64_t start_entry,
				       uint64_t num_entries, void *buffer,
				       void *vss_buffer, ftl_md_io_entry_cb cb, void *cb_arg,
				       struct ftl_md_io_entry_ctx *ctx));
DEFINE_STUB(ftl_mngt_get_step_ctx, void *, (struct ftl_mngt_process *mngt), NULL);
DEFINE_STUB_V(ftl_mngt_continue_step, (struct ftl_mngt_process *mngt));
DEFINE_STUB_V(ftl_mngt_next_step, (struct ftl_mngt_process *mngt));
DEFINE_STUB_V(ftl_mngt_fail_step, (struct ftl_mngt_process *mngt));
DEFINE_STUB(ftl_band_from_addr, struct ftl_band *, (struct spdk_ftl_dev *dev, ftl_addr addr), NULL);
DEFINE_STUB(ftl_io_init, int, (struct spdk_io_channel *_ioch, struct ftl_io *io, uint64_t lba,
			       size_t num_blocks,
			       struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_ctx, int type), 0);
DEFINE_STUB_V(ftl_io_inc_req, (struct ftl_io *io));
DEFINE_STUB_V(ftl_io_dec_req, (struct ftl_io *io));
DEFINE_STUB(ftl_io_iovec_addr, void *, (struct ftl_io *io), NULL);
DEFINE_STUB(ftl_io_iovec_len_left, size_t, (struct ftl_io *io), 0);
DEFINE_STUB_V(ftl_io_advance, (struct ftl_io *io, size_t num_blocks));
DEFINE_STUB(ftl_io_current_lba, uint64_t, (const struct ftl_io *io), 0);
DEFINE_STUB(ftl_io_get_lba, uint64_t, (const struct ftl_io *io, size_t offset), 0);
DEFINE_STUB(ftl_io_channel_get_ctx, struct ftl_io_channel *, (struct spdk_io_channel *ioch), NULL);
DEFINE_STUB(ftl_iovec_num_blocks, size_t, (struct iovec *iov, size_t iov_cnt), 0);
DEFINE_STUB_V(ftl_io_complete, (struct ftl_io *io));
DEFINE_STUB(ftl_mngt_trim, int, (struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks,
				 spdk_ftl_fn cb, void *cb_cntx), 0);
DEFINE_STUB(ftl_md_get_vss_buffer, union ftl_md_vss *, (struct ftl_md *md), NULL);
DEFINE_STUB_V(ftl_writer_run, (struct ftl_writer *writer));
DEFINE_STUB_V(ftl_reloc, (struct ftl_reloc *reloc));
DEFINE_STUB_V(ftl_l2p_process, (struct spdk_ftl_dev *dev));
DEFINE_STUB_V(ftl_nv_cache_process, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_reloc_is_halted, bool, (const struct ftl_reloc *reloc), false);
DEFINE_STUB(ftl_writer_is_halted, bool, (struct ftl_writer *writer), true);
DEFINE_STUB(ftl_nv_cache_is_halted, bool, (struct ftl_nv_cache *nvc), true);
DEFINE_STUB(ftl_l2p_is_halted, bool, (struct spdk_ftl_dev *dev), true);
DEFINE_STUB_V(ftl_reloc_halt, (struct ftl_reloc *reloc));
DEFINE_STUB_V(ftl_nv_cache_halt, (struct ftl_nv_cache *nvc));
DEFINE_STUB_V(ftl_l2p_halt, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_nv_cache_chunks_busy, int, (struct ftl_nv_cache *nvc), true);
DEFINE_STUB(ftl_nv_cache_throttle, bool, (struct spdk_ftl_dev *dev), true);
DEFINE_STUB(ftl_nv_cache_write, bool, (struct ftl_io *io), true);
DEFINE_STUB_V(ftl_band_set_state, (struct ftl_band *band, enum ftl_band_state state));
DEFINE_STUB_V(spdk_bdev_io_get_nvme_status, (const struct spdk_bdev_io *bdev_io, uint32_t *cdw0,
		int *sct, int *sc));
DEFINE_STUB(ftl_mngt_get_dev, struct spdk_ftl_dev *, (struct ftl_mngt_process *mngt), NULL);
DEFINE_STUB_V(ftl_l2p_pin, (struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count,
			    ftl_l2p_pin_cb cb, void *cb_ctx,
			    struct ftl_l2p_pin_ctx *pin_ctx));
DEFINE_STUB_V(ftl_l2p_pin_skip, (struct spdk_ftl_dev *dev, ftl_l2p_pin_cb cb, void *cb_ctx,
				 struct ftl_l2p_pin_ctx *pin_ctx));
DEFINE_STUB(ftl_l2p_get, ftl_addr, (struct spdk_ftl_dev *dev, uint64_t lba), 0);
DEFINE_STUB(ftl_nv_cache_acquire_trim_seq_id, uint64_t, (struct ftl_nv_cache *nv_cache), 0);
DEFINE_STUB(ftl_nv_cache_read, int, (struct ftl_io *io, ftl_addr addr, uint32_t num_blocks,
				     spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_read_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_queue_io_wait, int, (struct spdk_bdev *bdev, struct spdk_io_channel *ch,
		struct spdk_bdev_io_wait_entry *entry), 0);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB(ftl_mempool_get, void *, (struct ftl_mempool *mpool), NULL);
DEFINE_STUB(ftl_layout_upgrade_drop_regions, int, (struct spdk_ftl_dev *dev), 0);

#if defined(DEBUG)
DEFINE_STUB_V(ftl_trace_limits, (struct spdk_ftl_dev *dev, int limit, size_t num_free));
DEFINE_STUB_V(ftl_trace_submission, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     ftl_addr addr, size_t addr_cnt));
DEFINE_STUB_V(ftl_trace_completion, (struct spdk_ftl_dev *dev, const struct ftl_io *io,
				     enum ftl_trace_completion completion));
#endif

struct ftl_layout_region *
ftl_layout_region_get(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type)
{
	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	return &g_dev.layout.region[reg_type];
}

uint64_t
ftl_band_block_offset_from_addr(struct ftl_band *band, ftl_addr addr)
{
	return addr - band->start_addr;
}

ftl_addr
ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off)
{
	ftl_addr addr;

	addr = block_off + band->start_addr;
	return addr;
}

ftl_addr
ftl_band_next_addr(struct ftl_band *band, ftl_addr addr, size_t offset)
{
	uint64_t block_off = ftl_band_block_offset_from_addr(band, addr);

	return ftl_band_addr_from_block_offset(band, block_off + offset);
}

void *
ftl_md_get_buffer(struct ftl_md *md)
{
	return md_buffer;
}

ftl_addr
ftl_band_next_xfer_addr(struct ftl_band *band, ftl_addr addr, size_t num_blocks)
{
	CU_ASSERT_EQUAL(num_blocks % g_dev.xfer_size, 0);
	return addr += num_blocks;
}

static void
dev_setup(uint64_t xfer_size, uint64_t band_size)
{
	/* 512 KiB */
	g_dev.xfer_size = xfer_size;
	/* 1GiB */
	g_dev.num_blocks_in_band = band_size;
	g_dev.nv_cache.md_size = 0;
	g_dev.bands = &g_band;
	g_dev.layout.base.total_blocks = (uint64_t)100 * 1024 * 1024 * 1024;
	g_dev.layout.p2l.pages_per_xfer = spdk_divide_round_up(xfer_size, FTL_NUM_P2L_ENTRIES_NO_VSS);
	g_dev.layout.p2l.ckpt_pages = spdk_divide_round_up(band_size,
				      xfer_size) * g_dev.layout.p2l.pages_per_xfer;
	g_dev.layout.region[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC].type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC;
	g_dev.layout.region[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC].mirror_type =
		FTL_LAYOUT_REGION_TYPE_INVALID;
	TAILQ_INIT(&g_dev.p2l_ckpt.free);
	TAILQ_INIT(&g_dev.p2l_ckpt.inuse);
}

static void
band_setup(struct ftl_p2l_ckpt *ckpt, uint64_t xfer_size)
{
	g_band.p2l_map.p2l_ckpt = ckpt;
	g_band.dev = &g_dev;
	g_band.md = calloc(1, sizeof(struct ftl_band_md));
	g_band.md->seq = 0xDEADBEEF;
	g_band.md->p2l_md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC;
	g_band.p2l_map.band_map = calloc(g_dev.num_blocks_in_band, sizeof(struct ftl_p2l_map_entry));
	md_buffer = calloc(1, 1024 * 1024 * 1024);
}

static void
band_free(struct ftl_band *band)
{
	free(md_buffer);
	free(band->md);
	free(band->p2l_map.band_map);
}

static void
test_p2l_num_pages(void)
{
	struct ftl_p2l_ckpt *ckpt;
	uint64_t xfer_size, band_size;

	/* 1GiB band size, xfer size 512KiB, each write unit needs 1 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 512 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 2048);
	ftl_p2l_ckpt_destroy(ckpt);

	/* 1GiB band size, xfer size 256KiB, each write unit needs 1 page */
	xfer_size = 256 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 4096);
	ftl_p2l_ckpt_destroy(ckpt);

	/* 1GiB band size, xfer size 4KiB, each write unit needs 1 page */
	xfer_size = 1;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 262144);
	ftl_p2l_ckpt_destroy(ckpt);

	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 pages */
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 2048);
	ftl_p2l_ckpt_destroy(ckpt);

	/* 1GiB band size, xfer size 2MiB, each write unit needs 3 pages */
	xfer_size = 2 * 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 1536);
	ftl_p2l_ckpt_destroy(ckpt);

	/* 1GiB band size, xfer size 8MiB, each write unit needs 9 pages */
	xfer_size = 8 * 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 1152);
	ftl_p2l_ckpt_destroy(ckpt);

	/* 3GiB band size, xfer size 1.5MiB, each write unit needs 2 pages */
	band_size = (uint64_t)3 * 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 3 * 512 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 4096);
	ftl_p2l_ckpt_destroy(ckpt);

	/* 3GiB band size, xfer size 0.75MiB, each write unit needs 1 page */
	xfer_size = 3 * 256 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	CU_ASSERT_EQUAL(ckpt->num_pages, 4096);
	ftl_p2l_ckpt_destroy(ckpt);
}

static struct ftl_rq *
setup_rq(uint64_t xfer_size, uint64_t start_lba)
{
	struct ftl_rq *rq;

	rq = calloc(1, xfer_size * sizeof(struct ftl_rq_entry) + sizeof(struct ftl_rq));
	rq->dev = &g_dev;
	rq->io.band = &g_band;
	rq->io.addr = start_lba;
	rq->num_blocks = xfer_size;

	for (uint64_t i = 0; i < xfer_size; i++) {
		rq->entries[i].lba = start_lba + i;
		rq->entries[i].seq_id = 1;
	}

	return rq;
}

static void
free_rq(struct ftl_rq *rq)
{
	free(rq);
}

static void
verify_p2l(uint64_t start_page, uint64_t start_lba, uint64_t num_lbas)
{
	struct ftl_p2l_ckpt_page_no_vss *map_page, *first_page = md_buffer;
	uint64_t entry_idx = 0;

	map_page = first_page + start_page;

	for (uint64_t i = start_lba; i < start_lba + num_lbas; i++, entry_idx++) {
		if (entry_idx == FTL_NUM_P2L_ENTRIES_NO_VSS) {
			CU_ASSERT_EQUAL(map_page->metadata.p2l_ckpt.count, FTL_NUM_P2L_ENTRIES_NO_VSS);
			entry_idx = 0;
			map_page++;
		}
		CU_ASSERT_EQUAL(map_page->metadata.p2l_ckpt.seq_id, 0xDEADBEEF);

		CU_ASSERT_EQUAL(map_page->map[entry_idx].lba, i);
		CU_ASSERT_EQUAL(map_page->map[entry_idx].seq_id, 1);
	}

	CU_ASSERT_EQUAL(map_page->metadata.p2l_ckpt.count, num_lbas % FTL_NUM_P2L_ENTRIES_NO_VSS);
}

static void
test_ckpt_issue(void)
{
	struct ftl_p2l_ckpt *ckpt;
	struct ftl_rq *rq;
	uint64_t xfer_size, band_size;

	/* 1GiB band size, xfer size 512KiB, each write unit needs 1 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 512 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	band_setup(ckpt, xfer_size);

	/* Issue 2x 512 KiB writes */
	rq = setup_rq(xfer_size, 0);
	ftl_p2l_ckpt_issue(rq);
	free_rq(rq);

	rq = setup_rq(xfer_size, xfer_size);
	ftl_p2l_ckpt_issue(rq);
	free_rq(rq);

	/* Check contents of two expected P2L pages */
	verify_p2l(0, 0, xfer_size);
	verify_p2l(1, xfer_size, xfer_size);

	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);

	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 page */
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	band_setup(ckpt, xfer_size);

	/* Issue 2x 1 MiB writes */
	rq = setup_rq(xfer_size, 0);
	ftl_p2l_ckpt_issue(rq);
	free_rq(rq);

	rq = setup_rq(xfer_size, xfer_size);
	ftl_p2l_ckpt_issue(rq);
	free_rq(rq);

	/* Check contents of four expected P2L pages */
	verify_p2l(0, 0, xfer_size);
	verify_p2l(2, xfer_size, xfer_size);

	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);
}

static void
setup_sync_ctx(struct ftl_p2l_sync_ctx *ctx, uint64_t xfer_start)
{
	ctx->band = &g_band;
	ctx->xfer_start = xfer_start;
}

static void
fill_band_p2l(struct ftl_band *band, uint64_t start_lba)
{
	for (uint64_t i = 0; i < g_dev.num_blocks_in_band; i++) {
		band->p2l_map.band_map[i].lba = start_lba + i;
		band->p2l_map.band_map[i].seq_id = 1;
	}
}

static void
test_persist_band_p2l(void)
{
	struct ftl_p2l_sync_ctx ctx;
	struct ftl_p2l_ckpt *ckpt;
	uint64_t xfer_size, band_size;

	/* 1GiB band size, xfer size 512KiB, each write unit needs 1 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 512 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	band_setup(ckpt, xfer_size);
	fill_band_p2l(&g_band, 0);

	/* Persist two band P2L pages */
	setup_sync_ctx(&ctx, 0);
	ftl_mngt_persist_band_p2l(NULL, &ctx);

	setup_sync_ctx(&ctx, 1);
	ftl_mngt_persist_band_p2l(NULL, &ctx);

	/* Check contents of two expected P2L pages */
	verify_p2l(0, 0, xfer_size);
	verify_p2l(1, xfer_size, xfer_size);

	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);


	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 pages */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	band_setup(ckpt, xfer_size);
	fill_band_p2l(&g_band, 0);

	/* Persist two band P2L pages */
	setup_sync_ctx(&ctx, 0);
	ftl_mngt_persist_band_p2l(NULL, &ctx);

	setup_sync_ctx(&ctx, 1);
	ftl_mngt_persist_band_p2l(NULL, &ctx);

	/* Check contents of two expected P2L pages */
	verify_p2l(0, 0, xfer_size);
	verify_p2l(2, xfer_size, xfer_size);

	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);
}

static void
fill_running_p2l(uint64_t starting_page, uint64_t starting_lba, uint64_t num_lbas)
{
	struct ftl_p2l_ckpt_page_no_vss *map_page = md_buffer;
	uint64_t page_counter = 0;

	map_page += starting_page;
	map_page->metadata.p2l_ckpt.count = 0;

	for (uint64_t i = 0; i < num_lbas; i++, page_counter++) {
		if (page_counter == FTL_NUM_P2L_ENTRIES_NO_VSS) {
			page_counter = 0;
			map_page++;
			map_page->metadata.p2l_ckpt.count = 0;
		}
		map_page->metadata.p2l_ckpt.seq_id = 0xDEADBEEF;
		map_page->metadata.p2l_ckpt.count++;
		map_page->map[page_counter].lba = starting_lba + i;
		map_page->map[page_counter].seq_id = 1;
	}
}

static void
verify_band_p2l(struct ftl_band *band, uint64_t start_entry, uint64_t num_entries)
{
	for (uint64_t i = start_entry; i < num_entries; i++) {
		CU_ASSERT_EQUAL(band->p2l_map.band_map[i].seq_id, 1);
		CU_ASSERT_EQUAL(band->p2l_map.band_map[i].lba, i);
	}
}

static void
test_clean_restore_p2l(void)
{
	struct ftl_p2l_ckpt *ckpt;
	uint64_t xfer_size, band_size;

	/* 1GiB band size, xfer size 512KiB, each write unit needs 1 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 512 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	band_setup(ckpt, xfer_size);

	fill_running_p2l(0, 0, xfer_size);
	fill_running_p2l(1, xfer_size, xfer_size);
	verify_p2l(0, 0, xfer_size);
	verify_p2l(1, xfer_size, xfer_size);
	g_band.md->iter.offset = 2 * xfer_size;

	ftl_mngt_p2l_ckpt_restore_clean(&g_band);
	verify_band_p2l(&g_band, 0, 2 * xfer_size);

	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);


	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	band_setup(ckpt, xfer_size);

	fill_running_p2l(0, 0, xfer_size);
	fill_running_p2l(2, xfer_size, xfer_size);
	verify_p2l(0, 0, xfer_size);
	verify_p2l(2, xfer_size, xfer_size);
	g_band.md->iter.offset = 2 * xfer_size;

	ftl_mngt_p2l_ckpt_restore_clean(&g_band);
	verify_band_p2l(&g_band, 0, 2 * xfer_size);

	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);
}

static void
test_dirty_restore_p2l(void)
{
	struct ftl_p2l_ckpt *ckpt;
	uint64_t xfer_size, band_size;

	/* 1GiB band size, xfer size 512KiB, each write unit needs 1 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 512 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	TAILQ_INSERT_TAIL(&g_dev.p2l_ckpt.free, ckpt, link);
	band_setup(ckpt, xfer_size);

	/* Running P2L are fully filled */
	fill_running_p2l(0, 0, xfer_size);
	fill_running_p2l(1, xfer_size, xfer_size);

	ftl_mngt_p2l_ckpt_restore(&g_band, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC, 0xDEADBEEF);
	verify_band_p2l(&g_band, 0, 2 * xfer_size);
	CU_ASSERT_EQUAL(g_band.md->iter.offset, 2 * xfer_size);

	TAILQ_REMOVE(&g_dev.p2l_ckpt.inuse, ckpt, link);
	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);


	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	TAILQ_INSERT_TAIL(&g_dev.p2l_ckpt.free, ckpt, link);
	band_setup(ckpt, xfer_size);

	/* Running P2L are fully filled */
	fill_running_p2l(0, 0, xfer_size);
	fill_running_p2l(2, xfer_size, xfer_size);

	ftl_mngt_p2l_ckpt_restore(&g_band, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC, 0xDEADBEEF);
	verify_band_p2l(&g_band, 0, 2 * xfer_size);

	CU_ASSERT_EQUAL(g_band.md->iter.offset, 2 * xfer_size);

	TAILQ_REMOVE(&g_dev.p2l_ckpt.inuse, ckpt, link);
	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);

	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	TAILQ_INSERT_TAIL(&g_dev.p2l_ckpt.free, ckpt, link);
	band_setup(ckpt, xfer_size);

	/* Running P2L are fully filled, only second xfer_size was written */
	fill_running_p2l(2, xfer_size, xfer_size);

	ftl_mngt_p2l_ckpt_restore(&g_band, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC, 0xDEADBEEF);
	verify_band_p2l(&g_band, xfer_size, xfer_size);

	CU_ASSERT_EQUAL(g_band.md->iter.offset, 2 * xfer_size);

	TAILQ_REMOVE(&g_dev.p2l_ckpt.inuse, ckpt, link);
	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);

	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	TAILQ_INSERT_TAIL(&g_dev.p2l_ckpt.free, ckpt, link);
	band_setup(ckpt, xfer_size);

	/* Running P2L is partially filled, only first part of second xfer_size was written */
	fill_running_p2l(2, xfer_size, FTL_NUM_P2L_ENTRIES_NO_VSS);

	ftl_mngt_p2l_ckpt_restore(&g_band, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC, 0xDEADBEEF);
	verify_band_p2l(&g_band, xfer_size, FTL_NUM_P2L_ENTRIES_NO_VSS);

	CU_ASSERT_EQUAL(g_band.md->iter.offset, 2 * xfer_size);

	TAILQ_REMOVE(&g_dev.p2l_ckpt.inuse, ckpt, link);
	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);

	/* 1GiB band size, xfer size 1MiB, each write unit needs 2 page */
	band_size = 1024 * 1024 * 1024 / FTL_BLOCK_SIZE;
	xfer_size = 1024 * 1024 / FTL_BLOCK_SIZE;
	dev_setup(xfer_size, band_size);
	ckpt = ftl_p2l_ckpt_new(&g_dev, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC);
	TAILQ_INSERT_TAIL(&g_dev.p2l_ckpt.free, ckpt, link);
	band_setup(ckpt, xfer_size);

	/* Running P2L is partially filled, only second part of second xfer_size was written */
	fill_running_p2l(3, xfer_size, xfer_size - FTL_NUM_P2L_ENTRIES_NO_VSS);

	ftl_mngt_p2l_ckpt_restore(&g_band, FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC, 0xDEADBEEF);
	verify_band_p2l(&g_band, xfer_size + xfer_size - FTL_NUM_P2L_ENTRIES_NO_VSS,
			xfer_size - FTL_NUM_P2L_ENTRIES_NO_VSS);

	CU_ASSERT_EQUAL(g_band.md->iter.offset, 2 * xfer_size);

	TAILQ_REMOVE(&g_dev.p2l_ckpt.inuse, ckpt, link);
	ftl_p2l_ckpt_destroy(ckpt);
	band_free(&g_band);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_p2l_suite", NULL, NULL);

	CU_ADD_TEST(suite, test_p2l_num_pages);
	CU_ADD_TEST(suite, test_ckpt_issue);
	CU_ADD_TEST(suite, test_persist_band_p2l);
	CU_ADD_TEST(suite, test_clean_restore_p2l);
	CU_ADD_TEST(suite, test_dirty_restore_p2l);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
