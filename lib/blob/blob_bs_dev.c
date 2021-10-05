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
#include "spdk/blob.h"
#include "spdk/log.h"
#include "blobstore.h"

static void
blob_bs_dev_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		  uint64_t lba, uint32_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
blob_bs_dev_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt,
		   uint64_t lba, uint32_t lba_count,
		   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
blob_bs_dev_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			 uint64_t lba, uint64_t lba_count,
			 struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
blob_bs_dev_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  uint64_t lba, uint64_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
blob_bs_dev_read_cpl(void *cb_arg, int bserrno)
{
	struct spdk_bs_dev_cb_args *cb_args = (struct spdk_bs_dev_cb_args *)cb_arg;

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, bserrno);
}

static inline void
blob_bs_dev_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;

	spdk_blob_io_read(b->blob, channel, payload, lba, lba_count,
			  blob_bs_dev_read_cpl, cb_args);
}

static inline void
blob_bs_dev_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  struct iovec *iov, int iovcnt,
		  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;

	spdk_blob_io_readv(b->blob, channel, iov, iovcnt, lba, lba_count,
			   blob_bs_dev_read_cpl, cb_args);
}

static void
blob_bs_dev_destroy_cpl(void *cb_arg, int bserrno)
{
	if (bserrno != 0) {
		SPDK_ERRLOG("Error on blob_bs_dev destroy: %d", bserrno);
	}

	/* Free blob_bs_dev */
	free(cb_arg);
}

static void
blob_bs_dev_destroy(struct spdk_bs_dev *bs_dev)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)bs_dev;

	spdk_blob_close(b->blob, blob_bs_dev_destroy_cpl, b);
}


struct spdk_bs_dev *
bs_create_blob_bs_dev(struct spdk_blob *blob)
{
	struct spdk_blob_bs_dev  *b;

	b = calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	/* snapshot blob */
	b->bs_dev.blockcnt = blob->active.num_clusters *
			     blob->bs->pages_per_cluster * bs_io_unit_per_page(blob->bs);
	b->bs_dev.blocklen = spdk_bs_get_io_unit_size(blob->bs);
	b->bs_dev.create_channel = NULL;
	b->bs_dev.destroy_channel = NULL;
	b->bs_dev.destroy = blob_bs_dev_destroy;
	b->bs_dev.write = blob_bs_dev_write;
	b->bs_dev.writev = blob_bs_dev_writev;
	b->bs_dev.read = blob_bs_dev_read;
	b->bs_dev.readv = blob_bs_dev_readv;
	b->bs_dev.write_zeroes = blob_bs_dev_write_zeroes;
	b->bs_dev.unmap = blob_bs_dev_unmap;
	b->blob = blob;

	return &b->bs_dev;
}
