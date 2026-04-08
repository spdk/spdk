/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2025 Dell Inc, or its subsidiaries.
 */

/** \file
 * Raid block device abstraction layer
 */

#ifndef SPDK_MODULE_BDEV_RAID_H_
#define SPDK_MODULE_BDEV_RAID_H_

#ifdef __cplusplus
extern "C" {
#endif

enum spdk_bdev_raid_level {
	SPDK_BDEV_RAID_LEVEL_INVALID	= -1,
	SPDK_BDEV_RAID_LEVEL_RAID0	= 0,
	SPDK_BDEV_RAID_LEVEL_RAID1	= 1,
	SPDK_BDEV_RAID_LEVEL_RAID5F	= 95, /* 0x5f */
	SPDK_BDEV_RAID_LEVEL_CONCAT	= 99,
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MODULE_BDEV_RAID_H_ */
