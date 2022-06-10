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
#include "spdk/bdev_zone.h"

#include "ftl_mngt_zone.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"
#include "ftl_core.h"
#include "ftl_band.h"

#define FTL_ZONE_INFO_COUNT	64

struct ftl_zone_init_cntx {
	/* Buffer for reading zone info  */
	struct spdk_bdev_zone_info	info[FTL_ZONE_INFO_COUNT];
	/* Currently read zone */
	size_t				zone_id;
};

static void get_zone_info(struct ftl_mngt *mngt);

static void get_zone_info_cb(struct spdk_bdev_io *bdev_io,
			     bool success, void *cb_arg)
{
	struct ftl_mngt *mngt = cb_arg;
	struct ftl_zone_init_cntx *cntx;
	struct spdk_ftl_dev *dev;
	struct ftl_band *band;
	struct ftl_zone *zone;
	ftl_addr addr;
	size_t i, zones_left, num_zones;

	spdk_bdev_free_io(bdev_io);

	cntx = ftl_mngt_get_step_cntx(mngt);
	dev = ftl_mngt_get_dev(mngt);

	if (spdk_unlikely(!success)) {
		FTL_ERRLOG(dev, "Unable to read zone info for zone id: %"PRIu64"\n",
			   cntx->zone_id);
		ftl_mngt_fail_step(mngt);
		return;
	}

	zones_left = ftl_get_num_zones(dev) - (cntx->zone_id / ftl_get_num_blocks_in_zone(dev));
	num_zones = spdk_min(zones_left, FTL_ZONE_INFO_COUNT);

	for (i = 0; i < num_zones; ++i) {
		addr = cntx->info[i].zone_id;
		band = &dev->bands[ftl_addr_get_band(dev, addr)];
		zone = &band->zone_buf[ftl_addr_get_punit(dev, addr)];
		zone->info = cntx->info[i];

		/* TODO: add support for zone capacity less than zone size */
		if (zone->info.capacity != ftl_get_num_blocks_in_zone(dev)) {
			zone->info.state = SPDK_BDEV_ZONE_STATE_OFFLINE;
			FTL_ERRLOG(dev, "Zone capacity is not equal zone size for "
				   "zone id: %"PRIu64"\n", cntx->zone_id);
		}

		/* Set write pointer to the last block plus one for zone in full state */
		if (zone->info.state == SPDK_BDEV_ZONE_STATE_FULL) {
			zone->info.write_pointer = zone->info.zone_id + zone->info.capacity;
		}

		if (zone->info.state != SPDK_BDEV_ZONE_STATE_OFFLINE) {
			band->num_zones++;
			CIRCLEQ_INSERT_TAIL(&band->zones, zone, circleq);
		}
	}

	cntx->zone_id = cntx->zone_id + num_zones * ftl_get_num_blocks_in_zone(dev);
	get_zone_info(mngt);
}

static void get_zone_info(struct ftl_mngt *mngt)
{
	struct ftl_zone_init_cntx *cntx;
	struct spdk_ftl_dev *dev;
	size_t zones_left, num_zones;
	int rc;

	cntx = ftl_mngt_get_step_cntx(mngt);
	dev = ftl_mngt_get_dev(mngt);

	zones_left = ftl_get_num_zones(dev) - (cntx->zone_id / ftl_get_num_blocks_in_zone(dev));
	if (zones_left == 0) {
		ftl_mngt_next_step(mngt);
		return;
	}

	num_zones = spdk_min(zones_left, FTL_ZONE_INFO_COUNT);

	rc = spdk_bdev_get_zone_info(dev->base_bdev_desc, dev->base_ioch,
				     cntx->zone_id, num_zones, cntx->info,
				     get_zone_info_cb, mngt);

	if (spdk_unlikely(rc != 0)) {
		FTL_ERRLOG(dev, "Unable to read zone info for zone id: %"PRIu64"\n",
			   cntx->zone_id);
		ftl_mngt_fail_step(mngt);
	}
}

static void
zone_emulation_init(struct ftl_mngt *mngt)
{
	struct spdk_ftl_dev *dev;
	struct ftl_band *band;
	struct ftl_zone *zone;
	size_t i;

	dev = ftl_mngt_get_dev(mngt);

	for (i = 0; i < ftl_get_num_zones(dev); ++i) {
		band = &dev->bands[i];
		zone = &band->zone_buf[0];
		zone->info.capacity = ftl_get_num_blocks_in_zone(dev);
		zone->info.zone_id = i * zone->info.capacity;
		zone->info.write_pointer = zone->info.zone_id + zone->info.capacity;
		zone->info.state = SPDK_BDEV_ZONE_STATE_FULL;
		band->num_zones = 1;
		CIRCLEQ_INSERT_TAIL(&band->zones, zone, circleq);
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_init_zone(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_mngt_alloc_step_cntx(mngt,
				     sizeof(struct ftl_zone_init_cntx))) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (ftl_is_zoned(dev)) {
		get_zone_info(mngt);
	} else {
		zone_emulation_init(mngt);
	}
}
