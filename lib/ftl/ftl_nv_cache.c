/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

static inline uint64_t nvc_data_blocks(struct ftl_nv_cache *nv_cache) __attribute__((unused));

static inline const struct ftl_layout_region *
nvc_data_region(
	struct ftl_nv_cache *nv_cache)
{
	struct spdk_ftl_dev *dev;

	dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	return &dev->layout.region[FTL_LAYOUT_REGION_TYPE_DATA_NVC];
}

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
	return nvc_data_region(nv_cache)->current.offset;
}

static inline uint64_t
nvc_data_blocks(struct ftl_nv_cache *nv_cache)
{
	return nvc_data_region(nv_cache)->current.blocks;
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

int
ftl_nv_cache_init(struct spdk_ftl_dev *dev)
{
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_nv_cache_chunk *chunk;
	struct ftl_nv_cache_chunk_md *md;
	uint64_t i, offset;

	nv_cache->halt = true;

	nv_cache->md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];
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

	/* First chunk metadata */
	md = ftl_md_get_buffer(nv_cache->md);
	if (!md) {
		FTL_ERRLOG(dev, "No NV cache metadata\n");
		return -1;
	}

	nv_cache->chunk_free_count = nv_cache->chunk_count;

	chunk = nv_cache->chunks;
	offset = nvc_data_offset(nv_cache);
	for (i = 0; i < nv_cache->chunk_count; i++, chunk++, md++) {
		chunk->nv_cache = nv_cache;
		chunk->md = md;
		nvc_validate_md(nv_cache, md);
		chunk->offset = offset;
		offset += nv_cache->chunk_blocks;
		TAILQ_INSERT_TAIL(&nv_cache->chunk_free_list, chunk, entry);
	}
	assert(offset <= nvc_data_offset(nv_cache) + nvc_data_blocks(nv_cache));

#define FTL_MAX_OPEN_CHUNKS 2
	nv_cache->p2l_pool = ftl_mempool_create(FTL_MAX_OPEN_CHUNKS,
						nv_cache_p2l_map_pool_elem_size(nv_cache),
						FTL_BLOCK_SIZE,
						SPDK_ENV_SOCKET_ID_ANY);
	if (!nv_cache->p2l_pool) {
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
	ftl_mempool_destroy(nv_cache->p2l_pool);
	ftl_mempool_destroy(nv_cache->chunk_md_pool);
	nv_cache->md_pool = NULL;
	nv_cache->p2l_pool = NULL;
	nv_cache->chunk_md_pool = NULL;

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

static int
ftl_chunk_alloc_md_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_nv_cache *nv_cache = chunk->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;
	struct ftl_layout_region *region = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_NVC_MD];

	p2l_map->chunk_dma_md = ftl_mempool_get(nv_cache->chunk_md_pool);

	if (!p2l_map->chunk_dma_md) {
		return -ENOMEM;
	}

	memset(p2l_map->chunk_dma_md, 0, region->entry_size * FTL_BLOCK_SIZE);
	return 0;
}

static void
ftl_chunk_free_md_entry(struct ftl_nv_cache_chunk *chunk)
{
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;

	ftl_mempool_put(chunk->nv_cache->chunk_md_pool, p2l_map->chunk_dma_md);
	p2l_map->chunk_dma_md = NULL;
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
		ftl_l2p_update_cache(dev, ftl_io_get_lba(io, i), next_addr, io->map[i]);
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
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	int rc;

	rc = ftl_nv_cache_bdev_writev_blocks_with_md(dev,
			nv_cache->bdev_desc, nv_cache->cache_ioch,
			io->iov, io->iov_cnt, io->md,
			ftl_addr_to_nvc_offset(dev, io->addr), io->num_blocks,
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
	io->addr = ftl_addr_from_nvc_offset(dev, cache_offset);
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

	assert(ftl_addr_in_nvc(io->dev, addr));

	rc = ftl_nv_cache_bdev_read_blocks_with_md(io->dev, nv_cache->bdev_desc, nv_cache->cache_ioch,
			ftl_io_iovec_addr(io), NULL, ftl_addr_to_nvc_offset(io->dev, addr),
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

static void
ftl_chunk_set_addr(struct ftl_nv_cache_chunk *chunk, uint64_t lba, ftl_addr addr)
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

	ftl_chunk_set_addr(chunk, lba, addr);
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
		ftl_chunk_open(chunk);
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

	chunk = nv_cache->chunks;
	if (!chunk) {
		FTL_ERRLOG(dev, "Cannot save NV cache state, no NV cache metadata\n");
		return -ENOMEM;
	}

	for (i = 0; i < nv_cache->chunk_count; i++, chunk++) {
		nvc_validate_md(nv_cache, chunk->md);

		if (chunk->md->blocks_written == nv_cache->chunk_blocks) {
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

	spdk_bdev_free_io(bdev_io);
	brq->owner.cb(brq);
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
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;
	struct ftl_layout_region *region = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_NVC_MD];
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

	memcpy(p2l_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
	p2l_map->chunk_dma_md->state = FTL_CHUNK_STATE_OPEN;
	p2l_map->chunk_dma_md->p2l_map_checksum = 0;

	ftl_md_persist_entry(md, get_chunk_idx(chunk), p2l_map->chunk_dma_md,
			     NULL, chunk_open_cb, chunk,
			     &chunk->md_persist_entry_ctx);
}

static void
chunk_close_cb(int status, void *ctx)
{
	struct ftl_nv_cache_chunk *chunk = (struct ftl_nv_cache_chunk *)ctx;

	assert(chunk->md->write_pointer == chunk->nv_cache->chunk_blocks);

	if (spdk_likely(!status)) {
		chunk->md->p2l_map_checksum = chunk->p2l_map.chunk_dma_md->p2l_map_checksum;
		chunk_free_p2l_map(chunk);

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
	struct ftl_p2l_map *p2l_map = &chunk->p2l_map;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(chunk->nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_layout_region *region = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_NVC_MD];
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_NVC_MD];
	uint32_t chunk_map_crc;

	if (spdk_likely(brq->success)) {
		chunk_map_crc = spdk_crc32c_update(p2l_map->chunk_map,
						   chunk->nv_cache->tail_md_chunk_blocks * FTL_BLOCK_SIZE, 0);
		memcpy(p2l_map->chunk_dma_md, chunk->md, region->entry_size * FTL_BLOCK_SIZE);
		p2l_map->chunk_dma_md->state = FTL_CHUNK_STATE_CLOSED;
		p2l_map->chunk_dma_md->p2l_map_checksum = chunk_map_crc;
		ftl_md_persist_entry(md, get_chunk_idx(chunk), chunk->p2l_map.chunk_dma_md,
				     NULL, chunk_close_cb, chunk,
				     &chunk->md_persist_entry_ctx);
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
	void *metadata = chunk->p2l_map.chunk_map;

	ftl_basic_rq_init(dev, brq, metadata, chunk->nv_cache->tail_md_chunk_blocks);
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

		/* Chunks are moved between lists on metadata update submission, but state is changed
		 * on completion. Breaking early in such a case to make sure all the necessary resources
		 * will be freed (during next pass(es) of ftl_nv_cache_halt).
		 */
		if (chunk->md->state != FTL_CHUNK_STATE_OPEN) {
			break;
		}

		TAILQ_REMOVE(&nv_cache->chunk_open_list, chunk, entry);
		chunk_free_p2l_map(chunk);
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
