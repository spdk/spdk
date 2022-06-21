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

#ifndef FTL_CORE_H
#define FTL_CORE_H

#include "spdk/stdinc.h"
#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/ftl.h"
#include "spdk/bdev.h"
#include "spdk/bdev_zone.h"

#include "ftl_internal.h"
#include "ftl_io.h"
#include "ftl_nv_cache.h"
#include "ftl_layout.h"
#include "ftl_sb.h"
#include "ftl_l2p.h"
#include "mngt/ftl_mngt_zone.h"
#include "utils/ftl_log.h"

#ifdef SPDK_CONFIG_PMDK
#include "libpmem.h"
#endif /* SPDK_CONFIG_PMDK */

struct spdk_ftl_dev;
struct ftl_band;
struct ftl_zone;
struct ftl_io;

/*
 * We need to reserve at least 2 buffers for band close / open sequence
 * alone, plus additional (8) buffers for handling relocations.
 *
 */
#define LBA_MEMPOOL_SIZE (2 + 8)

/* When using VSS on nvcache, FTL sometimes doesn't require the contents of metadata.
 * Some devices have bugs when sending a NULL pointer as part of metadata when namespace
 * is formatted with VSS. This buffer is passed to such calls to avoid the bug. */
#define FTL_ZERO_BUFFER_SIZE 0x100000
extern void *g_ftl_zero_buf;
extern void *g_ftl_tmp_buf;

struct spdk_ftl_dev {
	/* Device instance */
	struct spdk_uuid			uuid;

	/* Device name */
	char						*name;

	/* Configuration */
	struct spdk_ftl_conf		conf;

	/* FTL device layout */
	struct ftl_layout			layout;

	/* FTL superblock */
	struct ftl_superblock		*sb;

	/* Indicates the device is fully initialized */
	int							initialized;

	/* Indicates the device is about to be stopped */
	int							halt;

	/* Queue of registered IO channels */
	TAILQ_HEAD(, ftl_io_channel) ioch_queue;

	/* Underlying device */
	struct spdk_bdev_desc		*base_bdev_desc;

	/* Cached properties of the underlying device */
	uint64_t					num_blocks_in_band;
	size_t						num_punits;
	size_t						num_blocks_in_zone;
	bool						is_zoned;

	/* Underlying device IO channel */
	struct spdk_io_channel		*base_ioch;

	/* Non-volatile write buffer cache */
	struct ftl_nv_cache			nv_cache;

	/* Media management events pool */
	struct spdk_mempool			*media_events_pool;

	/* counters for poller busy, include
	   1. nv cache read/write
	   2. metadata read/write
	   3. base bdev read/write */
	uint64_t					io_activity_total;

	/* Array of bands */
	struct ftl_band				*bands;

	/* Number of operational bands */
	size_t						num_bands;

	/* Next write band */
	struct ftl_band				*next_band;

	/* Free band list */
	TAILQ_HEAD(, ftl_band)		free_bands;

	/* Closed bands list */
	TAILQ_HEAD(, ftl_band)		shut_bands;

	/* Number of free bands */
	size_t						num_free;

	/* Logical -> physical table */
	void						*l2p;

	/* l2p deferred pins list */
	TAILQ_HEAD(, ftl_l2p_pin_ctx) l2p_deferred_pins;

	/* Size of the l2p table */
	uint64_t					num_lbas;

	/* L2P checkpointing */
	void						*l2p_ckpt;

	/* Metadata size */
	size_t						md_size;

	/* Transfer unit size */
	size_t						xfer_size;

	/* Inflight IO operations */
	uint32_t					num_inflight;

	/* Thread on which the poller is running */
	struct spdk_thread			*core_thread;

	/* IO channel */
	struct spdk_io_channel		*ioch;

	/* Poller */
	struct spdk_poller			*core_poller;

	/* Number of IO channels */
	uint64_t					num_io_channels;

	/* Read submission queue */
	TAILQ_HEAD(, ftl_io)		rd_sq;

