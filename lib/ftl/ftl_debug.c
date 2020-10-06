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

#include "spdk/log.h"
#include "spdk/ftl.h"
#include "ftl_debug.h"
#include "ftl_band.h"

#if defined(DEBUG)
#if defined(FTL_META_DEBUG)

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

bool
ftl_band_validate_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_addr addr_md, addr_l2p;
	size_t i, size, seg_off;
	bool valid = true;

	size = ftl_get_num_blocks_in_band(dev);

	pthread_spin_lock(&lba_map->lock);
	for (i = 0; i < size; ++i) {
		if (!spdk_bit_array_get(lba_map->vld, i)) {
			continue;
		}

		seg_off = i / FTL_NUM_LBA_IN_BLOCK;
		if (lba_map->segments[seg_off] != FTL_LBA_MAP_SEG_CACHED) {
			continue;
		}

		addr_md = ftl_band_addr_from_block_offset(band, i);
		addr_l2p = ftl_l2p_get(dev, lba_map->map[i]);

		if (addr_l2p.cached) {
			continue;
		}

		if (addr_l2p.offset != addr_md.offset) {
			valid = false;
			break;
		}

	}

	pthread_spin_unlock(&lba_map->lock);

	return valid;
}

void
ftl_dev_dump_bands(struct spdk_ftl_dev *dev)
{
	size_t i, total = 0;

	if (!dev->bands) {
		return;
	}

	ftl_debug("Bands validity:\n");
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		if (dev->bands[i].state == FTL_BAND_STATE_FREE &&
		    dev->bands[i].wr_cnt == 0) {
			continue;
		}

		if (!dev->bands[i].num_zones) {
			ftl_debug(" Band %3zu: all zones are offline\n", i + 1);
			continue;
		}

		total += dev->bands[i].lba_map.num_vld;
		ftl_debug(" Band %3zu: %8zu / %zu \tnum_zones: %zu \twr_cnt: %"PRIu64"\tmerit:"
			  "%10.3f\tstate: %s\n",
			  i + 1, dev->bands[i].lba_map.num_vld,
			  ftl_band_user_blocks(&dev->bands[i]),
			  dev->bands[i].num_zones,
			  dev->bands[i].wr_cnt,
			  dev->bands[i].merit,
			  ftl_band_state_str[dev->bands[i].state]);
	}
}

#endif /* defined(FTL_META_DEBUG) */

#if defined(FTL_DUMP_STATS)

void
ftl_dev_dump_stats(const struct spdk_ftl_dev *dev)
{
	size_t i, total = 0;
	char uuid[SPDK_UUID_STRING_LEN];
	double waf;
	const char *limits[] = {
		[SPDK_FTL_LIMIT_CRIT]  = "crit",
		[SPDK_FTL_LIMIT_HIGH]  = "high",
		[SPDK_FTL_LIMIT_LOW]   = "low",
		[SPDK_FTL_LIMIT_START] = "start"
	};

	if (!dev->bands) {
		return;
	}

	/* Count the number of valid LBAs */
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		total += dev->bands[i].lba_map.num_vld;
	}

	waf = (double)dev->stats.write_total / (double)dev->stats.write_user;

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->uuid);
	ftl_debug("\n");
	ftl_debug("device UUID:         %s\n", uuid);
	ftl_debug("total valid LBAs:    %zu\n", total);
	ftl_debug("total writes:        %"PRIu64"\n", dev->stats.write_total);
	ftl_debug("user writes:         %"PRIu64"\n", dev->stats.write_user);
	ftl_debug("WAF:                 %.4lf\n", waf);
	ftl_debug("limits:\n");
	for (i = 0; i < SPDK_FTL_LIMIT_MAX; ++i) {
		ftl_debug(" %5s: %"PRIu64"\n", limits[i], dev->stats.limits[i]);
	}
}

#endif /* defined(FTL_DUMP_STATS) */
#endif /* defined(DEBUG) */
