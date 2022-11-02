/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev_module.h"

#include "ftl_nv_cache.h"
#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_band.h"
#include "ftl_internal.h"
#include "ftl_l2p_cache.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "utils/ftl_addr_utils.h"

struct ftl_mngt_recovery_ctx {
	/* Main recovery FTL management process */
	struct ftl_mngt_process *main;
	int status;
	TAILQ_HEAD(, ftl_band) open_bands;
	uint64_t open_bands_num;
	struct {
		struct ftl_layout_region region;
		struct ftl_md *md;
		uint64_t *l2p;
		uint64_t *seq_id;
		uint64_t count;
	} l2p_snippet;
	struct {
		uint64_t block_limit;
		uint64_t lba_first;
		uint64_t lba_last;
		uint32_t i;
	} iter;
	uint64_t p2l_ckpt_seq_id[FTL_LAYOUT_REGION_TYPE_P2L_COUNT];
};

static const struct ftl_mngt_process_desc g_desc_recovery_iteration;
static const struct ftl_mngt_process_desc g_desc_recovery;
static const struct ftl_mngt_process_desc g_desc_recovery_shm;

static bool
recovery_iter_done(struct spdk_ftl_dev *dev, struct ftl_mngt_recovery_ctx *ctx)
{
	return 0 == ctx->l2p_snippet.region.current.blocks;
}

static void
recovery_iter_advance(struct spdk_ftl_dev *dev, struct ftl_mngt_recovery_ctx *ctx)
{
	struct ftl_layout_region *region, *snippet;
	uint64_t first_block, last_blocks;

	ctx->iter.i++;
	region = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_L2P];
	snippet = &ctx->l2p_snippet.region;

	/* Advance processed blocks */
	snippet->current.offset += snippet->current.blocks;
	snippet->current.blocks = region->current.offset + region->current.blocks - snippet->current.offset;
	snippet->current.blocks = spdk_min(snippet->current.blocks, ctx->iter.block_limit);

	first_block = snippet->current.offset - region->current.offset;
	ctx->iter.lba_first = first_block * (FTL_BLOCK_SIZE / dev->layout.l2p.addr_size);

	last_blocks = first_block + snippet->current.blocks;
	ctx->iter.lba_last = last_blocks * (FTL_BLOCK_SIZE / dev->layout.l2p.addr_size);

	if (ctx->iter.lba_last > dev->num_lbas) {
		ctx->iter.lba_last = dev->num_lbas;
	}
}

