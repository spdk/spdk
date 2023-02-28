/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "upgrade/ftl_layout_upgrade.h"

struct ftl_mngt_upgrade_ctx {
	struct ftl_mngt_process *parent;
	struct ftl_mngt_process *mngt;
	struct ftl_layout_upgrade_ctx upgrade_ctx;
};

static void
region_upgrade_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_mngt_upgrade_ctx *ctx = _ctx;

	free(ctx->upgrade_ctx.ctx);
	ctx->upgrade_ctx.ctx = NULL;

	if (status) {
		FTL_ERRLOG(dev, "Upgrade failed for region %d (rc=%d)\n", ctx->upgrade_ctx.reg->type, status);
		ftl_mngt_fail_step(ctx->mngt);
	} else {
		ftl_mngt_next_step(ctx->mngt);
	}
}

static void
region_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_mngt_upgrade_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	struct ftl_layout_upgrade_ctx *upgrade_ctx = &ctx->upgrade_ctx;
	size_t ctx_size = upgrade_ctx->upgrade->desc[upgrade_ctx->reg->prev.version].ctx_size;
	int rc = -1;

	assert(upgrade_ctx->ctx == NULL);
	if (ctx_size) {
		upgrade_ctx->ctx = calloc(1, ctx_size);
		if (!upgrade_ctx->ctx) {
			goto exit;
		}
	}
	upgrade_ctx->cb = region_upgrade_cb;
	upgrade_ctx->cb_ctx = ctx;
	ctx->mngt = mngt;

	rc = ftl_region_upgrade(dev, upgrade_ctx);
exit:
	if (rc) {
		region_upgrade_cb(dev, ctx, rc);
	}
}

static const struct ftl_mngt_process_desc desc_region_upgrade = {
	.name = "FTL region upgrade",
	.steps = {
		{
			.name = "Region upgrade",
			.action = region_upgrade,
		},
		{
			.name = "Persist superblock",
			.action = ftl_mngt_persist_superblock,
		},
		{}
	}
};

static void
layout_upgrade_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_mngt_upgrade_ctx *ctx = _ctx;

	if (status) {
		free(ctx->upgrade_ctx.ctx);
		ctx->upgrade_ctx.ctx = NULL;
		ftl_mngt_fail_step(ctx->parent);
		return;
	}

	/* go back to ftl_mngt_upgrade() step and select the next region/version to upgrade */
	ftl_mngt_continue_step(ctx->parent);
}

static void
layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *parent)
{
	struct ftl_mngt_upgrade_ctx *ctx = ftl_mngt_get_process_ctx(parent);
	int rc;

	ctx->parent = parent;
	rc = ftl_layout_upgrade_init_ctx(dev, &ctx->upgrade_ctx);

	switch (rc) {
	case FTL_LAYOUT_UPGRADE_CONTINUE:
		if (!ftl_mngt_process_execute(dev, &desc_region_upgrade, layout_upgrade_cb, ctx)) {
			return;
		}

		ftl_mngt_fail_step(parent);
		break;

	case FTL_LAYOUT_UPGRADE_DONE:
		if (ftl_upgrade_layout_dump(dev)) {
			FTL_ERRLOG(dev, "MD layout verification failed after upgrade.\n");
			ftl_mngt_fail_step(parent);
		} else {
			ftl_mngt_next_step(parent);
		}
		break;

	case FTL_LAYOUT_UPGRADE_FAULT:
		ftl_mngt_fail_step(parent);
		break;
	}
	if (ctx->upgrade_ctx.ctx) {
		free(ctx->upgrade_ctx.ctx);
	}
}

static const struct ftl_mngt_process_desc desc_layout_upgrade = {
	.name = "FTL layout upgrade",
	.ctx_size = sizeof(struct ftl_mngt_upgrade_ctx),
	.steps = {
		{
			.name = "Layout upgrade",
			.action = layout_upgrade,
		},
		{}
	}
};


void
ftl_mngt_layout_verify(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_layout_verify(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_layout_upgrade);
}
