/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_LAYOUT_H
#define FTL_LAYOUT_H

#include "spdk/stdinc.h"

struct spdk_ftl_dev;
struct ftl_md;

#define FTL_NV_CACHE_CHUNK_DATA_SIZE(blocks) ((uint64_t)blocks * FTL_BLOCK_SIZE)
#define FTL_NV_CACHE_CHUNK_SIZE(blocks) \
	(FTL_NV_CACHE_CHUNK_DATA_SIZE(blocks) + (2 * FTL_NV_CACHE_CHUNK_MD_SIZE))

#define FTL_LAYOUT_REGION_TYPE_P2L_COUNT \
	(FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX - FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN + 1)

enum ftl_layout_region_type {
	/* Superblock describing the basic FTL information */
	FTL_LAYOUT_REGION_TYPE_SB = 0,
	/* Mirrored instance of the superblock on the base device */
	FTL_LAYOUT_REGION_TYPE_SB_BASE = 1,
	/* If using cached L2P, this region stores the serialized instance of it */
	FTL_LAYOUT_REGION_TYPE_L2P = 2,

	/* State of bands */
	FTL_LAYOUT_REGION_TYPE_BAND_MD = 3,
	/* Mirrored instance of bands state */
	FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR = 4,

	/* Map of valid physical addresses, used for more efficient garbage collection */
	FTL_LAYOUT_REGION_TYPE_VALID_MAP = 5,

	/* State of chunks */
	FTL_LAYOUT_REGION_TYPE_NVC_MD = 6,
	/* Mirrored instance of the state of chunks */
	FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR = 7,

	/* User data region on the nv cache device */
	FTL_LAYOUT_REGION_TYPE_DATA_NVC = 8,

	/* User data region on the base device */
	FTL_LAYOUT_REGION_TYPE_DATA_BASE = 9,

	/* P2L checkpointing allows for emulation of VSS on base device.
	 * 4 entries are needed - 2 for each writer */
	FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC = 10,
	FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC,
	FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT = 11,
	FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP = 12,
	FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT = 13,
	FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT,

	/* Information about trimmed space in FTL */
	FTL_LAYOUT_REGION_TYPE_TRIM_MD = 14,
	/* Mirrored information about trim */
	FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR = 15,

	FTL_LAYOUT_REGION_TYPE_MAX = 16,
	FTL_LAYOUT_REGION_TYPE_MAX_V3 = 16
};

/* last nvc/base region in terms of lba address space */
#define FTL_LAYOUT_REGION_LAST_NVC FTL_LAYOUT_REGION_TYPE_DATA_NVC
#define FTL_LAYOUT_REGION_LAST_BASE FTL_LAYOUT_REGION_TYPE_VALID_MAP
#define FTL_LAYOUT_REGION_TYPE_FREE_BASE (UINT32_MAX - 2)
#define FTL_LAYOUT_REGION_TYPE_FREE_NVC (UINT32_MAX - 1)
#define FTL_LAYOUT_REGION_TYPE_FREE (UINT32_MAX - 1)
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

	/* Current region version */
	struct ftl_layout_region_descriptor current;

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
		uint64_t num_usable_blocks;
		uint64_t user_blocks;
	} base;

	/* Organization for NV cache */
	struct {
		uint64_t total_blocks;
		uint64_t chunk_data_blocks;
		uint64_t chunk_meta_size;
		uint64_t chunk_count;
		uint64_t chunk_tail_md_num_blocks;
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

	/* Organization of P2L checkpoints */
	struct {
		/* Number of P2L checkpoint pages */
		uint64_t ckpt_pages;
	} p2l;

	struct ftl_layout_region region[FTL_LAYOUT_REGION_TYPE_MAX];

	/* Metadata object corresponding to the regions */
	struct ftl_md *md[FTL_LAYOUT_REGION_TYPE_MAX];
};

/**
 * @brief FTL MD layout operations interface
 */
struct ftl_md_layout_ops {
	/**
	 * @brief Create a new MD region
	 *
	 * @param dev ftl device
	 * @param reg_type region type
	 * @param reg_version region version
	 * @param entry_size MD entry size in bytes
	 * @param entry_count number of MD entries
	 *
	 * @retval 0 on success
	 * @retval -1 on fault
	 */
	int (*region_create)(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			     uint32_t reg_version, size_t reg_blks);

	int (*region_open)(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
			   uint32_t reg_version, size_t entry_size, size_t entry_count, struct ftl_layout_region *region);
};

/**
 * @brief Get number of blocks required to store an MD region
 *
 * @param dev ftl device
 * @param bytes size of the MD region in bytes
 *
 * @retval Number of blocks required to store an MD region
 */
uint64_t ftl_md_region_blocks(struct spdk_ftl_dev *dev, uint64_t bytes);

/**
 * @brief Get number of blocks for md_region aligned to a common value
 *
 * @param dev ftl device
 * @param blocks size of the MD region in blocks
 *
 * @retval Aligned number of blocks required to store an MD region
 */
uint64_t ftl_md_region_align_blocks(struct spdk_ftl_dev *dev, uint64_t blocks);

/**
 * @brief Get name of an MD region
 *
 * @param reg_type MD region type
 *
 * @return Name of the MD region specified
 */
const char *ftl_md_region_name(enum ftl_layout_region_type reg_type);

/**
 * @brief Setup FTL layout
 */
int ftl_layout_setup(struct spdk_ftl_dev *dev);

/**
 * @brief Setup FTL layout of a superblock
 */
int ftl_layout_setup_superblock(struct spdk_ftl_dev *dev);

/**
 * @brief Clear the superblock from the layout. Used after failure of shared memory files verification causes a retry.
 */
int ftl_layout_clear_superblock(struct spdk_ftl_dev *dev);

void ftl_layout_dump(struct spdk_ftl_dev *dev);
int ftl_validate_regions(struct spdk_ftl_dev *dev, struct ftl_layout *layout);

/**
 * @brief Get number of blocks required to store metadata on bottom device
 */
uint64_t ftl_layout_base_md_blocks(struct spdk_ftl_dev *dev);

/**
 * @brief Get the FTL layout region
 *
 * @param dev FTL device
 * @param reg_type type of the layout region
 *
 * @return pointer to the layout region if region was created or NULL, if not created
 */
struct ftl_layout_region *ftl_layout_region_get(struct spdk_ftl_dev *dev,
		enum ftl_layout_region_type reg_type);

/**
 * @brief Store the layout data in the blob
 *
 * @param dev FTL device
 * @param blob_buf Blob buffer where the layout will be stored
 * @param blob_buf_sz Size of the blob buffer in bytes
 *
 * @return Number of bytes the stored blob entries take up. 0 if calculated value would exceed blob_buf_sz.
 */
size_t ftl_layout_blob_store(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_buf_sz);

/**
 * @brief Load the layout data from the blob
 *
 * @param dev FTL device
 * @param blob_buf Blob buffer from which the layout will be loaded
 * @param blob_sz Size of the blob buffer in bytes
 *
 * @return 0 on success, -1 on failure
 */
int ftl_layout_blob_load(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_sz);

uint64_t ftl_layout_base_offset(struct spdk_ftl_dev *dev);

#endif /* FTL_LAYOUT_H */
