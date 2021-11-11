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

#include "spdk/stdinc.h"

#include "spdk/bdev_zone.h"
#include "spdk/bdev_module.h"
#include "spdk/likely.h"

#include "bdev_internal.h"

uint64_t
spdk_bdev_get_zone_size(const struct spdk_bdev *bdev)
{
	return bdev->zone_size;
}

uint64_t
spdk_bdev_get_num_zones(const struct spdk_bdev *bdev)
{
	return bdev->zone_size ? bdev->blockcnt / bdev->zone_size : 0;
}

uint64_t
spdk_bdev_get_zone_id(const struct spdk_bdev *bdev, uint64_t offset_blocks)
{
	uint64_t zslba;

	if (spdk_likely(spdk_u64_is_pow2(bdev->zone_size))) {
		uint64_t zone_mask = bdev->zone_size - 1;
		zslba = offset_blocks & ~zone_mask;
	} else {
		/* integer division */
		zslba = (offset_blocks / bdev->zone_size) * bdev->zone_size;
	}

	return zslba;
}

uint32_t
spdk_bdev_get_max_zone_append_size(const struct spdk_bdev *bdev)
{
	return bdev->max_zone_append_size;
}

uint32_t
spdk_bdev_get_max_open_zones(const struct spdk_bdev *bdev)
{
	return bdev->max_open_zones;
}

uint32_t
spdk_bdev_get_max_active_zones(const struct spdk_bdev *bdev)
{
	return bdev->max_active_zones;
}

uint32_t
spdk_bdev_get_optimal_open_zones(const struct spdk_bdev *bdev)
{
	return bdev->optimal_open_zones;
}

int
spdk_bdev_get_zone_info(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t zone_id, size_t num_zones, struct spdk_bdev_zone_info *info,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_GET_ZONE_INFO;
	bdev_io->u.zone_mgmt.zone_id = zone_id;
	bdev_io->u.zone_mgmt.num_zones = num_zones;
	bdev_io->u.zone_mgmt.buf = info;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_zone_management(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  uint64_t zone_id, enum spdk_bdev_zone_action action,
			  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT;
	bdev_io->u.zone_mgmt.zone_action = action;
	bdev_io->u.zone_mgmt.zone_id = zone_id;
	bdev_io->u.zone_mgmt.num_zones = 1;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

static int
zone_bdev_append_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			 void *buf, void *md_buf, uint64_t zone_id, uint64_t num_blocks,
			 spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_APPEND;
	bdev_io->u.bdev.iovs = &bdev_io->iov;
	bdev_io->u.bdev.iovs[0].iov_base = buf;
	bdev_io->u.bdev.iovs[0].iov_len = num_blocks * bdev->blocklen;
	bdev_io->u.bdev.iovcnt = 1;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = zone_id;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_zone_append(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t start_lba, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return zone_bdev_append_with_md(desc, ch, buf, NULL, start_lba, num_blocks,
					cb, cb_arg);
}

int
spdk_bdev_zone_append_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			      void *buf, void *md, uint64_t start_lba, uint64_t num_blocks,
			      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return zone_bdev_append_with_md(desc, ch, buf, md, start_lba, num_blocks,
					cb, cb_arg);
}

int
spdk_bdev_zone_appendv_with_md(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			       struct iovec *iov, int iovcnt, void *md_buf, uint64_t zone_id,
			       uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
			       void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io = bdev_channel_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_APPEND;
	bdev_io->u.bdev.iovs = iov;
	bdev_io->u.bdev.iovcnt = iovcnt;
	bdev_io->u.bdev.md_buf = md_buf;
	bdev_io->u.bdev.num_blocks = num_blocks;
	bdev_io->u.bdev.offset_blocks = zone_id;
	bdev_io_init(bdev_io, bdev, cb_arg, cb);

	bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_zone_appendv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       struct iovec *iovs, int iovcnt, uint64_t zone_id, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_zone_appendv_with_md(desc, ch, iovs, iovcnt, NULL, zone_id, num_blocks,
					      cb, cb_arg);
}

uint64_t
spdk_bdev_io_get_append_location(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.offset_blocks;
}
