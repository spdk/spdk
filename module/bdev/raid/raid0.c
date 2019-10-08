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
 * cb_arg - function callback context, like parent io pointer
 * returns:
 * none
 */
static void
raid0_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io         *parent_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		spdk_bdev_io_complete(parent_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(parent_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
raid0_waitq_io_process(void *ctx);

/*
 * brief:
 * raid0_submit_rw_request function is used to submit I/O to the correct
 * member disk for raid0 bdevs.
 * params:
 * bdev_io - parent bdev io
 * start_strip - start strip number of this io
 * returns:
 * none
 */
static void
raid0_submit_rw_request(struct spdk_bdev_io *bdev_io, uint64_t start_strip)
{
	struct raid_bdev_io		*raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;
	struct raid_bdev_io_channel	*raid_ch = spdk_io_channel_get_ctx(raid_io->ch);
	struct raid_bdev		*raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	uint64_t			pd_strip;
	uint32_t			offset_in_strip;
	uint64_t			pd_lba;
	uint64_t			pd_blocks;
	uint8_t				pd_idx;
	int				ret = 0;

	pd_strip = start_strip / raid_bdev->num_base_bdevs;
	pd_idx = start_strip % raid_bdev->num_base_bdevs;
	offset_in_strip = bdev_io->u.bdev.offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;
	pd_blocks = bdev_io->u.bdev.num_blocks;
	if (raid_bdev->base_bdev_info[pd_idx].desc == NULL) {
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
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = spdk_bdev_readv_blocks(raid_bdev->base_bdev_info[pd_idx].desc,
					     raid_ch->base_channel[pd_idx],
					     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     pd_lba, pd_blocks, raid0_bdev_io_completion,
					     bdev_io);
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		ret = spdk_bdev_writev_blocks(raid_bdev->base_bdev_info[pd_idx].desc,
					      raid_ch->base_channel[pd_idx],
					      bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					      pd_lba, pd_blocks, raid0_bdev_io_completion,
					      bdev_io);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", bdev_io->type);
		assert(0);
	}

	if (ret) {
		raid_bdev_queue_io_wait(bdev_io, pd_idx, raid0_waitq_io_process, ret);
	}
}

/*
 * brief:
 * raid0_waitq_io_process function is the callback function
 * registered by raid bdev module to bdev when bdev_io was unavailable
 * for raid0 bdevs.
 * params:
 * ctx - pointer to raid_bdev_io
 * returns:
 * none
 */
static void
raid0_waitq_io_process(void *ctx)
{
	struct spdk_bdev_io     *bdev_io = ctx;
	struct raid_bdev	*raid_bdev;
	uint64_t		start_strip;

	/*
	 * Try to submit childs of parent bdev io. If failed due to resource
	 * crunch then break the loop and don't try to process other queued IOs.
	 */
	raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	start_strip = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	raid0_submit_rw_request(bdev_io, start_strip);
}

/*
 * brief:
 * raid0_start_rw_request function is the submit_request function for
 * read/write requests for raid0 bdevs.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
void
raid0_start_rw_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct raid_bdev_io		*raid_io;
	struct raid_bdev		*raid_bdev;
	uint64_t			start_strip = 0;
	uint64_t			end_strip = 0;

	raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;
	raid_io->ch = ch;
	start_strip = bdev_io->u.bdev.offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) >>
		    raid_bdev->strip_size_shift;
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 1) {
		assert(false);
		SPDK_ERRLOG("I/O spans strip boundary!\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	raid0_submit_rw_request(bdev_io, start_strip);
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
void
raid0_submit_null_payload_request(void *_bdev_io)
{
	struct spdk_bdev_io		*bdev_io = _bdev_io;
	struct raid_bdev_io		*raid_io;
	struct raid_bdev		*raid_bdev;
	struct raid_bdev_io_channel	*raid_ch;
	struct raid_bdev_io_range	io_range;
	int				ret;

	raid_bdev = (struct raid_bdev *)bdev_io->bdev->ctxt;
	raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;
	raid_ch = spdk_io_channel_get_ctx(raid_io->ch);

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

		_raid0_split_io_range(&io_range, disk_idx, &offset_in_disk, &nblocks_in_disk);

		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			ret = spdk_bdev_unmap_blocks(raid_bdev->base_bdev_info[disk_idx].desc,
						     raid_ch->base_channel[disk_idx],
						     offset_in_disk, nblocks_in_disk,
						     raid_bdev_base_io_completion, bdev_io);
			break;

		case SPDK_BDEV_IO_TYPE_FLUSH:
			ret = spdk_bdev_flush_blocks(raid_bdev->base_bdev_info[disk_idx].desc,
						     raid_ch->base_channel[disk_idx],
						     offset_in_disk, nblocks_in_disk,
						     raid_bdev_base_io_completion, bdev_io);
			break;

		default:
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", bdev_io->type);
			assert(false);
			ret = -EIO;
		}

		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else {
			raid_bdev_queue_io_wait(bdev_io, disk_idx,
						raid0_submit_null_payload_request, ret);
			return;
		}
	}
}

SPDK_LOG_REGISTER_COMPONENT("bdev_raid0", SPDK_LOG_BDEV_RAID0)
