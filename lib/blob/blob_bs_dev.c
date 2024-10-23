/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/log.h"
#include "spdk/likely.h"
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
zero_trailing_bytes(struct spdk_blob_bs_dev *b, struct iovec *iov, int iovcnt,
		    uint64_t lba, uint32_t *lba_count)
{
	uint32_t zero_lba_count;
	uint64_t zero_bytes, zero_len;
	uint64_t payload_bytes;
	uint64_t valid_bytes;
	void *zero_start;
	struct iovec *i;

	if (spdk_likely(lba + *lba_count <= b->bs_dev.blockcnt)) {
		return;
	}

	/* Figure out how many bytes in the payload will need to be zeroed. */
	zero_lba_count = spdk_min(*lba_count, lba + *lba_count - b->bs_dev.blockcnt);
	zero_bytes = zero_lba_count * (uint64_t)b->bs_dev.blocklen;

	payload_bytes = *lba_count * (uint64_t)b->bs_dev.blocklen;
	valid_bytes = payload_bytes - zero_bytes;

	i = iov;
	while (zero_bytes > 0) {
		if (i->iov_len > valid_bytes) {
			zero_start = i->iov_base + valid_bytes;
			zero_len = spdk_min(payload_bytes, i->iov_len - valid_bytes);
			memset(zero_start, 0, zero_bytes);
			valid_bytes = 0;
			zero_bytes -= zero_len;
		}
		valid_bytes -= spdk_min(valid_bytes, i->iov_len);
		payload_bytes -= spdk_min(payload_bytes, i->iov_len);
		i++;
	}

	*lba_count -= zero_lba_count;
}

static inline void
blob_bs_dev_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	struct iovec iov;

	iov.iov_base = payload;
	iov.iov_len = lba_count * b->bs_dev.blocklen;
	/* The backing blob may be smaller than this blob, so zero any trailing bytes. */
	zero_trailing_bytes(b, &iov, 1, lba, &lba_count);

	spdk_blob_io_read(b->blob, channel, payload, lba, lba_count,
			  blob_bs_dev_read_cpl, cb_args);
}

static inline void
blob_bs_dev_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  struct iovec *iov, int iovcnt,
		  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;

	/* The backing blob may be smaller than this blob, so zero any trailing bytes. */
	zero_trailing_bytes(b, iov, iovcnt, lba, &lba_count);

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

	/* The backing blob may be smaller than this blob, so zero any trailing bytes. */
	zero_trailing_bytes(b, iov, iovcnt, lba, &lba_count);

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
	bool is_valid_range;

	assert(lba == bs_cluster_to_lba(blob->bs, bs_lba_to_cluster(blob->bs, lba)));
	assert(lba_count == bs_dev_byte_to_lba(dev, blob->bs->cluster_sz));

	if (bs_io_unit_is_allocated(blob, lba)) {
		return false;
	}

	assert(blob->back_bs_dev != NULL);
	is_valid_range = blob->back_bs_dev->is_range_valid(blob->back_bs_dev, lba, lba_count);
	return is_valid_range && blob->back_bs_dev->is_zeroes(blob->back_bs_dev,
			bs_io_unit_to_back_dev_lba(blob, lba),
			bs_io_unit_to_back_dev_lba(blob, lba_count));
}

static bool
blob_bs_is_range_valid(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	struct spdk_blob *blob = b->blob;
	uint64_t	io_units_per_cluster;

	/* The lba here is supposed to be the first lba of cluster. lba_count
	 * will typically be fixed e.g. 8192 for 4MiB cluster. */
	assert(lba_count == blob->bs->cluster_sz / dev->blocklen);
	assert(lba % lba_count == 0);

	io_units_per_cluster = blob->bs->io_units_per_cluster;

	/* A blob will either have:
	* - no backing bs_bdev (normal thick blob), or
	* - zeroes backing bs_bdev (thin provisioned blob), or
	* - blob backing bs_bdev (e.g snapshot)
	* It may be possible that backing bs_bdev has lesser number of clusters
	* than the child lvol blob because lvol blob has been expanded after
	* taking snapshot. In such a case, page will be outside the cluster io_unit
	* range of the backing dev. Always return true for zeroes backing bdev. */
	return lba < blob->active.num_clusters * io_units_per_cluster;
}

static bool
blob_bs_translate_lba(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;
	struct spdk_blob *blob = b->blob;
	bool is_valid_range;

	assert(base_lba != NULL);
	if (bs_io_unit_is_allocated(blob, lba)) {
		*base_lba = bs_blob_io_unit_to_lba(blob, lba);
		return true;
	}

	assert(blob->back_bs_dev != NULL);
	/* Since here we don't get lba_count directly, passing lba_count derived
	 * from cluster_sz which typically happens for other calls like is_zeroes
	 * in CoW path. */
	is_valid_range = blob->back_bs_dev->is_range_valid(blob->back_bs_dev, lba,
			 bs_dev_byte_to_lba(blob->back_bs_dev, blob->bs->cluster_sz));
	return is_valid_range && blob->back_bs_dev->translate_lba(blob->back_bs_dev,
			bs_io_unit_to_back_dev_lba(blob, lba),
			base_lba);
}

static bool
blob_bs_is_degraded(struct spdk_bs_dev *dev)
{
	struct spdk_blob_bs_dev *b = (struct spdk_blob_bs_dev *)dev;

	return spdk_blob_is_degraded(b->blob);
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
	b->bs_dev.blockcnt = blob->active.num_clusters * blob->bs->io_units_per_cluster;
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
	b->bs_dev.is_range_valid = blob_bs_is_range_valid;
	b->bs_dev.translate_lba = blob_bs_translate_lba;
	b->bs_dev.is_degraded = blob_bs_is_degraded;
	b->blob = blob;

	return &b->bs_dev;
}
