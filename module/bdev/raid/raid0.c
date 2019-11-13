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
#include "spdk/io_channel.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/*
 * brief:
 * raid0_bdev_io_completion function is called by lower layers to notify raid
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdev io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context (parent raid_bdev_io)
 * returns:
 * none
 */
static void
raid0_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
raid0_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid0_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid0_submit_rw_request(raid_io);
}

/*
 * brief:
 * raid0_submit_rw_request function is used to submit I/O to the correct
 * member disk for raid0 bdevs.
 * params:
 * raid_io
 * returns:
 * none
 */
static void
raid0_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(raid_io);
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	uint64_t			pd_strip;
	uint32_t			offset_in_strip;
	uint64_t			pd_lba;
	uint64_t			pd_blocks;
	uint8_t				pd_idx;
	int				ret = 0;
	uint64_t			start_strip;
	uint64_t			end_strip;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	start_strip = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 1) {
		assert(false);
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	pd_strip = start_strip / raid_bdev->num_base_bdevs;
	pd_idx = start_strip % raid_bdev->num_base_bdevs;
	offset_in_strip = bdev_io->u.bdev.offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;
	pd_blocks = bdev_io->u.bdev.num_blocks;
	base_info = &raid_bdev->base_bdev_info[pd_idx];
	if (base_info->desc == NULL) {
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
		assert(0);
	}

	/*
	 * Submit child io to bdev layer with using base bdev descriptors, base
	 * bdev lba, base bdev child io length in blocks, buffer, completion
	 * function and function callback context
	 */
	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);
	base_ch = raid_ch->base_channel[pd_idx];
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = spdk_bdev_readv_blocks(base_info->desc, base_ch,
					     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     pd_lba, pd_blocks, raid0_bdev_io_completion,
					     raid_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		ret = spdk_bdev_writev_blocks(base_info->desc, base_ch,
					      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					      pd_lba, pd_blocks, raid0_bdev_io_completion,
					      raid_io);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
		assert(0);
	}

	if (ret == -ENOMEM) {
		raid_bdev_queue_io_wait(raid_io, base_info->bdev, base_ch,
					_raid0_submit_rw_request);
	} else if (ret != 0) {
		SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
		assert(false);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* raid0 IO range */
struct raid_bdev_io_range {
	uint64_t	strip_size;
	uint64_t	start_strip_in_disk;
	uint64_t	end_strip_in_disk;
	uint64_t	start_offset_in_strip;
	uint64_t	end_offset_in_strip;
	uint8_t		start_disk;
	uint8_t		end_disk;
	uint8_t		n_disks_involved;
};

static inline void
_raid0_get_io_range(struct raid_bdev_io_range *io_range,
		    uint8_t num_base_bdevs, uint64_t strip_size, uint64_t strip_size_shift,
		    uint64_t offset_blocks, uint64_t num_blocks)
{
	uint64_t	start_strip;
	uint64_t	end_strip;

	io_range->strip_size = strip_size;

	/* The start and end strip index in raid0 bdev scope */
	start_strip = offset_blocks >> strip_size_shift;
	end_strip = (offset_blocks + num_blocks - 1) >> strip_size_shift;
	io_range->start_strip_in_disk = start_strip / num_base_bdevs;
	io_range->end_strip_in_disk = end_strip / num_base_bdevs;

	/* The first strip may have unaligned start LBA offset.
	 * The end strip may have unaligned end LBA offset.
	 * Strips between them certainly have aligned offset and length to boundaries.
	 */
	io_range->start_offset_in_strip = offset_blocks % strip_size;
	io_range->end_offset_in_strip = (offset_blocks + num_blocks - 1) % strip_size;

	/* The base bdev indexes in which start and end strips are located */
	io_range->start_disk = start_strip % num_base_bdevs;
	io_range->end_disk = end_strip % num_base_bdevs;

	/* Calculate how many base_bdevs are involved in io operation.
	 * Number of base bdevs involved is between 1 and num_base_bdevs.
	 * It will be 1 if the first strip and last strip are the same one.
	 */
	io_range->n_disks_involved = spdk_min((end_strip - start_strip + 1), num_base_bdevs);
}

static inline void
_raid0_split_io_range(struct raid_bdev_io_range *io_range, uint8_t disk_idx,
		      uint64_t *_offset_in_disk, uint64_t *_nblocks_in_disk)
{
	uint64_t n_strips_in_disk;
	uint64_t start_offset_in_disk;
	uint64_t end_offset_in_disk;
	uint64_t offset_in_disk;
	uint64_t nblocks_in_disk;
	uint64_t start_strip_in_disk;
	uint64_t end_strip_in_disk;

	start_strip_in_disk = io_range->start_strip_in_disk;
	if (disk_idx < io_range->start_disk) {
		start_strip_in_disk += 1;
	}

	end_strip_in_disk = io_range->end_strip_in_disk;
	if (disk_idx > io_range->end_disk) {
		end_strip_in_disk -= 1;
	}

	assert(end_strip_in_disk >= start_strip_in_disk);
	n_strips_in_disk = end_strip_in_disk - start_strip_in_disk + 1;

	if (disk_idx == io_range->start_disk) {
		start_offset_in_disk = io_range->start_offset_in_strip;
	} else {
		start_offset_in_disk = 0;
	}

	if (disk_idx == io_range->end_disk) {
		end_offset_in_disk = io_range->end_offset_in_strip;
	} else {
		end_offset_in_disk = io_range->strip_size - 1;
	}

	offset_in_disk = start_offset_in_disk + start_strip_in_disk * io_range->strip_size;
	nblocks_in_disk = (n_strips_in_disk - 1) * io_range->strip_size
			  + end_offset_in_disk - start_offset_in_disk + 1;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID0,
		      "raid_bdev (strip_size 0x%lx) splits IO to base_bdev (%u) at (0x%lx, 0x%lx).\n",
		      io_range->strip_size, disk_idx, offset_in_disk, nblocks_in_disk);

	*_offset_in_disk = offset_in_disk;
	*_nblocks_in_disk = nblocks_in_disk;
}

