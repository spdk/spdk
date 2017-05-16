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

#include "spdk/blob_bdev.h"
#include "spdk/blob.h"
#include "spdk/bdev.h"
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/endian.h"

struct blob_bdev {
	struct spdk_bs_dev	bs_dev;
	struct spdk_bdev	*bdev;
};

static inline struct spdk_bdev *
__get_bdev(struct spdk_bs_dev *dev)
{
	return ((struct blob_bdev *)dev)->bdev;
}

static void
bdev_blob_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status, void *arg)
{
	struct spdk_bs_dev_cb_args *cb_args = arg;
	int bserrno;

	if (status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		bserrno = 0;
	} else {
		bserrno = -EIO;
	}
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, bserrno);
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_blob_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	       uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_bdev *bdev = __get_bdev(dev);
	struct spdk_bdev_io *bdev_io;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	bdev_io = spdk_bdev_read(bdev, channel, payload, lba * block_size,
				 lba_count * block_size, bdev_blob_io_complete, cb_args);
	if (bdev_io == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
	}
}

static void
bdev_blob_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_bdev *bdev = __get_bdev(dev);
	struct spdk_bdev_io *bdev_io;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	bdev_io = spdk_bdev_write(bdev, channel, payload, lba * block_size,
				  lba_count * block_size, bdev_blob_io_complete, cb_args);
	if (bdev_io == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
	}
}

static void
bdev_blob_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, uint64_t lba,
		uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_bdev *bdev = __get_bdev(dev);
	struct spdk_scsi_unmap_bdesc *desc;
	struct spdk_bdev_io *bdev_io;

	SPDK_STATIC_ASSERT(sizeof(cb_args->scratch) >= sizeof(*desc), "scratch too small");

	desc = (struct spdk_scsi_unmap_bdesc *)cb_args->scratch;
	to_be64(&desc->lba, lba);
	to_be32(&desc->block_count, lba_count);
	desc->reserved = 0;

	bdev_io = spdk_bdev_unmap(bdev, channel, desc, 1, bdev_blob_io_complete, cb_args);
	if (bdev_io == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
	}
}

static struct spdk_io_channel *
bdev_blob_create_channel(struct spdk_bs_dev *dev)
{
	struct spdk_bdev *bdev = __get_bdev(dev);

	return spdk_bdev_get_io_channel(bdev, SPDK_IO_PRIORITY_DEFAULT);
}

static void
bdev_blob_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

static void
bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	free(bs_dev);
}

struct spdk_bs_dev *
spdk_bdev_create_bs_dev(struct spdk_bdev *bdev)
{
	struct blob_bdev *b;

	b = calloc(1, sizeof(*b));

	if (b == NULL) {
		SPDK_ERRLOG("could not allocate blob_bdev\n");
		return NULL;
	}

	b->bdev = bdev;
	b->bs_dev.blockcnt = spdk_bdev_get_num_blocks(bdev);
	b->bs_dev.blocklen = spdk_bdev_get_block_size(bdev);
	b->bs_dev.create_channel = bdev_blob_create_channel;
	b->bs_dev.destroy_channel = bdev_blob_destroy_channel;
	b->bs_dev.destroy = bdev_blob_destroy;
	b->bs_dev.read = bdev_blob_read;
	b->bs_dev.write = bdev_blob_write;
	b->bs_dev.unmap = bdev_blob_unmap;

	return &b->bs_dev;
}
