/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/thread.h"

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_l2p.h"

static void
l2p_cb(struct spdk_ftl_dev *dev, int status, void *ctx)
{
	struct ftl_mngt_process *mngt = ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_init_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_l2p_init(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_deinit_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_deinit(dev);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_clear_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_clear(dev, l2p_cb, mngt);
}

void
ftl_mngt_persist_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_persist(dev, l2p_cb, mngt);
}

void
ftl_mngt_unmap_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_unmap(dev, l2p_cb, mngt);
}

void
ftl_mngt_restore_l2p(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_l2p_restore(dev, l2p_cb, mngt);
}
