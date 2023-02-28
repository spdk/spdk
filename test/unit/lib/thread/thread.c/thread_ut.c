/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "thread/thread_internal.h"

#include "thread/thread.c"
#include "common/lib/ut_multithread.c"

#define SMALL_BUFSIZE 128
#define LARGE_BUFSIZE 512

static int g_sched_rc = 0;

static int
_thread_schedule(struct spdk_thread *thread)
{
	return g_sched_rc;
}

static bool
_thread_op_supported(enum spdk_thread_op op)
{
	switch (op) {
	case SPDK_THREAD_OP_NEW:
		return true;
	default:
		return false;
	}
}

static int
_thread_op(struct spdk_thread *thread, enum spdk_thread_op op)
{
	switch (op) {
	case SPDK_THREAD_OP_NEW:
		return _thread_schedule(thread);
	default:
		return -ENOTSUP;
	}
}

static void
thread_alloc(void)
{
	struct spdk_thread *thread;

	/* No schedule callback */
	spdk_thread_lib_init(NULL, 0);
	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
	spdk_thread_lib_fini();

	/* Schedule callback exists */
	spdk_thread_lib_init(_thread_schedule, 0);

	/* Scheduling succeeds */
	g_sched_rc = 0;
	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	/* Scheduling fails */
	g_sched_rc = -1;
	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread == NULL);

	spdk_thread_lib_fini();

	/* Scheduling callback exists with extended thread library initialization. */
	spdk_thread_lib_init_ext(_thread_op, _thread_op_supported, 0,
				 SPDK_DEFAULT_MSG_MEMPOOL_SIZE);

	/* Scheduling succeeds */
	g_sched_rc = 0;
	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	/* Scheduling fails */
	g_sched_rc = -1;
	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread == NULL);

	spdk_thread_lib_fini();
}

static void
send_msg_cb(void *ctx)
{
	bool *done = ctx;

	*done = true;
}

static void
thread_send_msg(void)
{
	struct spdk_thread *thread0;
	bool done = false;

	allocate_threads(2);
	set_thread(0);
	thread0 = spdk_get_thread();

	set_thread(1);
	/* Simulate thread 1 sending a message to thread 0. */
	spdk_thread_send_msg(thread0, send_msg_cb, &done);

	/* We have not polled thread 0 yet, so done should be false. */
	CU_ASSERT(!done);

	/*
	 * Poll thread 1.  The message was sent to thread 0, so this should be
	 *  a nop and done should still be false.
	 */
	poll_thread(1);
	CU_ASSERT(!done);

	/*
	 * Poll thread 0.  This should execute the message and done should then
	 *  be true.
	 */
	poll_thread(0);
	CU_ASSERT(done);

	free_threads();
}

static int
poller_run_done(void *ctx)
{
	bool	*poller_run = ctx;

	*poller_run = true;

	return -1;
}

static void
thread_poller(void)
{
	struct spdk_poller	*poller = NULL;
	bool			poller_run = false;

	allocate_threads(1);

	set_thread(0);
	MOCK_SET(spdk_get_ticks, 0);
	/* Register a poller with no-wait time and test execution */
	poller = spdk_poller_register(poller_run_done, &poller_run, 0);
	CU_ASSERT(poller != NULL);

	poll_threads();
	CU_ASSERT(poller_run == true);

	spdk_poller_unregister(&poller);
	CU_ASSERT(poller == NULL);

	/* Register a poller with 1000us wait time and test single execution */
	poller_run = false;
	poller = spdk_poller_register(poller_run_done, &poller_run, 1000);
	CU_ASSERT(poller != NULL);

	poll_threads();
	CU_ASSERT(poller_run == false);

	spdk_delay_us(1000);
	poll_threads();
	CU_ASSERT(poller_run == true);

	poller_run = false;
	poll_threads();
	CU_ASSERT(poller_run == false);

	spdk_delay_us(1000);
	poll_threads();
	CU_ASSERT(poller_run == true);

	spdk_poller_unregister(&poller);
	CU_ASSERT(poller == NULL);

	free_threads();
}

struct poller_ctx {
	struct spdk_poller	*poller;
	bool			run;
};

static int
poller_run_pause(void *ctx)
{
	struct poller_ctx *poller_ctx = ctx;

	poller_ctx->run = true;
	spdk_poller_pause(poller_ctx->poller);

	return 0;
}

/* Verify the same poller can be switched multiple times between
 * pause and resume while it runs.
 */
static int
poller_run_pause_resume_pause(void *ctx)
{
	struct poller_ctx *poller_ctx = ctx;

	poller_ctx->run = true;

	spdk_poller_pause(poller_ctx->poller);
	spdk_poller_resume(poller_ctx->poller);
	spdk_poller_pause(poller_ctx->poller);

	return 0;
}

static void
poller_msg_pause_cb(void *ctx)
{
	struct spdk_poller *poller = ctx;

	spdk_poller_pause(poller);
}

static void
poller_msg_resume_cb(void *ctx)
{
	struct spdk_poller *poller = ctx;

	spdk_poller_resume(poller);
}

