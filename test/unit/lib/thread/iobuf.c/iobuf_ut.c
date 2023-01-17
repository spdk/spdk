/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation. All rights reserved.
 */

#include "spdk_cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
#include "spdk/thread.h"

#include "thread/iobuf.c"

struct ut_iobuf_entry {
	struct spdk_iobuf_channel	*ioch;
	struct spdk_iobuf_entry		iobuf;
	void				*buf;
	uint32_t			thread_id;
	const char			*module;
};

static void
ut_iobuf_finish_cb(void *ctx)
{
	*(int *)ctx = 1;
}

static void
ut_iobuf_get_buf_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct ut_iobuf_entry *ut_entry = SPDK_CONTAINEROF(entry, struct ut_iobuf_entry, iobuf);

	ut_entry->buf = buf;
}

static int
ut_iobuf_foreach_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry, void *cb_arg)
{
	struct ut_iobuf_entry *ut_entry = SPDK_CONTAINEROF(entry, struct ut_iobuf_entry, iobuf);

	ut_entry->buf = cb_arg;

	return 0;
}

#define SMALL_BUFSIZE 128
#define LARGE_BUFSIZE 512

static void
iobuf(void)
{
	struct spdk_iobuf_opts opts = {
		.small_pool_count = 2,
		.large_pool_count = 2,
		.small_bufsize = SMALL_BUFSIZE,
		.large_bufsize = LARGE_BUFSIZE,
	};
	struct ut_iobuf_entry *entry;
	struct spdk_iobuf_channel mod0_ch[2], mod1_ch[2];
	struct ut_iobuf_entry mod0_entries[] = {
		{ .thread_id = 0, .module = "ut_module0", },
		{ .thread_id = 0, .module = "ut_module0", },
		{ .thread_id = 0, .module = "ut_module0", },
		{ .thread_id = 0, .module = "ut_module0", },
		{ .thread_id = 1, .module = "ut_module0", },
		{ .thread_id = 1, .module = "ut_module0", },
		{ .thread_id = 1, .module = "ut_module0", },
		{ .thread_id = 1, .module = "ut_module0", },
	};
	struct ut_iobuf_entry mod1_entries[] = {
		{ .thread_id = 0, .module = "ut_module1", },
		{ .thread_id = 0, .module = "ut_module1", },
		{ .thread_id = 0, .module = "ut_module1", },
		{ .thread_id = 0, .module = "ut_module1", },
		{ .thread_id = 1, .module = "ut_module1", },
		{ .thread_id = 1, .module = "ut_module1", },
		{ .thread_id = 1, .module = "ut_module1", },
		{ .thread_id = 1, .module = "ut_module1", },
	};
	int rc, finish = 0;
	uint32_t i;

	allocate_cores(2);
	allocate_threads(2);

	set_thread(0);

	/* We cannot use spdk_iobuf_set_opts(), as it won't allow us to use such small pools */
	g_iobuf.opts = opts;
	rc = spdk_iobuf_initialize();
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_iobuf_register_module("ut_module0");
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_iobuf_register_module("ut_module1");
	CU_ASSERT_EQUAL(rc, 0);

	set_thread(0);
	rc = spdk_iobuf_channel_init(&mod0_ch[0], "ut_module0", 0, 0);
	CU_ASSERT_EQUAL(rc, 0);
	set_thread(1);
	rc = spdk_iobuf_channel_init(&mod0_ch[1], "ut_module0", 0, 0);
	CU_ASSERT_EQUAL(rc, 0);
	for (i = 0; i < SPDK_COUNTOF(mod0_entries); ++i) {
		mod0_entries[i].ioch = &mod0_ch[mod0_entries[i].thread_id];
	}
	set_thread(0);
	rc = spdk_iobuf_channel_init(&mod1_ch[0], "ut_module1", 0, 0);
	CU_ASSERT_EQUAL(rc, 0);
	set_thread(1);
	rc = spdk_iobuf_channel_init(&mod1_ch[1], "ut_module1", 0, 0);
	CU_ASSERT_EQUAL(rc, 0);
	for (i = 0; i < SPDK_COUNTOF(mod1_entries); ++i) {
		mod1_entries[i].ioch = &mod1_ch[mod1_entries[i].thread_id];
	}

	/* First check that it's possible to retrieve the whole pools from a single module */
	set_thread(0);
	entry = &mod0_entries[0];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[1];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	/* The next two should be put onto the large buf wait queue */
	entry = &mod0_entries[2];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod0_entries[3];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	/* Pick the two next buffers from the small pool */
	set_thread(1);
	entry = &mod0_entries[4];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[5];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	/* The next two should be put onto the small buf wait queue */
	entry = &mod0_entries[6];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod0_entries[7];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Now return one of the large buffers to the pool and verify that the first request's
	 * (entry 2) callback was executed and it was removed from the wait queue.
	 */
	set_thread(0);
	entry = &mod0_entries[0];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod0_entries[2];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[3];
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Return the second buffer and check that the other request is satisfied */
	entry = &mod0_entries[1];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod0_entries[3];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);

	/* Return the remaining two buffers */
	entry = &mod0_entries[2];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod0_entries[3];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);

	/* Check that it didn't change the requests waiting for the small buffers */
	entry = &mod0_entries[6];
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod0_entries[7];
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Do the same test as above, this time using the small pool */
	set_thread(1);
	entry = &mod0_entries[4];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod0_entries[6];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[7];
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Return the second buffer and check that the other request is satisfied */
	entry = &mod0_entries[5];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod0_entries[7];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);

	/* Return the remaining two buffers */
	entry = &mod0_entries[6];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod0_entries[7];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);

	/* Now check requesting buffers from different modules - first request all of them from one
	 * module, starting from the large pool
	 */
	set_thread(0);
	entry = &mod0_entries[0];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[1];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	/* Request all of them from the small one */
	set_thread(1);
	entry = &mod0_entries[4];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[5];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);

	/* Request one buffer per module from each pool  */
	set_thread(0);
	entry = &mod1_entries[0];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod0_entries[3];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	/* Change the order from the small pool and request a buffer from mod0 first */
	set_thread(1);
	entry = &mod0_entries[6];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod1_entries[4];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Now return one buffer to the large pool */
	set_thread(0);
	entry = &mod0_entries[0];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);

	/* Make sure the request from mod1 got the buffer, as it was the first to request it */
	entry = &mod1_entries[0];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[3];
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Return second buffer to the large pool and check the outstanding mod0 request */
	entry = &mod0_entries[1];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod0_entries[3];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);

	/* Return the remaining two buffers */
	entry = &mod1_entries[0];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod0_entries[3];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);

	/* Check the same for the small pool, but this time the order of the request is reversed
	 * (mod0 before mod1)
	 */
	set_thread(1);
	entry = &mod0_entries[4];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod0_entries[6];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	/* mod1 request was second in this case, so it still needs to wait */
	entry = &mod1_entries[4];
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Return the second requested buffer */
	entry = &mod0_entries[5];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod1_entries[4];
	CU_ASSERT_PTR_NOT_NULL(entry->buf);

	/* Return the remaining two buffers */
	entry = &mod0_entries[6];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod1_entries[4];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);

	/* Request buffers to make the pools empty */
	set_thread(0);
	entry = &mod0_entries[0];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod1_entries[0];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[1];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod1_entries[1];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);

	/* Queue more requests from both modules */
	entry = &mod0_entries[2];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod1_entries[2];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod1_entries[3];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod0_entries[3];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Check that abort correctly remove an entry from the queue */
	entry = &mod0_entries[2];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, LARGE_BUFSIZE);
	entry = &mod1_entries[3];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, SMALL_BUFSIZE);

	entry = &mod0_entries[0];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	CU_ASSERT_PTR_NOT_NULL(mod1_entries[2].buf);
	entry = &mod0_entries[1];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	CU_ASSERT_PTR_NOT_NULL(mod0_entries[3].buf);

	/* Clean up */
	entry = &mod1_entries[0];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod1_entries[2];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod1_entries[1];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod0_entries[3];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);

	/* Request buffers to make the pools empty */
	set_thread(0);
	entry = &mod0_entries[0];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod1_entries[0];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod0_entries[1];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);
	entry = &mod1_entries[1];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NOT_NULL(entry->buf);

	/* Request a buffer from each queue and each module on thread 0 */
	set_thread(0);
	entry = &mod0_entries[2];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod1_entries[2];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod0_entries[3];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod1_entries[3];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Do the same on thread 1 */
	set_thread(1);
	entry = &mod0_entries[6];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod1_entries[6];
	entry->buf = spdk_iobuf_get(entry->ioch, LARGE_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod0_entries[7];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);
	entry = &mod1_entries[7];
	entry->buf = spdk_iobuf_get(entry->ioch, SMALL_BUFSIZE, &entry->iobuf, ut_iobuf_get_buf_cb);
	CU_ASSERT_PTR_NULL(entry->buf);

	/* Now do the foreach and check that correct entries are iterated over by assigning their
	 * ->buf pointers to different values.
	 */
	set_thread(0);
	rc = spdk_iobuf_for_each_entry(&mod0_ch[0], &mod0_ch[0].large,
				       ut_iobuf_foreach_cb, (void *)0xdeadbeef);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_for_each_entry(&mod0_ch[0], &mod0_ch[0].small,
				       ut_iobuf_foreach_cb, (void *)0xbeefdead);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_for_each_entry(&mod1_ch[0], &mod1_ch[0].large,
				       ut_iobuf_foreach_cb, (void *)0xfeedbeef);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_for_each_entry(&mod1_ch[0], &mod1_ch[0].small,
				       ut_iobuf_foreach_cb, (void *)0xbeeffeed);
	CU_ASSERT_EQUAL(rc, 0);
	set_thread(1);
	rc = spdk_iobuf_for_each_entry(&mod0_ch[1], &mod0_ch[1].large,
				       ut_iobuf_foreach_cb, (void *)0xcafebabe);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_for_each_entry(&mod0_ch[1], &mod0_ch[1].small,
				       ut_iobuf_foreach_cb, (void *)0xbabecafe);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_for_each_entry(&mod1_ch[1], &mod1_ch[1].large,
				       ut_iobuf_foreach_cb, (void *)0xbeefcafe);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_for_each_entry(&mod1_ch[1], &mod1_ch[1].small,
				       ut_iobuf_foreach_cb, (void *)0xcafebeef);
	CU_ASSERT_EQUAL(rc, 0);

	/* thread 0 */
	CU_ASSERT_PTR_EQUAL(mod0_entries[2].buf, (void *)0xdeadbeef);
	CU_ASSERT_PTR_EQUAL(mod0_entries[3].buf, (void *)0xbeefdead);
	CU_ASSERT_PTR_EQUAL(mod1_entries[2].buf, (void *)0xfeedbeef);
	CU_ASSERT_PTR_EQUAL(mod1_entries[3].buf, (void *)0xbeeffeed);
	/* thread 1 */
	CU_ASSERT_PTR_EQUAL(mod0_entries[6].buf, (void *)0xcafebabe);
	CU_ASSERT_PTR_EQUAL(mod0_entries[7].buf, (void *)0xbabecafe);
	CU_ASSERT_PTR_EQUAL(mod1_entries[6].buf, (void *)0xbeefcafe);
	CU_ASSERT_PTR_EQUAL(mod1_entries[7].buf, (void *)0xcafebeef);

	/* Clean everything up */
	set_thread(0);
	entry = &mod0_entries[2];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, LARGE_BUFSIZE);
	entry = &mod0_entries[3];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, SMALL_BUFSIZE);
	entry = &mod1_entries[2];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, LARGE_BUFSIZE);
	entry = &mod1_entries[3];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, SMALL_BUFSIZE);

	entry = &mod0_entries[0];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod1_entries[0];
	spdk_iobuf_put(entry->ioch, entry->buf, LARGE_BUFSIZE);
	entry = &mod0_entries[1];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);
	entry = &mod1_entries[1];
	spdk_iobuf_put(entry->ioch, entry->buf, SMALL_BUFSIZE);

	set_thread(1);
	entry = &mod0_entries[6];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, LARGE_BUFSIZE);
	entry = &mod0_entries[7];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, SMALL_BUFSIZE);
	entry = &mod1_entries[6];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, LARGE_BUFSIZE);
	entry = &mod1_entries[7];
	spdk_iobuf_entry_abort(entry->ioch, &entry->iobuf, SMALL_BUFSIZE);

	set_thread(0);
	spdk_iobuf_channel_fini(&mod0_ch[0]);
	poll_threads();
	spdk_iobuf_channel_fini(&mod1_ch[0]);
	poll_threads();
	set_thread(1);
	spdk_iobuf_channel_fini(&mod0_ch[1]);
	poll_threads();
	spdk_iobuf_channel_fini(&mod1_ch[1]);
	poll_threads();

	spdk_iobuf_finish(ut_iobuf_finish_cb, &finish);
	poll_threads();

	CU_ASSERT_EQUAL(finish, 1);

	free_threads();
	free_cores();
}

