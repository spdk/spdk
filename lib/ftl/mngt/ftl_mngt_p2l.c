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

#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"
#include "ftl_core.h"

struct ftl_mngt_p2l_md_ctx {
	struct ftl_mngt *mngt;
	int             md_region;
	int             status;
};

static void
ftl_p2l_wipe_md_region(struct spdk_ftl_dev *dev, struct ftl_mngt_p2l_md_ctx *ctx);

void
ftl_mngt_p2l_init_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (!ftl_p2l_ckpt_init(dev)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

void
ftl_mngt_p2l_deinit_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
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

	if (ctx->md_region == ftl_layout_region_type_p2l_ckpt_max) {
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

	assert(ctx->md_region >= ftl_layout_region_type_p2l_ckpt_min);
	assert(ctx->md_region <= ftl_layout_region_type_p2l_ckpt_max);

	if (!md) {
		ftl_mngt_fail_step(ctx->mngt);
		return;
	}

	md->owner.cb_ctx = ctx;
	md->cb = ftl_p2l_wipe_md_region_cb;
	ftl_md_persist(md);
}

void
ftl_mngt_p2l_wipe(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_mngt_alloc_step_cntx(mngt, sizeof(struct ftl_mngt_p2l_md_ctx))) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	struct ftl_mngt_p2l_md_ctx *ctx = ftl_mngt_get_step_cntx(mngt);
	ctx->mngt = mngt;
	ctx->md_region = ftl_layout_region_type_p2l_ckpt_min;
	ftl_p2l_wipe_md_region(dev, ctx);
}

void
ftl_mngt_p2l_free_bufs(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_md *md;
	int region_type;
	for (region_type = ftl_layout_region_type_p2l_ckpt_min;
	     region_type <= ftl_layout_region_type_p2l_ckpt_max;
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
ftl_mngt_p2l_restore_ckpt(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
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

	if (ftl_mngt_alloc_step_cntx(mngt, sizeof(struct ftl_mngt_p2l_md_ctx))) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ctx = ftl_mngt_get_step_cntx(mngt);
	ctx->mngt = mngt;
	ctx->md_region = 0;
	ctx->status = 0;

	for (md_region = ftl_layout_region_type_p2l_ckpt_min;
	     md_region <= ftl_layout_region_type_p2l_ckpt_max; md_region++) {
		md = layout->md[md_region];
		assert(md);
		md->owner.cb_ctx = ctx;
		md->cb = ftl_mngt_p2l_restore_ckpt_cb;
		ftl_md_restore(md);
	}
}
