/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/ftl.h"
#include "spdk/string.h"

#include "ftl_nv_cache.h"
#include "ftl_nv_cache_io.h"
#include "ftl_core.h"
#include "ftl_band.h"
#include "utils/ftl_addr_utils.h"
#include "utils/ftl_defs.h"
#include "mngt/ftl_mngt.h"

static inline uint64_t nvc_data_blocks(struct ftl_nv_cache *nv_cache) __attribute__((unused));
static struct ftl_nv_cache_compactor *compactor_alloc(struct spdk_ftl_dev *dev);
static void compactor_free(struct spdk_ftl_dev *dev, struct ftl_nv_cache_compactor *compactor);
static void compaction_process_ftl_done(struct ftl_rq *rq);
static void compaction_process_read_entry(void *arg);
static void ftl_property_dump_cache_dev(struct spdk_ftl_dev *dev,
					const struct ftl_property *property,
					struct spdk_json_write_ctx *w);
static void read_chunk_p2l_map(void *arg);

static inline void
nvc_validate_md(struct ftl_nv_cache *nv_cache,
		struct ftl_nv_cache_chunk_md *chunk_md)
{
	struct ftl_md *md = nv_cache->md;
	void *buffer = ftl_md_get_buffer(md);
	uint64_t size = ftl_md_get_buffer_size(md);
	void *ptr = chunk_md;

	if (ptr < buffer) {
		ftl_abort();
	}

	ptr += sizeof(*chunk_md);
	if (ptr > buffer + size) {
		ftl_abort();
	}
}

static inline uint64_t
nvc_data_offset(struct ftl_nv_cache *nv_cache)
{
	return 0;
}

static inline uint64_t
nvc_data_blocks(struct ftl_nv_cache *nv_cache)
{
	return nv_cache->chunk_blocks * nv_cache->chunk_count;
}

size_t
ftl_nv_cache_chunk_tail_md_num_blocks(const struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev =  SPDK_CONTAINEROF(nv_cache,
				    struct spdk_ftl_dev, nv_cache);
	return spdk_divide_round_up(dev->layout.nvc.chunk_data_blocks * dev->layout.l2p.addr_size,
				    FTL_BLOCK_SIZE);
}

static size_t
nv_cache_p2l_map_pool_elem_size(const struct ftl_nv_cache *nv_cache)
{
	/* Map pool element holds the whole tail md */
	return nv_cache->tail_md_chunk_blocks * FTL_BLOCK_SIZE;
}

static uint64_t
get_chunk_idx(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache_chunk *first_chunk = chunk->nv_cache->chunks;

	return (chunk->offset - first_chunk->offset) / chunk->nv_cache->chunk_blocks;
}

static void
ftl_nv_cache_init_update_limits(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nvc = &dev->nv_cache;
	uint64_t usable_chunks = nvc->chunk_count - nvc->chunk_inactive_count;

	/* Start compaction when full chunks exceed given % of entire active chunks */
	nvc->chunk_compaction_threshold = usable_chunks *
					  dev->conf.nv_cache.chunk_compaction_threshold /
					  100;

	nvc->throttle.interval_tsc = FTL_NV_CACHE_THROTTLE_INTERVAL_MS *
				     (spdk_get_ticks_hz() / 1000);

	nvc->chunk_free_target = spdk_divide_round_up(usable_chunks *
				 dev->conf.nv_cache.chunk_free_target,
				 100);
}

struct nvc_scrub_ctx {
	uint64_t chunk_no;
	nvc_scrub_cb cb;
	void *cb_ctx;

	struct ftl_layout_region reg_chunk;
	struct ftl_md *md_chunk;
};

static int
nvc_scrub_find_next_chunk(struct spdk_ftl_dev *dev, struct nvc_scrub_ctx *scrub_ctx)
{
	while (scrub_ctx->chunk_no < dev->layout.nvc.chunk_count) {
		if (dev->nv_cache.nvc_type->ops.is_chunk_active(dev, scrub_ctx->reg_chunk.current.offset)) {
			return 0;
		}

		/* Move the dummy region along with the active chunk */
		scrub_ctx->reg_chunk.current.offset += dev->layout.nvc.chunk_data_blocks;
		scrub_ctx->chunk_no++;
	}
	return -ENOENT;
}

static void
nvc_scrub_clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct nvc_scrub_ctx *scrub_ctx = md->owner.cb_ctx;
	union ftl_md_vss vss;

	/* Move to the next chunk */
	scrub_ctx->chunk_no++;
	scrub_ctx->reg_chunk.current.offset += dev->layout.nvc.chunk_data_blocks;

	FTL_DEBUGLOG(dev, "Scrub progress: %"PRIu64"/%"PRIu64" chunks\n",
		     scrub_ctx->chunk_no, dev->layout.nvc.chunk_count);

	if (status || nvc_scrub_find_next_chunk(dev, scrub_ctx)) {
		/* IO error or no more active chunks found. Scrubbing finished. */
		scrub_ctx->cb(dev, scrub_ctx->cb_ctx, status);
		ftl_md_destroy(scrub_ctx->md_chunk, 0);
		free(scrub_ctx);
		return;
	}

	/* Scrub the next chunk */
	vss.version.md_version = 0;
	vss.nv_cache.lba = FTL_ADDR_INVALID;

	scrub_ctx->md_chunk->cb = nvc_scrub_clear_cb;
	scrub_ctx->md_chunk->owner.cb_ctx = scrub_ctx;

	ftl_md_clear(scrub_ctx->md_chunk, 0, &vss);
}

void
ftl_nv_cache_scrub(struct spdk_ftl_dev *dev, nvc_scrub_cb cb, void *cb_ctx)
{
	struct nvc_scrub_ctx *scrub_ctx = calloc(1, sizeof(*scrub_ctx));
	union ftl_md_vss vss;

	if (!scrub_ctx) {
		cb(dev, cb_ctx, -ENOMEM);
		return;
	}

	scrub_ctx->cb = cb;
	scrub_ctx->cb_ctx = cb_ctx;

	/* Setup a dummy region for the first chunk */
	scrub_ctx->reg_chunk.name = ftl_md_region_name(FTL_LAYOUT_REGION_TYPE_DATA_NVC);
	scrub_ctx->reg_chunk.type = FTL_LAYOUT_REGION_TYPE_DATA_NVC;
	scrub_ctx->reg_chunk.mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	scrub_ctx->reg_chunk.current.version = 0;
	scrub_ctx->reg_chunk.current.offset = 0;
	scrub_ctx->reg_chunk.current.blocks = dev->layout.nvc.chunk_data_blocks;
	scrub_ctx->reg_chunk.entry_size = FTL_BLOCK_SIZE;
	scrub_ctx->reg_chunk.num_entries = dev->layout.nvc.chunk_data_blocks;
	scrub_ctx->reg_chunk.vss_blksz = dev->nv_cache.md_size;
	scrub_ctx->reg_chunk.bdev_desc = dev->nv_cache.bdev_desc;
	scrub_ctx->reg_chunk.ioch = dev->nv_cache.cache_ioch;

	/* Setup an MD object for the region */
	scrub_ctx->md_chunk = ftl_md_create(dev, scrub_ctx->reg_chunk.current.blocks,
					    scrub_ctx->reg_chunk.vss_blksz, scrub_ctx->reg_chunk.name, FTL_MD_CREATE_NO_MEM,
					    &scrub_ctx->reg_chunk);

	if (!scrub_ctx->md_chunk) {
		free(scrub_ctx);
		cb(dev, cb_ctx, -ENOMEM);
		return;
	}

	if (nvc_scrub_find_next_chunk(dev, scrub_ctx)) {
		/* No active chunks found */
		ftl_md_destroy(scrub_ctx->md_chunk, 0);
		free(scrub_ctx);
		cb(dev, cb_ctx, -ENOENT);
		return;
	}

	/* Scrub the first chunk */
	vss.version.md_version = 0;
	vss.nv_cache.lba = FTL_ADDR_INVALID;

	scrub_ctx->md_chunk->cb = nvc_scrub_clear_cb;
	scrub_ctx->md_chunk->owner.cb_ctx = scrub_ctx;

	ftl_md_clear(scrub_ctx->md_chunk, 0, &vss);
	return;
}

int
ftl_nv_cache_init(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_nv_cache_chunk *chunk;
	struct ftl_nv_cache_chunk_md *md;
	struct ftl_nv_cache_compactor *compactor;
	uint64_t i, offset;

	nv_cache->halt = true;

	nv_cache->md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];
	if (!nv_cache->md) {
		FTL_ERRLOG(dev, "No NV cache metadata object\n");
		return -1;
	}

	nv_cache->md_pool = ftl_mempool_create(dev->conf.user_io_pool_size,
					       nv_cache->md_size * dev->xfer_size,
					       FTL_BLOCK_SIZE, SPDK_ENV_NUMA_ID_ANY);
	if (!nv_cache->md_pool) {
		FTL_ERRLOG(dev, "Failed to initialize NV cache metadata pool\n");
		return -1;
	}

	/*
	 * Initialize chunk info
	 */
	nv_cache->chunk_blocks = dev->layout.nvc.chunk_data_blocks;
	nv_cache->chunk_count = dev->layout.nvc.chunk_count;
	nv_cache->tail_md_chunk_blocks = ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache);

	/* Allocate chunks */
	nv_cache->chunks = calloc(nv_cache->chunk_count,
				  sizeof(nv_cache->chunks[0]));
	if (!nv_cache->chunks) {
		FTL_ERRLOG(dev, "Failed to initialize NV cache chunks\n");
		return -1;
	}

	TAILQ_INIT(&nv_cache->chunk_free_list);
	TAILQ_INIT(&nv_cache->chunk_open_list);
	TAILQ_INIT(&nv_cache->chunk_full_list);
	TAILQ_INIT(&nv_cache->chunk_comp_list);
	TAILQ_INIT(&nv_cache->chunk_inactive_list);
	TAILQ_INIT(&nv_cache->needs_free_persist_list);

	/* First chunk metadata */
	md = ftl_md_get_buffer(nv_cache->md);
	if (!md) {
		FTL_ERRLOG(dev, "No NV cache metadata\n");
		return -1;
	}

	chunk = nv_cache->chunks;
	offset = nvc_data_offset(nv_cache);
	for (i = 0; i < nv_cache->chunk_count; i++, chunk++, md++) {
		chunk->nv_cache = nv_cache;
		chunk->md = md;
		chunk->md->version = FTL_NVC_VERSION_CURRENT;
		nvc_validate_md(nv_cache, md);
		chunk->offset = offset;
		offset += nv_cache->chunk_blocks;

		if (nv_cache->nvc_type->ops.is_chunk_active(dev, chunk->offset)) {
			nv_cache->chunk_free_count++;
			TAILQ_INSERT_TAIL(&nv_cache->chunk_free_list, chunk, entry);
		} else {
			chunk->md->state = FTL_CHUNK_STATE_INACTIVE;
			nv_cache->chunk_inactive_count++;
			TAILQ_INSERT_TAIL(&nv_cache->chunk_inactive_list, chunk, entry);
		}
	}
	assert(nv_cache->chunk_free_count + nv_cache->chunk_inactive_count == nv_cache->chunk_count);
	assert(offset <= nvc_data_offset(nv_cache) + nvc_data_blocks(nv_cache));

	TAILQ_INIT(&nv_cache->compactor_list);
	for (i = 0; i < FTL_NV_CACHE_NUM_COMPACTORS; i++) {
		compactor = compactor_alloc(dev);

		if (!compactor) {
			FTL_ERRLOG(dev, "Cannot allocate compaction process\n");
			return -1;
		}

		TAILQ_INSERT_TAIL(&nv_cache->compactor_list, compactor, entry);
	}