static void
poller_pause(void)
{
	struct poller_ctx poller_ctx = {};
	unsigned int delay[] = { 0, 1000 };
	unsigned int i;

	allocate_threads(1);
	set_thread(0);

	/* Register a poller that pauses itself */
	poller_ctx.poller = spdk_poller_register(poller_run_pause, &poller_ctx, 0);
	CU_ASSERT_PTR_NOT_NULL(poller_ctx.poller);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, true);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, false);

	spdk_poller_unregister(&poller_ctx.poller);
	CU_ASSERT_PTR_NULL(poller_ctx.poller);

	/* Register a poller that switches between pause and resume itself */
	poller_ctx.poller = spdk_poller_register(poller_run_pause_resume_pause, &poller_ctx, 0);
	CU_ASSERT_PTR_NOT_NULL(poller_ctx.poller);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, true);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, false);

	spdk_poller_unregister(&poller_ctx.poller);
	CU_ASSERT_PTR_NULL(poller_ctx.poller);

	/* Verify that resuming an unpaused poller doesn't do anything */
	poller_ctx.poller = spdk_poller_register(poller_run_done, &poller_ctx.run, 0);
	CU_ASSERT_PTR_NOT_NULL(poller_ctx.poller);

	spdk_poller_resume(poller_ctx.poller);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, true);

	/* Verify that pausing the same poller twice works too */
	spdk_poller_pause(poller_ctx.poller);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, false);

	spdk_poller_pause(poller_ctx.poller);
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, false);

	spdk_poller_resume(poller_ctx.poller);
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, true);

	/* Verify that a poller is run when it's resumed immediately after pausing */
	poller_ctx.run = false;
	spdk_poller_pause(poller_ctx.poller);
	spdk_poller_resume(poller_ctx.poller);
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, true);

	spdk_poller_unregister(&poller_ctx.poller);
	CU_ASSERT_PTR_NULL(poller_ctx.poller);

	/* Poll the thread to make sure the previous poller gets unregistered */
	poll_threads();
	CU_ASSERT_EQUAL(spdk_thread_has_pollers(spdk_get_thread()), false);

	/* Verify that it's possible to unregister a paused poller */
	poller_ctx.poller = spdk_poller_register(poller_run_done, &poller_ctx.run, 0);
	CU_ASSERT_PTR_NOT_NULL(poller_ctx.poller);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, true);

	spdk_poller_pause(poller_ctx.poller);

	poller_ctx.run = false;
	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, false);

	spdk_poller_unregister(&poller_ctx.poller);

	poll_threads();
	CU_ASSERT_EQUAL(poller_ctx.run, false);
	CU_ASSERT_EQUAL(spdk_thread_has_pollers(spdk_get_thread()), false);

	/* Register pollers with 0 and 1000us wait time and pause/resume them */
	for (i = 0; i < SPDK_COUNTOF(delay); ++i) {
		poller_ctx.poller = spdk_poller_register(poller_run_done, &poller_ctx.run, delay[i]);
		CU_ASSERT_PTR_NOT_NULL(poller_ctx.poller);

		spdk_delay_us(delay[i]);
		poller_ctx.run = false;
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, true);

		spdk_poller_pause(poller_ctx.poller);

		spdk_delay_us(delay[i]);
		poller_ctx.run = false;
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, false);

		spdk_poller_resume(poller_ctx.poller);

		spdk_delay_us(delay[i]);
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, true);

		/* Verify that the poller can be paused/resumed from spdk_thread_send_msg */
		spdk_thread_send_msg(spdk_get_thread(), poller_msg_pause_cb, poller_ctx.poller);

		spdk_delay_us(delay[i]);
		poller_ctx.run = false;
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, false);

		spdk_thread_send_msg(spdk_get_thread(), poller_msg_resume_cb, poller_ctx.poller);

		poll_threads();
		if (delay[i] > 0) {
			spdk_delay_us(delay[i]);
			poll_threads();
		}
		CU_ASSERT_EQUAL(poller_ctx.run, true);

		spdk_poller_unregister(&poller_ctx.poller);
		CU_ASSERT_PTR_NULL(poller_ctx.poller);

		/* Register a timed poller that pauses itself */
		poller_ctx.poller = spdk_poller_register(poller_run_pause, &poller_ctx, delay[i]);
		CU_ASSERT_PTR_NOT_NULL(poller_ctx.poller);

		spdk_delay_us(delay[i]);
		poller_ctx.run = false;
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, true);

		poller_ctx.run = false;
		spdk_delay_us(delay[i]);
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, false);

		spdk_poller_resume(poller_ctx.poller);

		CU_ASSERT_EQUAL(poller_ctx.run, false);
		spdk_delay_us(delay[i]);
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, true);

		spdk_poller_unregister(&poller_ctx.poller);
		CU_ASSERT_PTR_NULL(poller_ctx.poller);

		/* Register a timed poller that switches between pause and resume itself */
		poller_ctx.poller = spdk_poller_register(poller_run_pause_resume_pause,
				    &poller_ctx, delay[i]);
		CU_ASSERT_PTR_NOT_NULL(poller_ctx.poller);

		spdk_delay_us(delay[i]);
		poller_ctx.run = false;
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, true);

		poller_ctx.run = false;
		spdk_delay_us(delay[i]);
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, false);

		spdk_poller_resume(poller_ctx.poller);

		CU_ASSERT_EQUAL(poller_ctx.run, false);
		spdk_delay_us(delay[i]);
		poll_threads();
		CU_ASSERT_EQUAL(poller_ctx.run, true);

		spdk_poller_unregister(&poller_ctx.poller);
		CU_ASSERT_PTR_NULL(poller_ctx.poller);
	}

	free_threads();
}

static void
for_each_cb(void *ctx)
{
	int *count = ctx;

	(*count)++;
}

static void
thread_for_each(void)
{
	int count = 0;
	int i;

	allocate_threads(3);
	set_thread(0);

	spdk_for_each_thread(for_each_cb, &count, for_each_cb);

	/* We have not polled thread 0 yet, so count should be 0 */
	CU_ASSERT(count == 0);

	/* Poll each thread to verify the message is passed to each */
	for (i = 0; i < 3; i++) {
		poll_thread(i);
		CU_ASSERT(count == (i + 1));
	}

	/*
	 * After each thread is called, the completion calls it
	 * one more time.
	 */
	poll_thread(0);
	CU_ASSERT(count == 4);

	free_threads();
}

static int
channel_create(void *io_device, void *ctx_buf)
{
	int *ch_count = io_device;

	(*ch_count)++;
	return 0;
}

static void
channel_destroy(void *io_device, void *ctx_buf)
{
	int *ch_count = io_device;

	(*ch_count)--;
}

static void
channel_msg(struct spdk_io_channel_iter *i)
{
	int *msg_count = spdk_io_channel_iter_get_ctx(i);

	(*msg_count)++;
	spdk_for_each_channel_continue(i, 0);
}

static void
channel_cpl(struct spdk_io_channel_iter *i, int status)
{
	int *msg_count = spdk_io_channel_iter_get_ctx(i);

	(*msg_count)++;
}

static void
for_each_channel_remove(void)
{
	struct spdk_io_channel *ch0, *ch1, *ch2;
	int ch_count = 0;
	int msg_count = 0;

	allocate_threads(3);
	set_thread(0);
	spdk_io_device_register(&ch_count, channel_create, channel_destroy, sizeof(int), NULL);
	ch0 = spdk_get_io_channel(&ch_count);
	set_thread(1);
	ch1 = spdk_get_io_channel(&ch_count);
	set_thread(2);
	ch2 = spdk_get_io_channel(&ch_count);
	CU_ASSERT(ch_count == 3);

	/*
	 * Test that io_channel handles the case where we start to iterate through
	 *  the channels, and during the iteration, one of the channels is deleted.
	 * This is done in some different and sometimes non-intuitive orders, because
	 *  some operations are deferred and won't execute until their threads are
	 *  polled.
	 *
	 * Case #1: Put the I/O channel before spdk_for_each_channel.
	 */
	set_thread(0);
	spdk_put_io_channel(ch0);
	CU_ASSERT(ch_count == 3);
	poll_threads();
	CU_ASSERT(ch_count == 2);
	spdk_for_each_channel(&ch_count, channel_msg, &msg_count, channel_cpl);
	CU_ASSERT(msg_count == 0);
	poll_threads();
	CU_ASSERT(msg_count == 3);

	msg_count = 0;

	/*
	 * Case #2: Put the I/O channel after spdk_for_each_channel, but before
	 *  thread 0 is polled.
	 */
	ch0 = spdk_get_io_channel(&ch_count);
	CU_ASSERT(ch_count == 3);
	spdk_for_each_channel(&ch_count, channel_msg, &msg_count, channel_cpl);
	spdk_put_io_channel(ch0);
	CU_ASSERT(ch_count == 3);

	poll_threads();
	CU_ASSERT(ch_count == 2);
	CU_ASSERT(msg_count == 4);
	set_thread(1);
	spdk_put_io_channel(ch1);
	CU_ASSERT(ch_count == 2);
	set_thread(2);
	spdk_put_io_channel(ch2);
	CU_ASSERT(ch_count == 2);
	poll_threads();
	CU_ASSERT(ch_count == 0);

	spdk_io_device_unregister(&ch_count, NULL);
	poll_threads();

	free_threads();
}

