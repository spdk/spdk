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

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_FILE_NAME_MAX	255

struct spdk_file;
struct spdk_filesystem;

typedef struct spdk_file *spdk_fs_iter;

struct spdk_blobfs_opts {
	uint32_t	cluster_sz;
};

struct spdk_file_stat {
	spdk_blob_id	blobid;
	uint64_t	size;
};

/**
 * Filesystem operation completion callback with handle.
 *
 * \param ctx Context for the operation.
 * \param fs Handle to a blobfs.
 * \param fserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_fs_op_with_handle_complete)(void *ctx, struct spdk_filesystem *fs,
		int fserrno);

/**
 * File operation completion callback with handle.
 *
 * \param ctx Context for the operation.
 * \param f Handle to a file.
 * \param fserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_file_op_with_handle_complete)(void *ctx, struct spdk_file *f, int fserrno);
typedef spdk_bs_op_complete spdk_fs_op_complete;

/**
 * File operation completion callback.
 *
 * \param ctx Context for the operation.
 * \param fserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_file_op_complete)(void *ctx, int fserrno);

/**
 * File stat operation completion callback.
 *
 * \param ctx Context for the operation.
 * \param stat Handle to the stat about the file.
 * \param fserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_file_stat_op_complete)(void *ctx, struct spdk_file_stat *stat, int fserrno);

/**
 * Function for a request of file system.
 *
 * \param arg Argument to the request function.
 */
typedef void (*fs_request_fn)(void *arg);

/**
 * Function for sending request.
 *
 * This function will be invoked any time when the filesystem wants to pass a
 * message to the main dispatch thread.
 *
 * \param fs_request_fn A pointer to the request function.
 * \param arg Argument to the request function.
 */
typedef void (*fs_send_request_fn)(fs_request_fn, void *arg);

/**
 * Initialize a spdk_blobfs_opts structure to the default option values.
 *
 * \param opts spdk_blobf_opts struture to intialize.
 */
void spdk_fs_opts_init(struct spdk_blobfs_opts *opts);

/**
 * Initialize blobstore filesystem.
 *
 * Initialize the blobstore filesystem on the blobstore block device which has
 * been created by the function spdk_bdev_create_bs_dev() in the blob_bdev.h.
 * The obtained blobstore filesystem will be passed to the callback function.
 *
 * \param dev Blobstore block device used by this blobstore filesystem.
 * \param opt Initialization options used for this blobstore filesystem.
 * \param send_request_fn The function for sending request. This function will
 * be invoked any time when the blobstore filesystem wants to pass a message to
 * the main dispatch thread.
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fs_init(struct spdk_bs_dev *dev, struct spdk_blobfs_opts *opt,
		  fs_send_request_fn send_request_fn,
		  spdk_fs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Load blobstore filesystem from the given blobstore block device.
 *
 * The obtained blobstore filesystem will be passed to the callback function.
 *
 * \param dev Blobstore block device used by this blobstore filesystem.
 * \param send_request_fn The function for sending request. This function will
 * be invoked any time when the blobstore filesystem wants to pass a message to
 * the main dispatch thread.
 * \param cb_fn Called when the loading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fs_load(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn,
		  spdk_fs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Unload blobstore filesystem.
 *
 * \param fs Blobstore filesystem to unload.
 * \param cb_fn Called when the unloading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fs_unload(struct spdk_filesystem *fs, spdk_fs_op_complete cb_fn, void *cb_arg);

/**
 * Allocate an I/O channel for asynchronous operations.
 *
 * \param fs Blobstore filesystem to allocate I/O channel.
 *
 * \return a pointer to the I/O channel on success or NULL otherwise.
 */
struct spdk_io_channel *spdk_fs_alloc_io_channel(struct spdk_filesystem *fs);

/**
 * Free I/O channel.
 *
 * This function will decrease the references of this I/O channel. If the reference
 * is reduced to 0, the I/O channel will be freed.
 *
 * \param channel I/O channel to free.
 */
void spdk_fs_free_io_channel(struct spdk_io_channel *channel);

/**
 * Allocate a context for synchronous operations.
 *
 * \param fs Blobstore filesystem for this context.
 *
 * \return a pointer to the context on success or NULL otherwise.
 */
struct spdk_fs_thread_ctx *spdk_fs_alloc_thread_ctx(struct spdk_filesystem *fs);

/**
 * Free thread context.
 *
 * \param ctx Thread context to free.
 */
void spdk_fs_free_thread_ctx(struct spdk_fs_thread_ctx *ctx);

