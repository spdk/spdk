/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_VBDEV_COMPRESS_H
#define SPDK_VBDEV_COMPRESS_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"

#define LB_SIZE_4K	0x1000UL
#define LB_SIZE_512B	0x200UL

/**
 * Get the first compression bdev.
 *
 * \return the first compression bdev.
 */
struct vbdev_compress *compress_bdev_first(void);

/**
 * Get the next compression bdev.
 *
 * \param prev previous compression bdev.
 * \return the next compression bdev.
 */
struct vbdev_compress *compress_bdev_next(struct vbdev_compress *prev);

/**
 * Test to see if a compression bdev orphan exists.
 *
 * \param name The name of the compression bdev.
 * \return true if found, false if not.
 */
bool compress_has_orphan(const char *name);

/**
 * Get the name of a compression bdev.
 *
 * \param comp_bdev The compression bdev.
 * \return the name of the compression bdev.
 */
const char *compress_get_name(const struct vbdev_compress *comp_bdev);

typedef void (*spdk_delete_compress_complete)(void *cb_arg, int bdeverrno);

/**
 * Create new compression bdev.
 *
 * \param bdev_name Bdev on which compression bdev will be created.
 * \param pm_path Path to persistent memory.
 * \param lb_size Logical block size for the compressed volume in bytes. Must be 4K or 512.
 * \return 0 on success, other on failure.
 */
int create_compress_bdev(const char *bdev_name, const char *pm_path, uint32_t lb_size);

/**
 * Delete compress bdev.
 *
 * \param bdev_name Bdev on which compression bdev will be deleted.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_compress_delete(const char *bdev_name, spdk_delete_compress_complete cb_fn,
			  void *cb_arg);

#endif /* SPDK_VBDEV_COMPRESS_H */
