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


#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/ftl.h"
#include "spdk/string.h"

#include "ftl_nv_cache.h"
#include "ftl_nv_cache_io.h"
#include "ftl_core.h"
#include "ftl_band.h"
#include "utils/ftl_addr_utils.h"
#include "mngt/ftl_mngt.h"

static struct ftl_nv_cache_compaction *compaction_alloc(struct spdk_ftl_dev *dev);
static void compaction_free(struct spdk_ftl_dev *dev, struct ftl_nv_cache_compaction *compaction);
static void compaction_process_ftl_done(struct ftl_rq *rq);

static inline const struct ftl_layout_region *nvc_data_region(
	struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev;

	dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	return &dev->layout.region[ftl_layout_region_type_data_nvc];
}

static inline void
nvc_metadata_validate(struct ftl_nv_cache *nv_cache,
		      struct ftl_nv_cache_chunk_md *chunk_metadata)
{
	struct ftl_md *md = nv_cache->md;
	void *buffer = ftl_md_get_buffer(md);
	uint64_t size = ftl_md_get_buffer_size(md);
	void *ptr = chunk_metadata;

	if (ptr < buffer) {
		ftl_abort();
	}

	ptr += sizeof(*chunk_metadata);
	if (ptr > buffer + size) {
		ftl_abort();
	}
}

static inline uint64_t nvc_data_offset(struct ftl_nv_cache *nv_cache)
{
	return nvc_data_region(nv_cache)->current.offset;
}

static inline uint64_t nvc_data_blocks(struct ftl_nv_cache *nv_cache)
{
	return nvc_data_region(nv_cache)->current.blocks;
}

static size_t
chunk_lba_map_num_blocks(const struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev =  SPDK_CONTAINEROF(nv_cache,
				    struct spdk_ftl_dev, nv_cache);
	return spdk_divide_round_up(dev->layout.nvc.chunk_data_blocks * dev->layout.l2p.addr_size,
				    FTL_BLOCK_SIZE);
}

size_t
ftl_nv_cache_chunk_tail_md_num_blocks(const struct ftl_nv_cache *nv_cache)
{
	return chunk_lba_map_num_blocks(nv_cache);
}

static size_t
nv_cache_lba_map_pool_elem_size(const struct ftl_nv_cache *nv_cache)
{
	/* Map pool element holds the whole tail md */
	return ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache) * FTL_BLOCK_SIZE;
}

static inline bool is_chunk_count_valid(struct ftl_nv_cache *nv_cache)
{
	uint64_t chunk_count = 0;

	chunk_count += nv_cache->chunk_open_count;
	chunk_count += nv_cache->chunk_free_count;
	chunk_count +=  nv_cache->chunk_full_count;
	chunk_count +=  nv_cache->chunk_comp_count;

	return chunk_count == nv_cache->chunk_count;
}

static uint64_t
get_chunk_idx(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache_chunk *first_chunk = chunk->nv_cache->chunks;
	return (chunk->offset - first_chunk->offset) / chunk->nv_cache->chunk_blocks;
}

static inline struct ftl_nv_cache_chunk_md *
nvc_metadata(struct ftl_nv_cache *nv_cache)
{
	return ftl_md_get_buffer(nv_cache->md);
}

int
ftl_nv_cache_init(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	uint64_t i, offset;

	nv_cache->halt = true;

	nv_cache->md = dev->layout.md[ftl_layout_region_type_nvc_md];
	if (!nv_cache->md) {
		FTL_ERRLOG(dev, "No NV cache metadata object\n");
		return -1;
	}

	nv_cache->md_pool = ftl_mempool_create(dev->conf.user_io_pool_size,
					       nv_cache->md_size * dev->xfer_size,
					       FTL_BLOCK_SIZE, SPDK_ENV_SOCKET_ID_ANY);
	if (!nv_cache->md_pool) {
		FTL_ERRLOG(dev, "Failed to initialize NV cache metadata pool\n");
		return -1;
	}

	/*
	 * Initialize chunk info
	 */
	nv_cache->chunk_blocks = dev->layout.nvc.chunk_data_blocks;
	nv_cache->chunk_count = dev->layout.nvc.chunk_count;

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
	TAILQ_INIT(&nv_cache->chunk_free_md_list);

	/* First chunk metadata */
	struct ftl_nv_cache_chunk_md *md = nvc_metadata(nv_cache);
	if (!md) {
		FTL_ERRLOG(dev, "No NV cache metadata\n");
		return -1;
	}

	nv_cache->chunk_free_count = nv_cache->chunk_count;

	struct ftl_nv_cache_chunk *chunk = nv_cache->chunks;
	offset = nvc_data_offset(nv_cache);
	for (i = 0; i < nv_cache->chunk_count; i++, chunk++, md++) {
		chunk->nv_cache = nv_cache;
		chunk->md = md;
		nvc_metadata_validate(nv_cache, md);
		chunk->offset = offset;
		offset += nv_cache->chunk_blocks;
		TAILQ_INSERT_TAIL(&nv_cache->chunk_free_list, chunk, entry);
	}
	assert(offset <= nvc_data_offset(nv_cache) + nvc_data_blocks(nv_cache));

	/* Start compaction when full chunks exceed given % of entire chunks */
	nv_cache->chunk_compaction_threshold = nv_cache->chunk_count *
					       dev->conf.nv_cache.chunk_compaction_threshold / 100;
	TAILQ_INIT(&nv_cache->compaction_list);
	for (i = 0; i < FTL_NV_CACHE_NUM_COMPACTORS; i++) {
		struct ftl_nv_cache_compaction *compact = compaction_alloc(dev);

		if (!compact) {
			FTL_ERRLOG(dev, "Cannot allocate compaction process\n");
			return -1;
		}

		TAILQ_INSERT_TAIL(&nv_cache->compaction_list, compact, entry);
	}

#define FTL_MAX_OPEN_CHUNKS 2
	nv_cache->lba_pool = ftl_mempool_create(FTL_MAX_OPEN_CHUNKS,
						nv_cache_lba_map_pool_elem_size(nv_cache),
						FTL_BLOCK_SIZE,
						SPDK_ENV_SOCKET_ID_ANY);
	if (!nv_cache->lba_pool) {
		return -ENOMEM;
	}

	/* One entry per open chunk */
	nv_cache->chunk_md_pool = ftl_mempool_create(FTL_MAX_OPEN_CHUNKS,
				  sizeof(struct ftl_nv_cache_chunk_md),
				  FTL_BLOCK_SIZE,
				  SPDK_ENV_SOCKET_ID_ANY);
	if (!nv_cache->chunk_md_pool) {
		return -ENOMEM;
	}

	/* Each compactor can be reading a different chunk which it needs to send free to at the end,
	 * plus one backup each for high invalidity chunks processing */
	nv_cache->free_chunk_md_pool = ftl_mempool_create(2 * FTL_NV_CACHE_NUM_COMPACTORS,
				       sizeof(struct ftl_nv_cache_chunk_md),
				       FTL_BLOCK_SIZE,
				       SPDK_ENV_SOCKET_ID_ANY);
	if (!nv_cache->free_chunk_md_pool) {
		return -ENOMEM;
	}

	return 0;
}

