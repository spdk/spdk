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
#include "utils/ftl_addr_utils.h"
#include "mngt/ftl_mngt.h"

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

	return 0;
}

void
ftl_nv_cache_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;

	ftl_mempool_destroy(nv_cache->md_pool);
	ftl_mempool_destroy(nv_cache->lba_pool);
	ftl_mempool_destroy(nv_cache->chunk_md_pool);
	nv_cache->md_pool = NULL;
	nv_cache->lba_pool = NULL;
	nv_cache->chunk_md_pool = NULL;

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
	if (nv_cache->chunk_open_count > 0) {
		return false;
	}

	return true;
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
	 * moved to free chunk list. */
	return nv_cache->chunk_open_count == 0;
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
