/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_BDEV_MDNS_H
#define SPDK_BDEV_MDNS_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/bdev_module.h"

int
bdev_nvme_start_mdns_discovery(const char *base_name,
                               const char *svcname,
                               struct spdk_nvme_ctrlr_opts *drv_opts,
                               struct nvme_ctrlr_opts *bdev_opts);
int
bdev_nvme_stop_mdns_discovery(const char *name);
void
bdev_nvme_get_mdns_discovery_info(struct spdk_json_write_ctx *w);
#endif /* SPDK_BDEV_MDNS_H */
