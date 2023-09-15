/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_sb_v3.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "upgrade/ftl_sb_upgrade.h"

bool
ftl_superblock_v3_check_magic(union ftl_superblock_ver *sb_ver)
{
	return sb_ver->header.magic == FTL_SUPERBLOCK_MAGIC;
}

bool
ftl_superblock_v3_md_layout_is_empty(union ftl_superblock_ver *sb_ver)
{
	return sb_ver->v3.md_layout_head.type == FTL_LAYOUT_REGION_TYPE_INVALID;
}

static bool
md_region_is_fixed(int reg_type)
{
	switch (reg_type) {
	case FTL_LAYOUT_REGION_TYPE_SB:
	case FTL_LAYOUT_REGION_TYPE_SB_BASE:
	case FTL_LAYOUT_REGION_TYPE_DATA_BASE:
		return true;

	default:
		return false;
	}
}

bool
ftl_superblock_v3_md_region_overflow(struct spdk_ftl_dev *dev,
				     struct ftl_superblock_v3_md_region *sb_reg)
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

int
ftl_superblock_v3_md_layout_load_all(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock_v3 *sb = (struct ftl_superblock_v3 *)dev->sb;
	struct ftl_superblock_v3_md_region *sb_reg = &sb->md_layout_head;
	struct ftl_layout *layout = &dev->layout;
	uint32_t regs_found;
	uint32_t n;
	ftl_df_obj_id df_sentinel = FTL_DF_OBJ_ID_INVALID;
	ftl_df_obj_id df_prev = ftl_df_get_obj_id(sb, sb_reg);

	for (n = 0; n < FTL_LAYOUT_REGION_TYPE_MAX_V3; n++) {
		if (md_region_is_fixed(n)) {
			continue;
		}
		layout->region[n].type = FTL_LAYOUT_REGION_TYPE_INVALID;
	}

	while (sb_reg->type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		struct ftl_layout_region *reg;

		/* TODO: major upgrades: add free regions tracking */
		if (sb_reg->type == FTL_LAYOUT_REGION_TYPE_FREE_NVC ||
		    sb_reg->type == FTL_LAYOUT_REGION_TYPE_FREE_BASE) {
			goto next_sb_reg;
		}

		if (sb_reg->type >= FTL_LAYOUT_REGION_TYPE_MAX_V3) {
			FTL_ERRLOG(dev, "Invalid MD region type found\n");
			return -1;
		}

		if (md_region_is_fixed(sb_reg->type)) {
			FTL_ERRLOG(dev, "Unsupported MD region type found\n");
			return -1;
		}

		reg = &layout->region[sb_reg->type];
		/* Find the oldest region version */
		if (reg->type == FTL_LAYOUT_REGION_TYPE_INVALID || sb_reg->version < reg->current.version) {
			reg->type = sb_reg->type;
			reg->current.offset = sb_reg->blk_offs;
			reg->current.blocks = sb_reg->blk_sz;
			reg->current.version = sb_reg->version;
		} else if (sb_reg->version == reg->current.version) {
			FTL_ERRLOG(dev, "Multiple/looping regions found\n");
			return -EAGAIN;
		}

next_sb_reg:
		if (sb_reg->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		if (UINT64_MAX - (uintptr_t)sb <= sb_reg->df_next) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return -EOVERFLOW;
		}

		if (sb_reg->df_next <= df_prev) {
			df_sentinel = df_prev;
		}
		df_prev = sb_reg->df_next;

		if (df_sentinel != FTL_DF_OBJ_ID_INVALID && sb_reg->df_next == df_sentinel) {
			FTL_ERRLOG(dev, "Looping regions found\n");
			return -ELOOP;
		}

		sb_reg = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
		if (ftl_superblock_v3_md_region_overflow(dev, sb_reg)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return -EOVERFLOW;
		}
	}

	for (regs_found = 0, n = 0; n < FTL_LAYOUT_REGION_TYPE_MAX_V3; n++) {
		if (layout->region[n].type == n) {
			regs_found++;
		}
	}

	if (regs_found != FTL_LAYOUT_REGION_TYPE_MAX_V3) {
		FTL_ERRLOG(dev, "Missing regions\n");
		return -1;
	}

	return 0;
}

void
ftl_superblock_v3_md_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock_v3 *sb = (struct ftl_superblock_v3 *)dev->sb;
	struct ftl_superblock_v3_md_region *sb_reg = &sb->md_layout_head;

	FTL_NOTICELOG(dev, "SB metadata layout:\n");
	while (sb_reg->type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		FTL_NOTICELOG(dev,
			      "Region df:0x%"PRIx64" type:0x%"PRIx32" ver:%"PRIu32" blk_offs:0x%"PRIx64" blk_sz:0x%"PRIx64"\n",
			      ftl_df_get_obj_id(sb, sb_reg), sb_reg->type, sb_reg->version, sb_reg->blk_offs, sb_reg->blk_sz);

		if (sb_reg->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		sb_reg = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
		if (ftl_superblock_v3_md_region_overflow(dev, sb_reg)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return;
		}
	}
}