struct unreg_ctx {
	bool	ch_done;
	bool	foreach_done;
};

static void
unreg_ch_done(struct spdk_io_channel_iter *i)
{
	struct unreg_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->ch_done = true;

	SPDK_CU_ASSERT_FATAL(i->cur_thread != NULL);
	spdk_for_each_channel_continue(i, 0);
}

static void
unreg_foreach_done(struct spdk_io_channel_iter *i, int status)
{
	struct unreg_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->foreach_done = true;
}

static void
for_each_channel_unreg(void)
{
	struct spdk_io_channel *ch0;
	struct io_device *dev;
	struct unreg_ctx ctx = {};
	int io_target = 0;

	allocate_threads(1);
	set_thread(0);
	CU_ASSERT(RB_EMPTY(&g_io_devices));
	spdk_io_device_register(&io_target, channel_create, channel_destroy, sizeof(int), NULL);
	CU_ASSERT(!RB_EMPTY(&g_io_devices));
	dev = RB_MIN(io_device_tree, &g_io_devices);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(RB_NEXT(io_device_tree, &g_io_devices, dev) == NULL);
	ch0 = spdk_get_io_channel(&io_target);

	spdk_io_device_register(&io_target, channel_create, channel_destroy, sizeof(int), NULL);

	/*
	 * There is already a device registered at &io_target, so a new io_device should not
	 *  have been added to g_io_devices.
	 */
	CU_ASSERT(dev == RB_MIN(io_device_tree, &g_io_devices));
	CU_ASSERT(RB_NEXT(io_device_tree, &g_io_devices, dev) == NULL);

	spdk_for_each_channel(&io_target, unreg_ch_done, &ctx, unreg_foreach_done);
	spdk_io_device_unregister(&io_target, NULL);
	/*
	 * There is an outstanding foreach call on the io_device, so the unregister should not
	 *  have immediately removed the device.
	 */
	CU_ASSERT(dev == RB_MIN(io_device_tree, &g_io_devices));

	poll_thread(0);
	CU_ASSERT(ctx.ch_done == true);
	CU_ASSERT(ctx.foreach_done == true);

	/*
	 * There are no more foreach operations outstanding, so the device should be
	 * unregistered.
	 */
	CU_ASSERT(RB_EMPTY(&g_io_devices));

	set_thread(0);
	spdk_put_io_channel(ch0);

	poll_threads();

	free_threads();
}

static void
thread_name(void)
{
	struct spdk_thread *thread;
	const char *name;

	spdk_thread_lib_init(NULL, 0);

	/* Create thread with no name, which automatically generates one */
	thread = spdk_thread_create(NULL, NULL);
	spdk_set_thread(thread);
	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	name = spdk_thread_get_name(thread);
	CU_ASSERT(name != NULL);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	/* Create thread named "test_thread" */
	thread = spdk_thread_create("test_thread", NULL);
	spdk_set_thread(thread);
	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	name = spdk_thread_get_name(thread);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	CU_ASSERT(strcmp(name, "test_thread") == 0);
	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);

	spdk_thread_lib_fini();
}

static uint64_t g_device1;
static uint64_t g_device2;
static uint64_t g_device3;

static uint64_t g_ctx1 = 0x1111;
static uint64_t g_ctx2 = 0x2222;

static int g_create_cb_calls = 0;
static int g_destroy_cb_calls = 0;

static int
create_cb_1(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &g_device1);
	*(uint64_t *)ctx_buf = g_ctx1;
	g_create_cb_calls++;
	return 0;
}

static void
destroy_cb_1(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &g_device1);
	CU_ASSERT(*(uint64_t *)ctx_buf == g_ctx1);
	g_destroy_cb_calls++;
}

static int
create_cb_2(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &g_device2);
	*(uint64_t *)ctx_buf = g_ctx2;
	g_create_cb_calls++;
	return 0;
}

static void
destroy_cb_2(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &g_device2);
	CU_ASSERT(*(uint64_t *)ctx_buf == g_ctx2);
	g_destroy_cb_calls++;
}

static void
channel(void)
{
	struct spdk_io_channel *ch1, *ch2;
	void *ctx;

	allocate_threads(1);
	set_thread(0);

	spdk_io_device_register(&g_device1, create_cb_1, destroy_cb_1, sizeof(g_ctx1), NULL);
	spdk_io_device_register(&g_device2, create_cb_2, destroy_cb_2, sizeof(g_ctx2), NULL);

	g_create_cb_calls = 0;
	ch1 = spdk_get_io_channel(&g_device1);
	CU_ASSERT(g_create_cb_calls == 1);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);
	CU_ASSERT(spdk_io_channel_get_io_device(ch1) == &g_device1);

	g_create_cb_calls = 0;
	ch2 = spdk_get_io_channel(&g_device1);
	CU_ASSERT(g_create_cb_calls == 0);
	CU_ASSERT(ch1 == ch2);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);
	CU_ASSERT(spdk_io_channel_get_io_device(ch2) == &g_device1);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch2);
	poll_threads();
	CU_ASSERT(g_destroy_cb_calls == 0);

	g_create_cb_calls = 0;
	ch2 = spdk_get_io_channel(&g_device2);
	CU_ASSERT(g_create_cb_calls == 1);
	CU_ASSERT(ch1 != ch2);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);
	CU_ASSERT(spdk_io_channel_get_io_device(ch2) == &g_device2);

	ctx = spdk_io_channel_get_ctx(ch2);
	CU_ASSERT(*(uint64_t *)ctx == g_ctx2);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch1);
	poll_threads();
	CU_ASSERT(g_destroy_cb_calls == 1);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch2);
	poll_threads();
	CU_ASSERT(g_destroy_cb_calls == 1);

	ch1 = spdk_get_io_channel(&g_device3);
	CU_ASSERT(ch1 == NULL);

	spdk_io_device_unregister(&g_device1, NULL);
	poll_threads();
	spdk_io_device_unregister(&g_device2, NULL);
	poll_threads();
	CU_ASSERT(RB_EMPTY(&g_io_devices));
	free_threads();
	CU_ASSERT(TAILQ_EMPTY(&g_threads));
}

static int
create_cb(void *io_device, void *ctx_buf)
{
	uint64_t *refcnt = (uint64_t *)ctx_buf;

	CU_ASSERT(*refcnt == 0);
	*refcnt = 1;

	return 0;
}

static void
destroy_cb(void *io_device, void *ctx_buf)
{
	uint64_t *refcnt = (uint64_t *)ctx_buf;

	CU_ASSERT(*refcnt == 1);
	*refcnt = 0;
}

/**
 * This test is checking that a sequence of get, put, get, put without allowing
 * the deferred put operation to complete doesn't result in releasing the memory
 * for the channel twice.
 */
