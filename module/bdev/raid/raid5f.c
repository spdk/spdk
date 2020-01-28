/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_raid.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/log.h"

struct raid5f_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;

	/* Number of data blocks in a stripe (without parity) */
	uint64_t stripe_blocks;

	/* Number of stripes on this array */
	uint64_t total_stripes;
};

static inline uint8_t
raid5f_stripe_data_chunks_num(const struct raid_bdev *raid_bdev)
{
	return raid_bdev->num_base_bdevs - raid_bdev->module->base_bdevs_max_degraded;
}

static inline uint8_t
raid5f_stripe_parity_chunk_index(const struct raid_bdev *raid_bdev, uint64_t stripe_index)
{
	return raid5f_stripe_data_chunks_num(raid_bdev) - stripe_index % raid_bdev->num_base_bdevs;
}

static void
raid5f_chunk_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete(raid_io, success ? SPDK_BDEV_IO_STATUS_SUCCESS :
			      SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid5f_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid5f_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid5f_submit_rw_request(raid_io);
}

static int
raid5f_submit_read_request(struct raid_bdev_io *raid_io, uint64_t stripe_index,
			   uint64_t stripe_offset)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	uint8_t chunk_data_idx = stripe_offset >> raid_bdev->strip_size_shift;
	uint8_t p_idx = raid5f_stripe_parity_chunk_index(raid_bdev, stripe_index);
	uint8_t chunk_idx = chunk_data_idx < p_idx ? chunk_data_idx : chunk_data_idx + 1;
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[chunk_idx];
	struct spdk_io_channel *base_ch = raid_io->raid_ch->base_channel[chunk_idx];
	uint64_t chunk_offset = stripe_offset - (chunk_data_idx << raid_bdev->strip_size_shift);
	uint64_t base_offset_blocks = (stripe_index << raid_bdev->strip_size_shift) + chunk_offset;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	int ret;

	ret = spdk_bdev_readv_blocks(base_info->desc, base_ch,
				     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     base_offset_blocks, bdev_io->u.bdev.num_blocks,
				     raid5f_chunk_read_complete, raid_io);
	if (spdk_unlikely(ret == -ENOMEM)) {
		raid_bdev_queue_io_wait(raid_io, base_info->bdev, base_ch,
					_raid5f_submit_rw_request);
		return 0;
	}

	return ret;
}

static void
raid5f_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct raid5f_info *r5f_info = raid_io->raid_bdev->module_private;
	uint64_t offset_blocks = bdev_io->u.bdev.offset_blocks;
	uint64_t stripe_index = offset_blocks / r5f_info->stripe_blocks;
	uint64_t stripe_offset = offset_blocks % r5f_info->stripe_blocks;
	int ret;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		assert(bdev_io->u.bdev.num_blocks <= r5f_info->raid_bdev->strip_size);
		ret = raid5f_submit_read_request(raid_io, stripe_index, stripe_offset);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (spdk_unlikely(ret)) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int
raid5f_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	struct raid_base_bdev_info *base_info;
	struct raid5f_info *r5f_info;

	r5f_info = calloc(1, sizeof(*r5f_info));
	if (!r5f_info) {
		SPDK_ERRLOG("Failed to allocate r5f_info\n");
		return -ENOMEM;
	}
	r5f_info->raid_bdev = raid_bdev;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		min_blockcnt = spdk_min(min_blockcnt, base_info->bdev->blockcnt);
	}

	r5f_info->total_stripes = min_blockcnt / raid_bdev->strip_size;
	r5f_info->stripe_blocks = raid_bdev->strip_size * raid5f_stripe_data_chunks_num(raid_bdev);

	raid_bdev->bdev.blockcnt = r5f_info->stripe_blocks * r5f_info->total_stripes;
	raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
	raid_bdev->bdev.split_on_optimal_io_boundary = true;

	raid_bdev->module_private = r5f_info;

	return 0;
}

static void
raid5f_stop(struct raid_bdev *raid_bdev)
{
	struct raid5f_info *r5f_info = raid_bdev->module_private;

	free(r5f_info);
}

static struct raid_bdev_module g_raid5f_module = {
	.level = RAID5F,
	.base_bdevs_min = 3,
	.base_bdevs_max_degraded = 1,
	.start = raid5f_start,
	.stop = raid5f_stop,
	.submit_rw_request = raid5f_submit_rw_request,
};
RAID_MODULE_REGISTER(&g_raid5f_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid5f)
