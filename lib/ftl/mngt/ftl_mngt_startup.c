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

static void ftl_mngt_select_startup_mode(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_mngt_call(mngt, &desc_first_start);
	} else {
		ftl_mngt_fail_step(mngt);
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
			.name = "Initialize IO channel",
			.action = ftl_mngt_init_io_channel,
			.cleanup = ftl_mngt_deinit_io_channel
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

int ftl_mngt_startup(struct spdk_ftl_dev *dev,
		     ftl_mngt_fn cb, void *cb_cntx)
{
	return ftl_mngt_execute(dev, &desc_startup, cb, cb_cntx);
}

void ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_mngt_call_rollback(mngt, &desc_startup);
}
