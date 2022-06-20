/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/ftl.h"
#include "ftl_debug.h"

void
ftl_dev_dump_stats(const struct spdk_ftl_dev *dev)
{
	size_t total = 0;
	char uuid[SPDK_UUID_STRING_LEN];

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->conf.uuid);
	FTL_NOTICELOG(dev, "\n");
	FTL_NOTICELOG(dev, "device UUID:         %s\n", uuid);
	FTL_NOTICELOG(dev, "total valid LBAs:    %zu\n", total);
}
