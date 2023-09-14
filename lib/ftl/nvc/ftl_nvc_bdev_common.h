/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#ifndef FTL_NVC_BDEV_COMMON_H
#define FTL_NVC_BDEV_COMMON_H

#include "ftl_core.h"
#include "ftl_layout.h"

bool ftl_nvc_bdev_common_is_chunk_active(struct spdk_ftl_dev *dev, uint64_t chunk_offset);

int ftl_nvc_bdev_common_region_create(struct spdk_ftl_dev *dev,
				      enum ftl_layout_region_type reg_type,
				      uint32_t reg_version, size_t reg_blks);

int ftl_nvc_bdev_common_region_open(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
				    uint32_t reg_version, size_t entry_size, size_t entry_count,
				    struct ftl_layout_region *region);

#endif /* FTL_NVC_BDEV_COMMON_H */
