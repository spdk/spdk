/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Operations on blobfs whose backing device is spdk_bdev
 */

#ifndef SPDK_BLOBFS_BDEV_H
#define SPDK_BLOBFS_BDEV_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * blobfs on bdev operation completion callback.
 *
 * \param cb_arg Callback argument.
 * \param fserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_blobfs_bdev_op_complete)(void *cb_arg, int fserrno);

/**
 * Detect whether blobfs exists on the given device.
 *
 * \param bdev_name Name of block device.
 * \param cb_fn Called when the detecting is complete. fserrno is -EILSEQ if no blobfs exists.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blobfs_bdev_detect(const char *bdev_name,
			     spdk_blobfs_bdev_op_complete cb_fn, void *cb_arg);

/**
 * Create a blobfs on the given device.
 *
 * \param bdev_name Name of block device.
 * \param cluster_sz Size of cluster in bytes. Must be multiple of 4KiB page size.
 * \param cb_fn Called when the creation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blobfs_bdev_create(const char *bdev_name, uint32_t cluster_sz,
			     spdk_blobfs_bdev_op_complete cb_fn, void *cb_arg);

/**
 * Mount a blobfs on given device to a host path by FUSE
 *
 * A new thread is created dedicatedly for one mountpoint to handle FUSE request
 * by blobfs API.
 *
 * \param bdev_name Name of block device.
 * \param mountpoint Host path to mount blobfs.
 * \param cb_fn Called when mount operation is complete. fserrno is -EILSEQ if no blobfs exists.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_blobfs_bdev_mount(const char *bdev_name, const char *mountpoint,
			    spdk_blobfs_bdev_op_complete cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BLOBFS_BDEV_H */
