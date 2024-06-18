/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_sb.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "upgrade/ftl_sb_upgrade.h"
#include "upgrade/ftl_sb_v3.h"
#include "upgrade/ftl_sb_v5.h"

static bool ftl_superblock_v2_check_magic(union ftl_superblock_ver *sb_ver);

struct sb_ops {
	bool (*check_magic)(union ftl_superblock_ver *sb_ver);

	bool (*blob_is_empty)(union ftl_superblock_ver *sb_ver);
	bool (*blob_validate)(struct spdk_ftl_dev *dev);
	int (*blob_store)(struct spdk_ftl_dev *dev);
	int (*blob_load)(struct spdk_ftl_dev *dev);

	int (*upgrade_region)(struct spdk_ftl_dev *dev, struct ftl_layout_region *reg,
			      uint32_t new_version);

	int (*layout_apply)(struct spdk_ftl_dev *dev);
	void (*layout_dump)(struct spdk_ftl_dev *dev);
};

static struct sb_ops *
sb_get_ops(uint64_t version)
{
	static struct sb_ops ops[] = {
		[FTL_SB_VERSION_0] = {
			.check_magic = ftl_superblock_v2_check_magic,
		},
		[FTL_SB_VERSION_1] = {
			.check_magic = ftl_superblock_v2_check_magic,
		},
		[FTL_SB_VERSION_2] = {
			.check_magic = ftl_superblock_v2_check_magic,
		},
		[FTL_SB_VERSION_3] = {
			.check_magic = ftl_superblock_v3_check_magic,
			.blob_is_empty = ftl_superblock_v3_md_layout_is_empty,
			.blob_load = ftl_superblock_v3_md_layout_load_all,
			.layout_dump = ftl_superblock_v3_md_layout_dump,
		},
		[FTL_SB_VERSION_4] = {
			.check_magic = ftl_superblock_v3_check_magic,
			.blob_is_empty = ftl_superblock_v3_md_layout_is_empty,
			.blob_load = ftl_superblock_v3_md_layout_load_all,
			.layout_dump = ftl_superblock_v3_md_layout_dump,
		},
		[FTL_SB_VERSION_5] = {
			.check_magic = ftl_superblock_v3_check_magic,
			.blob_is_empty = ftl_superblock_v5_is_blob_area_empty,
			.blob_validate = ftl_superblock_v5_validate_blob_area,
			.blob_store = ftl_superblock_v5_store_blob_area,
			.blob_load = ftl_superblock_v5_load_blob_area,
			.upgrade_region = ftl_superblock_v5_md_layout_upgrade_region,
			.layout_apply = ftl_superblock_v5_md_layout_apply,
			.layout_dump = ftl_superblock_v5_md_layout_dump,
		},
	};

	if (version >= SPDK_COUNTOF(ops)) {
		return NULL;
	}

	return &ops[version];
}

static bool
ftl_superblock_v2_check_magic(union ftl_superblock_ver *sb_ver)
{
	return sb_ver->header.magic == FTL_SUPERBLOCK_MAGIC_V2;
}

bool
ftl_superblock_check_magic(struct ftl_superblock *sb)
{
	union ftl_superblock_ver *sb_ver = (union ftl_superblock_ver *)sb;
	struct sb_ops *ops = sb_get_ops(sb_ver->header.version);

	if (!ops || !ops->check_magic) {
		ftl_abort();
		return false;
	}
	return ops->check_magic(sb_ver);
}

bool
ftl_superblock_is_blob_area_empty(struct ftl_superblock *sb)
{
	union ftl_superblock_ver *sb_ver = (union ftl_superblock_ver *)sb;
	struct sb_ops *ops = sb_get_ops(sb_ver->header.version);

	if (!ops || !ops->blob_is_empty) {
		ftl_abort();
		return false;
	}
	return ops->blob_is_empty(sb_ver);
}

bool
ftl_superblock_validate_blob_area(struct spdk_ftl_dev *dev)
{
	struct sb_ops *ops = sb_get_ops(dev->sb->header.version);

	if (!ops || !ops->blob_validate) {
		return true;
	}
	return ops->blob_validate(dev);
}

int
ftl_superblock_store_blob_area(struct spdk_ftl_dev *dev)
{
	struct sb_ops *ops = sb_get_ops(dev->sb->header.version);

	if (!ops || !ops->blob_store) {
		ftl_abort();
		return -1;
	}
	return ops->blob_store(dev);
}

int
ftl_superblock_load_blob_area(struct spdk_ftl_dev *dev)
{
	struct sb_ops *ops = sb_get_ops(dev->sb->header.version);

	if (!ops || !ops->blob_load) {
		ftl_abort();
		return -1;
	}
	return ops->blob_load(dev);
}

int
ftl_superblock_md_layout_upgrade_region(struct spdk_ftl_dev *dev,
					struct ftl_layout_region *reg, uint32_t new_version)
{
	struct sb_ops *ops = sb_get_ops(dev->sb->header.version);

	if (!ops || !ops->upgrade_region) {
		ftl_abort();
		return -1;
	}
	return ops->upgrade_region(dev, reg, new_version);
}

int
ftl_superblock_md_layout_apply(struct spdk_ftl_dev *dev)
{
	struct sb_ops *ops = sb_get_ops(dev->sb->header.version);

	if (!ops || !ops->layout_apply) {
		return 0;
	}
	return ops->layout_apply(dev);
}

void
ftl_superblock_md_layout_dump(struct spdk_ftl_dev *dev)
{
	struct sb_ops *ops = sb_get_ops(dev->sb->header.version);

	if (!ops || !ops->layout_dump) {
		ftl_abort();
		return;
	}
	return ops->layout_dump(dev);
}
