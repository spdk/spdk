/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_nv_cache.h"
#include "ftl_layout_upgrade.h"
#include "ftl_utils.h"

struct upgrade_ctx {
	struct ftl_md			*md_v2;
	struct ftl_layout_region	reg_v2;
};

static void
v1_to_v2_upgrade_cleanup(struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (ctx->md_v2) {
		ftl_md_destroy(ctx->md_v2, 0);
		ctx->md_v2 = NULL;
	}
}

static void
v1_to_v2_upgrade_finish(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx, int status)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	v1_to_v2_upgrade_cleanup(lctx);
	ftl_region_upgrade_completed(dev, lctx, ctx->reg_v2.entry_size, ctx->reg_v2.num_entries, status);
}

static void
v1_to_v2_upgrade_set(struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	struct ftl_nv_cache_chunk_md *md = ftl_md_get_buffer(ctx->md_v2);

	assert(sizeof(struct ftl_nv_cache_chunk_md) == FTL_BLOCK_SIZE);
	for (uint64_t i = 0; i < ctx->reg_v2.current.blocks; i++, md++) {
		ftl_nv_cache_chunk_md_initialize(md);
	}
}

static void
v1_to_v2_upgrade_md_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx;

	v1_to_v2_upgrade_finish(dev, lctx, status);
}

static int
v1_to_v2_upgrade_setup_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx,
			   uint32_t type)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	assert(sizeof(struct ftl_nv_cache_chunk_md) == FTL_BLOCK_SIZE);

	/* Create the new NV cache metadata region - v2 */
	if (md_ops->region_open(dev, type, FTL_NVC_VERSION_2, sizeof(struct ftl_nv_cache_chunk_md),
				dev->layout.nvc.chunk_count, &ctx->reg_v2)) {
		return -1;
	}
	ctx->md_v2 = ftl_md_create(dev, ctx->reg_v2.current.blocks, 0, ctx->reg_v2.name, FTL_MD_CREATE_HEAP,
				   &ctx->reg_v2);
	if (!ctx->md_v2) {
		return -1;
	}

	ctx->md_v2->owner.cb_ctx = lctx;
	ctx->md_v2->cb = v1_to_v2_upgrade_md_cb;
	v1_to_v2_upgrade_set(lctx);

	return 0;
}

static int
v1_to_v2_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	/*
	 * Chunks at this point should be fully drained of user data (major upgrade). This means that it's safe to reinitialize
	 * the MD and fully change the structure layout (we're not interpreting the metadata contents at this point).
	 * Once we're done the version of the region in the superblock will be updated.
	 */

	if (v1_to_v2_upgrade_setup_ctx(dev, lctx, lctx->reg->type)) {
		goto error;
	}
	ftl_md_persist(ctx->md_v2);
	return 0;

error:
	v1_to_v2_upgrade_cleanup(lctx);
	return -1;
}

static int
v1_to_v2_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	assert(sizeof(struct ftl_nv_cache_chunk_md) == FTL_BLOCK_SIZE);

	if (ftl_region_major_upgrade_enabled(dev, region)) {
		return -1;
	}

	/* Create the new NV cache metadata region (v2) up front - this allocates a separate entry in the superblock and
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
	if (md_ops->region_create(dev, region->type, FTL_NVC_VERSION_2, dev->layout.nvc.chunk_count) &&
	    md_ops->region_open(dev, region->type, FTL_NVC_VERSION_2, sizeof(struct ftl_nv_cache_chunk_md),
				dev->layout.nvc.chunk_count, NULL)) {
		return -1;
	}

	return 0;
}

struct ftl_region_upgrade_desc nvc_upgrade_desc[] = {
	[FTL_NVC_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled,
	},
	[FTL_NVC_VERSION_1] = {
		.verify = v1_to_v2_upgrade_enabled,
		.ctx_size = sizeof(struct upgrade_ctx),
		.new_version = FTL_NVC_VERSION_2,
		.upgrade = v1_to_v2_upgrade,
	},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(nvc_upgrade_desc) == FTL_NVC_VERSION_CURRENT,
		   "Missing NVC region upgrade descriptors");
