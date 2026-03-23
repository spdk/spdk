/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_BDEV_KVMALLOC_H
#define SPDK_BDEV_KVMALLOC_H

#include "spdk/stdinc.h"

#include "spdk/bdev_module.h"

typedef void (*delete_kvmalloc_complete)(void *cb_arg, int bdeverrno);

struct kvmalloc_bdev_opts {
	char *name;
	struct spdk_uuid uuid;
	uint32_t max_key_size;
	uint32_t max_value_size;
	uint32_t optimal_value_granularity;
	int32_t numa_id;
};

int create_kvmalloc_disk(struct spdk_bdev **bdev, const struct kvmalloc_bdev_opts *opts);

void delete_kvmalloc_disk(const char *name, delete_kvmalloc_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_KVMALLOC_H */
