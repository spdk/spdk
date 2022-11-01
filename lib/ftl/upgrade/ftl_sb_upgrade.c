/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_sb_upgrade.h"
#include "ftl_layout_upgrade.h"
#include "ftl_layout.h"
#include "ftl_core.h"

struct ftl_region_upgrade_desc sb_upgrade_desc[] = {
	[FTL_SB_VERSION_0] = {.verify = ftl_region_upgrade_disabled},
	[FTL_SB_VERSION_1] = {.verify = ftl_region_upgrade_disabled},
	[FTL_SB_VERSION_2] = {.verify = ftl_region_upgrade_disabled},
	[FTL_SB_VERSION_3] = {.verify = ftl_region_upgrade_disabled},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(sb_upgrade_desc) == FTL_SB_VERSION_CURRENT,
		   "Missing SB region upgrade descriptors");
