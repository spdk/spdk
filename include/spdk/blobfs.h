/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * SPDK Filesystem
 */

#ifndef SPDK_FS_H
#define SPDK_FS_H

#include "spdk/stdinc.h"

#include "spdk/blob.h"

#define SPDK_FILE_NAME_MAX	255

struct spdk_file;
struct spdk_filesystem;

typedef struct spdk_file *spdk_fs_iter;

struct spdk_file_stat {
	spdk_blob_id	blobid;
	uint64_t	size;
};

typedef void (*spdk_fs_op_with_handle_complete)(void *ctx, struct spdk_filesystem *fs,
		int fserrno);
typedef void (*spdk_file_op_with_handle_complete)(void *ctx, struct spdk_file *f, int fserrno);
typedef spdk_bs_op_complete spdk_fs_op_complete;

typedef void (*spdk_file_op_complete)(void *ctx, int fserrno);
typedef void (*spdk_file_stat_op_complete)(void *ctx, struct spdk_file_stat *stat, int fserrno);

typedef void (*fs_request_fn)(void *);
typedef void (*fs_send_request_fn)(fs_request_fn, void *);

void spdk_fs_init(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn,
		  spdk_fs_op_with_handle_complete cb_fn, void *cb_arg);
void spdk_fs_load(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn,
		  spdk_fs_op_with_handle_complete cb_fn, void *cb_arg);
void spdk_fs_unload(struct spdk_filesystem *fs, spdk_fs_op_complete cb_fn, void *cb_arg);

struct spdk_io_channel *spdk_fs_alloc_io_channel(struct spdk_filesystem *fs);

/*
 * Allocates an I/O channel suitable for using the synchronous blobfs API.  These channels do
 *  not allocate an I/O channel for the underlying blobstore, but rather allocate synchronizaiton
 *  primitives used to block until any necessary I/O operations are completed on a separate
 *  polling thread.
 */
struct spdk_io_channel *spdk_fs_alloc_io_channel_sync(struct spdk_filesystem *fs);

void spdk_fs_free_io_channel(struct spdk_io_channel *channel);

int spdk_fs_file_stat(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
		      const char *name, struct spdk_file_stat *stat);

#define SPDK_BLOBFS_OPEN_CREATE	(1ULL << 0)

int spdk_fs_create_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
			const char *name);

int spdk_fs_open_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
		      const char *name, uint32_t flags, struct spdk_file **file);

int spdk_file_close(struct spdk_file *file, struct spdk_io_channel *channel);

int spdk_fs_rename_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
			const char *old_name, const char *new_name);

int spdk_fs_delete_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
			const char *name);

spdk_fs_iter spdk_fs_iter_first(struct spdk_filesystem *fs);
spdk_fs_iter spdk_fs_iter_next(spdk_fs_iter iter);
#define spdk_fs_iter_get_file(iter)	((struct spdk_file *)(iter))

void spdk_file_truncate(struct spdk_file *file, struct spdk_io_channel *channel,
			uint64_t length);

const char *spdk_file_get_name(struct spdk_file *file);

uint64_t spdk_file_get_length(struct spdk_file *file);

int spdk_file_write(struct spdk_file *file, struct spdk_io_channel *channel,
		    void *payload, uint64_t offset, uint64_t length);

int64_t spdk_file_read(struct spdk_file *file, struct spdk_io_channel *channel,
		       void *payload, uint64_t offset, uint64_t length);

void spdk_fs_set_cache_size(uint64_t size_in_mb);
uint64_t spdk_fs_get_cache_size(void);

#define SPDK_FILE_PRIORITY_LOW	0 /* default */
#define SPDK_FILE_PRIORITY_HIGH	1

void spdk_file_set_priority(struct spdk_file *file, uint32_t priority);

int spdk_file_sync(struct spdk_file *file, struct spdk_io_channel *channel);

#endif /* SPDK_FS_H_ */
