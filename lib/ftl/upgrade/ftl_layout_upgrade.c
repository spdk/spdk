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

#include "ftl_layout_upgrade.h"
#include "ftl_layout.h"
#include "ftl_sb_current.h"
#include "ftl_sb_prev.h"
#include "ftl_core.h"
#include "ftl_band.h"

int ftl_region_upgrade_disabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	return -1;
}

int ftl_region_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	if (!(dev->sb->clean == 1 && dev->sb_shm->shm_clean == 0)) {
		FTL_ERRLOG(dev, "FTL region upgrade: SB dirty\n");
		return -1;
	}
	return 0;
}

#ifndef UTEST
extern struct ftl_region_upgrade_desc sb_upgrade_desc[];
extern struct ftl_region_upgrade_desc p2l_upgrade_desc[];
extern struct ftl_region_upgrade_desc nvc_upgrade_desc[];
extern struct ftl_region_upgrade_desc band_upgrade_desc[];

static struct ftl_layout_upgrade_desc layout_upgrade_desc[] = {
#ifdef SPDK_FTL_VSS_EMU
	[ftl_layout_region_type_vss] = {},
#endif
	[ftl_layout_region_type_sb] = {
		.reg_upgrade_desc_sz = FTL_METADATA_VERSION_CURRENT,
		.reg_upgrade_desc = sb_upgrade_desc,
	},
	[ftl_layout_region_type_sb_btm] = {
		.reg_upgrade_desc_sz = FTL_METADATA_VERSION_CURRENT,
		.reg_upgrade_desc = sb_upgrade_desc,
	},
	[ftl_layout_region_type_l2p] = {},
	[ftl_layout_region_type_band_md] = {
			.reg_upgrade_desc_sz = FTL_BAND_VERSION_CURRENT,
			.reg_upgrade_desc = band_upgrade_desc,
	},
	[ftl_layout_region_type_band_md_mirror] = {
			.reg_upgrade_desc_sz = FTL_BAND_VERSION_CURRENT,
			.reg_upgrade_desc = band_upgrade_desc,
	},
	[ftl_layout_region_type_valid_map] = {},
	[ftl_layout_region_type_nvc_md] = {},
	[ftl_layout_region_type_nvc_md_mirror] = {},
	[ftl_layout_region_type_data_nvc] = {},
	[ftl_layout_region_type_data_btm] = {},
	[ftl_layout_region_type_p2l_ckpt_gc] = {
			.reg_upgrade_desc_sz = FTL_P2L_VERSION_CURRENT,
			.reg_upgrade_desc = p2l_upgrade_desc,
	},
	[ftl_layout_region_type_p2l_ckpt_gc_next] = {
			.reg_upgrade_desc_sz = FTL_P2L_VERSION_CURRENT,
			.reg_upgrade_desc = p2l_upgrade_desc,
	},
	[ftl_layout_region_type_p2l_ckpt_comp] = {
			.reg_upgrade_desc_sz = FTL_P2L_VERSION_CURRENT,
			.reg_upgrade_desc = p2l_upgrade_desc,
	},
	[ftl_layout_region_type_p2l_ckpt_comp_next] = {
			.reg_upgrade_desc_sz = FTL_P2L_VERSION_CURRENT,
			.reg_upgrade_desc = p2l_upgrade_desc,
	},
	[ftl_layout_region_type_trim_md] = {},
	[ftl_layout_region_type_trim_md_mirror] = {},
};

SPDK_STATIC_ASSERT(sizeof(layout_upgrade_desc) / sizeof(*layout_upgrade_desc) == ftl_layout_region_type_max,
	"Missing layout upgrade descriptors");
#endif

static int region_verify(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	assert(ctx->reg);
	assert(ctx->reg->current.version == ctx->upgrade->reg_upgrade_desc_sz);

	uint64_t ver = ctx->reg->prev.version;
	if (ver > ctx->upgrade->reg_upgrade_desc_sz) {
		FTL_ERRLOG(dev, "Unknown region version\n");
		return -1;
	}

	while (ver < ctx->reg->current.version) {
		int rc = ctx->upgrade->reg_upgrade_desc[ver].verify(dev, ctx->reg);
		if (rc) {
			return rc;
		}
		ver = ctx->upgrade->reg_upgrade_desc[ver].new_version;
	}
	return 0;
}

