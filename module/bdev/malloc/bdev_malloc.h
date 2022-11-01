/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_BDEV_MALLOC_H
#define SPDK_BDEV_MALLOC_H

#include "spdk/stdinc.h"

#include "spdk/bdev_module.h"

typedef void (*spdk_delete_malloc_complete)(void *cb_arg, int bdeverrno);

struct malloc_bdev_opts {
	char *name;
	struct spdk_uuid uuid;
	uint64_t num_blocks;
	uint32_t block_size;
	uint32_t optimal_io_boundary;
	uint32_t md_size;
	bool md_interleave;
	enum spdk_dif_type dif_type;
	bool dif_is_head_of_md;
};

int create_malloc_disk(struct spdk_bdev **bdev, const struct malloc_bdev_opts *opts);

void delete_malloc_disk(const char *name, spdk_delete_malloc_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_MALLOC_H */
