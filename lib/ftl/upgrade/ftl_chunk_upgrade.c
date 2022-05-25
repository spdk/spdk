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

#include "ftl_nv_cache.h"
#include "ftl_layout_upgrade.h"

struct ftl_nv_cache_chunk_md_v0 {
	/* Sequence id of writing */
	uint64_t seq_id;

	/* Sequence ID when chunk was closed */
	uint64_t close_seq_id;

	/* Current lba to write */
	uint32_t write_pointer;

	/* Number of blocks written */
	uint32_t blocks_written;

	/* Number of skipped block (case when IO size is greater than blocks left in chunk) */
	uint32_t blocks_skipped;

	/* Current compacted block */
	uint32_t read_pointer;

	/* Number of compacted blocks */
	uint32_t blocks_compacted;

	/* Chunk state */
	enum ftl_chunk_state state;
} __attribute__((aligned(FTL_BLOCK_SIZE)));

struct nvc_v0_to_v1_ctx {
	uint64_t current_nvc;
	struct ftl_md_io_entry_ctx	md_entry_ctx;
	struct ftl_layout_region	*region;
	struct spdk_ftl_dev			*dev;
	struct ftl_nv_cache_chunk_md *nvc_meta;
	union ftl_md_vss			*vss_md_page;
	void					 	*lba_map;
};

static void upgrade_v0_to_v1_read_cb(int status, void *cb_arg);
static void upgrade_v0_to_v1_write_cb(int status, void *cb_arg);

static void free_ctx(struct nvc_v0_to_v1_ctx *nvc_ctx)
{
	spdk_dma_free(nvc_ctx->nvc_meta);
	spdk_dma_free(nvc_ctx->vss_md_page);
	spdk_dma_free(nvc_ctx->lba_map);
}

static void read_next_nvc_entry(struct ftl_layout_upgrade_ctx *layout_ctx)
{
	struct nvc_v0_to_v1_ctx *nvc_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = nvc_ctx->dev;
	struct ftl_layout_region *region = nvc_ctx->region;
	uint64_t region_translated = nvc_ctx->current_nvc * region->entry_size;
	struct ftl_nv_cache_chunk_md *nvc_md = nvc_ctx->nvc_meta;


	if (nvc_ctx->current_nvc < region->num_entries) {
		// Not all entries upgraded, get the next one
		ftl_md_read_entry(dev->layout.md[region->type], nvc_ctx->current_nvc, nvc_ctx->nvc_meta,
				nvc_ctx->vss_md_page, upgrade_v0_to_v1_read_cb, layout_ctx, &nvc_ctx->md_entry_ctx);
	} else if (region_translated < region->current.blocks) {
		// We've upgraded all the entries we want, but due to alignment there's still part of the region
		// where we need to bump the version in VSS. Clear the main/VSS buffer otherwise
		memset(nvc_md, 0, sizeof(struct ftl_nv_cache_chunk_md));
		memset(nvc_ctx->vss_md_page, 0, sizeof(union ftl_md_vss));
		nvc_ctx->vss_md_page->version.md_version = FTL_NVC_VERSION_1;

		ftl_md_persist_entry(dev->layout.md[region->type], nvc_ctx->current_nvc, nvc_md,
				nvc_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &nvc_ctx->md_entry_ctx);
	} else {
		// All done
		free_ctx(nvc_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, 0);
	}
}

static void upgrade_v0_to_v1_write_cb(int status, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct nvc_v0_to_v1_ctx *nvc_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = nvc_ctx->dev;

	if (spdk_unlikely(status)) {
		free_ctx(nvc_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, status);
		return;
	}

	nvc_ctx->current_nvc++;
	read_next_nvc_entry(layout_ctx);
}

