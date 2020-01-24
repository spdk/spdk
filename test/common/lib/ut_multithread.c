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

#include "spdk_cunit.h"
#include "spdk/thread.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/thread.h"

#include "common/lib/test_env.c"

static uint32_t g_ut_num_threads;

int allocate_threads(int num_threads);
void free_threads(void);
void poll_threads(void);
bool poll_thread(uintptr_t thread_id);

struct ut_msg {
	spdk_msg_fn		fn;
	void			*ctx;
	TAILQ_ENTRY(ut_msg)	link;
};

struct ut_thread {
	struct spdk_thread	*thread;
	struct spdk_io_channel	*ch;
};

struct ut_thread *g_ut_threads;

#define INVALID_THREAD 0x1000

static uint64_t g_thread_id = INVALID_THREAD;

static void
set_thread(uintptr_t thread_id)
{
	g_thread_id = thread_id;
	if (thread_id == INVALID_THREAD) {
		spdk_set_thread(NULL);
	} else {
		spdk_set_thread(g_ut_threads[thread_id].thread);
	}

}

int
allocate_threads(int num_threads)
{
	struct spdk_thread *thread;
	uint32_t i;

	spdk_thread_lib_init(NULL, NULL, 0);

	g_ut_num_threads = num_threads;

	g_ut_threads = calloc(num_threads, sizeof(*g_ut_threads));
	assert(g_ut_threads != NULL);

	for (i = 0; i < g_ut_num_threads; i++) {
		set_thread(i);
		thread = spdk_thread_create(NULL, NULL);
		assert(thread != NULL);
		g_ut_threads[i].thread = thread;
	}

	set_thread(INVALID_THREAD);
	return 0;
}

void
free_threads(void)
{
	uint32_t i;
	int rc __attribute__((unused));

	for (i = 0; i < g_ut_num_threads; i++) {
		set_thread(i);
		rc = spdk_thread_exit(g_ut_threads[i].thread);
		assert(rc == 0);
		spdk_thread_destroy(g_ut_threads[i].thread);
		g_ut_threads[i].thread = NULL;
	}

	g_ut_num_threads = 0;
	free(g_ut_threads);
	g_ut_threads = NULL;

	spdk_thread_lib_fini();
}

bool
poll_thread(uintptr_t thread_id)
{
	bool busy = false;
	struct ut_thread *thread = &g_ut_threads[thread_id];
	uintptr_t original_thread_id;

	assert(thread_id != (uintptr_t)INVALID_THREAD);
	assert(thread_id < g_ut_num_threads);

	original_thread_id = g_thread_id;
	set_thread(INVALID_THREAD);

	while (spdk_thread_poll(thread->thread, 0, 0) > 0) {
		busy = true;
	}

	set_thread(original_thread_id);

	return busy;
}

void
poll_threads(void)
{
	while (true) {
		bool busy = false;

		for (uint32_t i = 0; i < g_ut_num_threads; i++) {
			busy = busy || poll_thread(i);
		}

		if (!busy) {
			break;
		}
	}
}
