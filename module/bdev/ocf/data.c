/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include <ocf/ocf.h>
#include "spdk/bdev.h"
#include "data.h"

struct bdev_ocf_data *
vbdev_ocf_data_alloc(uint32_t iovcnt)
{
	struct bdev_ocf_data *data;

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
vbdev_ocf_data_free(struct bdev_ocf_data *data)
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
vbdev_ocf_iovs_add(struct bdev_ocf_data *data, void *base, size_t len)
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

struct bdev_ocf_data *
vbdev_ocf_data_from_spdk_io(struct spdk_bdev_io *bdev_io)
{
	struct bdev_ocf_data *data;

	if (bdev_io == NULL) {
		return NULL;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_READ:
		assert(bdev_io->u.bdev.iovs);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		break;
	default:
		SPDK_ERRLOG("Unsupported IO type %d\n", bdev_io->type);
		return NULL;
	}

	data = (struct bdev_ocf_data *)bdev_io->driver_ctx;
	data->iovs = bdev_io->u.bdev.iovs;
	data->iovcnt = bdev_io->u.bdev.iovcnt;
	data->size = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

	return data;
}
