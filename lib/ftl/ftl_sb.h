/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_SB_H
#define FTL_SB_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"
#include "ftl_sb_current.h"

struct spdk_ftl_dev;
struct ftl_layout_region;

bool ftl_superblock_check_magic(struct ftl_superblock *sb);

bool ftl_superblock_is_blob_area_empty(struct ftl_superblock *sb);

bool ftl_superblock_validate_blob_area(struct spdk_ftl_dev *dev);

int ftl_superblock_store_blob_area(struct spdk_ftl_dev *dev);

int ftl_superblock_load_blob_area(struct spdk_ftl_dev *dev);

int ftl_superblock_md_layout_upgrade_region(struct spdk_ftl_dev *dev,
		struct ftl_layout_region *reg, uint32_t new_version);

int ftl_superblock_md_layout_apply(struct spdk_ftl_dev *dev);

void ftl_superblock_md_layout_dump(struct spdk_ftl_dev *dev);

#endif /* FTL_SB_H */