static void
channel_destroy_races(void)
{
	uint64_t device;
	struct spdk_io_channel *ch;

	allocate_threads(1);
	set_thread(0);

	spdk_io_device_register(&device, create_cb, destroy_cb, sizeof(uint64_t), NULL);

	ch = spdk_get_io_channel(&device);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel(&device);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	spdk_put_io_channel(ch);
	poll_threads();

	spdk_io_device_unregister(&device, NULL);
	poll_threads();

	CU_ASSERT(RB_EMPTY(&g_io_devices));
	free_threads();
	CU_ASSERT(TAILQ_EMPTY(&g_threads));
}

static void
thread_exit_test(void)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch;
	struct spdk_poller *poller1, *poller2;
	void *ctx;
	bool done1 = false, done2 = false, poller1_run = false, poller2_run = false;
	int rc __attribute__((unused));

	MOCK_SET(spdk_get_ticks, 10);
	MOCK_SET(spdk_get_ticks_hz, 1);

	allocate_threads(4);

	/* Test if all pending messages are reaped for the exiting thread, and the
	 * thread moves to the exited state.
	 */
	set_thread(0);
	thread = spdk_get_thread();

	/* Sending message to thread 0 will be accepted. */
	rc = spdk_thread_send_msg(thread, send_msg_cb, &done1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!done1);

	/* Move thread 0 to the exiting state. */
	spdk_thread_exit(thread);

	CU_ASSERT(spdk_thread_is_exited(thread) == false);

	/* Sending message to thread 0 will be still accepted. */
	rc = spdk_thread_send_msg(thread, send_msg_cb, &done2);
	CU_ASSERT(rc == 0);

	/* Thread 0 will reap pending messages. */
	poll_thread(0);
	CU_ASSERT(done1 == true);
	CU_ASSERT(done2 == true);

	/* Thread 0 will move to the exited state. */
	CU_ASSERT(spdk_thread_is_exited(thread) == true);

	/* Test releasing I/O channel is reaped even after the thread moves to
	 * the exiting state
	 */
	set_thread(1);

	spdk_io_device_register(&g_device1, create_cb_1, destroy_cb_1, sizeof(g_ctx1), NULL);

	g_create_cb_calls = 0;
	ch = spdk_get_io_channel(&g_device1);
	CU_ASSERT(g_create_cb_calls == 1);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ctx = spdk_io_channel_get_ctx(ch);
	CU_ASSERT(*(uint64_t *)ctx == g_ctx1);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch);

	thread = spdk_get_thread();
	spdk_thread_exit(thread);

	/* Thread 1 will not move to the exited state yet because I/O channel release
	 * does not complete yet.
	 */
	CU_ASSERT(spdk_thread_is_exited(thread) == false);

	/* Thread 1 will be able to get the another reference of I/O channel
	 * even after the thread moves to the exiting state.
	 */
	g_create_cb_calls = 0;
	ch = spdk_get_io_channel(&g_device1);

	CU_ASSERT(g_create_cb_calls == 0);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ctx = spdk_io_channel_get_ctx(ch);
	CU_ASSERT(*(uint64_t *)ctx == g_ctx1);

	spdk_put_io_channel(ch);

	poll_threads();
	CU_ASSERT(g_destroy_cb_calls == 1);

	/* Thread 1 will move to the exited state after I/O channel is released.
	 * are released.
	 */
	CU_ASSERT(spdk_thread_is_exited(thread) == true);

	spdk_io_device_unregister(&g_device1, NULL);
	poll_threads();

	/* Test if unregistering poller is reaped for the exiting thread, and the
	 * thread moves to the exited thread.
	 */
	set_thread(2);
	thread = spdk_get_thread();

	poller1 = spdk_poller_register(poller_run_done, &poller1_run, 0);
	CU_ASSERT(poller1 != NULL);

	spdk_poller_unregister(&poller1);

	spdk_thread_exit(thread);

	poller2 = spdk_poller_register(poller_run_done, &poller2_run, 0);

	poll_threads();

	CU_ASSERT(poller1_run == false);
	CU_ASSERT(poller2_run == true);

	CU_ASSERT(spdk_thread_is_exited(thread) == false);

	spdk_poller_unregister(&poller2);

	poll_threads();

	CU_ASSERT(spdk_thread_is_exited(thread) == true);

	/* Test if the exiting thread is exited forcefully after timeout. */
	set_thread(3);
	thread = spdk_get_thread();

	poller1 = spdk_poller_register(poller_run_done, &poller1_run, 0);
	CU_ASSERT(poller1 != NULL);

	spdk_thread_exit(thread);

	CU_ASSERT(spdk_thread_is_exited(thread) == false);

	MOCK_SET(spdk_get_ticks, 11);

	poll_threads();

	CU_ASSERT(spdk_thread_is_exited(thread) == false);

	/* Cause timeout forcefully. */
	MOCK_SET(spdk_get_ticks, 15);

	poll_threads();

	CU_ASSERT(spdk_thread_is_exited(thread) == true);

	spdk_poller_unregister(&poller1);

	poll_threads();

	MOCK_CLEAR(spdk_get_ticks);
	MOCK_CLEAR(spdk_get_ticks_hz);

	free_threads();
}

static int
poller_run_idle(void *ctx)
{
	uint64_t delay_us = (uint64_t)ctx;

	spdk_delay_us(delay_us);

	return 0;
}

static int
poller_run_busy(void *ctx)
{
	uint64_t delay_us = (uint64_t)ctx;

	spdk_delay_us(delay_us);

	return 1;
}

static void
thread_update_stats_test(void)
{
	struct spdk_poller	*poller;
	struct spdk_thread	*thread;

	MOCK_SET(spdk_get_ticks, 10);

	allocate_threads(1);

	set_thread(0);
	thread = spdk_get_thread();

	CU_ASSERT(thread->tsc_last == 10);
	CU_ASSERT(thread->stats.idle_tsc == 0);
	CU_ASSERT(thread->stats.busy_tsc == 0);

	/* Test if idle_tsc is updated expectedly. */
	poller = spdk_poller_register(poller_run_idle, (void *)1000, 0);
	CU_ASSERT(poller != NULL);

	spdk_delay_us(100);

	poll_thread_times(0, 1);

	CU_ASSERT(thread->tsc_last == 1110);
	CU_ASSERT(thread->stats.idle_tsc == 1000);
	CU_ASSERT(thread->stats.busy_tsc == 0);

	spdk_delay_us(100);

	poll_thread_times(0, 1);

	CU_ASSERT(thread->tsc_last == 2210);
	CU_ASSERT(thread->stats.idle_tsc == 2000);
	CU_ASSERT(thread->stats.busy_tsc == 0);

	spdk_poller_unregister(&poller);

	/* Test if busy_tsc is updated expectedly. */
	poller = spdk_poller_register(poller_run_busy, (void *)100000, 0);
	CU_ASSERT(poller != NULL);

	spdk_delay_us(10000);

	poll_thread_times(0, 1);

	CU_ASSERT(thread->tsc_last == 112210);
	CU_ASSERT(thread->stats.idle_tsc == 2000);
	CU_ASSERT(thread->stats.busy_tsc == 100000);

	spdk_delay_us(10000);

	poll_thread_times(0, 1);

	CU_ASSERT(thread->tsc_last == 222210);
	CU_ASSERT(thread->stats.idle_tsc == 2000);
	CU_ASSERT(thread->stats.busy_tsc == 200000);

	spdk_poller_unregister(&poller);

	MOCK_CLEAR(spdk_get_ticks);

	free_threads();
}

