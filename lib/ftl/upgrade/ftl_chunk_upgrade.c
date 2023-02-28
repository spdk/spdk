/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_nv_cache.h"
#include "ftl_layout_upgrade.h"

struct ftl_region_upgrade_desc nvc_upgrade_desc[] = {
	[FTL_NVC_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled,
	},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(nvc_upgrade_desc) == FTL_NVC_VERSION_CURRENT,
		   "Missing NVC region upgrade descriptors");