static void upgrade_v0_to_v1_lba_map_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct nvc_v0_to_v1_ctx *nvc_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = nvc_ctx->dev;
	struct ftl_layout_region *region = nvc_ctx->region;
	struct ftl_nv_cache_chunk_md *nvc_md = nvc_ctx->nvc_meta;

	if (spdk_unlikely(!success)) {
		free_ctx(nvc_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, -EIO);
		spdk_bdev_free_io(bdev_io);
		return;
	}

	nvc_md->lba_map_checksum = spdk_crc32c_update(nvc_ctx->lba_map,
			ftl_nv_cache_chunk_tail_md_num_blocks(&dev->nv_cache) * FTL_BLOCK_SIZE, 0);
	nvc_ctx->vss_md_page->version.md_version = FTL_NVC_VERSION_1;

	ftl_md_persist_entry(dev->layout.md[region->type], nvc_ctx->current_nvc, nvc_md,
			nvc_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &nvc_ctx->md_entry_ctx);
	spdk_bdev_free_io(bdev_io);
}

static void upgrade_v0_to_v1_read_cb(int status, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct nvc_v0_to_v1_ctx *nvc_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = nvc_ctx->dev;
	struct ftl_layout_region *region = nvc_ctx->region;
	struct ftl_nv_cache_chunk_md *nvc_md = nvc_ctx->nvc_meta;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;

	if (spdk_unlikely(status)) {
		free_ctx(nvc_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, status);
		return;
	}

	if (nvc_ctx->vss_md_page->version.md_version == FTL_NVC_VERSION_1) {
		// already upgraded, nothing to be done
		nvc_ctx->current_nvc++;
		read_next_nvc_entry(layout_ctx);
	} else if (nvc_md->state != FTL_CHUNK_STATE_CLOSED) {
		// not upgraded, but no need to recalculate CRC, update the version and move on
		nvc_md->lba_map_checksum = 0;
		nvc_ctx->vss_md_page->version.md_version = FTL_NVC_VERSION_1;

		ftl_md_persist_entry(dev->layout.md[region->type], nvc_ctx->current_nvc, nvc_md,
				nvc_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &nvc_ctx->md_entry_ctx);
	} else {
		// not upgraded, need to read the LBA map
		struct ftl_nv_cache_chunk *chunk = &nv_cache->chunks[nvc_ctx->current_nvc];

		status = spdk_bdev_read_blocks(nv_cache->bdev_desc, nv_cache->cache_ioch,
					   nvc_ctx->lba_map, chunk->offset + chunk_tail_md_offset(nv_cache),
					   ftl_nv_cache_chunk_tail_md_num_blocks(&dev->nv_cache), upgrade_v0_to_v1_lba_map_cb, layout_ctx);

		if (status) {
			free_ctx(nvc_ctx);
			ftl_region_upgrade_completed(dev, layout_ctx, status);
		}
	}
}


static int
ftl_nvc_upgrade_v0_to_v1(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *layout_ctx)
{
	struct nvc_v0_to_v1_ctx *nvc_ctx = layout_ctx->ctx;

	nvc_ctx->dev = dev;
	nvc_ctx->region = layout_ctx->reg;
	nvc_ctx->current_nvc = 0;
	nvc_ctx->nvc_meta = spdk_zmalloc(sizeof(struct ftl_nv_cache_chunk_md), FTL_BLOCK_SIZE, NULL,
			   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	nvc_ctx->vss_md_page = ftl_md_vss_buf_alloc(layout_ctx->reg, 1);
	nvc_ctx->lba_map = spdk_zmalloc(ftl_nv_cache_chunk_tail_md_num_blocks(&dev->nv_cache) * FTL_BLOCK_SIZE, FTL_BLOCK_SIZE,
			   NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (!nvc_ctx->nvc_meta || !nvc_ctx->vss_md_page || !nvc_ctx->lba_map) {
		free_ctx(nvc_ctx);
		return -ENOMEM;
	}

	read_next_nvc_entry(layout_ctx);
	return 0;
}

struct ftl_region_upgrade_desc nvc_upgrade_desc[] = {
	[FTL_NVC_VERSION_0] = {.verify = ftl_region_upgrade_enabled,
		.upgrade = ftl_nvc_upgrade_v0_to_v1,
		.new_version = FTL_NVC_VERSION_1,
		.ctx_size = sizeof (struct nvc_v0_to_v1_ctx)},
};

SPDK_STATIC_ASSERT(sizeof(nvc_upgrade_desc) / sizeof(*nvc_upgrade_desc) == FTL_NVC_VERSION_CURRENT,
	"Missing NVC region upgrade descriptors");
