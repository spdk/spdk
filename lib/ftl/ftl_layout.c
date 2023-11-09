/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
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
#include "nvc/ftl_nvc_dev.h"
#include "utils/ftl_layout_tracker_bdev.h"

enum ftl_layout_setup_mode {
	FTL_LAYOUT_SETUP_MODE_LOAD_CURRENT = 0,
	FTL_LAYOUT_SETUP_MODE_NO_RESTRICT,
	FTL_LAYOUT_SETUP_MODE_LEGACY_DEFAULT,
};

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

uint64_t
ftl_md_region_blocks(struct spdk_ftl_dev *dev, uint64_t bytes)
{
	const uint64_t alignment = superblock_region_size(dev);
	uint64_t result;

	result = spdk_divide_round_up(bytes, alignment);
	result *= alignment;
	result /= FTL_BLOCK_SIZE;

	return result;
}

uint64_t
ftl_md_region_align_blocks(struct spdk_ftl_dev *dev, uint64_t blocks)
{
	const uint64_t alignment = superblock_region_blocks(dev);
	uint64_t result;

	result = spdk_divide_round_up(blocks, alignment);
	result *= alignment;

	return result;
}

const char *
ftl_md_region_name(enum ftl_layout_region_type reg_type)
{
	static const char *md_region_name[FTL_LAYOUT_REGION_TYPE_MAX] = {
		[FTL_LAYOUT_REGION_TYPE_SB] = "sb",
		[FTL_LAYOUT_REGION_TYPE_SB_BASE] = "sb_mirror",
		[FTL_LAYOUT_REGION_TYPE_L2P] = "l2p",
		[FTL_LAYOUT_REGION_TYPE_BAND_MD] = "band_md",
		[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR] = "band_md_mirror",
		[FTL_LAYOUT_REGION_TYPE_VALID_MAP] = "vmap",
		[FTL_LAYOUT_REGION_TYPE_NVC_MD] = "nvc_md",
		[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR] = "nvc_md_mirror",
		[FTL_LAYOUT_REGION_TYPE_DATA_NVC] = "data_nvc",
		[FTL_LAYOUT_REGION_TYPE_DATA_BASE] = "data_btm",
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC] = "p2l0",
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT] = "p2l1",
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP] = "p2l2",
		[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT] = "p2l3",
		[FTL_LAYOUT_REGION_TYPE_TRIM_MD] = "trim_md",
		[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR] = "trim_md_mirror",
	};
	const char *reg_name = md_region_name[reg_type];

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	assert(reg_name != NULL);
	return reg_name;
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
	enum ftl_layout_region_type i, j;

	/* Validate if regions doesn't overlap each other  */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++) {
		struct ftl_layout_region *r1 = ftl_layout_region_get(dev, i);

		if (!r1) {
			continue;
		}

		for (j = 0; j < FTL_LAYOUT_REGION_TYPE_MAX; j++) {
			struct ftl_layout_region *r2 = ftl_layout_region_get(dev, j);

			if (!r2) {
				continue;
			}

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

static uint64_t
layout_blocks_left(struct spdk_ftl_dev *dev, struct ftl_layout_tracker_bdev *layout_tracker)
{
	uint64_t max_reg_size = 0;
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

	while (true) {
		ftl_layout_tracker_bdev_find_next_region(layout_tracker, FTL_LAYOUT_REGION_TYPE_FREE,
				&reg_search_ctx);
		if (!reg_search_ctx) {
			break;
		}
		max_reg_size = spdk_max(max_reg_size, reg_search_ctx->blk_sz);
	}
	return max_reg_size;
}

struct ftl_layout_region *
ftl_layout_region_get(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type)
{
	struct ftl_layout_region *reg = &dev->layout.region[reg_type];

	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	return reg->type == reg_type ? reg : NULL;
}

uint64_t
ftl_layout_base_offset(struct spdk_ftl_dev *dev)
{
	return dev->num_bands * ftl_get_num_blocks_in_band(dev);
}

static int
layout_region_create_nvc(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			 uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_desc->ops.md_layout_ops;
	size_t reg_blks = ftl_md_region_blocks(dev, entry_count * entry_size);

	if (md_ops->region_create(dev, reg_type, reg_version, reg_blks)) {
		return -1;
	}
	if (md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count,
				&dev->layout.region[reg_type])) {
		return -1;
	}
	return 0;
}

static int
layout_region_create_base(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			  uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	const struct ftl_md_layout_ops *md_ops = &dev->base_type->ops.md_layout_ops;
	size_t reg_blks = ftl_md_region_blocks(dev, entry_count * entry_size);

	if (md_ops->region_create(dev, reg_type, reg_version, reg_blks)) {
		return -1;
	}
	if (md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count,
				&dev->layout.region[reg_type])) {
		return -1;
	}
	return 0;
}

