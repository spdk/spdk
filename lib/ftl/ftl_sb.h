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

bool ftl_superblock_check_magic(struct ftl_superblock *sb);

bool ftl_superblock_md_layout_is_empty(struct ftl_superblock *sb);

int ftl_superblock_md_layout_build(struct spdk_ftl_dev *dev);

int ftl_superblock_md_layout_load_all(struct spdk_ftl_dev *dev);

int ftl_superblock_md_layout_upgrade_region(struct spdk_ftl_dev *dev,
		struct ftl_superblock_md_region *sb_reg, uint32_t new_version);

void ftl_superblock_md_layout_dump(struct spdk_ftl_dev *dev);

#endif /* FTL_SB_H */
