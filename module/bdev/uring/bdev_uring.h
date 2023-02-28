/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_URING_H
#define SPDK_BDEV_URING_H

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/bdev.h"

#include "spdk/bdev_module.h"

typedef void (*spdk_delete_uring_complete)(void *cb_arg, int bdeverrno);

struct spdk_bdev *create_uring_bdev(const char *name, const char *filename, uint32_t block_size);

void delete_uring_bdev(const char *name, spdk_delete_uring_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_URING_H */
