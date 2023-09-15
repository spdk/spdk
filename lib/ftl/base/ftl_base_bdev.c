/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_base_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	return spdk_bdev_get_block_size(bdev) == FTL_BLOCK_SIZE;
}

static struct ftl_layout_region *
md_region_create(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
		 uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;
	uint64_t reg_free_offs = 0, reg_current_end, reg_offs, data_base_alignment;
	const char *md_region_name;

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	md_region_name = ftl_md_region_name(reg_type);

	/* As new MD regions are added one after another, find where all existing regions end on the device */
	region = layout->region;
	for (int reg_idx = 0; reg_idx < FTL_LAYOUT_REGION_TYPE_MAX; reg_idx++, region++) {
		if (region->bdev_desc == dev->base_bdev_desc) {
			reg_current_end = region->current.offset + region->current.blocks;
			reg_free_offs = spdk_max(reg_free_offs, reg_current_end);
		}
	}


	data_base_alignment = 8 * ftl_bitmap_buffer_alignment;
	/* Allocating a ftl_bitmap requires a 8B input buffer alignment, since we're reusing the global valid map md buffer
	 * this means that each band starting address needs to be aligned too - each device sector takes 1b in the valid map,
	 * so 64 sectors (8*8) is the needed alignment
	 */
	reg_offs = SPDK_ALIGN_CEIL(reg_free_offs, data_base_alignment);

	region = &layout->region[reg_type];
	region->type = reg_type;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	region->name = md_region_name;
	region->current.version = region->prev.version = reg_version;
	region->current.offset = reg_offs;
	region->current.blocks = ftl_md_region_blocks(dev, entry_count * entry_size);
	region->entry_size = entry_size / FTL_BLOCK_SIZE;
	region->num_entries = entry_count;

	region->bdev_desc = dev->base_bdev_desc;
	region->ioch = dev->base_ioch;
	region->vss_blksz = 0;

	reg_offs += region->current.blocks;
	if (reg_offs > layout->base.total_blocks) {
		return NULL;
	}

	return region;
}

struct ftl_base_device_type base_bdev = {
	.name = "base_bdev",
	.ops = {
		.is_bdev_compatible = is_bdev_compatible,
		.md_layout_ops = {
			.region_create = md_region_create,
		},
	}
};
FTL_BASE_DEVICE_TYPE_REGISTER(base_bdev)
