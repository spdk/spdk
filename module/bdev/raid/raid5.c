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

#include "bdev_raid.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/log.h"

struct raid5_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;

	/* Number of data blocks in a stripe (without parity) */
	uint64_t stripe_blocks;

	/* Number of stripes on this array */
	uint64_t total_stripes;
};

static inline uint8_t
raid5_stripe_data_chunks_num(const struct raid_bdev *raid_bdev)
{
	return raid_bdev->num_base_bdevs - raid_bdev->module->base_bdevs_max_degraded;
}

static void
raid5_submit_rw_request(struct raid_bdev_io *raid_io)
{
	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static int
raid5_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	struct raid5_info *r5info;

	r5info = calloc(1, sizeof(*r5info));
	if (!r5info) {
		SPDK_ERRLOG("Failed to allocate r5info\n");
		return -ENOMEM;
	}
	r5info->raid_bdev = raid_bdev;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		min_blockcnt = spdk_min(min_blockcnt, base_info->bdev->blockcnt);
	}

	r5info->total_stripes = min_blockcnt / raid_bdev->strip_size;
	r5info->stripe_blocks = raid_bdev->strip_size * raid5_stripe_data_chunks_num(raid_bdev);

	raid_bdev->bdev.blockcnt = r5info->stripe_blocks * r5info->total_stripes;
	raid_bdev->bdev.optimal_io_boundary = r5info->stripe_blocks;
	raid_bdev->bdev.split_on_optimal_io_boundary = true;

	raid_bdev->module_private = r5info;

	return 0;
}

static void
raid5_stop(struct raid_bdev *raid_bdev)
{
	struct raid5_info *r5info = raid_bdev->module_private;

	free(r5info);
}

static struct raid_bdev_module g_raid5_module = {
	.level = RAID5,
	.base_bdevs_min = 3,
	.base_bdevs_max_degraded = 1,
	.start = raid5_start,
	.stop = raid5_stop,
	.submit_rw_request = raid5_submit_rw_request,
};
RAID_MODULE_REGISTER(&g_raid5_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid5)
