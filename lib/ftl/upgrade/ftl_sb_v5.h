/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Solidigm.
 *   All rights reserved.
 */

#ifndef FTL_SB_V5_H
#define FTL_SB_V5_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"
#include "ftl_sb_current.h"
#include "upgrade/ftl_sb_prev.h"

struct spdk_ftl_dev;
struct ftl_layout_region;
union ftl_superblock_ver;

bool ftl_superblock_v5_is_blob_area_empty(union ftl_superblock_ver *sb_ver);

bool ftl_superblock_v5_validate_blob_area(struct spdk_ftl_dev *dev);

int ftl_superblock_v5_store_blob_area(struct spdk_ftl_dev *dev);

int ftl_superblock_v5_load_blob_area(struct spdk_ftl_dev *dev);

int ftl_superblock_v5_md_layout_upgrade_region(struct spdk_ftl_dev *dev,
		struct ftl_layout_region *reg, uint32_t new_version);

int ftl_superblock_v5_md_layout_apply(struct spdk_ftl_dev *dev);

void ftl_superblock_v5_md_layout_dump(struct spdk_ftl_dev *dev);

#endif /* FTL_SB_V5_H */