static void
ftl_mngt_recovery_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	const uint64_t lbas_in_block = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;
	uint64_t mem_limit, lba_limit, l2p_limit, iterations, seq_limit;
	uint64_t l2p_limit_block, seq_limit_block, md_blocks;
	int md_flags;

	ctx->main = mngt;

	if (ftl_fast_recovery(dev)) {
		/* If shared memory fast recovery then we don't need temporary buffers */
		ftl_mngt_next_step(mngt);
		return;
	}

	/*
	 * Recovery process allocates temporary buffers, to not exceed memory limit free L2P
	 * metadata buffers if they exist, they will be recreated in L2P initialization phase
	 */
	ftl_md_unlink(dev, FTL_L2P_CACHE_MD_NAME_L1, ftl_md_create_shm_flags(dev));
	ftl_md_unlink(dev, FTL_L2P_CACHE_MD_NAME_L2, ftl_md_create_shm_flags(dev));
	ftl_md_unlink(dev, FTL_L2P_CACHE_MD_NAME_L2_CTX, ftl_md_create_shm_flags(dev));

	/* Below values are in byte unit */
	mem_limit = dev->conf.l2p_dram_limit * MiB;
	mem_limit = spdk_min(mem_limit, spdk_divide_round_up(dev->num_lbas * dev->layout.l2p.addr_size,
			     MiB) * MiB);

	lba_limit = mem_limit / (sizeof(uint64_t) + dev->layout.l2p.addr_size);
	l2p_limit = lba_limit * dev->layout.l2p.addr_size;
	iterations = spdk_divide_round_up(dev->num_lbas, lba_limit);

	ctx->iter.block_limit = spdk_divide_round_up(l2p_limit, FTL_BLOCK_SIZE);

	/* Round to block size */
	ctx->l2p_snippet.count = ctx->iter.block_limit * lbas_in_block;

	seq_limit = ctx->l2p_snippet.count * sizeof(uint64_t);

	FTL_NOTICELOG(dev, "Recovery memory limit: %"PRIu64"MiB\n", (uint64_t)(mem_limit / MiB));
	FTL_NOTICELOG(dev, "L2P resident size: %"PRIu64"MiB\n", (uint64_t)(l2p_limit / MiB));
	FTL_NOTICELOG(dev, "Seq ID resident size: %"PRIu64"MiB\n", (uint64_t)(seq_limit / MiB));
	FTL_NOTICELOG(dev, "Recovery iterations: %"PRIu64"\n", iterations);
	dev->sb->ckpt_seq_id = 0;

	/* Initialize region */
	ctx->l2p_snippet.region = dev->layout.region[FTL_LAYOUT_REGION_TYPE_L2P];
	/* Limit blocks in region, it will be needed for ftl_md_set_region */
	ctx->l2p_snippet.region.current.blocks = ctx->iter.block_limit;

	l2p_limit_block = ctx->iter.block_limit;
	seq_limit_block = spdk_divide_round_up(seq_limit, FTL_BLOCK_SIZE);

	md_blocks = l2p_limit_block + seq_limit_block;
	md_flags = FTL_MD_CREATE_SHM | FTL_MD_CREATE_SHM_NEW;

	/* Initialize snippet of L2P metadata */
	ctx->l2p_snippet.md = ftl_md_create(dev, md_blocks, 0, "l2p_recovery", md_flags,
					    &ctx->l2p_snippet.region);
	if (!ctx->l2p_snippet.md) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	ctx->l2p_snippet.l2p = ftl_md_get_buffer(ctx->l2p_snippet.md);

	/* Initialize recovery iterator, we call it with blocks set to zero,
	 * it means zero block done (processed), thanks that it will recalculate
	 *  offsets and starting LBA to initial position */
	ctx->l2p_snippet.region.current.blocks = 0;
	recovery_iter_advance(dev, ctx);

	/* Initialize snippet of sequence IDs */
	ctx->l2p_snippet.seq_id = (uint64_t *)((char *)ftl_md_get_buffer(ctx->l2p_snippet.md) +
					       (l2p_limit_block * FTL_BLOCK_SIZE));

	TAILQ_INIT(&ctx->open_bands);
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_recovery_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ftl_md_destroy(ctx->l2p_snippet.md, 0);
	ctx->l2p_snippet.md = NULL;
	ctx->l2p_snippet.seq_id = NULL;

	ftl_mngt_next_step(mngt);
}

static void
recovery_iteration_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_mngt_recovery_ctx *ctx = _ctx;

	recovery_iter_advance(dev, ctx);

	if (status) {
		ftl_mngt_fail_step(ctx->main);
	} else {
		ftl_mngt_continue_step(ctx->main);
	}
}

static void
ftl_mngt_recovery_run_iteration(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	if (ftl_fast_recovery(dev)) {
		ftl_mngt_skip_step(mngt);
		return;
	}

	if (recovery_iter_done(dev, ctx)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_process_execute(dev, &g_desc_recovery_iteration, recovery_iteration_cb, ctx);
	}
}

