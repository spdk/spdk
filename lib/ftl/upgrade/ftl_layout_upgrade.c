/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
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
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_SB_BASE] = {
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_L2P] = {},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD] = {
		.latest_ver = FTL_BAND_VERSION_CURRENT,
		.count = FTL_BAND_VERSION_CURRENT,
		.desc = band_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR] = {
		.latest_ver = FTL_BAND_VERSION_CURRENT,
		.count = FTL_BAND_VERSION_CURRENT,
		.desc = band_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_VALID_MAP] = {},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD] = {
		.latest_ver = FTL_NVC_VERSION_CURRENT,
		.count = FTL_NVC_VERSION_CURRENT,
		.desc = nvc_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR] = {
		.latest_ver = FTL_NVC_VERSION_CURRENT,
		.count = FTL_NVC_VERSION_CURRENT,
		.desc = nvc_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_DATA_NVC] = {},
	[FTL_LAYOUT_REGION_TYPE_DATA_BASE] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
		.desc = p2l_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD] = {},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR] = {},
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
	ver = ctx->reg->current.version;
	if (ver > ctx->upgrade->latest_ver) {
		FTL_ERRLOG(dev, "Unknown region version\n");
		return -1;
	}

	while (ver < ctx->upgrade->latest_ver) {
		int rc = ctx->upgrade->desc[ver].verify(dev, ctx->reg);
		if (rc) {
			return rc;
		}
		ftl_bug(ver > ctx->upgrade->desc[ver].new_version);
		ftl_bug(ctx->upgrade->desc[ver].new_version > ctx->upgrade->latest_ver);
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
	assert(ctx->reg->current.version <= ctx->upgrade->latest_ver);
	ver = ctx->reg->current.version;
	if (ver < ctx->upgrade->latest_ver) {
		ctx->next_reg_ver = ctx->upgrade->desc[ver].new_version;
		rc = ctx->upgrade->desc[ver].upgrade(dev, ctx);
	}
	return rc;
}

void
ftl_region_upgrade_completed(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx,
			     uint64_t entry_size, uint64_t num_entries, int status)
{
	int rc;

	assert(ctx->reg);
	assert(ctx->reg->current.version < ctx->next_reg_ver);
	assert(ctx->next_reg_ver <= ctx->upgrade->latest_ver);

	if (!status) {
		if (ctx->reg->type != FTL_LAYOUT_REGION_TYPE_SB) {
			/* Superblock region is always default-created in the latest version - see ftl_layout_setup_superblock() */
			rc = ftl_superblock_md_layout_upgrade_region(dev, ctx->reg, ctx->next_reg_ver);
			if (entry_size && num_entries) {
				dev->layout.region[ctx->reg->type].entry_size = entry_size;
				dev->layout.region[ctx->reg->type].num_entries = num_entries;
			}

			ftl_bug(rc != 0);
		}

		ctx->reg->current.version = ctx->next_reg_ver;
	}

	if (ctx->cb) {
		ctx->cb(dev, ctx->cb_ctx, status);
	}
}

int
ftl_layout_verify(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_upgrade_ctx ctx = {0};
	enum ftl_layout_region_type reg_type;

	/**
	 * Upon SB upgrade some MD regions may be missing in the MD layout blob - e.g. v3 to v5, FTL_LAYOUT_REGION_TYPE_DATA_BASE.
	 * The regions couldn't have be added in the SB upgrade path, as the FTL layout wasn't initialized at that point.
	 * Now that the FTL layout is initialized, add the missing regions and store the MD layout blob again.
	 */

	if (ftl_validate_regions(dev, layout)) {
		return -1;
	}

	for (reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX; reg_type++) {
		ctx.reg = ftl_layout_region_get(dev, reg_type);
		ctx.upgrade = &layout_upgrade_desc[reg_type];
		if (!ctx.reg) {
			continue;
		}

		if (region_verify(dev, &ctx)) {
			return -1;
		}
	}

	return 0;
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
	struct ftl_layout_region *reg = ftl_layout_region_get(dev, FTL_LAYOUT_REGION_TYPE_SB);
	int rc;

	ctx.reg = reg;
	ctx.upgrade = &layout_upgrade_desc[FTL_LAYOUT_REGION_TYPE_SB];
	reg->current.version = dev->sb->header.version;

	rc = region_verify(dev, &ctx);
	if (rc) {
		return rc;
	}

	while (reg->current.version < ctx.upgrade->latest_ver) {
		rc = ftl_region_upgrade(dev, &ctx);
		if (rc) {
			return rc;
		}
		/* SB upgrades are all synchronous */
		ftl_region_upgrade_completed(dev, &ctx, 0, 0, rc);
	}

	/* The mirror shares the same DMA buf, so it is automatically updated upon SB store */
	dev->layout.region[FTL_LAYOUT_REGION_TYPE_SB_BASE].current.version = reg->current.version;
	return 0;
}

static int
layout_upgrade_select_next_region(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *reg;
	uint64_t reg_ver, reg_latest_ver;
	uint32_t reg_type = ctx->reg->type;

	while (reg_type != FTL_LAYOUT_REGION_TYPE_MAX) {
		assert(ctx->reg);
		assert(ctx->upgrade);
		reg = ctx->reg;
		reg_latest_ver = ctx->upgrade->latest_ver;
		reg_ver = reg->current.version;

		if (reg_ver == reg_latest_ver || reg->type == FTL_LAYOUT_REGION_TYPE_INVALID) {
			/* select the next region to upgrade */
			reg_type++;
			if (reg_type == FTL_LAYOUT_REGION_TYPE_MAX) {
				break;
			}
			ctx->reg++;
			ctx->upgrade++;
		} else if (reg_ver < reg_latest_ver) {
			/* qualify region version to upgrade */
			return FTL_LAYOUT_UPGRADE_CONTINUE;
		} else {
			/* unknown version */
			assert(reg_ver <= reg_latest_ver);
			FTL_ERRLOG(dev, "Region %d upgrade fault: version %"PRIu64"/%"PRIu64"\n", reg_type, reg_ver,
				   reg_latest_ver);
			return FTL_LAYOUT_UPGRADE_FAULT;
		}
	}

	return FTL_LAYOUT_UPGRADE_DONE;
}

int
ftl_layout_upgrade_init_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	if (!ctx->reg) {
		ctx->reg = ftl_layout_region_get(dev, 0);
		ctx->upgrade = &layout_upgrade_desc[0];
		static_assert(FTL_LAYOUT_REGION_TYPE_SB == 0, "Invalid SB region type");
	}

	return layout_upgrade_select_next_region(dev, ctx);
}

uint64_t
ftl_layout_upgrade_region_get_latest_version(enum ftl_layout_region_type reg_type)
{
	assert(reg_type < FTL_LAYOUT_REGION_TYPE_MAX);
	return layout_upgrade_desc[reg_type].latest_ver;
}
