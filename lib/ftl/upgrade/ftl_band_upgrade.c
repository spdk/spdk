/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_band.h"
#include "ftl_layout_upgrade.h"

struct ftl_region_upgrade_desc band_upgrade_desc[] = {
	[FTL_BAND_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled,
	},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(band_upgrade_desc) == FTL_BAND_VERSION_CURRENT,
		   "Missing band region upgrade descriptors");
