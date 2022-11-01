/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_SB_COMMON_H
#define FTL_SB_COMMON_H

#include "spdk/stdinc.h"
#include "utils/ftl_defs.h"
#include "utils/ftl_df.h"

/* Size of superblock on NV cache, make it bigger for future fields */
#define FTL_SUPERBLOCK_SIZE (128ULL * KiB)

#define FTL_MAGIC(a, b, c, d) \
    ((UINT64_C(a) << 48) | (UINT64_C(b) << 32) | (UINT64_C(c) << 16) | \
     UINT64_C(d))

/**
 * Magic number identifies FTL superblock
 */
#define FTL_SUPERBLOCK_MAGIC FTL_MAGIC(0x1410, 0x1683, 0x1920, 0x1989)

struct ftl_superblock_gc_info {
	/* High priority band; if there's no free bands after dirty shutdown, don't restart GC from same id, or phys_id -
	 * pick actual lowest validity band to avoid being stuck and try to write it to the open band.
	 */
	uint64_t band_id_high_prio;
	/* Currently relocated band (note it's just id, not seq_id ie. its actual location on disk) */
	uint64_t current_band_id;
	/* Bands are grouped together into larger reclaim units; this is the band id translated to those units */
	uint64_t band_phys_id;
	/* May be updating multiple fields at the same time, clearing/setting this marks the transaction */
	uint64_t is_valid;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct ftl_superblock_gc_info) == 32,
		   "ftl_superblock_gc_info incorrect size");

struct ftl_superblock_header {
	uint64_t magic;
	uint64_t crc;
	uint64_t version;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct ftl_superblock_header) == 24,
		   "ftl_superblock_header incorrect size");

struct ftl_superblock_md_region {
	uint32_t		type;
	uint32_t		version;
	uint64_t		blk_offs;
	uint64_t		blk_sz;
	ftl_df_obj_id		df_next;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct ftl_superblock_md_region) == 32,
		   "ftl_superblock_md_region incorrect size");

struct ftl_superblock_shm {
	/* SHM initialization completed */
	bool				shm_ready;

	/* SHM status - fast restart */
	bool				shm_clean;

	/* Used to continue trim after SHM recovery */
	struct {
		bool			in_progress;
		uint64_t		start_lba;
		uint64_t		num_blocks;
		uint64_t		seq_id;
	} trim;

	struct ftl_superblock_gc_info	gc_info;
};

#endif /* FTL_SB_COMMON_H */
