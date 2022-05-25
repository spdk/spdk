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

#include "ftl_band.h"
#include "ftl_layout_upgrade.h"

struct ftl_band_md_v0 {
	/* Band iterator for writing */
	struct {
		/* Current address */
		ftl_addr			addr;

		/* Current logical block's offset */
		uint64_t			offset;
	} iter;

	/* Band's state */
	enum ftl_band_state			state;

	/* Band type set during opening */
	enum ftl_band_type			type;

	/* nv_cache p2l md region associated with band */
	enum ftl_layout_region_type		p2l_md_region;

	/* Sequence number */
	uint64_t				seq;

	/* Sequence ID when band was closed */
	uint64_t				close_seq_id;

	/* Number of defrag cycles */
	uint64_t				wr_cnt;

	/* Df object id for LBA map */
	ftl_df_obj_id			df_lba_map;
} __attribute__((aligned(FTL_BLOCK_SIZE)));

struct band_v0_to_v1_ctx {
	uint64_t current_band;
	struct ftl_md_io_entry_ctx	md_entry_ctx;
	struct ftl_layout_region *region;
	struct spdk_ftl_dev		 *dev;
	struct ftl_band_md		 *band_meta;
	union ftl_md_vss		 *vss_md_page;
	void					 *lba_map;
};

static void upgrade_v0_to_v1_read_cb(int status, void *cb_arg);
static void upgrade_v0_to_v1_write_cb(int status, void *cb_arg);

static void free_ctx(struct band_v0_to_v1_ctx *band_ctx)
{
	spdk_dma_free(band_ctx->band_meta);
	spdk_dma_free(band_ctx->vss_md_page);
	spdk_dma_free(band_ctx->lba_map);
}

static void read_next_band_entry(struct ftl_layout_upgrade_ctx *layout_ctx)
{
	struct band_v0_to_v1_ctx *band_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = band_ctx->dev;
	struct ftl_layout_region *region = band_ctx->region;
	uint64_t region_translated = band_ctx->current_band * region->entry_size;
	struct ftl_band_md *band_md = band_ctx->band_meta;


	if (band_ctx->current_band < region->num_entries) {
		// Not all entries upgraded, get the next one
		ftl_md_read_entry(dev->layout.md[region->type], band_ctx->current_band, band_ctx->band_meta,
				band_ctx->vss_md_page, upgrade_v0_to_v1_read_cb, layout_ctx, &band_ctx->md_entry_ctx);
	} else if (region_translated < region->current.blocks) {
		// We've upgraded all the entries we want, but due to alignment there's still part of the region
		// where we need to bump the version in VSS. Clear the main/VSS buffer otherwise
		memset(band_md, 0, sizeof(struct ftl_band_md));
		memset(band_ctx->vss_md_page, 0, sizeof(union ftl_md_vss));
		band_ctx->vss_md_page->version.md_version = FTL_BAND_VERSION_1;

		ftl_md_persist_entry(dev->layout.md[region->type], band_ctx->current_band, band_md,
				band_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &band_ctx->md_entry_ctx);
	} else {
		// All done
		free_ctx(band_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, 0);
	}
}

static void upgrade_v0_to_v1_write_cb(int status, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct band_v0_to_v1_ctx *band_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = band_ctx->dev;

	if (spdk_unlikely(status)) {
		free_ctx(band_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, status);
		return;
	}

	band_ctx->current_band++;
	read_next_band_entry(layout_ctx);
}

static void upgrade_v0_to_v1_lba_map_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct band_v0_to_v1_ctx *band_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = band_ctx->dev;
	struct ftl_layout_region *region = band_ctx->region;
	struct ftl_band_md *band_md = band_ctx->band_meta;

	if (spdk_unlikely(!success)) {
		free_ctx(band_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, -EIO);
		spdk_bdev_free_io(bdev_io);
		return;
	}

	band_md->lba_map_checksum = spdk_crc32c_update(band_ctx->lba_map,
			ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE, 0);
	band_ctx->vss_md_page->version.md_version = FTL_BAND_VERSION_1;

	ftl_md_persist_entry(dev->layout.md[region->type], band_ctx->current_band, band_md,
			band_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &band_ctx->md_entry_ctx);
	spdk_bdev_free_io(bdev_io);
}

static void upgrade_v0_to_v1_read_cb(int status, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct band_v0_to_v1_ctx *band_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = band_ctx->dev;
	struct ftl_layout_region *region = band_ctx->region;
	struct ftl_band_md *band_meta = band_ctx->band_meta;

	if (spdk_unlikely(status)) {
		free_ctx(band_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, status);
		return;
	}

	if (band_ctx->vss_md_page->version.md_version == FTL_BAND_VERSION_1) {
		// already upgraded, nothing to be done
		band_ctx->current_band++;
		read_next_band_entry(layout_ctx);
	} else if (band_meta->state != FTL_BAND_STATE_CLOSED) {
		// not upgraded, but no need to recalculate CRC, update the version and move on
		band_meta->lba_map_checksum = 0;
		band_ctx->vss_md_page->version.md_version = FTL_BAND_VERSION_1;

		ftl_md_persist_entry(dev->layout.md[region->type], band_ctx->current_band, band_meta,
				band_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &band_ctx->md_entry_ctx);
	} else {
		// not upgraded, need to read the LBA map
		struct ftl_band *band = &dev->bands[band_ctx->current_band];

		status = spdk_bdev_read_blocks(dev->base_bdev_desc, dev->base_ioch,
					   band_ctx->lba_map, band->tail_md_addr,
					   ftl_tail_md_num_blocks(dev), upgrade_v0_to_v1_lba_map_cb, layout_ctx);

		if (status) {
			free_ctx(band_ctx);
			ftl_region_upgrade_completed(dev, layout_ctx, status);
		}
	}
}


static int
ftl_band_upgrade_v0_to_v1(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *layout_ctx)
{
	struct band_v0_to_v1_ctx *band_ctx = layout_ctx->ctx;

	band_ctx->dev = dev;
	band_ctx->region = layout_ctx->reg;
	band_ctx->current_band = 0;
	band_ctx->band_meta = spdk_zmalloc(sizeof(struct ftl_band_md), FTL_BLOCK_SIZE, NULL,
			   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	band_ctx->vss_md_page = ftl_md_vss_buf_alloc(layout_ctx->reg, 1);
	band_ctx->lba_map = spdk_zmalloc(ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE, FTL_BLOCK_SIZE,
			   NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (!band_ctx->band_meta || !band_ctx->vss_md_page || !band_ctx->lba_map) {
		free_ctx(band_ctx);
		return -ENOMEM;
	}

	read_next_band_entry(layout_ctx);
	return 0;
}

struct ftl_region_upgrade_desc band_upgrade_desc[] = {
	[FTL_BAND_VERSION_0] = {.verify = ftl_region_upgrade_enabled,
		.upgrade = ftl_band_upgrade_v0_to_v1,
		.new_version = FTL_BAND_VERSION_1,
		.ctx_size = sizeof (struct band_v0_to_v1_ctx)},
};

SPDK_STATIC_ASSERT(sizeof(band_upgrade_desc) / sizeof(*band_upgrade_desc) == FTL_BAND_VERSION_CURRENT,
	"Missing band region upgrade descriptors");
