/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_BDEV_MALLOC_H
#define SPDK_BDEV_MALLOC_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"

typedef void (*spdk_delete_malloc_complete)(void *cb_arg, int bdeverrno);

int create_malloc_disk(struct spdk_bdev **bdev, const char *name, const struct spdk_uuid *uuid,
		       uint64_t num_blocks, uint32_t block_size, uint32_t optimal_io_boundary);

void delete_malloc_disk(const char *name, spdk_delete_malloc_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_MALLOC_H */
