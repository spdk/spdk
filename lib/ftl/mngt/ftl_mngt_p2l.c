/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"
#include "ftl_core.h"

struct ftl_mngt_p2l_md_ctx {
	struct ftl_mngt_process *mngt;
	int md_region;
	int status;
};

static void ftl_p2l_wipe_md_region(struct spdk_ftl_dev *dev, struct ftl_mngt_p2l_md_ctx *ctx);

void
ftl_mngt_p2l_init_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (!ftl_p2l_ckpt_init(dev)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

void
ftl_mngt_p2l_deinit_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_p2l_ckpt_deinit(dev);
	ftl_mngt_next_step(mngt);
}

static void
ftl_p2l_wipe_md_region_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_p2l_md_ctx *ctx = md->owner.cb_ctx;

	if (status) {
		ftl_mngt_fail_step(ctx->mngt);
		return;
	}

	if (ctx->md_region == FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX) {
		ftl_mngt_next_step(ctx->mngt);
		return;
	}

	ctx->md_region++;
	ftl_p2l_wipe_md_region(dev, ctx);
}

static void
ftl_p2l_wipe_md_region(struct spdk_ftl_dev *dev, struct ftl_mngt_p2l_md_ctx *ctx)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_md *md = layout->md[ctx->md_region];

	assert(ctx->md_region >= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN);
	assert(ctx->md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX);

	if (!md) {
		ftl_mngt_fail_step(ctx->mngt);
		return;
	}

	md->owner.cb_ctx = ctx;
	md->cb = ftl_p2l_wipe_md_region_cb;
	ftl_md_persist(md);
}

void
ftl_mngt_p2l_wipe(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_p2l_md_ctx *ctx;

	if (ftl_mngt_alloc_step_ctx(mngt, sizeof(struct ftl_mngt_p2l_md_ctx))) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ctx = ftl_mngt_get_step_ctx(mngt);
	ctx->mngt = mngt;
	ctx->md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	ftl_p2l_wipe_md_region(dev, ctx);
}

void
ftl_mngt_p2l_free_bufs(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md;
	int region_type;

	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		md = dev->layout.md[region_type];
		assert(md);
		ftl_md_free_buf(md, ftl_md_destroy_region_flags(dev, dev->layout.region[region_type].type));
	}
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_p2l_restore_ckpt_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_p2l_md_ctx *ctx = md->owner.cb_ctx;
	assert(ctx);
	if (status) {
		ctx->status = status;
	}

	if (++ctx->md_region == FTL_LAYOUT_REGION_TYPE_P2L_COUNT) {
		if (!ctx->status) {
			ftl_mngt_next_step(ctx->mngt);
		} else {
			ftl_mngt_fail_step(ctx->mngt);
		}
	}
}

void
ftl_mngt_p2l_restore_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_md *md;
	struct ftl_mngt_p2l_md_ctx *ctx;
	int md_region;

	if (ftl_fast_startup(dev)) {
		FTL_NOTICELOG(dev, "SHM: skipping p2l ckpt restore\n");
		ftl_mngt_next_step(mngt);
		return;
	}

	if (ftl_mngt_alloc_step_ctx(mngt, sizeof(struct ftl_mngt_p2l_md_ctx))) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ctx = ftl_mngt_get_step_ctx(mngt);
	ctx->mngt = mngt;
	ctx->md_region = 0;
	ctx->status = 0;

	for (md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX; md_region++) {
		md = layout->md[md_region];
		assert(md);
		md->owner.cb_ctx = ctx;
		md->cb = ftl_mngt_p2l_restore_ckpt_cb;
		ftl_md_restore(md);
	}
}
