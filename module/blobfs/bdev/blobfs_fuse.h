/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BLOBFS_FUSE_H
#define SPDK_BLOBFS_FUSE_H

#include "spdk/stdinc.h"
#include "spdk/blobfs.h"

struct spdk_blobfs_fuse;

void blobfs_fuse_send_request(fs_request_fn fn, void *arg);

typedef void (*blobfs_fuse_unmount_cb)(void *arg);

int blobfs_fuse_start(const char *bdev_name, const char *mountpoint,
		      struct spdk_filesystem *fs, blobfs_fuse_unmount_cb cb_fn,
		      void *cb_arg, struct spdk_blobfs_fuse **bfuse);

void blobfs_fuse_stop(struct spdk_blobfs_fuse *bfuse);

#endif /* SPDK_BLOBFS_FUSE_H */
