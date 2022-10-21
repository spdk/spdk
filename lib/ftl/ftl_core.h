/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
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

#include "ftl_internal.h"
#include "ftl_io.h"
#include "ftl_trace.h"
#include "ftl_nv_cache.h"
#include "ftl_writer.h"
#include "ftl_layout.h"
#include "ftl_sb.h"
#include "ftl_l2p.h"
#include "utils/ftl_bitmap.h"
#include "utils/ftl_log.h"

/*
 * We need to reserve at least 2 buffers for band close / open sequence
 * alone, plus additional (8) buffers for handling relocations.
 */
#define P2L_MEMPOOL_SIZE (2 + 8)

/* When using VSS on nvcache, FTL sometimes doesn't require the contents of metadata.
 * Some devices have bugs when sending a NULL pointer as part of metadata when namespace
 * is formatted with VSS. This buffer is passed to such calls to avoid the bug. */
#define FTL_ZERO_BUFFER_SIZE 0x100000
extern void *g_ftl_write_buf;
extern void *g_ftl_read_buf;

struct spdk_ftl_dev {
	/* Configuration */
	struct spdk_ftl_conf		conf;

	/* FTL device layout */
	struct ftl_layout		layout;

	/* FTL superblock */
	struct ftl_superblock		*sb;

	/* FTL shm superblock */
	struct ftl_superblock_shm	*sb_shm;
	struct ftl_md			*sb_shm_md;

	/* Queue of registered IO channels */
	TAILQ_HEAD(, ftl_io_channel)	ioch_queue;

	/* Underlying device */
	struct spdk_bdev_desc		*base_bdev_desc;

	/* Cached properties of the underlying device */
	uint64_t			num_blocks_in_band;
	bool				is_zoned;

	/* Indicates the device is fully initialized */
	bool				initialized;

	/* Indicates the device is about to be stopped */
	bool				halt;

	/* Indicates if the device is registered as an IO device */
	bool				io_device_registered;

	/* Management process to be continued after IO device unregistration completes */
	struct ftl_mngt_process		*unregister_process;

	/* Non-volatile write buffer cache */
	struct ftl_nv_cache		nv_cache;

	/* P2L map memory pool */
	struct ftl_mempool		*p2l_pool;

	/* Underlying SHM buf for P2L map mempool */
	struct ftl_md			*p2l_pool_md;

	/* Band md memory pool */
	struct ftl_mempool		*band_md_pool;

	/* Traces */
	struct ftl_trace		trace;

	/* Statistics */
	struct ftl_stats		stats;

	/* Array of bands */
	struct ftl_band			*bands;

	/* Number of operational bands */
	uint64_t			num_bands;

	/* Next write band */
	struct ftl_band			*next_band;

	/* Free band list */
	TAILQ_HEAD(, ftl_band)		free_bands;

	/* Closed bands list */
	TAILQ_HEAD(, ftl_band)		shut_bands;

	/* Number of free bands */
	uint64_t			num_free;

	/* Logical -> physical table */
	void				*l2p;

	/* l2p deferred pins list */
	TAILQ_HEAD(, ftl_l2p_pin_ctx)	l2p_deferred_pins;

	/* Size of the l2p table */
	uint64_t			num_lbas;

	/* P2L valid map */
	struct ftl_bitmap		*valid_map;

	/* Metadata size */
	uint64_t			md_size;

	/* Transfer unit size */
	uint64_t			xfer_size;

	/* Current user write limit */
	int				limit;

	/* Inflight IO operations */
	uint32_t			num_inflight;

	/* Manages data relocation */
	struct ftl_reloc		*reloc;

	/* Thread on which the poller is running */
	struct spdk_thread		*core_thread;

	/* IO channel to the FTL device, used for internal management operations
	 * consuming FTL's external API
	 */
	struct spdk_io_channel		*ioch;

	/* Underlying device IO channel */
	struct spdk_io_channel		*base_ioch;

	/* Poller */
	struct spdk_poller		*core_poller;

	/* Read submission queue */
	TAILQ_HEAD(, ftl_io)		rd_sq;

	/* Write submission queue */
	TAILQ_HEAD(, ftl_io)		wr_sq;

	/* Trim submission queue */
	TAILQ_HEAD(, ftl_io)		unmap_sq;

	/* Trim valid map */
	struct ftl_bitmap		*unmap_map;
	struct ftl_md			*unmap_map_md;
	size_t				unmap_qd;
	bool				unmap_in_progress;

	/* Writer for user IOs */
	struct ftl_writer		writer_user;

