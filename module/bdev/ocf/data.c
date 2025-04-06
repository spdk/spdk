/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#include <ocf/ocf.h>
#include "spdk/bdev.h"
#include "data.h"

struct vbdev_ocf_data *
vbdev_ocf_data_alloc(uint32_t iovcnt)
{
	struct vbdev_ocf_data *data;

	data = env_malloc(sizeof(*data), ENV_MEM_NOIO);
	if (!data) {
		return NULL;
	}

	data->seek = 0;

	if (iovcnt) {
		data->iovs = env_malloc(sizeof(*data->iovs) * iovcnt, ENV_MEM_NOIO);
		if (!data->iovs) {
			env_free(data);
			return NULL;
		}
	}

	data->iovcnt = 0;
	data->iovalloc = iovcnt;

	return data;
}

void
vbdev_ocf_data_free(struct vbdev_ocf_data *data)
{
	if (!data) {
		return;
	}

	if (data->iovalloc != 0) {
		env_free(data->iovs);
	}

	env_free(data);
}

void
vbdev_ocf_iovs_add(struct vbdev_ocf_data *data, void *base, size_t len)
{
	assert(NULL != data);
	assert(data->iovalloc != -1);

	if (data->iovcnt == data->iovalloc) {
		/* TODO: Realloc iovs */
		SPDK_ERRLOG("IOV error\n");
	}

	data->iovs[data->iovcnt].iov_base = base;
	data->iovs[data->iovcnt].iov_len = len;
	data->iovcnt++;
}
