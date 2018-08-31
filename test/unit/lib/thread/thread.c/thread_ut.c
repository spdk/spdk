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

#include "thread/thread.c"
#include "common/lib/ut_multithread.c"

static void
thread_alloc(void)
{
	CU_ASSERT(TAILQ_EMPTY(&g_threads));
	allocate_threads(1);
	CU_ASSERT(!TAILQ_EMPTY(&g_threads));
	free_threads();
	CU_ASSERT(TAILQ_EMPTY(&g_threads));
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
	return 0;
}

static void
channel_destroy(void *io_device, void *ctx_buf)
{
}

static void
channel_msg(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	int *count = spdk_io_channel_get_ctx(ch);

	(*count)++;

	spdk_for_each_channel_continue(i, 0);
}

static void
channel_cpl(struct spdk_io_channel_iter *i, int status)
{
}

static void
for_each_channel_remove(void)
{
	struct spdk_io_channel *ch0, *ch1, *ch2;
	int io_target;
	int count = 0;

	allocate_threads(3);
	spdk_io_device_register(&io_target, channel_create, channel_destroy, sizeof(int), NULL);
	set_thread(0);
	ch0 = spdk_get_io_channel(&io_target);
	set_thread(1);
	ch1 = spdk_get_io_channel(&io_target);
	set_thread(2);
	ch2 = spdk_get_io_channel(&io_target);

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
	poll_threads();
	spdk_for_each_channel(&io_target, channel_msg, &count, channel_cpl);
	poll_threads();

	/*
	 * Case #2: Put the I/O channel after spdk_for_each_channel, but before
	 *  thread 0 is polled.
	 */
	ch0 = spdk_get_io_channel(&io_target);
	spdk_for_each_channel(&io_target, channel_msg, &count, channel_cpl);
	spdk_put_io_channel(ch0);
	poll_threads();

	set_thread(1);
	spdk_put_io_channel(ch1);
	set_thread(2);
	spdk_put_io_channel(ch2);
	spdk_io_device_unregister(&io_target, NULL);
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
	int io_target;

	allocate_threads(1);
	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));
	spdk_io_device_register(&io_target, channel_create, channel_destroy, sizeof(int), NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_io_devices));
	dev = TAILQ_FIRST(&g_io_devices);
	SPDK_CU_ASSERT_FATAL(dev != NULL);
	CU_ASSERT(TAILQ_NEXT(dev, tailq) == NULL);
	set_thread(0);
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

	/* Create thread with no name, which automatically generates one */
	spdk_allocate_thread(NULL, NULL, NULL, NULL, NULL);
	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	name = spdk_thread_get_name(thread);
	CU_ASSERT(name != NULL);
	spdk_free_thread();

	/* Create thread named "test_thread" */
	spdk_allocate_thread(NULL, NULL, NULL, NULL, "test_thread");
	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	name = spdk_thread_get_name(thread);
	SPDK_CU_ASSERT_FATAL(name != NULL);
	CU_ASSERT(strcmp(name, "test_thread") == 0);
	spdk_free_thread();
}

static uint64_t device1;
static uint64_t device2;
static uint64_t device3;

static uint64_t ctx1 = 0x1111;
static uint64_t ctx2 = 0x2222;

static int g_create_cb_calls = 0;
static int g_destroy_cb_calls = 0;

static int
create_cb_1(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &device1);
	*(uint64_t *)ctx_buf = ctx1;
	g_create_cb_calls++;
	return 0;
}

static void
destroy_cb_1(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &device1);
	CU_ASSERT(*(uint64_t *)ctx_buf == ctx1);
	g_destroy_cb_calls++;
}

static int
create_cb_2(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &device2);
	*(uint64_t *)ctx_buf = ctx2;
	g_create_cb_calls++;
	return 0;
}

static void
destroy_cb_2(void *io_device, void *ctx_buf)
{
	CU_ASSERT(io_device == &device2);
	CU_ASSERT(*(uint64_t *)ctx_buf == ctx2);
	g_destroy_cb_calls++;
}