struct ut_nested_ch {
	struct spdk_io_channel *child;
	struct spdk_poller *poller;
};

struct ut_nested_dev {
	struct ut_nested_dev *child;
};

static int
ut_null_poll(void *ctx)
{
	return -1;
}

static int
ut_nested_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct ut_nested_ch *_ch = ctx_buf;
	struct ut_nested_dev *_dev = io_device;
	struct ut_nested_dev *_child;

	_child = _dev->child;

	if (_child != NULL) {
		_ch->child = spdk_get_io_channel(_child);
		SPDK_CU_ASSERT_FATAL(_ch->child != NULL);
	} else {
		_ch->child = NULL;
	}

	_ch->poller = spdk_poller_register(ut_null_poll, NULL, 0);
	SPDK_CU_ASSERT_FATAL(_ch->poller != NULL);

	return 0;
}

static void
ut_nested_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ut_nested_ch *_ch = ctx_buf;
	struct spdk_io_channel *child;

	child = _ch->child;
	if (child != NULL) {
		spdk_put_io_channel(child);
	}

	spdk_poller_unregister(&_ch->poller);
}

static void
ut_check_nested_ch_create(struct spdk_io_channel *ch, struct io_device *dev)
{
	CU_ASSERT(ch->ref == 1);
	CU_ASSERT(ch->dev == dev);
	CU_ASSERT(dev->refcnt == 1);
}

static void
ut_check_nested_ch_destroy_pre(struct spdk_io_channel *ch, struct io_device *dev)
{
	CU_ASSERT(ch->ref == 0);
	CU_ASSERT(ch->destroy_ref == 1);
	CU_ASSERT(dev->refcnt == 1);
}

static void
ut_check_nested_ch_destroy_post(struct io_device *dev)
{
	CU_ASSERT(dev->refcnt == 0);
}

static void
ut_check_nested_poller_register(struct spdk_poller *poller)
{
	SPDK_CU_ASSERT_FATAL(poller != NULL);
}

static void
nested_channel(void)
{
	struct ut_nested_dev _dev1, _dev2, _dev3;
	struct ut_nested_ch *_ch1, *_ch2, *_ch3;
	struct io_device *dev1, *dev2, *dev3;
	struct spdk_io_channel *ch1, *ch2, *ch3;
	struct spdk_poller *poller;
	struct spdk_thread *thread;

	allocate_threads(1);
	set_thread(0);

	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);

	_dev1.child = &_dev2;
	_dev2.child = &_dev3;
	_dev3.child = NULL;

	spdk_io_device_register(&_dev1, ut_nested_ch_create_cb, ut_nested_ch_destroy_cb,
				sizeof(struct ut_nested_ch), "dev1");
	spdk_io_device_register(&_dev2, ut_nested_ch_create_cb, ut_nested_ch_destroy_cb,
				sizeof(struct ut_nested_ch), "dev2");
	spdk_io_device_register(&_dev3, ut_nested_ch_create_cb, ut_nested_ch_destroy_cb,
				sizeof(struct ut_nested_ch), "dev3");

	dev1 = io_device_get(&_dev1);
	SPDK_CU_ASSERT_FATAL(dev1 != NULL);
	dev2 = io_device_get(&_dev2);
	SPDK_CU_ASSERT_FATAL(dev2 != NULL);
	dev3 = io_device_get(&_dev3);
	SPDK_CU_ASSERT_FATAL(dev3 != NULL);

	/* A single call spdk_get_io_channel() to dev1 will also create channels
	 * to dev2 and dev3 continuously. Pollers will be registered together.
	 */
	ch1 = spdk_get_io_channel(&_dev1);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	_ch1 = spdk_io_channel_get_ctx(ch1);
	ch2 = _ch1->child;
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	_ch2 = spdk_io_channel_get_ctx(ch2);
	ch3 = _ch2->child;
	SPDK_CU_ASSERT_FATAL(ch3 != NULL);

	_ch3 = spdk_io_channel_get_ctx(ch3);
	CU_ASSERT(_ch3->child == NULL);

	ut_check_nested_ch_create(ch1, dev1);
	ut_check_nested_ch_create(ch2, dev2);
	ut_check_nested_ch_create(ch3, dev3);

	poller = spdk_poller_register(ut_null_poll, NULL, 0);

	ut_check_nested_poller_register(poller);
	ut_check_nested_poller_register(_ch1->poller);
	ut_check_nested_poller_register(_ch2->poller);
	ut_check_nested_poller_register(_ch3->poller);

	spdk_poller_unregister(&poller);
	poll_thread_times(0, 1);

	/* A single call spdk_put_io_channel() to dev1 will also destroy channels
	 * to dev2 and dev3 continuously. Pollers will be unregistered together.
	 */
	spdk_put_io_channel(ch1);

	/* Start exiting the current thread after unregistering the non-nested
	 * I/O channel.
	 */
	spdk_thread_exit(thread);

	ut_check_nested_ch_destroy_pre(ch1, dev1);
	poll_thread_times(0, 1);
	ut_check_nested_ch_destroy_post(dev1);

	CU_ASSERT(spdk_thread_is_exited(thread) == false);

	ut_check_nested_ch_destroy_pre(ch2, dev2);
	poll_thread_times(0, 1);
	ut_check_nested_ch_destroy_post(dev2);

	CU_ASSERT(spdk_thread_is_exited(thread) == false);

	ut_check_nested_ch_destroy_pre(ch3, dev3);
	poll_thread_times(0, 1);
	ut_check_nested_ch_destroy_post(dev3);

	CU_ASSERT(spdk_thread_is_exited(thread) == true);

	spdk_io_device_unregister(&_dev1, NULL);
	spdk_io_device_unregister(&_dev2, NULL);
	spdk_io_device_unregister(&_dev3, NULL);
	CU_ASSERT(RB_EMPTY(&g_io_devices));

	free_threads();
	CU_ASSERT(TAILQ_EMPTY(&g_threads));
}

static int
create_cb2(void *io_device, void *ctx_buf)
{
	uint64_t *devcnt = (uint64_t *)io_device;

	*devcnt += 1;

	return 0;
}

static void
destroy_cb2(void *io_device, void *ctx_buf)
{
	uint64_t *devcnt = (uint64_t *)io_device;

	CU_ASSERT(*devcnt > 0);
	*devcnt -= 1;
}

static void
unregister_cb2(void *io_device)
{
	uint64_t *devcnt = (uint64_t *)io_device;

	CU_ASSERT(*devcnt == 0);
}

