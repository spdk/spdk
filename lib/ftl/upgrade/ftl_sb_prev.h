/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_SB_PREV_H
#define FTL_SB_PREV_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"

/**
 * Magic number identifies FTL superblock
 *
 * This old (pre v3) version has a bug - it's generating the magic number off 16b numbers, but only utilizing 8b from each
 */
#define FTL_MAGIC_V2(a, b, c, d) \
    ((UINT64_C(a) << 24) | (UINT64_C(b) << 16) | (UINT64_C(c) << 8) | UINT64_C(d))

#define FTL_SUPERBLOCK_MAGIC_V2 FTL_MAGIC_V2(0x1410, 0x1683, 0x1920, 0x1989)

#define FTL_SB_VERSION_0	0
#define FTL_SB_VERSION_1	1
#define FTL_SB_VERSION_2	2
#define FTL_SB_VERSION_3	3

struct ftl_superblock_v2 {
	struct ftl_superblock_header	header;

	struct spdk_uuid		uuid;

	/* Current sequence number */
	uint64_t			seq_id;

	/* Flag describing clean shutdown */
	uint64_t			clean;

	/* Number of surfaced LBAs */
	uint64_t			lba_cnt;
	/* Number of reserved addresses not exposed to the user */
	size_t				lba_rsvd;

	/* Maximum IO depth per band relocate */
	size_t				max_reloc_qdepth;

	/* Maximum active band relocates */
	size_t				max_active_relocs;

	/* Use append instead of write */
	bool				use_append;

	/* Maximum supported number of IO channels */
	uint32_t			max_io_channels;

	/* Last L2P checkpoint +1 (i.e. min_seq_id, 0:no ckpt) */
	uint64_t			ckpt_seq_id;

	struct ftl_superblock_gc_info	gc_info;
};


SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock_v2, header) == 0,
		   "Invalid placement of header");

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock_v2),
		   "FTL SB metadata size is invalid");

#endif /* FTL_SB_PREV_H */
