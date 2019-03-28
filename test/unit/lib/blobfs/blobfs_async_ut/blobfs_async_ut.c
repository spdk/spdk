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

#include "spdk/stdinc.h"

#include "CUnit/Basic.h"

#include "common/lib/ut_multithread.c"

#include "spdk_cunit.h"
#include "blobfs/blobfs.c"
#include "blobfs/tree.c"
#include "blob/blobstore.h"

#include "spdk_internal/thread.h"

#include "unit/lib/blob/bs_dev_common.c"

struct spdk_filesystem *g_fs;
struct spdk_file *g_file;
int g_fserrno;

/* Return NULL to test hardcoded defaults. */
struct spdk_conf_section *
spdk_conf_find_section(struct spdk_conf *cp, const char *name)
{
	return NULL;
}

/* Return -1 to test hardcoded defaults. */
int
spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key)
{
	return -1;
}

static void
fs_op_complete(void *ctx, int fserrno)
{
	g_fserrno = fserrno;
}

static void
fs_op_with_handle_complete(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	g_fs = fs;
	g_fserrno = fserrno;
}

static void
fs_init(void)
{
	struct spdk_filesystem *fs;
	struct spdk_bs_dev *dev;

	dev = init_dev();

	spdk_fs_init(dev, NULL, NULL, fs_op_with_handle_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
	fs = g_fs;
	SPDK_CU_ASSERT_FATAL(fs->bs->dev == dev);

	g_fserrno = 1;
	spdk_fs_unload(fs, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
}

static void
create_cb(void *ctx, int fserrno)
{
	g_fserrno = fserrno;
}

static void
open_cb(void *ctx, struct spdk_file *f, int fserrno)
{
	g_fserrno = fserrno;
	g_file = f;
}

static void
delete_cb(void *ctx, int fserrno)
{
	g_fserrno = fserrno;
}

static void
fs_open(void)
{
	struct spdk_filesystem *fs;
	spdk_fs_iter iter;
	struct spdk_bs_dev *dev;
	struct spdk_file *file;
	char name[257] = {'\0'};

	dev = init_dev();
	memset(name, 'a', sizeof(name) - 1);

	spdk_fs_init(dev, NULL, NULL, fs_op_with_handle_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
	fs = g_fs;
	SPDK_CU_ASSERT_FATAL(fs->bs->dev == dev);

	g_fserrno = 0;
	/* Open should fail, because the file name is too long. */
	spdk_fs_open_file_async(fs, name, SPDK_BLOBFS_OPEN_CREATE, open_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == -ENAMETOOLONG);

	g_fserrno = 0;
	spdk_fs_open_file_async(fs, "file1", 0, open_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == -ENOENT);

	g_file = NULL;
	g_fserrno = 1;
	spdk_fs_open_file_async(fs, "file1", SPDK_BLOBFS_OPEN_CREATE, open_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);
	CU_ASSERT(!strcmp("file1", g_file->name));
	CU_ASSERT(g_file->ref_count == 1);

	iter = spdk_fs_iter_first(fs);
	CU_ASSERT(iter != NULL);
	file = spdk_fs_iter_get_file(iter);
	SPDK_CU_ASSERT_FATAL(file != NULL);
	CU_ASSERT(!strcmp("file1", file->name));
	iter = spdk_fs_iter_next(iter);
	CU_ASSERT(iter == NULL);

	g_fserrno = 0;
	/* Delete should successful, we will mark the file as deleted. */
	spdk_fs_delete_file_async(fs, "file1", delete_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(!TAILQ_EMPTY(&fs->files));

	g_fserrno = 1;
	spdk_file_close_async(g_file, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(TAILQ_EMPTY(&fs->files));

	g_fserrno = 1;
	spdk_fs_unload(fs, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
}

static void
fs_create(void)
{
	struct spdk_filesystem *fs;
	struct spdk_bs_dev *dev;
	char name[257] = {'\0'};

	dev = init_dev();
	memset(name, 'a', sizeof(name) - 1);

	spdk_fs_init(dev, NULL, NULL, fs_op_with_handle_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
	fs = g_fs;
	SPDK_CU_ASSERT_FATAL(fs->bs->dev == dev);

	g_fserrno = 0;
	/* Create should fail, because the file name is too long. */
	spdk_fs_create_file_async(fs, name, create_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == -ENAMETOOLONG);

	g_fserrno = 1;
	spdk_fs_create_file_async(fs, "file1", create_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);

	g_fserrno = 1;
	spdk_fs_create_file_async(fs, "file1", create_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == -EEXIST);

	g_fserrno = 1;
	spdk_fs_delete_file_async(fs, "file1", delete_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(TAILQ_EMPTY(&fs->files));

	g_fserrno = 1;
	spdk_fs_unload(fs, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
}

static void
fs_truncate(void)
{
	struct spdk_filesystem *fs;
	struct spdk_bs_dev *dev;

	dev = init_dev();

	spdk_fs_init(dev, NULL, NULL, fs_op_with_handle_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
	fs = g_fs;
	SPDK_CU_ASSERT_FATAL(fs->bs->dev == dev);

	g_file = NULL;
	g_fserrno = 1;
	spdk_fs_open_file_async(fs, "file1", SPDK_BLOBFS_OPEN_CREATE, open_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	g_fserrno = 1;
	spdk_file_truncate_async(g_file, 18 * 1024 * 1024 + 1, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(g_file->length == 18 * 1024 * 1024 + 1);

	g_fserrno = 1;
	spdk_file_truncate_async(g_file, 1, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(g_file->length == 1);

	g_fserrno = 1;
	spdk_file_truncate_async(g_file, 18 * 1024 * 1024 + 1, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(g_file->length == 18 * 1024 * 1024 + 1);

	g_fserrno = 1;
	spdk_file_close_async(g_file, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(g_file->ref_count == 0);

	g_fserrno = 1;
	spdk_fs_delete_file_async(fs, "file1", delete_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(TAILQ_EMPTY(&fs->files));

	g_fserrno = 1;
	spdk_fs_unload(fs, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
}

static void
fs_rename(void)
{
	struct spdk_filesystem *fs;
	struct spdk_file *file, *file2, *file_iter;
	struct spdk_bs_dev *dev;

	dev = init_dev();

	spdk_fs_init(dev, NULL, NULL, fs_op_with_handle_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
	fs = g_fs;
	SPDK_CU_ASSERT_FATAL(fs->bs->dev == dev);

	g_fserrno = 1;
	spdk_fs_create_file_async(fs, "file1", create_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);

	g_file = NULL;
	g_fserrno = 1;
	spdk_fs_open_file_async(fs, "file1", 0, open_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);
	CU_ASSERT(g_file->ref_count == 1);

	file = g_file;
	g_file = NULL;
	g_fserrno = 1;
	spdk_file_close_async(file, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	SPDK_CU_ASSERT_FATAL(file->ref_count == 0);

	g_file = NULL;
	g_fserrno = 1;
	spdk_fs_open_file_async(fs, "file2", SPDK_BLOBFS_OPEN_CREATE, open_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);
	CU_ASSERT(g_file->ref_count == 1);

	file2 = g_file;
	g_file = NULL;
	g_fserrno = 1;
	spdk_file_close_async(file2, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	SPDK_CU_ASSERT_FATAL(file2->ref_count == 0);

	/*
	 * Do a 3-way rename.  This should delete the old "file2", then rename
	 *  "file1" to "file2".
	 */
	g_fserrno = 1;
	spdk_fs_rename_file_async(fs, "file1", "file2", fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(file->ref_count == 0);
	CU_ASSERT(!strcmp(file->name, "file2"));
	CU_ASSERT(TAILQ_FIRST(&fs->files) == file);
	CU_ASSERT(TAILQ_NEXT(file, tailq) == NULL);

	g_fserrno = 0;
	spdk_fs_delete_file_async(fs, "file1", delete_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == -ENOENT);
	CU_ASSERT(!TAILQ_EMPTY(&fs->files));
	TAILQ_FOREACH(file_iter, &fs->files, tailq) {
		if (file_iter == NULL) {
			SPDK_CU_ASSERT_FATAL(false);
		}
	}

	g_fserrno = 1;
	spdk_fs_delete_file_async(fs, "file2", delete_cb, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	CU_ASSERT(TAILQ_EMPTY(&fs->files));

	g_fserrno = 1;
	spdk_fs_unload(fs, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
}

static void
tree_find_buffer_ut(void)
{
	struct cache_tree *root;
	struct cache_tree *level1_0;
	struct cache_tree *level0_0_0;
	struct cache_tree *level0_0_12;
	struct cache_buffer *leaf_0_0_4;
	struct cache_buffer *leaf_0_12_8;
	struct cache_buffer *leaf_9_23_15;
	struct cache_buffer *buffer;

	level1_0 = calloc(1, sizeof(struct cache_tree));
	SPDK_CU_ASSERT_FATAL(level1_0 != NULL);
	level0_0_0 = calloc(1, sizeof(struct cache_tree));
	SPDK_CU_ASSERT_FATAL(level0_0_0 != NULL);
	level0_0_12 = calloc(1, sizeof(struct cache_tree));
	SPDK_CU_ASSERT_FATAL(level0_0_12 != NULL);
	leaf_0_0_4 = calloc(1, sizeof(struct cache_buffer));
	SPDK_CU_ASSERT_FATAL(leaf_0_0_4 != NULL);
	leaf_0_12_8 = calloc(1, sizeof(struct cache_buffer));
	SPDK_CU_ASSERT_FATAL(leaf_0_12_8 != NULL);
	leaf_9_23_15 = calloc(1, sizeof(struct cache_buffer));
	SPDK_CU_ASSERT_FATAL(leaf_9_23_15 != NULL);

	level1_0->level = 1;
	level0_0_0->level = 0;
	level0_0_12->level = 0;

	leaf_0_0_4->offset = CACHE_BUFFER_SIZE * 4;
	level0_0_0->u.buffer[4] = leaf_0_0_4;
	level0_0_0->present_mask |= (1ULL << 4);

	leaf_0_12_8->offset = CACHE_TREE_LEVEL_SIZE(1) * 12 + CACHE_BUFFER_SIZE * 8;
	level0_0_12->u.buffer[8] = leaf_0_12_8;
	level0_0_12->present_mask |= (1ULL << 8);

	level1_0->u.tree[0] = level0_0_0;
	level1_0->present_mask |= (1ULL << 0);
	level1_0->u.tree[12] = level0_0_12;
	level1_0->present_mask |= (1ULL << 12);

	buffer = spdk_tree_find_buffer(NULL, 0);
	CU_ASSERT(buffer == NULL);

	buffer = spdk_tree_find_buffer(level0_0_0, 0);
	CU_ASSERT(buffer == NULL);

	buffer = spdk_tree_find_buffer(level0_0_0, CACHE_TREE_LEVEL_SIZE(0) + 1);
	CU_ASSERT(buffer == NULL);

	buffer = spdk_tree_find_buffer(level0_0_0, leaf_0_0_4->offset);
	CU_ASSERT(buffer == leaf_0_0_4);

	buffer = spdk_tree_find_buffer(level1_0, leaf_0_0_4->offset);
	CU_ASSERT(buffer == leaf_0_0_4);

	buffer = spdk_tree_find_buffer(level1_0, leaf_0_12_8->offset);
	CU_ASSERT(buffer == leaf_0_12_8);

	buffer = spdk_tree_find_buffer(level1_0, leaf_0_12_8->offset + CACHE_BUFFER_SIZE - 1);
	CU_ASSERT(buffer == leaf_0_12_8);

	buffer = spdk_tree_find_buffer(level1_0, leaf_0_12_8->offset - 1);
	CU_ASSERT(buffer == NULL);

	leaf_9_23_15->offset = CACHE_TREE_LEVEL_SIZE(2) * 9 +
			       CACHE_TREE_LEVEL_SIZE(1) * 23 +
			       CACHE_BUFFER_SIZE * 15;
	root = spdk_tree_insert_buffer(level1_0, leaf_9_23_15);
	CU_ASSERT(root != level1_0);
	buffer = spdk_tree_find_buffer(root, leaf_9_23_15->offset);
	CU_ASSERT(buffer == leaf_9_23_15);
	spdk_tree_free_buffers(root);
	free(root);
}

static void
channel_ops(void)
{
	struct spdk_filesystem *fs;
	struct spdk_bs_dev *dev;
	struct spdk_io_channel *channel;

	dev = init_dev();

	spdk_fs_init(dev, NULL, NULL, fs_op_with_handle_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
	fs = g_fs;
	SPDK_CU_ASSERT_FATAL(fs->bs->dev == dev);

	channel =  spdk_fs_alloc_io_channel(fs);
	CU_ASSERT(channel != NULL);

	spdk_fs_free_io_channel(channel);

	g_fserrno = 1;
	spdk_fs_unload(fs, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	g_fs = NULL;
}

static void
channel_ops_sync(void)
{
	struct spdk_filesystem *fs;
	struct spdk_bs_dev *dev;
	struct spdk_fs_thread_ctx *channel;

	dev = init_dev();

	spdk_fs_init(dev, NULL, NULL, fs_op_with_handle_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
	fs = g_fs;
	SPDK_CU_ASSERT_FATAL(fs->bs->dev == dev);

	channel =  spdk_fs_alloc_thread_ctx(fs);
	CU_ASSERT(channel != NULL);

	spdk_fs_free_thread_ctx(channel);

	g_fserrno = 1;
	spdk_fs_unload(fs, fs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_fserrno == 0);
	g_fs = NULL;
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("blobfs_async_ut", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "fs_init", fs_init) == NULL ||
		CU_add_test(suite, "fs_open", fs_open) == NULL ||
		CU_add_test(suite, "fs_create", fs_create) == NULL ||
		CU_add_test(suite, "fs_truncate", fs_truncate) == NULL ||
		CU_add_test(suite, "fs_rename", fs_rename) == NULL ||
		CU_add_test(suite, "tree_find_buffer", tree_find_buffer_ut) == NULL ||
		CU_add_test(suite, "channel_ops", channel_ops) == NULL ||
		CU_add_test(suite, "channel_ops_sync", channel_ops_sync) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	allocate_threads(1);
	set_thread(0);

	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	free(g_dev_buffer);

	free_threads();

	return num_failures;
}
