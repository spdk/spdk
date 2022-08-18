/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev.h"

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_layout.h"
#include "ftl_nv_cache.h"
#include "ftl_sb.h"

#define FTL_NV_CACHE_CHUNK_DATA_SIZE(blocks) ((uint64_t)blocks * FTL_BLOCK_SIZE)
#define FTL_NV_CACHE_CHUNK_SIZE(blocks) \
	(FTL_NV_CACHE_CHUNK_DATA_SIZE(blocks) + (2 * FTL_NV_CACHE_CHUNK_MD_SIZE))

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

static inline uint64_t
blocks_region(uint64_t bytes)
{
	const uint64_t alignment = FTL_LAYOUT_REGION_ALIGNMENT_BYTES;
	uint64_t result;

	result = spdk_divide_round_up(bytes, alignment);
	result *= alignment;
	result /= FTL_BLOCK_SIZE;

	return result;
}

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
	reg->bdev_desc = dev->nv_cache.bdev_desc;
	reg->ioch = dev->nv_cache.cache_ioch;
	reg->vss_blksz = dev->nv_cache.md_size;
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
	uint64_t left, offset = 0;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region, *mirror;

#ifdef SPDK_FTL_VSS_EMU
	/* Skip the already init`d VSS region */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_VSS];
	offset += region->current.blocks;

	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}
#endif

	/* Skip the superblock region. Already init`d in ftl_layout_setup_superblock */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB];
	offset += region->current.blocks;

	/* Initialize L2P region */
	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_L2P];
	region->type = FTL_LAYOUT_REGION_TYPE_L2P;
	region->name = "l2p";
	region->current.version = 0;
	region->prev.version = 0;
	region->current.offset = offset;
	region->current.blocks = blocks_region(layout->l2p.addr_size * dev->num_lbas);
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}

	/*
	 * Initialize NV Cache metadata
	 */
	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}

	left = layout->nvc.total_blocks - offset;
	layout->nvc.chunk_data_blocks =
		FTL_NV_CACHE_CHUNK_DATA_SIZE(ftl_get_num_blocks_in_band(dev)) / FTL_BLOCK_SIZE;
	layout->nvc.chunk_meta_size = FTL_NV_CACHE_CHUNK_MD_SIZE;
	layout->nvc.chunk_count = (left * FTL_BLOCK_SIZE) /
				  FTL_NV_CACHE_CHUNK_SIZE(ftl_get_num_blocks_in_band(dev));
	layout->nvc.chunk_tail_md_num_blocks = ftl_nv_cache_chunk_tail_md_num_blocks(&dev->nv_cache);

	if (0 == layout->nvc.chunk_count) {
		goto error;
	}
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_NVC_MD];
	region->type = FTL_LAYOUT_REGION_TYPE_NVC_MD;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR;
	region->name = "nvc_md";
	region->current.version = region->prev.version = FTL_NVC_VERSION_CURRENT;
	region->current.offset = offset;
	region->current.blocks = blocks_region(layout->nvc.chunk_count *
					       sizeof(struct ftl_nv_cache_chunk_md));
	region->entry_size = sizeof(struct ftl_nv_cache_chunk_md) / FTL_BLOCK_SIZE;
	region->num_entries = layout->nvc.chunk_count;
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	/*
	 * Initialize NV Cache metadata mirror
	 */
	mirror = &layout->region[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR];
	*mirror = *region;
	mirror->type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR;
	mirror->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	mirror->name = "nvc_md_mirror";
	mirror->current.offset += region->current.blocks;
	offset += mirror->current.blocks;

	/*
	 * Initialize data region on NV cache
	 */
	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_DATA_NVC];
	region->type = FTL_LAYOUT_REGION_TYPE_DATA_NVC;
	region->name = "data_nvc";
	region->current.version = region->prev.version = 0;
	region->current.offset = offset;
	region->current.blocks = layout->nvc.chunk_count * layout->nvc.chunk_data_blocks;
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	left = layout->nvc.total_blocks - offset;
	if (left > layout->nvc.chunk_data_blocks) {
		FTL_ERRLOG(dev, "Error when setup NV cache layout\n");
		return -1;
	}

	if (offset > layout->nvc.total_blocks) {
		FTL_ERRLOG(dev, "Error when setup NV cache layout\n");
		goto error;
	}

	return 0;

error:
	FTL_ERRLOG(dev, "Insufficient NV Cache capacity to preserve metadata\n");
	return -1;
}

static ftl_addr
layout_base_offset(struct spdk_ftl_dev *dev)
{
	ftl_addr addr;

	addr = dev->num_bands * ftl_get_num_blocks_in_band(dev);
	return addr;
}

