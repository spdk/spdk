/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_INTERNAL_H
#define SPDK_BDEV_INTERNAL_H

#include "spdk/bdev.h"

#define ZERO_BUFFER_SIZE	0x100000

struct spdk_bdev;
struct spdk_bdev_io;
struct spdk_bdev_channel;

struct spdk_bdev_io *bdev_channel_get_io(struct spdk_bdev_channel *channel);

void bdev_io_init(struct spdk_bdev_io *bdev_io, struct spdk_bdev *bdev, void *cb_arg,
		  spdk_bdev_io_completion_cb cb);

void bdev_io_submit(struct spdk_bdev_io *bdev_io);

#endif /* SPDK_BDEV_INTERNAL_H */