	/* Writer for GC IOs */
	struct ftl_writer		writer_gc;

	uint32_t			num_logical_bands_in_physical;

	/* Retry init sequence */
	bool				init_retry;

	/* P2L checkpointing */
	struct {
		/* Free regions */
		TAILQ_HEAD(, ftl_p2l_ckpt)	free;
		/* In use regions */
		TAILQ_HEAD(, ftl_p2l_ckpt)	inuse;
	} p2l_ckpt;
};

void ftl_apply_limits(struct spdk_ftl_dev *dev);

void ftl_invalidate_addr(struct spdk_ftl_dev *dev, ftl_addr addr);

int ftl_core_poller(void *ctx);

int ftl_io_channel_poll(void *arg);

struct ftl_io_channel *ftl_io_channel_get_ctx(struct spdk_io_channel *ioch);

bool ftl_needs_reloc(struct spdk_ftl_dev *dev);

struct ftl_band *ftl_band_get_next_free(struct spdk_ftl_dev *dev);

void ftl_set_unmap_map(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks,
		       uint64_t seq_id);

void ftl_recover_max_seq(struct spdk_ftl_dev *dev);

void ftl_stats_bdev_io_completed(struct spdk_ftl_dev *dev, enum ftl_stats_type type,
				 struct spdk_bdev_io *bdev_io);

void ftl_stats_crc_error(struct spdk_ftl_dev *dev, enum ftl_stats_type type);

int ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	      uint64_t lba, size_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg);

static inline uint64_t
ftl_get_num_blocks_in_band(const struct spdk_ftl_dev *dev)
{
	return dev->num_blocks_in_band;
}

static inline uint32_t
ftl_get_write_unit_size(struct spdk_bdev *bdev)
{
	/* Full block of P2L map worth of xfer_sz is needed for P2L checkpointing */
	return FTL_NUM_LBA_IN_BLOCK;
}

static inline struct spdk_thread *
ftl_get_core_thread(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread;
}

static inline void
ftl_add_io_activity(struct spdk_ftl_dev *dev)
{
	dev->stats.io_activity_total++;
}

static inline uint64_t
ftl_get_num_bands(const struct spdk_ftl_dev *dev)
{
	return dev->num_bands;
}

static inline bool
ftl_check_core_thread(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread == spdk_get_thread();
}

static inline int
ftl_addr_packed(const struct spdk_ftl_dev *dev)
{
	return dev->layout.l2p.addr_size < sizeof(ftl_addr);
}

static inline int
ftl_addr_in_nvc(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	assert(addr != FTL_ADDR_INVALID);
	return addr >= dev->layout.base.total_blocks;
}

static inline uint64_t
ftl_addr_to_nvc_offset(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	assert(ftl_addr_in_nvc(dev, addr));
	return addr - dev->layout.base.total_blocks;
}

static inline ftl_addr
ftl_addr_from_nvc_offset(const struct spdk_ftl_dev *dev, uint64_t cache_offset)
{
	return cache_offset + dev->layout.base.total_blocks;
}

static inline uint64_t
ftl_get_next_seq_id(struct spdk_ftl_dev *dev)
{
	return ++dev->sb->seq_id;
}

static inline size_t
ftl_p2l_map_num_blocks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_get_num_blocks_in_band(dev) *
				    sizeof(struct ftl_p2l_map_entry), FTL_BLOCK_SIZE);
}

static inline size_t
ftl_tail_md_num_blocks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(
		       ftl_p2l_map_num_blocks(dev),
		       dev->xfer_size) * dev->xfer_size;
}

/*
 * shm_ready being set is a necessary part of the validity of the shm superblock
 * If it's not set, then the recovery or startup must proceed from disk
 *
 * - If both sb and shm_sb are clean, then shm memory can be relied on for startup
 * - If shm_sb wasn't set to clean, then disk startup/recovery needs to be done (which depends on the sb->clean flag)
 * - sb->clean clear and sb_shm->clean is technically not possible (due to the order of these operations), but it should
 * probably do a full recovery from disk to be on the safe side (which the ftl_fast_recovery will guarantee)
 */

static inline bool
ftl_fast_startup(const struct spdk_ftl_dev *dev)
{
	return dev->sb->clean && dev->sb_shm->shm_clean && dev->sb_shm->shm_ready;
}

static inline bool
ftl_fast_recovery(const struct spdk_ftl_dev *dev)
{
	return !dev->sb->clean && !dev->sb_shm->shm_clean && dev->sb_shm->shm_ready;
}

#endif /* FTL_CORE_H */
