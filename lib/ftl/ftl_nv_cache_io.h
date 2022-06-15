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

#ifndef FTL_NV_CACHE_IO_H
#define FTL_NV_CACHE_IO_H

#include "spdk/bdev.h"
#include "ftl_core.h"

static inline int
ftl_nv_cache_bdev_readv_blocks_with_md(struct spdk_ftl_dev *dev,
				       struct spdk_bdev_desc *desc,
				       struct spdk_io_channel *ch,
				       struct iovec *iov, int iovcnt, void *md,
				       uint64_t offset_blocks, uint64_t num_blocks,
				       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_readv_blocks_with_md(desc, ch, iov, iovcnt, md,
					      offset_blocks, num_blocks,
					      cb, cb_arg);
}

static inline int
ftl_nv_cache_bdev_writev_blocks_with_md(struct spdk_ftl_dev *dev,
					struct spdk_bdev_desc *desc,
					struct spdk_io_channel *ch,
					struct iovec *iov, int iovcnt, void *md_buf,
					uint64_t offset_blocks, uint64_t num_blocks,
					spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_writev_blocks_with_md(desc, ch, iov, iovcnt, md_buf,
					       offset_blocks, num_blocks, cb,
					       cb_arg);
}

static inline int
ftl_nv_cache_bdev_read_blocks_with_md(struct spdk_ftl_dev *dev,
				      struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch,
				      void *buf, void *md,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_read_blocks_with_md(desc, ch, buf, md ?: g_ftl_tmp_buf,
					     offset_blocks, num_blocks,
					     cb, cb_arg);
}

static inline int
ftl_nv_cache_bdev_write_blocks_with_md(struct spdk_ftl_dev *dev,
				       struct spdk_bdev_desc *desc,
				       struct spdk_io_channel *ch,
				       void *buf, void *md,
				       uint64_t offset_blocks, uint64_t num_blocks,
				       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	return spdk_bdev_write_blocks_with_md(desc, ch, buf, md ? : g_ftl_zero_buf,
					      offset_blocks, num_blocks,
					      cb, cb_arg);
}

#endif /* FTL_NV_CACHE_IO_H */
