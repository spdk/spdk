/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"

static const struct ftl_mngt_process_desc desc_shutdown = {
	.name = "FTL shutdown",
	.error_handler = ftl_mngt_rollback_device,
	.steps = {
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
