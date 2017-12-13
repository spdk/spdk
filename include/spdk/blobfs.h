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

/**
 * Initialize filesystem.
 *
 * This function will initialize a filesystem and allocate I/O channels for it.
 *
 * \param dev Blobstore device to be used by this filesystem.
 * \param send_request_fn The function for sending request.
 * \param cb_fn Called when the initialization is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fs_init(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn,
		  spdk_fs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Load filesystem.
 *
 * This function will load the filesystem from the given device and allocate I/O
 * channels for it.
 *
 * \param dev Blobstore device.
 * \param send_request_fn The function for sending request.
 * \param cb_fn Called when the loading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fs_load(struct spdk_bs_dev *dev, fs_send_request_fn send_request_fn,
		  spdk_fs_op_with_handle_complete cb_fn, void *cb_arg);

/**
 * Unload filesystem.
 *
 * This function will unload the filesystem and free the allocated I/O channels.
 *
 * \param fs Filesystem to unload.
 * \param cb_fn Called when the unloading is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_fs_unload(struct spdk_filesystem *fs, spdk_fs_op_complete cb_fn, void *cb_arg);

/**
 * Allocate asynchronous I/O channel.
 *
 * This function will allocate an asynchronous I/O channel for the given filesystem.
 *
 * \param fs Filesystem to allocate I/O channel.
 * \return The allocated I/O channel on success or NULL ohtherwise.
 */
struct spdk_io_channel *spdk_fs_alloc_io_channel(struct spdk_filesystem *fs);

/**
 * Allocate synchronous I/O channel.
 *
 * This function will allocate an I/O channel suitable for using the synchronous
 * blobfs API. These channels do not allocate an I/O channel for the underlying
 * blobstore, but rather allocate synchronizaiton primitives used to block until
 * any necessary I/O operations are completed on a separate polling thread.
 *
 * \param fs Filesystem to allocate I/O channel.
 * \return The allocated I/O channel on success or NULL otherwise.
 */
struct spdk_io_channel *spdk_fs_alloc_io_channel_sync(struct spdk_filesystem *fs);

/**
 * Free I/O channel.
 *
 * This function will decrease the references of this I/O channel. If the reference
 * is reduced to 0, the I/O channel will be freed and the context will be removed.
 *
 * \param channel I/O channel to free.
 */
void spdk_fs_free_io_channel(struct spdk_io_channel *channel);

/**
 * Get the information about the file, mainly blob id and file size.
 *
 * \param fs Filesystem. The requested file should belong to this filesystem.
 * \param channel The I/O channel used to allocate file request.
 * \param name The file name used to look up the matched file in the filesystem.
 * \param stat Caller allocated structure to store the obtained information about this file.
 * \return 0 on success, negated errno on failure.
 */
int spdk_fs_file_stat(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
		      const char *name, struct spdk_file_stat *stat);

#define SPDK_BLOBFS_OPEN_CREATE	(1ULL << 0)

/**
 * Create a new file.
 *
 * This function will create a new file in the given filesystem.
 *
 * \param fs Filesystem.
 * \param channel The I/O channel used to allocate file request.
 * \param name The file name for this new file.
 * \return 0 on success, negated errno on failure.
 */
int spdk_fs_create_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
			const char *name);

/**
 * Open the file.
 *
 * \param fs Filesystem. The requested file should belong to this filesystem.
 * \param channel The I/O channel used to allocate file request.
 * \param name The file name used to look up the matched file in the filesystem.
 * \param flags This flags will be used to control the open mode.
 * \param file It will point to the open file if sccessful or NULL otherwirse.
 * \return 0 on success, negated errno on failure.
 */
int spdk_fs_open_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
		      const char *name, uint32_t flags, struct spdk_file **file);

/**
 * Close the file.
 *
 * \param file File to close.
 * \param channel The I/O channel used to allocate file request.
 * \return 0 on success, negated errno on failure.
 */
int spdk_file_close(struct spdk_file *file, struct spdk_io_channel *channel);

