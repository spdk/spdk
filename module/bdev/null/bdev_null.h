/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

#ifndef SPDK_BDEV_NULL_H
#define SPDK_BDEV_NULL_H

#include "spdk/stdinc.h"

typedef void (*spdk_delete_null_complete)(void *cb_arg, int bdeverrno);

struct spdk_bdev;
struct spdk_uuid;

struct spdk_null_bdev_opts {
	const char *name;
	const struct spdk_uuid *uuid;
	uint64_t num_blocks;
	uint32_t block_size;
	uint32_t md_size;
	bool md_interleave;
	enum spdk_dif_type dif_type;
	bool dif_is_head_of_md;
};

int bdev_null_create(struct spdk_bdev **bdev, const struct spdk_null_bdev_opts *opts);

/**
 * Delete null bdev.
 *
 * \param bdev_name Name of null bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_null_delete(const char *bdev_name, spdk_delete_null_complete cb_fn,
		      void *cb_arg);
/**
 * Resize null bdev.
 *
 * \param bdev_name Name of null bdev.
 * \param new_size_in_mb The new size in MiB for this bdev
 */
int bdev_null_resize(const char *bdev_name, const uint64_t new_size_in_mb);

#endif /* SPDK_BDEV_NULL_H */
