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

#ifndef FTL_SB_CURRENT_H
#define FTL_SB_CURRENT_H

#include "spdk/uuid.h"
#include "ftl_sb_common.h"

#define FTL_METADATA_VERSION_3			3
#define FTL_METADATA_VERSION_CURRENT	FTL_METADATA_VERSION_3

struct ftl_superblock {
	struct ftl_superblock_header	header;

	struct spdk_uuid				uuid;

	/* Flag describing clean shutdown */
	uint64_t						clean;

	/* Number of surfaced LBAs */
	uint64_t						lba_cnt;

	/* Number of reserved addresses not exposed to the user */
	uint64_t						lba_rsvd;

	/* Maximum IO depth per band relocate */
	uint64_t						max_reloc_qdepth;

	/* Reserved field */
	uint64_t						reserved;

	/* Use append instead of write */
	bool							use_append;

	/* Maximum supported number of IO channels */
	uint32_t						max_io_channels;

	struct ftl_superblock_gc_info	gc_info;

	struct ftl_superblock_md_region	md_layout_head;
};

SPDK_STATIC_ASSERT(offsetof(struct ftl_superblock, header) == 0,
		   "Invalid placement of header");

SPDK_STATIC_ASSERT(FTL_SUPERBLOCK_SIZE >= sizeof(struct ftl_superblock),
		   "FTL SB metadata size is invalid");

#endif /* FTL_SB_CURRENT_H */
