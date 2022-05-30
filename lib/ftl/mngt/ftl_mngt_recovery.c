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

struct ftl_mngt_recovery_context {
	/* Main recovery FTL management process */
	struct ftl_mngt *main;
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

static const struct ftl_mngt_process_desc desc_recovery_iteration;
static const struct ftl_mngt_process_desc desc_recovery;
static const struct ftl_mngt_process_desc desc_recovery_shm;

static bool recovery_iter_done(struct spdk_ftl_dev *dev,
			       struct ftl_mngt_recovery_context *ctx)
{
	return 0 == ctx->l2p_snippet.region.current.blocks;
}

static void recovery_iter_advance(struct spdk_ftl_dev *dev,
				  struct ftl_mngt_recovery_context *ctx)
{
	struct ftl_layout_region *region, *snippet;
	ctx->iter.i++;

	region = &dev->layout.region[ftl_layout_region_type_l2p];
	snippet = &ctx->l2p_snippet.region;

	/* Advance processed blocks */
	snippet->current.offset += snippet->current.blocks;
	snippet->current.blocks = region->current.offset + region->current.blocks - snippet->current.offset;
	snippet->current.blocks = spdk_min(snippet->current.blocks, ctx->iter.block_limit);

	uint64_t first_block = snippet->current.offset - region->current.offset;
	ctx->iter.lba_first = first_block * (FTL_BLOCK_SIZE / dev->layout.l2p.addr_size);

	uint64_t last_blocks = first_block + snippet->current.blocks;
	ctx->iter.lba_last = last_blocks * (FTL_BLOCK_SIZE / dev->layout.l2p.addr_size);

