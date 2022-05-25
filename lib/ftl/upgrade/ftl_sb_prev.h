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

#ifndef FTL_SB_PREV_H
#define FTL_SB_PREV_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"

/**
 * Magic number identifies FTL superblock
 */
#define FTL_MAGIC_V2(a, b, c, d) \
    ((UINT64_C(a) << 24) | (UINT64_C(b) << 16) | (UINT64_C(c) << 8) | \
     UINT64_C(d))

#define FTL_SUPERBLOCK_MAGIC_V2 FTL_MAGIC_V2(0x1410, 0x1683, 0x1920, 0x1989)

#define FTL_METADATA_VERSION_0	0
#define FTL_METADATA_VERSION_1	1
#define FTL_METADATA_VERSION_2	2

struct ftl_superblock_v2 {
	struct ftl_superblock_header	header;

	struct spdk_uuid				uuid;

	/* Current sequence number */
	uint64_t						seq_id;

	/* Flag describing clean shutdown */
	uint64_t						clean;

	/* Number of surfaced LBAs */
	uint64_t						lba_cnt;
	/* Number of reserved addresses not exposed to the user */
	size_t							lba_rsvd;

	/* Maximum IO depth per band relocate */
	size_t							max_reloc_qdepth;

	/* Maximum active band relocates */
	size_t							max_active_relocs;

	/* Use append instead of write */
	bool							use_append;

	/* Maximum supported number of IO channels */
	uint32_t						max_io_channels;

	/* Last L2P checkpoint +1 (i.e. min_seq_id, 0:no ckpt) */
	uint64_t						ckpt_seq_id;

	struct ftl_superblock_gc_info	gc_info;
};


SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock_v2, header) == 0,
		   "Invalid placement of header");

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock_v2),
		   "FTL SB metadata size is invalid");

#endif /* FTL_SB_PREV_H */
