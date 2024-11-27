/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_base_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "ftl_band.h"
#include "utils/ftl_layout_tracker_bdev.h"

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	uint64_t write_unit_size = spdk_bdev_get_write_unit_size(bdev);

	if (spdk_bdev_get_block_size(bdev) != FTL_BLOCK_SIZE) {
		FTL_ERRLOG(dev, "Unsupported block size, only 4096 is supprted.\nn");
		return false;
	}

	if (spdk_bdev_get_md_size(bdev) != 0) {
		/* Bdev's metadata is unsupported */
		FTL_ERRLOG(dev, "Unsupported metadata size, sector metadata isn't supported.\n");
		return false;
	}

	if (write_unit_size == 1) {
		/* No write unit size, all alignments will work out fine */
		return true;
	}

	if (!spdk_u32_is_pow2(write_unit_size) || write_unit_size > (MiB / FTL_BLOCK_SIZE)) {
		/* Needs to be a power of 2 for current band size (1GiB) restrictions.
		 * Current buffers are allocated in 1MiB sizes (256 blocks), so it can't be larger than that.
		 * In the future, if the restriction is relaxed, the ftl_bitmap_buffer_alignment (64 blocks)
		 * will need to be taken into consideration as well.
		 */
		FTL_ERRLOG(dev,
			   "Unsupported write unit size (%lu), must be a power of 2 (in blocks). Can't be larger than 256 (1MiB)\n",
			   write_unit_size);
		return false;
	}

	return true;
}

static void
md_region_setup(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
		struct ftl_layout_region *region)
{
	assert(region);
	region->type = reg_type;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	region->name = ftl_md_region_name(reg_type);

	region->bdev_desc = dev->base_bdev_desc;
	region->ioch = dev->base_ioch;
	region->vss_blksz = 0;
}

static int
md_region_create(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
		 uint32_t reg_version, size_t reg_blks)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_props;
	uint64_t data_base_alignment;

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	reg_blks = ftl_md_region_align_blocks(dev, reg_blks);

	/* Allocating a ftl_bitmap requires a 8B input buffer alignment, since we're reusing the global valid map md buffer
	 * this means that each band starting address needs to be aligned too - each device sector takes 1b in the valid map,
	 * so 64 sectors (8*8) is the needed alignment
	 */
	data_base_alignment = 8 * ftl_bitmap_buffer_alignment;
	reg_props = ftl_layout_tracker_bdev_add_region(dev->base_layout_tracker, reg_type, reg_version,
			reg_blks, data_base_alignment);
	if (!reg_props) {
		return -1;
	}
	assert(reg_props->type == reg_type);
	assert(reg_props->ver == reg_version);
	assert(reg_props->blk_sz == reg_blks);
	assert(reg_props->blk_offs + reg_blks <= dev->layout.base.total_blocks);
	return 0;
}

static int
md_region_open(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type, uint32_t reg_version,
	       size_t entry_size, size_t entry_count, struct ftl_layout_region *region)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;
	uint64_t reg_blks = ftl_md_region_blocks(dev, entry_size * entry_count);

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);

	while (true) {
		ftl_layout_tracker_bdev_find_next_region(dev->base_layout_tracker, reg_type, &reg_search_ctx);
		if (!reg_search_ctx || reg_search_ctx->ver == reg_version) {
			break;
		}
	}

	if (!reg_search_ctx || reg_search_ctx->blk_sz < reg_blks) {
		/* Region not found or insufficient space */
		return -1;
	}

	if (!region) {
		return 0;
	}

	md_region_setup(dev, reg_type, region);

	region->entry_size = entry_size / FTL_BLOCK_SIZE;
	region->num_entries = entry_count;

	region->current.version = reg_version;
	region->current.offset = reg_search_ctx->blk_offs;
	region->current.blocks = reg_search_ctx->blk_sz;

	return 0;
}

struct ftl_base_device_type base_bdev = {
	.name = "base_bdev",
	.ops = {
		.is_bdev_compatible = is_bdev_compatible,

		.md_layout_ops = {
			.region_create = md_region_create,
			.region_open = md_region_open,
		},
	}
};
FTL_BASE_DEVICE_TYPE_REGISTER(base_bdev)
