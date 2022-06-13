/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_NV_CACHE_H
#define FTL_NV_CACHE_H

#include "spdk/stdinc.h"
#include "spdk/crc32.h"

#include "ftl_io.h"
#include "ftl_utils.h"

#define FTL_NVC_VERSION_0	0
#define FTL_NVC_VERSION_1	1

#define FTL_NVC_VERSION_CURRENT FTL_NVC_VERSION_1

struct ftl_nvcache_restore;
typedef void (*ftl_nv_cache_restore_fn)(struct ftl_nvcache_restore *, int, void *cb_arg);

enum ftl_chunk_state {
	FTL_CHUNK_STATE_FREE,
	FTL_CHUNK_STATE_OPEN,
	FTL_CHUNK_STATE_CLOSED,
	FTL_CHUNK_STATE_MAX
};

struct ftl_nv_cache_chunk_md {
	/* Current lba to write */
	uint32_t write_pointer;

	/* Number of blocks written */
	uint32_t blocks_written;

	/* Number of skipped block (case when IO size is greater than blocks left in chunk) */
	uint32_t blocks_skipped;

	/* Next block to be compacted */
	uint32_t read_pointer;

	/* Number of compacted (both valid and invalid) blocks */
	uint32_t blocks_compacted;

	/* Chunk state */
	enum ftl_chunk_state state;

	/* CRC32 checksum of the associated P2L map when chunk is in closed state */
	uint32_t p2l_map_checksum;
} __attribute__((aligned(FTL_BLOCK_SIZE)));

#define FTL_NV_CACHE_CHUNK_MD_SIZE sizeof(struct ftl_nv_cache_chunk_md)
SPDK_STATIC_ASSERT(FTL_NV_CACHE_CHUNK_MD_SIZE == FTL_BLOCK_SIZE,
		   "FTL NV Chunk metadata size is invalid");

struct ftl_nv_cache_chunk {
	struct ftl_nv_cache *nv_cache;

	struct ftl_nv_cache_chunk_md *md;

	/* Offset from start lba of the cache */
	uint64_t offset;

	/* P2L map */
	struct ftl_p2l_map p2l_map;

	/* Metadata request */
	struct ftl_basic_rq metadata_rq;

	TAILQ_ENTRY(ftl_nv_cache_chunk) entry;

	/* This flag is used to indicate chunk is used in recovery */
	bool recovery;

	/* For writing metadata */
	struct ftl_md_io_entry_ctx md_persist_entry_ctx;
};

struct ftl_nv_cache {
	/* Flag indicating halt request */
	bool halt;

	/* Write buffer cache bdev */
	struct spdk_bdev_desc *bdev_desc;

	/* Persistent cache IO channel */
	struct spdk_io_channel *cache_ioch;

	/* Metadata pool */
	struct ftl_mempool *md_pool;

	/* P2L map memory pool */
	struct ftl_mempool *p2l_pool;

	/* Chunk md memory pool */
	struct ftl_mempool *chunk_md_pool;

	/* Block Metadata size */
	uint64_t md_size;

	/* NV cache metadata object handle */
	struct ftl_md *md;

	/* Number of blocks in chunk */
	uint64_t chunk_blocks;

	/* Number of blocks in tail md per chunk */
	uint64_t tail_md_chunk_blocks;

	/* Number of chunks */
	uint64_t chunk_count;

	/* Current processed chunk */
	struct ftl_nv_cache_chunk *chunk_current;

	/* Free chunks list */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_free_list;
	uint64_t chunk_free_count;

	/* Open chunks list */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_open_list;
	uint64_t chunk_open_count;

	/* Full chunks list */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_full_list;
	uint64_t chunk_full_count;

	struct ftl_nv_cache_chunk *chunks;
};

int ftl_nv_cache_init(struct spdk_ftl_dev *dev);
void ftl_nv_cache_deinit(struct spdk_ftl_dev *dev);
void ftl_nv_cache_fill_md(struct ftl_io *io);
int ftl_nv_cache_read(struct ftl_io *io, ftl_addr addr, uint32_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg);
bool ftl_nv_cache_full(struct ftl_nv_cache *nv_cache);
void ftl_nv_cache_process(struct spdk_ftl_dev *dev);

void ftl_chunk_map_set_lba(struct ftl_nv_cache_chunk *chunk,
			   uint64_t offset, uint64_t lba);
uint64_t ftl_chunk_map_get_lba(struct ftl_nv_cache_chunk *chunk, uint64_t offset);

void ftl_nv_cache_set_addr(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr);

int ftl_nv_cache_save_state(struct ftl_nv_cache *nv_cache);

void ftl_nv_cache_halt(struct ftl_nv_cache *nv_cache);

int ftl_nv_cache_chunks_busy(struct ftl_nv_cache *nv_cache);

static inline void
ftl_nv_cache_resume(struct ftl_nv_cache *nv_cache)
{
	nv_cache->halt = false;
}

bool ftl_nv_cache_is_halted(struct ftl_nv_cache *nv_cache);

size_t ftl_nv_cache_chunk_tail_md_num_blocks(const struct ftl_nv_cache *nv_cache);

uint64_t chunk_tail_md_offset(struct ftl_nv_cache *nv_cache);

typedef int (*ftl_chunk_md_cb)(struct ftl_nv_cache_chunk *chunk, void *cntx);

struct ftl_nv_cache_chunk *ftl_nv_cache_get_chunk_from_addr(struct spdk_ftl_dev *dev,
		ftl_addr addr);

#endif  /* FTL_NV_CACHE_H */
