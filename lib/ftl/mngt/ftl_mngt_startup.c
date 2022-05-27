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

static const struct ftl_mngt_process_desc desc_startup;
static const struct ftl_mngt_process_desc desc_first_start;
static const struct ftl_mngt_process_desc desc_restore;
static const struct ftl_mngt_process_desc desc_clean_start;

static void ftl_mngt_select_startup_mode(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_mngt_call(mngt, &desc_first_start);
	} else {
		ftl_mngt_call(mngt, &desc_restore);
	}
}

static void ftl_mngt_select_restore_mode(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	if (dev->sb->clean) {
		ftl_mngt_call(mngt, &desc_clean_start);
	} else {
		ftl_mngt_recover(dev, mngt);
	}
}

static const struct ftl_mngt_process_desc desc_startup = {
	.name = "FTL startup",
	.steps = {
		{
			.name = "Check configuration",
			.action = ftl_mngt_check_conf,
		},
		{
			.name = "Open base bdev",
			.action = ftl_mngt_open_base_bdev,
			.cleanup = ftl_mngt_close_base_bdev
		},
		{
			.name = "Open cache bdev",
			.action = ftl_mngt_open_cache_bdev,
			.cleanup = ftl_mngt_close_cache_bdev
		},
#ifdef SPDK_FTL_VSS_EMU
		{
			.name = "Initialize VSS emu",
			.action = ftl_mngt_md_init_vss_emu,
			.cleanup = ftl_mngt_md_deinit_vss_emu
		},
#endif
		{
			.name = "Initialize superblock",
			.action = ftl_mngt_superblock_init,
			.cleanup = ftl_mngt_superblock_deinit
		},
		{
			.name = "Initialize memory pools",
			.action = ftl_mngt_init_mem_pools,
			.cleanup = ftl_mngt_deinit_mem_pools
		},
		{
			.name = "Initialize bands",
			.action = ftl_mngt_init_bands,
			.cleanup = ftl_mngt_deinit_bands
		},
		{
			.name = "Initialize IO channel",
			.action = ftl_mngt_init_io_channel,
			.cleanup = ftl_mngt_deinit_io_channel
		},
		{
			.name = "Initialize zones",
			.action = ftl_mngt_init_zone

		},
		{
			.name = "Decorate bands",
			.action = ftl_mngt_decorate_bands
		},
		{
			.name = "Initialize layout",
			.action = ftl_mngt_init_layout
		},
		{
			.name = "Initialize metadata",
			.action = ftl_mngt_init_md,
			.cleanup = ftl_mngt_deinit_md
		},
		{
			.name = "Initialize NV cache",
			.action = ftl_mngt_init_nv_cache,
			.cleanup = ftl_mngt_deinit_nv_cache
		},
		{
			.name = "Initialize valid map",
			.action = ftl_mngt_init_vld_map,
			.cleanup = ftl_mngt_deinit_vld_map
		},
		{
			.name = "Initialize trim map",
			.action = ftl_mngt_init_unmap_map,
			.cleanup = ftl_mngt_deinit_unmap_map
		},
		{
			.name = "Initialize bands metadata",
			.action = ftl_mngt_init_bands_md,
			.cleanup = ftl_mngt_deinit_bands_md
		},
		{
			.name = "Initialize reloc",
			.action = ftl_mngt_init_reloc,
			.cleanup = ftl_mngt_deinit_reloc
		},
		{
			.name = "Select startup mode",
			.action = ftl_mngt_select_startup_mode
		},
		{}
	}
};

static const struct ftl_mngt_process_desc desc_first_start = {
	.name = "FTL first start",
	.steps = {
		{
			.name = "Initialize L2P",
			.action = ftl_mngt_init_l2p,
			.cleanup = ftl_mngt_deinit_l2p
		},
		{
			.name = "Clear L2P",
			.action = ftl_mngt_clear_l2p,
			.cleanup = ftl_mngt_clear_l2p
		},
		{
			.name = "Scrub NV cache",
			.action = ftl_mngt_scrub_nv_cache,
		},
		{
			.name = "Finalize band initialization",
			.action = ftl_mngt_finalize_init_bands,
		},
		{
			.name = "Save initial band info metadata",
			.action = ftl_mngt_persist_band_info_metadata,
		},
		{
			.name = "Save initial chunk info metadata",
			.action = ftl_mngt_persist_nv_cache_metadata,
		},
		{
			.name = "Initialize P2L checkpointing",
			.action = ftl_mngt_p2l_init_ckpt,
			.cleanup = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Wipe P2L region",
			.action = ftl_mngt_p2l_wipe,
		},
		{
			.name = "Clear trim map",
			.action = ftl_mngt_unmap_clear,
		},
		{
			.name = "Free P2L region bufs",
			.action = ftl_mngt_p2l_free_bufs,
		},
		{
			.name = "Set FTL dirty state",
			.action = ftl_mngt_set_dirty,
		},
		{
			.name = "Start task core",
			.action = ftl_mngt_start_task_core,
			.cleanup = ftl_mngt_stop_task_core
		},
		{
			.name = "Finalize initialization",
			.action = ftl_mngt_finalize_init,
		},
		{}
	}
};