int ftl_region_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	assert(ctx->reg);
	assert(ctx->reg->prev.version <= ctx->reg->current.version);
	assert(ctx->reg->current.version == ctx->upgrade->reg_upgrade_desc_sz);

	int rc = 0;
	uint64_t ver = ctx->reg->prev.version;
	if (ver < ctx->upgrade->reg_upgrade_desc_sz) {
		ctx->next_reg_ver = ctx->upgrade->reg_upgrade_desc[ver].new_version;
		rc = ctx->upgrade->reg_upgrade_desc[ver].upgrade(dev, ctx);
	}
	return rc;
}

void ftl_region_upgrade_completed(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx, int status)
{
	assert(ctx->reg);
	assert(ctx->reg->prev.version < ctx->next_reg_ver);
	assert(ctx->next_reg_ver <= ctx->reg->current.version);

	// SB MD isn't tracked in SB MD region list
	if (!status) {
		if (ctx->reg->prev.sb_md_reg) {
			int rc = ftl_superblock_md_layout_upgrade_region(dev, ctx->reg->prev.sb_md_reg, ctx->next_reg_ver);
			ftl_bug(rc != 0);
		}

		ctx->reg->prev.version = ctx->next_reg_ver;
	}

	if (ctx->cb) {
		ctx->cb(dev, ctx->cb_ctx, status);
	}
}

int ftl_layout_verify(struct spdk_ftl_dev *dev)
{
	int rc = 0;
	struct ftl_layout *layout = &dev->layout;

	if (ftl_superblock_md_layout_is_empty(dev->sb)) {
		rc = ftl_superblock_md_layout_build(dev);
		goto exit;
	}

	if (ftl_superblock_md_layout_load_all(dev)) {
		return -1;
	}

	if (validate_regions(dev, layout)) {
		return -1;
	}

	struct ftl_layout_upgrade_ctx ctx = {0};
	ctx.reg = &dev->layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];

	while (true) {
		if (region_verify(dev, &ctx)) {
			return -1;
		}

		if (ctx.reg->type == ftl_layout_region_type_max - 1) {
			break;
		}

		ctx.reg++;
		ctx.upgrade++;
	}

exit:
	return rc;
}

int ftl_layout_dump(struct spdk_ftl_dev *dev)
{
	if (validate_regions(dev, &dev->layout)) {
		return -1;
	}

	layout_dump(dev);
	ftl_superblock_md_layout_dump(dev);
	return 0;
}

int ftl_superblock_upgrade(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_upgrade_ctx ctx = {0};
	struct ftl_layout_region *reg = &dev->layout.region[ftl_layout_region_type_sb];

	ctx.reg = reg;
	ctx.upgrade = &layout_upgrade_desc[ftl_layout_region_type_sb];
	reg->prev.version = dev->sb->header.version;

	int rc = region_verify(dev, &ctx);
	if (rc) {
		return rc;
	}

	while(reg->prev.version < reg->current.version) {
		int rc = ftl_region_upgrade(dev, &ctx);
		if (rc) {
			return rc;
		}
		// SB upgrades are all synchronous
		ftl_region_upgrade_completed(dev, &ctx, rc);
	}

	dev->layout.region[ftl_layout_region_type_sb_btm].prev.version = reg->prev.version;
	return 0;
}

static int layout_upgrade_select_next_region(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
next_region:
	assert(ctx->reg);
	assert(ctx->upgrade);
	struct ftl_layout_region *reg = ctx->reg;
	uint32_t reg_type = reg->type;
	uint64_t reg_ver = reg->prev.version;

	// qualify region version to upgrade
	if (reg_ver < reg->current.version) {
		return ftl_layout_upgrade_continue;
	}

	// select the next region to upgrade
	if (reg_ver == reg->current.version) {
		reg_type++;
		if (reg_type == ftl_layout_region_type_max) {
			return ftl_layout_upgrade_done;
		}

		ctx->reg++;
		ctx->upgrade++;
		goto next_region;
	}

	// unknown version
	assert(reg_ver > reg->current.version);
	FTL_ERRLOG(dev, "Region %d upgrade fault: version %"PRIu64"/%"PRIu64"\n",
		reg_type, reg_ver, reg->current.version);
	return ftl_layout_upgrade_fault;
}

int ftl_layout_upgrade_init_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	if (!ctx->reg) {
		ctx->reg = &dev->layout.region[0];
		ctx->upgrade = &layout_upgrade_desc[0];
	}

	return layout_upgrade_select_next_region(dev, ctx);
}
