/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "ftl_nvc_bdev_common.h"

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (spdk_bdev_get_md_size(bdev) != 0) {
		/* Bdev's metadata is invalid size */
		return false;
	}

	return true;
}

static int
setup_layout(struct spdk_ftl_dev *dev)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;
	const uint64_t blocks = ftl_p2l_log_get_md_blocks_required(dev, 1, ftl_get_num_blocks_in_band(dev));
	enum ftl_layout_region_type region_type;

	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX;
	     region_type++) {
		if (md_ops->region_create(dev, region_type, FTL_P2L_LOG_VERSION_CURRENT, blocks)) {
			return -1;
		}

		if (md_ops->region_open(dev, region_type, FTL_P2L_LOG_VERSION_CURRENT,
					FTL_BLOCK_SIZE, blocks,
					&dev->layout.region[region_type])) {
			return -1;
		}
	}

	return 0;
}

struct ftl_nv_cache_device_type nvc_bdev_non_vss = {
	.name = "bdev-non-vss",
	.features = {
	},
	.ops = {
		.is_bdev_compatible = is_bdev_compatible,
		.is_chunk_active = ftl_nvc_bdev_common_is_chunk_active,
		.setup_layout = setup_layout,
		.md_layout_ops = {
			.region_create = ftl_nvc_bdev_common_region_create,
			.region_open = ftl_nvc_bdev_common_region_open,
		}
	}
};
