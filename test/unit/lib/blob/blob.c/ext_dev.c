/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 SUSE LLC.
 *   All rights reserved.
 */

#include "thread/thread_internal.h"
#include "spdk/blob.h"


#define EXT_DEV_BUFFER_SIZE (4 * 1024 * 1024)
uint8_t g_ext_dev_buffer[EXT_DEV_BUFFER_SIZE];
struct spdk_io_channel g_ext_io_channel;

static struct spdk_io_channel *
ext_dev_create_channel(struct spdk_bs_dev *dev)
{
	return &g_ext_io_channel;
}

static void
ext_dev_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
}

static void
ext_dev_destroy(struct spdk_bs_dev *dev)
{
	free(dev);
}

static void
ext_dev_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	     uint64_t lba, uint32_t lba_count,
	     struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	offset = lba * dev->blocklen;
	length = lba_count * dev->blocklen;
	SPDK_CU_ASSERT_FATAL(offset + length <= EXT_DEV_BUFFER_SIZE);

	if (length > 0) {
		memcpy(payload, &g_ext_dev_buffer[offset], length);
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
ext_dev_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	      uint64_t lba, uint32_t lba_count,
	      struct spdk_bs_dev_cb_args *cb_args)
{
	uint64_t offset, length;

	offset = lba * dev->blocklen;
	length = lba_count * dev->blocklen;
	SPDK_CU_ASSERT_FATAL(offset + length <= EXT_DEV_BUFFER_SIZE);

	memcpy(&g_ext_dev_buffer[offset], payload, length);

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static struct spdk_bs_dev *
init_ext_dev(uint64_t blockcnt, uint32_t blocklen)
{
	struct spdk_bs_dev *dev = calloc(1, sizeof(*dev));

	SPDK_CU_ASSERT_FATAL(dev != NULL);

	dev->create_channel = ext_dev_create_channel;
	dev->destroy_channel = ext_dev_destroy_channel;
	dev->destroy = ext_dev_destroy;
	dev->read = ext_dev_read;
	dev->write = ext_dev_write;
	dev->blockcnt = blockcnt;
	dev->blocklen = blocklen;

	return dev;
}
