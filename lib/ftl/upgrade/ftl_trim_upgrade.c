/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_nv_cache.h"
#include "ftl_layout_upgrade.h"
#include "ftl_utils.h"

struct upgrade_ctx {
	struct ftl_md			*md;
	struct ftl_layout_region	reg;
};

static void
v0_to_v1_upgrade_cleanup(struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (ctx->md) {
		ftl_md_destroy(ctx->md, 0);
		ctx->md = NULL;
	}
}

static void
v0_to_v1_upgrade_finish(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx, int status)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	v0_to_v1_upgrade_cleanup(lctx);
	ftl_region_upgrade_completed(dev, lctx, ctx->reg.entry_size, ctx->reg.num_entries, status);
}

static void
v0_to_v1_upgrade_md_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx;

	v0_to_v1_upgrade_finish(dev, lctx, status);
}

static int
v0_to_v1_upgrade_setup_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx,
			   uint32_t type)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	/* Create the new NV cache metadata region - v2 */
	if (md_ops->region_open(dev, type, FTL_TRIM_LOG_VERSION_1, sizeof(struct ftl_trim_log), 1,
				&ctx->reg)) {
		return -1;
	}
	ctx->md = ftl_md_create(dev, ctx->reg.current.blocks, 0, ctx->reg.name, FTL_MD_CREATE_HEAP,
				&ctx->reg);
	if (!ctx->md) {
		return -1;
	}

	ctx->md->owner.cb_ctx = lctx;
	ctx->md->cb = v0_to_v1_upgrade_md_cb;

	return 0;
}

static int
v0_to_v1_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (v0_to_v1_upgrade_setup_ctx(dev, lctx, lctx->reg->type)) {
		goto error;
	}
	ftl_md_clear(ctx->md, 0, NULL);
	return 0;

error:
	v0_to_v1_upgrade_cleanup(lctx);
	return -1;
}

static int
v0_to_v1_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	assert(sizeof(struct ftl_nv_cache_chunk_md) == FTL_BLOCK_SIZE);

	if (ftl_region_major_upgrade_enabled(dev, region)) {
		return -1;
	}

	/* Create the new trim metadata region (v1) up front - this allocates a separate entry in the superblock and
	 * area on the cache for us. This is to reserve space for other region upgrades allocating new regions and it
	 * allows us to do an atomic upgrade of the whole region.
	 *
	 * If the upgrade is stopped by power failure/crash after the V1 region has been added, then the upgrade process
	 * will start again (since V0 still exists), but region_create will fail (since the v1 region has already been
	 * created). In such a case only verification of the region length by region_open is needed.
	 *
	 * Once the upgrade is fully done, the old v0 region entry will be removed from the SB and its area on the cache
	 * freed.
	 */


	if (md_ops->region_create(dev, region->type, FTL_TRIM_LOG_VERSION_1, 1) &&
	    md_ops->region_open(dev, region->type, FTL_TRIM_LOG_VERSION_1, sizeof(struct ftl_trim_log),
				1, NULL)) {
		return -1;
	}

	return 0;
}

struct ftl_region_upgrade_desc trim_log_upgrade_desc[] = {
	[FTL_TRIM_LOG_VERSION_0] = {
		.verify = v0_to_v1_upgrade_enabled,
		.ctx_size = sizeof(struct upgrade_ctx),
		.new_version = FTL_TRIM_LOG_VERSION_1,
		.upgrade = v0_to_v1_upgrade,
	},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(trim_log_upgrade_desc) == FTL_TRIM_LOG_VERSION_CURRENT,
		   "Missing NVC region upgrade descriptors");