static void
restore_band_state_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_band *band;
	uint64_t num_bands = ftl_get_num_bands(dev);
	uint64_t i;

	if (status) {
		/* Restore error, end step */
		ftl_mngt_fail_step(mngt);
		return;
	}

	for (i = 0; i < num_bands; i++) {
		band = &dev->bands[i];

		switch (band->md->state) {
		case FTL_BAND_STATE_FREE:
			ftl_band_initialize_free_state(band);
			break;
		case FTL_BAND_STATE_OPEN:
			TAILQ_REMOVE(&band->dev->shut_bands, band, queue_entry);
			TAILQ_INSERT_HEAD(&pctx->open_bands, band, queue_entry);
			break;
		case FTL_BAND_STATE_CLOSED:
			break;
		default:
			status = -EINVAL;
		}
	}

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void
ftl_mngt_recovery_restore_band_state(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];

	md->owner.cb_ctx = mngt;
	md->cb = restore_band_state_cb;
	ftl_md_restore(md);
}

struct band_md_ctx {
	int status;
	uint64_t qd;
	uint64_t id;
};

static void
ftl_mngt_recovery_walk_band_tail_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
				    ftl_band_md_cb cb)
{
	struct band_md_ctx *sctx = ftl_mngt_get_step_ctx(mngt);
	uint64_t num_bands = ftl_get_num_bands(dev);

	/*
	 * This function generates a high queue depth and will utilize ftl_mngt_continue_step during completions to make sure all bands
	 * are processed before returning an error (if any were found) or continuing on.
	 */
	if (0 == sctx->qd && sctx->id == num_bands) {
		if (sctx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
		return;
	}

	while (sctx->id < num_bands) {
		struct ftl_band *band = &dev->bands[sctx->id];

		if (FTL_BAND_STATE_FREE == band->md->state) {
			sctx->id++;
			continue;
		}

		if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
			/* This band is already open and has valid P2L map */
			sctx->id++;
			sctx->qd++;
			ftl_band_acquire_p2l_map(band);
			cb(band, mngt, FTL_MD_SUCCESS);
			continue;
		} else {
			if (dev->sb->ckpt_seq_id && (band->md->close_seq_id <= dev->sb->ckpt_seq_id)) {
				sctx->id++;
				continue;
			}

			band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_p2l_map(band)) {
				/* No more free P2L map, try later */
				break;
			}
		}

		sctx->id++;
		ftl_band_read_tail_brq_md(band, cb, mngt);
		sctx->qd++;
	}

	if (0 == sctx->qd) {
		/*
		 * No QD could happen due to all leftover bands being in free state.
		 * For streamlining of all potential error handling (since many bands are reading P2L at the same time),
		 * we're using ftl_mngt_continue_step to arrive at the same spot of checking for mngt step end (see beginning of function).
		 */
		ftl_mngt_continue_step(mngt);
	}
}

static void
ftl_mngt_recovery_iteration_init_seq_ids(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
	uint64_t *trim_map = ftl_md_get_buffer(md);
	uint64_t page_id, trim_seq_id;
	uint32_t lbas_in_page = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;
	uint64_t lba, lba_off;

	if (dev->sb->ckpt_seq_id) {
		FTL_ERRLOG(dev, "Checkpoint recovery not supported!\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	for (lba = ctx->iter.lba_first; lba < ctx->iter.lba_last; lba++) {
		lba_off = lba - ctx->iter.lba_first;
		page_id = lba / lbas_in_page;

		assert(page_id < ftl_md_get_buffer_size(md) / sizeof(*trim_map));
		assert(page_id < dev->layout.region[FTL_LAYOUT_REGION_TYPE_L2P].current.blocks);
		assert(lba_off < ctx->l2p_snippet.count);

		trim_seq_id = trim_map[page_id];

		ctx->l2p_snippet.seq_id[lba_off] = trim_seq_id;
		ftl_addr_store(dev, ctx->l2p_snippet.l2p, lba_off, FTL_ADDR_INVALID);
	}

	ftl_mngt_next_step(mngt);
}

static void
l2p_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void
ftl_mngt_recovery_iteration_load_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	struct ftl_md *md = ctx->l2p_snippet.md;
	struct ftl_layout_region *region = &ctx->l2p_snippet.region;

	FTL_NOTICELOG(dev, "L2P recovery, iteration %u\n", ctx->iter.i);
	FTL_NOTICELOG(dev, "Load L2P, blocks [%"PRIu64", %"PRIu64"), LBAs [%"PRIu64", %"PRIu64")\n",
		      region->current.offset, region->current.offset + region->current.blocks,
		      ctx->iter.lba_first, ctx->iter.lba_last);

	if (ftl_md_set_region(md, &ctx->l2p_snippet.region)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = l2p_cb;
	ftl_md_restore(md);
}

static void
ftl_mngt_recovery_iteration_save_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	struct ftl_md *md = ctx->l2p_snippet.md;

	md->owner.cb_ctx = mngt;
	md->cb = l2p_cb;
	ftl_md_persist(md);
}