#define FTL_MAX_OPEN_CHUNKS 2
#define FTL_MAX_COMPACTED_CHUNKS 2
	nv_cache->p2l_pool = ftl_mempool_create(FTL_MAX_OPEN_CHUNKS + FTL_MAX_COMPACTED_CHUNKS,
						nv_cache_p2l_map_pool_elem_size(nv_cache),
						FTL_BLOCK_SIZE,
						SPDK_ENV_NUMA_ID_ANY);
	if (!nv_cache->p2l_pool) {
		return -ENOMEM;
	}

	/* One entry per open chunk */
	nv_cache->chunk_md_pool = ftl_mempool_create(FTL_MAX_OPEN_CHUNKS + FTL_MAX_COMPACTED_CHUNKS,
				  sizeof(struct ftl_nv_cache_chunk_md),
				  FTL_BLOCK_SIZE,
				  SPDK_ENV_NUMA_ID_ANY);
	if (!nv_cache->chunk_md_pool) {
		return -ENOMEM;
	}

	/* Each compactor can be reading a different chunk which it needs to switch state to free to at the end,
	 * plus one backup each for high invalidity chunks processing (if there's a backlog of chunks with extremely
	 * small, even 0, validity then they can be processed by the compactors quickly and trigger a lot of updates
	 * to free state at once) */
	nv_cache->free_chunk_md_pool = ftl_mempool_create(2 * FTL_NV_CACHE_NUM_COMPACTORS,
				       sizeof(struct ftl_nv_cache_chunk_md),
				       FTL_BLOCK_SIZE,
				       SPDK_ENV_NUMA_ID_ANY);
	if (!nv_cache->free_chunk_md_pool) {
		return -ENOMEM;
	}

	ftl_nv_cache_init_update_limits(dev);
	ftl_property_register(dev, "cache_device", NULL, 0, NULL, NULL, ftl_property_dump_cache_dev, NULL,
			      NULL, true);

	nv_cache->throttle.interval_tsc = FTL_NV_CACHE_THROTTLE_INTERVAL_MS *
					  (spdk_get_ticks_hz() / 1000);
	nv_cache->chunk_free_target = spdk_divide_round_up(nv_cache->chunk_count *
				      dev->conf.nv_cache.chunk_free_target,
				      100);

	if (nv_cache->nvc_type->ops.init) {
		return nv_cache->nvc_type->ops.init(dev);
	} else {
		return 0;
	}
}

void
ftl_nv_cache_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_nv_cache_compactor *compactor;

	if (nv_cache->nvc_type->ops.deinit) {
		nv_cache->nvc_type->ops.deinit(dev);
	}

	while (!TAILQ_EMPTY(&nv_cache->compactor_list)) {
		compactor = TAILQ_FIRST(&nv_cache->compactor_list);
		TAILQ_REMOVE(&nv_cache->compactor_list, compactor, entry);

		compactor_free(dev, compactor);
	}

	ftl_mempool_destroy(nv_cache->md_pool);
	ftl_mempool_destroy(nv_cache->p2l_pool);
	ftl_mempool_destroy(nv_cache->chunk_md_pool);
	ftl_mempool_destroy(nv_cache->free_chunk_md_pool);
	nv_cache->md_pool = NULL;
	nv_cache->p2l_pool = NULL;
	nv_cache->chunk_md_pool = NULL;
	nv_cache->free_chunk_md_pool = NULL;

	free(nv_cache->chunks);
	nv_cache->chunks = NULL;
}

static uint64_t
chunk_get_free_space(struct ftl_nv_cache *nv_cache,
		     struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->md->write_pointer + nv_cache->tail_md_chunk_blocks <=
	       nv_cache->chunk_blocks);
	return nv_cache->chunk_blocks - chunk->md->write_pointer -
	       nv_cache->tail_md_chunk_blocks;
}

static bool
chunk_is_closed(struct ftl_nv_cache_chunk *chunk)
{
	return chunk->md->write_pointer == chunk->nv_cache->chunk_blocks;
}

static void ftl_chunk_close(struct ftl_nv_cache_chunk *chunk);

static uint64_t
ftl_nv_cache_get_wr_buffer(struct ftl_nv_cache *nv_cache, struct ftl_io *io)
{
	uint64_t address = FTL_LBA_INVALID;
	uint64_t num_blocks = io->num_blocks;
	uint64_t free_space;
	struct ftl_nv_cache_chunk *chunk;

	do {
		chunk = nv_cache->chunk_current;
		/* Chunk has been closed so pick new one */
		if (chunk && chunk_is_closed(chunk))  {
			chunk = NULL;
		}

		if (!chunk) {
			chunk = TAILQ_FIRST(&nv_cache->chunk_open_list);
			if (chunk && chunk->md->state == FTL_CHUNK_STATE_OPEN) {
				TAILQ_REMOVE(&nv_cache->chunk_open_list, chunk, entry);
				nv_cache->chunk_current = chunk;
			} else {
				break;
			}
		}

		free_space = chunk_get_free_space(nv_cache, chunk);

		if (free_space >= num_blocks) {
			/* Enough space in chunk */

			/* Calculate address in NV cache */
			address = chunk->offset + chunk->md->write_pointer;

			/* Set chunk in IO */
			io->nv_cache_chunk = chunk;

			/* Move write pointer */
			chunk->md->write_pointer += num_blocks;

			if (free_space == num_blocks) {
				nv_cache->chunk_current = NULL;
			}
			break;
		}

		/* Not enough space in nv_cache_chunk */
		nv_cache->chunk_current = NULL;

		if (0 == free_space) {
			continue;
		}

		chunk->md->blocks_skipped = free_space;
		chunk->md->blocks_written += free_space;
		chunk->md->write_pointer += free_space;

		if (chunk->md->blocks_written == chunk_tail_md_offset(nv_cache)) {
			ftl_chunk_close(chunk);
		}
	} while (1);

	return address;
}

void
ftl_nv_cache_fill_md(struct ftl_io *io)
{
	struct ftl_nv_cache_chunk *chunk = io->nv_cache_chunk;
	uint64_t i;
	union ftl_md_vss *metadata = io->md;
	uint64_t lba = ftl_io_get_lba(io, 0);

	for (i = 0; i < io->num_blocks; ++i, lba++, metadata++) {
		metadata->nv_cache.lba = lba;
		metadata->nv_cache.seq_id = chunk->md->seq_id;
	}
}

uint64_t
chunk_tail_md_offset(struct ftl_nv_cache *nv_cache)
{
	return nv_cache->chunk_blocks - nv_cache->tail_md_chunk_blocks;
}

static void
chunk_advance_blocks(struct ftl_nv_cache *nv_cache, struct ftl_nv_cache_chunk *chunk,
		     uint64_t advanced_blocks)
{
	chunk->md->blocks_written += advanced_blocks;

	assert(chunk->md->blocks_written <= nv_cache->chunk_blocks);

	if (chunk->md->blocks_written == chunk_tail_md_offset(nv_cache)) {
		ftl_chunk_close(chunk);
	}
}

static uint64_t
chunk_user_blocks_written(struct ftl_nv_cache_chunk *chunk)
{
	return chunk->md->blocks_written - chunk->md->blocks_skipped -
	       chunk->nv_cache->tail_md_chunk_blocks;
}

static bool
is_chunk_compacted(struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->md->blocks_written != 0);

	if (chunk_user_blocks_written(chunk) == chunk->md->blocks_compacted) {
		return true;
	}

	return false;
}

static int
ftl_chunk_alloc_md_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	p2l_map->chunk_dma_md = ftl_mempool_get(nv_cache->chunk_md_pool);

	if (!p2l_map->chunk_dma_md) {
		return -ENOMEM;
	}

	ftl_nv_cache_chunk_md_initialize(p2l_map->chunk_dma_md);
	return 0;
}

static void
ftl_chunk_free_md_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	ftl_mempool_put(chunk->nv_cache->chunk_md_pool, p2l_map->chunk_dma_md);
	p2l_map->chunk_dma_md = NULL;
}

static void chunk_free_p2l_map(struct ftl_nv_cache_chunk *chunk);

static void
ftl_chunk_free(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;

	/* Reset chunk */
	ftl_nv_cache_chunk_md_initialize(chunk->md);

	TAILQ_INSERT_TAIL(&nv_cache->needs_free_persist_list, chunk, entry);
	nv_cache->chunk_free_persist_count++;
}

static int
ftl_chunk_alloc_chunk_free_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	p2l_map->chunk_dma_md = ftl_mempool_get(nv_cache->free_chunk_md_pool);
	if (!p2l_map->chunk_dma_md) {
		return -ENOMEM;
	}

	ftl_nv_cache_chunk_md_initialize(p2l_map->chunk_dma_md);
	return 0;
}

static void
ftl_chunk_free_chunk_free_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	ftl_mempool_put(chunk->nv_cache->free_chunk_md_pool, p2l_map->chunk_dma_md);
	p2l_map->chunk_dma_md = NULL;
}

