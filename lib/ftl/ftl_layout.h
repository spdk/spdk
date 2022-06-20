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

#ifndef FTL_LAYOUT_H
#define FTL_LAYOUT_H

#include "spdk/stdinc.h"

struct spdk_ftl_dev;
struct ftl_md;

enum ftl_layout_region_type {
	/* User data region on the nv cache device */
	ftl_layout_region_type_data_nvc,

	/* User data region on the base device */
	ftl_layout_region_type_data_btm,

	ftl_layout_region_type_max,

	/* last nvc/btm region in terms of lba address space */
	ftl_layout_region_last_nvc = ftl_layout_region_type_data_nvc,
	ftl_layout_region_last_btm = ftl_layout_region_type_data_btm,

	ftl_layout_region_type_free_btm = UINT32_MAX - 2,
	ftl_layout_region_type_free_nvc = UINT32_MAX - 1,
	ftl_layout_region_type_invalid = UINT32_MAX,
};

struct ftl_layout_region_version {
	/* Current version of the region */
	uint64_t version;

	/* Offset on a device in FTL_BLOCK_SIZE unit*/
	uint64_t offset;

	/* Number of blocks in FTL_BLOCK_SIZE unit */
	uint64_t blocks;

	struct ftl_superblock_md_region *sb_md_reg;
};

/* Data or metadata region on devices */
struct ftl_layout_region {
	/* Name of the region */
	const char *name;

	/* Region type */
	enum ftl_layout_region_type type;

	/* Mirror region type - a region may be mirrored for higher durability */
	enum ftl_layout_region_type mirror_type;

	/* Latest region version */
	struct ftl_layout_region_version current;

	/* Previous region version, if found */
	struct ftl_layout_region_version prev;

	/* Number of blocks in FTL_BLOCK_SIZE unit of a single entry */
	uint64_t entry_size;

	/* Number of entries */
	uint64_t num_entries;

	/* VSS MD size or 0:disable VSS MD */
	uint64_t vss_blksz;

	/* Device of region */
	struct spdk_bdev_desc *bdev_desc;

	/* IO channel of region */
	struct spdk_io_channel *ioch;
};

/*
 * This structure describes the geometry (space organization) of FTL
 */
struct ftl_layout {
	/* Organization for base device */
	struct {
		uint64_t total_blocks;
	} btm;

	/* Organization for NV cache */
	struct {
		uint64_t total_blocks;
	} nvc;

	/* Information corresponding to L2P */
	struct {
		/* Address length in bits */
		uint64_t addr_length;
		/* Address size in bytes */
		uint64_t addr_size;
		/* Number of LBAS in memory page */
		uint64_t lbas_in_page;
	} l2p;

	struct ftl_layout_region region[ftl_layout_region_type_max];
};

/**
 * @brief Setup FTL layout
 */
int ftl_layout_setup(struct spdk_ftl_dev *dev);

void layout_dump(struct spdk_ftl_dev *dev);
int validate_regions(struct spdk_ftl_dev *dev, struct ftl_layout *layout);

#endif /* FTL_LAYOUT_H */
