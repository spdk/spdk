/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/blobfs.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/barrier.h"
#include "thread/thread_internal.h"

#include "spdk_cunit.h"
#include "unit/lib/blob/bs_dev_common.c"
#include "common/lib/test_env.c"
#include "blobfs/blobfs.c"
#include "blobfs/tree.c"

struct spdk_filesystem *g_fs;
struct spdk_file *g_file;
int g_fserrno;
struct spdk_thread *g_dispatch_thread = NULL;

struct ut_request {
	fs_request_fn fn;
	void *arg;
	volatile int done;
};

DEFINE_STUB(spdk_memory_domain_memzero, int, (struct spdk_memory_domain *src_domain,
		void *src_domain_ctx, struct iovec *iov, uint32_t iovcnt, void (*cpl_cb)(void *, int),
		void *cpl_cb_arg), 0);
DEFINE_STUB(spdk_mempool_lookup, struct spdk_mempool *, (const char *name), NULL);

static void
send_request(fs_request_fn fn, void *arg)
{
	spdk_thread_send_msg(g_dispatch_thread, (spdk_msg_fn)fn, arg);
}

static void
ut_call_fn(void *arg)
{
	struct ut_request *req = arg;

	req->fn(req->arg);
	req->done = 1;
}

static void
ut_send_request(fs_request_fn fn, void *arg)
{
	struct ut_request req;

	req.fn = fn;
	req.arg = arg;
	req.done = 0;

	spdk_thread_send_msg(g_dispatch_thread, ut_call_fn, &req);

	/* Wait for this to finish */
	while (req.done == 0) {	}
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
fs_thread_poll(void)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	while (spdk_thread_poll(thread, 0, 0) > 0) {}
	while (spdk_thread_poll(g_cache_pool_thread, 0, 0) > 0) {}
}

static void
_fs_init(void *arg)
{
	struct spdk_bs_dev *dev;

	g_fs = NULL;
	g_fserrno = -1;
	dev = init_dev();
	spdk_fs_init(dev, NULL, send_request, fs_op_with_handle_complete, NULL);

	fs_thread_poll();

	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	SPDK_CU_ASSERT_FATAL(g_fs->bdev == dev);
	CU_ASSERT(g_fserrno == 0);
}

static void
_fs_load(void *arg)
{
	struct spdk_bs_dev *dev;

	g_fs = NULL;
	g_fserrno = -1;
	dev = init_dev();
	spdk_fs_load(dev, send_request, fs_op_with_handle_complete, NULL);

	fs_thread_poll();

	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	SPDK_CU_ASSERT_FATAL(g_fs->bdev == dev);
	CU_ASSERT(g_fserrno == 0);
}

static void
_fs_unload(void *arg)
{
	g_fserrno = -1;
	spdk_fs_unload(g_fs, fs_op_complete, NULL);

	fs_thread_poll();

	CU_ASSERT(g_fserrno == 0);
	g_fs = NULL;
}

static void
_nop(void *arg)
{
}