static void
chunk_free_cb(int status, void *ctx)
{
	struct ftl_nv_cache_chunk *chunk = (struct ftl_nv_cache_chunk *)ctx;

	if (spdk_likely(!status)) {
		struct ftl_nv_cache *nv_cache = chunk->nv_cache;

		nv_cache->chunk_free_persist_count--;
		TAILQ_INSERT_TAIL(&nv_cache->chunk_free_list, chunk, entry);
		nv_cache->chunk_free_count++;
		nv_cache->chunk_full_count--;
		chunk->md->state = FTL_CHUNK_STATE_FREE;
		chunk->md->close_seq_id = 0;
		ftl_chunk_free_chunk_free_entry(chunk);
	} else {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		ftl_md_persist_entry_retry(&chunk->md_persist_entry_ctx);
#else
		ftl_abort();
#endif
	}
}

static void
ftl_chunk_persist_free_state(struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_p2l_map *p2l_map;
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];
	struct ftl_layout_region *region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD);
	struct ftl_nv_cache_chunk *tchunk, *chunk = NULL;
	int rc;

	TAILQ_FOREACH_SAFE(chunk, &nv_cache->needs_free_persist_list, entry, tchunk) {
		p2l_map = &chunk->p2l_map;
		rc = ftl_chunk_alloc_chunk_free_entry(chunk);
		if (rc) {
			break;
		}

		TAILQ_REMOVE(&nv_cache->needs_free_persist_list, chunk, entry);

		memcpy(p2l_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
		p2l_map->chunk_dma_md->state = FTL_CHUNK_STATE_FREE;
		p2l_map->chunk_dma_md->close_seq_id = 0;
		p2l_map->chunk_dma_md->p2l_map_checksum = 0;

		ftl_md_persist_entries(md, get_chunk_idx(chunk), 1, p2l_map->chunk_dma_md, NULL,
				       chunk_free_cb, chunk, &chunk->md_persist_entry_ctx);
	}
}

static void
compaction_stats_update(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct compaction_bw_stats *compaction_bw = &nv_cache->compaction_recent_bw;
	double *ptr;

	if (spdk_unlikely(chunk->compaction_length_tsc == 0)) {
		return;
	}

	if (spdk_likely(compaction_bw->count == FTL_NV_CACHE_COMPACTION_SMA_N)) {
		ptr = compaction_bw->buf + compaction_bw->first;
		compaction_bw->first++;
		if (compaction_bw->first == FTL_NV_CACHE_COMPACTION_SMA_N) {
			compaction_bw->first = 0;
		}
		compaction_bw->sum -= *ptr;
	} else {
		ptr = compaction_bw->buf + compaction_bw->count;
		compaction_bw->count++;
	}

	*ptr = (double)chunk->md->blocks_compacted * FTL_BLOCK_SIZE / chunk->compaction_length_tsc;
	chunk->compaction_length_tsc = 0;

	compaction_bw->sum += *ptr;
	nv_cache->compaction_sma = compaction_bw->sum / compaction_bw->count;
}

static void
chunk_compaction_advance(struct ftl_nv_cache_chunk *chunk, uint64_t num_blocks)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	uint64_t tsc = spdk_thread_get_last_tsc(spdk_get_thread());

	chunk->compaction_length_tsc += tsc - chunk->compaction_start_tsc;
	chunk->compaction_start_tsc = tsc;

	chunk->md->blocks_compacted += num_blocks;
	assert(chunk->md->blocks_compacted <= chunk_user_blocks_written(chunk));
	if (!is_chunk_compacted(chunk)) {
		return;
	}

	/* Remove chunk from compacted list */
	TAILQ_REMOVE(&nv_cache->chunk_comp_list, chunk, entry);
	nv_cache->chunk_comp_count--;

	compaction_stats_update(chunk);

	chunk_free_p2l_map(chunk);

	ftl_chunk_free(chunk);
}

static bool
is_compaction_required_for_upgrade(struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);

	if (dev->conf.prep_upgrade_on_shutdown) {
		if (nv_cache->chunk_full_count || nv_cache->chunk_open_count) {
			return true;
		}
	}

	return false;
}

static bool
is_compaction_required(struct ftl_nv_cache *nv_cache)
{
	if (spdk_unlikely(nv_cache->halt)) {
		return is_compaction_required_for_upgrade(nv_cache);
	}

	if (nv_cache->chunk_full_count >= nv_cache->chunk_compaction_threshold) {
		return true;
	}

	return false;
}

static void compaction_process_finish_read(struct ftl_nv_cache_compactor *compactor);
static void compaction_process_pin_lba(struct ftl_nv_cache_compactor *comp);

static void
_compaction_process_pin_lba(void *_comp)
{
	struct ftl_nv_cache_compactor *comp = _comp;

	compaction_process_pin_lba(comp);
}

static void
compaction_process_pin_lba_cb(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_nv_cache_compactor *comp = pin_ctx->cb_ctx;
	struct ftl_rq *rq = comp->rq;

	if (status) {
		rq->iter.status = status;
		pin_ctx->lba = FTL_LBA_INVALID;
	}

	if (--rq->iter.remaining == 0) {
		if (rq->iter.status) {
			/* unpin and try again */
			ftl_rq_unpin(rq);
			spdk_thread_send_msg(spdk_get_thread(), _compaction_process_pin_lba, comp);
			return;
		}

		compaction_process_finish_read(comp);
	}
}

static void
compaction_process_pin_lba(struct ftl_nv_cache_compactor *comp)
{
	struct ftl_rq *rq = comp->rq;
	struct spdk_ftl_dev *dev = rq->dev;
	struct ftl_rq_entry *entry;

	assert(rq->iter.count);
	rq->iter.remaining = rq->iter.count;
	rq->iter.status = 0;

	FTL_RQ_ENTRY_LOOP(rq, entry, rq->iter.count) {
		struct ftl_l2p_pin_ctx *pin_ctx = &entry->l2p_pin_ctx;

		if (entry->lba == FTL_LBA_INVALID) {
			ftl_l2p_pin_skip(dev, compaction_process_pin_lba_cb, comp, pin_ctx);
		} else {
			ftl_l2p_pin(dev, entry->lba, 1, compaction_process_pin_lba_cb, comp, pin_ctx);
		}
	}
}

static void
compaction_process_read_entry_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_rq_entry *entry = arg;
	struct ftl_rq *rq = ftl_rq_from_entry(entry);
	struct spdk_ftl_dev *dev = rq->dev;
	struct ftl_nv_cache_compactor *compactor = rq->owner.priv;

	ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_CMP, bdev_io);

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		/* retry */
		spdk_thread_send_msg(spdk_get_thread(), compaction_process_read_entry, entry);
		return;
	}

	assert(rq->iter.remaining >= entry->bdev_io.num_blocks);
	rq->iter.remaining -= entry->bdev_io.num_blocks;
	if (0 == rq->iter.remaining) {
		/* All IOs processed, go to next phase - pining */
		compaction_process_pin_lba(compactor);
	}
}

static void
compaction_process_read_entry(void *arg)
{
	struct ftl_rq_entry *entry = arg;
	struct ftl_rq *rq = ftl_rq_from_entry(entry);
	struct spdk_ftl_dev *dev = rq->dev;
	int rc;

	rc = ftl_nv_cache_bdev_read_blocks_with_md(dev->nv_cache.bdev_desc, dev->nv_cache.cache_ioch,
			entry->io_payload, NULL, entry->bdev_io.offset_blocks, entry->bdev_io.num_blocks,
			compaction_process_read_entry_cb, entry);

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
			entry->bdev_io.wait_entry.bdev = bdev;
			entry->bdev_io.wait_entry.cb_fn = compaction_process_read_entry;
			entry->bdev_io.wait_entry.cb_arg = entry;
			spdk_bdev_queue_io_wait(bdev, dev->nv_cache.cache_ioch, &entry->bdev_io.wait_entry);
		} else {
			ftl_abort();
		}
	}

	dev->stats.io_activity_total += entry->bdev_io.num_blocks;
}

static bool
is_chunk_to_read(struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->md->blocks_written != 0);

	if (chunk_user_blocks_written(chunk) == chunk->md->read_pointer) {
		return false;
	}

	return true;
}

static void
read_chunk_p2l_map_cb(struct ftl_basic_rq *brq)
{
	struct ftl_nv_cache_chunk *chunk = brq->io.chunk;
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;

	if (!brq->success) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		chunk_free_p2l_map(chunk);
		read_chunk_p2l_map(chunk);
		return;
#else
		ftl_abort();
#endif
	}

	TAILQ_INSERT_TAIL(&nv_cache->chunk_comp_list, chunk, entry);
}

static int chunk_alloc_p2l_map(struct ftl_nv_cache_chunk *chunk);
static int ftl_chunk_read_tail_md(struct ftl_nv_cache_chunk *chunk, struct ftl_basic_rq *brq,
				  void (*cb)(struct ftl_basic_rq *brq), void *cb_ctx);

static void
read_chunk_p2l_map(void *arg)
{
	struct ftl_nv_cache_chunk *chunk = arg;
	int rc;

	if (chunk_alloc_p2l_map(chunk)) {
		ftl_abort();
	}

	rc = ftl_chunk_read_tail_md(chunk, &chunk->metadata_rq, read_chunk_p2l_map_cb, NULL);
	if (rc) {
		if (rc == -ENOMEM) {
			struct ftl_nv_cache *nv_cache = chunk->nv_cache;
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			struct spdk_bdev_io_wait_entry *wait_entry = &chunk->metadata_rq.io.bdev_io_wait;

			chunk_free_p2l_map(chunk);
			wait_entry->bdev = bdev;
			wait_entry->cb_fn = read_chunk_p2l_map;
			wait_entry->cb_arg = chunk;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, wait_entry);
		} else {
			ftl_abort();
		}
	}
}

static void
prepare_chunk_for_compaction(struct ftl_nv_cache *nv_cache)
{
	struct ftl_nv_cache_chunk *chunk = NULL;

	if (TAILQ_EMPTY(&nv_cache->chunk_full_list)) {
		return;
	}

	chunk = TAILQ_FIRST(&nv_cache->chunk_full_list);
	TAILQ_REMOVE(&nv_cache->chunk_full_list, chunk, entry);
	assert(chunk->md->write_pointer);

	nv_cache->chunk_comp_count++;
	read_chunk_p2l_map(chunk);
}


static struct ftl_nv_cache_chunk *
get_chunk_for_compaction(struct ftl_nv_cache *nv_cache)
{
	struct ftl_nv_cache_chunk *chunk = NULL;

	if (TAILQ_EMPTY(&nv_cache->chunk_comp_list)) {
		return NULL;
	}

	chunk = TAILQ_FIRST(&nv_cache->chunk_comp_list);
	if (!is_chunk_to_read(chunk)) {
		return NULL;
	}

	return chunk;
}

