/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
blob_bs_dev_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt,
		       uint64_t lba, uint32_t lba_count,
		       struct spdk_bs_dev_cb_args *cb_args,
		       struct spdk_blob_ext_io_opts *ext_opts)
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

static inline void
blob_bs_dev_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		      struct iovec *iov, int iovcnt,
		      uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
		      struct spdk_blob_ext_io_opts *ext_opts)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;

	spdk_blob_io_readv_ext(b->blob, channel, iov, iovcnt, lba, lba_count,
			       blob_bs_dev_read_cpl, cb_args, ext_opts);
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

static bool
blob_bs_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	struct spdk_blob *blob = b->blob;

	assert(lba == bs_cluster_to_lba(blob->bs, bs_lba_to_cluster(blob->bs, lba)));
	assert(lba_count == bs_dev_byte_to_lba(dev, blob->bs->cluster_sz));

	if (bs_io_unit_is_allocated(blob, lba)) {
		return false;
	}

	assert(blob->back_bs_dev != NULL);
	return blob->back_bs_dev->is_zeroes(blob->back_bs_dev,
					    bs_io_unit_to_back_dev_lba(blob, lba),
					    bs_io_unit_to_back_dev_lba(blob, lba_count));
}

static bool
blob_bs_translate_lba(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	struct spdk_blob *blob = b->blob;

	assert(base_lba != NULL);
	if (bs_io_unit_is_allocated(blob, lba)) {
		*base_lba = bs_blob_io_unit_to_lba(blob, lba);
		return true;
	}

	assert(blob->back_bs_dev != NULL);
	return blob->back_bs_dev->translate_lba(blob->back_bs_dev,
						bs_io_unit_to_back_dev_lba(blob, lba),
						base_lba);
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
	b->bs_dev.writev_ext = blob_bs_dev_writev_ext;
	b->bs_dev.read = blob_bs_dev_read;
	b->bs_dev.readv = blob_bs_dev_readv;
	b->bs_dev.readv_ext = blob_bs_dev_readv_ext;
	b->bs_dev.write_zeroes = blob_bs_dev_write_zeroes;
	b->bs_dev.unmap = blob_bs_dev_unmap;
	b->bs_dev.is_zeroes = blob_bs_is_zeroes;
	b->bs_dev.translate_lba = blob_bs_translate_lba;
	b->blob = blob;

	return &b->bs_dev;
}
