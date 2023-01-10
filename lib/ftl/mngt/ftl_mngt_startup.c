/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"

static const struct ftl_mngt_process_desc desc_startup;
static const struct ftl_mngt_process_desc desc_first_start;
static const struct ftl_mngt_process_desc desc_restore;
static const struct ftl_mngt_process_desc desc_clean_start;

static void
ftl_mngt_select_startup_mode(struct spdk_ftl_dev *dev,
			     struct ftl_mngt_process *mngt)
{
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_mngt_call_process(mngt, &desc_first_start);
	} else {
		ftl_mngt_call_process(mngt, &desc_restore);
	}
}

static void
ftl_mngt_select_restore_mode(struct spdk_ftl_dev *dev,
			     struct ftl_mngt_process *mngt)
{
	if (dev->sb->clean) {
		ftl_mngt_call_process(mngt, &desc_clean_start);
	} else {
		ftl_mngt_recover(dev, mngt);
	}
}

/*
 * Common startup steps required by FTL in all cases (creation, load, dirty shutdown recovery).
 * Includes actions like opening the devices, calculating the expected size and version of metadata, etc.
 */
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
			.name = "Register IO device",
			.action = ftl_mngt_register_io_device,
			.cleanup = ftl_mngt_unregister_io_device
		},
		{
			.name = "Initialize core IO channel",
			.action = ftl_mngt_init_io_channel,
			.cleanup = ftl_mngt_deinit_io_channel
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
			.name = "Verify layout",
			.action = ftl_mngt_layout_verify,
		},
		{
			.name = "Initialize metadata",
			.action = ftl_mngt_init_md,
			.cleanup = ftl_mngt_deinit_md
		},
		{
			.name = "Initialize band addresses",
			.action = ftl_mngt_initialize_band_address
		},
		{
			.name = "Initialize NV cache",
			.action = ftl_mngt_init_nv_cache,
			.cleanup = ftl_mngt_deinit_nv_cache
		},
		{
			.name = "Upgrade layout",
			.action = ftl_mngt_layout_upgrade,
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

/*
 * Steps executed when creating FTL for the first time - most important being scrubbing
 * old data/metadata (so it's not leaked during dirty shutdown recovery) and laying out
 * regions for the new metadata (initializing band states, etc).
 */
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
			.name = "Start core poller",
			.action = ftl_mngt_start_core_poller,
			.cleanup = ftl_mngt_stop_core_poller
		},
		{
			.name = "Finalize initialization",
			.action = ftl_mngt_finalize_startup,
		},
		{}
	}
};

/*
 * Step utilized on loading of an FTL instance - decides on dirty/clean shutdown path.
 */
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

/*
 * Loading of FTL after clean shutdown.
 */
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
			.name = "Start core poller",
			.action = ftl_mngt_start_core_poller,
			.cleanup = ftl_mngt_stop_core_poller
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
			.action = ftl_mngt_finalize_startup,
		},
		{}
	}
};

int
ftl_mngt_call_dev_startup(struct spdk_ftl_dev *dev, ftl_mngt_completion cb, void *cb_cntx)
{
	return ftl_mngt_process_execute(dev, &desc_startup, cb, cb_cntx);
}

struct ftl_unmap_ctx {
	uint64_t lba;
	uint64_t num_blocks;
	spdk_ftl_fn cb_fn;
	void *cb_arg;
	struct spdk_thread *thread;
	int status;
};

static void
ftl_mngt_process_unmap_cb(void *ctx, int status)
{
	struct ftl_mngt_process *mngt = ctx;

	if (status) {
		ftl_mngt_fail_step(ctx);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void
ftl_mngt_process_unmap(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_io *io = ftl_mngt_get_process_ctx(mngt);
	struct ftl_unmap_ctx *ctx = ftl_mngt_get_caller_ctx(mngt);
	int rc;

	if (!dev->ioch) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	rc = spdk_ftl_unmap(dev, io, dev->ioch, ctx->lba, ctx->num_blocks, ftl_mngt_process_unmap_cb, mngt);
	if (rc == -EAGAIN) {
		ftl_mngt_continue_step(mngt);
	}
}

/*
 * RPC unmap path.
 */
static const struct ftl_mngt_process_desc g_desc_unmap = {
	.name = "FTL unmap",
	.ctx_size = sizeof(struct ftl_io),
	.steps = {
		{
			.name = "Process unmap",
			.action = ftl_mngt_process_unmap,
		},
		{}
	}
};

static void
unmap_user_cb(void *_ctx)
{
	struct ftl_unmap_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, ctx->status);
	free(ctx);
}

static void
ftl_mngt_unmap_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_unmap_ctx *ctx = _ctx;
	ctx->status = status;

	if (spdk_thread_send_msg(ctx->thread, unmap_user_cb, ctx)) {
		ftl_abort();
	}
}

int
ftl_mngt_unmap(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks, spdk_ftl_fn cb,
	       void *cb_cntx)
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
	ctx->thread = spdk_get_thread();

	return ftl_mngt_process_execute(dev, &g_desc_unmap, ftl_mngt_unmap_cb, ctx);
}

void
ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process_rollback(mngt, &desc_startup);
}