static uint64_t
chunk_blocks_to_read(struct ftl_nv_cache_chunk *chunk)
{
	uint64_t blocks_written;
	uint64_t blocks_to_read;

	assert(chunk->md->blocks_written >= chunk->md->blocks_skipped);
	blocks_written = chunk_user_blocks_written(chunk);

	assert(blocks_written >= chunk->md->read_pointer);
	blocks_to_read = blocks_written - chunk->md->read_pointer;

	return blocks_to_read;
}

static void
compactor_deactivate(struct ftl_nv_cache_compactor *compactor)
{
	struct ftl_nv_cache *nv_cache = compactor->nv_cache;

	compactor->rq->iter.count = 0;
	assert(nv_cache->compaction_active_count);
	nv_cache->compaction_active_count--;
	TAILQ_INSERT_TAIL(&nv_cache->compactor_list, compactor, entry);
}

static void
compaction_process_invalidate_entry(struct ftl_rq_entry *entry)
{
	entry->addr = FTL_ADDR_INVALID;
	entry->lba = FTL_LBA_INVALID;
	entry->seq_id = 0;
	entry->owner.priv = NULL;
}

static void
compaction_process_pad(struct ftl_nv_cache_compactor *compactor, uint64_t idx)
{
	struct ftl_rq *rq = compactor->rq;
	struct ftl_rq_entry *entry;

	assert(idx < rq->num_blocks);
	FTL_RQ_ENTRY_LOOP_FROM(rq, &rq->entries[idx], entry, rq->num_blocks) {
		compaction_process_invalidate_entry(entry);
	}
}

static void
compaction_process_read(struct ftl_nv_cache_compactor *compactor)
{
	struct ftl_rq *rq = compactor->rq;
	struct ftl_nv_cache *nv_cache = compactor->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_rq_entry *entry, *io;

	assert(rq->iter.count);
	rq->iter.remaining = rq->iter.count;

	io = rq->entries;
	io->bdev_io.num_blocks = 1;
	io->bdev_io.offset_blocks = ftl_addr_to_nvc_offset(dev, io->addr);
	FTL_RQ_ENTRY_LOOP_FROM(rq, &rq->entries[1], entry,  rq->iter.count) {
		if (entry->addr == io->addr + io->bdev_io.num_blocks) {
			io->bdev_io.num_blocks++;
		} else {
			compaction_process_read_entry(io);
			io = entry;
			io->bdev_io.num_blocks = 1;
			io->bdev_io.offset_blocks = ftl_addr_to_nvc_offset(dev, io->addr);
		}
	}
	compaction_process_read_entry(io);
}

static ftl_addr
compaction_chunk_read_pos(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk)
{
	ftl_addr start, pos;
	uint64_t skip, to_read = chunk_blocks_to_read(chunk);

	if (0 == to_read) {
		return FTL_ADDR_INVALID;
	}

	start = ftl_addr_from_nvc_offset(dev, chunk->offset + chunk->md->read_pointer);
	pos = ftl_bitmap_find_first_set(dev->valid_map, start, start + to_read - 1);

	if (pos == UINT64_MAX) {
		chunk->md->read_pointer += to_read;
		chunk_compaction_advance(chunk, to_read);
		return FTL_ADDR_INVALID;
	}

	assert(pos >= start);
	skip = pos - start;
	if (skip) {
		chunk->md->read_pointer += skip;
		chunk_compaction_advance(chunk, skip);
	}

	return pos;
}

static bool
compaction_entry_read_pos(struct ftl_nv_cache *nv_cache, struct ftl_rq_entry *entry)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_nv_cache_chunk *chunk = NULL;
	ftl_addr addr = FTL_ADDR_INVALID;

	while (!chunk) {
		/* Get currently handled chunk */
		chunk = get_chunk_for_compaction(nv_cache);
		if (!chunk) {
			return false;
		}
		chunk->compaction_start_tsc = spdk_thread_get_last_tsc(spdk_get_thread());

		/* Get next read position in chunk */
		addr = compaction_chunk_read_pos(dev, chunk);
		if (FTL_ADDR_INVALID == addr) {
			chunk = NULL;
		}
	}

	assert(FTL_ADDR_INVALID != addr);

	/* Set entry address info and chunk */
	entry->addr = addr;
	entry->owner.priv = chunk;
	entry->lba = ftl_chunk_map_get_lba(chunk, chunk->md->read_pointer);

	/* Move read pointer in the chunk */
	chunk->md->read_pointer++;

	return true;
}

static void
compaction_process_start(struct ftl_nv_cache_compactor *compactor)
{
	struct ftl_rq *rq = compactor->rq;
	struct ftl_nv_cache *nv_cache = compactor->nv_cache;
	struct ftl_rq_entry *entry;

	assert(0 == compactor->rq->iter.count);
	FTL_RQ_ENTRY_LOOP(rq, entry, rq->num_blocks) {
		if (!compaction_entry_read_pos(nv_cache, entry)) {
			compaction_process_pad(compactor, entry->index);
			break;
		}
		rq->iter.count++;
	}

	if (rq->iter.count) {
		/* Schedule Read IOs */
		compaction_process_read(compactor);
	} else {
		compactor_deactivate(compactor);
	}
}

static void
compaction_process(struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_nv_cache_compactor *compactor;

	if (!is_compaction_required(nv_cache)) {
		return;
	}

	if (nv_cache->chunk_comp_count < FTL_MAX_COMPACTED_CHUNKS) {
		prepare_chunk_for_compaction(nv_cache);
	}

	if (TAILQ_EMPTY(&nv_cache->chunk_comp_list)) {
		return;
	}

	compactor = TAILQ_FIRST(&nv_cache->compactor_list);
	if (!compactor) {
		return;
	}

	TAILQ_REMOVE(&nv_cache->compactor_list, compactor, entry);
	compactor->nv_cache->compaction_active_count++;
	compaction_process_start(compactor);
	ftl_add_io_activity(dev);
}

static void
compaction_process_ftl_done(struct ftl_rq *rq)
{
	struct spdk_ftl_dev *dev = rq->dev;
	struct ftl_nv_cache_compactor *compactor = rq->owner.priv;
	struct ftl_band *band = rq->io.band;
	struct ftl_rq_entry *entry;
	ftl_addr addr;

	if (spdk_unlikely(false == rq->success)) {
		/* IO error retry writing */
#ifdef SPDK_FTL_RETRY_ON_ERROR
		ftl_writer_queue_rq(&dev->writer_user, rq);
		return;
#else
		ftl_abort();
#endif
	}

	assert(rq->iter.count);

	/* Update L2P table */
	addr = rq->io.addr;
	FTL_RQ_ENTRY_LOOP(rq, entry, rq->iter.count) {
		struct ftl_nv_cache_chunk *chunk = entry->owner.priv;

		if (entry->lba != FTL_LBA_INVALID) {
			ftl_l2p_update_base(dev, entry->lba, addr, entry->addr);
			ftl_l2p_unpin(dev, entry->lba, 1);
			chunk_compaction_advance(chunk, 1);
		} else {
			assert(entry->addr == FTL_ADDR_INVALID);
		}

		addr = ftl_band_next_addr(band, addr, 1);
		compaction_process_invalidate_entry(entry);
	}

	compactor_deactivate(compactor);
}

static void
compaction_process_finish_read(struct ftl_nv_cache_compactor *compactor)
{
	struct ftl_rq *rq = compactor->rq;
	struct spdk_ftl_dev *dev = rq->dev;
	struct ftl_rq_entry *entry;
	ftl_addr current_addr;
	uint64_t skip = 0;

	FTL_RQ_ENTRY_LOOP(rq, entry, rq->iter.count) {
		struct ftl_nv_cache_chunk *chunk = entry->owner.priv;
		uint64_t lba = entry->lba;

		if (lba == FTL_LBA_INVALID) {
			skip++;
			compaction_process_invalidate_entry(entry);
			chunk_compaction_advance(chunk, 1);
			continue;
		}

		current_addr = ftl_l2p_get(dev, lba);
		if (current_addr == entry->addr) {
			entry->seq_id = chunk->md->seq_id;
		} else {
			/* This address already invalidated, just omit this block */
			skip++;
			ftl_l2p_unpin(dev, lba, 1);
			compaction_process_invalidate_entry(entry);
			chunk_compaction_advance(chunk, 1);
		}
	}

	if (skip < rq->iter.count) {
		/*
		 * Request contains data to be placed on FTL, compact it
		 */
		ftl_writer_queue_rq(&dev->writer_user, rq);
	} else {
		compactor_deactivate(compactor);
	}
}

static void
compactor_free(struct spdk_ftl_dev *dev, struct ftl_nv_cache_compactor *compactor)
{
	if (!compactor) {
		return;
	}

	ftl_rq_del(compactor->rq);
	free(compactor);
}

static struct ftl_nv_cache_compactor *
compactor_alloc(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache_compactor *compactor;
	struct ftl_rq_entry *entry;

	compactor = calloc(1, sizeof(*compactor));
	if (!compactor) {
		goto error;
	}

	/* Allocate help request for reading */
	compactor->rq = ftl_rq_new(dev, dev->nv_cache.md_size);
	if (!compactor->rq) {
		goto error;
	}

	compactor->nv_cache = &dev->nv_cache;
	compactor->rq->owner.priv = compactor;
	compactor->rq->owner.cb = compaction_process_ftl_done;
	compactor->rq->owner.compaction = true;

	FTL_RQ_ENTRY_LOOP(compactor->rq, entry, compactor->rq->num_blocks) {
		compaction_process_invalidate_entry(entry);
	}

	return compactor;

error:
	compactor_free(dev, compactor);
	return NULL;
}

static void
ftl_nv_cache_submit_cb_done(struct ftl_io *io)
{
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;

	chunk_advance_blocks(nv_cache, io->nv_cache_chunk, io->num_blocks);
	io->nv_cache_chunk = NULL;

	ftl_io_complete(io);
}

static void
ftl_nv_cache_l2p_update(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	ftl_addr next_addr = io->addr;
	size_t i;

	for (i = 0; i < io->num_blocks; ++i, ++next_addr) {
		ftl_l2p_update_cache(dev, ftl_io_get_lba(io, i), next_addr, io->map[i]);
	}

	ftl_l2p_unpin(dev, io->lba, io->num_blocks);
	ftl_nv_cache_submit_cb_done(io);
}