	if (ctx->iter.lba_last > dev->num_lbas) {
		ctx->iter.lba_last = dev->num_lbas;
	}
}

static void ftl_mngt_recovery_init(struct spdk_ftl_dev *dev,
				   struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *ctx = ftl_mngt_get_process_cntx(mngt);
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

	const uint64_t lbas_in_block = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;

	/* Below values are in byte unit */
	uint64_t mem_limit = dev->conf.l2p_dram_limit * MiB;
	mem_limit = spdk_min(mem_limit, spdk_divide_round_up(dev->num_lbas * dev->layout.l2p.addr_size,
			     MiB) * MiB);

	uint64_t lba_limit = mem_limit / (sizeof(uint64_t) + dev->layout.l2p.addr_size);
	uint64_t l2p_limit = lba_limit * dev->layout.l2p.addr_size;
	uint64_t iterations = spdk_divide_round_up(dev->num_lbas, lba_limit);

	ctx->iter.block_limit = spdk_divide_round_up(l2p_limit, FTL_BLOCK_SIZE);

	/* Round to block size */
	ctx->l2p_snippet.count = ctx->iter.block_limit * lbas_in_block;

	uint64_t seq_limit = ctx->l2p_snippet.count * sizeof(uint64_t);

	FTL_NOTICELOG(dev, "Recovery memory limit: %"PRIu64"MiB\n",
		      (uint64_t)(mem_limit / MiB));
	FTL_NOTICELOG(dev, "L2P resident size: %"PRIu64"MiB\n",
		      (uint64_t)(l2p_limit / MiB));
	FTL_NOTICELOG(dev, "Seq ID resident size: %"PRIu64"MiB\n",
		      (uint64_t)(seq_limit / MiB));
	FTL_NOTICELOG(dev, "Recovery iterations: %"PRIu64"\n", iterations);
	dev->sb->ckpt_seq_id = 0;

	/* Initialize region */
	ctx->l2p_snippet.region = dev->layout.region[ftl_layout_region_type_l2p];
	/* Limit blocks in region, it will be needed for ftl_md_set_region */
	ctx->l2p_snippet.region.current.blocks = ctx->iter.block_limit;

	uint64_t l2p_limit_block = ctx->iter.block_limit;
	uint64_t seq_limit_block = spdk_divide_round_up(seq_limit, FTL_BLOCK_SIZE);

	uint64_t md_blocks = l2p_limit_block + seq_limit_block;
	int md_flags = FTL_MD_CREATE_SHM | FTL_MD_CREATE_SHM_NEW | FTL_MD_CREATE_SHM_HUGE;

	/* Initialize snippet of L2P metadata */
	ctx->l2p_snippet.md = ftl_md_create(dev, md_blocks, 0, "l2p_recovery", md_flags);
	if (!ctx->l2p_snippet.md) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (ftl_md_set_region(ctx->l2p_snippet.md, &ctx->l2p_snippet.region)) {
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

static void ftl_mngt_recovery_deinit(struct spdk_ftl_dev *dev,
				     struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *ctx = ftl_mngt_get_process_cntx(mngt);

	ftl_md_destroy(ctx->l2p_snippet.md, 0);
	ctx->l2p_snippet.md = NULL;
	ctx->l2p_snippet.seq_id = NULL;

	ftl_mngt_next_step(mngt);
}

static void
recovery_iteration_cb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *ctx = ftl_mngt_get_caller_context(mngt);
	int status = ftl_mngt_get_status(mngt);

	recovery_iter_advance(dev, ctx);

	if (status) {
		ftl_mngt_fail_step(ctx->main);
	} else {
		ftl_mngt_continue_step(ctx->main);
	}
}

static void ftl_mngt_recovery_run_iteration(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *ctx = ftl_mngt_get_process_cntx(mngt);

	if (ftl_fast_recovery(dev)) {
		ftl_mngt_skip_step(mngt);
		return;
	}

	if (recovery_iter_done(dev, ctx)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_execute(dev, &desc_recovery_iteration,
				 recovery_iteration_cb, ctx);
	}
}

struct band_md_ctx {
	int status;
	uint64_t qd;
	uint64_t id;
};

static void
restore_band_state_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt *mngt = md->owner.cb_ctx;
	struct ftl_mngt_recovery_context *pctx = ftl_mngt_get_process_cntx(mngt);
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
			band->md->state = FTL_BAND_STATE_CLOSED;
			TAILQ_REMOVE(&band->dev->shut_bands, band, queue_entry);
			ftl_band_set_state(band, FTL_BAND_STATE_FREE);
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

static void ftl_mngt_recovery_restore_band_state(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_band_md];

	md->owner.cb_ctx = mngt;
	md->cb = restore_band_state_cb;
	ftl_md_restore(md);
}

static void ftl_mngt_recovery_walk_band_tail_md(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt, ftl_band_md_cb cb)
{
	struct band_md_ctx *sctx = ftl_mngt_get_step_cntx(mngt);
	uint64_t num_bands = ftl_get_num_bands(dev);

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
			/* This band is already open and have valid LBA map */
			sctx->id++;
			sctx->qd++;
			ftl_band_acquire_lba_map(band);
			cb(band, mngt, FTL_MD_SUCCESS);
			continue;
		} else {
			if (dev->sb->ckpt_seq_id && (band->md->close_seq_id <= dev->sb->ckpt_seq_id)) {
				sctx->id++;
				continue;
			}

			band->md->df_lba_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_lba_map(band)) {
				/* No more free LBA map, try later */
				break;
			}
		}

		sctx->id++;
		ftl_band_read_tail_brq_md(band, cb, mngt);
		sctx->qd++;
	}

	if (0 == sctx->qd) {
		/*
		 * No QD because of error, continue the step,
		 * it will be finished when all bands processed
		 */
		ftl_mngt_continue_step(mngt);
	}
}

static void
ftl_mngt_recovery_iteration_init_seq_ids(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *ctx = ftl_mngt_get_caller_context(mngt);
	size_t size = sizeof(ctx->l2p_snippet.seq_id[0]) * ctx->l2p_snippet.count;
	memset(ctx->l2p_snippet.seq_id, 0, size);

	ftl_mngt_next_step(mngt);
}

