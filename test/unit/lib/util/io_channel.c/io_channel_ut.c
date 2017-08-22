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

#include "util/io_channel.c"

static void
_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

static void
thread_alloc(void)
{
	struct spdk_thread *thread;
	const char *name;

	/* Create thread with no name */
	spdk_allocate_thread(_send_msg, NULL, NULL);
	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	name = spdk_thread_get_name(thread);
	CU_ASSERT(name == NULL);
	spdk_free_thread();

	/* Create thread named "test_thread" */
	spdk_allocate_thread(_send_msg, NULL, "test_thread");
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

static int
create_cb_null(void *io_device, void *ctx_buf)
{
	return -1;
}

static void
channel(void)
{
	struct spdk_io_channel *ch1, *ch2;
	void *ctx;

	spdk_allocate_thread(_send_msg, NULL, "thread0");
	spdk_io_device_register(&device1, create_cb_1, destroy_cb_1, sizeof(ctx1));
	spdk_io_device_register(&device2, create_cb_2, destroy_cb_2, sizeof(ctx2));
	spdk_io_device_register(&device3, create_cb_null, NULL, 0);

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
	CU_ASSERT(g_destroy_cb_calls == 1);

	g_destroy_cb_calls = 0;
	spdk_put_io_channel(ch2);
	CU_ASSERT(g_destroy_cb_calls == 1);

	ch1 = spdk_get_io_channel(&device3);
	CU_ASSERT(ch1 == NULL);

	spdk_io_device_unregister(&device1, NULL);
	spdk_io_device_unregister(&device2, NULL);
	spdk_io_device_unregister(&device3, NULL);
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
		CU_add_test(suite, "channel", channel) == NULL
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
