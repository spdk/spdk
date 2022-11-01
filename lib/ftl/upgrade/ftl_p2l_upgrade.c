/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "mngt/ftl_mngt.h"
#include "mngt/ftl_mngt_steps.h"
#include "ftl_layout_upgrade.h"

struct ftl_region_upgrade_desc p2l_upgrade_desc[] = {
	[FTL_P2L_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled
	},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(p2l_upgrade_desc) == FTL_P2L_VERSION_CURRENT,
		   "Missing P2L region upgrade descriptors");
