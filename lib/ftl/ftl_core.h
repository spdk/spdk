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
#include "utils/ftl_log.h"

struct spdk_ftl_dev {
	/* Device instance */
	struct spdk_uuid		uuid;

	/* Device name */
	char				*name;

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

	/* counters for poller busy, include
	   1. nv cache read/write
	   2. metadata read/write
	   3. base bdev read/write */
	uint64_t			io_activity_total;

	/* Number of operational bands */
	uint64_t			num_bands;

	/* Number of free bands */
	uint64_t			num_free;

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

	/* Read submission queue */
	TAILQ_HEAD(, ftl_io)		rd_sq;

	/* Write submission queue */
	TAILQ_HEAD(, ftl_io)		wr_sq;
};

#endif /* FTL_CORE_H */