static void
iobuf_cache(void)
{
	struct spdk_iobuf_opts opts = {
		.small_pool_count = 4,
		.large_pool_count = 4,
		.small_bufsize = SMALL_BUFSIZE,
		.large_bufsize = LARGE_BUFSIZE,
	};
	struct spdk_iobuf_channel iobuf_ch[2];
	struct ut_iobuf_entry *entry;
	struct ut_iobuf_entry mod0_entries[] = {
		{ .thread_id = 0, .module = "ut_module0", },
		{ .thread_id = 0, .module = "ut_module0", },
		{ .thread_id = 0, .module = "ut_module0", },
		{ .thread_id = 0, .module = "ut_module0", },
	};
	struct ut_iobuf_entry mod1_entries[] = {
		{ .thread_id = 0, .module = "ut_module1", },
		{ .thread_id = 0, .module = "ut_module1", },
	};
	int rc, finish = 0;
	uint32_t i, j, bufsize;

	allocate_cores(1);
	allocate_threads(1);

	set_thread(0);

	/* We cannot use spdk_iobuf_set_opts(), as it won't allow us to use such small pools */
	g_iobuf.opts = opts;
	rc = spdk_iobuf_initialize();
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_iobuf_register_module("ut_module0");
	CU_ASSERT_EQUAL(rc, 0);

	rc = spdk_iobuf_register_module("ut_module1");
	CU_ASSERT_EQUAL(rc, 0);

	/* First check that channel initialization fails when it's not possible to fill in the cache
	 * from the pool.
	 */
	rc = spdk_iobuf_channel_init(&iobuf_ch[0], "ut_module0", 5, 1);
	CU_ASSERT_EQUAL(rc, -ENOMEM);
	rc = spdk_iobuf_channel_init(&iobuf_ch[0], "ut_module0", 1, 5);
	CU_ASSERT_EQUAL(rc, -ENOMEM);

	rc = spdk_iobuf_channel_init(&iobuf_ch[0], "ut_module0", 4, 4);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_channel_init(&iobuf_ch[1], "ut_module1", 4, 4);
	CU_ASSERT_EQUAL(rc, -ENOMEM);

	spdk_iobuf_channel_fini(&iobuf_ch[0]);
	poll_threads();

	/* Initialize one channel with cache, acquire buffers, and check that a second one can be
	 * created once the buffers acquired from the first one are returned to the pool
	 */
	rc = spdk_iobuf_channel_init(&iobuf_ch[0], "ut_module0", 2, 2);
	CU_ASSERT_EQUAL(rc, 0);

	for (i = 0; i < 3; ++i) {
		mod0_entries[i].buf = spdk_iobuf_get(&iobuf_ch[0], LARGE_BUFSIZE, &mod0_entries[i].iobuf,
						     ut_iobuf_get_buf_cb);
		CU_ASSERT_PTR_NOT_NULL(mod0_entries[i].buf);
	}

	/* It should be able to create a channel with a single entry in the cache */
	rc = spdk_iobuf_channel_init(&iobuf_ch[1], "ut_module1", 2, 1);
	CU_ASSERT_EQUAL(rc, 0);
	spdk_iobuf_channel_fini(&iobuf_ch[1]);
	poll_threads();

	/* But not with two entries */
	rc = spdk_iobuf_channel_init(&iobuf_ch[1], "ut_module1", 2, 2);
	CU_ASSERT_EQUAL(rc, -ENOMEM);

	for (i = 0; i < 2; ++i) {
		spdk_iobuf_put(&iobuf_ch[0], mod0_entries[i].buf, LARGE_BUFSIZE);
		rc = spdk_iobuf_channel_init(&iobuf_ch[1], "ut_module1", 2, 2);
		CU_ASSERT_EQUAL(rc, -ENOMEM);
	}

	spdk_iobuf_put(&iobuf_ch[0], mod0_entries[2].buf, LARGE_BUFSIZE);

	/* The last buffer should be released back to the pool, so we should be able to create a new
	 * channel
	 */
	rc = spdk_iobuf_channel_init(&iobuf_ch[1], "ut_module1", 2, 2);
	CU_ASSERT_EQUAL(rc, 0);

	spdk_iobuf_channel_fini(&iobuf_ch[0]);
	spdk_iobuf_channel_fini(&iobuf_ch[1]);
	poll_threads();

	/* Check that the pool is only used when the cache is empty and that the cache guarantees a
	 * certain set of buffers
	 */
	rc = spdk_iobuf_channel_init(&iobuf_ch[0], "ut_module0", 2, 2);
	CU_ASSERT_EQUAL(rc, 0);
	rc = spdk_iobuf_channel_init(&iobuf_ch[1], "ut_module1", 1, 1);
	CU_ASSERT_EQUAL(rc, 0);

	uint32_t buffer_sizes[] = { SMALL_BUFSIZE, LARGE_BUFSIZE };
	for (i = 0; i < SPDK_COUNTOF(buffer_sizes); ++i) {
		bufsize = buffer_sizes[i];

		for (j = 0; j < 3; ++j) {
			entry = &mod0_entries[j];
			entry->buf = spdk_iobuf_get(&iobuf_ch[0], bufsize, &entry->iobuf,
						    ut_iobuf_get_buf_cb);
			CU_ASSERT_PTR_NOT_NULL(entry->buf);
		}

		mod1_entries[0].buf = spdk_iobuf_get(&iobuf_ch[1], bufsize, &mod1_entries[0].iobuf,
						     ut_iobuf_get_buf_cb);
		CU_ASSERT_PTR_NOT_NULL(mod1_entries[0].buf);

		/* The whole pool is exhausted now */
		mod1_entries[1].buf = spdk_iobuf_get(&iobuf_ch[1], bufsize, &mod1_entries[1].iobuf,
						     ut_iobuf_get_buf_cb);
		CU_ASSERT_PTR_NULL(mod1_entries[1].buf);
		mod0_entries[3].buf = spdk_iobuf_get(&iobuf_ch[0], bufsize, &mod0_entries[3].iobuf,
						     ut_iobuf_get_buf_cb);
		CU_ASSERT_PTR_NULL(mod0_entries[3].buf);

		/* If there are outstanding requests waiting for a buffer, they should have priority
		 * over filling in the cache, even if they're from different modules.
		 */
		spdk_iobuf_put(&iobuf_ch[0], mod0_entries[2].buf, bufsize);
		/* Also make sure the queue is FIFO and doesn't care about which module requested
		 * and which module released the buffer.
		 */
		CU_ASSERT_PTR_NOT_NULL(mod1_entries[1].buf);
		CU_ASSERT_PTR_NULL(mod0_entries[3].buf);

		/* Return the buffers back */
		spdk_iobuf_entry_abort(&iobuf_ch[0], &mod0_entries[3].iobuf, bufsize);
		for (j = 0; j < 2; ++j) {
			spdk_iobuf_put(&iobuf_ch[0], mod0_entries[j].buf, bufsize);
			spdk_iobuf_put(&iobuf_ch[1], mod1_entries[j].buf, bufsize);
		}
	}

	spdk_iobuf_channel_fini(&iobuf_ch[0]);
	spdk_iobuf_channel_fini(&iobuf_ch[1]);
	poll_threads();

	spdk_iobuf_finish(ut_iobuf_finish_cb, &finish);
	poll_threads();

	CU_ASSERT_EQUAL(finish, 1);

	free_threads();
	free_cores();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("io_channel", NULL, NULL);
	CU_ADD_TEST(suite, iobuf);
	CU_ADD_TEST(suite, iobuf_cache);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
