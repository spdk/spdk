/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/ftl.h"
#include "ftl_debug.h"
#include "ftl_band.h"

/* TODO: Switch to INFOLOG instead, we can control the printing via spdk_log_get_flag */
#if defined(DEBUG)

static const char *ftl_band_state_str[] = {
	"free",
	"prep",
	"opening",
	"open",
	"full",
	"closing",
	"closed",
	"max"
};

struct ftl_band_validate_ctx {
	struct ftl_band *band;
	ftl_band_validate_md_cb cb;
	int remaining;
	uint64_t pin_cnt;
	uint64_t current_offset;
	struct ftl_l2p_pin_ctx l2p_pin_ctx[];
};

static void ftl_band_validate_md_l2p_pin_cb(struct spdk_ftl_dev *dev, int status,
		struct ftl_l2p_pin_ctx *pin_ctx);

#define FTL_MD_VALIDATE_LBA_PER_ITERATION 128

static void
ftl_band_validate_md_pin(struct ftl_band_validate_ctx *ctx)
{
	struct ftl_band *band = ctx->band;
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	size_t i, size;
	struct ftl_l2p_pin_ctx tmp_pin_ctx = {
		.cb_ctx = ctx
	};

	/* Since the first L2P page may already be pinned, the ftl_band_validate_md_l2p_pin_cb could be prematurely
	 * triggered. Initializing to 1 and then triggering the callback again manually prevents the issue.
	 */
	ctx->remaining = 1;
	size = spdk_min(FTL_MD_VALIDATE_LBA_PER_ITERATION,
			ftl_get_num_blocks_in_band(dev) - ctx->current_offset);

	for (i = ctx->current_offset; i < ctx->current_offset + size; ++i) {
		if (!ftl_bitmap_get(p2l_map->valid, i)) {
			ctx->l2p_pin_ctx[i].lba = FTL_LBA_INVALID;
			continue;
		}

		assert(p2l_map->band_map[i].lba != FTL_LBA_INVALID);
		ctx->remaining++;
		ctx->pin_cnt++;
		ftl_l2p_pin(dev, p2l_map->band_map[i].lba, 1, ftl_band_validate_md_l2p_pin_cb, ctx,
			    &ctx->l2p_pin_ctx[i]);
	}

	ftl_band_validate_md_l2p_pin_cb(dev, 0, &tmp_pin_ctx);
}

static void
_ftl_band_validate_md(void *_ctx)
{
	struct ftl_band_validate_ctx *ctx = _ctx;
	struct ftl_band *band = ctx->band;
	struct spdk_ftl_dev *dev = band->dev;
	ftl_addr addr_l2p;
	size_t i, size;
	bool valid = true;
	uint64_t lba;

	size = spdk_min(FTL_MD_VALIDATE_LBA_PER_ITERATION,
			ftl_get_num_blocks_in_band(dev) - ctx->current_offset);

	for (i = ctx->current_offset; i < ctx->current_offset + size; ++i) {
		lba = ctx->l2p_pin_ctx[i].lba;
		if (lba == FTL_LBA_INVALID) {
			continue;
		}

		if (ftl_bitmap_get(band->p2l_map.valid, i)) {
			addr_l2p = ftl_l2p_get(dev, lba);

			if (addr_l2p != FTL_ADDR_INVALID && !ftl_addr_in_nvc(dev, addr_l2p) &&
			    addr_l2p != ftl_band_addr_from_block_offset(band, i)) {
				valid = false;
			}
		}

		ctx->pin_cnt--;
		ftl_l2p_unpin(dev, lba, 1);
	}
	assert(ctx->pin_cnt == 0);

	ctx->current_offset += size;

	if (ctx->current_offset == ftl_get_num_blocks_in_band(dev)) {
		ctx->cb(band, valid);
		free(ctx);
		return;
	}

	ftl_band_validate_md_pin(ctx);
}

static void
ftl_band_validate_md_l2p_pin_cb(struct spdk_ftl_dev *dev, int status,
				struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_band_validate_ctx *ctx = pin_ctx->cb_ctx;

	assert(status == 0);

	if (--ctx->remaining == 0) {
		spdk_thread_send_msg(dev->core_thread, _ftl_band_validate_md, ctx);
	}
}

void
ftl_band_validate_md(struct ftl_band *band, ftl_band_validate_md_cb cb)
{
	struct ftl_band_validate_ctx *ctx;
	size_t size;

	assert(cb);

	size = ftl_get_num_blocks_in_band(band->dev);

	ctx = malloc(sizeof(*ctx) + size * sizeof(*ctx->l2p_pin_ctx));

	if (!ctx) {
		FTL_ERRLOG(band->dev, "Failed to allocate memory for band validate context");
		cb(band, false);
		return;
	}

	ctx->band = band;
	ctx->cb = cb;
	ctx->pin_cnt = 0;
	ctx->current_offset = 0;

	ftl_band_validate_md_pin(ctx);
}

void
ftl_dev_dump_bands(struct spdk_ftl_dev *dev)
{
	uint64_t i;

	if (!dev->bands) {
		return;
	}

	FTL_NOTICELOG(dev, "Bands validity:\n");
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		FTL_NOTICELOG(dev, " Band %3zu: %8zu / %zu \twr_cnt: %"PRIu64
			      "\tstate: %s\n",
			      i + 1, dev->bands[i].p2l_map.num_valid,
			      ftl_band_user_blocks(&dev->bands[i]),
			      dev->bands[i].md->wr_cnt,
			      ftl_band_state_str[dev->bands[i].md->state]);
	}
}

#endif /* defined(DEBUG) */

void
ftl_dev_dump_stats(const struct spdk_ftl_dev *dev)
{
	uint64_t i, total = 0;
	char uuid[SPDK_UUID_STRING_LEN];
	double waf;
	uint64_t write_user, write_total;
	const char *limits[] = {
		[SPDK_FTL_LIMIT_CRIT]  = "crit",
		[SPDK_FTL_LIMIT_HIGH]  = "high",
		[SPDK_FTL_LIMIT_LOW]   = "low",
		[SPDK_FTL_LIMIT_START] = "start"
	};

	(void)limits;

	if (!dev->bands) {
		return;
	}

	/* Count the number of valid LBAs */
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		total += dev->bands[i].p2l_map.num_valid;
	}

	write_user = dev->stats.entries[FTL_STATS_TYPE_CMP].write.blocks;
	write_total = write_user +
		      dev->stats.entries[FTL_STATS_TYPE_GC].write.blocks +
		      dev->stats.entries[FTL_STATS_TYPE_MD_BASE].write.blocks;

	waf = (double)write_total / (double)write_user;

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->conf.uuid);
	FTL_NOTICELOG(dev, "\n");
	FTL_NOTICELOG(dev, "device UUID:         %s\n", uuid);
	FTL_NOTICELOG(dev, "total valid LBAs:    %zu\n", total);
	FTL_NOTICELOG(dev, "total writes:        %"PRIu64"\n", write_total);
	FTL_NOTICELOG(dev, "user writes:         %"PRIu64"\n", write_user);
	FTL_NOTICELOG(dev, "WAF:                 %.4lf\n", waf);
#ifdef DEBUG
	FTL_NOTICELOG(dev, "limits:\n");
	for (i = 0; i < SPDK_FTL_LIMIT_MAX; ++i) {
		FTL_NOTICELOG(dev, " %5s: %"PRIu64"\n", limits[i], dev->stats.limits[i]);
	}
#endif
}
