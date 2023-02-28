/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_NV_CACHE_IO_H
#define FTL_NV_CACHE_IO_H

#include "spdk/bdev.h"
#include "ftl_core.h"

#ifndef SPDK_FTL_VSS_EMU

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
	return spdk_bdev_read_blocks_with_md(desc, ch, buf, md ? : g_ftl_read_buf,
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
	return spdk_bdev_write_blocks_with_md(desc, ch, buf, md ? : g_ftl_write_buf,
					      offset_blocks, num_blocks,
					      cb, cb_arg);
}

#else

/* TODO: Maybe we can add a non-power-fail-safe support for VSS in AIO bdev and get rid of this */
static inline void
ftl_nv_cache_bdev_get_md(struct spdk_ftl_dev *dev,
			 uint64_t offset_blocks, uint64_t num_blocks,
			 void *md_buf)
{
	struct ftl_md *vss = dev->layout.md[FTL_LAYOUT_REGION_TYPE_VSS];
	union ftl_md_vss *src;
	union ftl_md_vss *dst = md_buf;
	union ftl_md_vss *dst_end = dst + num_blocks;

	assert(offset_blocks + num_blocks <= dev->layout.nvc.total_blocks);

	if (!md_buf) {
		return;
	}

	src = ftl_md_get_buffer(vss);
	src += offset_blocks;
	while (dst < dst_end) {
		*dst = *src;
		dst++;
		src++;
	}
}

static inline int
ftl_nv_cache_bdev_readv_blocks_with_md(struct spdk_ftl_dev *dev,
				       struct spdk_bdev_desc *desc,
				       struct spdk_io_channel *ch,
				       struct iovec *iov, int iovcnt, void *md,
				       uint64_t offset_blocks, uint64_t num_blocks,
				       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	assert(desc == dev->nv_cache.bdev_desc);
	ftl_nv_cache_bdev_get_md(dev, offset_blocks, num_blocks, md);
	return spdk_bdev_readv_blocks(desc, ch, iov, iovcnt, offset_blocks,
				      num_blocks, cb, cb_arg);
}

static inline void
ftl_nv_cache_bdev_set_md(struct spdk_ftl_dev *dev,
			 uint64_t offset_blocks, uint64_t num_blocks,
			 void *md_buf)
{
	struct ftl_md *vss = dev->layout.md[FTL_LAYOUT_REGION_TYPE_VSS];
	union ftl_md_vss *src = md_buf;
	union ftl_md_vss *src_end = src + num_blocks;
	union ftl_md_vss *dst;

	assert(offset_blocks + num_blocks <= dev->layout.nvc.total_blocks);

	if (!md_buf) {
		return;
	}

	dst = ftl_md_get_buffer(vss);
	dst += offset_blocks;
	while (src < src_end) {
		*dst = *src;
		dst++;
		src++;
	}
}

static inline int
ftl_nv_cache_bdev_writev_blocks_with_md(struct spdk_ftl_dev *dev,
					struct spdk_bdev_desc *desc,
					struct spdk_io_channel *ch,
					struct iovec *iov, int iovcnt, void *md_buf,
					uint64_t offset_blocks, uint64_t num_blocks,
					spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	assert(desc == dev->nv_cache.bdev_desc);
	ftl_nv_cache_bdev_set_md(dev, offset_blocks, num_blocks, md_buf);
	return spdk_bdev_writev_blocks(desc, ch, iov, iovcnt,
				       offset_blocks, num_blocks,
				       cb, cb_arg);
}

static inline int
ftl_nv_cache_bdev_read_blocks_with_md(struct spdk_ftl_dev *dev,
				      struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch,
				      void *buf, void *md,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	assert(desc == dev->nv_cache.bdev_desc);
	ftl_nv_cache_bdev_get_md(dev, offset_blocks, num_blocks, md);
	return spdk_bdev_read_blocks(desc, ch, buf, offset_blocks,
				     num_blocks, cb, cb_arg);
}

static inline int
ftl_nv_cache_bdev_write_blocks_with_md(struct spdk_ftl_dev *dev,
				       struct spdk_bdev_desc *desc,
				       struct spdk_io_channel *ch,
				       void *buf, void *md,
				       uint64_t offset_blocks, uint64_t num_blocks,
				       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	assert(desc == dev->nv_cache.bdev_desc);
	ftl_nv_cache_bdev_set_md(dev, offset_blocks, num_blocks, md);
	return spdk_bdev_write_blocks(desc, ch, buf,
				      offset_blocks, num_blocks,
				      cb, cb_arg);
}

#endif
#endif /* FTL_NV_CACHE_IO_H */