	/* Write submission queue */
	TAILQ_HEAD(, ftl_io)		wr_sq;
};

struct ftl_media_event {
	/* Owner */
	struct spdk_ftl_dev				*dev;
	/* Media event */
	struct spdk_bdev_media_event	event;
};

void ftl_invalidate_addr(struct spdk_ftl_dev *dev, ftl_addr addr);

int ftl_task_core(void *ctx);

void ftl_get_media_events(struct spdk_ftl_dev *dev);

int ftl_io_channel_poll(void *arg);

struct spdk_io_channel *ftl_get_io_channel(const struct spdk_ftl_dev *dev);

struct ftl_io_channel *ftl_io_channel_get_ctx(struct spdk_io_channel *ioch);

static inline uint64_t
ftl_get_num_blocks_in_band(const struct spdk_ftl_dev *dev)
{
	return dev->num_blocks_in_band;
}

static inline size_t
ftl_get_num_punits(const struct spdk_ftl_dev *dev)
{
	return dev->num_punits;
}

static inline size_t
ftl_get_num_blocks_in_zone(const struct spdk_ftl_dev *dev)
{
	return dev->num_blocks_in_zone;
}

static inline size_t
ftl_is_zoned(const struct spdk_ftl_dev *dev)
{
	return dev->is_zoned;
}

static inline uint64_t
ftl_addr_get_band(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	return addr / ftl_get_num_blocks_in_band(dev);
}

static inline uint64_t
ftl_addr_get_punit(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	if (ftl_is_zoned(dev)) {
		return (addr / ftl_get_num_blocks_in_zone(dev)) % ftl_get_num_punits(dev);
	}

	return 0;
}

static inline uint64_t
ftl_addr_get_zone_offset(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	if (ftl_is_zoned(dev)) {
		return addr % ftl_get_num_blocks_in_zone(dev);
	}

	return addr & (ftl_get_num_blocks_in_zone(dev) - 1);
}

static inline uint32_t
ftl_get_write_unit_size(struct spdk_bdev *bdev)
{
	if (spdk_bdev_is_zoned(bdev)) {
		return spdk_bdev_get_write_unit_size(bdev);
	}

	/* TODO: this should be passed via input parameter */
	return 32;
}

static inline struct spdk_thread *
ftl_get_core_thread(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread;
}

static inline size_t
ftl_get_num_bands(const struct spdk_ftl_dev *dev)
{
	return dev->num_bands;
}

static inline size_t
ftl_get_num_zones(const struct spdk_ftl_dev *dev)
{
	return ftl_get_num_bands(dev) * ftl_get_num_punits(dev);
}

static inline int
ftl_addr_packed(const struct spdk_ftl_dev *dev)
{
	return dev->layout.l2p.addr_size < sizeof(ftl_addr);
}

static inline int
ftl_addr_cached(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	assert(addr != FTL_ADDR_INVALID);
	return addr >= dev->layout.btm.total_blocks;
}

static inline uint64_t
ftl_addr_get_cache_offset(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	assert(ftl_addr_cached(dev, addr));
	return addr - dev->layout.btm.total_blocks;
}

static inline ftl_addr
ftl_addr_to_cached(const struct spdk_ftl_dev *dev, uint64_t cache_offset)
{
	return cache_offset + dev->layout.btm.total_blocks;
}

static inline bool
ftl_check_core_thread(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread == spdk_get_thread();
}

static inline bool
ftl_is_append_supported(const struct spdk_ftl_dev *dev)
{
	return dev->conf.use_append;
}

static inline size_t
ftl_lba_map_num_blocks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_get_num_blocks_in_band(dev) * sizeof(uint64_t),
				    FTL_BLOCK_SIZE);
}

static inline size_t
ftl_tail_md_num_blocks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(
		       ftl_lba_map_num_blocks(dev),
		       dev->xfer_size) * dev->xfer_size;
}

#endif /* FTL_CORE_H */