void
ftl_nv_cache_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_nv_cache_compaction *compaction;

	while (!TAILQ_EMPTY(&nv_cache->compaction_list)) {
		compaction = TAILQ_FIRST(&nv_cache->compaction_list);
		TAILQ_REMOVE(&nv_cache->compaction_list, compaction, entry);

		compaction_free(dev, compaction);
	}

	ftl_mempool_destroy(nv_cache->md_pool);
	ftl_mempool_destroy(nv_cache->lba_pool);
	ftl_mempool_destroy(nv_cache->chunk_md_pool);
	ftl_mempool_destroy(nv_cache->free_chunk_md_pool);
	nv_cache->md_pool = NULL;
	nv_cache->lba_pool = NULL;
	nv_cache->chunk_md_pool = NULL;
	nv_cache->free_chunk_md_pool = NULL;

	free(nv_cache->chunks);
	nv_cache->chunks = NULL;
}

static uint64_t
chunk_get_free_space(struct ftl_nv_cache *nv_cache,
		     struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->md->write_pointer + ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache) <= nv_cache->chunk_blocks);
	return nv_cache->chunk_blocks - chunk->md->write_pointer - ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache);
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

		if (chunk->md->blocks_written +
		    ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache) == nv_cache->chunk_blocks) {
			ftl_chunk_close(chunk);
		}
	} while (1);

	return address;
}

void
ftl_nv_cache_fill_md(struct ftl_io *io)
{
	uint64_t i;
	union ftl_md_vss *metadata = io->md;
	uint64_t lba = ftl_io_get_lba(io, 0);

	for (i = 0; i < io->num_blocks; ++i, lba++, metadata++) {
		metadata->nv_cache.lba = lba;
	}
}

uint64_t
chunk_tail_md_offset(struct ftl_nv_cache *nv_cache)
{
	return nv_cache->chunk_blocks - ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache);
}

static void
chunk_advance_blocks(struct ftl_nv_cache *nv_cache, struct ftl_nv_cache_chunk *chunk,
		     uint64_t advanced_blocks)
{
	chunk->md->blocks_written += advanced_blocks;

	assert(chunk->md->blocks_written <= nv_cache->chunk_blocks);

	if ((chunk->md->blocks_written +
	     ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache)) == nv_cache->chunk_blocks) {
		ftl_chunk_close(chunk);
	}
}

static uint64_t
chunk_md_num_blocks(struct ftl_nv_cache *nv_cache)
{
	return ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache);
}

static uint64_t
chunk_blocks_written(struct ftl_nv_cache_chunk *chunk)
{
	return chunk->md->blocks_written - chunk->md->blocks_skipped -
	       chunk_md_num_blocks(chunk->nv_cache);
}

static bool
is_chunk_compacted(struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->md->blocks_written != 0);

	if (chunk_blocks_written(chunk) == chunk->md->blocks_compacted) {
		return true;
	}

	return false;
}

static int
ftl_chunk_alloc_md_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_lba_map *lba_map = &chunk->lba_map;
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_nvc_md];

	lba_map->chunk_dma_md = ftl_mempool_get(nv_cache->chunk_md_pool);

	if (!lba_map->chunk_dma_md) {
		return -1;
	}

	memset(lba_map->chunk_dma_md, 0, region->entry_size * FTL_BLOCK_SIZE);
	return 0;
}

static void
ftl_chunk_free_md_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_lba_map *lba_map = &chunk->lba_map;

	ftl_mempool_put(chunk->nv_cache->chunk_md_pool, lba_map->chunk_dma_md);
	lba_map->chunk_dma_md = NULL;
}

static void
ftl_chunk_free(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;

	/* Reset chunk */
	memset(chunk->md, 0, sizeof(*chunk->md));

	TAILQ_INSERT_TAIL(&nv_cache->chunk_free_md_list, chunk, entry);
	nv_cache->chunk_free_md_count++;
}