static void
raid0_submit_null_payload_request(struct raid_bdev_io *raid_io);

static void
_raid0_submit_null_payload_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid0_submit_null_payload_request(raid_io);
}

/*
 * brief:
 * raid0_submit_null_payload_request function submits the next batch of
 * io requests with range but without payload, like FLUSH and UNMAP, to member disks;
 * it will submit as many as possible unless one base io request fails with -ENOMEM,
 * in which case it will queue itself for later submission.
 * params:
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
raid0_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	struct spdk_bdev_io		*bdev_io;
	struct raid_bdev		*raid_bdev;
	struct raid_bdev_io_range	io_range;
	int				ret;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	bdev_io = spdk_bdev_io_from_ctx(raid_io);
	raid_bdev = raid_io->raid_bdev;

	_raid0_get_io_range(&io_range, raid_bdev->num_base_bdevs,
			    raid_bdev->strip_size, raid_bdev->strip_size_shift,
			    bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks);

	raid_io->base_bdev_io_expected = io_range.n_disks_involved;

	while (raid_io->base_bdev_io_submitted < raid_io->base_bdev_io_expected) {
		uint8_t disk_idx;
		uint64_t offset_in_disk;
		uint64_t nblocks_in_disk;

		/* base_bdev is started from start_disk to end_disk.
		 * It is possible that index of start_disk is larger than end_disk's.
		 */
		disk_idx = (io_range.start_disk + raid_io->base_bdev_io_submitted) % raid_bdev->num_base_bdevs;
		base_info = &raid_bdev->base_bdev_info[disk_idx];
		base_ch = raid_io->raid_ch->base_channel[disk_idx];

		_raid0_split_io_range(&io_range, disk_idx, &offset_in_disk, &nblocks_in_disk);

		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			ret = spdk_bdev_unmap_blocks(base_info->desc, base_ch,
						     offset_in_disk, nblocks_in_disk,
						     raid_bdev_base_io_completion, raid_io);
			break;

		case SPDK_BDEV_IO_TYPE_FLUSH:
			ret = spdk_bdev_flush_blocks(base_info->desc, base_ch,
						     offset_in_disk, nblocks_in_disk,
						     raid_bdev_base_io_completion, raid_io);
			break;

		default:
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", bdev_io->type);
			assert(false);
			ret = -EIO;
		}

		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, base_info->bdev, base_ch,
						_raid0_submit_null_payload_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

static int raid0_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt;
	uint8_t i;

	min_blockcnt = raid_bdev->base_bdev_info[0].bdev->blockcnt;
	for (i = 1; i < raid_bdev->num_base_bdevs; i++) {
		/* Calculate minimum block count from all base bdevs */
		if (raid_bdev->base_bdev_info[i].bdev->blockcnt < min_blockcnt) {
			min_blockcnt = raid_bdev->base_bdev_info[i].bdev->blockcnt;
		}
	}

	/*
	 * Take the minimum block count based approach where total block count
	 * of raid bdev is the number of base bdev times the minimum block count
	 * of any base bdev.
	 */
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_RAID0, "min blockcount %lu,  numbasedev %u, strip size shift %u\n",
		      min_blockcnt, raid_bdev->num_base_bdevs, raid_bdev->strip_size_shift);
	raid_bdev->bdev.blockcnt = ((min_blockcnt >> raid_bdev->strip_size_shift) <<
				    raid_bdev->strip_size_shift)  * raid_bdev->num_base_bdevs;

	if (raid_bdev->num_base_bdevs > 1) {
		raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
		raid_bdev->bdev.split_on_optimal_io_boundary = true;
	} else {
		/* Do not need to split reads/writes on single bdev RAID modules. */
		raid_bdev->bdev.optimal_io_boundary = 0;
		raid_bdev->bdev.split_on_optimal_io_boundary = false;
	}

	return 0;
}

static struct raid_bdev_module g_raid0_module = {
	.level = RAID0,
	.start = raid0_start,
	.submit_rw_request = raid0_submit_rw_request,
	.submit_null_payload_request = raid0_submit_null_payload_request,
};
RAID_MODULE_REGISTER(&g_raid0_module)

SPDK_LOG_REGISTER_COMPONENT("bdev_raid0", SPDK_LOG_BDEV_RAID0)
