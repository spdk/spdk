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

#include "spdk_cunit.h"

#include "spdk_internal/thread.h"

#include "thread/thread.c"
#include "common/lib/ut_multithread.c"

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
	spdk_thread_lib_init_ext(_thread_op, _thread_op_supported, 0);

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
	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));
	spdk_io_device_register(&io_target, channel_create, channel_destroy, sizeof(int), NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_io_devices));
	dev = TAILQ_FIRST(&g_io_devices);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(TAILQ_NEXT(dev, tailq) == NULL);
	ch0 = spdk_get_io_channel(&io_target);
	spdk_for_each_channel(&io_target, unreg_ch_done, &ctx, unreg_foreach_done);

	spdk_io_device_unregister(&io_target, NULL);
	/*
	 * There is an outstanding foreach call on the io_device, so the unregister should not
	 *  have removed the device.
	 */
	CU_ASSERT(dev == TAILQ_FIRST(&g_io_devices));
	spdk_io_device_register(&io_target, channel_create, channel_destroy, sizeof(int), NULL);
	/*
	 * There is already a device registered at &io_target, so a new io_device should not
	 *  have been added to g_io_devices.
	 */
	CU_ASSERT(dev == TAILQ_FIRST(&g_io_devices));
	CU_ASSERT(TAILQ_NEXT(dev, tailq) == NULL);

	poll_thread(0);
	CU_ASSERT(ctx.ch_done == true);
	CU_ASSERT(ctx.foreach_done == true);
	/*
	 * There are no more foreach operations outstanding, so we can unregister the device,
	 *  even though a channel still exists for the device.
	 */
	spdk_io_device_unregister(&io_target, NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));

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

	g_create_cb_calls = 0;
	ch2 = spdk_get_io_channel(&g_device1);
	CU_ASSERT(g_create_cb_calls == 0);
	CU_ASSERT(ch1 == ch2);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch2);
	poll_threads();
	CU_ASSERT(g_destroy_cb_calls == 0);

	g_create_cb_calls = 0;
	ch2 = spdk_get_io_channel(&g_device2);
	CU_ASSERT(g_create_cb_calls == 1);
	CU_ASSERT(ch1 != ch2);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

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
	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));
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

	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));
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

static struct io_device *
ut_get_io_device(void *dev)
{
	struct io_device *tmp;

	TAILQ_FOREACH(tmp, &g_io_devices, tailq) {
		if (tmp->io_device == dev) {
			return tmp;
		}
	}

	return NULL;
}

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

	dev1 = ut_get_io_device(&_dev1);
	SPDK_CU_ASSERT_FATAL(dev1 != NULL);
	dev2 = ut_get_io_device(&_dev2);
	SPDK_CU_ASSERT_FATAL(dev2 != NULL);
	dev3 = ut_get_io_device(&_dev3);
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
	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));

	free_threads();
	CU_ASSERT(TAILQ_EMPTY(&g_threads));
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

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
