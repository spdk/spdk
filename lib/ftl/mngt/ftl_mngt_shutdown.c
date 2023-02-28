/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"

/*
 * Steps executed during clean shutdown - includes persisting metadata and rolling
 * back any setup steps executed during startup (closing bdevs, io channels, etc)
 */
static const struct ftl_mngt_process_desc desc_shutdown = {
	.name = "FTL shutdown",
	.error_handler = ftl_mngt_rollback_device,
	.steps = {
		{
			.name = "Deinit core IO channel",
			.action = ftl_mngt_deinit_io_channel
		},
		{
			.name = "Unregister IO device",
			.action = ftl_mngt_unregister_io_device
		},
		{
			.name = "Stop core poller",
			.action = ftl_mngt_stop_core_poller
		},
		{
			.name = "Persist L2P",
			.action = ftl_mngt_persist_l2p
		},
		{
			.name = "Finish L2P unmaps",
			.action = ftl_mngt_unmap_l2p,
		},
		{
			.name = "Persist metadata",
			.action = ftl_mngt_persist_md
		},
		{
			.name = "Set FTL clean state",
			.action = ftl_mngt_set_clean
		},
		{
			.name = "Dump statistics",
			.action = ftl_mngt_dump_stats
		},
		{
			.name = "Deinitialize L2P",
			.action = ftl_mngt_deinit_l2p
		},
		{
			.name = "Deinitialize P2L checkpointing",
			.action = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Rollback FTL device",
			.action = ftl_mngt_rollback_device
		},
		{}
	}
};

/*
 * Steps executed during fast clean shutdown (shutting down to shared memory). Utilizes
 * minimum amount of metadata persistence and rolls back any setup steps executed during
 * startup (closing bdevs, io channels, etc)
 */
static const struct ftl_mngt_process_desc desc_fast_shutdown = {
	.name = "FTL fast shutdown",
	.steps = {
		{
			.name = "Deinit core IO channel",
			.action = ftl_mngt_deinit_io_channel
		},
		{
			.name = "Unregister IO device",
			.action = ftl_mngt_unregister_io_device
		},
		{
			.name = "Stop core poller",
			.action = ftl_mngt_stop_core_poller
		},
		{
			.name = "Fast persist metadata",
			.action = ftl_mngt_fast_persist_md
		},
		{
			.name = "Set FTL SHM clean state",
			.action = ftl_mngt_set_shm_clean
		},
		{
			.name = "Dump statistics",
			.action = ftl_mngt_dump_stats
		},
		{
			.name = "Deinitialize L2P",
			.action = ftl_mngt_deinit_l2p
		},
		{
			.name = "Deinitialize P2L checkpointing",
			.action = ftl_mngt_p2l_deinit_ckpt
		},
		{
			.name = "Rollback FTL device",
			.action = ftl_mngt_rollback_device
		},
		{}
	}
};

int
ftl_mngt_call_dev_shutdown(struct spdk_ftl_dev *dev, ftl_mngt_completion cb, void *cb_cntx)
{
	const struct ftl_mngt_process_desc *pdesc;

	if (dev->conf.fast_shutdown) {
		pdesc = &desc_fast_shutdown;
	} else {
		pdesc = &desc_shutdown;
	}
	return ftl_mngt_process_execute(dev, pdesc, cb, cb_cntx);
}
