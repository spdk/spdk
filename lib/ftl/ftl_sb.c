/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_sb.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "upgrade/ftl_sb_prev.h"

bool
ftl_superblock_check_magic(struct ftl_superblock *sb)
{
	if (sb->header.version >= FTL_SB_VERSION_3) {
		return sb->header.magic == FTL_SUPERBLOCK_MAGIC;
	} else {
		return sb->header.magic == FTL_SUPERBLOCK_MAGIC_V2;
	}
}
bool
ftl_superblock_md_layout_is_empty(struct ftl_superblock *sb)
{
	return sb->md_layout_head.type == FTL_LAYOUT_REGION_TYPE_INVALID;
}

static bool
md_region_is_fixed(int reg_type)
{
	switch (reg_type) {
#ifdef SPDK_FTL_VSS_EMU
	case FTL_LAYOUT_REGION_TYPE_VSS:
#endif
	case FTL_LAYOUT_REGION_TYPE_SB:
	case FTL_LAYOUT_REGION_TYPE_SB_BASE:
	case FTL_LAYOUT_REGION_TYPE_DATA_BASE:
		return true;

	default:
		return false;
	}
}

static uint32_t
md_regions_count(void)
{
	uint32_t reg_cnt = 0, reg_type;

	for (reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX; reg_type++) {
		if (md_region_is_fixed(reg_type)) {
			continue;
		}

		reg_cnt++;
	}
	return reg_cnt;
}

static bool
superblock_md_region_overflow(struct spdk_ftl_dev *dev, struct ftl_superblock_md_region *sb_reg)
{
	/* sb_reg is part of the sb structure - the pointer should be at a positive offset */
	if ((uintptr_t)sb_reg < (uintptr_t)dev->sb) {
		return true;
	}

	/* Make sure the entry doesn't overflow the pointer value (probably overkill to check) */
	if (UINT64_MAX - (uintptr_t)sb_reg <= sizeof(*sb_reg)) {
		return true;
	}

	/* There's only a finite (FTL_SUPERBLOCK_SIZE) amount of space in the superblock. Make sure the region wholly fits in that space. */
	if ((uintptr_t)(sb_reg + 1) > ((uintptr_t)(dev->sb) + FTL_SUPERBLOCK_SIZE)) {
		return true;
	}

	return false;
}

static int
superblock_md_layout_add(struct spdk_ftl_dev *dev, struct ftl_superblock_md_region *sb_reg,
			 uint32_t reg_type, uint32_t reg_version, uint64_t blk_offs, uint64_t blk_sz)
{
	if (superblock_md_region_overflow(dev, sb_reg)) {
		FTL_ERRLOG(dev, "Buffer overflow\n");
		return -EOVERFLOW;
	}

	sb_reg->type = reg_type;
	sb_reg->version = reg_version;
	sb_reg->blk_offs = blk_offs;
	sb_reg->blk_sz = blk_sz;
	return 0;
}

static int
superblock_md_layout_add_free(struct spdk_ftl_dev *dev, struct ftl_superblock_md_region **sb_reg,
			      uint32_t reg_type, uint32_t free_type, uint64_t total_blocks)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *reg = &layout->region[reg_type];
	uint64_t blks_left = total_blocks - reg->current.offset - reg->current.blocks;

	if (blks_left == 0) {
		return 0;
	}

	(*sb_reg)->df_next = ftl_df_get_obj_id(dev->sb, (*sb_reg) + 1);
	(*sb_reg) = (*sb_reg) + 1;

	if (superblock_md_layout_add(dev, *sb_reg, free_type, 0,
				     reg->current.offset + reg->current.blocks, blks_left)) {
		return -1;
	}

	(*sb_reg)->df_next = FTL_DF_OBJ_ID_INVALID;

	return 0;
}

