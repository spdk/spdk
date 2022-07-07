/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
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
			.name = "Dump statistics",
			.action = ftl_mngt_dump_stats
		},
		{
			.name = "Deinitialize L2P",
			.action = ftl_mngt_deinit_l2p
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
	return ftl_mngt_process_execute(dev, &desc_shutdown, cb, cb_cntx);
}
