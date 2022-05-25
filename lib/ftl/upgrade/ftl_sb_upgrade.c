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

#include "ftl_sb_upgrade.h"
#include "ftl_layout_upgrade.h"
#include "ftl_layout.h"
#include "ftl_core.h"

static int
ftl_sb_verify_v2_to_v3(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	union ftl_superblock_ver *sb = (union ftl_superblock_ver *)dev->sb;
	assert(sb->header.version == FTL_METADATA_VERSION_2);

	if (!(sb->v2.clean == 1 && dev->sb_shm->shm_clean == 0)) {
		FTL_ERRLOG(dev, "FTL superblock upgrade: dirty\n");
		return -1;
	}
	return 0;
}

static int
ftl_sb_upgrade_v2_to_v3(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	union ftl_superblock_ver *sb = (union ftl_superblock_ver *)dev->sb;
	FTL_NOTICELOG(dev, "FTL superblock upgrade v2 to v3\n");

	// correct the magic
	sb->header.magic = FTL_SUPERBLOCK_MAGIC;

	// bump up version no
	sb->header.version = FTL_METADATA_VERSION_CURRENT;

	// leave md region list empty - initialize in ftl_mngt_layout_upgrade()
	sb->v3.md_layout_head.type = ftl_layout_region_type_invalid;
	sb->v3.md_layout_head.df_next = FTL_DF_OBJ_ID_INVALID;

	// crc will be updated later
	// i.e. ftl_mngt_set_dirty (1st/clean start) or ftl_mngt_set_clean (shutdown)
	// note dirty startup doesn't guarantee the updated sb will be stored
	return 0;
}

struct ftl_region_upgrade_desc sb_upgrade_desc[] = {
	[FTL_METADATA_VERSION_0] = {.verify = ftl_region_upgrade_disabled},
	[FTL_METADATA_VERSION_1] = {.verify = ftl_region_upgrade_disabled},
	[FTL_METADATA_VERSION_2] = {.verify = ftl_sb_verify_v2_to_v3,
		.upgrade = ftl_sb_upgrade_v2_to_v3,
		.new_version = FTL_METADATA_VERSION_3},
};

SPDK_STATIC_ASSERT(sizeof(sb_upgrade_desc) / sizeof(*sb_upgrade_desc) == FTL_METADATA_VERSION_CURRENT,
	"Missing SB region upgrade descriptors");