static void
device_unregister_and_thread_exit_race(void)
{
	uint64_t device = 0;
	struct spdk_io_channel *ch1, *ch2;
	struct spdk_thread *thread1, *thread2;

	/* Create two threads and each thread gets a channel from the same device. */
	allocate_threads(2);
	set_thread(0);

	thread1 = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread1 != NULL);

	spdk_io_device_register(&device, create_cb2, destroy_cb2, sizeof(uint64_t), NULL);

	ch1 = spdk_get_io_channel(&device);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	set_thread(1);

	thread2 = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread2 != NULL);

	ch2 = spdk_get_io_channel(&device);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	set_thread(0);

	/* Move thread 0 to the exiting state, but it should keep exiting until two channels
	 * and a device are released.
	 */
	spdk_thread_exit(thread1);
	poll_thread(0);

	spdk_put_io_channel(ch1);

	spdk_io_device_unregister(&device, unregister_cb2);
	poll_thread(0);

	CU_ASSERT(spdk_thread_is_exited(thread1) == false);

	set_thread(1);

	/* Move thread 1 to the exiting state, but it should keep exiting until its channel
	 * is released.
	 */
	spdk_thread_exit(thread2);
	poll_thread(1);

	CU_ASSERT(spdk_thread_is_exited(thread2) == false);

	spdk_put_io_channel(ch2);
	poll_thread(1);

	CU_ASSERT(spdk_thread_is_exited(thread1) == false);
	CU_ASSERT(spdk_thread_is_exited(thread2) == true);

	poll_thread(0);

	CU_ASSERT(spdk_thread_is_exited(thread1) == true);

	free_threads();
}

static int
dummy_poller(void *arg)
{
	return SPDK_POLLER_IDLE;
}

static void
cache_closest_timed_poller(void)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller1, *poller2, *poller3, *tmp;

	allocate_threads(1);
	set_thread(0);

	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);

	poller1 = spdk_poller_register(dummy_poller, NULL, 1000);
	SPDK_CU_ASSERT_FATAL(poller1 != NULL);

	poller2 = spdk_poller_register(dummy_poller, NULL, 1500);
	SPDK_CU_ASSERT_FATAL(poller2 != NULL);

	poller3 = spdk_poller_register(dummy_poller, NULL, 1800);
	SPDK_CU_ASSERT_FATAL(poller3 != NULL);

	poll_threads();

	/* When multiple timed pollers are inserted, the cache should
	 * have the closest timed poller.
	 */
	CU_ASSERT(thread->first_timed_poller == poller1);
	CU_ASSERT(RB_MIN(timed_pollers_tree, &thread->timed_pollers) == poller1);

	spdk_delay_us(1000);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == poller2);
	CU_ASSERT(RB_MIN(timed_pollers_tree, &thread->timed_pollers) == poller2);

	/* If we unregister a timed poller by spdk_poller_unregister()
	 * when it is waiting, it is marked as being unregistered and
	 * is actually unregistered when it is expired.
	 *
	 * Hence if we unregister the closest timed poller when it is waiting,
	 * the cache is not updated to the next timed poller until it is expired.
	 */
	tmp = poller2;

	spdk_poller_unregister(&poller2);
	CU_ASSERT(poller2 == NULL);

	spdk_delay_us(499);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == tmp);
	CU_ASSERT(RB_MIN(timed_pollers_tree, &thread->timed_pollers) == tmp);

	spdk_delay_us(1);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == poller3);
	CU_ASSERT(RB_MIN(timed_pollers_tree, &thread->timed_pollers) == poller3);

	/* If we pause a timed poller by spdk_poller_pause() when it is waiting,
	 * it is marked as being paused and is actually paused when it is expired.
	 *
	 * Hence if we pause the closest timed poller when it is waiting, the cache
	 * is not updated to the next timed poller until it is expired.
	 */
	spdk_poller_pause(poller3);

	spdk_delay_us(299);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == poller3);
	CU_ASSERT(RB_MIN(timed_pollers_tree, &thread->timed_pollers) == poller3);

	spdk_delay_us(1);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == poller1);
	CU_ASSERT(RB_MIN(timed_pollers_tree, &thread->timed_pollers) == poller1);

	/* After unregistering all timed pollers, the cache should
	 * be NULL.
	 */
	spdk_poller_unregister(&poller1);
	spdk_poller_unregister(&poller3);

	spdk_delay_us(200);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == NULL);
	CU_ASSERT(RB_EMPTY(&thread->timed_pollers));

	free_threads();
}

static void
multi_timed_pollers_have_same_expiration(void)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller1, *poller2, *poller3, *poller4, *tmp;
	uint64_t start_ticks;

	allocate_threads(1);
	set_thread(0);

	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);

	/*
	 * case 1: multiple timed pollers have the same next_run_tick.
	 */
	start_ticks = spdk_get_ticks();

	poller1 = spdk_poller_register(dummy_poller, NULL, 500);
	SPDK_CU_ASSERT_FATAL(poller1 != NULL);

	poller2 = spdk_poller_register(dummy_poller, NULL, 500);
	SPDK_CU_ASSERT_FATAL(poller2 != NULL);

	poller3 = spdk_poller_register(dummy_poller, NULL, 1000);
	SPDK_CU_ASSERT_FATAL(poller3 != NULL);

	poller4 = spdk_poller_register(dummy_poller, NULL, 1500);
	SPDK_CU_ASSERT_FATAL(poller4 != NULL);

	/* poller1 and poller2 have the same next_run_tick but cache has poller1
	 * because poller1 is registered earlier than poller2.
	 */
	CU_ASSERT(thread->first_timed_poller == poller1);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 500);
	CU_ASSERT(poller2->next_run_tick == start_ticks + 500);
	CU_ASSERT(poller3->next_run_tick == start_ticks + 1000);
	CU_ASSERT(poller4->next_run_tick == start_ticks + 1500);

	/* after 500 usec, poller1 and poller2 are expired. */
	spdk_delay_us(500);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 500);
	poll_threads();

	/* poller1, poller2, and poller3 have the same next_run_tick but cache
	 * has poller3 because poller3 is not expired yet.
	 */
	CU_ASSERT(thread->first_timed_poller == poller3);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 1000);
	CU_ASSERT(poller2->next_run_tick == start_ticks + 1000);
	CU_ASSERT(poller3->next_run_tick == start_ticks + 1000);
	CU_ASSERT(poller4->next_run_tick == start_ticks + 1500);

	/* after 500 usec, poller1, poller2, and poller3 are expired. */
	spdk_delay_us(500);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 1000);
	poll_threads();

	/* poller1, poller2, and poller4 have the same next_run_tick but cache
	 * has poller4 because poller4 is not expired yet.
	 */
	CU_ASSERT(thread->first_timed_poller == poller4);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 1500);
	CU_ASSERT(poller2->next_run_tick == start_ticks + 1500);
	CU_ASSERT(poller3->next_run_tick == start_ticks + 2000);
	CU_ASSERT(poller4->next_run_tick == start_ticks + 1500);

	/* after 500 usec, poller1, poller2, and poller4 are expired. */
	spdk_delay_us(500);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 1500);
	poll_threads();

	/* poller1, poller2, and poller3 have the same next_run_tick but cache
	 * has poller3 because poller3 is updated earlier than poller1 and poller2.
	 */
	CU_ASSERT(thread->first_timed_poller == poller3);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 2000);
	CU_ASSERT(poller2->next_run_tick == start_ticks + 2000);
	CU_ASSERT(poller3->next_run_tick == start_ticks + 2000);
	CU_ASSERT(poller4->next_run_tick == start_ticks + 3000);

	spdk_poller_unregister(&poller1);
	spdk_poller_unregister(&poller2);
	spdk_poller_unregister(&poller3);
	spdk_poller_unregister(&poller4);

	spdk_delay_us(1500);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 3000);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == NULL);
	CU_ASSERT(RB_EMPTY(&thread->timed_pollers));

	/*
	 * case 2: unregister timed pollers while multiple timed pollers are registered.
	 */
	start_ticks = spdk_get_ticks();

	poller1 = spdk_poller_register(dummy_poller, NULL, 500);
	SPDK_CU_ASSERT_FATAL(poller1 != NULL);

	CU_ASSERT(thread->first_timed_poller == poller1);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 500);

	/* after 250 usec, register poller2 and poller3. */
	spdk_delay_us(250);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 250);

	poller2 = spdk_poller_register(dummy_poller, NULL, 500);
	SPDK_CU_ASSERT_FATAL(poller2 != NULL);

	poller3 = spdk_poller_register(dummy_poller, NULL, 750);
	SPDK_CU_ASSERT_FATAL(poller3 != NULL);

	CU_ASSERT(thread->first_timed_poller == poller1);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 500);
	CU_ASSERT(poller2->next_run_tick == start_ticks + 750);
	CU_ASSERT(poller3->next_run_tick == start_ticks + 1000);

	/* unregister poller2 which is not the closest. */
	tmp = poller2;
	spdk_poller_unregister(&poller2);

	/* after 250 usec, poller1 is expired. */
	spdk_delay_us(250);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 500);
	poll_threads();

	/* poller2 is not unregistered yet because it is not expired. */
	CU_ASSERT(thread->first_timed_poller == tmp);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 1000);
	CU_ASSERT(tmp->next_run_tick == start_ticks + 750);
	CU_ASSERT(poller3->next_run_tick == start_ticks + 1000);

	spdk_delay_us(250);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 750);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == poller3);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 1000);
	CU_ASSERT(poller3->next_run_tick == start_ticks + 1000);

	spdk_poller_unregister(&poller3);

	spdk_delay_us(250);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 1000);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == poller1);
	CU_ASSERT(poller1->next_run_tick == start_ticks + 1500);

	spdk_poller_unregister(&poller1);

	spdk_delay_us(500);
	CU_ASSERT(spdk_get_ticks() == start_ticks + 1500);
	poll_threads();

	CU_ASSERT(thread->first_timed_poller == NULL);
	CU_ASSERT(RB_EMPTY(&thread->timed_pollers));

	free_threads();
}

