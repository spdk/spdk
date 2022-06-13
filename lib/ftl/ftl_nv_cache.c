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

void ftl_nv_cache_process(struct spdk_ftl_dev *dev)
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
