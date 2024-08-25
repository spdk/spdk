/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_AIO_H
#define SPDK_BDEV_AIO_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

// enum spdk_file_bit_array_state {
	
// 	/* The file is opend and have vaild file descriptor	*/
// 	SPDK_FILE_STATE_DIRTY,

// 	/* The file is closed and have unvaild file descriptor	*/
// 	SPDK_FILE_STATE_SYNC,

// 	/* The file is deleted and have unvaild file descriptor	*/
// 	SPDK_FILE_STATE_DELETED,
// };

enum spdk_file_state {

    /* The file is not create or opened before*/
    SPDK_FILE_STATE_CLEAN,

    /* The file is opened and has a valid file descriptor */
    SPDK_FILE_STATE_OPENED,

    /* The file is closed and has an invalid file descriptor */
    SPDK_FILE_STATE_CLOSED,

    /* The file is deleted and has an invalid file descriptor */
    SPDK_FILE_STATE_DELETED,
};

enum spdk_iov_state {

    /* The IO request requires one file.
     * Default state. */
    SPDK_IOVS_ONE_FILE,

    /* The IO request requires two files,
     * but with a single IOVCNT. */
    SPDK_IOV_SINGLE_IOVCNT,

    /* The IO request requires two files,
     * with more than one IOVCNT. We just split the IOVCNT. */
    SPDK_IOVS_SPLIT_IOVCNT,

    /* The IO request requires two files,
     * with more than one IOVCNT. We have to split both the IOVCNT
     * and the iov_len in the iov structure. */
    SPDK_IOVS_SPLIT_IOV,
};

typedef void (*delete_aio_bdev_complete)(void *cb_arg, int bdeverrno);

// int create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly,
// 		    bool falloc);
// int create_md_array(struct file_disk *fdisk, uint32_t file_cnt);
int create_aio_bdev(const char *name, const char *filename, uint32_t block_size, bool readonly,
		bool fallocate, uint64_t disk_size_t, uint32_t size_per_file_t, bool filled_t);
int bdev_aio_rescan(const char *name);
void bdev_aio_delete(const char *name, delete_aio_bdev_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_AIO_H */
