/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Operations on aio filesystem device
 */

#ifndef SPDK_FSDEV_AIO_H
#define SPDK_FSDEV_AIO_H

#include "spdk/stdinc.h"
#include "spdk/fsdev_module.h"

struct spdk_fsdev_aio_opts {
	bool xattr_enabled;
	bool writeback_cache_enabled;
	uint32_t max_write;
};

typedef void (*spdk_delete_aio_fsdev_complete)(void *cb_arg, int fsdeverrno);

void spdk_fsdev_aio_get_default_opts(struct spdk_fsdev_aio_opts *opts);

int spdk_fsdev_aio_create(struct spdk_fsdev **fsdev, const char *name, const char *root_path,
			  const struct spdk_fsdev_aio_opts *opts);
void spdk_fsdev_aio_delete(const char *name, spdk_delete_aio_fsdev_complete cb_fn, void *cb_arg);

#endif /* SPDK_FSDEV_AIO_H */
