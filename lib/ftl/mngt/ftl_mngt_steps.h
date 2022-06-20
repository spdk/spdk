/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_MNGT_STEPS_H
#define FTL_MNGT_STEPS_H

#include "ftl_mngt.h"

void ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

#endif /* FTL_MNGT_STEPS_H */