static const struct ftl_mngt_process_desc desc_restore = {
	.name = "FTL restore",
	.steps = {
		{
			.name = "Select recovery mode",
			.action = ftl_mngt_select_restore_mode,
		},
		{}
	}
};

static const struct ftl_mngt_process_desc desc_clean_start = {
	.name = "Clean startup",
	.steps = {
		{
			.name = "Restore metadata",
			.action = ftl_mngt_restore_md
		},
		{
			.name = "Initialize P2L checkpointing",
			.action = ftl_mngt_p2l_init_ckpt,
			.cleanup = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Restore P2L checkpoints",
			.action = ftl_mngt_p2l_restore_ckpt
		},
		{
			.name = "Initialize L2P",
			.action = ftl_mngt_init_l2p,
			.cleanup = ftl_mngt_deinit_l2p
		},
		{
			.name = "Restore L2P",
			.action = ftl_mngt_restore_l2p,
		},
		{
			.name = "Finalize band initialization",
			.action = ftl_mngt_finalize_init_bands,
		},
		{
			.name = "Free P2L region bufs",
			.action = ftl_mngt_p2l_free_bufs,
		},
		{
			.name = "Start task core",
			.action = ftl_mngt_start_task_core,
			.cleanup = ftl_mngt_stop_task_core
		},
		{
			.name = "Self test on startup",
			.action = ftl_mngt_self_test,
		},
		{
			.name = "Set FTL dirty state",
			.action = ftl_mngt_set_dirty,
		},
		{
			.name = "Finalize initialization",
			.action = ftl_mngt_finalize_init,
		},
		{}
	}
};

int ftl_mngt_startup(struct spdk_ftl_dev *dev,
		     ftl_mngt_fn cb, void *cb_cntx)
{
	return ftl_mngt_execute(dev, &desc_startup, cb, cb_cntx);
}

struct ftl_unmap_ctx {
	uint64_t lba;
	uint64_t num_blocks;
	spdk_ftl_fn cb_fn;
	void *cb_arg;
};

static void ftl_mngt_process_unmap_cb(void *ctx, int status)
{
	struct ftl_mngt *mngt = ctx;

	if (status) {
		ftl_mngt_fail_step(ctx);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void ftl_mngt_process_unmap(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_io *io = ftl_mngt_get_process_cntx(mngt);
	struct spdk_io_channel *ch = ftl_get_io_channel(dev);
	struct ftl_unmap_ctx *ctx = ftl_mngt_get_caller_context(mngt);
	int rc;

	rc = spdk_ftl_unmap(dev, io, ch, ctx->lba, ctx->num_blocks, ftl_mngt_process_unmap_cb, mngt);
	if (rc == -EAGAIN) {
		ftl_mngt_continue_step(mngt);
	}
}

static const struct ftl_mngt_process_desc desc_unmap = {
	.name = "FTL unmap",
	.arg_size = sizeof(struct ftl_io),
	.steps = {
		{
			.name = "Process unmap",
			.action = ftl_mngt_process_unmap,
		},
		{}
	}
};

static void ftl_mngt_unmap_cb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_unmap_ctx *ctx = ftl_mngt_get_caller_context(mngt);

	ctx->cb_fn(ctx->cb_arg, ftl_mngt_get_status(mngt));

	free(ctx);
}

int ftl_mngt_unmap(struct spdk_ftl_dev *dev,
		   uint64_t lba, uint64_t num_blocks,
		   spdk_ftl_fn cb, void *cb_cntx)
{
	struct ftl_unmap_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -EAGAIN;
	}

	ctx->lba = lba;
	ctx->num_blocks = num_blocks;
	ctx->cb_fn = cb;
	ctx->cb_arg = cb_cntx;

	return ftl_mngt_execute(dev, &desc_unmap, ftl_mngt_unmap_cb, ctx);
}

void ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_mngt_call_rollback(mngt, &desc_startup);
}