int
ftl_superblock_md_layout_build(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock *sb = dev->sb;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *reg;
	int n = 0;
	bool is_empty = ftl_superblock_md_layout_is_empty(dev->sb);
	struct ftl_superblock_md_region *sb_reg = &sb->md_layout_head;

	/* TODO: major upgrades: add all free regions being tracked
	 * For now SB MD layout must be empty - otherwise md free regions may be lost */
	assert(is_empty);

	for (; n < FTL_LAYOUT_REGION_TYPE_MAX;) {
		reg = &layout->region[n];
		if (md_region_is_fixed(reg->type)) {
			reg->current.sb_md_reg = NULL;
			n++;

			if (n >= FTL_LAYOUT_REGION_TYPE_MAX) {
				/* For VSS emulation the last layout type is a fixed region, we need to move back the list and end the list on previous entry */
				sb_reg--;
				break;
			}
			continue;
		}

		if (superblock_md_layout_add(dev, sb_reg, reg->type, reg->current.version,
					     reg->current.offset, reg->current.blocks)) {
			return -1;
		}
		reg->current.sb_md_reg = sb_reg;

		n++;
		if (n < FTL_LAYOUT_REGION_TYPE_MAX) {
			/* next region */
			sb_reg->df_next = ftl_df_get_obj_id(sb, sb_reg + 1);
			sb_reg++;
		}
	}

	/* terminate the list */
	sb_reg->df_next = FTL_DF_OBJ_ID_INVALID;

	/* create free_nvc/free_base regions on the first run */
	if (is_empty) {
		superblock_md_layout_add_free(dev, &sb_reg, FTL_LAYOUT_REGION_LAST_NVC,
					      FTL_LAYOUT_REGION_TYPE_FREE_NVC, layout->nvc.total_blocks);

		superblock_md_layout_add_free(dev, &sb_reg, FTL_LAYOUT_REGION_LAST_BASE,
					      FTL_LAYOUT_REGION_TYPE_FREE_BASE, layout->base.total_blocks);
	}

	return 0;
}

int
ftl_superblock_md_layout_load_all(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock *sb = dev->sb;
	struct ftl_superblock_md_region *sb_reg = &sb->md_layout_head;
	struct ftl_layout *layout = &dev->layout;
	uint32_t regs_found = 0;

	for (int n = 0; n < FTL_LAYOUT_REGION_TYPE_MAX; n++) {
		if (md_region_is_fixed(sb_reg->type)) {
			continue;
		}

		layout->region[n].current.sb_md_reg = NULL;
		layout->region[n].prev = layout->region[n].current;
	}

	while (sb_reg->type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		struct ftl_layout_region *reg;

		/* TODO: major upgrades: add free regions tracking */
		if (sb_reg->type == FTL_LAYOUT_REGION_TYPE_FREE_NVC ||
		    sb_reg->type == FTL_LAYOUT_REGION_TYPE_FREE_BASE) {
			goto next_sb_reg;
		}

		if (sb_reg->type >= FTL_LAYOUT_REGION_TYPE_MAX) {
			FTL_ERRLOG(dev, "Invalid MD region type found\n");
			return -1;
		}

		if (md_region_is_fixed(sb_reg->type)) {
			FTL_ERRLOG(dev, "Unsupported MD region type found\n");
			return -1;
		}

		reg = &layout->region[sb_reg->type];
		if (sb_reg->version == reg->current.version) {
			if (reg->current.sb_md_reg) {
				FTL_ERRLOG(dev, "Multiple/looping current regions found\n");
				return -1;
			}

			reg->current.offset = sb_reg->blk_offs;
			reg->current.blocks = sb_reg->blk_sz;
			reg->current.sb_md_reg = sb_reg;

			if (!reg->prev.sb_md_reg) {
				regs_found++;
			}
		} else {
			if (sb_reg->version > reg->current.version) {
				FTL_ERRLOG(dev, "Unknown region version found\n");
				return -1;
			}
			/*
			 * The metadata regions are kept as a linked list, it's therefore possible to have it corrupted and contain loops.
			 * If the prev region has already been updated to the oldest found version (see following comment/block) and we see
			 * the same version again, it's a loop and error.
			 */
			if (sb_reg->version == reg->prev.version) {
				FTL_ERRLOG(dev, "Multiple/looping prev regions found\n");
				return -1;
			}

			/*
			 * The following check is in preparation for major metadata upgrade.
			 * It's possible that a region will need to be increased in size. It will allocate an additional region in this case
			 * and upgrade each entries one by one. If in the middle of this upgrade process a dirty shutdown occurs, there will be
			 * multiple entries of the same metadata region in the superblock (old original and partially updated one). This will then
			 * effectively rollback the upgrade to the oldest found metadata section and start from the beginning.
			 */
			if (sb_reg->version < reg->prev.version) {
				if ((!reg->current.sb_md_reg) && (!reg->prev.sb_md_reg)) {
					regs_found++;
				}

				reg->prev.offset = sb_reg->blk_offs;
				reg->prev.blocks = sb_reg->blk_sz;
				reg->prev.version = sb_reg->version;
				reg->prev.sb_md_reg = sb_reg;
			}
		}

next_sb_reg:
		if (sb_reg->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		if (UINT64_MAX - (uintptr_t)sb <= sb_reg->df_next) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return -EOVERFLOW;
		}

		sb_reg = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
		if (superblock_md_region_overflow(dev, sb_reg)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return -EOVERFLOW;
		}
	}

	if (regs_found != md_regions_count()) {
		FTL_ERRLOG(dev, "Missing regions\n");
		return -1;
	}

	return 0;
}

