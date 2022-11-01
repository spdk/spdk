/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_SPLIT_H
#define SPDK_VBDEV_SPLIT_H

#include "spdk/bdev_module.h"

/**
 * Add given disk name to split config. If bdev with \c base_bdev_name name
 * exist the split bdevs will be created right away, if not the split bdevs will
 * be created when base bdev became be available (during examination process).
 *
 * \param base_bdev_name Base bdev name
 * \param split_count number of splits to be created.
 * \param split_size_mb size of each bdev. If 0 use base bdev size / split_count
 * \return value >= 0 - number of splits create. Negative errno code on error.
 */
int create_vbdev_split(const char *base_bdev_name, unsigned split_count, uint64_t split_size_mb);

/**
 * Remove all created split bdevs and split config.
 *
 * \param base_bdev_name base bdev name
 * \return 0 on success or negative errno value.
 */
int vbdev_split_destruct(const char *base_bdev_name);

/**
 * Get the spdk_bdev_part_base associated with the given split base_bdev.
 *
 * \param base_bdev Bdev to get the part_base from
 * \return pointer to the associated spdk_bdev_part_base
 * \return NULL if the base_bdev is not being split by the split module
 */
struct spdk_bdev_part_base *vbdev_split_get_part_base(struct spdk_bdev *base_bdev);

#endif /* SPDK_VBDEV_SPLIT_H */
