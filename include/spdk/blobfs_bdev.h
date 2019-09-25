/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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

#ifdef SPDK_CONFIG_FUSE
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
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BLOBFS_BDEV_H */
