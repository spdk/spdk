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

#include "ftl_sb.h"
#include "ftl_core.h"
#include "ftl_layout.h"

bool
ftl_superblock_check_magic(struct ftl_superblock *sb)
{
	return sb->header.magic == FTL_SUPERBLOCK_MAGIC;
}

bool
ftl_superblock_md_layout_is_empty(struct ftl_superblock *sb)
{
	return sb->md_layout_head.type == ftl_layout_region_type_invalid;
}

static bool
md_region_is_fixed(int reg_type)
{
	switch (reg_type) {
#ifdef SPDK_FTL_VSS_EMU
		case ftl_layout_region_type_vss:
#endif
		case ftl_layout_region_type_sb:
		case ftl_layout_region_type_sb_btm:
		case ftl_layout_region_type_data_btm:
			return true;

		default:
			return false;
	}
}

static uint32_t
md_regions_count(void)
{
	uint32_t reg_cnt = 0;
	for (uint32_t reg_type = 0; reg_type < ftl_layout_region_type_max; reg_type++) {
		if (md_region_is_fixed(reg_type)) {
			continue;
		}

		reg_cnt++;
	}
	return reg_cnt;
}

static bool
superblock_md_region_overflow(struct spdk_ftl_dev *dev, struct ftl_superblock_md_region *sb_reg)
{
	return ((uintptr_t)sb_reg < (uintptr_t)dev->sb
	    || (uintptr_t)(sb_reg + 1) > ((uintptr_t)(dev->sb) + FTL_SUPERBLOCK_SIZE))
	    || (uintptr_t)(sb_reg + 1) < (uintptr_t)sb_reg;
}

static int
superblock_md_layout_add(struct spdk_ftl_dev *dev, struct ftl_superblock_md_region *sb_reg,
    uint32_t reg_type, uint32_t reg_version, uint64_t blk_offs, uint64_t blk_sz)
{
	if (superblock_md_region_overflow(dev, sb_reg)) {
		FTL_ERRLOG(dev, "Buffer overflow\n");
		return -EOVERFLOW;
	}

	sb_reg->type = reg_type;
	sb_reg->version = reg_version;
	sb_reg->blk_offs = blk_offs;
	sb_reg->blk_sz = blk_sz;
	return 0;
}

static int
superblock_md_layout_add_free(struct spdk_ftl_dev *dev, struct ftl_superblock_md_region **sb_reg,
    uint32_t reg_type, uint32_t free_type, uint64_t total_blocks)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *reg = &layout->region[reg_type];
	uint64_t blks_left = total_blocks - reg->current.offset - reg->current.blocks;
	if (blks_left) {
		(*sb_reg)->df_next = ftl_df_get_obj_id(dev->sb, (*sb_reg) + 1);
		(*sb_reg) = (*sb_reg) + 1;

		if (superblock_md_layout_add(dev, *sb_reg, free_type, 0,
			reg->current.offset + reg->current.blocks, blks_left)) {
			return -1;
		}

		(*sb_reg)->df_next = FTL_DF_OBJ_ID_INVALID;
	}
	return 0;
}

int
ftl_superblock_md_layout_build(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock *sb = dev->sb;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *reg;
	int n = 0;
	bool is_empty = ftl_superblock_md_layout_is_empty(dev->sb);

	// TODO: major upgrades: add all free regions being tracked
	// For now SB MD layout must be empty - otherwise md free regions may be lost
	assert(is_empty);

	struct ftl_superblock_md_region *sb_reg = &sb->md_layout_head;
	for (;;) {
		reg = &layout->region[n];
		if (md_region_is_fixed(reg->type)) {
			reg->current.sb_md_reg = NULL;
			n++;
			continue;
		}

		if (superblock_md_layout_add(dev, sb_reg, reg->type, reg->current.version,
			  reg->current.offset, reg->current.blocks)) {
			return -1;
		}
		reg->current.sb_md_reg = sb_reg;

		n++;
		if (n < ftl_layout_region_type_max) {
			// next region
			sb_reg->df_next = ftl_df_get_obj_id(sb, sb_reg + 1);
			sb_reg++;
		} else {
			// terminate the list
			sb_reg->df_next = FTL_DF_OBJ_ID_INVALID;
			break;
		}
	}

	// create free_nvc/free_btm regions on the first run
	if (is_empty) {
		superblock_md_layout_add_free(dev, &sb_reg, ftl_layout_region_last_nvc,
		  ftl_layout_region_type_free_nvc, layout->nvc.total_blocks);

		superblock_md_layout_add_free(dev, &sb_reg, ftl_layout_region_last_btm,
		  ftl_layout_region_type_free_btm, layout->btm.total_blocks);
	}

	return 0;
}

