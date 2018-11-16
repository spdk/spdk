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

#include "spdk/blobfs.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/barrier.h"

#include "spdk_cunit.h"
#include "unit/lib/blob/bs_dev_common.c"
#include "common/lib/test_env.c"
#include "blobfs/blobfs.c"
#include "blobfs/tree.c"

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
_fs_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

struct ut_request {
	fs_request_fn fn;
	void *arg;
	volatile int done;
	int from_ut;
};

static struct ut_request *g_req = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
send_request(fs_request_fn fn, void *arg)
{
	struct ut_request *req;

	req = calloc(1, sizeof(*req));
	assert(req != NULL);
	req->fn = fn;
	req->arg = arg;
	req->done = 0;
	req->from_ut = 0;

	pthread_mutex_lock(&g_mutex);
	g_req = req;
	pthread_mutex_unlock(&g_mutex);
}

static void
ut_send_request(fs_request_fn fn, void *arg)
{
	struct ut_request req;


	req.fn = fn;
	req.arg = arg;
	req.done = 0;
	req.from_ut = 1;

	pthread_mutex_lock(&g_mutex);
	g_req = &req;
	pthread_mutex_unlock(&g_mutex);

	while (1) {
		pthread_mutex_lock(&g_mutex);
		if (req.done == 1) {
			pthread_mutex_unlock(&g_mutex);
			break;
		}
		pthread_mutex_unlock(&g_mutex);
	}

	/*
	 * Make sure the address of the local req variable is not in g_req when we exit this
	 * function to make static analysis tools happy.
	 */
	g_req = NULL;
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
_fs_init(void *arg)
{
	struct spdk_bs_dev *dev;

	g_fs = NULL;
	g_fserrno = -1;
	dev = init_dev();
	spdk_fs_init(dev, NULL, send_request, fs_op_with_handle_complete, NULL);
	SPDK_CU_ASSERT_FATAL(g_fs != NULL);
	CU_ASSERT(g_fserrno == 0);
}

static void
_fs_unload(void *arg)
{
	g_fserrno = -1;
	spdk_fs_unload(g_fs, fs_op_complete, NULL);
	CU_ASSERT(g_fserrno == 0);
	g_fs = NULL;
}

static void
cache_write(void)
{
	uint64_t length;
	int rc;
	char buf[100];
	struct spdk_io_channel *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_io_channel_sync(g_fs);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	length = (4 * 1024 * 1024);
	rc = spdk_file_truncate(g_file, channel, length);
	CU_ASSERT(rc == 0);

	spdk_file_write(g_file, channel, buf, 0, sizeof(buf));

	CU_ASSERT(spdk_file_get_length(g_file) == length);

	rc = spdk_file_truncate(g_file, channel, sizeof(buf));
	CU_ASSERT(rc == 0);

	spdk_file_close(g_file, channel);
	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == -ENOENT);

	spdk_fs_free_io_channel(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
cache_write_null_buffer(void)
{
	uint64_t length;
	int rc;
	struct spdk_io_channel *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_io_channel_sync(g_fs);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	length = 0;
	rc = spdk_file_truncate(g_file, channel, length);
	CU_ASSERT(rc == 0);

	rc = spdk_file_write(g_file, channel, NULL, 0, 0);
	CU_ASSERT(rc == 0);

	spdk_file_close(g_file, channel);
	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_io_channel(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
fs_create_sync(void)
{
	int rc;
	struct spdk_io_channel *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_io_channel_sync(g_fs);
	CU_ASSERT(channel != NULL);

	rc = spdk_fs_create_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	/* Create should fail, because the file already exists. */
	rc = spdk_fs_create_file(g_fs, channel, "testfile");
	CU_ASSERT(rc != 0);

	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_io_channel(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
cache_append_no_cache(void)
{
	int rc;
	char buf[100];
	struct spdk_io_channel *channel;

	ut_send_request(_fs_init, NULL);

	channel = spdk_fs_alloc_io_channel_sync(g_fs);

	rc = spdk_fs_open_file(g_fs, channel, "testfile", SPDK_BLOBFS_OPEN_CREATE, &g_file);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_file != NULL);

	spdk_file_write(g_file, channel, buf, 0 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 1 * sizeof(buf));
	spdk_file_write(g_file, channel, buf, 1 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 2 * sizeof(buf));
	spdk_file_sync(g_file, channel);
	cache_free_buffers(g_file);
	spdk_file_write(g_file, channel, buf, 2 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 3 * sizeof(buf));
	spdk_file_write(g_file, channel, buf, 3 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 4 * sizeof(buf));
	spdk_file_write(g_file, channel, buf, 4 * sizeof(buf), sizeof(buf));
	CU_ASSERT(spdk_file_get_length(g_file) == 5 * sizeof(buf));

	spdk_file_close(g_file, channel);
	rc = spdk_fs_delete_file(g_fs, channel, "testfile");
	CU_ASSERT(rc == 0);

	spdk_fs_free_io_channel(channel);

	ut_send_request(_fs_unload, NULL);
}

static void
fs_delete_file_without_close(void)
{
	int rc;
	struct spdk_io_channel *channel;
	struct spdk_file *file;

	ut_send_request(_fs_init, NULL);
	channel = spdk_fs_alloc_io_channel_sync(g_fs);
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

	rc = spdk_fs_open_file(g_fs, channel, "testfile", 0, &file);
	CU_ASSERT(rc != 0);

	spdk_fs_free_io_channel(channel);

	ut_send_request(_fs_unload, NULL);

}

static void
terminate_spdk_thread(void *arg)
{
	spdk_free_thread();
	pthread_exit(NULL);
}

static void *
spdk_thread(void *arg)
{
	struct ut_request *req;

	spdk_allocate_thread(_fs_send_msg, NULL, NULL, NULL, "thread1");

	while (1) {
		pthread_mutex_lock(&g_mutex);
		if (g_req != NULL) {
			req = g_req;
			req->fn(req->arg);
			req->done = 1;
			if (!req->from_ut) {
				free(req);
			}
			g_req = NULL;
		}
		pthread_mutex_unlock(&g_mutex);
	}

	spdk_free_thread();

	return NULL;
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	pthread_t	spdk_tid;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("blobfs_sync_ut", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "write", cache_write) == NULL ||
		CU_add_test(suite, "write_null_buffer", cache_write_null_buffer) == NULL ||
		CU_add_test(suite, "create_sync", fs_create_sync) == NULL ||
		CU_add_test(suite, "append_no_cache", cache_append_no_cache) == NULL ||
		CU_add_test(suite, "delete_file_without_close", fs_delete_file_without_close) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	spdk_allocate_thread(_fs_send_msg, NULL, NULL, NULL, "thread0");

	pthread_create(&spdk_tid, NULL, spdk_thread, NULL);
	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	free(g_dev_buffer);
	send_request(terminate_spdk_thread, NULL);
	pthread_join(spdk_tid, NULL);
	spdk_free_thread();
	return num_failures;
}
