/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_SB_CURRENT_H
#define FTL_SB_CURRENT_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"

#define FTL_METADATA_VERSION_4			4
#define FTL_METADATA_VERSION_CURRENT	FTL_METADATA_VERSION_4

struct ftl_superblock {
	struct ftl_superblock_header	header;

	struct spdk_uuid		uuid;

	/* Current sequence number */
	uint64_t			seq_id;

	/* Flag describing clean shutdown */
	uint64_t			clean;

	/* Number of surfaced LBAs */
	uint64_t			lba_cnt;

	/* Percentage of base device blocks not exposed to the user */
	uint64_t			overprovisioning;

	/* Maximum IO depth per band relocate */
	uint64_t			max_reloc_qdepth;

	/* Reserved fields */
	uint64_t			reserved;

	bool				reserved3;

	uint32_t			reserved2;

	struct ftl_superblock_gc_info	gc_info;

	struct ftl_superblock_md_region	md_layout_head;
};

SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock, header) == 0,
		   "Invalid placement of header");

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock),
		   "FTL SB metadata size is invalid");

#endif /* FTL_SB_CURRENT_H */