static int
ftl_chunk_alloc_chunk_free_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_lba_map *lba_map = &chunk->lba_map;
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_nvc_md];

	lba_map->chunk_dma_md = ftl_mempool_get(nv_cache->free_chunk_md_pool);

	if (!lba_map->chunk_dma_md) {
		return -1;
	}

	memset(lba_map->chunk_dma_md, 0, region->entry_size * FTL_BLOCK_SIZE);
	return 0;
}

static void
ftl_chunk_free_chunk_free_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_lba_map *lba_map = &chunk->lba_map;

	ftl_mempool_put(chunk->nv_cache->free_chunk_md_pool, lba_map->chunk_dma_md);
	lba_map->chunk_dma_md = NULL;
}

static void chunk_free_cb(int status, void *ctx)
{
	struct ftl_nv_cache_chunk *chunk = (struct ftl_nv_cache_chunk *)ctx;

	if (spdk_likely(!status)) {
		struct ftl_nv_cache *nv_cache = chunk->nv_cache;

		nv_cache->chunk_free_md_count--;
		TAILQ_INSERT_TAIL(&nv_cache->chunk_free_list, chunk, entry);
		nv_cache->chunk_free_count++;
		nv_cache->chunk_full_count--;
		chunk->md->state = FTL_CHUNK_STATE_FREE;
		ftl_chunk_free_chunk_free_entry(chunk);
	} else {
		ftl_md_persist_entry_retry(&chunk->md_persist_entry_ctx);
	}
}

static void
ftl_chunk_send_free_state(struct ftl_nv_cache *nv_cache)
{
	int rc;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_lba_map *lba_map;
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_nvc_md];
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_nvc_md];
	struct ftl_nv_cache_chunk *tchunk, *chunk = NULL;

	TAILQ_FOREACH_SAFE(chunk, &nv_cache->chunk_free_md_list, entry, tchunk) {
		lba_map = &chunk->lba_map;
		rc = ftl_chunk_alloc_chunk_free_entry(chunk);
		if (rc) {
			break;
		}

		TAILQ_REMOVE(&nv_cache->chunk_free_md_list, chunk, entry);

		memcpy(lba_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
		lba_map->chunk_dma_md->state = FTL_CHUNK_STATE_FREE;
		lba_map->chunk_dma_md->lba_map_checksum = 0;

		ftl_md_persist_entry(md, get_chunk_idx(chunk), lba_map->chunk_dma_md, ftl_md_get_vss_buffer(md),
				     chunk_free_cb, chunk, &chunk->md_persist_entry_ctx);
	}
}

static void
chunk_compaction_advance(struct ftl_nv_cache_chunk *chunk, uint64_t num_blocks)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	uint64_t tsc = spdk_thread_get_last_tsc(spdk_get_thread());

	chunk->compaction_length_tsc += tsc - chunk->compaction_start_tsc;
	chunk->compaction_start_tsc = tsc;

	chunk->md->blocks_compacted += num_blocks;
	if (!is_chunk_compacted(chunk)) {
		return;
	}

	/* Remove chunk from compacted list */
	TAILQ_REMOVE(&nv_cache->chunk_comp_list, chunk, entry);
	nv_cache->chunk_comp_count--;

	ftl_chunk_free(chunk);
}

static bool
is_compaction_required(struct ftl_nv_cache *nv_cache)
{
	uint64_t full;

	if (spdk_unlikely(nv_cache->halt)) {
		return false;
	}

	full = nv_cache->chunk_full_count - nv_cache->compaction_active_count;
	if (full >= nv_cache->chunk_compaction_threshold) {
		return true;
	}

	return false;
}

static void compaction_process_finish_read(struct ftl_nv_cache_compaction *compaction);
static void compaction_process_pin_lba(struct ftl_nv_cache_compaction *comp);

static void
_compaction_process_pin_lba(void *_comp)
{
	struct ftl_nv_cache_compaction *comp = _comp;

	compaction_process_pin_lba(comp);
}

static void
compaction_process_pin_lba_cb(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_nv_cache_compaction *comp = pin_ctx->cb_ctx;
	struct ftl_rq *rq = comp->rd;

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
compaction_process_pin_lba(struct ftl_nv_cache_compaction *comp)
{
	union ftl_md_vss *md;
	struct spdk_ftl_dev *dev = comp->rd->dev;
	uint64_t i;
	uint32_t count = comp->rd->iter.count;
	struct ftl_rq_entry *entry;
	struct ftl_l2p_pin_ctx *pin_ctx;

	assert(comp->rd->iter.idx == 0);
	comp->rd->iter.remaining = count;
	comp->rd->iter.status = 0;

	for (i = 0; i < count; i++) {
		entry = &comp->rd->entries[i];
		pin_ctx = &entry->l2p_pin_ctx;
		md = entry->io_md;
		if (md->nv_cache.lba == FTL_LBA_INVALID) {
			ftl_l2p_pin_skip(dev, compaction_process_pin_lba_cb, comp, pin_ctx);
		} else {
			ftl_l2p_pin(dev, md->nv_cache.lba, 1, compaction_process_pin_lba_cb, comp, pin_ctx);
		}
	}
}

static int compaction_submit_read(struct ftl_nv_cache_compaction *compaction, ftl_addr addr,
				  uint64_t num_blocks);

static void
compaction_retry_read(void *_compaction)
{
	struct ftl_nv_cache_compaction *compaction = _compaction;
	struct ftl_rq *rq = compaction->rd;
	int ret;

	ret = compaction_submit_read(compaction, rq->io.addr, rq->iter.count);
	if (ret) {
		spdk_thread_send_msg(spdk_get_thread(), compaction_retry_read, compaction);
	}
}

static void
compaction_process_read_cb(struct spdk_bdev_io *bdev_io,
			   bool success, void *cb_arg)
{
	struct ftl_nv_cache_compaction *compaction = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		/* retry */
		spdk_thread_send_msg(spdk_get_thread(), compaction_retry_read, compaction);
		return;
	}

	compaction_process_pin_lba(compaction);
}

