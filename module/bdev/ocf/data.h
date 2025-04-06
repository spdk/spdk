/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#ifndef VBDEV_OCF_DATA_H
#define VBDEV_OCF_DATA_H

#include "ocf_env.h"
#include "spdk/bdev_module.h"

struct vbdev_ocf_data {
	struct iovec *iovs;
	int iovcnt;
	int iovalloc;
	uint32_t size;
	uint32_t seek;
};

struct vbdev_ocf_data *vbdev_ocf_data_alloc(uint32_t nvecs);

void vbdev_ocf_data_free(struct vbdev_ocf_data *data);

struct vbdev_ocf_data *vbdev_ocf_data_from_iov(struct iovec *iovs);

void vbdev_ocf_iovs_add(struct vbdev_ocf_data *data, void *base, size_t len);

#endif