static void
legacy_layout_verify_region(struct ftl_layout_tracker_bdev *layout_tracker,
			    enum ftl_layout_region_type reg_type, uint32_t reg_version)
{
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;
	const struct ftl_layout_tracker_bdev_region_props *reg_found = NULL;

	while (true) {
		ftl_layout_tracker_bdev_find_next_region(layout_tracker, reg_type, &reg_search_ctx);
		if (!reg_search_ctx) {
			break;
		}

		/* Only a single region version is present in upgrade from the legacy layout */
		ftl_bug(reg_search_ctx->ver != reg_version);
		ftl_bug(reg_found != NULL);

		reg_found = reg_search_ctx;
	}
}

static int
legacy_layout_region_open_nvc(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			      uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	struct ftl_layout_region *reg = &dev->layout.region[reg_type];
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_desc->ops.md_layout_ops;

	legacy_layout_verify_region(dev->nvc_layout_tracker, reg_type, reg_version);
	return md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count, reg);
}

static int
legacy_layout_region_open_base(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			       uint32_t reg_version, size_t entry_size, size_t entry_count)
{
	struct ftl_layout_region *reg = &dev->layout.region[reg_type];
	const struct ftl_md_layout_ops *md_ops = &dev->base_type->ops.md_layout_ops;

	legacy_layout_verify_region(dev->nvc_layout_tracker, reg_type, reg_version);
	return md_ops->region_open(dev, reg_type, reg_version, entry_size, entry_count, reg);
}

static int
layout_setup_legacy_default_nvc(struct spdk_ftl_dev *dev)
{
	int region_type;
	uint64_t blocks, chunk_count;
	struct ftl_layout *layout = &dev->layout;
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

	/* Initialize L2P region */
	blocks = ftl_md_region_blocks(dev, layout->l2p.addr_size * dev->num_lbas);
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_L2P, 0, FTL_BLOCK_SIZE,
					  blocks)) {
		goto error;
	}

	/* Initialize band info metadata */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD, FTL_BAND_VERSION_1,
					  sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}

	/* Initialize band info metadata mirror */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR, FTL_BAND_VERSION_1,
					  sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_BAND_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR;

	/*
	 * Initialize P2L checkpointing regions
	 */
	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

		/* Get legacy number of blocks */
		ftl_layout_tracker_bdev_find_next_region(dev->nvc_layout_tracker, region_type, &reg_search_ctx);
		if (!reg_search_ctx || reg_search_ctx->ver != FTL_P2L_VERSION_1) {
			goto error;
		}
		blocks = reg_search_ctx->blk_sz;

		if (legacy_layout_region_open_nvc(dev, region_type, FTL_P2L_VERSION_1, FTL_BLOCK_SIZE, blocks)) {
			goto error;
		}
	}

	/*
	 * Initialize trim metadata region
	 */
	blocks = layout->region[FTL_LAYOUT_REGION_TYPE_L2P].current.blocks;
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD, 0, sizeof(uint64_t),
					  blocks)) {
		goto error;
	}

	/* Initialize trim metadata mirror region */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR, 0, sizeof(uint64_t),
					  blocks)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_TRIM_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR;

	/* Restore chunk count */
	ftl_layout_tracker_bdev_find_next_region(dev->nvc_layout_tracker, FTL_LAYOUT_REGION_TYPE_DATA_NVC,
			&reg_search_ctx);
	if (!reg_search_ctx || reg_search_ctx->ver != 0) {
		goto error;
	}
	blocks = reg_search_ctx->blk_sz;
	chunk_count = blocks / ftl_get_num_blocks_in_band(dev);
	if (0 == chunk_count) {
		goto error;
	}

	/*
	 * Initialize NV Cache metadata
	 */
	if (0 == chunk_count) {
		goto error;
	}
	layout->nvc.chunk_count = chunk_count;

	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD, FTL_NVC_VERSION_1,
					  sizeof(struct ftl_nv_cache_chunk_md), chunk_count)) {
		goto error;
	}

	/*
	 * Initialize NV Cache metadata mirror
	 */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR, FTL_NVC_VERSION_1,
					  sizeof(struct ftl_nv_cache_chunk_md), chunk_count)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_NVC_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR;

	/*
	 * Initialize data region on NV cache
	 */
	if (legacy_layout_region_open_nvc(dev, FTL_LAYOUT_REGION_TYPE_DATA_NVC, 0,
					  layout->nvc.chunk_data_blocks * FTL_BLOCK_SIZE, chunk_count)) {
		goto error;
	}

	return 0;

