/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_FTL_H
#define SPDK_FTL_H

#include "spdk/stdinc.h"
#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_ftl_dev;

struct spdk_ftl_conf {
	/* Percentage of base device blocks not exposed to the user */
	uint64_t				overprovisioning;

	/* Core mask - core thread plus additional relocation threads */
	char					*core_mask;

	/* IO pool size per user thread */
	size_t					user_io_pool_size;

	/* FTL startup mode mask, see spdk_ftl_mode enum for possible values */
	uint32_t				mode;

	/* Name of base block device (zoned or non-zoned) */
	char					*base_bdev;

	/* Name of cache block device (must support extended metadata) */
	char					*cache_bdev;

	/* Base bdev reclaim unit size */
	uint64_t				base_bdev_reclaim_unit_size;
};

enum spdk_ftl_mode {
	/* Create new device */
	SPDK_FTL_MODE_CREATE = (1 << 0),
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FTL_H */
