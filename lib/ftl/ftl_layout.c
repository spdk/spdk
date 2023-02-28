/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev.h"

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_band.h"
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

static uint64_t
superblock_region_size(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	uint64_t wus = spdk_bdev_get_write_unit_size(bdev) * FTL_BLOCK_SIZE;

	if (wus > FTL_SUPERBLOCK_SIZE) {
		return wus;
	} else {
		return wus * spdk_divide_round_up(FTL_SUPERBLOCK_SIZE, wus);
	}
}

static uint64_t
superblock_region_blocks(struct spdk_ftl_dev *dev)
{
	return superblock_region_size(dev) / FTL_BLOCK_SIZE;
}

static inline uint64_t
blocks_region(struct spdk_ftl_dev *dev, uint64_t bytes)
{
	const uint64_t alignment = superblock_region_size(dev);
	uint64_t result;

	result = spdk_divide_round_up(bytes, alignment);
	result *= alignment;
	result /= FTL_BLOCK_SIZE;

	return result;
}

static void
dump_region(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	assert(!(region->current.offset % superblock_region_blocks(dev)));
	assert(!(region->current.blocks % superblock_region_blocks(dev)));

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
	uint64_t blocks;

	blocks = dev->num_bands * ftl_get_num_blocks_in_band(dev);
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
	int region_type;
	uint64_t left, offset = 0, l2p_blocks;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region, *mirror;
	static const char *p2l_region_name[] = {
		"p2l0",
		"p2l1",
		"p2l2",
		"p2l3"
	};

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
	region->current.blocks = blocks_region(dev, layout->l2p.addr_size * dev->num_lbas);
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	/* Initialize band info metadata */
	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	region->type = FTL_LAYOUT_REGION_TYPE_BAND_MD;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR;
	region->name = "band_md";
	region->current.version = region->prev.version = FTL_BAND_VERSION_CURRENT;
	region->current.offset = offset;
	region->current.blocks = blocks_region(dev, ftl_get_num_bands(dev) * sizeof(struct ftl_band_md));
	region->entry_size = sizeof(struct ftl_band_md) / FTL_BLOCK_SIZE;
	region->num_entries = ftl_get_num_bands(dev);
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	/* Initialize band info metadata mirror */
	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}
	mirror = &layout->region[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR];
	*mirror = *region;
	mirror->type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR;
	mirror->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	mirror->name = "band_md_mirror";
	mirror->current.offset += region->current.blocks;
	offset += mirror->current.blocks;

	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}

	/*
	 * Initialize P2L checkpointing regions
	 */
	SPDK_STATIC_ASSERT(SPDK_COUNTOF(p2l_region_name) == FTL_LAYOUT_REGION_TYPE_P2L_COUNT,
			   "Incorrect # of P2L region names");
	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		if (offset >= layout->nvc.total_blocks) {
			goto error;
		}
		region = &layout->region[region_type];
		region->type = region_type;
		region->name = p2l_region_name[region_type - FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN];
		region->current.version = FTL_P2L_VERSION_CURRENT;
		region->prev.version = FTL_P2L_VERSION_CURRENT;
		region->current.offset = offset;
		region->current.blocks = blocks_region(dev, layout->p2l.ckpt_pages * FTL_BLOCK_SIZE);
		region->entry_size = 1;
		region->num_entries = region->current.blocks;
		set_region_bdev_nvc(region, dev);
		offset += region->current.blocks;
	}

	/*
	 * Initialize trim metadata region
	 */
	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}
	l2p_blocks = layout->region[FTL_LAYOUT_REGION_TYPE_L2P].current.blocks;
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
	region->type = FTL_LAYOUT_REGION_TYPE_TRIM_MD;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR;
	region->name = "trim_md";
	region->current.version = 0;
	region->prev.version = 0;
	region->current.offset = offset;
	region->current.blocks = blocks_region(dev, l2p_blocks * sizeof(uint64_t));
	region->entry_size = 1;
	region->num_entries = region->current.blocks;
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	/* Initialize trim metadata mirror region */
	if (offset >= layout->nvc.total_blocks) {
		goto error;
	}
	mirror = &layout->region[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR];
	*mirror = *region;
	mirror->type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR;
	mirror->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	mirror->name = "trim_md_mirror";
	mirror->current.offset += region->current.blocks;
	offset += mirror->current.blocks;

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
	region->current.blocks = blocks_region(dev, layout->nvc.chunk_count *
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
	uint64_t data_base_alignment = 8 * ftl_bitmap_buffer_alignment;
	/* Allocating a ftl_bitmap requires a 8B input buffer alignment, since we're reusing the global valid map md buffer
	 * this means that each band starting address needs to be aligned too - each device sector takes 1b in the valid map,
	 * so 64 sectors (8*8) is the needed alignment
	 */

	layout->base.num_usable_blocks = ftl_get_num_blocks_in_band(dev);
	layout->base.user_blocks = ftl_band_user_blocks(dev->bands);

	/* Base device layout is following:
	 * - superblock
	 * - data
	 * - valid map
	 */
	offset = layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE].current.blocks;
	offset = SPDK_ALIGN_CEIL(offset, data_base_alignment);

	/* Setup data region on base device */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_DATA_BASE];
	region->type = FTL_LAYOUT_REGION_TYPE_DATA_BASE;
	region->name = "data_btm";
	region->current.version = region->prev.version = 0;
	region->current.offset = offset;
	region->current.blocks = layout_base_offset(dev);
	set_region_bdev_btm(region, dev);

	offset += region->current.blocks;

	/* Setup validity map */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_VALID_MAP];
	region->type = FTL_LAYOUT_REGION_TYPE_VALID_MAP;
	region->name = "vmap";
	region->current.version = region->prev.version = 0;
	region->current.offset = offset;
	region->current.blocks = blocks_region(dev, spdk_divide_round_up(
			layout->base.total_blocks + layout->nvc.total_blocks, 8));
	set_region_bdev_btm(region, dev);
	offset += region->current.blocks;

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

	/* Setup P2L ckpt */
	layout->p2l.ckpt_pages = spdk_divide_round_up(ftl_get_num_blocks_in_band(dev), dev->xfer_size);

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
	FTL_NOTICELOG(dev, "L2P entries:                    %"PRIu64"\n", dev->num_lbas);
	FTL_NOTICELOG(dev, "L2P address size:               %"PRIu64"\n", layout->l2p.addr_size);
	FTL_NOTICELOG(dev, "P2L checkpoint pages:           %"PRIu64"\n", layout->p2l.ckpt_pages);

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
	region->current.blocks = blocks_region(dev, dev->nv_cache.md_size * layout->nvc.total_blocks);

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
	region->current.version = FTL_SB_VERSION_CURRENT;
	region->prev.version = FTL_SB_VERSION_CURRENT;
	region->current.offset = 0;

	/*
	 * VSS region must go first in case SB to make calculating its relative size easier
	 */