static void
restore_band_l2p_cb(struct ftl_band *band, void *cntx, enum ftl_md_status status)
{
	struct ftl_mngt_process *mngt = cntx;
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_caller_ctx(mngt);
	struct band_md_ctx *sctx = ftl_mngt_get_step_ctx(mngt);
	struct spdk_ftl_dev *dev = band->dev;
	ftl_addr addr, curr_addr;
	uint64_t i, lba, seq_id, num_blks_in_band;
	uint32_t band_map_crc;
	int rc = 0;

	if (status != FTL_MD_SUCCESS) {
		FTL_ERRLOG(dev, "L2P band restore error, failed to read P2L map\n");
		rc = -EIO;
		goto cleanup;
	}

	band_map_crc = spdk_crc32c_update(band->p2l_map.band_map,
					  ftl_tail_md_num_blocks(band->dev) * FTL_BLOCK_SIZE, 0);

	/* P2L map is only valid if the band state is closed */
	if (FTL_BAND_STATE_CLOSED == band->md->state && band->md->p2l_map_checksum != band_map_crc) {
		FTL_ERRLOG(dev, "L2P band restore error, inconsistent P2L map CRC\n");
		ftl_stats_crc_error(dev, FTL_STATS_TYPE_MD_BASE);
		rc = -EINVAL;
		goto cleanup;
	}

	num_blks_in_band = ftl_get_num_blocks_in_band(dev);
	for (i = 0; i < num_blks_in_band; ++i) {
		uint64_t lba_off;
		lba = band->p2l_map.band_map[i].lba;
		seq_id = band->p2l_map.band_map[i].seq_id;

		if (lba == FTL_LBA_INVALID) {
			continue;
		}
		if (lba >= dev->num_lbas) {
			FTL_ERRLOG(dev, "L2P band restore ERROR, LBA out of range\n");
			rc = -EINVAL;
			break;
		}
		if (lba < pctx->iter.lba_first || lba >= pctx->iter.lba_last) {
			continue;
		}

		lba_off = lba - pctx->iter.lba_first;
		if (seq_id < pctx->l2p_snippet.seq_id[lba_off]) {

			/* Overlapped band/chunk has newer data - invalidate P2L map on open/full band  */
			if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
				addr = ftl_band_addr_from_block_offset(band, i);
				ftl_band_set_p2l(band, FTL_LBA_INVALID, addr, 0);
			}

			/* Newer data already recovered */
			continue;
		}

		addr = ftl_band_addr_from_block_offset(band, i);

		curr_addr = ftl_addr_load(dev, pctx->l2p_snippet.l2p, lba_off);

		/* Overlapped band/chunk has newer data - invalidate P2L map on open/full band  */
		if (curr_addr != FTL_ADDR_INVALID && !ftl_addr_in_nvc(dev, curr_addr) && curr_addr != addr) {
			struct ftl_band *curr_band = ftl_band_from_addr(dev, curr_addr);

			if (FTL_BAND_STATE_OPEN == curr_band->md->state || FTL_BAND_STATE_FULL == curr_band->md->state) {
				size_t prev_offset = ftl_band_block_offset_from_addr(curr_band, curr_addr);
				if (curr_band->p2l_map.band_map[prev_offset].lba == lba &&
				    seq_id >= curr_band->p2l_map.band_map[prev_offset].seq_id) {
					ftl_band_set_p2l(curr_band, FTL_LBA_INVALID, curr_addr, 0);
				}
			}
		}

		ftl_addr_store(dev, pctx->l2p_snippet.l2p, lba_off, addr);
		pctx->l2p_snippet.seq_id[lba_off] = seq_id;
	}