error:
	FTL_ERRLOG(dev, "Invalid legacy NV Cache metadata layout\n");
	return -1;
}

static int
layout_setup_legacy_default_base(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;

	/* Base device layout is as follows:
	 * - superblock
	 * - data
	 * - valid map
	 */
	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_DATA_BASE, 0, FTL_BLOCK_SIZE,
				      ftl_layout_base_offset(dev))) {
		return -1;
	}

	if (legacy_layout_region_open_base(dev, FTL_LAYOUT_REGION_TYPE_VALID_MAP, 0, FTL_BLOCK_SIZE,
					   ftl_md_region_blocks(dev, spdk_divide_round_up(layout->base.total_blocks + layout->nvc.total_blocks,
							   8)))) {
		return -1;
	}

	return 0;
}

static int
layout_setup_legacy_default(struct spdk_ftl_dev *dev)
{
	if (layout_setup_legacy_default_nvc(dev) || layout_setup_legacy_default_base(dev)) {
		return -1;
	}
	return 0;
}

static int
layout_setup_default_nvc(struct spdk_ftl_dev *dev)
{
	int region_type;
	uint64_t left, l2p_blocks;
	struct ftl_layout *layout = &dev->layout;

	/* Initialize L2P region */
	l2p_blocks = ftl_md_region_blocks(dev, layout->l2p.addr_size * dev->num_lbas);
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_L2P, 0, FTL_BLOCK_SIZE, l2p_blocks)) {
		goto error;
	}

	/* Initialize band info metadata */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD, FTL_BAND_VERSION_CURRENT,
				     sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}

	/* Initialize band info metadata mirror */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR, FTL_BAND_VERSION_CURRENT,
				     sizeof(struct ftl_band_md), ftl_get_num_bands(dev))) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_BAND_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR;

	/*
	 * Initialize P2L checkpointing regions
	 */
	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		if (layout_region_create_nvc(dev, region_type, FTL_P2L_VERSION_CURRENT, FTL_BLOCK_SIZE,
					     layout->p2l.ckpt_pages)) {
			goto error;
		}
	}

	/*
	 * Initialize trim metadata region
	 */
	l2p_blocks = layout->region[FTL_LAYOUT_REGION_TYPE_L2P].current.blocks;
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD, 0, sizeof(uint64_t),
				     l2p_blocks)) {
		goto error;
	}

	/* Initialize trim metadata mirror region */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR, 0, sizeof(uint64_t),
				     l2p_blocks)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_TRIM_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR;

	/*
	 * Initialize NV Cache metadata
	 */
	left = layout_blocks_left(dev, dev->nvc_layout_tracker);
	layout->nvc.chunk_count = (left * FTL_BLOCK_SIZE) /
				  FTL_NV_CACHE_CHUNK_SIZE(ftl_get_num_blocks_in_band(dev));
	if (0 == layout->nvc.chunk_count) {
		goto error;
	}
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD, FTL_NVC_VERSION_CURRENT,
				     sizeof(struct ftl_nv_cache_chunk_md), layout->nvc.chunk_count)) {
		goto error;
	}

	/*
	 * Initialize NV Cache metadata mirror
	 */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR, FTL_NVC_VERSION_CURRENT,
				     sizeof(struct ftl_nv_cache_chunk_md), layout->nvc.chunk_count)) {
		goto error;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_NVC_MD].mirror_type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR;

	/*
	 * Initialize data region on NV cache
	 */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_DATA_NVC, 0,
				     layout->nvc.chunk_data_blocks * FTL_BLOCK_SIZE, layout->nvc.chunk_count)) {
		goto error;
	}

	return 0;

error:
	FTL_ERRLOG(dev, "Insufficient NV Cache capacity to preserve metadata\n");
	return -1;
}

static int
layout_setup_default_base(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	uint64_t valid_map_size;

	/* Base device layout is as follows:
	 * - superblock
	 * - data
	 * - valid map
	 */
	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_DATA_BASE, 0, FTL_BLOCK_SIZE,
				      ftl_layout_base_offset(dev))) {
		return -1;
	}

	valid_map_size = spdk_divide_round_up(layout->base.total_blocks + layout->nvc.total_blocks, 8);
	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_VALID_MAP, 0, FTL_BLOCK_SIZE,
				      ftl_md_region_blocks(dev, valid_map_size))) {
		return -1;
	}

	return 0;
}