static int
dummy_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
dummy_destroy_cb(void *io_device, void *ctx_buf)
{
}

/* We had a bug that the compare function for the io_device tree
 * did not work as expected because subtraction caused overflow
 * when the difference between two keys was more than 32 bits.
 * This test case verifies the fix for the bug.
 */
static void
io_device_lookup(void)
{
	struct io_device dev1, dev2, *dev;
	struct spdk_io_channel *ch;

	/* The compare function io_device_cmp() had a overflow bug.
	 * Verify the fix first.
	 */
	dev1.io_device = (void *)0x7FFFFFFF;
	dev2.io_device = NULL;
	CU_ASSERT(io_device_cmp(&dev1, &dev2) > 0);
	CU_ASSERT(io_device_cmp(&dev2, &dev1) < 0);

	/* Check if overflow due to 32 bits does not occur. */
	dev1.io_device = (void *)0x80000000;
	CU_ASSERT(io_device_cmp(&dev1, &dev2) > 0);
	CU_ASSERT(io_device_cmp(&dev2, &dev1) < 0);

	dev1.io_device = (void *)0x100000000;
	CU_ASSERT(io_device_cmp(&dev1, &dev2) > 0);
	CU_ASSERT(io_device_cmp(&dev2, &dev1) < 0);

	dev1.io_device = (void *)0x8000000000000000;
	CU_ASSERT(io_device_cmp(&dev1, &dev2) > 0);
	CU_ASSERT(io_device_cmp(&dev2, &dev1) < 0);

	allocate_threads(1);
	set_thread(0);

	spdk_io_device_register((void *)0x1, dummy_create_cb, dummy_destroy_cb, 0, NULL);
	spdk_io_device_register((void *)0x7FFFFFFF, dummy_create_cb, dummy_destroy_cb, 0, NULL);
	spdk_io_device_register((void *)0x80000000, dummy_create_cb, dummy_destroy_cb, 0, NULL);
	spdk_io_device_register((void *)0x100000000, dummy_create_cb, dummy_destroy_cb, 0, NULL);
	spdk_io_device_register((void *)0x8000000000000000, dummy_create_cb, dummy_destroy_cb, 0, NULL);
	spdk_io_device_register((void *)0x8000000100000000, dummy_create_cb, dummy_destroy_cb, 0, NULL);
	spdk_io_device_register((void *)UINT64_MAX, dummy_create_cb, dummy_destroy_cb, 0, NULL);

	/* RB_MIN and RB_NEXT should return devs in ascending order by addresses.
	 * RB_FOREACH uses RB_MIN and RB_NEXT internally.
	 */
	dev = RB_MIN(io_device_tree, &g_io_devices);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(dev->io_device == (void *)0x1);

	dev = RB_NEXT(io_device_tree, &g_io_devices, dev);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(dev->io_device == (void *)0x7FFFFFFF);

	dev = RB_NEXT(io_device_tree, &g_io_devices, dev);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(dev->io_device == (void *)0x80000000);

	dev = RB_NEXT(io_device_tree, &g_io_devices, dev);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(dev->io_device == (void *)0x100000000);

	dev = RB_NEXT(io_device_tree, &g_io_devices, dev);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(dev->io_device == (void *)0x8000000000000000);

	dev = RB_NEXT(io_device_tree, &g_io_devices, dev);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(dev->io_device == (void *)0x8000000100000000);

	dev = RB_NEXT(io_device_tree, &g_io_devices, dev);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(dev->io_device == (void *)UINT64_MAX);

	/* Verify spdk_get_io_channel() creates io_channels associated with the
	 * correct io_devices.
	 */
	ch = spdk_get_io_channel((void *)0x1);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	CU_ASSERT(ch->dev->io_device == (void *)0x1);
	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel((void *)0x7FFFFFFF);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	CU_ASSERT(ch->dev->io_device == (void *)0x7FFFFFFF);
	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel((void *)0x80000000);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	CU_ASSERT(ch->dev->io_device == (void *)0x80000000);
	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel((void *)0x100000000);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	CU_ASSERT(ch->dev->io_device == (void *)0x100000000);
	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel((void *)0x8000000000000000);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	CU_ASSERT(ch->dev->io_device == (void *)0x8000000000000000);
	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel((void *)0x8000000100000000);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	CU_ASSERT(ch->dev->io_device == (void *)0x8000000100000000);
	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel((void *)UINT64_MAX);
	SPDK_CU_ASSERT_FATAL(ch != NULL);
	CU_ASSERT(ch->dev->io_device == (void *)UINT64_MAX);
	spdk_put_io_channel(ch);

	poll_threads();

	spdk_io_device_unregister((void *)0x1, NULL);
	spdk_io_device_unregister((void *)0x7FFFFFFF, NULL);
	spdk_io_device_unregister((void *)0x80000000, NULL);
	spdk_io_device_unregister((void *)0x100000000, NULL);
	spdk_io_device_unregister((void *)0x8000000000000000, NULL);
	spdk_io_device_unregister((void *)0x8000000100000000, NULL);
	spdk_io_device_unregister((void *)UINT64_MAX, NULL);

	poll_threads();

	CU_ASSERT(RB_EMPTY(&g_io_devices));

	free_threads();
}