cleanup:
	ftl_band_release_p2l_map(band);

	sctx->qd--;
	if (rc) {
		sctx->status = rc;
	}

	ftl_mngt_continue_step(mngt);
}

static void
ftl_mngt_recovery_iteration_restore_band_l2p(struct spdk_ftl_dev *dev,
		struct ftl_mngt_process *mngt)
{
	ftl_mngt_recovery_walk_band_tail_md(dev, mngt, restore_band_l2p_cb);
}

static int
restore_chunk_l2p_cb(struct ftl_nv_cache_chunk *chunk, void *ctx)
{
	struct ftl_mngt_recovery_ctx *pctx = ctx;
	struct spdk_ftl_dev *dev;
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	ftl_addr addr;
	const uint64_t seq_id = chunk->md->seq_id;
	uint64_t i, lba;
	uint32_t chunk_map_crc;

	dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);

	chunk_map_crc = spdk_crc32c_update(chunk->p2l_map.chunk_map,
					   ftl_nv_cache_chunk_tail_md_num_blocks(chunk->nv_cache) * FTL_BLOCK_SIZE, 0);
	if (chunk->md->p2l_map_checksum != chunk_map_crc) {
		ftl_stats_crc_error(dev, FTL_STATS_TYPE_MD_NV_CACHE);
		return -1;
	}

	for (i = 0; i < nv_cache->chunk_blocks; ++i) {
		uint64_t lba_off;

		lba = ftl_chunk_map_get_lba(chunk, i);

		if (lba == FTL_LBA_INVALID) {
			continue;
		}
		if (lba >= dev->num_lbas) {
			FTL_ERRLOG(dev, "L2P Chunk restore ERROR, LBA out of range\n");
			return -1;
		}
		if (lba < pctx->iter.lba_first || lba >= pctx->iter.lba_last) {
			continue;
		}

		lba_off = lba - pctx->iter.lba_first;
		if (seq_id < pctx->l2p_snippet.seq_id[lba_off]) {
			/* Newer data already recovered */
			continue;
		}

		addr = ftl_addr_from_nvc_offset(dev, chunk->offset + i);
		ftl_addr_store(dev, pctx->l2p_snippet.l2p, lba_off, addr);
		pctx->l2p_snippet.seq_id[lba_off] = seq_id;
	}

	return 0;
}

static void
ftl_mngt_recovery_iteration_restore_chunk_l2p(struct spdk_ftl_dev *dev,
		struct ftl_mngt_process *mngt)
{
	ftl_mngt_nv_cache_restore_l2p(dev, mngt, restore_chunk_l2p_cb, ftl_mngt_get_caller_ctx(mngt));
}

static void
ftl_mngt_recovery_iteration_restore_valid_map(struct spdk_ftl_dev *dev,
		struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_caller_ctx(mngt);
	uint64_t lba, lba_off;
	ftl_addr addr;

	for (lba = pctx->iter.lba_first; lba < pctx->iter.lba_last; lba++) {
		lba_off = lba - pctx->iter.lba_first;
		addr = ftl_addr_load(dev, pctx->l2p_snippet.l2p, lba_off);

		if (addr == FTL_ADDR_INVALID) {
			continue;
		}

		if (!ftl_addr_in_nvc(dev, addr)) {
			struct ftl_band *band = ftl_band_from_addr(dev, addr);
			band->p2l_map.num_valid++;
		}

		if (ftl_bitmap_get(dev->valid_map, addr)) {
			assert(false);
			ftl_mngt_fail_step(mngt);
			return;
		} else {
			ftl_bitmap_set(dev->valid_map, addr);
		}
	}

	ftl_mngt_next_step(mngt);
}

