/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/dma.h"

#include "blobstore.h"

static void
zeroes_destroy(struct spdk_bs_dev *bs_dev)
{
	return;
}

static void
zeroes_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	    uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	memset(payload, 0, dev->blocklen * lba_count);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
zeroes_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	     uint64_t lba, uint32_t lba_count,
	     struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	     struct iovec *iov, int iovcnt,
	     uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		memset(iov[i].iov_base, 0, iov[i].iov_len);
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
zeroes_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	      struct iovec *iov, int iovcnt,
	      uint64_t lba, uint32_t lba_count,
	      struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
_read_memory_domain_memzero_done(void *ctx, int rc)
{
	struct spdk_bs_dev_cb_args *cb_args = (struct spdk_bs_dev_cb_args *)ctx;

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
}

static void
zeroes_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		 struct iovec *iov, int iovcnt,
		 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
		 struct spdk_blob_ext_io_opts *ext_io_opts)
{
	int i, rc;

	if (ext_io_opts->memory_domain) {
		rc = spdk_memory_domain_memzero(ext_io_opts->memory_domain, ext_io_opts->memory_domain_ctx, iov,
						iovcnt, _read_memory_domain_memzero_done, cb_args);
		if (rc) {
			cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
		}
		return;
	}

	for (i = 0; i < iovcnt; i++) {
		memset(iov[i].iov_base, 0, iov[i].iov_len);
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
zeroes_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  struct iovec *iov, int iovcnt,
		  uint64_t lba, uint32_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args,
		  struct spdk_blob_ext_io_opts *ext_io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		    uint64_t lba, uint64_t lba_count,
		    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	     uint64_t lba, uint64_t lba_count,
	     struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static bool
zeroes_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return true;
}

static bool
zeroes_translate_lba(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba)
{
	return false;
}

static struct spdk_bs_dev g_zeroes_bs_dev = {
	.blockcnt = UINT64_MAX,
	.blocklen = 512,
	.create_channel = NULL,
	.destroy_channel = NULL,
	.destroy = zeroes_destroy,
	.read = zeroes_read,
	.write = zeroes_write,
	.readv = zeroes_readv,
	.writev = zeroes_writev,
	.readv_ext = zeroes_readv_ext,
	.writev_ext = zeroes_writev_ext,
	.write_zeroes = zeroes_write_zeroes,
	.unmap = zeroes_unmap,
	.is_zeroes = zeroes_is_zeroes,
	.translate_lba = zeroes_translate_lba,
};

struct spdk_bs_dev *
bs_create_zeroes_dev(void)
{
	return &g_zeroes_bs_dev;
}
