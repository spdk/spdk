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
	return ftl_nv_cache_chunk_tail_md_num_blocks(nv_cache) * FTL_BLOCK_SIZE;
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
ftl_nv_cache_process(struct spdk_ftl_dev *dev)
{
	if (!dev->nv_cache.bdev_desc) {
		return;
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
	nv_cache->halt = true;
}
