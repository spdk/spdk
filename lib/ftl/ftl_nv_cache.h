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

#ifndef FTL_NV_CACHE_H
#define FTL_NV_CACHE_H

#include "spdk/stdinc.h"
#include "spdk/crc32.h"

#include "ftl_io.h"
#include "ftl_utils.h"

/*
 * FTL non volatile cache is divided into groups of blocks called chunks.
 * Size of each chunk is multiple of xfer size plus additional metadata.
 * For each block associated lba is stored in metadata. Cache space is
 * written chunk by chunk sequentially. When number of free chunks reaches
 * some threshold oldest chunks are moved from cache to backend storage to
 * create space for new user data.
 */
#define FTL_NV_CACHE_NUM_COMPACTORS 8

/*
 * Parameters controlling nv cache write throttling.
 *
 * The write throttle limit value is calculated as follows:
 * limit = compaction_average_bw * (1.0 + modifier)
 *
 * The modifier depends on the number of free chunks vs the configured threshold. Its value is
 * zero if the number of free chunks is at the threshold, negative if below and positive if above.
 */

/* Interval in miliseconds between write throttle updates. */
#define FTL_NV_CACHE_THROTTLE_INTERVAL_MILIS	20
/* Throttle modifier proportional gain */
#define FTL_NV_CACHE_THROTTLE_MODIFIER_KP	20
/* Min and max modifier values */
#define FTL_NV_CACHE_THROTTLE_MODIFIER_MIN	-0.8
#define FTL_NV_CACHE_THROTTLE_MODIFIER_MAX	0.5

#define FTL_NVC_VERSION_0	0
#define FTL_NVC_VERSION_1	1

#define FTL_NVC_VERSION_CURRENT FTL_NVC_VERSION_1

/*
 * FTL non volatile cache is divided into groups of blocks called chunks.
 * Size of each chunk is multiple of xfer size plus additional metadata.
 * For each block associated lba is stored in metadata. Cache space is
 * written chunk by chunk sequentially. When number of free chunks reaches
 * some threshold oldest chunks are moved from cache to backend storage to
 * create space for new user data.
 */
#define FTL_NV_CACHE_NUM_COMPACTORS 8

struct spdk_ftl_dev;
struct ftl_mngt;

struct ftl_nvcache_restore;
typedef void (*ftl_nv_cache_restore_fn)(struct ftl_nvcache_restore *, int, void *cb_arg);

enum ftl_chunk_state {
	FTL_CHUNK_STATE_FREE,
	FTL_CHUNK_STATE_OPEN,
	FTL_CHUNK_STATE_CLOSED,
	FTL_CHUNK_STATE_MAX
};

struct ftl_nv_cache_chunk_md {
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

	/* CRC32 checksum of the associated LBA map when chunk is in closed state */
	uint32_t lba_map_checksum;
} __attribute__((aligned(FTL_BLOCK_SIZE)));

#define FTL_NV_CACHE_CHUNK_META_SIZE sizeof(struct ftl_nv_cache_chunk_md)
SPDK_STATIC_ASSERT(FTL_NV_CACHE_CHUNK_META_SIZE == FTL_BLOCK_SIZE,
		   "FTL NV Chunk metadata size is invalid");

struct ftl_nv_cache_chunk {
	struct ftl_nv_cache *nv_cache;

	struct ftl_nv_cache_chunk_md *md;

	/* Offset from start lba of the cache */
	uint64_t offset;

	/* LBA map */
	struct ftl_lba_map lba_map;

	/* Metadata request */
	struct ftl_basic_rq metadata_rq;

	TAILQ_ENTRY(ftl_nv_cache_chunk) entry;

	/* This flag is used to indicate chunk is used in recovery */
	bool recovery;

	/* Compaction start time */
	uint64_t compaction_start_tsc;

	/* Compaction duration */
	uint64_t compaction_length_tsc;

	/* For writing metadata */
	struct ftl_md_io_entry_ctx md_persist_entry_ctx;
};

struct ftl_nv_cache_compaction {
	struct ftl_nv_cache *nv_cache;
	struct ftl_rq *wr;
	struct ftl_rq *rd;
	TAILQ_ENTRY(ftl_nv_cache_compaction) entry;
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

	/* LBA map memory pool */
	struct ftl_mempool *lba_pool;

	/* Chunk md memory pool */
	struct ftl_mempool *chunk_md_pool;

	/* Chunk md memory pool for freeing chunks */
	struct ftl_mempool *free_chunk_md_pool;

	/* Block Metadata size */
	uint64_t md_size;

	/* NV cache metadata object handle */
	struct ftl_md *md;

	/* Number of blocks in chunk */
	uint64_t chunk_blocks;

	/* Number of chunks */
	uint64_t chunk_count;

	/* Current processed chunk */
	struct ftl_nv_cache_chunk *chunk_current;

	/* Number of currently open chunks */
	uint64_t chunk_open_count;

	/* Free chunks list */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_free_list;
	uint64_t chunk_free_count;

	/* Open chunks list */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_open_list;

	/* Full chunks list */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_full_list;
	uint64_t chunk_full_count;

	/* Chunks being compacted */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_comp_list;
	uint64_t chunk_comp_count;

	/* Chunks being freed */
	TAILQ_HEAD(, ftl_nv_cache_chunk) chunk_free_md_list;
	uint64_t chunk_free_md_count;

	TAILQ_HEAD(, ftl_nv_cache_compaction) compaction_list;
	uint64_t compaction_active_count;
	uint64_t chunk_compaction_threshold;

	struct ftl_nv_cache_chunk *chunks;

	uint64_t last_seq_id;
};

int ftl_nv_cache_init(struct spdk_ftl_dev *dev);
void ftl_nv_cache_deinit(struct spdk_ftl_dev *dev);
bool ftl_nv_cache_write(struct ftl_io *io);
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

int ftl_nv_cache_load_state(struct ftl_nv_cache *nv_cache);

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
/**
 * @brief Iterates over NV caches chunks and returns the max open and closed sequence id
 *
 * @param nv_cache FLT NV cache
 * @param[out] open_seq_id Max detected open sequence id
 * @param[out] close_seq_id Max detected clos sequence id
 */
void ftl_nv_cache_get_max_seq_id(struct ftl_nv_cache *nv_cache, uint64_t *open_seq_id,
				 uint64_t *close_seq_id);

typedef int (*ftl_chunk_md_cb)(struct ftl_nv_cache_chunk *chunk, void *cntx);

struct ftl_nv_cache_chunk *ftl_nv_cache_get_chunk_from_addr(struct spdk_ftl_dev *dev,
		ftl_addr addr);

#endif  /* FTL_NV_CACHE_H */