static void
ftl_nv_cache_pin_cb(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_io *io = pin_ctx->cb_ctx;
	size_t i;

	if (spdk_unlikely(status != 0)) {
		/* Retry on the internal L2P fault */
		FTL_ERRLOG(dev, "Cannot PIN LBA for NV cache write failed at %"PRIx64"\n",
			   io->addr);
		io->status = -EAGAIN;
		ftl_nv_cache_submit_cb_done(io);
		return;
	}

	/* Remember previous l2p mapping to resolve conflicts in case of outstanding write-after-write */
	for (i = 0; i < io->num_blocks; ++i) {
		io->map[i] = ftl_l2p_get(dev, ftl_io_get_lba(io, i));
	}

	assert(io->iov_pos == 0);

	ftl_trace_submission(io->dev, io, io->addr, io->num_blocks);

	dev->nv_cache.nvc_type->ops.write(io);
}

void
ftl_nv_cache_write_complete(struct ftl_io *io, bool success)
{
	if (spdk_unlikely(!success)) {
		FTL_ERRLOG(io->dev, "Non-volatile cache write failed at %"PRIx64"\n",
			   io->addr);
		io->status = -EIO;
		ftl_l2p_unpin(io->dev, io->lba, io->num_blocks);
		ftl_nv_cache_submit_cb_done(io);
		return;
	}

	ftl_nv_cache_l2p_update(io);
}

bool
ftl_nv_cache_write(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	uint64_t cache_offset;

	/* Reserve area on the write buffer cache */
	cache_offset = ftl_nv_cache_get_wr_buffer(&dev->nv_cache, io);
	if (cache_offset == FTL_LBA_INVALID) {
		/* No free space in NV cache, resubmit request */
		return false;
	}
	io->addr = ftl_addr_from_nvc_offset(dev, cache_offset);

	ftl_l2p_pin(io->dev, io->lba, io->num_blocks,
		    ftl_nv_cache_pin_cb, io,
		    &io->l2p_pin_ctx);

	dev->nv_cache.throttle.blocks_submitted += io->num_blocks;

	return true;
}

int
ftl_nv_cache_read(struct ftl_io *io, ftl_addr addr, uint32_t num_blocks,
		  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	int rc;
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;

	assert(ftl_addr_in_nvc(io->dev, addr));

	rc = ftl_nv_cache_bdev_read_blocks_with_md(nv_cache->bdev_desc, nv_cache->cache_ioch,
			ftl_io_iovec_addr(io), NULL, ftl_addr_to_nvc_offset(io->dev, addr),
			num_blocks, cb, cb_arg);

	return rc;
}

bool
ftl_nv_cache_is_halted(struct ftl_nv_cache *nv_cache)
{
	if (nv_cache->compaction_active_count) {
		return false;
	}

	if (nv_cache->chunk_open_count > 0) {
		return false;
	}

	if (is_compaction_required_for_upgrade(nv_cache)) {
		return false;
	}

	return true;
}

void
ftl_chunk_map_set_lba(struct ftl_nv_cache_chunk *chunk,
		      uint64_t offset, uint64_t lba)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	ftl_lba_store(dev, p2l_map->chunk_map, offset, lba);
}

uint64_t
ftl_chunk_map_get_lba(struct ftl_nv_cache_chunk *chunk, uint64_t offset)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	return ftl_lba_load(dev, p2l_map->chunk_map, offset);
}

void
ftl_nv_cache_chunk_set_addr(struct ftl_nv_cache_chunk *chunk, uint64_t lba, ftl_addr addr)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	uint64_t cache_offset = ftl_addr_to_nvc_offset(dev, addr);
	uint64_t offset;

	offset = (cache_offset - chunk->offset) % chunk->nv_cache->chunk_blocks;
	ftl_chunk_map_set_lba(chunk, offset, lba);
}

struct ftl_nv_cache_chunk *
ftl_nv_cache_get_chunk_from_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	struct ftl_nv_cache_chunk *chunk = dev->nv_cache.chunks;
	uint64_t chunk_idx;
	uint64_t cache_offset = ftl_addr_to_nvc_offset(dev, addr);

	assert(chunk != NULL);
	chunk_idx = (cache_offset - chunk->offset) / chunk->nv_cache->chunk_blocks;
	chunk += chunk_idx;

	return chunk;
}

void
ftl_nv_cache_set_addr(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	struct ftl_nv_cache_chunk *chunk;

	chunk = ftl_nv_cache_get_chunk_from_addr(dev, addr);

	assert(lba != FTL_LBA_INVALID);

	ftl_nv_cache_chunk_set_addr(chunk, lba, addr);
	ftl_bitmap_set(dev->valid_map, addr);
}

static void
ftl_nv_cache_throttle_update(struct ftl_nv_cache *nv_cache)
{
	double err;
	double modifier;

	err = ((double)nv_cache->chunk_free_count - nv_cache->chunk_free_target) / nv_cache->chunk_count;
	modifier = FTL_NV_CACHE_THROTTLE_MODIFIER_KP * err;

	if (modifier < FTL_NV_CACHE_THROTTLE_MODIFIER_MIN) {
		modifier = FTL_NV_CACHE_THROTTLE_MODIFIER_MIN;
	} else if (modifier > FTL_NV_CACHE_THROTTLE_MODIFIER_MAX) {
		modifier = FTL_NV_CACHE_THROTTLE_MODIFIER_MAX;
	}

	if (spdk_unlikely(nv_cache->compaction_sma == 0 || nv_cache->compaction_active_count == 0)) {
		nv_cache->throttle.blocks_submitted_limit = UINT64_MAX;
	} else {
		double blocks_per_interval = nv_cache->compaction_sma * nv_cache->throttle.interval_tsc /
					     FTL_BLOCK_SIZE;
		nv_cache->throttle.blocks_submitted_limit = blocks_per_interval * (1.0 + modifier);
	}
}

static void
ftl_nv_cache_process_throttle(struct ftl_nv_cache *nv_cache)
{
	uint64_t tsc = spdk_thread_get_last_tsc(spdk_get_thread());

	if (spdk_unlikely(!nv_cache->throttle.start_tsc)) {
		nv_cache->throttle.start_tsc = tsc;
	} else if (tsc - nv_cache->throttle.start_tsc >= nv_cache->throttle.interval_tsc) {
		ftl_nv_cache_throttle_update(nv_cache);
		nv_cache->throttle.start_tsc = tsc;
		nv_cache->throttle.blocks_submitted = 0;
	}
}

static void ftl_chunk_open(struct ftl_nv_cache_chunk *chunk);

void
ftl_nv_cache_process(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;

	assert(dev->nv_cache.bdev_desc);

	if (nv_cache->chunk_open_count < FTL_MAX_OPEN_CHUNKS && spdk_likely(!nv_cache->halt) &&
	    !TAILQ_EMPTY(&nv_cache->chunk_free_list)) {
		struct ftl_nv_cache_chunk *chunk = TAILQ_FIRST(&nv_cache->chunk_free_list);
		TAILQ_REMOVE(&nv_cache->chunk_free_list, chunk, entry);
		TAILQ_INSERT_TAIL(&nv_cache->chunk_open_list, chunk, entry);
		nv_cache->chunk_free_count--;
		chunk->md->seq_id = ftl_get_next_seq_id(dev);
		ftl_chunk_open(chunk);
		ftl_add_io_activity(dev);
	}

	compaction_process(nv_cache);
	ftl_chunk_persist_free_state(nv_cache);
	ftl_nv_cache_process_throttle(nv_cache);

	if (nv_cache->nvc_type->ops.process) {
		nv_cache->nvc_type->ops.process(dev);
	}
}

static bool
ftl_nv_cache_full(struct ftl_nv_cache *nv_cache)
{
	if (0 == nv_cache->chunk_open_count && NULL == nv_cache->chunk_current) {
		return true;
	} else {
		return false;
	}
}

bool
ftl_nv_cache_throttle(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;

	if (dev->nv_cache.throttle.blocks_submitted >= nv_cache->throttle.blocks_submitted_limit ||
	    ftl_nv_cache_full(nv_cache)) {
		return true;
	}

	return false;
}

static void
chunk_free_p2l_map(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	ftl_mempool_put(nv_cache->p2l_pool, p2l_map->chunk_map);
	p2l_map->chunk_map = NULL;

	ftl_chunk_free_md_entry(chunk);
}

int
ftl_nv_cache_save_state(struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_nv_cache_chunk *chunk;
	int status = 0;
	uint64_t i;

	assert(nv_cache->chunk_open_count == 0);

	if (nv_cache->compaction_active_count) {
		FTL_ERRLOG(dev, "Cannot save NV cache state, compaction in progress\n");
		return -EINVAL;
	}

	chunk = nv_cache->chunks;
	if (!chunk) {
		FTL_ERRLOG(dev, "Cannot save NV cache state, no NV cache metadata\n");
		return -ENOMEM;
	}

	for (i = 0; i < nv_cache->chunk_count; i++, chunk++) {
		nvc_validate_md(nv_cache, chunk->md);

		if (chunk->md->read_pointer)  {
			/* Only full chunks can be compacted */
			if (chunk->md->blocks_written != nv_cache->chunk_blocks) {
				assert(0);
				status = -EINVAL;
				break;
			}

			/*
			 * Chunk in the middle of compaction, start over after
			 * load
			 */
			chunk->md->read_pointer = chunk->md->blocks_compacted = 0;
		} else if (chunk->md->blocks_written == nv_cache->chunk_blocks) {
			/* Full chunk */
		} else if (0 == chunk->md->blocks_written) {
			/* Empty chunk */
		} else {
			assert(0);
			status = -EINVAL;
			break;
		}
	}

	if (status) {
		FTL_ERRLOG(dev, "Cannot save NV cache state, inconsistent NV cache"
			   "metadata\n");
	}

	return status;
}

static int
sort_chunks_cmp(const void *a, const void *b)
{
	struct ftl_nv_cache_chunk *a_chunk = *(struct ftl_nv_cache_chunk **)a;
	struct ftl_nv_cache_chunk *b_chunk = *(struct ftl_nv_cache_chunk **)b;

	return a_chunk->md->seq_id - b_chunk->md->seq_id;
}

