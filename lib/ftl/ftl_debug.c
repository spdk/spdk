/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/ftl.h"
#include "ftl_debug.h"
#include "ftl_band.h"

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
	size_t i, total = 0;

	if (!dev->bands) {
		return;
	}

	ftl_debug(dev, "Bands validity:\n");
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		if (!dev->bands[i].num_zones) {
			ftl_debug(dev, " Band %3zu: all zones are offline\n", i + 1);
			continue;
		}

		total += dev->bands[i].lba_map.num_vld;
		ftl_debug(dev, " Band %3zu: %8zu / %zu \tnum_zones: %zu \twr_cnt: %"PRIu64
			  "\tstate: %s\n",
			  i + 1, dev->bands[i].lba_map.num_vld,
			  ftl_band_user_blocks(&dev->bands[i]),
			  dev->bands[i].num_zones,
			  dev->bands[i].md->wr_cnt,
			  ftl_band_state_str[dev->bands[i].md->state]);
	}
}

#endif /* defined(DEBUG) */

void
ftl_dev_dump_stats(const struct spdk_ftl_dev *dev)
{
	size_t i, total = 0;
	char uuid[SPDK_UUID_STRING_LEN];

	if (!dev->bands) {
		return;
	}

	/* Count the number of valid LBAs */
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		total += dev->bands[i].lba_map.num_vld;
	}

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->uuid);
	FTL_NOTICELOG(dev, "\n");
	FTL_NOTICELOG(dev, "device UUID:         %s\n", uuid);
	FTL_NOTICELOG(dev, "total valid LBAs:    %zu\n", total);
}