static bool
is_chunk_to_read(struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->md->blocks_written != 0);

	if (chunk_blocks_written(chunk) == chunk->md->read_pointer) {
		return false;
	}

	return true;
}

static struct ftl_nv_cache_chunk *
get_chunk_for_compaction(struct ftl_nv_cache *nv_cache)
{
	struct ftl_nv_cache_chunk *chunk = NULL;

	if (!TAILQ_EMPTY(&nv_cache->chunk_comp_list)) {
		chunk = TAILQ_FIRST(&nv_cache->chunk_comp_list);
		if (is_chunk_to_read(chunk)) {
			return chunk;
		}
	}

	if (!TAILQ_EMPTY(&nv_cache->chunk_full_list)) {
		chunk = TAILQ_FIRST(&nv_cache->chunk_full_list);
		TAILQ_REMOVE(&nv_cache->chunk_full_list, chunk, entry);

		assert(chunk->md->write_pointer);
	} else {
		return NULL;
	}

	if (spdk_likely(chunk)) {
		assert(chunk->md->write_pointer != 0);
		TAILQ_INSERT_HEAD(&nv_cache->chunk_comp_list, chunk, entry);
		nv_cache->chunk_comp_count++;
	}

	return chunk;
}

static uint64_t
chunk_blocks_to_read(struct ftl_nv_cache_chunk *chunk)
{
	uint64_t blocks_written;
	uint64_t blocks_to_read;

	assert(chunk->md->blocks_written >= chunk->md->blocks_skipped);
	blocks_written = chunk_blocks_written(chunk);

	assert(blocks_written >= chunk->md->read_pointer);
	blocks_to_read = blocks_written - chunk->md->read_pointer;

	return blocks_to_read;
}

static void
compaction_inactivate(struct ftl_nv_cache_compaction *compaction)
{
	struct ftl_nv_cache *nv_cache = compaction->nv_cache;

	nv_cache->compaction_active_count--;
	TAILQ_INSERT_TAIL(&nv_cache->compaction_list, compaction, entry);
}

static int
compaction_submit_read(struct ftl_nv_cache_compaction *compaction, ftl_addr addr,
		       uint64_t num_blocks)
{
	struct ftl_nv_cache *nv_cache = compaction->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);

	return ftl_nv_cache_bdev_readv_blocks_with_md(dev, nv_cache->bdev_desc,
			nv_cache->cache_ioch,
			compaction->rd->io_vec, num_blocks,
			compaction->rd->io_md,
			ftl_addr_get_cache_offset(dev, addr), num_blocks,
			compaction_process_read_cb,
			compaction);
}

static void
compaction_process_pad(struct ftl_nv_cache_compaction *compaction)
{
	struct ftl_rq *wr = compaction->wr;
	const uint64_t num_entries = wr->num_blocks;
	struct ftl_rq_entry *iter;

	iter = &wr->entries[wr->iter.idx];

	while (wr->iter.idx < num_entries) {
		iter->addr = FTL_ADDR_INVALID;
		iter->owner.priv = NULL;
		iter->lba = FTL_LBA_INVALID;
		iter++;
		wr->iter.idx++;
	}
}

static void
compaction_process_start(struct ftl_nv_cache_compaction *compaction)
{
	struct ftl_nv_cache *nv_cache = compaction->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache,
				   struct spdk_ftl_dev, nv_cache);
	struct ftl_nv_cache_chunk *chunk;
	uint64_t to_read, addr, begin, end, offset;
	int rc;

	/* Check if all read blocks done */
	assert(compaction->rd->iter.idx <= compaction->rd->iter.count);
	if (compaction->rd->iter.idx < compaction->rd->iter.count) {
		compaction_process_finish_read(compaction);
		return;
	}

	/*
	 * Get currently handled chunk
	 */
	chunk = get_chunk_for_compaction(nv_cache);
	if (!chunk) {
		/* No chunks to compact, pad this request */
		compaction_process_pad(compaction);
		ftl_writer_queue_rq(&dev->writer_user, compaction->wr);
		return;
	}

	chunk->compaction_start_tsc = spdk_thread_get_last_tsc(spdk_get_thread());

	/*
	 * Get range of blocks to read
	 */
	to_read = chunk_blocks_to_read(chunk);
	assert(to_read > 0);

	addr = ftl_addr_to_cached(dev, chunk->offset + chunk->md->read_pointer);
	begin = ftl_bitmap_find_first_set(dev->valid_map, addr, addr + to_read);
	if (begin != UINT64_MAX) {
		offset = spdk_min(begin - addr, to_read);
	} else {
		offset = to_read;
	}

	if (offset) {
		chunk->md->read_pointer += offset;
		chunk_compaction_advance(chunk, offset);
		to_read -= offset;
		if (!to_read) {
			compaction_inactivate(compaction);
			return;
		}
	}

	end = ftl_bitmap_find_first_clear(dev->valid_map, begin + 1, begin + to_read);
	if (end != UINT64_MAX) {
		to_read = end - begin;
	}

	addr = begin;
	to_read = spdk_min(to_read, compaction->rd->num_blocks);

	/* Read data and metadata from NV cache */
	rc = compaction_submit_read(compaction, addr, to_read);
	if (spdk_unlikely(rc)) {
		/* An error occurred, inactivate this compaction, it will retry
		 * in next iteration
		 */
		compaction_inactivate(compaction);
		return;
	}

	/* IO has started, initialize compaction */
	compaction->rd->owner.priv = chunk;
	compaction->rd->iter.idx = 0;
	compaction->rd->iter.count = to_read;
	compaction->rd->io.addr = addr;

	/* Move read pointer in the chunk */
	chunk->md->read_pointer += to_read;
}

