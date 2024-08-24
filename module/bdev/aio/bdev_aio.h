/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_AIO_H
#define SPDK_BDEV_AIO_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

enum spdk_FILE_state {
	/* The blob in-memory version does not match the on-disk
	 * version.
	 */
	SPDK_FILE_STATE_DIRTY,

	/* The blob in memory version of the blob matches the on disk
	 * version.
	 */
	SPDK_FILE_STATE_OPENED,

	/* The in-memory state being synchronized with the on-disk
	 * blob state. */
	SPDK_FILE_STATE_CLOSED,

	SPDK_FILE_STATE_DELETED,
};

typedef void (*delete_aio_bdev_complete)(void *cb_arg, int bdeverrno);

// int create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly,
// 		    bool falloc);
// int create_md_array(struct file_disk *fdisk, uint32_t file_cnt);
int create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly,
		bool fallocate, uint64_t disk_size_t, uint32_t size_per_file_t);
int bdev_aio_rescan(const char *name);
void bdev_aio_delete(const char *name, delete_aio_bdev_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_AIO_H */