static void
p2l_ckpt_preprocess(struct spdk_ftl_dev *dev, struct ftl_mngt_recovery_ctx *pctx)
{
	uint64_t seq_id;
	int md_region, ckpt_id;

	for (md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX; md_region++) {
		ckpt_id = md_region - FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
		seq_id = ftl_mngt_p2l_ckpt_get_seq_id(dev, md_region);
		pctx->p2l_ckpt_seq_id[ckpt_id] = seq_id;
		FTL_NOTICELOG(dev, "P2L ckpt_id=%d found seq_id=%"PRIu64"\n", ckpt_id, seq_id);
	}
}

static int
p2l_ckpt_restore_p2l(struct ftl_mngt_recovery_ctx *pctx, struct ftl_band *band)
{
	uint64_t seq_id;
	int md_region, ckpt_id;

	memset(band->p2l_map.band_map, -1,
	       FTL_BLOCK_SIZE * ftl_p2l_map_num_blocks(band->dev));

	for (md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX; md_region++) {
		ckpt_id = md_region - FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
		seq_id = pctx->p2l_ckpt_seq_id[ckpt_id];
		if (seq_id == band->md->seq) {
			FTL_NOTICELOG(band->dev, "Restore band P2L band_id=%u ckpt_id=%d seq_id=%"
				      PRIu64"\n", band->id, ckpt_id, seq_id);
			return ftl_mngt_p2l_ckpt_restore(band, md_region, seq_id);
		}
	}

	/* Band opened but no valid blocks within it, set write pointer to 0 */
	ftl_band_iter_init(band);
	FTL_NOTICELOG(band->dev, "Restore band P2L band_id=%u, band_seq_id=%"PRIu64" does not"
		      " match any P2L checkpoint\n", band->id, band->md->seq);
	return 0;
}