static int
layout_setup_default(struct spdk_ftl_dev *dev)
{
	if (layout_setup_default_nvc(dev) || layout_setup_default_base(dev)) {
		return -1;
	}
	return 0;
}

static int
layout_load(struct spdk_ftl_dev *dev)
{
	if (ftl_superblock_load_blob_area(dev)) {
		return -1;
	}
	dev->layout.nvc.chunk_count = dev->layout.region[FTL_LAYOUT_REGION_TYPE_DATA_NVC].num_entries;
	if (ftl_superblock_md_layout_apply(dev)) {
		return -1;
	}
	return 0;
}

int
ftl_layout_setup(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	uint64_t i;
	uint64_t num_lbas;
	enum ftl_layout_setup_mode setup_mode;
	int rc;

	/*
	 * SB v5 adds the ability to create MD regions dynamically, i.e. depending on the underlying device type.
	 * For compatibility reasons:
	 * 1. When upgrading from pre-v5 SB, only the legacy default layout is created.
	 *    Pre-v5: some regions were static and not stored in the SB layout. These must be created to match
	 *            the legacy default layout.
	 *    v5: all regions are stored in the SB layout. Upon the SB upgrade, the legacy default layout
	 *        is updated with pre-v5 layout stored in the SB. The whole layout is then stored in v5 SB.
	 *
	 * 2. When SB v5 or later was loaded, the layout is instantiated from the nvc and base layout blobs.
	 *    No default layout is created.
	 *
	 * 3. When the FTL layout is being created for the first time, there are no restrictions.
	 *
	 * Any new regions to be created in cases (1) and (2) can only be placed in the unallocated area
	 * of the underlying device.
	 */

	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		setup_mode = FTL_LAYOUT_SETUP_MODE_NO_RESTRICT;
	} else if (ftl_superblock_is_blob_area_empty(dev->sb)) {
		setup_mode = FTL_LAYOUT_SETUP_MODE_LEGACY_DEFAULT;
	} else {
		setup_mode = FTL_LAYOUT_SETUP_MODE_LOAD_CURRENT;
	}
	FTL_NOTICELOG(dev, "FTL layout setup mode %d\n", (int)setup_mode);

	/* Invalidate all regions */
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		if (i == FTL_LAYOUT_REGION_TYPE_SB || i == FTL_LAYOUT_REGION_TYPE_SB_BASE) {
			/* Super block has been already initialized */
			continue;
		}

		layout->region[i].mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
		/* Mark the region inactive */
		layout->region[i].type = FTL_LAYOUT_REGION_TYPE_INVALID;
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

	layout->nvc.chunk_data_blocks =
		FTL_NV_CACHE_CHUNK_DATA_SIZE(ftl_get_num_blocks_in_band(dev)) / FTL_BLOCK_SIZE;
	layout->nvc.chunk_meta_size = FTL_NV_CACHE_CHUNK_MD_SIZE;
	layout->nvc.chunk_tail_md_num_blocks = ftl_nv_cache_chunk_tail_md_num_blocks(&dev->nv_cache);

	layout->base.num_usable_blocks = ftl_get_num_blocks_in_band(dev);
	layout->base.user_blocks = ftl_band_user_blocks(dev->bands);

	switch (setup_mode) {
	case FTL_LAYOUT_SETUP_MODE_LEGACY_DEFAULT:
		if (layout_setup_legacy_default(dev)) {
			return -EINVAL;
		}
		break;

	case FTL_LAYOUT_SETUP_MODE_LOAD_CURRENT:
		if (layout_load(dev)) {
			return -EINVAL;
		}
		break;

	case FTL_LAYOUT_SETUP_MODE_NO_RESTRICT:
		if (layout_setup_default(dev)) {
			return -EINVAL;
		}
		break;

	default:
		ftl_abort();
		break;
	}

	if (ftl_validate_regions(dev, layout)) {
		return -EINVAL;
	}

	rc = ftl_superblock_store_blob_area(dev);

	FTL_NOTICELOG(dev, "Base device capacity:         %.2f MiB\n",
		      blocks2mib(layout->base.total_blocks));
	FTL_NOTICELOG(dev, "NV cache device capacity:       %.2f MiB\n",
		      blocks2mib(layout->nvc.total_blocks));
	FTL_NOTICELOG(dev, "L2P entries:                    %"PRIu64"\n", dev->num_lbas);
	FTL_NOTICELOG(dev, "L2P address size:               %"PRIu64"\n", layout->l2p.addr_size);
	FTL_NOTICELOG(dev, "P2L checkpoint pages:           %"PRIu64"\n", layout->p2l.ckpt_pages);
	FTL_NOTICELOG(dev, "NV cache chunk count            %"PRIu64"\n", dev->layout.nvc.chunk_count);

	return rc;
}

