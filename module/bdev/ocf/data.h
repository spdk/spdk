/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_DATA_H
#define VBDEV_OCF_DATA_H

#include "ocf_env.h"
#include "spdk/bdev_module.h"

struct bdev_ocf_data {
	struct iovec *iovs;
	int iovcnt;
	int iovalloc;
	uint32_t size;
	uint32_t seek;
};

struct bdev_ocf_data *vbdev_ocf_data_from_spdk_io(struct spdk_bdev_io *bdev_io);

struct bdev_ocf_data *vbdev_ocf_data_alloc(uint32_t nvecs);

void vbdev_ocf_data_free(struct bdev_ocf_data *data);

struct bdev_ocf_data *vbdev_ocf_data_from_iov(struct iovec *iovs);

void vbdev_ocf_iovs_add(struct bdev_ocf_data *data, void *base, size_t len);

#endif
