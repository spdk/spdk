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

#include "spdk/ftl.h"
#include "ftl_debug.h"
#include "ftl_band.h"

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

#define FTL_MD_VALIDATE_PIN_QD 128

static void
ftl_band_validate_md_pin(struct ftl_band_validate_ctx *ctx)
{
	struct ftl_band *band = ctx->band;
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;
	size_t i, size;
	struct ftl_l2p_pin_ctx tmp_pin_ctx = {
		.cb_ctx = ctx
	};

	size = spdk_min(FTL_MD_VALIDATE_PIN_QD,
			ftl_get_num_blocks_in_band(dev) - ctx->current_offset);

	for (i = ctx->current_offset; i < ctx->current_offset + size; ++i) {
		if (!ftl_bitmap_get(lba_map->vld, i)) {
			ctx->l2p_pin_ctx[i].lba = FTL_LBA_INVALID;
			continue;
		}

		assert(lba_map->band_map[i].lba != FTL_LBA_INVALID);
		ctx->remaining++;
		ctx->pin_cnt++;
		ftl_l2p_pin(dev, lba_map->band_map[i].lba, 1, ftl_band_validate_md_l2p_pin_cb, ctx,
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

	size = spdk_min(FTL_MD_VALIDATE_PIN_QD,
			ftl_get_num_blocks_in_band(dev) - ctx->current_offset);

	for (i = ctx->current_offset; i < ctx->current_offset + size; ++i) {
		lba = ctx->l2p_pin_ctx[i].lba;
		if (lba == FTL_LBA_INVALID) {
			continue;
		}

		if (ftl_bitmap_get(band->lba_map.vld, i)) {
			addr_l2p = ftl_l2p_get(dev, lba);

			if (addr_l2p != FTL_ADDR_INVALID && !ftl_addr_cached(dev, addr_l2p) &&
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

	ctx->remaining = 1;
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

	if (!band->num_zones) {
		cb(band, true);
		return;
	}

	size = ftl_get_num_blocks_in_band(band->dev);

	ctx = malloc(sizeof(*ctx) + size * sizeof(*ctx->l2p_pin_ctx));
	assert(ctx);
	ctx->band = band;
	ctx->cb = cb;
	ctx->remaining = 1;
	ctx->pin_cnt = 0;
	ctx->current_offset = 0;

	ftl_band_validate_md_pin(ctx);
}

void
ftl_dev_dump_bands(struct spdk_ftl_dev *dev)
{
	size_t i, total = 0;

	if (!dev->bands) {
		return;
	}

	ftl_debug(dev, "Bands validity:\n");
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		if (!dev->bands[i].num_zones) {
			ftl_debug(dev, " Band %3zu: all zones are offline\n", i + 1);
			continue;
		}

		total += dev->bands[i].lba_map.num_vld;
		ftl_debug(dev, " Band %3zu: %8zu / %zu \tnum_zones: %zu \twr_cnt: %"PRIu64
			  "\tstate: %s\n",
			  i + 1, dev->bands[i].lba_map.num_vld,
			  ftl_band_user_blocks(&dev->bands[i]),
			  dev->bands[i].num_zones,
			  dev->bands[i].md->wr_cnt,
			  ftl_band_state_str[dev->bands[i].md->state]);
	}
}

#endif /* defined(DEBUG) */

void
ftl_dev_dump_stats(const struct spdk_ftl_dev *dev)
{
	size_t i, total = 0;
	char uuid[SPDK_UUID_STRING_LEN];

	if (!dev->bands) {
		return;
	}

	/* Count the number of valid LBAs */
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		total += dev->bands[i].lba_map.num_vld;
	}

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->uuid);
	FTL_NOTICELOG(dev, "\n");
	FTL_NOTICELOG(dev, "device UUID:         %s\n", uuid);
	FTL_NOTICELOG(dev, "total valid LBAs:    %zu\n", total);
}