static int
sort_chunks(struct ftl_nv_cache *nv_cache)
{
	struct ftl_nv_cache_chunk **chunks_list;
	struct ftl_nv_cache_chunk *chunk;
	uint32_t i;

	if (TAILQ_EMPTY(&nv_cache->chunk_full_list)) {
		return 0;
	}

	chunks_list = calloc(nv_cache->chunk_full_count,
			     sizeof(chunks_list[0]));
	if (!chunks_list) {
		return -ENOMEM;
	}

	i = 0;
	TAILQ_FOREACH(chunk, &nv_cache->chunk_full_list, entry) {
		chunks_list[i] = chunk;
		i++;
	}
	assert(i == nv_cache->chunk_full_count);

	qsort(chunks_list, nv_cache->chunk_full_count, sizeof(chunks_list[0]),
	      sort_chunks_cmp);

	TAILQ_INIT(&nv_cache->chunk_full_list);
	for (i = 0; i < nv_cache->chunk_full_count; i++) {
		chunk = chunks_list[i];
		TAILQ_INSERT_TAIL(&nv_cache->chunk_full_list, chunk, entry);
	}

	free(chunks_list);
	return 0;
}

static int
chunk_alloc_p2l_map(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	assert(p2l_map->ref_cnt == 0);
	assert(p2l_map->chunk_map == NULL);

	p2l_map->chunk_map = ftl_mempool_get(nv_cache->p2l_pool);

	if (!p2l_map->chunk_map) {
		return -ENOMEM;
	}

	if (ftl_chunk_alloc_md_entry(chunk)) {
		ftl_mempool_put(nv_cache->p2l_pool, p2l_map->chunk_map);
		p2l_map->chunk_map = NULL;
		return -ENOMEM;
	}

	/* Set the P2L to FTL_LBA_INVALID */
	memset(p2l_map->chunk_map, -1, FTL_BLOCK_SIZE * nv_cache->tail_md_chunk_blocks);

	return 0;
}

int
ftl_nv_cache_load_state(struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_nv_cache_chunk *chunk;
	uint64_t chunks_number, offset, i;
	int status = 0;
	bool active;

	nv_cache->chunk_current = NULL;
	TAILQ_INIT(&nv_cache->chunk_free_list);
	TAILQ_INIT(&nv_cache->chunk_full_list);
	TAILQ_INIT(&nv_cache->chunk_inactive_list);
	nv_cache->chunk_full_count = 0;
	nv_cache->chunk_free_count = 0;
	nv_cache->chunk_inactive_count = 0;

	assert(nv_cache->chunk_open_count == 0);
	offset = nvc_data_offset(nv_cache);
	if (!nv_cache->chunks) {
		FTL_ERRLOG(dev, "No NV cache metadata\n");
		return -1;
	}

	if (dev->sb->upgrade_ready) {
		/*
		 * During upgrade some transitions are allowed:
		 *
		 * 1. FREE -> INACTIVE
		 * 2. INACTIVE -> FREE
		 */
		chunk = nv_cache->chunks;
		for (i = 0; i < nv_cache->chunk_count; i++, chunk++) {
			active = nv_cache->nvc_type->ops.is_chunk_active(dev, chunk->offset);

			if (chunk->md->state == FTL_CHUNK_STATE_FREE) {
				if (!active) {
					chunk->md->state = FTL_CHUNK_STATE_INACTIVE;
				}
			} else if (chunk->md->state == FTL_CHUNK_STATE_INACTIVE) {
				if (active) {
					chunk->md->state = FTL_CHUNK_STATE_FREE;
				}
			}
		}
	}

	chunk = nv_cache->chunks;
	for (i = 0; i < nv_cache->chunk_count; i++, chunk++) {
		chunk->nv_cache = nv_cache;
		nvc_validate_md(nv_cache, chunk->md);

		if (offset != chunk->offset) {
			status = -EINVAL;
			goto error;
		}

		if (chunk->md->version != FTL_NVC_VERSION_CURRENT) {
			status = -EINVAL;
			goto error;
		}

		active = nv_cache->nvc_type->ops.is_chunk_active(dev, chunk->offset);
		if (false == active) {
			if (chunk->md->state != FTL_CHUNK_STATE_INACTIVE) {
				status = -EINVAL;
				goto error;
			}
		}

		switch (chunk->md->state) {
		case FTL_CHUNK_STATE_FREE:
			if (chunk->md->blocks_written || chunk->md->write_pointer) {
				status = -EINVAL;
				goto error;
			}
			/* Chunk empty, move it on empty list */
			TAILQ_INSERT_TAIL(&nv_cache->chunk_free_list, chunk, entry);
			nv_cache->chunk_free_count++;
			break;
		case FTL_CHUNK_STATE_OPEN:
			/* All chunks needs to closed at this point */
			status = -EINVAL;
			goto error;
			break;
		case FTL_CHUNK_STATE_CLOSED:
			if (chunk->md->blocks_written != nv_cache->chunk_blocks) {
				status = -EINVAL;
				goto error;
			}
			/* Chunk full, move it on full list */
			TAILQ_INSERT_TAIL(&nv_cache->chunk_full_list, chunk, entry);
			nv_cache->chunk_full_count++;
			break;
		case FTL_CHUNK_STATE_INACTIVE:
			TAILQ_INSERT_TAIL(&nv_cache->chunk_inactive_list, chunk, entry);
			nv_cache->chunk_inactive_count++;
			break;
		default:
			status = -EINVAL;
			FTL_ERRLOG(dev, "Invalid chunk state\n");
			goto error;
		}

		offset += nv_cache->chunk_blocks;
	}

	chunks_number = nv_cache->chunk_free_count + nv_cache->chunk_full_count +
			nv_cache->chunk_inactive_count;
	assert(nv_cache->chunk_current == NULL);

	if (chunks_number != nv_cache->chunk_count) {
		FTL_ERRLOG(dev, "Inconsistent NV cache metadata\n");
		status = -EINVAL;
		goto error;
	}

	status = sort_chunks(nv_cache);
	if (status) {
		FTL_ERRLOG(dev, "FTL NV Cache: sorting chunks ERROR\n");
	}

	FTL_NOTICELOG(dev, "FTL NV Cache: full chunks = %lu, empty chunks = %lu\n",
		      nv_cache->chunk_full_count, nv_cache->chunk_free_count);

	if (0 == status) {
		FTL_NOTICELOG(dev, "FTL NV Cache: state loaded successfully\n");
	} else {
		FTL_ERRLOG(dev, "FTL NV Cache: loading state ERROR\n");
	}

	/* The number of active/inactive chunks calculated at initialization can change at this point due to metadata
	 * upgrade. Recalculate the thresholds that depend on active chunk count.
	 */
	ftl_nv_cache_init_update_limits(dev);
error:
	return status;
}

void
ftl_nv_cache_get_max_seq_id(struct ftl_nv_cache *nv_cache, uint64_t *open_seq_id,
			    uint64_t *close_seq_id)
{
	uint64_t i, o_seq_id = 0, c_seq_id = 0;
	struct ftl_nv_cache_chunk *chunk;

	chunk = nv_cache->chunks;
	assert(chunk);

	/* Iterate over chunks and get their max open and close seq id */
	for (i = 0; i < nv_cache->chunk_count; i++, chunk++) {
		o_seq_id = spdk_max(o_seq_id, chunk->md->seq_id);
		c_seq_id = spdk_max(c_seq_id, chunk->md->close_seq_id);
	}

	*open_seq_id = o_seq_id;
	*close_seq_id = c_seq_id;
}

typedef void (*ftl_chunk_ops_cb)(struct ftl_nv_cache_chunk *chunk, void *cntx, bool status);

static void
write_brq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_basic_rq *brq = arg;
	struct ftl_nv_cache_chunk *chunk = brq->io.chunk;

	ftl_stats_bdev_io_completed(brq->dev, FTL_STATS_TYPE_MD_NV_CACHE, bdev_io);

	brq->success = success;
	if (spdk_likely(success)) {
		chunk_advance_blocks(chunk->nv_cache, chunk, brq->num_blocks);
	}

	spdk_bdev_free_io(bdev_io);
	brq->owner.cb(brq);
}

static void
_ftl_chunk_basic_rq_write(void *_brq)
{
	struct ftl_basic_rq *brq = _brq;
	struct ftl_nv_cache *nv_cache = brq->io.chunk->nv_cache;
	int rc;

	rc = ftl_nv_cache_bdev_write_blocks_with_md(nv_cache->bdev_desc, nv_cache->cache_ioch,
			brq->io_payload, NULL, brq->io.addr,
			brq->num_blocks, write_brq_end, brq);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			brq->io.bdev_io_wait.bdev = bdev;
			brq->io.bdev_io_wait.cb_fn = _ftl_chunk_basic_rq_write;
			brq->io.bdev_io_wait.cb_arg = brq;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &brq->io.bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

static void
ftl_chunk_basic_rq_write(struct ftl_nv_cache_chunk *chunk, struct ftl_basic_rq *brq)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);

	brq->io.chunk = chunk;
	brq->success = false;

	_ftl_chunk_basic_rq_write(brq);

	chunk->md->write_pointer += brq->num_blocks;
	dev->stats.io_activity_total += brq->num_blocks;
}

static void
read_brq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_basic_rq *brq = arg;

	ftl_stats_bdev_io_completed(brq->dev, FTL_STATS_TYPE_MD_NV_CACHE, bdev_io);

	brq->success = success;

	brq->owner.cb(brq);
	spdk_bdev_free_io(bdev_io);
}

static int
ftl_chunk_basic_rq_read(struct ftl_nv_cache_chunk *chunk, struct ftl_basic_rq *brq)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	int rc;

	brq->io.chunk = chunk;
	brq->success = false;

	rc = ftl_nv_cache_bdev_read_blocks_with_md(nv_cache->bdev_desc, nv_cache->cache_ioch,
			brq->io_payload, NULL, brq->io.addr, brq->num_blocks, read_brq_end, brq);

	if (spdk_likely(!rc)) {
		dev->stats.io_activity_total += brq->num_blocks;
	}

	return rc;
}

static void
chunk_open_cb(int status, void *ctx)
{
	struct ftl_nv_cache_chunk *chunk = (struct ftl_nv_cache_chunk *)ctx;

	if (spdk_unlikely(status)) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		ftl_md_persist_entry_retry(&chunk->md_persist_entry_ctx);
		return;
#else
		ftl_abort();
#endif
	}

	chunk->md->state = FTL_CHUNK_STATE_OPEN;
}