static void
cache_read_after_write(void)
{
	uint64_t length;
	int rc;
	char w_buf[100], r_buf[100];
	struct spdk_fs_thread_ctx *channel;
	struct spdk_file_stat stat = {0};

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	length = (4 * 1024 * 1024);
	rc = spdk_file_truncate(g_file, channel, length);
	CU_ASSERT(rc == 0);

	memset(w_buf, 0x5a, sizeof(w_buf));
	spdk_file_write(g_file, channel, w_buf, 0, sizeof(w_buf));

	CU_ASSERT(spdk_file_get_length(g_file) == length);

	rc = spdk_file_truncate(g_file, channel, sizeof(w_buf));
	CU_ASSERT(rc == 0);

	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_file_stat(g_fs, channel, "testfile", &stat);
	CU_ASSERT(rc == 0);
	CU_ASSERT(sizeof(w_buf) == stat.size);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", 0, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	memset(r_buf, 0, sizeof(r_buf));
	spdk_file_read(g_file, channel, r_buf, 0, sizeof(r_buf));
	CU_ASSERT(memcmp(w_buf, r_buf, sizeof(r_buf)) == 0);

	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == -ENOENT);

	spdk_fs_free_thread_ctx(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
file_length(void)
{
	int rc;
	char *buf;
	uint64_t buf_length;
	volatile uint64_t *length_flushed;
	struct spdk_fs_thread_ctx *channel;
	struct spdk_file_stat stat = {0};

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);

	g_file = NULL;
	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	/* Write one CACHE_BUFFER.  Filling at least one cache buffer triggers
	 * a flush to disk.
	 */
	buf_length = CACHE_BUFFER_SIZE;
	buf = calloc(1, buf_length);
	spdk_file_write(g_file, channel, buf, 0, buf_length);
	free(buf);

	/* Spin until all of the data has been flushed to the SSD.  There's been no
	 * sync operation yet, so the xattr on the file is still 0.
	 *
	 * length_flushed: This variable is modified by a different thread in this unit
	 * test. So we need to dereference it as a volatile to ensure the value is always
	 * re-read.
	 */
	length_flushed = &g_file->length_flushed;
	while (*length_flushed != buf_length) {}

	/* Close the file.  This causes an implicit sync which should write the
	 * length_flushed value as the "length" xattr on the file.
	 */
	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_file_stat(g_fs, channel, "testfile", &stat);
	CU_ASSERT(rc == 0);
	CU_ASSERT(buf_length == stat.size);

	spdk_fs_free_thread_ctx(channel);

	/* Unload and reload the filesystem.  The file length will be
	 * read during load from the length xattr.  We want to make sure
	 * it matches what was written when the file was originally
	 * written and closed.
	 */
	ut_send_request(_fs_unload, NULL);

	ut_send_request(_fs_load, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);

	rc = spdk_fs_file_stat(g_fs, channel, "testfile", &stat);
	CU_ASSERT(rc == 0);
	CU_ASSERT(buf_length == stat.size);

	g_file = NULL;
	rc = spdk_fs_open_file(g_fs, channel, "testfile", 0, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_thread_ctx(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
append_write_to_extend_blob(void)
{
	uint64_t blob_size, buf_length;
	char *buf, append_buf[64];
	int rc;
	struct spdk_fs_thread_ctx *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);

	/* create a file and write the file with blob_size - 1 data length */
	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	blob_size = __file_get_blob_size(g_file);

	buf_length = blob_size - 1;
	buf = calloc(1, buf_length);
	rc = spdk_file_write(g_file, channel, buf, 0, buf_length);
	CU_ASSERT(rc == 0);
	free(buf);

	spdk_file_close(g_file, channel);
	fs_thread_poll();
	spdk_fs_free_thread_ctx(channel);
	ut_send_request(_fs_unload, NULL);

	/* load existing file and write extra 2 bytes to cross blob boundary */
	ut_send_request(_fs_load, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);
	g_file = NULL;
	rc = spdk_fs_open_file(g_fs, channel, "testfile", 0, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	CU_ASSERT(g_file->length == buf_length);
	CU_ASSERT(g_file->last == NULL);
	CU_ASSERT(g_file->append_pos == buf_length);

	rc = spdk_file_write(g_file, channel, append_buf, buf_length, 2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(2 * blob_size == __file_get_blob_size(g_file));
	spdk_file_close(g_file, channel);
	fs_thread_poll();
	CU_ASSERT(g_file->length == buf_length + 2);

	spdk_fs_free_thread_ctx(channel);
	ut_send_request(_fs_unload, NULL);
}

static void
partial_buffer(void)
{
	int rc;
	char *buf;
	uint64_t buf_length;
	struct spdk_fs_thread_ctx *channel;
	struct spdk_file_stat stat = {0};

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);

	g_file = NULL;
	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	/* Write one CACHE_BUFFER plus one byte.  Filling at least one cache buffer triggers
	 * a flush to disk.  We want to make sure the extra byte is not implicitly flushed.
	 * It should only get flushed once we sync or close the file.
	 */
	buf_length = CACHE_BUFFER_SIZE + 1;
	buf = calloc(1, buf_length);
	spdk_file_write(g_file, channel, buf, 0, buf_length);
	free(buf);

	/* Send some nop messages to the dispatch thread.  This will ensure any of the
	 * pending write operations are completed.  A well-functioning blobfs should only
	 * issue one write for the filled CACHE_BUFFER - a buggy one might try to write
	 * the extra byte.  So do a bunch of _nops to make sure all of them (even the buggy
	 * ones) get a chance to run.  Note that we can't just send a message to the
	 * dispatch thread to call spdk_thread_poll() because the messages are themselves
	 * run in the context of spdk_thread_poll().
	 */
	ut_send_request(_nop, NULL);
	ut_send_request(_nop, NULL);
	ut_send_request(_nop, NULL);
	ut_send_request(_nop, NULL);
	ut_send_request(_nop, NULL);
	ut_send_request(_nop, NULL);

	CU_ASSERT(g_file->length_flushed == CACHE_BUFFER_SIZE);

	/* Close the file.  This causes an implicit sync which should write the
	 * length_flushed value as the "length" xattr on the file.
	 */
	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_file_stat(g_fs, channel, "testfile", &stat);
	CU_ASSERT(rc == 0);
	CU_ASSERT(buf_length == stat.size);

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_thread_ctx(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
cache_write_null_buffer(void)
{
	uint64_t length;
	int rc;
	struct spdk_fs_thread_ctx *channel;
	struct spdk_thread *thread;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	length = 0;
	rc = spdk_file_truncate(g_file, channel, length);
	CU_ASSERT(rc == 0);

	rc = spdk_file_write(g_file, channel, NULL, 0, 0);
	CU_ASSERT(rc == 0);

	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_thread_ctx(channel);

	thread = spdk_get_thread();
	while (spdk_thread_poll(thread, 0, 0) > 0) {}

	ut_send_request(_fs_unload, NULL);
}

static void
fs_create_sync(void)
{
	int rc;
	struct spdk_fs_thread_ctx *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);
	CU_ASSERT(channel != NULL);

	rc = spdk_fs_create_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	/* Create should fail, because the file already exists. */
	rc = spdk_fs_create_file(g_fs, channel, "testfile");
	CU_ASSERT(rc != 0);

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_thread_ctx(channel);

	fs_thread_poll();

	ut_send_request(_fs_unload, NULL);
}

static void
fs_rename_sync(void)
{
	int rc;
	struct spdk_fs_thread_ctx *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);
	CU_ASSERT(channel != NULL);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	CU_ASSERT(strcmp(spdk_file_get_name(g_file), "testfile") == 0);

	rc = spdk_fs_rename_file(g_fs, channel, "testfile", "newtestfile");
	CU_ASSERT(rc == 0);
	CU_ASSERT(strcmp(spdk_file_get_name(g_file), "newtestfile") == 0);

	spdk_file_close(g_file, channel);

	fs_thread_poll();

	spdk_fs_free_thread_ctx(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
cache_append_no_cache(void)
{
	int rc;
	char buf[100];
	struct spdk_fs_thread_ctx *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_thread_ctx(g_fs);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	spdk_file_write(g_file, channel, buf, 0 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 1 * sizeof(buf));
	spdk_file_write(g_file, channel, buf, 1 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 2 * sizeof(buf));
	spdk_file_sync(g_file, channel);

	fs_thread_poll();

	spdk_file_write(g_file, channel, buf, 2 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 3 * sizeof(buf));
	spdk_file_write(g_file, channel, buf, 3 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 4 * sizeof(buf));
	spdk_file_write(g_file, channel, buf, 4 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 5 * sizeof(buf));

	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_thread_ctx(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
fs_delete_file_without_close(void)
{
	int rc;
	struct spdk_fs_thread_ctx *channel;
	struct spdk_file *file;

	ut_send_request(_fs_init, NULL);
	channel = spdk_fs_alloc_thread_ctx(g_fs);
	CU_ASSERT(channel != NULL);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_file->ref_count != 0);
	CU_ASSERT(g_file->is_deleted == true);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", 0, &file);
	CU_ASSERT(rc != 0);

	spdk_file_close(g_file, channel);

	fs_thread_poll();

	rc = spdk_fs_open_file(g_fs, channel, "testfile", 0, &file);
	CU_ASSERT(rc != 0);

	spdk_fs_free_thread_ctx(channel);

	ut_send_request(_fs_unload, NULL);

}

static bool g_thread_exit = false;

static void
terminate_spdk_thread(void *arg)
{
	g_thread_exit = true;
}

static void *
spdk_thread(void *arg)
{
	struct spdk_thread *thread = arg;

	spdk_set_thread(thread);

	while (!g_thread_exit) {
		spdk_thread_poll(thread, 0, 0);
	}

	return NULL;
}

int
main(int argc, char **argv)
{
	struct spdk_thread *thread;
	CU_pSuite	suite = NULL;
	pthread_t	spdk_tid;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("blobfs_sync_ut", NULL, NULL);

	CU_ADD_TEST(suite, cache_read_after_write);
	CU_ADD_TEST(suite, file_length);
	CU_ADD_TEST(suite, append_write_to_extend_blob);
	CU_ADD_TEST(suite, partial_buffer);
	CU_ADD_TEST(suite, cache_write_null_buffer);
	CU_ADD_TEST(suite, fs_create_sync);
	CU_ADD_TEST(suite, fs_rename_sync);
	CU_ADD_TEST(suite, cache_append_no_cache);
	CU_ADD_TEST(suite, fs_delete_file_without_close);

	spdk_thread_lib_init(NULL, 0);

	thread = spdk_thread_create("test_thread", NULL);
	spdk_set_thread(thread);

	g_dispatch_thread = spdk_thread_create("dispatch_thread", NULL);
	pthread_create(&spdk_tid, NULL, spdk_thread, g_dispatch_thread);

	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free(g_dev_buffer);

	ut_send_request(terminate_spdk_thread, NULL);
	pthread_join(spdk_tid, NULL);

	while (spdk_thread_poll(g_dispatch_thread, 0, 0) > 0) {}
	while (spdk_thread_poll(thread, 0, 0) > 0) {}

	spdk_set_thread(thread);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	spdk_set_thread(g_dispatch_thread);
	spdk_thread_exit(g_dispatch_thread);
	while (!spdk_thread_is_exited(g_dispatch_thread)) {
		spdk_thread_poll(g_dispatch_thread, 0, 0);
	}
	spdk_thread_destroy(g_dispatch_thread);

	spdk_thread_lib_fini();

	return num_failures;
}
