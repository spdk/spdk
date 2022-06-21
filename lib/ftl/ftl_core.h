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
#include "ftl_layout.h"
#include "mngt/ftl_mngt_zone.h"
#include "utils/ftl_log.h"

struct spdk_ftl_dev;
struct ftl_zone;
struct ftl_io;

struct spdk_ftl_dev {
	/* Device instance */
	struct spdk_uuid			uuid;

	/* Device name */
	char						*name;

	/* Configuration */
	struct spdk_ftl_conf		conf;

	/* FTL device layout */
	struct ftl_layout			layout;

	/* Indicates the device is fully initialized */
	int							initialized;

	/* Indicates the device is about to be stopped */
	int							halt;

	/* Queue of registered IO channels */
	TAILQ_HEAD(, ftl_io_channel) ioch_queue;

	/* Underlying device */
	struct spdk_bdev_desc		*base_bdev_desc;

	/* Cache device */
	struct spdk_bdev_desc		*cache_bdev_desc;

	/* Cache VSS metadata size */
	uint64_t					cache_md_size;

	/* Cached properties of the underlying device */
	uint64_t					num_blocks_in_band;
	size_t						num_punits;
	size_t						num_blocks_in_zone;
	bool						is_zoned;

	/* Underlying device IO channel */
	struct spdk_io_channel		*base_ioch;

	/* counters for poller busy, include
	   1. nv cache read/write
	   2. metadata read/write
	   3. base bdev read/write */
	uint64_t					io_activity_total;

	/* Number of operational bands */
	size_t						num_bands;

	/* Number of free bands */
	size_t						num_free;

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

	/* Cache IO channel */
	struct spdk_io_channel		*cache_ioch;

	/* Poller */
	struct spdk_poller			*core_poller;

	/* Number of IO channels */
	uint64_t					num_io_channels;

	/* Read submission queue */
	TAILQ_HEAD(, ftl_io)		rd_sq;

	/* Write submission queue */
	TAILQ_HEAD(, ftl_io)		wr_sq;
};

int ftl_task_core(void *ctx);

int ftl_io_channel_poll(void *arg);

struct spdk_io_channel *ftl_get_io_channel(const struct spdk_ftl_dev *dev);

struct ftl_io_channel *ftl_io_channel_get_ctx(struct spdk_io_channel *ioch);

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

#endif /* FTL_CORE_H */