static void l2p_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt *mngt = md->owner.cb_ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void
ftl_mngt_recovery_iteration_load_l2p(struct spdk_ftl_dev *dev,
				     struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *ctx = ftl_mngt_get_caller_context(mngt);
	struct ftl_md *md = ctx->l2p_snippet.md;
	struct ftl_layout_region *region = &ctx->l2p_snippet.region;

	FTL_NOTICELOG(dev, "L2P recovery, iteration %u\n", ctx->iter.i);
	FTL_NOTICELOG(dev, "Load L2P, blocks [%"PRIu64", %"PRIu64"), "
		      "LBAs [%"PRIu64", %"PRIu64")\n",
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
ftl_mngt_recovery_iteration_save_l2p(struct spdk_ftl_dev *dev,
				     struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *ctx = ftl_mngt_get_caller_context(mngt);
	struct ftl_md *md = ctx->l2p_snippet.md;

	md->owner.cb_ctx = mngt;
	md->cb = l2p_cb;
	ftl_md_persist(md);
}

static void
restore_band_l2p_cb(struct ftl_band *band, void *cntx, enum ftl_md_status status)
{
	struct ftl_mngt *mngt = cntx;
	struct ftl_mngt_recovery_context *pctx = ftl_mngt_get_caller_context(mngt);
	struct band_md_ctx *sctx = ftl_mngt_get_step_cntx(mngt);
	struct spdk_ftl_dev *dev = band->dev;
	ftl_addr addr, curr_addr;
	uint64_t i, lba, seq_id;
	uint32_t band_map_crc;
	int rc = 0;

	if (status != FTL_MD_SUCCESS) {
		FTL_ERRLOG(dev, "L2P band restore error, failed to read LBA map\n");
		rc = -EIO;
		goto cleanup;
	}

	band_map_crc = spdk_crc32c_update(band->lba_map.dma_buf,
					  ftl_tail_md_num_blocks(band->dev) * FTL_BLOCK_SIZE, 0);

	/* LBA map is only valid if the band state is closed - additionally checking
	 * for non-zero value helps with upgrade path and shared memory recovery */
	if (FTL_BAND_STATE_CLOSED == band->md->state && band->md->lba_map_checksum &&
	    band->md->lba_map_checksum != band_map_crc) {
		FTL_ERRLOG(dev, "L2P band restore error, inconsistent LBA map CRC\n");
		rc = -EINVAL;
		goto cleanup;
	}

	uint64_t num_blks_in_band = ftl_get_num_blocks_in_band(dev);
	for (i = 0; i < num_blks_in_band; ++i) {
		lba = band->lba_map.band_map[i].lba;
		seq_id = band->lba_map.band_map[i].seq_id;

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

		uint64_t lba_off = lba - pctx->iter.lba_first;
		if (seq_id < pctx->l2p_snippet.seq_id[lba_off]) {

			/* Overlapped band/chunk has newer data - invalidate lba map on open/full band  */
			if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
				addr = ftl_band_addr_from_block_offset(band, i);
				ftl_band_set_p2l(band, FTL_LBA_INVALID, addr, 0);
			}

			/* Newer data already recovered */
			continue;
		}

		addr = ftl_band_addr_from_block_offset(band, i);

		curr_addr = ftl_addr_load(dev, pctx->l2p_snippet.l2p, lba_off);

		/* Overlapped band/chunk has newer data - invalidate lba map on open/full band  */
		if (curr_addr != FTL_ADDR_INVALID && !ftl_addr_cached(dev, curr_addr) && curr_addr != addr) {

			struct ftl_band *curr_band = ftl_band_from_addr(dev, curr_addr);

			if (FTL_BAND_STATE_OPEN == curr_band->md->state || FTL_BAND_STATE_FULL == curr_band->md->state) {
				size_t prev_offset = ftl_band_block_offset_from_addr(curr_band, curr_addr);
				if (curr_band->lba_map.band_map[prev_offset].lba == lba &&
				    seq_id >= curr_band->lba_map.band_map[prev_offset].seq_id) {
					ftl_band_set_p2l(curr_band, FTL_LBA_INVALID, curr_addr, 0);
				}
			}
		}

		ftl_addr_store(dev, pctx->l2p_snippet.l2p, lba_off, addr);
		pctx->l2p_snippet.seq_id[lba_off] = seq_id;
	}


cleanup:
	ftl_band_release_lba_map(band);

	sctx->qd--;
	if (rc) {
		sctx->status = rc;
	}

	ftl_mngt_continue_step(mngt);
}

static void
ftl_mngt_recovery_iteration_restore_band_l2p(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	ftl_mngt_recovery_walk_band_tail_md(dev, mngt, restore_band_l2p_cb);
}

static void
ftl_mngt_recovery_iteration_restore_valid_map(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *pctx = ftl_mngt_get_caller_context(mngt);
	uint64_t lba, lba_off;
	ftl_addr addr;

	for (lba = pctx->iter.lba_first; lba < pctx->iter.lba_last; lba++) {
		lba_off = lba - pctx->iter.lba_first;
		addr = ftl_addr_load(dev, pctx->l2p_snippet.l2p, lba_off);

		if (addr == FTL_ADDR_INVALID) {
			continue;
		}

		if (!ftl_addr_cached(dev, addr)) {
			struct ftl_band *band = ftl_band_from_addr(dev, addr);
			band->lba_map.num_vld++;
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
p2l_ckpt_preprocess(struct spdk_ftl_dev *dev, struct ftl_mngt_recovery_context *pctx)
{
	uint64_t seq_id;
	int md_region, ckpt_id;

	for (md_region = ftl_layout_region_type_p2l_ckpt_min;
	     md_region <= ftl_layout_region_type_p2l_ckpt_max; md_region++) {
		ckpt_id = md_region - ftl_layout_region_type_p2l_ckpt_min;
		seq_id = ftl_mngt_p2l_ckpt_get_seq_id(dev, md_region);
		pctx->p2l_ckpt_seq_id[ckpt_id] = seq_id;
		FTL_NOTICELOG(dev, "P2L ckpt_id=%d found seq_id=%"PRIu64"\n", ckpt_id, seq_id);
	}
}

static int
p2l_ckpt_restore_p2l(struct ftl_mngt_recovery_context *pctx, struct ftl_band *band)
{
	uint64_t seq_id;
	int md_region, ckpt_id;

	memset(band->lba_map.band_map, -1,
	       FTL_BLOCK_SIZE * ftl_lba_map_num_blocks(band->dev));

	for (md_region = ftl_layout_region_type_p2l_ckpt_min;
	     md_region <= ftl_layout_region_type_p2l_ckpt_max; md_region++) {
		ckpt_id = md_region - ftl_layout_region_type_p2l_ckpt_min;
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
ftl_mngt_recovery_pre_process_p2l(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *pctx = ftl_mngt_get_process_cntx(mngt);
	p2l_ckpt_preprocess(dev, pctx);
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_recover_seq_id(struct spdk_ftl_dev *dev,
			struct ftl_mngt *mngt)
{
	ftl_recover_max_seq(dev);
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_recovery_open_bands_p2l(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_mngt_recovery_context *pctx = ftl_mngt_get_process_cntx(mngt);
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

	if (!ftl_mngt_get_step_cntx(mngt)) {
		ftl_mngt_alloc_step_cntx(mngt, sizeof(bool));

		/* Step first time called, initialize */
		TAILQ_FOREACH(band, &pctx->open_bands, queue_entry) {
			band->md->df_lba_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_lba_map(band)) {
				FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot allocate LBA map\n");
				ftl_mngt_fail_step(mngt);
				return;
			}

			if (p2l_ckpt_restore_p2l(pctx, band)) {
				FTL_ERRLOG(dev, "Open band recovery ERROR, Cannot restore P2L\n");
				ftl_mngt_fail_step(mngt);
				return;
			}

			if (!band->lba_map.p2l_ckpt) {
				band->lba_map.p2l_ckpt = ftl_p2l_ckpt_acquire_region_type(dev,
							 band->md->p2l_md_region);
				if (!band->lba_map.p2l_ckpt) {
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

	FTL_NOTICELOG(dev, "Open band recovered, id = %u, seq id %"PRIu64", "
		      "write offset %"PRIu64"\n",
		      band->id, band->md->seq, band->md->iter.offset);

	ftl_mngt_continue_step(mngt);
}

static void
ftl_mngt_restore_valid_counters(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_valid_map_load_state(dev);
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_recovery_shm_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_fast_recovery(dev)) {
		ftl_mngt_call(mngt, &desc_recovery_shm);
	} else {
		ftl_mngt_skip_step(mngt);
	}
}

static const struct ftl_mngt_process_desc desc_recovery_iteration = {
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
			.name = "Restore band L2P",
			.arg_size = sizeof(struct band_md_ctx),
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

static const struct ftl_mngt_process_desc desc_recovery = {
	.name = "FTL recovery",
	.arg_size = sizeof(struct ftl_mngt_recovery_context),
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
			.name = "Start task core",
			.action = ftl_mngt_start_task_core,
			.cleanup = ftl_mngt_stop_task_core
		},
		{
			.name = "Self test on startup",
			.action = ftl_mngt_self_test
		},
		{
			.name = "Finalize initialization",
			.action = ftl_mngt_finalize_init,
		},
		{}
	}
};

static const struct ftl_mngt_process_desc desc_recovery_shm = {
	.name = "FTL recovery from SHM",
	.arg_size = sizeof(struct ftl_mngt_recovery_context),
	.steps = {
		{
			.name = "Restore L2P from SHM",
			.action = ftl_mngt_restore_l2p,
		},
		{
			.name = "Restore valid maps counters",
			.action = ftl_mngt_restore_valid_counters,
		},
		{}
	}
};

void ftl_mngt_recover(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_mngt_call(mngt, &desc_recovery);
}
