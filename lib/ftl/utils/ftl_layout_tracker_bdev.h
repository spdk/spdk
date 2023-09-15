/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#ifndef FTL_LAYOUT_TRACKER_BDEV_H
#define FTL_LAYOUT_TRACKER_BDEV_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "ftl_layout.h"

/**
 * FTL layout region descriptor
 */
struct ftl_layout_tracker_bdev_region_props {
	/* Region type */
	uint32_t type;

	/* Region version */
	uint32_t ver;

	/* Region offset in blocks */
	uint64_t blk_offs;

	/* Region size in blocks */
	uint64_t blk_sz;
};

/**
 * FTL layout region tracker
 */
struct ftl_layout_tracker_bdev;

/**
 * @brief Initialize the FTL layout tracker
 *
 * @param bdev_blks bdev's number of blocks
 *
 * @return pointer to ftl_md_layout_tracker_bdev on success : NULL on fault
 */
struct ftl_layout_tracker_bdev *ftl_layout_tracker_bdev_init(uint64_t bdev_blks);

/**
 * @brief Deinitialize the FTL layout tracker
 *
 * @param tracker pointer to the tracker instance
 */
void ftl_layout_tracker_bdev_fini(struct ftl_layout_tracker_bdev *tracker);

/**
 * @brief Add a new FTL layout region
 *
 * @param tracker pointer to the tracker instance
 * @param reg_type FTL layout region type
 * @param reg_ver FTL layout region version
 * @param blk_sz size in blocks of the FTL layout region
 * @param blk_align offset alignment in blocks of the FTL layout region
 *
 * @return pointer to the ftl_layout_tracker_bdev_region_props, describing the region added or NULL upon fault
 */
const struct ftl_layout_tracker_bdev_region_props *ftl_layout_tracker_bdev_add_region(
	struct ftl_layout_tracker_bdev *tracker, enum ftl_layout_region_type reg_type, uint32_t reg_ver,
	uint64_t blk_sz, uint64_t blk_align);

/**
 * @brief Remove an existing FTL lyout region
 *
 * @param tracker pointer to the tracker instance
 * @param reg_type FTL layout region type
 * @param reg_ver FTL layout region version
 *
 * @return 0 on success : -1 on fault
 */
int ftl_layout_tracker_bdev_rm_region(struct ftl_layout_tracker_bdev *tracker,
				      enum ftl_layout_region_type reg_type, uint32_t reg_ver);

/**
 * @brief Find the next FTL layout region of a given type
 *
 * @param tracker pointer to the tracker instance
 * @param reg_type region type filter, FTL_LAYOUT_REGION_TYPE_INVALID to iterate through all regions
 * @param search_ctx search context, points to NULL to find the first region
 */
void ftl_layout_tracker_bdev_find_next_region(struct ftl_layout_tracker_bdev *tracker,
		enum ftl_layout_region_type reg_type,
		const struct ftl_layout_tracker_bdev_region_props **search_ctx);

#endif /* FTL_LAYOUT_TRACKER_BDEV_H */
