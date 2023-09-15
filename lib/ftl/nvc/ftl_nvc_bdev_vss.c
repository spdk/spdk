/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (!spdk_bdev_is_md_separate(bdev)) {
		/* It doesn't support separate metadata buffer IO */
		return false;
	}

	if (spdk_bdev_get_md_size(bdev) != sizeof(union ftl_md_vss)) {
		/* Bdev's metadata is invalid size */
		return false;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		/* Unsupported DIF type used by bdev */
		return false;
	}

	if (ftl_md_xfer_blocks(dev) * spdk_bdev_get_md_size(bdev) > FTL_ZERO_BUFFER_SIZE) {
		FTL_ERRLOG(dev, "Zero buffer too small for bdev %s metadata transfer\n",
			   spdk_bdev_get_name(bdev));
		return false;
	}

	return true;
}

static struct ftl_layout_region *
md_region_create(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
		 uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;
	uint64_t reg_free_offs = 0, reg_current_end;
	const char *md_region_name;

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	md_region_name = ftl_md_region_name(reg_type);

	/* As new MD regions are added one after another, find where all existing regions end on the device */
	region = layout->region;
	for (int reg_idx = 0; reg_idx < FTL_LAYOUT_REGION_TYPE_MAX; reg_idx++, region++) {
		reg_current_end = region->current.offset + region->current.blocks;
		if (reg_free_offs < reg_current_end) {
			reg_free_offs = reg_current_end;
		}
	}

	region = &layout->region[reg_type];
	region->type = reg_type;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	region->name = md_region_name;
	region->current.version = region->prev.version = reg_version;
	region->current.offset = reg_free_offs;
	region->current.blocks = ftl_md_region_blocks(dev, entry_count * entry_size);
	region->entry_size = entry_size / FTL_BLOCK_SIZE;
	region->num_entries = entry_count;

	region->bdev_desc = dev->nv_cache.bdev_desc;
	region->ioch = dev->nv_cache.cache_ioch;
	region->vss_blksz = dev->nv_cache.md_size;

	reg_free_offs += region->current.blocks;
	if (reg_free_offs > layout->nvc.total_blocks) {
		return NULL;
	}

	return region;
}

struct ftl_nv_cache_device_desc nvc_bdev_vss = {
	.name = "bdev",
	.features = {
	},
	.ops = {
		.is_bdev_compatible = is_bdev_compatible,
		.md_layout_ops = {
			.region_create = md_region_create,
		},
	}
};
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_vss)