static void
compaction_process_ftl_done(struct ftl_rq *rq)
{
	struct spdk_ftl_dev *dev = rq->dev;
	struct ftl_nv_cache_compaction *compaction = rq->owner.priv;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_band *band = rq->io.band;
	struct ftl_rq_entry *entry;
	ftl_addr addr;
	uint64_t i;

	if (spdk_unlikely(false == rq->success)) {
		/* IO error retry writing */
		ftl_writer_queue_rq(&dev->writer_user, rq);
		return;
	}

	/* Update L2P table */
	addr = rq->io.addr;
	for (i = 0, entry = rq->entries; i < rq->num_blocks; i++, entry++) {
		struct ftl_nv_cache_chunk *chunk = entry->owner.priv;

		if (entry->lba == FTL_LBA_INVALID) {
			assert(entry->addr == FTL_ADDR_INVALID);
			addr = ftl_band_next_addr(band, addr, 1);
			continue;
		}

		ftl_l2p_update(dev, entry->lba, addr, entry->addr);
		ftl_l2p_unpin(dev, entry->lba, 1);

		chunk_compaction_advance(chunk, 1);
		addr = ftl_band_next_addr(band, addr, 1);
	}

	compaction->wr->iter.idx = 0;

	if (is_compaction_required(nv_cache)) {
		compaction_process_start(compaction);
	} else {
		compaction_inactivate(compaction);
	}
}

static void
compaction_process_finish_read(struct ftl_nv_cache_compaction *compaction)
{
	struct ftl_rq *wr = compaction->wr;
	struct ftl_rq *rd = compaction->rd;
	ftl_addr cache_addr = rd->io.addr;
	struct ftl_nv_cache_chunk *chunk = rd->owner.priv;
	struct spdk_ftl_dev *dev;
	struct ftl_rq_entry *iter;
	union ftl_md_vss *md;
	ftl_addr current_addr;
	const uint64_t num_entries = wr->num_blocks;
	uint64_t tsc = spdk_thread_get_last_tsc(spdk_get_thread());

	chunk->compaction_length_tsc += tsc - chunk->compaction_start_tsc;
	chunk->compaction_start_tsc = tsc;

	dev = SPDK_CONTAINEROF(compaction->nv_cache,
			       struct spdk_ftl_dev, nv_cache);

	assert(wr->iter.idx < num_entries);
	assert(rd->iter.idx < rd->iter.count);

	cache_addr += rd->iter.idx;
	iter = &wr->entries[wr->iter.idx];

	while (wr->iter.idx < num_entries && rd->iter.idx < rd->iter.count) {
		/* Get metadata */
		md = rd->entries[rd->iter.idx].io_md;
		if (md->nv_cache.lba == FTL_LBA_INVALID) {
			cache_addr++;
			rd->iter.idx++;
			chunk_compaction_advance(chunk, 1);
			continue;
		}

		current_addr = ftl_l2p_get(dev, md->nv_cache.lba);
		if (current_addr == cache_addr) {
			/* Swap payload */
			ftl_rq_swap_payload(wr, wr->iter.idx, rd, rd->iter.idx);

			/*
			 * Address still the same, we may continue to compact it
			 * back to  FTL, set valid number of entries within
			 * this batch
			 */
			iter->addr = current_addr;
			iter->owner.priv = chunk;
			iter->lba = md->nv_cache.lba;

			/* Advance within batch */
			iter++;
			wr->iter.idx++;
		} else {
			/* This address already invalidated, just omit this block */
			chunk_compaction_advance(chunk, 1);
			ftl_l2p_unpin(dev, md->nv_cache.lba, 1);
		}

		/* Advance within reader */
		rd->iter.idx++;
		cache_addr++;
	}

	if (num_entries == wr->iter.idx) {
		/*
		 *Request contains data to be placed on FTL, compact it
		 */
		ftl_writer_queue_rq(&dev->writer_user, wr);
	} else {
		struct ftl_nv_cache *nv_cache = compaction->nv_cache;

		if (is_compaction_required(compaction->nv_cache)) {
			compaction_process_start(compaction);
		} else {
			nv_cache->compaction_active_count--;
			TAILQ_INSERT_HEAD(&nv_cache->compaction_list,
					  compaction, entry);
		}
	}
}

static void
compaction_free(struct spdk_ftl_dev *dev, struct ftl_nv_cache_compaction *compaction)
{
	if (!compaction) {
		return;
	}

	ftl_rq_del(compaction->wr);
	ftl_rq_del(compaction->rd);
	free(compaction);
}

