/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_LAYOUT_H
#define FTL_LAYOUT_H

#include "spdk/stdinc.h"

struct spdk_ftl_dev;
struct ftl_md;

enum ftl_layout_region_type {
#ifdef SPDK_FTL_VSS_EMU
	/** VSS region for NV cache VSS emulation */
	FTL_LAYOUT_REGION_TYPE_VSS,
#endif
	/* User data region on the nv cache device */
	FTL_LAYOUT_REGION_TYPE_DATA_NVC,

	/* User data region on the base device */
	FTL_LAYOUT_REGION_TYPE_DATA_BASE,

	FTL_LAYOUT_REGION_TYPE_MAX,
};

/* last nvc/base region in terms of lba address space */
#define FTL_LAYOUT_REGION_LAST_NVC FTL_LAYOUT_REGION_TYPE_DATA_NVC
#define FTL_LAYOUT_REGION_LAST_BASE FTL_LAYOUT_REGION_TYPE_DATA_BASE
#define FTL_LAYOUT_REGION_TYPE_FREE_BASE (UINT32_MAX - 2)
#define FTL_LAYOUT_REGION_TYPE_FREE_NVC (UINT32_MAX - 1)
#define FTL_LAYOUT_REGION_TYPE_INVALID (UINT32_MAX)

struct ftl_layout_region_descriptor {
	/* Current version of the region */
	uint64_t version;

	/* Offset in FTL_BLOCK_SIZE unit where the region exists on the device */
	uint64_t offset;

	/* Number of blocks in FTL_BLOCK_SIZE unit */
	uint64_t blocks;
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
	struct ftl_layout_region_descriptor current;

	/* Previous region version, if found */
	struct ftl_layout_region_descriptor prev;

	/* Number of blocks in FTL_BLOCK_SIZE unit of a single entry.
	 * A metadata region may be subdivided into multiple smaller entries.
	 * Eg. there's one region describing all bands, but you may be able to access
	 * metadata of a single one.
	 */
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
	} base;

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

	struct ftl_layout_region region[FTL_LAYOUT_REGION_TYPE_MAX];

	/* Metadata object corresponding to the regions */
	struct ftl_md *md[FTL_LAYOUT_REGION_TYPE_MAX];
};

/**
 * @brief Setup FTL layout
 */
int ftl_layout_setup(struct spdk_ftl_dev *dev);

#ifdef SPDK_FTL_VSS_EMU
/**
 * @brief Setup FTL layout of VSS emu
 */
void ftl_layout_setup_vss_emu(struct spdk_ftl_dev *dev);
#endif

void ftl_layout_dump(struct spdk_ftl_dev *dev);
int ftl_validate_regions(struct spdk_ftl_dev *dev, struct ftl_layout *layout);

#endif /* FTL_LAYOUT_H */