static void
ftl_chunk_open(struct ftl_nv_cache_chunk *chunk)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;
	struct ftl_layout_region *region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD);
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];

	if (chunk_alloc_p2l_map(chunk)) {
		assert(0);
		/*
		 * We control number of opening chunk and it shall be consistent with size of chunk
		 * P2L map pool
		 */
		ftl_abort();
		return;
	}

	chunk->nv_cache->chunk_open_count++;

	assert(chunk->md->write_pointer == 0);
	assert(chunk->md->blocks_written == 0);

	if (dev->nv_cache.nvc_type->ops.on_chunk_open) {
		dev->nv_cache.nvc_type->ops.on_chunk_open(dev, chunk);
	}

	memcpy(p2l_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
	p2l_map->chunk_dma_md->state = FTL_CHUNK_STATE_OPEN;
	p2l_map->chunk_dma_md->p2l_map_checksum = 0;

	ftl_md_persist_entries(md, get_chunk_idx(chunk), 1, p2l_map->chunk_dma_md,
			       NULL, chunk_open_cb, chunk,
			       &chunk->md_persist_entry_ctx);
}

static void
chunk_close_cb(int status, void *ctx)
{
	struct ftl_nv_cache_chunk *chunk = (struct ftl_nv_cache_chunk *)ctx;
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);

	assert(chunk->md->write_pointer == chunk->nv_cache->chunk_blocks);

	if (spdk_likely(!status)) {
		chunk->md->p2l_map_checksum = chunk->p2l_map.chunk_dma_md->p2l_map_checksum;
		chunk_free_p2l_map(chunk);

		assert(chunk->nv_cache->chunk_open_count > 0);
		chunk->nv_cache->chunk_open_count--;

		/* Chunk full move it on full list */
		TAILQ_INSERT_TAIL(&chunk->nv_cache->chunk_full_list, chunk, entry);
		chunk->nv_cache->chunk_full_count++;

		chunk->nv_cache->last_seq_id = chunk->md->close_seq_id;

		chunk->md->state = FTL_CHUNK_STATE_CLOSED;
		if (nv_cache->nvc_type->ops.on_chunk_closed) {
			nv_cache->nvc_type->ops.on_chunk_closed(dev, chunk);
		}
	} else {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		ftl_md_persist_entry_retry(&chunk->md_persist_entry_ctx);
#else
		ftl_abort();
#endif
	}
}

static void
chunk_map_write_cb(struct ftl_basic_rq *brq)
{
	struct ftl_nv_cache_chunk *chunk = brq->io.chunk;
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_layout_region *region = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD);
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];
	uint32_t chunk_map_crc;

	if (spdk_likely(brq->success)) {
		chunk_map_crc = spdk_crc32c_update(p2l_map->chunk_map,
						   chunk->nv_cache->tail_md_chunk_blocks * FTL_BLOCK_SIZE, 0);
		memcpy(p2l_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
		p2l_map->chunk_dma_md->state = FTL_CHUNK_STATE_CLOSED;
		p2l_map->chunk_dma_md->p2l_map_checksum = chunk_map_crc;
		ftl_md_persist_entries(md, get_chunk_idx(chunk), 1, chunk->p2l_map.chunk_dma_md,
				       NULL, chunk_close_cb, chunk,
				       &chunk->md_persist_entry_ctx);
	} else {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* retry */
		chunk->md->write_pointer -= brq->num_blocks;
		ftl_chunk_basic_rq_write(chunk, brq);
#else
		ftl_abort();
#endif
	}
}

static void
ftl_chunk_close(struct ftl_nv_cache_chunk *chunk)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_basic_rq *brq = &chunk->metadata_rq;
	void *metadata = chunk->p2l_map.chunk_map;

	chunk->md->close_seq_id = ftl_get_next_seq_id(dev);
	ftl_basic_rq_init(dev, brq, metadata, chunk->nv_cache->tail_md_chunk_blocks);
	ftl_basic_rq_set_owner(brq, chunk_map_write_cb, chunk);

	assert(chunk->md->write_pointer == chunk_tail_md_offset(chunk->nv_cache));
	brq->io.addr = chunk->offset + chunk->md->write_pointer;

	ftl_chunk_basic_rq_write(chunk, brq);
}

static int
ftl_chunk_read_tail_md(struct ftl_nv_cache_chunk *chunk, struct ftl_basic_rq *brq,
		       void (*cb)(struct ftl_basic_rq *brq), void *cb_ctx)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	void *metadata;
	int rc;

	metadata = chunk->p2l_map.chunk_map;
	ftl_basic_rq_init(dev, brq, metadata, chunk->nv_cache->tail_md_chunk_blocks);
	ftl_basic_rq_set_owner(brq, cb, cb_ctx);

	brq->io.addr = chunk->offset + chunk_tail_md_offset(chunk->nv_cache);
	rc = ftl_chunk_basic_rq_read(chunk, brq);

	return rc;
}

struct restore_chunk_md_ctx {
	ftl_chunk_md_cb cb;
	void *cb_ctx;
	int status;
	uint64_t qd;
	uint64_t id;
};

static inline bool
is_chunk_count_valid(struct ftl_nv_cache *nv_cache)
{
	uint64_t chunk_count = 0;

	chunk_count += nv_cache->chunk_open_count;
	chunk_count += nv_cache->chunk_free_count;
	chunk_count += nv_cache->chunk_full_count;
	chunk_count += nv_cache->chunk_comp_count;
	chunk_count += nv_cache->chunk_inactive_count;

	return chunk_count == nv_cache->chunk_count;
}

static void
walk_tail_md_cb(struct ftl_basic_rq *brq)
{
	struct ftl_mngt_process *mngt = brq->owner.priv;
	struct ftl_nv_cache_chunk *chunk = brq->io.chunk;
	struct restore_chunk_md_ctx *ctx = ftl_mngt_get_step_ctx(mngt);
	int rc = 0;

	if (brq->success) {
		rc = ctx->cb(chunk, ctx->cb_ctx);
	} else {
		rc = -EIO;
	}

	if (rc) {
		ctx->status = rc;
	}
	ctx->qd--;
	chunk_free_p2l_map(chunk);
	ftl_mngt_continue_step(mngt);
}

static void
ftl_mngt_nv_cache_walk_tail_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
			       uint64_t seq_id, ftl_chunk_md_cb cb, void *cb_ctx)
{
	struct ftl_nv_cache *nvc = &dev->nv_cache;
	struct restore_chunk_md_ctx *ctx;

	ctx = ftl_mngt_get_step_ctx(mngt);
	if (!ctx) {
		if (ftl_mngt_alloc_step_ctx(mngt, sizeof(*ctx))) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ctx = ftl_mngt_get_step_ctx(mngt);
		assert(ctx);

		ctx->cb = cb;
		ctx->cb_ctx = cb_ctx;
	}

	/*
	 * This function generates a high queue depth and will utilize ftl_mngt_continue_step during completions to make sure all chunks
	 * are processed before returning an error (if any were found) or continuing on.
	 */
	if (0 == ctx->qd && ctx->id == nvc->chunk_count) {
		if (!is_chunk_count_valid(nvc)) {
			FTL_ERRLOG(dev, "Recovery ERROR, invalid number of chunk\n");
			assert(false);
			ctx->status = -EINVAL;
		}

		if (ctx->status) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
		return;
	}

	while (ctx->id < nvc->chunk_count) {
		struct ftl_nv_cache_chunk *chunk = &nvc->chunks[ctx->id];
		int rc;

		if (!chunk->recovery) {
			/* This chunk is inactive or empty and not used in recovery */
			ctx->id++;
			continue;
		}

		if (seq_id && (chunk->md->close_seq_id <= seq_id)) {
			ctx->id++;
			continue;
		}

		if (chunk_alloc_p2l_map(chunk)) {
			/* No more free P2L map, break and continue later */
			break;
		}
		ctx->id++;

		rc = ftl_chunk_read_tail_md(chunk, &chunk->metadata_rq, walk_tail_md_cb, mngt);

		if (0 == rc) {
			ctx->qd++;
		} else {
			chunk_free_p2l_map(chunk);
			ctx->status = rc;
		}
	}

	if (0 == ctx->qd) {
		/*
		 * No QD could happen due to all leftover chunks being in free state.
		 * Additionally ftl_chunk_read_tail_md could fail starting with the first IO in a given patch.
		 * For streamlining of all potential error handling (since many chunks are reading P2L at the same time),
		 * we're using ftl_mngt_continue_step to arrive at the same spot of checking for mngt step end (see beginning of function).
		 */
		ftl_mngt_continue_step(mngt);
	}

}

void
ftl_mngt_nv_cache_restore_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
			      ftl_chunk_md_cb cb, void *cb_ctx)
{
	ftl_mngt_nv_cache_walk_tail_md(dev, mngt, dev->sb->ckpt_seq_id, cb, cb_ctx);
}