static struct ftl_nv_cache_compaction *
compaction_alloc(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache_compaction *compaction;

	compaction = calloc(1, sizeof(*compaction));
	if (!compaction) {
		goto ERROR;
	}

	/* Allocate help request for writing */
	compaction->wr = ftl_rq_new(dev, dev->md_size);
	if (!compaction->wr) {
		goto ERROR;
	}

	/* Allocate help request for reading */
	compaction->rd = ftl_rq_new(dev, dev->nv_cache.md_size);
	if (!compaction->rd) {
		goto ERROR;
	}

	compaction->nv_cache = &dev->nv_cache;
	compaction->wr->owner.priv = compaction;
	compaction->wr->owner.cb = compaction_process_ftl_done;
	compaction->wr->owner.uio = true;

	return compaction;

ERROR:
	compaction_free(dev, compaction);
	return NULL;
}

static void
ftl_nv_cache_submit_cb_done(struct ftl_io *io)
{
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;

	chunk_advance_blocks(nv_cache, io->nv_cache_chunk, io->num_blocks);
	io->nv_cache_chunk = NULL;

	ftl_mempool_put(nv_cache->md_pool, io->md);
	ftl_io_complete(io);
}

static void
ftl_nv_cache_l2p_update(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	ftl_addr next_addr = io->addr;
	size_t i;

	for (i = 0; i < io->num_blocks; ++i, ++next_addr) {
		ftl_l2p_update_cached(dev, ftl_io_get_lba(io, i), next_addr, io->map[i]);
	}

	ftl_l2p_unpin(dev, io->lba, io->num_blocks);
	ftl_nv_cache_submit_cb_done(io);
}

static void
ftl_nv_cache_submit_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (spdk_unlikely(!success)) {
		FTL_ERRLOG(io->dev, "Non-volatile cache write failed at %"PRIx64"\n",
			   io->addr);
		io->status = -EIO;
		ftl_nv_cache_submit_cb_done(io);
	} else {
		ftl_nv_cache_l2p_update(io);
	}
}

static void
nv_cache_write(void *_io)
{
	struct ftl_io *io = _io;
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	int rc;

	rc = ftl_nv_cache_bdev_writev_blocks_with_md(dev,
			nv_cache->bdev_desc, nv_cache->cache_ioch,
			io->iov, io->iov_cnt, io->md,
			ftl_addr_get_cache_offset(dev, io->addr), io->num_blocks,
			ftl_nv_cache_submit_cb, io);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			io->bdev_io_wait.bdev = bdev;
			io->bdev_io_wait.cb_fn = nv_cache_write;
			io->bdev_io_wait.cb_arg = io;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &io->bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
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

	nv_cache_write(io);
}

bool
ftl_nv_cache_write(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	uint64_t cache_offset;
	io->md = ftl_mempool_get(dev->nv_cache.md_pool);
	if (spdk_unlikely(!io->md)) {
		return false;
	}

	/* Reserve area on the write buffer cache */
	cache_offset = ftl_nv_cache_get_wr_buffer(&dev->nv_cache, io);
	if (cache_offset == FTL_LBA_INVALID) {
		/* No free space in NV cache, resubmit request */
		ftl_mempool_put(dev->nv_cache.md_pool, io->md);
		return false;
	}
	io->addr = ftl_addr_to_cached(dev, cache_offset);
	io->nv_cache_chunk = dev->nv_cache.chunk_current;

	ftl_nv_cache_fill_md(io);
	ftl_l2p_pin(io->dev, io->lba, io->num_blocks,
		    ftl_nv_cache_pin_cb, io,
		    &io->l2p_pin_ctx);

	return true;
}

int
ftl_nv_cache_read(struct ftl_io *io, ftl_addr addr, uint32_t num_blocks,
		  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	int rc;
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;

	assert(ftl_addr_cached(io->dev, addr));

	rc = ftl_nv_cache_bdev_read_blocks_with_md(io->dev, nv_cache->bdev_desc, nv_cache->cache_ioch,
				   ftl_io_iovec_addr(io), NULL, ftl_addr_get_cache_offset(io->dev, addr),
				   num_blocks, cb, cb_arg);

	return rc;
}

bool
ftl_nv_cache_is_halted(struct ftl_nv_cache *nv_cache)
{
	struct ftl_nv_cache_compaction *compaction;

	if (nv_cache->compaction_active_count) {
		return false;
	}

	TAILQ_FOREACH(compaction, &nv_cache->compaction_list, entry) {
		if (compaction->rd->iter.idx != 0 || compaction->wr->iter.idx != 0) {
			return false;
		}
	}

	if (nv_cache->chunk_open_count > 0) {
		return false;
	}

	return true;
}

static void
ftl_nv_cache_compaction_reset(struct ftl_nv_cache_compaction *compaction)
{
	struct ftl_rq *rd = compaction->rd;
	struct ftl_rq *wr = compaction->wr;
	uint64_t lba;
	uint64_t i;

	for (i = rd->iter.idx; i < rd->iter.count; i++) {
		lba = ((union ftl_md_vss *)rd->entries[i].io_md)->nv_cache.lba;
		if (lba != FTL_LBA_INVALID) {
			ftl_l2p_unpin(rd->dev, lba, 1);
		}
	}

	rd->iter.idx = rd->iter.count = 0;

	for (i = 0; i < wr->iter.idx; i++) {
		lba = wr->entries[i].lba;
		assert(lba != FTL_LBA_INVALID);
		ftl_l2p_unpin(wr->dev, lba, 1);
	}

	wr->iter.idx = 0;
}

