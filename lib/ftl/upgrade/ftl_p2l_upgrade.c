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

#include "mngt/ftl_mngt.h"
#include "mngt/ftl_mngt_steps.h"
#include "ftl_layout_upgrade.h"

struct p2l_ckpt_vss_v0 {
	uint64_t	seq_id;
};

struct p2l_v0_to_v1_ctx {
	uint64_t current_p2l;
	struct ftl_md_io_entry_ctx	md_entry_ctx;
	struct ftl_layout_region *region;
	struct spdk_ftl_dev		 *dev;
	struct ftl_p2l_ckpt_page *p2l_page;
	union ftl_md_vss		 *vss_md_page;
};

static void upgrade_v0_to_v1_read_cb(int status, void *cb_arg);
static void upgrade_v0_to_v1_write_cb(int status, void *cb_arg);

static void free_ctx(struct p2l_v0_to_v1_ctx *p2l_ctx)
{
	spdk_dma_free(p2l_ctx->p2l_page);
	spdk_dma_free(p2l_ctx->vss_md_page);
}

static void read_next_p2l_entry(struct ftl_layout_upgrade_ctx *layout_ctx)
{
	struct p2l_v0_to_v1_ctx *p2l_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = p2l_ctx->dev;
	struct ftl_layout_region *region = p2l_ctx->region;
	uint64_t region_translated = p2l_ctx->current_p2l * region->entry_size;

	if (p2l_ctx->current_p2l < region->num_entries) {
		ftl_md_read_entry(dev->layout.md[region->type], p2l_ctx->current_p2l, p2l_ctx->p2l_page,
				p2l_ctx->vss_md_page, upgrade_v0_to_v1_read_cb, layout_ctx, &p2l_ctx->md_entry_ctx);
	} else if (region_translated < region->current.blocks) {
		// We've upgraded all the entries we want, but due to alignment there's still part of the region
		// where we need to bump the version in VSS. Clear the main/VSS buffer otherwise
		memset(p2l_ctx->p2l_page, 0, sizeof(struct ftl_p2l_ckpt_page));
		memset(p2l_ctx->vss_md_page, 0, sizeof(union ftl_md_vss));
		p2l_ctx->vss_md_page->version.md_version = FTL_P2L_VERSION_1;

		ftl_md_persist_entry(dev->layout.md[region->type], p2l_ctx->current_p2l, p2l_ctx->p2l_page,
				p2l_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &p2l_ctx->md_entry_ctx);
	} else {
		free_ctx(p2l_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, 0);
	}
}

static void upgrade_v0_to_v1_write_cb(int status, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct p2l_v0_to_v1_ctx *p2l_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = p2l_ctx->dev;

	if (spdk_unlikely(status)) {
		free_ctx(p2l_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, status);
		return;
	}

	p2l_ctx->current_p2l++;
	read_next_p2l_entry(layout_ctx);
}

static void upgrade_v0_to_v1_read_cb(int status, void *cb_arg)
{
	struct ftl_layout_upgrade_ctx *layout_ctx= cb_arg;
	struct p2l_v0_to_v1_ctx *p2l_ctx = layout_ctx->ctx;
	struct spdk_ftl_dev *dev = p2l_ctx->dev;
	struct ftl_layout_region *region = p2l_ctx->region;
	struct ftl_p2l_ckpt_page *p2l_page = p2l_ctx->p2l_page;
	union ftl_md_vss		 *md_page = p2l_ctx->vss_md_page;

	if (spdk_unlikely(status)) {
		free_ctx(p2l_ctx);
		ftl_region_upgrade_completed(dev, layout_ctx, status);
		return;
	}

	if (p2l_ctx->vss_md_page->version.md_version == FTL_P2L_VERSION_1) {
		p2l_ctx->current_p2l++;
		read_next_p2l_entry(layout_ctx);
	} else {
		md_page->p2l_ckpt.p2l_checksum = spdk_crc32c_update(p2l_page,
				FTL_NUM_LBA_IN_BLOCK * sizeof(struct ftl_lba_map_entry), 0);
		p2l_ctx->vss_md_page->version.md_version = FTL_P2L_VERSION_1;

		ftl_md_persist_entry(dev->layout.md[region->type], p2l_ctx->current_p2l, p2l_ctx->p2l_page,
				p2l_ctx->vss_md_page, upgrade_v0_to_v1_write_cb, layout_ctx, &p2l_ctx->md_entry_ctx);
	}
}

static int
ftl_p2l_upgrade_v0_to_v1(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *layout_ctx)
{
	struct p2l_v0_to_v1_ctx *p2l_ctx = layout_ctx->ctx;

	p2l_ctx->dev = dev;
	p2l_ctx->region = layout_ctx->reg;
	p2l_ctx->current_p2l = 0;
	p2l_ctx->p2l_page = spdk_zmalloc(sizeof(struct ftl_p2l_ckpt_page), FTL_BLOCK_SIZE, NULL,
			   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	p2l_ctx->vss_md_page = ftl_md_vss_buf_alloc(layout_ctx->reg, 1);

	if (!p2l_ctx->p2l_page || !p2l_ctx->vss_md_page) {
		free_ctx(p2l_ctx);
		return -ENOMEM;
	}

	read_next_p2l_entry(layout_ctx);
	return 0;
}

struct ftl_region_upgrade_desc p2l_upgrade_desc[] = {
	[FTL_P2L_VERSION_0] = {.verify = ftl_region_upgrade_enabled,
		.upgrade = ftl_p2l_upgrade_v0_to_v1,
		.new_version = FTL_P2L_VERSION_1,
		.ctx_size = sizeof (struct p2l_v0_to_v1_ctx)},
};

SPDK_STATIC_ASSERT(sizeof(p2l_upgrade_desc) / sizeof(*p2l_upgrade_desc) == FTL_P2L_VERSION_CURRENT,
	"Missing P2L region upgrade descriptors");
