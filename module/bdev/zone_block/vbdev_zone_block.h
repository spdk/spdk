/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_ZONE_BLOCK_H
#define SPDK_VBDEV_ZONE_BLOCK_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

int vbdev_zone_block_create(const char *bdev_name, const char *vbdev_name,
			    uint64_t zone_capacity, uint64_t optimal_open_zones);

void vbdev_zone_block_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#endif /* SPDK_VBDEV_ZONE_BLOCK_H */
