/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Filesystem device internal APIs
 */

#ifndef SPDK_FSDEV_INT_H
#define SPDK_FSDEV_INT_H

#include "spdk/thread.h"

void fsdev_io_submit(struct spdk_fsdev_io *fsdev_io);
struct spdk_fsdev_io *fsdev_channel_get_io(struct spdk_fsdev_channel *channel);

#define __io_ch_to_fsdev_ch(io_ch)	((struct spdk_fsdev_channel *)spdk_io_channel_get_ctx(io_ch))

#endif /* SPDK_FSDEV_INT_H */
