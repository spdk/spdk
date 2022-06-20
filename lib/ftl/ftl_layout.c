/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev.h"

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_layout.h"

static inline float
blocks2mib(uint64_t blocks)
{
	float result;

	result = blocks;
	result *= FTL_BLOCK_SIZE;
	result /= 1024UL;
	result /= 1024UL;

	return result;
}
/* TODO: This should be aligned to the write unit size of the device a given piece of md is on.
 * The tricky part is to make sure interpreting old alignment values will still be valid...
 */
#define FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS 32ULL
#define FTL_LAYOUT_REGION_ALIGNMENT_BYTES (FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS * FTL_BLOCK_SIZE)

static void
dump_region(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	assert(!(region->current.offset % FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS));
	assert(!(region->current.blocks % FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS));

	FTL_NOTICELOG(dev, "Region %s\n", region->name);
	FTL_NOTICELOG(dev, "	offset:                      %.2f MiB\n",
		      blocks2mib(region->current.offset));
	FTL_NOTICELOG(dev, "	blocks:                      %.2f MiB\n",
		      blocks2mib(region->current.blocks));
}

int
ftl_validate_regions(struct spdk_ftl_dev *dev, struct ftl_layout *layout)
{
	uint64_t i, j;

	/* Validate if regions doesn't overlap each other  */
	/* TODO: major upgrades: keep track of and validate free_nvc/free_btm regions */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++) {
		struct ftl_layout_region *r1 = &layout->region[i];

		for (j = 0; j < FTL_LAYOUT_REGION_TYPE_MAX; j++) {
			struct ftl_layout_region *r2 = &layout->region[j];

			if (r1->bdev_desc != r2->bdev_desc) {
				continue;
			}

			if (i == j) {
				continue;
			}

			uint64_t r1_begin = r1->current.offset;
			uint64_t r1_end = r1->current.offset + r1->current.blocks - 1;
			uint64_t r2_begin = r2->current.offset;
			uint64_t r2_end = r2->current.offset + r2->current.blocks - 1;

			if (spdk_max(r1_begin, r2_begin) <= spdk_min(r1_end, r2_end)) {
				FTL_ERRLOG(dev, "Layout initialization ERROR, two regions overlap, "
					   "%s and %s\n", r1->name, r2->name);
				return -1;
			}
		}
	}

	return 0;
}

static uint64_t
get_num_user_lbas(struct spdk_ftl_dev *dev)
{
	uint64_t blocks = 0;

	blocks = dev->layout.base.total_blocks;
	blocks = (blocks * (100 - dev->conf.overprovisioning)) / 100;

	return blocks;
}

static void
set_region_bdev_nvc(struct ftl_layout_region *reg, struct spdk_ftl_dev *dev)
{
	reg->bdev_desc = dev->cache_bdev_desc;
	reg->ioch = dev->cache_ioch;
	reg->vss_blksz = dev->cache_md_size;
}

static void
set_region_bdev_btm(struct ftl_layout_region *reg, struct spdk_ftl_dev *dev)
{
	reg->bdev_desc = dev->base_bdev_desc;
	reg->ioch = dev->base_ioch;
	reg->vss_blksz = 0;
}

static int
setup_layout_nvc(struct spdk_ftl_dev *dev)
{
	uint64_t offset = 0;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;

	region = &layout->region[FTL_LAYOUT_REGION_TYPE_DATA_NVC];
	region->type = FTL_LAYOUT_REGION_TYPE_DATA_NVC;
	region->name = "data_nvc";
	region->current.version = region->prev.version = 0;
	region->current.offset = offset;
	region->current.blocks = layout->nvc.total_blocks - offset;
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	if (offset > layout->nvc.total_blocks) {
		FTL_ERRLOG(dev, "Error when setup NV cache layout\n");
		goto error;
	}

	return 0;

error:
	FTL_ERRLOG(dev, "Insufficient NV Cache capacity to preserve metadata\n");
	return -1;
}

static int
setup_layout_base(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;

	/* Setup data region on base device */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_DATA_BASE];
	region->type = FTL_LAYOUT_REGION_TYPE_DATA_BASE;
	region->name = "data_btm";
	region->current.version = region->prev.version = 0;
	region->current.offset = 0;
	region->current.blocks = layout->base.total_blocks;
	set_region_bdev_btm(region, dev);

	return 0;
}

int
ftl_layout_setup(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	struct ftl_layout *layout = &dev->layout;
	uint64_t i;
	uint64_t num_lbas;

	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	layout->base.total_blocks = spdk_bdev_get_num_blocks(bdev);

	bdev = spdk_bdev_desc_get_bdev(dev->cache_bdev_desc);
	layout->nvc.total_blocks = spdk_bdev_get_num_blocks(bdev);

	/* Initialize mirrors types */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		layout->region[i].mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	}

	/*
	 * Initialize L2P information
	 */
	num_lbas = get_num_user_lbas(dev);
	if (dev->num_lbas == 0) {
		assert(dev->conf.mode & SPDK_FTL_MODE_CREATE);
		dev->num_lbas = num_lbas;
	} else if (dev->num_lbas != num_lbas) {
		FTL_ERRLOG(dev, "Mismatched FTL num_lbas\n");
		return -EINVAL;
	}
	layout->l2p.addr_length = spdk_u64log2(layout->base.total_blocks + layout->nvc.total_blocks) + 1;
	layout->l2p.addr_size = layout->l2p.addr_length > 32 ? 8 : 4;
	layout->l2p.lbas_in_page = FTL_BLOCK_SIZE / layout->l2p.addr_size;

	if (setup_layout_nvc(dev)) {
		return -EINVAL;
	}

	if (setup_layout_base(dev)) {
		return -EINVAL;
	}

	if (ftl_validate_regions(dev, layout)) {
		return -EINVAL;
	}

	FTL_NOTICELOG(dev, "Base device capacity:         %.2f MiB\n",
		      blocks2mib(layout->base.total_blocks));
	FTL_NOTICELOG(dev, "NV cache device capacity:       %.2f MiB\n",
		      blocks2mib(layout->nvc.total_blocks));
	FTL_NOTICELOG(dev, "L2P entries:                    %"PRIu64"\n",
		      dev->num_lbas);
	FTL_NOTICELOG(dev, "L2P address size:               %"PRIu64"\n",
		      layout->l2p.addr_size);

	return 0;
}

void
ftl_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	int i;
	FTL_NOTICELOG(dev, "NV cache layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		if (layout->region[i].bdev_desc == dev->cache_bdev_desc) {
			dump_region(dev, &layout->region[i]);
		}
	}
	FTL_NOTICELOG(dev, "Bottom device layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		if (layout->region[i].bdev_desc == dev->base_bdev_desc) {
			dump_region(dev, &layout->region[i]);
		}
	}
}
