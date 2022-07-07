/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
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
#include "spdk/bdev_zone.h"

#include "ftl_internal.h"
#include "ftl_io.h"
#include "ftl_layout.h"
#include "ftl_sb.h"
#include "ftl_l2p.h"
#include "utils/ftl_log.h"

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

	/* Queue of registered IO channels */
	TAILQ_HEAD(, ftl_io_channel)	ioch_queue;

	/* Underlying device */
	struct spdk_bdev_desc		*base_bdev_desc;

	/* Cache device */
	struct spdk_bdev_desc		*cache_bdev_desc;

	/* Cache VSS metadata size */
	uint64_t			cache_md_size;

	/* Cached properties of the underlying device */
	uint64_t			num_blocks_in_band;
	uint64_t			num_zones_in_band;
	uint64_t			num_blocks_in_zone;
	bool				is_zoned;

	/* Indicates the device is fully initialized */
	bool				initialized;

	/* Indicates the device is about to be stopped */
	bool				halt;

	/* Indicates if the device is registered as an IO device */
	bool				io_device_registered;

	/* Management process to be continued after IO device unregistration completes */
	struct ftl_mngt_process		*unregister_process;

	/* counters for poller busy, include
	   1. nv cache read/write
	   2. metadata read/write
	   3. base bdev read/write */
	uint64_t			io_activity_total;

	/* Number of operational bands */
	uint64_t			num_bands;

	/* Number of free bands */
	uint64_t			num_free;

	/* Logical -> physical table */
	void				*l2p;

	/* l2p deferred pins list */
	TAILQ_HEAD(, ftl_l2p_pin_ctx) l2p_deferred_pins;

	/* Size of the l2p table */
	uint64_t			num_lbas;

	/* Metadata size */
	uint64_t			md_size;

	/* Transfer unit size */
	uint64_t			xfer_size;

	/* Inflight IO operations */
	uint32_t			num_inflight;

	/* Thread on which the poller is running */
	struct spdk_thread		*core_thread;

	/* IO channel to the FTL device, used for internal management operations
	 * consuming FTL's external API
	 */
	struct spdk_io_channel		*ioch;

	/* Underlying device IO channel */
	struct spdk_io_channel		*base_ioch;

	/* Cache IO channel */
	struct spdk_io_channel		*cache_ioch;

	/* Poller */
	struct spdk_poller		*core_poller;

	/* Read submission queue */
	TAILQ_HEAD(, ftl_io)		rd_sq;

	/* Write submission queue */
	TAILQ_HEAD(, ftl_io)		wr_sq;
};

void ftl_invalidate_addr(struct spdk_ftl_dev *dev, ftl_addr addr);

int ftl_core_poller(void *ctx);

int ftl_io_channel_poll(void *arg);

struct ftl_io_channel *ftl_io_channel_get_ctx(struct spdk_io_channel *ioch);

static inline uint64_t
ftl_get_num_blocks_in_band(const struct spdk_ftl_dev *dev)
{
	return dev->num_blocks_in_band;
}

static inline size_t
ftl_get_num_zones_in_band(const struct spdk_ftl_dev *dev)
{
	return dev->num_zones_in_band;
}

static inline size_t
ftl_get_num_blocks_in_zone(const struct spdk_ftl_dev *dev)
{
	return dev->num_blocks_in_zone;
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
	return ftl_get_num_bands(dev) * ftl_get_num_zones_in_band(dev);
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

#endif /* FTL_CORE_H */
