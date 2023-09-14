/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_NV_CACHE_IO_H
#define FTL_NV_CACHE_IO_H

#include "spdk/bdev.h"
#include "ftl_core.h"

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

#endif /* FTL_NV_CACHE_IO_H */