int
ftl_layout_setup_superblock(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB];
	uint64_t total_blocks, offset, left;

	assert(layout->md[FTL_LAYOUT_REGION_TYPE_SB] == NULL);

	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	layout->base.total_blocks = spdk_bdev_get_num_blocks(bdev);

	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	layout->nvc.total_blocks = spdk_bdev_get_num_blocks(bdev);

	/* Initialize superblock region */
	if (layout_region_create_nvc(dev, FTL_LAYOUT_REGION_TYPE_SB, FTL_SB_VERSION_CURRENT,
				     superblock_region_size(dev), 1)) {
		FTL_ERRLOG(dev, "Error when setting up primary super block\n");
		return -1;
	}

	assert(region->bdev_desc != NULL);
	assert(region->ioch != NULL);
	assert(region->current.offset == 0);

	if (layout_region_create_base(dev, FTL_LAYOUT_REGION_TYPE_SB_BASE, FTL_SB_VERSION_CURRENT,
				      superblock_region_size(dev), 1)) {
		FTL_ERRLOG(dev, "Error when setting up secondary super block\n");
		return -1;
	}
	layout->region[FTL_LAYOUT_REGION_TYPE_SB].mirror_type = FTL_LAYOUT_REGION_TYPE_SB_BASE;

	region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE];
	assert(region->current.offset == 0);

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

int
ftl_layout_clear_superblock(struct spdk_ftl_dev *dev)
{
	int rc;

	rc = ftl_layout_tracker_bdev_rm_region(dev->nvc_layout_tracker, FTL_LAYOUT_REGION_TYPE_SB,
					       FTL_SB_VERSION_CURRENT);
	if (rc) {
		return rc;
	}

	return ftl_layout_tracker_bdev_rm_region(dev->base_layout_tracker, FTL_LAYOUT_REGION_TYPE_SB_BASE,
			FTL_SB_VERSION_CURRENT);
}

void
ftl_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_region *reg;
	enum ftl_layout_region_type i;

	FTL_NOTICELOG(dev, "NV cache layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		reg = ftl_layout_region_get(dev, i);
		if (reg && reg->bdev_desc == dev->nv_cache.bdev_desc) {
			dump_region(dev, reg);
		}
	}
	FTL_NOTICELOG(dev, "Base device layout:\n");
	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; ++i) {
		reg = ftl_layout_region_get(dev, i);
		if (reg && reg->bdev_desc == dev->base_bdev_desc) {
			dump_region(dev, reg);
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
	md_blocks += ftl_md_region_blocks(dev, spdk_divide_round_up(total_blocks, 8));

	/* Count space needed for superblock */
	md_blocks += superblock_region_blocks(dev);
	return md_blocks;
}

struct layout_blob_entry {
	uint32_t type;
	uint64_t entry_size;
	uint64_t num_entries;
} __attribute__((packed));

size_t
ftl_layout_blob_store(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_buf_sz)
{
	struct layout_blob_entry *blob_entry = blob_buf;
	struct ftl_layout_region *reg;
	enum ftl_layout_region_type reg_type;
	size_t blob_sz = 0;

	for (reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX; reg_type++) {
		if (blob_sz + sizeof(*blob_entry) > blob_buf_sz) {
			return 0;
		}

		reg = &dev->layout.region[reg_type];
		blob_entry->type = reg_type;
		blob_entry->entry_size = reg->entry_size;
		blob_entry->num_entries = reg->num_entries;

		blob_entry++;
		blob_sz += sizeof(*blob_entry);
	}

	return blob_sz;
}

int
ftl_layout_blob_load(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_sz)
{
	struct layout_blob_entry *blob_entry = blob_buf;
	size_t blob_entry_num = blob_sz / sizeof(*blob_entry);
	struct layout_blob_entry *blob_entry_end = blob_entry + blob_entry_num;
	struct ftl_layout_region *reg;

	if (blob_sz % sizeof(*blob_entry) != 0) {
		/* Invalid blob size */
		return -1;
	}

	for (; blob_entry < blob_entry_end; blob_entry++) {
		/* Verify the type */
		if (blob_entry->type >= FTL_LAYOUT_REGION_TYPE_MAX) {
			return -1;
		}

		/* Load the entry */
		reg = &dev->layout.region[blob_entry->type];
		reg->entry_size = blob_entry->entry_size;
		reg->num_entries = blob_entry->num_entries;
	}

	return 0;
}