void ftl_chunk_map_set_lba(struct ftl_nv_cache_chunk *chunk,
			   uint64_t offset, uint64_t lba)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_lba_map *lba_map = &chunk->lba_map;

	ftl_lba_store(dev, lba_map->chunk_map, offset, lba);
}

uint64_t ftl_chunk_map_get_lba(struct ftl_nv_cache_chunk *chunk, uint64_t offset)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_lba_map *lba_map = &chunk->lba_map;

	return ftl_lba_load(dev, lba_map->chunk_map, offset);
}

static void
ftl_chunk_set_addr(struct ftl_nv_cache_chunk *chunk, uint64_t lba, ftl_addr addr)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	uint64_t cache_offset = ftl_addr_get_cache_offset(dev, addr);
	uint64_t offset;

	offset = (cache_offset - chunk->offset) % chunk->nv_cache->chunk_blocks;
	ftl_chunk_map_set_lba(chunk, offset, lba);
}

struct ftl_nv_cache_chunk *
ftl_nv_cache_get_chunk_from_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	struct ftl_nv_cache_chunk *chunk = dev->nv_cache.chunks;
	uint64_t chunk_idx;
	uint64_t cache_offset = ftl_addr_get_cache_offset(dev, addr);

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

	ftl_chunk_set_addr(chunk, lba, addr);
	ftl_bitmap_set(dev->valid_map, addr);
}

static void ftl_chunk_open(struct ftl_nv_cache_chunk *chunk);

void ftl_nv_cache_process(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;

	if (!dev->nv_cache.bdev_desc) {
		return;
	}

	if (nv_cache->chunk_open_count < FTL_MAX_OPEN_CHUNKS && spdk_likely(!nv_cache->halt)) {
		struct ftl_nv_cache_chunk *chunk;
		if (!TAILQ_EMPTY(&nv_cache->chunk_free_list)) {
			chunk = TAILQ_FIRST(&nv_cache->chunk_free_list);
			TAILQ_REMOVE(&nv_cache->chunk_free_list, chunk, entry);
			TAILQ_INSERT_TAIL(&nv_cache->chunk_open_list, chunk, entry);
			nv_cache->chunk_free_count--;
			ftl_chunk_open(chunk);
		}
	}

	if (is_compaction_required(nv_cache) &&
	    !TAILQ_EMPTY(&nv_cache->compaction_list)) {
		struct ftl_nv_cache_compaction *comp =
			TAILQ_FIRST(&nv_cache->compaction_list);

		TAILQ_REMOVE(&nv_cache->compaction_list, comp, entry);

		nv_cache->compaction_active_count++;
		compaction_process_start(comp);
	}

	ftl_chunk_send_free_state(nv_cache);

	if (spdk_unlikely(nv_cache->halt)) {
		struct ftl_nv_cache_compaction *compaction;

		TAILQ_FOREACH(compaction, &nv_cache->compaction_list, entry) {
			ftl_nv_cache_compaction_reset(compaction);
		}
	}
}

bool
ftl_nv_cache_full(struct ftl_nv_cache *nv_cache)
{
	if (0 == nv_cache->chunk_open_count && NULL == nv_cache->chunk_current) {
		return true;
	} else {
		return false;
	}
}

static void
chunk_free_lba_map(struct ftl_nv_cache_chunk *chunk)
{

	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct ftl_lba_map *lba_map = &chunk->lba_map;

	ftl_mempool_put(nv_cache->lba_pool, lba_map->dma_buf);
	lba_map->chunk_map = NULL;
	lba_map->dma_buf = NULL;

	ftl_chunk_free_md_entry(chunk);
}

int ftl_nv_cache_save_state(struct ftl_nv_cache *nv_cache)
{
	int status = 0;
	uint64_t i;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);

	assert(nv_cache->chunk_open_count == 0);

	if (nv_cache->compaction_active_count) {
		FTL_ERRLOG(dev, "Cannot save NV cache state, compaction in progress\n");
		return -EINVAL;
	}

	struct ftl_nv_cache_chunk *chunk = nv_cache->chunks;
	if (!chunk) {
		FTL_ERRLOG(dev, "Cannot save NV cache state, no NV cache metadata\n");
		return -1;
	}

	for (i = 0; i < nv_cache->chunk_count; i++, chunk++) {
		nvc_metadata_validate(nv_cache, chunk->md);

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
chunk_alloc_lba_map(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct ftl_lba_map *lba_map = &chunk->lba_map;

	assert(lba_map->ref_cnt == 0);
	assert(lba_map->chunk_map == NULL);

	lba_map->dma_buf = ftl_mempool_get(nv_cache->lba_pool);

	if (!lba_map->dma_buf) {
		return -1;
	}

	if (ftl_chunk_alloc_md_entry(chunk)) {
		ftl_mempool_put(nv_cache->lba_pool, lba_map->dma_buf);
		lba_map->chunk_map = NULL;
		lba_map->dma_buf = NULL;
		return -1;
	}

	/* Set the P2L to FTL_LBA_INVALID */
	memset(lba_map->dma_buf, -1, FTL_BLOCK_SIZE * chunk_lba_map_num_blocks(nv_cache));

	lba_map->chunk_map = lba_map->dma_buf;

	return 0;
}

typedef void (*ftl_chunk_ops_cb)(struct ftl_nv_cache_chunk *chunk, void *cntx, bool status);

static void
write_brq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_basic_rq *brq = arg;
	struct ftl_nv_cache_chunk *chunk = brq->io.chunk;

	brq->success = success;
	if (spdk_likely(success)) {
		chunk_advance_blocks(chunk->nv_cache, chunk, brq->num_blocks);
	}

	brq->owner.cb(brq);
	spdk_bdev_free_io(bdev_io);
}

