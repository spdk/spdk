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

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "upgrade/ftl_layout_upgrade.h"

struct ftl_mngt_upgrade_ctx {
	struct ftl_mngt *parent;
	struct ftl_mngt *mngt;
	struct ftl_layout_upgrade_ctx upgrade_ctx;
};

static void region_upgrade_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_mngt_upgrade_ctx *ctx = _ctx;
	if (ctx->upgrade_ctx.ctx) {
		free(ctx->upgrade_ctx.ctx);
		ctx->upgrade_ctx.ctx = NULL;
	}

	if (status) {
		FTL_ERRLOG(dev, "Upgrade failed for region %d (rc=%d)\n", ctx->upgrade_ctx.reg->type, status);
		ftl_mngt_fail_step(ctx->mngt);
	} else {
		ftl_mngt_next_step(ctx->mngt);
	}
}

static void region_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_mngt_upgrade_ctx *ctx = ftl_mngt_get_caller_context(mngt);
	size_t ctx_size = ctx->upgrade_ctx.upgrade->reg_upgrade_desc[ctx->upgrade_ctx.reg->prev.version].ctx_size;

	int rc = -1;
	assert(ctx->upgrade_ctx.ctx == NULL);
	if (ctx_size) {
		ctx->upgrade_ctx.ctx = calloc(1, ctx_size);
		if (!ctx->upgrade_ctx.ctx) {
			goto exit;
		}
	}
	ctx->upgrade_ctx.cb = region_upgrade_cb;
	ctx->upgrade_ctx.cb_ctx = ctx;
	ctx->mngt = mngt;

	rc = ftl_region_upgrade(dev, &ctx->upgrade_ctx);
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

static void layout_upgrade_cb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_mngt_upgrade_ctx *ctx = ftl_mngt_get_caller_context(mngt);
	int rc = ftl_mngt_get_status(mngt);
	if (rc) {
		if (ctx->upgrade_ctx.ctx) {
			free(ctx->upgrade_ctx.ctx);
		}
		ftl_mngt_fail_step(ctx->parent);
		return;
	}

	// go back to ftl_mngt_upgrade() step and select the next region/version to upgrade
	ftl_mngt_continue_step(ctx->parent);
}

static void layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt *parent)
{
	struct ftl_mngt_upgrade_ctx *ctx = ftl_mngt_get_process_cntx(parent);
	ctx->parent = parent;
	int rc = ftl_layout_upgrade_init_ctx(dev, &ctx->upgrade_ctx);
	switch (rc) {
		case ftl_layout_upgrade_continue:
			if (!ftl_mngt_execute(dev, &desc_region_upgrade, layout_upgrade_cb, ctx)) {
				return;
			}

			ftl_mngt_fail_step(parent);
			break;

		case ftl_layout_upgrade_done:
			if (ftl_layout_dump(dev)) {
				FTL_ERRLOG(dev, "MD layout verification failed after upgrade.\n");
				ftl_mngt_fail_step(parent);
			} else {
				ftl_mngt_next_step(parent);
			}
			break;

		case ftl_layout_upgrade_fault:
			ftl_mngt_fail_step(parent);
			break;
	}
	if (ctx->upgrade_ctx.ctx) {
		free(ctx->upgrade_ctx.ctx);
	}
}

static const struct ftl_mngt_process_desc desc_layout_upgrade = {
	.name = "FTL layout upgrade",
	.arg_size = sizeof(struct ftl_mngt_upgrade_ctx),
	.steps = {
		{
			.name = "Layout upgrade",
			.action = layout_upgrade,
		},
		{}
	}
};


void ftl_mngt_layout_verify(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_layout_verify(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_layout_upgrade(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_mngt_call(mngt, &desc_layout_upgrade);
}
