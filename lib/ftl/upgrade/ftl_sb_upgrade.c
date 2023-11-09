/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_sb_upgrade.h"
#include "ftl_layout_upgrade.h"
#include "ftl_layout.h"
#include "ftl_core.h"
#include "ftl_sb_v3.h"
#include "utils/ftl_df.h"
#include "utils/ftl_layout_tracker_bdev.h"

static int
sb_v4_to_v5_verify(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	struct ftl_layout_region *reg;
	uint32_t reg_no;
	int rc = ftl_region_upgrade_enabled(dev, region);

	if (rc) {
		return rc;
	}

	/* Verify there are no pending major upgrades */
	for (reg_no = 0; reg_no < FTL_LAYOUT_REGION_TYPE_MAX; reg_no++) {
		reg = ftl_layout_region_get(dev, reg_no);
		if (!reg) {
			/* This region does not exist */
			continue;
		}

		if (reg->current.version <= ftl_layout_upgrade_region_get_latest_version(reg->type)) {
			/* Only latest region version found */
			continue;
		}

		/* Previous version found, major upgrade */
		FTL_WARNLOG(dev, "FTL superblock upgrade v4 to v5 disabled: " \
			    "cannot upgrade region type 0x%"PRIx32" v%"PRId64" to v%"PRId64", " \
			    "offs 0x%"PRIx64", blks 0x%"PRIx64"\n",
			    reg->type, reg->current.version, ftl_layout_upgrade_region_get_latest_version(reg->type),
			    reg->current.offset, reg->current.blocks);
		return -1;
	}

	return 0;
}

static bool
sb_v3_md_region_is_fixed(int reg_type)
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

static bool
sb_v3_md_region_is_nvc(int reg_type)
{
	switch (reg_type) {
	case FTL_LAYOUT_REGION_TYPE_SB_BASE:
	case FTL_LAYOUT_REGION_TYPE_VALID_MAP:
	case FTL_LAYOUT_REGION_TYPE_DATA_BASE:
		return false;

	default:
		return true;
	}
}

static int
sb_v3_md_layout_convert(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock_v3 *sb = (struct ftl_superblock_v3 *)dev->sb;
	struct ftl_superblock_v3_md_region *sb_reg = &sb->md_layout_head;
	const struct ftl_layout_tracker_bdev_region_props *reg_props;

	while (sb_reg->type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		if (sb_reg->type == FTL_LAYOUT_REGION_TYPE_FREE_NVC ||
		    sb_reg->type == FTL_LAYOUT_REGION_TYPE_FREE_BASE) {
			goto next_sb_reg;
		}

		if (sb_reg->type >= FTL_LAYOUT_REGION_TYPE_MAX) {
			FTL_ERRLOG(dev, "Invalid MD region type found\n");
			return -1;
		}

		if (sb_v3_md_region_is_fixed(sb_reg->type)) {
			FTL_ERRLOG(dev, "Unsupported MD region type found\n");
			return -1;
		}

		if (sb_v3_md_region_is_nvc(sb_reg->type)) {
			reg_props = ftl_layout_tracker_bdev_insert_region(dev->nvc_layout_tracker, sb_reg->type,
					sb_reg->version, sb_reg->blk_offs, sb_reg->blk_sz);
		} else {
			reg_props = ftl_layout_tracker_bdev_insert_region(dev->base_layout_tracker, sb_reg->type,
					sb_reg->version, sb_reg->blk_offs, sb_reg->blk_sz);
		}
		if (!reg_props) {
			FTL_ERRLOG(dev, "Cannot upgrade SB MD layout - region type 0x%"PRIx32" v%"PRId32" " \
				   "offs 0x%"PRIx64" blks 0x%"PRIx64"\n",
				   sb_reg->type, sb_reg->version, sb_reg->blk_offs, sb_reg->blk_sz);
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
		if (ftl_superblock_v3_md_region_overflow(dev, sb_reg)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return -EOVERFLOW;
		}
	}

	return 0;
}

static int
sb_v4_to_v5_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	union ftl_superblock_ver *sb = (union ftl_superblock_ver *)dev->sb;

	FTL_NOTICELOG(dev, "FTL superblock upgrade v4 to v5\n");

	/* Convert v3 MD layout */
	if (ftl_superblock_is_blob_area_empty(dev->sb)) {
		FTL_ERRLOG(dev, "SBv3 MD layout empty\n");
		return -1;
	}
	if (sb_v3_md_layout_convert(dev)) {
		FTL_ERRLOG(dev, "SBv3 MD layout load failed\n");
		return -1;
	}

	/* Bump up the version */
	sb->v5.header.version = FTL_SB_VERSION_5;
	sb->v5.blob_area_end = 0;

	/* Keep v5 layout empty */
	memset(sb->v5.nvc_dev_name, 0, sizeof(sb->v5.nvc_dev_name));
	memset(&sb->v5.md_layout_nvc, 0, sizeof(sb->v5.md_layout_nvc));
	memset(sb->v5.base_dev_name, 0, sizeof(sb->v5.base_dev_name));
	memset(&sb->v5.md_layout_base, 0, sizeof(sb->v5.md_layout_base));
	memset(&sb->v5.layout_params, 0, sizeof(sb->v5.layout_params));

	return 0;
}

struct ftl_region_upgrade_desc sb_upgrade_desc[] = {
	[FTL_SB_VERSION_0] = {.verify = ftl_region_upgrade_disabled},
	[FTL_SB_VERSION_1] = {.verify = ftl_region_upgrade_disabled},
	[FTL_SB_VERSION_2] = {.verify = ftl_region_upgrade_disabled},
	[FTL_SB_VERSION_3] = {.verify = ftl_region_upgrade_disabled},
	[FTL_SB_VERSION_4] = {.verify = sb_v4_to_v5_verify, .upgrade = sb_v4_to_v5_upgrade, .new_version = FTL_SB_VERSION_5},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(sb_upgrade_desc) == FTL_SB_VERSION_CURRENT,
		   "Missing SB region upgrade descriptors");