static void
restore_chunk_state_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
	struct ftl_nv_cache *nvc = &dev->nv_cache;
	struct ftl_nv_cache_chunk *chunk;
	uint64_t i;

	if (status) {
		/* Restore error, end step */
		ftl_mngt_fail_step(mngt);
		return;
	}

	for (i = 0; i < nvc->chunk_count; i++) {
		chunk = &nvc->chunks[i];

		if (false == nvc->nvc_type->ops.is_chunk_active(dev, chunk->offset) &&
		    chunk->md->state != FTL_CHUNK_STATE_INACTIVE) {
			status = -EINVAL;
			break;
		}

		if (chunk->md->version != FTL_NVC_VERSION_CURRENT) {
			status = -EINVAL;
			break;
		}

		switch (chunk->md->state) {
		case FTL_CHUNK_STATE_FREE:
			break;
		case FTL_CHUNK_STATE_OPEN:
			TAILQ_REMOVE(&nvc->chunk_free_list, chunk, entry);
			nvc->chunk_free_count--;

			TAILQ_INSERT_TAIL(&nvc->chunk_open_list, chunk, entry);
			nvc->chunk_open_count++;

			/* Chunk is not empty, mark it to be recovered */
			chunk->recovery = true;
			break;
		case FTL_CHUNK_STATE_CLOSED:
			TAILQ_REMOVE(&nvc->chunk_free_list, chunk, entry);
			nvc->chunk_free_count--;

			TAILQ_INSERT_TAIL(&nvc->chunk_full_list, chunk, entry);
			nvc->chunk_full_count++;

			/* Chunk is not empty, mark it to be recovered */
			chunk->recovery = true;
			break;
		case FTL_CHUNK_STATE_INACTIVE:
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

void
ftl_mngt_nv_cache_restore_chunk_state(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];

	md->owner.cb_ctx = mngt;
	md->cb = restore_chunk_state_cb;
	ftl_md_restore(md);
}

struct recover_open_chunk_ctx {
	struct ftl_nv_cache_chunk *chunk;
};

static void
recover_open_chunk_prepare(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_nv_cache *nvc = &dev->nv_cache;
	struct recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ftl_bug(TAILQ_EMPTY(&nvc->chunk_open_list));
	ctx->chunk = TAILQ_FIRST(&nvc->chunk_open_list);

	FTL_NOTICELOG(dev, "Start recovery open chunk, offset = %"PRIu64", seq id %"PRIu64"\n",
		      ctx->chunk->offset, ctx->chunk->md->seq_id);

	if (chunk_alloc_p2l_map(ctx->chunk)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void
recover_open_chunk_persist_p2l_map_cb(struct ftl_basic_rq *rq)
{
	struct ftl_mngt_process *mngt = rq->owner.priv;

	if (rq->success) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static void
recover_open_chunk_persist_p2l_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;
	struct ftl_basic_rq *rq = ftl_mngt_get_step_ctx(mngt);
	void *p2l_map = chunk->p2l_map.chunk_map;

	ftl_basic_rq_init(dev, rq, p2l_map, chunk->nv_cache->tail_md_chunk_blocks);
	ftl_basic_rq_set_owner(rq, recover_open_chunk_persist_p2l_map_cb, mngt);

	rq->io.addr = chunk->offset + chunk_tail_md_offset(chunk->nv_cache);
	ftl_chunk_basic_rq_write(chunk, rq);
}

static void
recover_open_chunk_close_chunk_cb(int status, void *cb_arg)
{
	struct ftl_mngt_process *mngt = cb_arg;
	struct recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;
	struct ftl_nv_cache *nvc = chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nvc, struct spdk_ftl_dev, nv_cache);

	if (0 == status) {
		*chunk->md = *chunk->p2l_map.chunk_dma_md;

		FTL_NOTICELOG(dev, "Recovered chunk, offset = %"PRIu64", seq id %"PRIu64"\n", chunk->offset,
			      chunk->md->seq_id);

		TAILQ_REMOVE(&nvc->chunk_open_list, chunk, entry);
		nvc->chunk_open_count--;

		TAILQ_INSERT_TAIL(&nvc->chunk_full_list, chunk, entry);
		nvc->chunk_full_count++;

		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static void
recover_open_chunk_close_chunk(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;
	struct ftl_nv_cache_chunk_md *chunk_md = chunk->p2l_map.chunk_dma_md;
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];

	*chunk_md = *chunk->md;

	chunk_md->state = FTL_CHUNK_STATE_CLOSED;
	chunk_md->write_pointer = chunk->nv_cache->chunk_blocks;
	chunk_md->blocks_written = chunk->nv_cache->chunk_blocks;
	chunk_md->p2l_map_checksum = spdk_crc32c_update(chunk->p2l_map.chunk_map,
				     chunk->nv_cache->tail_md_chunk_blocks * FTL_BLOCK_SIZE, 0);

	ftl_md_persist_entries(md, get_chunk_idx(chunk), 1, chunk_md, NULL,
			       recover_open_chunk_close_chunk_cb, mngt,
			       &chunk->md_persist_entry_ctx);
}

static void
recover_open_chunk_execute(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache *nvc = ctx->chunk->nv_cache;

	nvc->nvc_type->ops.recover_open_chunk(dev, mngt, ctx->chunk);
}

static void
recover_open_chunk_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;

	if (chunk->p2l_map.chunk_map) {
		chunk_free_p2l_map(ctx->chunk);
	}
	ftl_mngt_next_step(mngt);
}

static const struct ftl_mngt_process_desc desc_recover_open_chunk = {
	.name = "Recover open chunk",
	.ctx_size = sizeof(struct recover_open_chunk_ctx),
	.steps = {
		{
			.name = "Chunk recovery, prepare",
			.action = recover_open_chunk_prepare,
			.cleanup = recover_open_chunk_cleanup
		},
		{
			.name = "Chunk recovery, execute",
			.action = recover_open_chunk_execute,
		},
		{
			.name = "Chunk recovery, persist P2L map",
			.ctx_size = sizeof(struct ftl_basic_rq),
			.action = recover_open_chunk_persist_p2l_map,
		},
		{
			.name = "Chunk recovery, close chunk",
			.action = recover_open_chunk_close_chunk,
		},
		{
			.name = "Chunk recovery, cleanup",
			.action = recover_open_chunk_cleanup,
		},
		{}
	}
};

static void
ftl_mngt_nv_cache_recover_open_chunk_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_mngt_process *mngt = ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_continue_step(mngt);
	}
}

void
ftl_mngt_nv_cache_recover_open_chunk(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_nv_cache *nvc = &dev->nv_cache;

	if (TAILQ_EMPTY(&nvc->chunk_open_list)) {
		if (!is_chunk_count_valid(nvc)) {
			FTL_ERRLOG(dev, "Recovery ERROR, invalid number of chunk\n");
			ftl_mngt_fail_step(mngt);
			return;
		}

		/*
		 * Now all chunks loaded and closed, do final step of restoring
		 * chunks state
		 */
		if (ftl_nv_cache_load_state(nvc)) {
			ftl_mngt_fail_step(mngt);
		} else {
			ftl_mngt_next_step(mngt);
		}
	} else {
		if (ftl_mngt_process_execute(dev, &desc_recover_open_chunk,
					     ftl_mngt_nv_cache_recover_open_chunk_cb, mngt)) {
			ftl_mngt_fail_step(mngt);
		}
	}
}

int
ftl_nv_cache_chunks_busy(struct ftl_nv_cache *nv_cache)
{
	/* chunk_current is migrating to closed status when closing, any others should already be
	 * moved to free chunk list. Also need to wait for free md requests */
	return nv_cache->chunk_open_count == 0 && nv_cache->chunk_free_persist_count == 0;
}

void
ftl_nv_cache_halt(struct ftl_nv_cache *nv_cache)
{
	struct ftl_nv_cache_chunk *chunk;
	uint64_t free_space;

	nv_cache->halt = true;

	/* Set chunks on open list back to free state since no user data has been written to it */
	while (!TAILQ_EMPTY(&nv_cache->chunk_open_list)) {
		chunk = TAILQ_FIRST(&nv_cache->chunk_open_list);

		/* Chunks are moved between lists on metadata update submission, but state is changed
		 * on completion. Breaking early in such a case to make sure all the necessary resources
		 * will be freed (during next pass(es) of ftl_nv_cache_halt).
		 */
		if (chunk->md->state != FTL_CHUNK_STATE_OPEN) {
			break;
		}

		TAILQ_REMOVE(&nv_cache->chunk_open_list, chunk, entry);
		chunk_free_p2l_map(chunk);
		ftl_nv_cache_chunk_md_initialize(chunk->md);
		assert(nv_cache->chunk_open_count > 0);
		nv_cache->chunk_open_count--;
	}

	/* Close current chunk by skipping all not written blocks */
	chunk = nv_cache->chunk_current;
	if (chunk != NULL) {
		nv_cache->chunk_current = NULL;
		if (chunk_is_closed(chunk)) {
			return;
		}

		free_space = chunk_get_free_space(nv_cache, chunk);
		chunk->md->blocks_skipped = free_space;
		chunk->md->blocks_written += free_space;
		chunk->md->write_pointer += free_space;
		ftl_chunk_close(chunk);
	}
}

uint64_t
ftl_nv_cache_acquire_trim_seq_id(struct ftl_nv_cache *nv_cache)
{
	struct ftl_nv_cache_chunk *chunk = nv_cache->chunk_current;
	uint64_t seq_id, free_space;

	if (!chunk) {
		chunk = TAILQ_FIRST(&nv_cache->chunk_open_list);
		if (chunk && chunk->md->state == FTL_CHUNK_STATE_OPEN) {
			return chunk->md->seq_id;
		} else {
			return 0;
		}
	}

	if (chunk_is_closed(chunk)) {
		return 0;
	}

	seq_id = nv_cache->chunk_current->md->seq_id;
	free_space = chunk_get_free_space(nv_cache, chunk);

	chunk->md->blocks_skipped = free_space;
	chunk->md->blocks_written += free_space;
	chunk->md->write_pointer += free_space;
	if (chunk->md->blocks_written == chunk_tail_md_offset(nv_cache)) {
		ftl_chunk_close(chunk);
	}
	nv_cache->chunk_current = NULL;

	seq_id++;
	return seq_id;
}

static double
ftl_nv_cache_get_chunk_utilization(struct ftl_nv_cache *nv_cache,
				   struct ftl_nv_cache_chunk *chunk)
{
	double capacity = nv_cache->chunk_blocks;
	double used = chunk->md->blocks_written + chunk->md->blocks_skipped;

	return used / capacity;
}

static const char *
ftl_nv_cache_get_chunk_state_name(struct ftl_nv_cache_chunk *chunk)
{
	static const char *names[] = {
		"FREE", "OPEN", "CLOSED", "INACTIVE"
	};

	assert(chunk->md->state < SPDK_COUNTOF(names));
	if (chunk->md->state < SPDK_COUNTOF(names)) {
		return names[chunk->md->state];
	} else {
		assert(false);
		return "?";
	}
}

static void
ftl_property_dump_cache_dev(struct spdk_ftl_dev *dev, const struct ftl_property *property,
			    struct spdk_json_write_ctx *w)
{
	uint64_t i;
	struct ftl_nv_cache_chunk *chunk;

	spdk_json_write_named_string(w, "type", dev->nv_cache.nvc_type->name);
	spdk_json_write_named_array_begin(w, "chunks");
	for (i = 0, chunk = dev->nv_cache.chunks; i < dev->nv_cache.chunk_count; i++, chunk++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "id", i);
		spdk_json_write_named_string(w, "state", ftl_nv_cache_get_chunk_state_name(chunk));
		spdk_json_write_named_double(w, "utilization",
					     ftl_nv_cache_get_chunk_utilization(&dev->nv_cache, chunk));
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

void
ftl_nv_cache_chunk_md_initialize(struct ftl_nv_cache_chunk_md *md)
{
	memset(md, 0, sizeof(*md));
	md->version = FTL_NVC_VERSION_CURRENT;
}
