/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_layout_upgrade.h"
#include "ftl_layout.h"
#include "ftl_sb_current.h"
#include "ftl_sb_prev.h"
#include "ftl_core.h"
#include "ftl_band.h"

int
ftl_region_upgrade_disabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	return -1;
}

int
ftl_region_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
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

static struct ftl_layout_upgrade_desc_list layout_upgrade_desc[] = {
	[FTL_LAYOUT_REGION_TYPE_SB] = {
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_SB_BASE] = {
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_L2P] = {},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD] = {
		.count = FTL_BAND_VERSION_CURRENT,
		.desc = band_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR] = {
		.count = FTL_BAND_VERSION_CURRENT,
		.desc = band_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_VALID_MAP] = {},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD] = {
		.count = FTL_NVC_VERSION_CURRENT,
		.desc = nvc_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR] = {
		.count = FTL_NVC_VERSION_CURRENT,
		.desc = nvc_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_DATA_NVC] = {},
	[FTL_LAYOUT_REGION_TYPE_DATA_BASE] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC] = {
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT] = {
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP] = {
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT] = {
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD] = {},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR] = {},
#ifdef SPDK_FTL_VSS_EMU
	[FTL_LAYOUT_REGION_TYPE_VSS] = {},
#endif
};

SPDK_STATIC_ASSERT(sizeof(layout_upgrade_desc) / sizeof(*layout_upgrade_desc) ==
		   FTL_LAYOUT_REGION_TYPE_MAX,
		   "Missing layout upgrade descriptors");
#endif

static int
region_verify(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	uint64_t ver;

	assert(ctx->reg);
	assert(ctx->reg->current.version == ctx->upgrade->count);
	ver = ctx->reg->prev.version;
	if (ver > ctx->upgrade->count) {
		FTL_ERRLOG(dev, "Unknown region version\n");
		return -1;
	}

	while (ver < ctx->reg->current.version) {
		int rc = ctx->upgrade->desc[ver].verify(dev, ctx->reg);
		if (rc) {
			return rc;
		}
		ver = ctx->upgrade->desc[ver].new_version;
	}
	return 0;
}

int
ftl_region_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	int rc = 0;
	uint64_t ver;

	assert(ctx->reg);
	assert(ctx->reg->prev.version <= ctx->reg->current.version);
	assert(ctx->reg->current.version == ctx->upgrade->count);
	ver = ctx->reg->prev.version;
	if (ver < ctx->upgrade->count) {
		ctx->next_reg_ver = ctx->upgrade->desc[ver].new_version;
		rc = ctx->upgrade->desc[ver].upgrade(dev, ctx);
	}
	return rc;
}

void
ftl_region_upgrade_completed(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx,
			     int status)
{
	assert(ctx->reg);
	assert(ctx->reg->prev.version < ctx->next_reg_ver);
	assert(ctx->next_reg_ver <= ctx->reg->current.version);

	/* SB MD isn't tracked in SB MD region list */
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

int
ftl_layout_verify(struct spdk_ftl_dev *dev)
{
	int rc = 0;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_upgrade_ctx ctx = {0};

	if (ftl_superblock_md_layout_is_empty(dev->sb)) {
		rc = ftl_superblock_md_layout_build(dev);
		goto exit;
	}

	if (ftl_superblock_md_layout_load_all(dev)) {
		return -1;
	}

	if (ftl_validate_regions(dev, layout)) {
		return -1;
	}

	ctx.reg = &dev->layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];

	while (true) {
		if (region_verify(dev, &ctx)) {
			return -1;
		}

		if (ctx.reg->type == FTL_LAYOUT_REGION_TYPE_MAX - 1) {
			break;
		}

		ctx.reg++;
		ctx.upgrade++;
	}

exit:
	return rc;
}

int
ftl_upgrade_layout_dump(struct spdk_ftl_dev *dev)
{
	if (ftl_validate_regions(dev, &dev->layout)) {
		return -1;
	}

	ftl_layout_dump(dev);
	ftl_superblock_md_layout_dump(dev);
	return 0;
}

int
ftl_superblock_upgrade(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_upgrade_ctx ctx = {0};
	struct ftl_layout_region *reg = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_SB];
	int rc;

	ctx.reg = reg;
	ctx.upgrade = &layout_upgrade_desc[FTL_LAYOUT_REGION_TYPE_SB];
	reg->prev.version = dev->sb->header.version;

	rc = region_verify(dev, &ctx);
	if (rc) {
		return rc;
	}

	while (reg->prev.version < reg->current.version) {
		rc = ftl_region_upgrade(dev, &ctx);
		if (rc) {
			return rc;
		}
		/* SB upgrades are all synchronous */
		ftl_region_upgrade_completed(dev, &ctx, rc);
	}

	dev->layout.region[FTL_LAYOUT_REGION_TYPE_SB_BASE].prev.version = reg->prev.version;
	return 0;
}

static int
layout_upgrade_select_next_region(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *reg;
	uint64_t reg_ver;
	uint32_t reg_type = ctx->reg->type;

	while (reg_type != FTL_LAYOUT_REGION_TYPE_MAX) {
		assert(ctx->reg);
		assert(ctx->upgrade);
		reg = ctx->reg;
		reg_ver = reg->prev.version;

		if (reg_ver < reg->current.version) {
			/* qualify region version to upgrade */
			return FTL_LAYOUT_UPGRADE_CONTINUE;
		} else if (reg_ver == reg->current.version) {
			/* select the next region to upgrade */
			reg_type++;
			if (reg_type == FTL_LAYOUT_REGION_TYPE_MAX) {
				break;
			}
			ctx->reg++;
			ctx->upgrade++;
		} else {
			/* unknown version */
			assert(reg_ver > reg->current.version);
			FTL_ERRLOG(dev, "Region %d upgrade fault: version %"PRIu64"/%"PRIu64"\n", reg_type, reg_ver,
				   reg->current.version);
			return FTL_LAYOUT_UPGRADE_FAULT;
		}
	}

	return FTL_LAYOUT_UPGRADE_DONE;
}

int
ftl_layout_upgrade_init_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	if (!ctx->reg) {
		ctx->reg = &dev->layout.region[0];
		ctx->upgrade = &layout_upgrade_desc[0];
	}

	return layout_upgrade_select_next_region(dev, ctx);
}
