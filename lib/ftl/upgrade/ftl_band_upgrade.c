/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_band.h"
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
v2_upgrade_md_persist_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx;

	v2_upgrade_finish(dev, lctx, status);
}

static void
v2_upgrade_md_restore_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_layout_upgrade_ctx *lctx = md->owner.cb_ctx;
	struct upgrade_ctx *ctx = lctx->ctx;
	struct ftl_band_md *band = ftl_md_get_buffer(md);
	uint64_t move = sizeof(band->version);

	if (status) {
		v2_upgrade_finish(dev, lctx, status);
		return;
	}

	/* If the upgrade process is interrupted while only part of the update persisted,
	 * then the V1 version will be read from again and this section will rewrite the whole band md.
	 */
	for (uint64_t i = 0; i < dev->num_bands; i++, band++) {
		char *buffer = (char *)band;

		memmove(buffer + move, buffer, sizeof(*band) - move);
		band->version = FTL_BAND_VERSION_2;

		if (band->state != FTL_BAND_STATE_CLOSED && band->state != FTL_BAND_STATE_FREE) {
			v2_upgrade_finish(dev, lctx, -EINVAL);
			return;
		}
	}

	ctx->md->cb = v2_upgrade_md_persist_cb;
	ftl_md_set_region(ctx->md, &ctx->reg);
	ftl_md_persist(ctx->md);
}

static int
v2_upgrade_setup_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;

	assert(sizeof(struct ftl_band_md) == FTL_BLOCK_SIZE);

	if (lctx->reg->num_entries != dev->num_bands) {
		return -1;
	}

	/* Open metadata region */
	if (md_ops->region_open(dev, lctx->reg->type, FTL_BAND_VERSION_2, sizeof(struct ftl_band_md),
				dev->num_bands, &ctx->reg)) {
		return -1;
	}

	if (lctx->reg->current.blocks != ctx->reg.current.blocks) {
		return -1;
	}

	ctx->md = ftl_md_create(dev, lctx->reg->current.blocks, 0, ctx->reg.name, FTL_MD_CREATE_HEAP,
				lctx->reg);
	if (!ctx->md) {
		return -1;
	}

	ctx->md->owner.cb_ctx = lctx;
	ctx->md->cb = v2_upgrade_md_restore_cb;

	return 0;
}

static int
v2_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *lctx)
{
	struct upgrade_ctx *ctx = lctx->ctx;

	if (v2_upgrade_setup_ctx(dev, lctx)) {
		goto error;
	}
	/* At this point we're reading the contents of the v1 md */
	ftl_md_restore(ctx->md);
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

	/* Create the new band metadata region (v2) up front - this allocates a separate entry in the superblock and
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
	if (md_ops->region_create(dev, region->type, FTL_BAND_VERSION_2, dev->num_bands) &&
	    md_ops->region_open(dev, region->type, FTL_BAND_VERSION_2, sizeof(struct ftl_band_md),
				dev->num_bands, NULL)) {
		return -1;
	}

	return 0;
}

struct ftl_region_upgrade_desc band_upgrade_desc[] = {
	[FTL_BAND_VERSION_0] = {
		.verify = ftl_region_upgrade_disabled,
	},
	[FTL_BAND_VERSION_1] = {
		.verify = v1_to_v2_upgrade_enabled,
		.ctx_size = sizeof(struct upgrade_ctx),
		.new_version = FTL_BAND_VERSION_2,
		.upgrade = v2_upgrade,
	},
};

SPDK_STATIC_ASSERT(SPDK_COUNTOF(band_upgrade_desc) == FTL_BAND_VERSION_CURRENT,
		   "Missing band region upgrade descriptors");
