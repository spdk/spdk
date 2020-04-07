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
zeroes_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		    uint64_t lba, uint32_t lba_count,
		    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	     uint64_t lba, uint32_t lba_count,
	     struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
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
	.write_zeroes = zeroes_write_zeroes,
	.unmap = zeroes_unmap,
};

struct spdk_bs_dev *
bs_create_zeroes_dev(void)
{
	return &g_zeroes_bs_dev;
}
