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

enum spdk_bdev_raid_state {
	/* raid bdev is ready and is seen by upper layers */
	SPDK_BDEV_RAID_STATE_ONLINE = 0,

	/*
	 * raid bdev is configuring, not all underlying bdevs are present.
	 * And can't be seen by upper layers.
	 */
	SPDK_BDEV_RAID_STATE_CONFIGURING,

	/*
	 * In offline state, raid bdev layer will complete all incoming commands without
	 * submitting to underlying base nvme bdevs
	 */
	SPDK_BDEV_RAID_STATE_OFFLINE,

	/* raid bdev state max, new states should be added before this */
	SPDK_BDEV_RAID_STATE_MAX,
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MODULE_BDEV_RAID_H_ */
