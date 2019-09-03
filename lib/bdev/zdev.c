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

#include "spdk/zdev.h"
#include "spdk/zdev_module.h"

#include "bdev_internal.h"

size_t
spdk_bdev_get_zone_size(const struct spdk_zdev *zdev)
{
	return zdev->zone_size;
}

size_t
spdk_bdev_get_max_open_zones(const struct spdk_zdev *zdev)
{
	return zdev->max_open_zones;
}

size_t
spdk_bdev_get_optimal_open_zones(const struct spdk_zdev *zdev)
{
	return zdev->optimal_open_zones;
}

int
spdk_bdev_get_zone_info(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			uint64_t zone_id, size_t num_zones, struct spdk_zdev_zone_info *info,
			spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io = spdk_bdev_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT;
	bdev_io->u.zdev.zone_action = SPDK_ZDEV_ZONE_INFO;
	bdev_io->u.zdev.zone_id = zone_id;
	bdev_io->u.zdev.num_zones = num_zones;
	bdev_io->u.zdev.info_buf = info;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

static int
bdev_zone_management(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, uint64_t zone_id,
		     enum spdk_zdev_zone_action action,  spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct spdk_bdev_io *bdev_io;
	struct spdk_bdev_channel *channel = spdk_io_channel_get_ctx(ch);

	bdev_io = spdk_bdev_get_io(channel);
	if (!bdev_io) {
		return -ENOMEM;
	}

	bdev_io->internal.ch = channel;
	bdev_io->internal.desc = desc;
	bdev_io->type = SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT;
	bdev_io->u.zdev.zone_action = action;
	bdev_io->u.zdev.zone_id = zone_id;
	spdk_bdev_io_init(bdev_io, bdev, cb_arg, cb);

	spdk_bdev_io_submit(bdev_io);
	return 0;
}

int
spdk_bdev_zone_open(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    uint64_t zone_id, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_zone_management(desc, ch, zone_id, SPDK_ZDEV_ZONE_OPEN, cb, cb_arg);
}

int
spdk_bdev_zone_finish(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      uint64_t zone_id, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return bdev_zone_management(desc, ch, zone_id, SPDK_ZDEV_ZONE_FINISH, cb, cb_arg);
}
