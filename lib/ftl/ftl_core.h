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

	/* Indicates the device is fully initialized */
	int							initialized;

	/* Indicates the device is about to be stopped */
	int							halt;

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

	/* Read submission queue */
	TAILQ_HEAD(, ftl_io)		rd_sq;

	/* Write submission queue */
	TAILQ_HEAD(, ftl_io)		wr_sq;
};

#endif /* FTL_CORE_H */
