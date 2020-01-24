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
#include "common/lib/test_env.c"
#include "event/reactor.c"

static void
test_create_reactor(void)
{
	struct spdk_reactor reactor = {};

	g_reactors = &reactor;

	spdk_reactor_construct(&reactor, 0);

	CU_ASSERT(spdk_reactor_get(0) == &reactor);

	spdk_ring_free(reactor.events);
	g_reactors = NULL;
}

static void
test_init_reactors(void)
{
	uint32_t core;

	allocate_cores(3);

	CU_ASSERT(spdk_reactors_init() == 0);

	CU_ASSERT(g_reactor_state == SPDK_REACTOR_STATE_INITIALIZED);
	for (core = 0; core < 3; core++) {
		CU_ASSERT(spdk_reactor_get(core) != NULL);
	}

	spdk_reactors_fini();

	free_cores();
}

static void
ut_event_fn(void *arg1, void *arg2)
{
	uint8_t *test1 = arg1;
	uint8_t *test2 = arg2;

	*test1 = 1;
	*test2 = 0xFF;
}

static void
test_event_call(void)
{
	uint8_t test1 = 0, test2 = 0;
	struct spdk_event *evt;
	struct spdk_reactor *reactor;

	allocate_cores(1);

	CU_ASSERT(spdk_reactors_init() == 0);

	evt = spdk_event_allocate(0, ut_event_fn, &test1, &test2);
	CU_ASSERT(evt != NULL);

	spdk_event_call(evt);

	reactor = spdk_reactor_get(0);
	CU_ASSERT(reactor != NULL);

	CU_ASSERT(_spdk_event_queue_run_batch(reactor) == 1);
	CU_ASSERT(test1 == 1);
	CU_ASSERT(test2 == 0xFF);

	spdk_reactors_fini();

	free_cores();
}

static void
test_schedule_thread(void)
{
	struct spdk_cpuset cpuset = {};
	struct spdk_thread *thread;
	struct spdk_reactor *reactor;
	struct spdk_lw_thread *lw_thread;

	allocate_cores(5);

	CU_ASSERT(spdk_reactors_init() == 0);

	spdk_cpuset_set_cpu(&cpuset, 3, true);
	g_next_core = 4;

	/* spdk_reactor_schedule_thread() will be called in spdk_thread_create()
	 * at its end because it is passed to SPDK thread library by
	 * spdk_thread_lib_init().
	 */
	thread = spdk_thread_create(NULL, &cpuset);
	CU_ASSERT(thread != NULL);

	reactor = spdk_reactor_get(3);
	CU_ASSERT(reactor != NULL);

	MOCK_SET(spdk_env_get_current_core, 3);

	CU_ASSERT(_spdk_event_queue_run_batch(reactor) == 1);

	MOCK_CLEAR(spdk_env_get_current_core);

	lw_thread = TAILQ_FIRST(&reactor->threads);
	CU_ASSERT(lw_thread != NULL);
	CU_ASSERT(spdk_thread_get_from_ctx(lw_thread) == thread);

	spdk_set_thread(thread);
	spdk_thread_exit(thread);

	spdk_reactors_fini();

	free_cores();
}

static void
for_each_reactor_done(void *arg1, void *arg2)
{
	uint32_t *count = arg1;
	bool *done = arg2;

	(*count)++;
	*done = true;
}

static void
for_each_reactor_cb(void *arg1, void *arg2)
{
	uint32_t *count = arg1;

	(*count)++;
}

static void
test_for_each_reactor(void)
{
	uint32_t count = 0, i;
	bool done = false;
	struct spdk_reactor *reactor;

	allocate_cores(5);

	CU_ASSERT(spdk_reactors_init() == 0);

	MOCK_SET(spdk_env_get_current_core, 0);

	spdk_for_each_reactor(for_each_reactor_cb, &count, &done, for_each_reactor_done);

	MOCK_CLEAR(spdk_env_get_current_core);

	/* We have not processed any event yet, so count and done should be 0 and false,
	 *  respectively.
	 */
	CU_ASSERT(count == 0);

	/* Poll each reactor to verify the event is passed to each */
	for (i = 0; i < 5; i++) {
		reactor = spdk_reactor_get(i);
		CU_ASSERT(reactor != NULL);

		_spdk_event_queue_run_batch(reactor);
		CU_ASSERT(count == (i + 1));
		CU_ASSERT(done == false);
	}

	/* After each reactor is called, the completion calls it one more time. */
	reactor = spdk_reactor_get(0);
	CU_ASSERT(reactor != NULL);

	_spdk_event_queue_run_batch(reactor);
	CU_ASSERT(count == 6);
	CU_ASSERT(done == true);

	spdk_reactors_fini();

	free_cores();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("app_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test_create_reactor", test_create_reactor) == NULL ||
		CU_add_test(suite, "test_init_reactors", test_init_reactors) == NULL ||
		CU_add_test(suite, "test_event_call", test_event_call) == NULL ||
		CU_add_test(suite, "test_schedule_thread", test_schedule_thread) == NULL ||
		CU_add_test(suite, "test_for_each_reactor", test_for_each_reactor) == NULL
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