static void
_ftl_chunk_basic_rq_write(void *_brq)
{
	struct ftl_basic_rq *brq = _brq;
	struct ftl_nv_cache *nv_cache = brq->io.chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	int rc;

	rc = ftl_nv_cache_bdev_write_blocks_with_md(dev, nv_cache->bdev_desc, nv_cache->cache_ioch,
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
	dev->io_activity_total += brq->num_blocks;
}

static void
chunk_open_cb(int status, void *ctx)
{
	struct ftl_nv_cache_chunk *chunk = (struct ftl_nv_cache_chunk *)ctx;

	if (spdk_unlikely(status)) {
		ftl_md_persist_entry_retry(&chunk->md_persist_entry_ctx);
		return;
	}

	chunk->md->state = FTL_CHUNK_STATE_OPEN;
}

static void
ftl_chunk_open(struct ftl_nv_cache_chunk *chunk)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_lba_map *lba_map = &chunk->lba_map;
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_nvc_md];
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_nvc_md];

	if (chunk_alloc_lba_map(chunk)) {
		assert(0);
		/*
		 * We control number of opening chunk and it shall be consistent with size of chunk
		 * LBA map pool
		 */
		ftl_abort();
		return;
	}

	chunk->nv_cache->chunk_open_count++;

	assert(chunk->md->write_pointer == 0);
	assert(chunk->md->blocks_written == 0);

	memcpy(lba_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
	lba_map->chunk_dma_md->state = FTL_CHUNK_STATE_OPEN;
	lba_map->chunk_dma_md->lba_map_checksum = 0;

	ftl_md_persist_entry(md, get_chunk_idx(chunk), lba_map->chunk_dma_md, ftl_md_get_vss_buffer(md), chunk_open_cb,
			     chunk, &chunk->md_persist_entry_ctx);
}

static void
chunk_close_cb(int status, void *ctx)
{
	struct ftl_nv_cache_chunk *chunk = (struct ftl_nv_cache_chunk *)ctx;

	assert(chunk->md->write_pointer == chunk->nv_cache->chunk_blocks);

	if (spdk_likely(!status)) {
		chunk->md->lba_map_checksum = chunk->lba_map.chunk_dma_md->lba_map_checksum;
		chunk_free_lba_map(chunk);

		assert(chunk->nv_cache->chunk_open_count > 0);
		chunk->nv_cache->chunk_open_count--;

		/* Chunk full move it on full list */
		TAILQ_INSERT_TAIL(&chunk->nv_cache->chunk_full_list, chunk, entry);
		chunk->nv_cache->chunk_full_count++;

		chunk->md->state = FTL_CHUNK_STATE_CLOSED;
	} else {
		ftl_md_persist_entry_retry(&chunk->md_persist_entry_ctx);
	}
}

static void
chunk_map_write_cb(struct ftl_basic_rq *brq)
{
	struct ftl_nv_cache_chunk *chunk = brq->io.chunk;
	struct ftl_lba_map *lba_map = &chunk->lba_map;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_nvc_md];
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_nvc_md];
	uint32_t chunk_map_crc;

	if (spdk_likely(brq->success)) {
		chunk_map_crc = spdk_crc32c_update(lba_map->dma_buf,
				ftl_nv_cache_chunk_tail_md_num_blocks(chunk->nv_cache) * FTL_BLOCK_SIZE, 0);
		memcpy(lba_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
		lba_map->chunk_dma_md->state = FTL_CHUNK_STATE_CLOSED;
		lba_map->chunk_dma_md->lba_map_checksum = chunk_map_crc;
		ftl_md_persist_entry(md, get_chunk_idx(chunk), chunk->lba_map.chunk_dma_md, ftl_md_get_vss_buffer(md),
				     chunk_close_cb, chunk, &chunk->md_persist_entry_ctx);
	} else {
		/* retry */
		chunk->md->write_pointer -= brq->num_blocks;
		ftl_chunk_basic_rq_write(chunk, brq);
	}
}

static void
ftl_chunk_close(struct ftl_nv_cache_chunk *chunk)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_basic_rq *brq = &chunk->metadata_rq;
	void *metadata = chunk->lba_map.dma_buf;
	uint64_t num_blocks = ftl_nv_cache_chunk_tail_md_num_blocks(chunk->nv_cache);

	ftl_basic_rq_init(dev, brq, metadata, num_blocks);
	ftl_basic_rq_set_owner(brq, chunk_map_write_cb, chunk);

	assert(chunk->md->write_pointer == chunk_tail_md_offset(chunk->nv_cache));
	brq->io.addr = chunk->offset + chunk->md->write_pointer;

	ftl_chunk_basic_rq_write(chunk, brq);
}

int
ftl_nv_cache_chunks_busy(struct ftl_nv_cache *nv_cache)
{
	/* chunk_current is migrating to closed status when closing, any others should already be
	 * moved to free chunk list. Also need to wait for free md requests */
	return nv_cache->chunk_open_count == 0 && nv_cache->chunk_free_md_count == 0;
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

		if (chunk->md->state != FTL_CHUNK_STATE_OPEN) {
			break;
		}

		TAILQ_REMOVE(&nv_cache->chunk_open_list, chunk, entry);
		chunk_free_lba_map(chunk);
		memset(chunk->md, 0, sizeof(*chunk->md));
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