static int
setup_layout_base(struct spdk_ftl_dev *dev)
{
	uint64_t left, offset;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;

	/* Base device layout is following:
	 * - data
	 * - superblock
	 * - valid map
	 *
	 * Superblock has been already configured, its offset marks the end of the data region
	 */
	offset = layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE].current.offset;

	/* Setup data region on base device */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_DATA_BASE];
	region->type = FTL_LAYOUT_REGION_TYPE_DATA_BASE;
	region->name = "data_btm";
	region->current.version = region->prev.version = 0;
	region->current.offset = 0;
	region->current.blocks = offset;
	set_region_bdev_btm(region, dev);

	/* Move offset after base superblock */
	offset += layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE].current.blocks;

	/* Checking for underflow */
	left = layout->base.total_blocks - offset;
	if (left > layout->base.total_blocks) {
		FTL_ERRLOG(dev, "Error when setup base device layout\n");
		return -1;
	}

	if (offset > layout->base.total_blocks) {
		FTL_ERRLOG(dev, "Error when setup base device layout\n");
		return -1;
	}

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

	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	layout->nvc.total_blocks = spdk_bdev_get_num_blocks(bdev);

	/* Initialize mirrors types */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		if (i == FTL_LAYOUT_REGION_TYPE_SB) {
			/* Super block has been already initialized */
			continue;
		}

		layout->region[i].mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	}

	/*
	 * Initialize L2P information
	 */
	num_lbas = get_num_user_lbas(dev);
	if (dev->num_lbas == 0) {
		assert(dev->conf.mode & SPDK_FTL_MODE_CREATE);
		dev->num_lbas = num_lbas;
		dev->sb->lba_cnt = num_lbas;
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

#ifdef SPDK_FTL_VSS_EMU
void
ftl_layout_setup_vss_emu(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_VSS];

	assert(layout->md[FTL_LAYOUT_REGION_TYPE_VSS] == NULL);

	region = &layout->region[FTL_LAYOUT_REGION_TYPE_VSS];
	region->type = FTL_LAYOUT_REGION_TYPE_VSS;
	region->name = "vss";
	region->current.version = region->prev.version = 0;
	region->current.offset = 0;

	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	layout->nvc.total_blocks = spdk_bdev_get_num_blocks(bdev);
	region->current.blocks = blocks_region(dev->nv_cache.md_size * layout->nvc.total_blocks);

	region->vss_blksz = 0;
	region->bdev_desc = dev->nv_cache.bdev_desc;
	region->ioch = dev->nv_cache.cache_ioch;

	assert(region->bdev_desc != NULL);
	assert(region->ioch != NULL);
}
#endif

int
ftl_layout_setup_superblock(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB];
	uint64_t total_blocks, offset, left;

	assert(layout->md[FTL_LAYOUT_REGION_TYPE_SB] == NULL);

	/* Initialize superblock region */
	region->type = FTL_LAYOUT_REGION_TYPE_SB;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_SB_BASE;
	region->name = "sb";
	region->current.version = FTL_METADATA_VERSION_CURRENT;
	region->prev.version = FTL_METADATA_VERSION_CURRENT;
	region->current.offset = 0;

	/*
	 * VSS region must go first in case SB to make calculating its relative size easier
	 */
#ifdef SPDK_FTL_VSS_EMU
	region->current.offset = layout->region[FTL_LAYOUT_REGION_TYPE_VSS].current.offset +
				 layout->region[FTL_LAYOUT_REGION_TYPE_VSS].current.blocks;
#endif

	region->current.blocks = blocks_region(FTL_SUPERBLOCK_SIZE);
	region->vss_blksz = 0;
	region->bdev_desc = dev->nv_cache.bdev_desc;
	region->ioch = dev->nv_cache.cache_ioch;

	assert(region->bdev_desc != NULL);
	assert(region->ioch != NULL);

	region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE];
	region->type = FTL_LAYOUT_REGION_TYPE_SB_BASE;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_MAX;
	region->name = "sb_mirror";
	region->current.version = FTL_METADATA_VERSION_CURRENT;
	region->prev.version = FTL_METADATA_VERSION_CURRENT;
	/* TODO: This should really be at offset 0 - think how best to upgrade between the two layouts
	 * This is an issue if some other metadata appears at block 0 of base device (most likely GPT or blobstore)
	 */
	region->current.offset = layout_base_offset(dev);
	region->current.blocks = blocks_region(FTL_SUPERBLOCK_SIZE);
	set_region_bdev_btm(region, dev);

	/* Check if SB can be stored at the end of base device */
	total_blocks = spdk_bdev_get_num_blocks(
			       spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
	offset = region->current.offset + region->current.blocks;
	left = total_blocks - offset;
	if ((left > total_blocks) || (offset > total_blocks)) {
		FTL_ERRLOG(dev, "Error when setup base device super block\n");
		return -1;
	}

	return 0;
}

void
ftl_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	int i;
	FTL_NOTICELOG(dev, "NV cache layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		if (layout->region[i].bdev_desc == dev->nv_cache.bdev_desc) {
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