static void
ftl_mngt_recovery_pre_process_p2l(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_process_ctx(mngt);

	p2l_ckpt_preprocess(dev, pctx);
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_recover_seq_id(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_recover_max_seq(dev);
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_recovery_open_bands_p2l(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_recovery_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_band *band;

	if (TAILQ_EMPTY(&pctx->open_bands)) {
		FTL_NOTICELOG(dev, "No more open bands to recover from P2L\n");
		if (pctx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
		return;
	}

	if (!ftl_mngt_get_step_ctx(mngt)) {
		ftl_mngt_alloc_step_ctx(mngt, sizeof(bool));

		/* Step first time called, initialize */
		TAILQ_FOREACH(band, &pctx->open_bands, queue_entry) {
			band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_p2l_map(band)) {
				FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot allocate P2L map\n");
				ftl_mngt_fail_step(mngt);
				return;
			}

			if (p2l_ckpt_restore_p2l(pctx, band)) {
				FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot restore P2L\n");
				ftl_mngt_fail_step(mngt);
				return;
			}

			if (!band->p2l_map.p2l_ckpt) {
				band->p2l_map.p2l_ckpt = ftl_p2l_ckpt_acquire_region_type(dev, band->md->p2l_md_region);
				if (!band->p2l_map.p2l_ckpt) {
					FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot acquire P2L\n");
					ftl_mngt_fail_step(mngt);
					return;
				}
			}
		}
	}

	band = TAILQ_FIRST(&pctx->open_bands);

	if (ftl_band_filled(band, band->md->iter.offset)) {
		band->md->state = FTL_BAND_STATE_FULL;
	}

	/* In a next step (finalize band initialization) this band will
	 * be assigned to the writer. So temporary we move this band
	 * to the closed list, and in the next step it will be moved to
	 * the writer from such list.
	 */
	TAILQ_REMOVE(&pctx->open_bands, band, queue_entry);
	TAILQ_INSERT_TAIL(&dev->shut_bands, band, queue_entry);

	FTL_NOTICELOG(dev, "Open band recovered, id = %u, seq id %"PRIu64", write offset %"PRIu64"\n",
		      band->id, band->md->seq, band->md->iter.offset);

	ftl_mngt_continue_step(mngt);
}

static void
ftl_mngt_restore_valid_counters(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_valid_map_load_state(dev);
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_complete_unmap_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;

	dev->sb_shm->trim.in_progress = false;

	if (!status) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static void
ftl_mngt_complete_unmap(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	uint64_t start_lba, num_blocks, seq_id;
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];

	if (dev->sb_shm->trim.in_progress) {
		start_lba = dev->sb_shm->trim.start_lba;
		num_blocks = dev->sb_shm->trim.num_blocks;
		seq_id = dev->sb_shm->trim.seq_id;

		assert(seq_id <= dev->sb->seq_id);

		FTL_NOTICELOG(dev, "Incomplete unmap detected lba: %"PRIu64" num_blocks: %"PRIu64"\n",
			      start_lba, num_blocks);

		ftl_set_unmap_map(dev, start_lba, num_blocks, seq_id);
	}

	md->owner.cb_ctx = mngt;
	md->cb = ftl_mngt_complete_unmap_cb;

	ftl_md_persist(md);
}

static void
ftl_mngt_recover_unmap_map_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
	uint64_t num_md_blocks, first_page, num_pages;
	uint32_t lbas_in_page = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;
	uint64_t *page = ftl_md_get_buffer(md);
	union ftl_md_vss *page_vss = ftl_md_get_vss_buffer(md);
	uint64_t lba, num_blocks, vss_seq_id;
	size_t i, j;

	if (status) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	num_md_blocks = ftl_md_get_buffer_size(md) / FTL_BLOCK_SIZE;

	for (i = 0; i < num_md_blocks; ++i, page_vss++) {
		lba = page_vss->unmap.start_lba;
		num_blocks = page_vss->unmap.num_blocks;
		vss_seq_id = page_vss->unmap.seq_id;

		first_page = lba / lbas_in_page;
		num_pages = num_blocks / lbas_in_page;

		if (lba % lbas_in_page || num_blocks % lbas_in_page) {
			ftl_mngt_fail_step(mngt);
			return;
		}

		for (j = first_page; j < first_page + num_pages; ++j) {
			page[j] = spdk_max(vss_seq_id, page[j]);
		}
	}

	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_recover_unmap_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];

	if (ftl_fast_recovery(dev)) {
		FTL_DEBUGLOG(dev, "SHM: skipping unmap map recovery\n");
		ftl_mngt_next_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = ftl_mngt_recover_unmap_map_cb;
	ftl_md_restore(md);
}

static void
ftl_mngt_recovery_shm_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_recovery(dev)) {
		ftl_mngt_call_process(mngt, &g_desc_recovery_shm);
	} else {
		ftl_mngt_skip_step(mngt);
	}
}

/*
 * During dirty shutdown recovery, the whole L2P needs to be reconstructed. However,
 * recreating it all at the same time may take up to much DRAM, so it's done in multiple
 * iterations. This process describes the recovery of a part of L2P in one iteration.
 */
static const struct ftl_mngt_process_desc g_desc_recovery_iteration = {
	.name = "FTL recovery iteration",
	.steps = {
		{
			.name = "Load L2P",
			.action = ftl_mngt_recovery_iteration_load_l2p,
		},
		{
			.name = "Initialize sequence IDs",
			.action = ftl_mngt_recovery_iteration_init_seq_ids,
		},
		{
			.name = "Restore chunk L2P",
			.action = ftl_mngt_recovery_iteration_restore_chunk_l2p,
		},
		{
			.name = "Restore band L2P",
			.ctx_size = sizeof(struct band_md_ctx),
			.action = ftl_mngt_recovery_iteration_restore_band_l2p,
		},
		{
			.name = "Restore valid map",
			.action = ftl_mngt_recovery_iteration_restore_valid_map,
		},
		{
			.name = "Save L2P",
			.action = ftl_mngt_recovery_iteration_save_l2p,
		},
		{}
	}
};

