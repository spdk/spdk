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
#include "spdk/io_channel.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include "spdk_internal/bdev.h"

struct blob_bdev {
	struct spdk_bs_dev	bs_dev;
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;
	bool			claimed;
};

static inline struct spdk_bdev_desc *
__get_desc(struct spdk_bs_dev *dev)
{
	return ((struct blob_bdev *)dev)->desc;
}

static void
bdev_blob_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct spdk_bs_dev_cb_args *cb_args = arg;
	int bserrno;

	if (success) {
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
	int rc;

	rc = spdk_bdev_read_blocks(__get_desc(dev), channel, payload, lba,
				   lba_count, bdev_blob_io_complete, cb_args);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_write_blocks(__get_desc(dev), channel, payload, lba,
				    lba_count, bdev_blob_io_complete, cb_args);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}


static void
bdev_blob_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		struct iovec *iov, int iovcnt,
		uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_readv_blocks(__get_desc(dev), channel, iov, iovcnt, lba,
				    lba_count, bdev_blob_io_complete, cb_args);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		 struct iovec *iov, int iovcnt,
		 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_writev_blocks(__get_desc(dev), channel, iov, iovcnt, lba,
				     lba_count, bdev_blob_io_complete, cb_args);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, uint64_t lba,
		       uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_write_zeroes_blocks(__get_desc(dev), channel, lba,
					   lba_count, bdev_blob_io_complete, cb_args);
	if (rc) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, uint64_t lba,
		uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct blob_bdev *blob_bdev = (struct blob_bdev *)dev;
	int rc;

	if (spdk_bdev_io_type_supported(blob_bdev->bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		rc = spdk_bdev_unmap_blocks(__get_desc(dev), channel, lba, lba_count,
					    bdev_blob_io_complete, cb_args);
		if (rc) {
			cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
		}
	} else {
		/*
		 * If the device doesn't support unmap, immediately complete
		 * the request. Blobstore does not rely on unmap zeroing
		 * data.
		 */
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
	}
}

int
spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module)
{
	struct blob_bdev *blob_bdev = (struct blob_bdev *)bs_dev;
	int rc;

	rc = spdk_bdev_module_claim_bdev(blob_bdev->bdev, NULL, module);
	if (rc != 0) {
		SPDK_ERRLOG("could not claim bs dev\n");
		return rc;
	}

	blob_bdev->claimed = true;

	return rc;
}

static struct spdk_io_channel *
bdev_blob_create_channel(struct spdk_bs_dev *dev)
{
	struct blob_bdev *blob_bdev = (struct blob_bdev *)dev;

	return spdk_bdev_get_io_channel(blob_bdev->desc);
}

static void
bdev_blob_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

static void
bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	struct spdk_bdev_desc *desc = __get_desc(bs_dev);
	struct blob_bdev *blob_bdev = (struct blob_bdev *)bs_dev;

	if (blob_bdev->claimed) {
		spdk_bdev_module_release_bdev(blob_bdev->bdev);
	}

	spdk_bdev_close(desc);
	free(bs_dev);
}

struct spdk_bs_dev *
spdk_bdev_create_bs_dev(struct spdk_bdev *bdev, spdk_bdev_remove_cb_t remove_cb, void *remove_ctx)
{
	struct blob_bdev *b;
	struct spdk_bdev_desc *desc;
	int rc;

	b = calloc(1, sizeof(*b));

	if (b == NULL) {
		SPDK_ERRLOG("could not allocate blob_bdev\n");
		return NULL;
	}

	rc = spdk_bdev_open(bdev, true, remove_cb, remove_ctx, &desc);
	if (rc != 0) {
		free(b);
		return NULL;
	}

	b->bdev = bdev;
	b->desc = desc;
	b->bs_dev.blockcnt = spdk_bdev_get_num_blocks(bdev);
	b->bs_dev.blocklen = spdk_bdev_get_block_size(bdev);
	b->bs_dev.create_channel = bdev_blob_create_channel;
	b->bs_dev.destroy_channel = bdev_blob_destroy_channel;
	b->bs_dev.destroy = bdev_blob_destroy;
	b->bs_dev.read = bdev_blob_read;
	b->bs_dev.write = bdev_blob_write;
	b->bs_dev.readv = bdev_blob_readv;
	b->bs_dev.writev = bdev_blob_writev;
	b->bs_dev.write_zeroes = bdev_blob_write_zeroes;
	b->bs_dev.unmap = bdev_blob_unmap;

	return &b->bs_dev;
}