static void
channel(void)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch1, *ch2;
	void *ctx;

	thread = spdk_allocate_thread(NULL, NULL, NULL, NULL, "thread0");
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_io_device_register(&device1, create_cb_1, destroy_cb_1, sizeof(ctx1), NULL);
	spdk_io_device_register(&device2, create_cb_2, destroy_cb_2, sizeof(ctx2), NULL);

	g_create_cb_calls = 0;
	ch1 = spdk_get_io_channel(&device1);
	CU_ASSERT(g_create_cb_calls == 1);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);

	g_create_cb_calls = 0;
	ch2 = spdk_get_io_channel(&device1);
	CU_ASSERT(g_create_cb_calls == 0);
	CU_ASSERT(ch1 == ch2);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch2);
	while (spdk_thread_poll(thread, 0) > 0) {}
	CU_ASSERT(g_destroy_cb_calls == 0);

	g_create_cb_calls = 0;
	ch2 = spdk_get_io_channel(&device2);
	CU_ASSERT(g_create_cb_calls == 1);
	CU_ASSERT(ch1 != ch2);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);

	ctx = spdk_io_channel_get_ctx(ch2);
	CU_ASSERT(*(uint64_t *)ctx == ctx2);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch1);
	while (spdk_thread_poll(thread, 0) > 0) {}
	CU_ASSERT(g_destroy_cb_calls == 1);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch2);
	while (spdk_thread_poll(thread, 0) > 0) {}
	CU_ASSERT(g_destroy_cb_calls == 1);

	ch1 = spdk_get_io_channel(&device3);
	CU_ASSERT(ch1 == NULL);

	spdk_io_device_unregister(&device1, NULL);
	while (spdk_thread_poll(thread, 0) > 0) {}
	spdk_io_device_unregister(&device2, NULL);
	while (spdk_thread_poll(thread, 0) > 0) {}
	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));
	spdk_free_thread();
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
 * This test is checking for two critical behaviors. The first is that a sequence
 * of get, put, get, put without allowing the deferred put operation to complete
 * doesn't result in releasing the memory for the channel twice. The second is
 * that this same sequence results in 1 channel creation callback followed by
 * a channel destruction, followed by a channel creation, followed by a channel
 * destruction, in specifically that order.
 */
static void
channel_destroy_races(void)
{
	struct spdk_thread *thread;
	uint64_t device;
	struct spdk_io_channel *ch;

	thread = spdk_allocate_thread(NULL, NULL, NULL, NULL, "thread0");
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_io_device_register(&device, create_cb, destroy_cb, sizeof(uint64_t), NULL);

	ch = spdk_get_io_channel(&device);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	spdk_put_io_channel(ch);

	ch = spdk_get_io_channel(&device);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	spdk_put_io_channel(ch);
	while (spdk_thread_poll(thread, 0) > 0) {}

	spdk_io_device_unregister(&device, NULL);
	while (spdk_thread_poll(thread, 0) > 0) {}

	CU_ASSERT(TAILQ_EMPTY(&g_io_devices));
	spdk_free_thread();
	CU_ASSERT(TAILQ_EMPTY(&g_threads));
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("io_channel", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "thread_alloc", thread_alloc) == NULL ||
		CU_add_test(suite, "thread_send_msg", thread_send_msg) == NULL ||
		CU_add_test(suite, "thread_poller", thread_poller) == NULL ||
		CU_add_test(suite, "thread_for_each", thread_for_each) == NULL ||
		CU_add_test(suite, "for_each_channel_remove", for_each_channel_remove) == NULL ||
		CU_add_test(suite, "for_each_channel_unreg", for_each_channel_unreg) == NULL ||
		CU_add_test(suite, "thread_name", thread_name) == NULL ||
		CU_add_test(suite, "channel", channel) == NULL ||
		CU_add_test(suite, "channel_destroy_races", channel_destroy_races) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
