/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/ftl.h"
#include "ftl_debug.h"
#include "ftl_band.h"

/* TODO: Switch to INFOLOG instead, we can control the printing via spdk_log_get_flag */
#if defined(DEBUG)

static const char *ftl_band_state_str[] = {
	"free",
	"prep",
	"opening",
	"open",
	"full",
	"closing",
	"closed",
	"max"
};

void
ftl_dev_dump_bands(struct spdk_ftl_dev *dev)
{
	uint64_t i;

	if (!dev->bands) {
		return;
	}

	FTL_NOTICELOG(dev, "Bands validity:\n");
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		FTL_NOTICELOG(dev, " Band %3zu: %8zu / %zu \twr_cnt: %"PRIu64
			      "\tstate: %s\n",
			      i + 1, dev->bands[i].p2l_map.num_valid,
			      ftl_band_user_blocks(&dev->bands[i]),
			      dev->bands[i].md->wr_cnt,
			      ftl_band_state_str[dev->bands[i].md->state]);
	}
}

#endif /* defined(DEBUG) */

void
ftl_dev_dump_stats(const struct spdk_ftl_dev *dev)
{
	uint64_t i, total = 0;
	char uuid[SPDK_UUID_STRING_LEN];

	if (!dev->bands) {
		return;
	}

	/* Count the number of valid LBAs */
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		total += dev->bands[i].p2l_map.num_valid;
	}

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->conf.uuid);
	FTL_NOTICELOG(dev, "\n");
	FTL_NOTICELOG(dev, "device UUID:         %s\n", uuid);
	FTL_NOTICELOG(dev, "total valid LBAs:    %zu\n", total);
}