/**
 * Change the file name.
 *
 * This operation will overwrite an existing file if there is a file with the
 * same name.
 *
 * \param fs Filesystem. The requested file should belong to this filesystem.
 * \param channel The I/O channel used to allocate file request.
 * \param old_name Old name of the file.
 * \param new_name New name of the file.
 * \return 0 on success, negated errno on failure.
 */
int spdk_fs_rename_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
			const char *old_name, const char *new_name);

/**
 * Delete the file.
 *
 * \param fs Filesystem. The requested file should belong to this filesystem.
 * \param channel The I/O channel used to allocate file request.
 * \param name The name of the file to be deleted.
 * \return 0 on success, negated errno on failure.
 */
int spdk_fs_delete_file(struct spdk_filesystem *fs, struct spdk_io_channel *channel,
			const char *name);

/**
 * Get the first file.
 *
 * This function is used to traverse the filesystem.
 * It will get the first file in the linked list of this filesystem.
 *
 * \param fs Filesysetm to traverse.
 * \return The iterator which points to the file struct.
 */
spdk_fs_iter spdk_fs_iter_first(struct spdk_filesystem *fs);

/**
 * Get the next file.
 *
 * This function is used to traverse the filesystem.
 * It will get the next file in the linked list by using input iterator.
 *
 * \param iter The iterator which points to the current file struct.
 * \return The iterator which points to the next file struct.
 */
spdk_fs_iter spdk_fs_iter_next(spdk_fs_iter iter);

#define spdk_fs_iter_get_file(iter)	((struct spdk_file *)(iter))

/**
 * Truncate the file.
 *
 * This function will change the size of the file to the size specified by the
 * parameter length. If the original file size is larger than the parameter length,
 * the excess part will be deleted.
 *
 * \param file File to truncate.
 * \param channel The I/O channel used to allocate file request.
 * \param length New size of the file.
 */
void spdk_file_truncate(struct spdk_file *file, struct spdk_io_channel *channel,
			uint64_t length);

/**
 * Obtain the file name through the file struct.
 *
 * \param file File to query.
 * \return The name of the file.
 */
const char *spdk_file_get_name(struct spdk_file *file);

/**
 * Obtain the size of the file.
 *
 * \param file File to query.
 * \return The size of the file.
 */
uint64_t spdk_file_get_length(struct spdk_file *file);

/**
 * Write data to the given file.
 *
 * \param file File to write.
 * \param channel The I/O channel used to allocate file request.
 * \param payload The specified buffer which should contain the data to be transmitted.
 * \param offset The beginning position to write data.
 * \param length The size of data to write.
 * \return 0 on success, negated errno on failure.
 */
int spdk_file_write(struct spdk_file *file, struct spdk_io_channel *channel,
		    void *payload, uint64_t offset, uint64_t length);

/**
 * Read data to user buffer from the given file.
 *
 * \param file File to read.
 * \param channel The I/O channel used to allocate file request.
 * \param payload The specified buffer which will store the obtained data.
 * \param offset The beginning opsition to read.
 * \param length The size of data to read.
 * \return The end position of this read operation on success, negated errno on failure.
 */
int64_t spdk_file_read(struct spdk_file *file, struct spdk_io_channel *channel,
		       void *payload, uint64_t offset, uint64_t length);

/**
 * Set cache size for the blobfs.
 *
 * \param size_in_mb Cache size in megabytes.
 */
void spdk_fs_set_cache_size(uint64_t size_in_mb);

/**
 * Obtain the cache size.
 *
 * \return Cache size in megabytes.
 */
uint64_t spdk_fs_get_cache_size(void);

#define SPDK_FILE_PRIORITY_LOW	0 /* default */
#define SPDK_FILE_PRIORITY_HIGH	1

/**
 * Set priority for the file.
 *
 * \param file File to set priority.
 * \param priority Priority level.
 */
void spdk_file_set_priority(struct spdk_file *file, uint32_t priority);

/**
 * Synchronize the data form the cache to the disk.
 *
 * \param file File to sync.
 * \param channel The I/O channel used to allocate file request.
 * \return 0 on success.
 */
int spdk_file_sync(struct spdk_file *file, struct spdk_io_channel *channel);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FS_H_ */
