/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_SB_CURRENT_H
#define FTL_SB_CURRENT_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"

#define FTL_SB_VERSION_5			5
#define FTL_SB_VERSION_CURRENT			FTL_SB_VERSION_5

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

	/* Flag indicates that the FTL is ready for upgrade */
	uint8_t				upgrade_ready;

	/* Reserved field */
	uint8_t				reserved3[15];

	/* Last L2P checkpoint +1 (i.e. min_seq_id, 0:no ckpt) */
	uint64_t			ckpt_seq_id;

	struct ftl_superblock_gc_info	gc_info;

	/* Points to the end of blob area */
	ftl_df_obj_id			blob_area_end;

	/* NVC device name */
	char				nvc_dev_name[16];

	/* NVC-stored MD layout tracking info */
	struct ftl_superblock_v5_md_blob_hdr	md_layout_nvc;

	/* Base device name */
	char					base_dev_name[16];

	/* Base dev-stored MD layout tracking info */
	struct ftl_superblock_v5_md_blob_hdr	md_layout_base;

	/* FTL layout params */
	struct ftl_superblock_v5_md_blob_hdr	layout_params;

	/* Start of the blob area */
	char blob_area[0];
} __attribute__((packed));

SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock, header) == 0,
		   "Invalid placement of header");

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock),
		   "FTL SB metadata size is invalid");

#endif /* FTL_SB_CURRENT_H */
