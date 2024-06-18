/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "mngt/ftl_mngt.h"
#include "mngt/ftl_mngt_steps.h"
#include "ftl_layout_upgrade.h"

struct upgrade_ctx {
	struct ftl_md			*md;
	struct ftl_layout_region	reg;
};

static void
v2_upgrade_cleanup(struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (ctx->md) {
		ftl_md_destroy(ctx->md, 0);
		ctx->md = NULL;
	}
}

static void
v2_upgrade_finish(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx, int status)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	v2_upgrade_cleanup(lctx);
	ftl_region_upgrade_completed(dev, lctx, ctx->reg.entry_size, ctx->reg.num_entries, status);
}

static void
v2_upgrade_md_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx;

	v2_upgrade_finish(dev, lctx, status);
}

static int
v2_upgrade_setup_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx, uint32_t type)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	/* TODO Add validation if no open bands */

	/* Open metadata region */
	if (md_ops->region_open(dev, lctx->reg->type, FTL_P2L_VERSION_2,
				sizeof(struct ftl_p2l_ckpt_page_no_vss),
				dev->layout.p2l.ckpt_pages, &ctx->reg)) {
		return -1;
	}

	ctx->md = ftl_md_create(dev, ctx->reg.current.blocks, 0, ctx->reg.name, FTL_MD_CREATE_HEAP,
				&ctx->reg);
	if (!ctx->md) {
		return -1;
	}

	ctx->md->owner.cb_ctx = lctx;
	ctx->md->cb = v2_upgrade_md_cb;

	return 0;
}

static int
v2_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (v2_upgrade_setup_ctx(dev, lctx, lctx->reg->type)) {
		goto error;
	}
	ftl_md_clear(ctx->md, 0, NULL);
	return 0;
error:
	v2_upgrade_cleanup(lctx);
	return -1;
}

static int
v1_to_v2_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	if (ftl_region_major_upgrade_enabled(dev, region)) {
		return -1;
	}

	/* Create the new P2L metadata region (v2) up front - this allocates a separate entry in the superblock and
	 * area on the cache for us. This is to reserve space for other region upgrades allocating new regions and it
	 * allows us to do an atomic upgrade of the whole region.
	 *
	 * If the upgrade is stopped by power failure/crash after the V2 region has been added, then the upgrade process
	 * will start again (since V1 still exists), but region_create will fail (since the v2 region has already been
	 * created). In such a case only verification of the region length by region_open is needed.
	 *
	 * Once the upgrade is fully done, the old v1 region entry will be removed from the SB and its area on the cache
	 * freed.
	 */
	if (md_ops->region_create(dev, region->type, FTL_P2L_VERSION_2, dev->layout.p2l.ckpt_pages) &&
	    md_ops->region_open(dev, region->type, FTL_P2L_VERSION_2, sizeof(struct ftl_p2l_ckpt_page_no_vss),
				dev->layout.p2l.ckpt_pages, NULL)) {
		return -1;
	}

	return 0;
}

struct ftl_region_upgrade_desc p2l_upgrade_desc[] = {
	[FTL_P2L_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled
	},
	[FTL_P2L_VERSION_1] = {
		.verify = v1_to_v2_upgrade_enabled,
		.ctx_size = sizeof(struct upgrade_ctx),
		.new_version = FTL_P2L_VERSION_2,
		.upgrade = v2_upgrade,
	},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(p2l_upgrade_desc) == FTL_P2L_VERSION_CURRENT,
		   "Missing P2L region upgrade descriptors");
