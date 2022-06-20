/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/thread.h"
#include "spdk/crc32.h"

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_utils.h"
#include "ftl_internal.h"

void
ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_layout_setup(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}