static enum spin_error g_spin_err;
static uint32_t g_spin_err_count = 0;

static void
ut_track_abort(enum spin_error err)
{
	g_spin_err = err;
	g_spin_err_count++;
}

static void
spdk_spin(void)
{
	struct spdk_spinlock lock;

	g_spin_abort_fn = ut_track_abort;

	/* Do not need to be on an SPDK thread to initialize an spdk_spinlock */
	g_spin_err_count = 0;
	spdk_spin_init(&lock);
	CU_ASSERT(g_spin_err_count == 0);

	/* Trying to take a lock while not on an SPDK thread is an error */
	g_spin_err_count = 0;
	spdk_spin_lock(&lock);
	CU_ASSERT(g_spin_err_count == 1);
	CU_ASSERT(g_spin_err == SPIN_ERR_NOT_SPDK_THREAD);

	/* Trying to check if a lock is held while not on an SPDK thread is an error */
	g_spin_err_count = 0;
	spdk_spin_held(&lock);
	CU_ASSERT(g_spin_err_count == 1);
	CU_ASSERT(g_spin_err == SPIN_ERR_NOT_SPDK_THREAD);

	/* Do not need to be on an SPDK thread to destroy an spdk_spinlock */
	g_spin_err_count = 0;
	spdk_spin_destroy(&lock);
	CU_ASSERT(g_spin_err_count == 0);

	allocate_threads(2);
	set_thread(0);

	/* Can initialize an spdk_spinlock on an SPDK thread */
	g_spin_err_count = 0;
	spdk_spin_init(&lock);
	CU_ASSERT(g_spin_err_count == 0);

	/* Can take spinlock */
	g_spin_err_count = 0;
	spdk_spin_lock(&lock);
	CU_ASSERT(g_spin_err_count == 0);

	/* Can release spinlock */
	g_spin_err_count = 0;
	spdk_spin_unlock(&lock);
	CU_ASSERT(g_spin_err_count == 0);

	/* Deadlock detected */
	g_spin_err_count = 0;
	g_spin_err = SPIN_ERR_NONE;
	spdk_spin_lock(&lock);
	CU_ASSERT(g_spin_err_count == 0);
	spdk_spin_lock(&lock);
	CU_ASSERT(g_spin_err_count == 1);
	CU_ASSERT(g_spin_err == SPIN_ERR_DEADLOCK);

	/* Cannot unlock from wrong thread */
	set_thread(1);
	g_spin_err_count = 0;
	spdk_spin_unlock(&lock);
	CU_ASSERT(g_spin_err_count == 1);
	CU_ASSERT(g_spin_err == SPIN_ERR_WRONG_THREAD);

	/* Get back to a known good state */
	set_thread(0);
	g_spin_err_count = 0;
	spdk_spin_unlock(&lock);
	CU_ASSERT(g_spin_err_count == 0);

	/* Cannot release the same lock twice */
	g_spin_err_count = 0;
	spdk_spin_lock(&lock);
	CU_ASSERT(g_spin_err_count == 0);
	spdk_spin_unlock(&lock);
	CU_ASSERT(g_spin_err_count == 0);
	spdk_spin_unlock(&lock);
	CU_ASSERT(g_spin_err_count == 1);
	CU_ASSERT(g_spin_err == SPIN_ERR_WRONG_THREAD);

	/* A lock that is not held is properly recognized */
	g_spin_err_count = 0;
	CU_ASSERT(!spdk_spin_held(&lock));
	CU_ASSERT(g_spin_err_count == 0);

	/* A lock that is held is recognized as held by only the thread that holds it. */
	set_thread(1);
	g_spin_err_count = 0;
	spdk_spin_lock(&lock);
	CU_ASSERT(g_spin_err_count == 0);
	CU_ASSERT(spdk_spin_held(&lock));
	CU_ASSERT(g_spin_err_count == 0);
	set_thread(0);
	CU_ASSERT(!spdk_spin_held(&lock));
	CU_ASSERT(g_spin_err_count == 0);

	/* After releasing, no one thinks it is held */
	set_thread(1);
	spdk_spin_unlock(&lock);
	CU_ASSERT(g_spin_err_count == 0);
	CU_ASSERT(!spdk_spin_held(&lock));
	CU_ASSERT(g_spin_err_count == 0);
	set_thread(0);
	CU_ASSERT(!spdk_spin_held(&lock));
	CU_ASSERT(g_spin_err_count == 0);

	/* Destroying a lock that is held is an error. */
	set_thread(0);
	g_spin_err_count = 0;
	spdk_spin_lock(&lock);
	CU_ASSERT(g_spin_err_count == 0);
	spdk_spin_destroy(&lock);
	CU_ASSERT(g_spin_err_count == 1);
	CU_ASSERT(g_spin_err == SPIN_ERR_LOCK_HELD);
	g_spin_err_count = 0;
	spdk_spin_unlock(&lock);
	CU_ASSERT(g_spin_err_count == 0);

	/* Clean up */
	g_spin_err_count = 0;
	spdk_spin_destroy(&lock);
	CU_ASSERT(g_spin_err_count == 0);
	free_threads();
	g_spin_abort_fn = __posix_abort;
}

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

	CU_ADD_TEST(suite, thread_alloc);
	CU_ADD_TEST(suite, thread_send_msg);
	CU_ADD_TEST(suite, thread_poller);
	CU_ADD_TEST(suite, poller_pause);
	CU_ADD_TEST(suite, thread_for_each);
	CU_ADD_TEST(suite, for_each_channel_remove);
	CU_ADD_TEST(suite, for_each_channel_unreg);
	CU_ADD_TEST(suite, thread_name);
	CU_ADD_TEST(suite, channel);
	CU_ADD_TEST(suite, channel_destroy_races);
	CU_ADD_TEST(suite, thread_exit_test);
	CU_ADD_TEST(suite, thread_update_stats_test);
	CU_ADD_TEST(suite, nested_channel);
	CU_ADD_TEST(suite, device_unregister_and_thread_exit_race);
	CU_ADD_TEST(suite, cache_closest_timed_poller);
	CU_ADD_TEST(suite, multi_timed_pollers_have_same_expiration);
	CU_ADD_TEST(suite, io_device_lookup);
	CU_ADD_TEST(suite, spdk_spin);
	CU_ADD_TEST(suite, iobuf);
	CU_ADD_TEST(suite, iobuf_cache);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
