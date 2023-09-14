/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (spdk_bdev_get_md_size(bdev) != 0) {
		/* Bdev's metadata is invalid size */
		return false;
	}

	return true;
}

struct ftl_nv_cache_device_type nvc_bdev_non_vss = {
	.name = "bdev-non-vss",
	.features = {
	},
	.ops = {
		.is_bdev_compatible = is_bdev_compatible
	}
};
