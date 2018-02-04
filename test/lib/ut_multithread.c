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
#include "spdk/io_channel.h"
#include "spdk_internal/mock.h"

static uint32_t g_ut_num_threads;
static uint64_t g_current_time_in_us = 0;

int allocate_threads(int num_threads);
void free_threads(void);
void poll_threads(void);
int poll_thread(uintptr_t thread_id);
void increment_time(uint64_t time_in_us);
void reset_time(void);

struct ut_msg {
	spdk_thread_fn		fn;
	void			*ctx;
	TAILQ_ENTRY(ut_msg)	link;
};

struct ut_thread {
	struct spdk_thread	*thread;
	struct spdk_io_channel	*ch;
	TAILQ_HEAD(, ut_msg)	msgs;
	TAILQ_HEAD(, ut_poller)	pollers;
};

struct ut_thread *g_ut_threads;

struct ut_poller {
	spdk_poller_fn		fn;
	void			*arg;
	TAILQ_ENTRY(ut_poller)	tailq;
	uint64_t		period_us;
	uint64_t		next_expiration_in_us;
};

static void
__send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	struct ut_thread *thread = thread_ctx;
	struct ut_msg *msg;

	msg = calloc(1, sizeof(*msg));
	SPDK_CU_ASSERT_FATAL(msg != NULL);

	msg->fn = fn;
	msg->ctx = ctx;
	TAILQ_INSERT_TAIL(&thread->msgs, msg, link);
}

static struct spdk_poller *
__start_poller(void *thread_ctx, spdk_thread_fn fn, void *arg, uint64_t period_microseconds)
{
	struct ut_thread *thread = thread_ctx;
	struct ut_poller *poller = calloc(1, sizeof(struct ut_poller));

	SPDK_CU_ASSERT_FATAL(poller != NULL);

	poller->fn = fn;
	poller->arg = arg;
	poller->period_us = period_microseconds;
	poller->next_expiration_in_us = g_current_time_in_us + poller->period_us;

	TAILQ_INSERT_TAIL(&thread->pollers, poller, tailq);

	return (struct spdk_poller *)poller;
}

static void
__stop_poller(struct spdk_poller *poller, void *thread_ctx)
{
	struct ut_thread *thread = thread_ctx;

	TAILQ_REMOVE(&thread->pollers, (struct ut_poller *)poller, tailq);

	free(poller);
}

static uintptr_t g_thread_id = MOCK_PASS_THRU;

static void
set_thread(uintptr_t thread_id)
{
	g_thread_id = thread_id;
	MOCK_SET(pthread_self, pthread_t, (pthread_t)thread_id);
}

int
allocate_threads(int num_threads)
{
	struct spdk_thread *thread;
	uint32_t i;

	g_ut_num_threads = num_threads;

	g_ut_threads = calloc(num_threads, sizeof(*g_ut_threads));
	SPDK_CU_ASSERT_FATAL(g_ut_threads != NULL);

	for (i = 0; i < g_ut_num_threads; i++) {
		set_thread(i);
		spdk_allocate_thread(__send_msg, __start_poller, __stop_poller,
				     &g_ut_threads[i], NULL);
		thread = spdk_get_thread();
		SPDK_CU_ASSERT_FATAL(thread != NULL);
		g_ut_threads[i].thread = thread;
		TAILQ_INIT(&g_ut_threads[i].msgs);
		TAILQ_INIT(&g_ut_threads[i].pollers);
	}

	set_thread(MOCK_PASS_THRU);
	return 0;
}

void
free_threads(void)
{
	uint32_t i;

	for (i = 0; i < g_ut_num_threads; i++) {
		set_thread(i);
		spdk_free_thread();
	}

	g_ut_num_threads = 0;
	free(g_ut_threads);
	g_ut_threads = NULL;
}

void
increment_time(uint64_t time_in_us)
{
	g_current_time_in_us += time_in_us;
}

static void
reset_pollers(void)
{
	uint32_t		i = 0;
	struct ut_thread	*thread = NULL;
	struct ut_poller	*poller = NULL;
	uintptr_t		original_thread_id = g_thread_id;

	CU_ASSERT(g_current_time_in_us == 0);

	for (i = 0; i < g_ut_num_threads; i++) {
		set_thread(i);
		thread = &g_ut_threads[i];

		TAILQ_FOREACH(poller, &thread->pollers, tailq) {
			poller->next_expiration_in_us = g_current_time_in_us + poller->period_us;
		}
	}

	set_thread(original_thread_id);
}

void
reset_time(void)
{
	g_current_time_in_us = 0;
	reset_pollers();
}

int
poll_thread(uintptr_t thread_id)
{
	int count = 0;
	struct ut_thread *thread = &g_ut_threads[thread_id];
	struct ut_msg *msg;
	struct ut_poller *poller;
	uintptr_t original_thread_id;
	TAILQ_HEAD(, ut_poller)	tmp_pollers;

	CU_ASSERT(thread_id != (uintptr_t)MOCK_PASS_THRU);
	CU_ASSERT(thread_id < g_ut_num_threads);

	original_thread_id = g_thread_id;
	set_thread(thread_id);

	while (!TAILQ_EMPTY(&thread->msgs)) {
		msg = TAILQ_FIRST(&thread->msgs);
		TAILQ_REMOVE(&thread->msgs, msg, link);

		msg->fn(msg->ctx);
		count++;
		free(msg);
	}

	TAILQ_INIT(&tmp_pollers);

	while (!TAILQ_EMPTY(&thread->pollers)) {
		poller = TAILQ_FIRST(&thread->pollers);
		TAILQ_REMOVE(&thread->pollers, poller, tailq);

		while (g_current_time_in_us >= poller->next_expiration_in_us) {
			if (poller->fn) {
				poller->fn(poller->arg);
			}

			if (poller->period_us == 0) {
				break;
			} else {
				poller->next_expiration_in_us += poller->period_us;
			}
		}

		TAILQ_INSERT_TAIL(&tmp_pollers, poller, tailq);
	}

	TAILQ_SWAP(&tmp_pollers, &thread->pollers, ut_poller, tailq);

	set_thread(original_thread_id);

	return count;
}

void
poll_threads(void)
{
	bool msg_processed;
	uint32_t i, count;

	while (true) {
		msg_processed = false;

		for (i = 0; i < g_ut_num_threads; i++) {
			count = poll_thread(i);
			if (count > 0) {
				msg_processed = true;
			}
		}

		if (!msg_processed) {
			break;
		}
	}
}
