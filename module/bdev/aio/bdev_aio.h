/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_AIO_H
#define SPDK_BDEV_AIO_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

typedef void (*delete_aio_bdev_complete)(void *cb_arg, int bdeverrno);

int create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly);

int bdev_aio_rescan(const char *name);
void bdev_aio_delete(const char *name, delete_aio_bdev_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_AIO_H */
