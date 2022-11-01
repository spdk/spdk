/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_SB_UPGRADE_H
#define FTL_SB_UPGRADE_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"
#include "ftl_sb_prev.h"
#include "ftl_sb_current.h"

struct spdk_ftl_dev;
struct ftl_layout_region;

union ftl_superblock_ver {
	struct ftl_superblock_header header;
	struct ftl_superblock_v2 v2;
	struct ftl_superblock v3;
};

#endif /* FTL_SB_UPGRADE_H */