/**
 * Get statistics about the file including the underlying blob id and the file size.
 *
 * \param fs Blobstore filesystem.
 * \param ctx The thread context for this operation
 * \param name The file name used to look up the matched file in the blobstore filesystem.
 * \param stat Caller allocated structure to store the obtained information about
 * this file.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_fs_file_stat(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
		      const char *name, struct spdk_file_stat *stat);

#define SPDK_BLOBFS_OPEN_CREATE	(1ULL << 0)

/**
 * Create a new file on the given blobstore filesystem.
 *
 * \param fs Blobstore filesystem.
 * \param ctx The thread context for this operation
 * \param name The file name for this new file.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_fs_create_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
			const char *name);

/**
 * Open the file.
 *
 * \param fs Blobstore filesystem.
 * \param ctx The thread context for this operation
 * \param name The file name used to look up the matched file in the blobstore filesystem.
 * \param flags This flags will be used to control the open mode.
 * \param file It will point to the open file if sccessful or NULL otherwirse.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_fs_open_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
		      const char *name, uint32_t flags, struct spdk_file **file);

/**
 * Close the file.
 *
 * \param file File to close.
 * \param ctx The thread context for this operation
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_file_close(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx);

/**
 * Change the file name.
 *
 * This operation will overwrite an existing file if there is a file with the
 * same name.
 *
 * \param fs Blobstore filesystem.
 * \param ctx The thread context for this operation
 * \param old_name Old name of the file.
 * \param new_name New name of the file.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_fs_rename_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
			const char *old_name, const char *new_name);

/**
 * Delete the file.
 *
 * \param fs Blobstore filesystem.
 * \param ctx The thread context for this operation
 * \param name The name of the file to be deleted.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_fs_delete_file(struct spdk_filesystem *fs, struct spdk_fs_thread_ctx *ctx,
			const char *name);

/**
 * Get the first file in the blobstore filesystem.
 *
 * \param fs Blobstore filesystem to traverse.
 *
 * \return an iterator which points to the first file in the blobstore filesystem.
 */
spdk_fs_iter spdk_fs_iter_first(struct spdk_filesystem *fs);

/**
 * Get the next file in the blobstore filesystem by using the input iterator.
 *
 * \param iter The iterator which points to the current file struct.
 *
 * \return an iterator which points to the next file in the blobstore filesystem.
 */
spdk_fs_iter spdk_fs_iter_next(spdk_fs_iter iter);

#define spdk_fs_iter_get_file(iter)	((struct spdk_file *)(iter))

/**
 * Truncate the file.
 *
 * \param file File to truncate.
 * \param ctx The thread context for this operation
 * \param length New size in bytes of the file.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_file_truncate(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx,
		       uint64_t length);

/**
 * Get file name.
 *
 * \param file File to query.
 *
 * \return the name of the file.
 */
const char *spdk_file_get_name(struct spdk_file *file);

/**
 * Obtain the size of the file.
 *
 * \param file File to query.
 *
 * \return the size in bytes of the file.
 */
uint64_t spdk_file_get_length(struct spdk_file *file);

/**
 * Write data to the given file.
 *
 * \param file File to write.
 * \param ctx The thread context for this operation
 * \param payload The specified buffer which should contain the data to be transmitted.
 * \param offset The beginning position to write data.
 * \param length The size in bytes of data to write.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_file_write(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx,
		    void *payload, uint64_t offset, uint64_t length);

/**
 * Read data to user buffer from the given file.
 *
 * \param file File to read.
 * \param ctx The thread context for this operation
 * \param payload The specified buffer which will store the obtained data.
 * \param offset The beginning position to read.
 * \param length The size in bytes of data to read.
 *
 * \return the end position of this read operation on success, negated errno on failure.
 */
int64_t spdk_file_read(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx,
		       void *payload, uint64_t offset, uint64_t length);

/**
 * Set cache size for the blobstore filesystem.
 *
 * \param size_in_mb Cache size in megabytes.
 */
void spdk_fs_set_cache_size(uint64_t size_in_mb);

/**
 * Obtain the cache size.
 *
 * \return cache size in megabytes.
 */
uint64_t spdk_fs_get_cache_size(void);

#define SPDK_FILE_PRIORITY_LOW	0 /* default */
#define SPDK_FILE_PRIORITY_HIGH	1

/**
 * Set priority for the file.
 *
 * \param file File to set priority.
 * \param priority Priority level (SPDK_FILE_PRIORITY_LOW or SPDK_FILE_PRIORITY_HIGH).
 */
void spdk_file_set_priority(struct spdk_file *file, uint32_t priority);

/**
 * Synchronize the data from the cache to the disk.
 *
 * \param file File to sync.
 * \param ctx The thread context for this operation
 *
 * \return 0 on success.
 */
int spdk_file_sync(struct spdk_file *file, struct spdk_fs_thread_ctx *ctx);

/**
 * Get the unique ID for the file.
 *
 * \param file File to get the ID.
 * \param id ID buffer.
 * \param size Size of the ID buffer.
 *
 * \return the length of ID on success.
 */
int spdk_file_get_id(struct spdk_file *file, void *id, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FS_H_ */
