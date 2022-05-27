/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
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
};

static const struct ftl_mngt_process_desc g_desc_recovery;

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

static void
ftl_mngt_recover_seq_id(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_recover_max_seq(dev);
	ftl_mngt_next_step(mngt);
}

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
			.name = "Recover max seq ID",
			.action = ftl_mngt_recover_seq_id
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
			.name = "Finalize band initialization",
			.action = ftl_mngt_finalize_init_bands,
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

void
ftl_mngt_recover(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &g_desc_recovery);
}