/*
 * Loading of FTL after dirty shutdown. Recovers metadata, L2P, decides on amount of recovery
 * iterations to be executed (dependent on ratio of L2P cache size and total L2P size)
 */
static const struct ftl_mngt_process_desc g_desc_recovery = {
	.name = "FTL recovery",
	.ctx_size = sizeof(struct ftl_mngt_recovery_ctx),
	.steps = {
		{
			.name = "Initialize recovery",
			.action = ftl_mngt_recovery_init,
			.cleanup = ftl_mngt_recovery_deinit
		},
		{
			.name = "Recover band state",
			.action = ftl_mngt_recovery_restore_band_state,
		},
		{
			.name = "Initialize P2L checkpointing",
			.action = ftl_mngt_p2l_init_ckpt,
			.cleanup = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Restore P2L checkpoints",
			.action = ftl_mngt_p2l_restore_ckpt
		},
		{
			.name = "Preprocess P2L checkpoints",
			.action = ftl_mngt_recovery_pre_process_p2l
		},
		{
			.name = "Recover open bands P2L",
			.action = ftl_mngt_recovery_open_bands_p2l
		},
		{
			.name = "Recover chunk state",
			.action = ftl_mngt_nv_cache_restore_chunk_state
		},
		{
			.name = "Recover max seq ID",
			.action = ftl_mngt_recover_seq_id
		},
		{
			.name = "Recover unmap map",
			.action = ftl_mngt_recover_unmap_map
		},
		{
			.name = "Recover open chunks P2L",
			.action = ftl_mngt_nv_cache_recover_open_chunk
		},
		{
			.name = "Recovery iterations",
			.action = ftl_mngt_recovery_run_iteration,
		},
		{
			.name = "Deinitialize recovery",
			.action = ftl_mngt_recovery_deinit
		},
		{
			.name = "Initialize L2P",
			.action = ftl_mngt_init_l2p,
			.cleanup = ftl_mngt_deinit_l2p
		},
		{
			.name = "Recover L2P from shared memory",
			.action = ftl_mngt_recovery_shm_l2p,
		},
		{
			.name = "Finalize band initialization",
			.action = ftl_mngt_finalize_init_bands,
		},
		{
			.name = "Free P2L region bufs",
			.action = ftl_mngt_p2l_free_bufs,
		},
		{
			.name = "Start core poller",
			.action = ftl_mngt_start_core_poller,
			.cleanup = ftl_mngt_stop_core_poller
		},
		{
			.name = "Self test on startup",
			.action = ftl_mngt_self_test
		},
		{
			.name = "Finalize initialization",
			.action = ftl_mngt_finalize_startup,
		},
		{}
	}
};

/*
 * Shared memory specific steps for dirty shutdown recovery - main task is rebuilding the state of
 * L2P cache (paged in/out status, dirtiness etc. of individual pages).
 */
static const struct ftl_mngt_process_desc g_desc_recovery_shm = {
	.name = "FTL recovery from SHM",
	.ctx_size = sizeof(struct ftl_mngt_recovery_ctx),
	.steps = {
		{
			.name = "Restore L2P from SHM",
			.action = ftl_mngt_restore_l2p,
		},
		{
			.name = "Restore valid maps counters",
			.action = ftl_mngt_restore_valid_counters,
		},
		{
			.name = "Complete unmap transaction",
			.action = ftl_mngt_complete_unmap,
		},
		{}
	}
};

void
ftl_mngt_recover(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &g_desc_recovery);
}
