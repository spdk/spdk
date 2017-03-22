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

#ifndef SPDK_BLOBFS_INTERNAL_H
#define SPDK_BLOBFS_INTERNAL_H

struct cache_tree;

struct cache_buffer {
	uint8_t			*buf;
	struct cache_buffer	*next;
	uint64_t		offset;
	uint32_t		buf_size;
	uint32_t		bytes_filled;
	uint32_t		bytes_flushed;
	bool			in_progress;
};

#define CACHE_BUFFER_SIZE (256 * 1024)
#define CACHE_BUFFER_SHIFT (18)
#define NEXT_CACHE_BUFFER_OFFSET(offset)	\
	(((offset + CACHE_BUFFER_SIZE) >> CACHE_BUFFER_SHIFT) << CACHE_BUFFER_SHIFT)

#define CACHE_TREE_WIDTH 64
#define CACHE_TREE_SHIFT 6
#define CACHE_TREE_LEVEL_SHIFT(level)	(CACHE_BUFFER_SHIFT + (level) * CACHE_TREE_SHIFT)
#define CACHE_TREE_LEVEL_SIZE(level)	(1ULL << CACHE_TREE_LEVEL_SHIFT(level))
#define CACHE_TREE_LEVEL_MASK(level)	(CACHE_TREE_LEVEL_SIZE(level) - 1)
#define CACHE_TREE_INDEX(level, offset)	((offset >> CACHE_TREE_LEVEL_SHIFT(level)) & (CACHE_TREE_WIDTH - 1))

struct cache_tree {
	uint8_t			level;
	uint64_t		present_mask;
	union {
		struct cache_buffer	*buffer[CACHE_TREE_WIDTH];
		struct cache_tree	*tree[CACHE_TREE_WIDTH];
	} u;
};

void spdk_cache_buffer_free(struct cache_buffer *cache_buffer);

struct cache_tree *spdk_tree_insert_buffer(struct cache_tree *root, struct cache_buffer *buffer);
void spdk_tree_free_buffers(struct cache_tree *tree);
struct cache_buffer *spdk_tree_find_buffer(struct cache_tree *tree, uint64_t offset);
struct cache_buffer *spdk_tree_find_filled_buffer(struct cache_tree *tree, uint64_t offset);
void spdk_tree_remove_buffer(struct cache_tree *tree, struct cache_buffer *buffer);

void spdk_fs_file_stat_async(struct spdk_filesystem *fs, const char *name,
			     spdk_file_stat_op_complete cb_fn, void *cb_arg);
void spdk_fs_create_file_async(struct spdk_filesystem *fs, const char *name,
			       spdk_file_op_complete cb_fn, void *cb_args);
void spdk_fs_open_file_async(struct spdk_filesystem *fs, const char *name, uint32_t flags,
			     spdk_file_op_with_handle_complete cb_fn, void *cb_arg);
void spdk_file_close_async(struct spdk_file *file, spdk_file_op_complete cb_fn, void *cb_arg);
void spdk_fs_rename_file_async(struct spdk_filesystem *fs, const char *old_name,
			       const char *new_name, spdk_fs_op_complete cb_fn,
			       void *cb_arg);
void spdk_fs_delete_file_async(struct spdk_filesystem *fs, const char *name,
			       spdk_file_op_complete cb_fn, void *cb_arg);
void spdk_file_truncate_async(struct spdk_file *file, uint64_t length,
			      spdk_file_op_complete cb_fn, void *arg);
void spdk_file_write_async(struct spdk_file *file, struct spdk_io_channel *channel,
			   void *payload, uint64_t offset, uint64_t length,
			   spdk_file_op_complete cb_fn, void *cb_arg);
void spdk_file_read_async(struct spdk_file *file, struct spdk_io_channel *channel,
			  void *payload, uint64_t offset, uint64_t length,
			  spdk_file_op_complete cb_fn, void *cb_arg);

/* Sync all dirty cache buffers to the backing block device.  For async
 *  usage models, completion of the sync indicates only that data written
 *  when the sync command was issued have been flushed to disk - it does
 *  not guarantee any writes submitted after the sync have been flushed,
 *  even if those writes are completed before the sync.
 */
void spdk_file_sync_async(struct spdk_file *file, struct spdk_io_channel *channel,
			  spdk_file_op_complete cb_fn, void *cb_arg);

#endif /* SPDK_BLOBFS_INTERNAL_H_ */
