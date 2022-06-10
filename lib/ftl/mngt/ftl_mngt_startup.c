/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"

static const struct ftl_mngt_process_desc desc_startup;
static const struct ftl_mngt_process_desc desc_first_start;

static void
ftl_mngt_select_startup_mode(struct spdk_ftl_dev *dev,
			     struct ftl_mngt_process *mngt)
{
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_mngt_call_process(mngt, &desc_first_start);
	} else {
		ftl_mngt_fail_step(mngt);
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
			.name = "Initialize bands metadata",
			.action = ftl_mngt_init_bands_md
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

int
ftl_mngt_call_dev_startup(struct spdk_ftl_dev *dev, ftl_mngt_completion cb, void *cb_cntx)
{
	return ftl_mngt_process_execute(dev, &desc_startup, cb, cb_cntx);
}

void
ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process_rollback(mngt, &desc_startup);
}