int
ftl_superblock_md_layout_load_all(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock *sb = dev->sb;
	struct ftl_superblock_md_region *sb_reg = &sb->md_layout_head;
	struct ftl_layout *layout = &dev->layout;
	uint32_t regs_found = 0;

	for (int n = 0; n < ftl_layout_region_type_max; n++) {
		if (md_region_is_fixed(sb_reg->type)) {
			continue;
		}

		layout->region[n].current.sb_md_reg = NULL;
		layout->region[n].prev = layout->region[n].current;
	}

	while (sb_reg->type != ftl_layout_region_type_invalid) {
		// TODO: major upgrades: add free regions tracking
		if (sb_reg->type == ftl_layout_region_type_free_nvc
		  || sb_reg->type == ftl_layout_region_type_free_btm) {
			goto next_sb_reg;
		}

		if (sb_reg->type >= ftl_layout_region_type_max) {
			FTL_ERRLOG(dev, "Invalid MD region type found\n");
			return -1;
		}

		if (md_region_is_fixed(sb_reg->type)) {
			FTL_ERRLOG(dev, "Unsupported MD region type found\n");
			return -1;
		}

		struct ftl_layout_region *reg = &layout->region[sb_reg->type];
		if (sb_reg->version == reg->current.version) {
			if (reg->current.sb_md_reg) {
				FTL_ERRLOG(dev, "Multiple/looping current regions found\n");
				return -1;
			}

			reg->current.offset = sb_reg->blk_offs;
			reg->current.blocks = sb_reg->blk_sz;
			reg->current.sb_md_reg = sb_reg;

			if (!reg->prev.sb_md_reg) {
				regs_found++;
			}
		} else {
			if (sb_reg->version > reg->current.version) {
				FTL_ERRLOG(dev, "Unknown region version found\n");
				return -1;
			}
			if (sb_reg->version == reg->prev.version) {
				FTL_ERRLOG(dev, "Multiple/looping prev regions found\n");
				return -1;
			}

			if (sb_reg->version < reg->prev.version) {
				if ((!reg->current.sb_md_reg) && (!reg->prev.sb_md_reg)) {
					regs_found++;
				}

				reg->prev.offset = sb_reg->blk_offs;
				reg->prev.blocks = sb_reg->blk_sz;
				reg->prev.version = sb_reg->version;
				reg->prev.sb_md_reg = sb_reg;
			}
		}

next_sb_reg:
		if (sb_reg->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		sb_reg = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
		if (superblock_md_region_overflow(dev, sb_reg)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return -EOVERFLOW;
		}
	}

	if (regs_found != md_regions_count()) {
		FTL_ERRLOG(dev, "Missing regions\n");
		return -1;
	}

	return 0;
}

void ftl_superblock_md_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock *sb = dev->sb;
	struct ftl_superblock_md_region *sb_reg = &sb->md_layout_head;

	FTL_NOTICELOG(dev, "SB metadata layout:\n");
	while (sb_reg->type != ftl_layout_region_type_invalid) {
		FTL_NOTICELOG(dev, "Region df:0x%"PRIx64" type:0x%"PRIx32" ver:%"PRIu32" blk_offs:0x%"PRIx64" blk_sz:0x%"PRIx64"\n",
		  ftl_df_get_obj_id(sb, sb_reg), sb_reg->type, sb_reg->version, sb_reg->blk_offs, sb_reg->blk_sz);

		if (sb_reg->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		sb_reg = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
		if (superblock_md_region_overflow(dev, sb_reg)) {
			FTL_ERRLOG(dev, "Buffer overflow\n");
			return;
		}
	}
}