static void
ftl_superblock_md_layout_free_region(struct spdk_ftl_dev *dev,
				     struct ftl_superblock_md_region *sb_reg)
{
	/* TODO: major upgrades: implement */
	sb_reg->type = FTL_LAYOUT_REGION_TYPE_FREE_NVC;
}

int
ftl_superblock_md_layout_upgrade_region(struct spdk_ftl_dev *dev,
					struct ftl_superblock_md_region *sb_reg, uint32_t new_version)
{
	struct ftl_superblock *sb = dev->sb;
	struct ftl_superblock_md_region *sb_reg_iter = &sb->md_layout_head;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *reg = &layout->region[sb_reg->type];
	uint32_t old_version = sb_reg->version;

	assert(sb_reg);
	assert(reg->prev.sb_md_reg == sb_reg);
	assert(new_version > old_version);

	while (sb_reg_iter->type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		if (sb_reg_iter->type != sb_reg->type) {
			goto next_sb_reg_iter;
		}

		/* Verify all region versions up to new_version are updated: */
		if (sb_reg_iter->version != old_version && sb_reg_iter->version < new_version) {
			FTL_ERRLOG(dev, "Region upgrade skipped\n");
			return -1;
		}

		if (sb_reg_iter->version == new_version) {
			/* Major upgrades: update region prev version to the new version region found */
			assert(sb_reg != sb_reg_iter);
			reg->prev.offset = sb_reg_iter->blk_offs;
			reg->prev.blocks = sb_reg_iter->blk_sz;
			reg->prev.version = sb_reg_iter->version;
			reg->prev.sb_md_reg = sb_reg_iter;

			ftl_superblock_md_layout_free_region(dev, sb_reg);
			goto exit;
		}

next_sb_reg_iter:
		if (sb_reg_iter->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		sb_reg_iter = ftl_df_get_obj_ptr(sb, sb_reg_iter->df_next);
		if (superblock_md_region_overflow(dev, sb_reg_iter)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return -EOVERFLOW;
		}
	}

	/* Minor upgrades: update the region in place (only the new version) */
	assert(sb_reg == reg->prev.sb_md_reg);
	sb_reg->version = new_version;
	reg->prev.version = new_version;

exit:
	/* Update the region current version */
	if (new_version == reg->current.version) {
		reg->current.offset = sb_reg->blk_offs;
		reg->current.blocks = sb_reg->blk_sz;
		reg->current.version = sb_reg->version;
		reg->current.sb_md_reg = sb_reg;
	}

	return 0;
}

void
ftl_superblock_md_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock *sb = dev->sb;
	struct ftl_superblock_md_region *sb_reg = &sb->md_layout_head;

	FTL_NOTICELOG(dev, "SB metadata layout:\n");
	while (sb_reg->type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		FTL_NOTICELOG(dev,
			      "Region df:0x%"PRIx64" type:0x%"PRIx32" ver:%"PRIu32" blk_offs:0x%"PRIx64" blk_sz:0x%"PRIx64"\n",
			      ftl_df_get_obj_id(sb, sb_reg), sb_reg->type, sb_reg->version, sb_reg->blk_offs, sb_reg->blk_sz);

		if (sb_reg->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		sb_reg = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
		if (superblock_md_region_overflow(dev, sb_reg)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return;
		}
	}
}