#ifdef SPDK_FTL_VSS_EMU
	region->current.offset = layout->region[FTL_LAYOUT_REGION_TYPE_VSS].current.offset +
				 layout->region[FTL_LAYOUT_REGION_TYPE_VSS].current.blocks;
#endif

	region->current.blocks = superblock_region_blocks(dev);
	region->vss_blksz = 0;
	region->bdev_desc = dev->nv_cache.bdev_desc;
	region->ioch = dev->nv_cache.cache_ioch;

	assert(region->bdev_desc != NULL);
	assert(region->ioch != NULL);

	region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE];
	region->type = FTL_LAYOUT_REGION_TYPE_SB_BASE;
	region->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
	region->name = "sb_mirror";
	region->current.version = FTL_SB_VERSION_CURRENT;
	region->prev.version = FTL_SB_VERSION_CURRENT;
	region->current.offset = 0;
	region->current.blocks = superblock_region_blocks(dev);
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
	FTL_NOTICELOG(dev, "Base device layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		if (layout->region[i].bdev_desc == dev->base_bdev_desc) {
			dump_region(dev, &layout->region[i]);
		}
	}
}

uint64_t
ftl_layout_base_md_blocks(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	uint64_t md_blocks = 0, total_blocks = 0;

	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	total_blocks += spdk_bdev_get_num_blocks(bdev);

	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	total_blocks += spdk_bdev_get_num_blocks(bdev);

	/* Count space needed for validity map */
	md_blocks += blocks_region(dev, spdk_divide_round_up(total_blocks, 8));

	/* Count space needed for superblock */
	md_blocks += superblock_region_blocks(dev);
	return md_blocks;
}
