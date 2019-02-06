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
#include "spdk/likely.h"

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"
#include "spdk_internal/thread.h"

#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h"

#define SPDK_EVENT_BATCH_SIZE		8

enum spdk_reactor_state {
	SPDK_REACTOR_STATE_INVALID = 0,
	SPDK_REACTOR_STATE_INITIALIZED = 1,
	SPDK_REACTOR_STATE_RUNNING = 2,
	SPDK_REACTOR_STATE_EXITING = 3,
	SPDK_REACTOR_STATE_SHUTDOWN = 4,
};

struct spdk_lw_thread {
	struct spdk_thread		*thread;
	TAILQ_ENTRY(spdk_lw_thread)	link;
};

struct spdk_reactor {
	/* Logical core number for this reactor. */
	uint32_t					lcore;

	/* Lightweight threads running on this reactor */
	TAILQ_HEAD(, spdk_lw_thread)			threads;

	/* Poller for get the rusage for the reactor. */
	struct spdk_poller				*rusage_poller;

	/* The last known rusage values */
	struct rusage					rusage;

	struct spdk_ring				*events;

	uint64_t					max_delay_us;
} __attribute__((aligned(64)));

static struct spdk_reactor *g_reactors;

static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_INVALID;

static bool g_context_switch_monitor_enabled = true;

static void spdk_reactor_construct(struct spdk_reactor *w, uint32_t lcore,
				   uint64_t max_delay_us);

static struct spdk_mempool *g_spdk_event_mempool = NULL;

static struct spdk_cpuset *g_spdk_app_core_mask;

static struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;
	reactor = spdk_likely(g_reactors) ? &g_reactors[lcore] : NULL;
	return reactor;
}

struct spdk_event *
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	struct spdk_event *event = NULL;
	struct spdk_reactor *reactor = spdk_reactor_get(lcore);

	if (!reactor) {
		assert(false);
		return NULL;
	}

	event = spdk_mempool_get(g_spdk_event_mempool);
	if (event == NULL) {
		assert(false);
		return NULL;
	}

	event->lcore = lcore;
	event->fn = fn;
	event->arg1 = arg1;
	event->arg2 = arg2;

	return event;
}

void
spdk_event_call(struct spdk_event *event)
{
	int rc;
	struct spdk_reactor *reactor;

	reactor = spdk_reactor_get(event->lcore);

	assert(reactor->events != NULL);
	rc = spdk_ring_enqueue(reactor->events, (void **)&event, 1);
	if (rc != 1) {
		assert(false);
	}
}

static inline uint32_t
_spdk_event_queue_run_batch(struct spdk_reactor *reactor, struct spdk_thread *thread)
{
	unsigned count, i;
	void *events[SPDK_EVENT_BATCH_SIZE];

#ifdef DEBUG
	/*
	 * spdk_ring_dequeue() fills events and returns how many entries it wrote,
	 * so we will never actually read uninitialized data from events, but just to be sure
	 * (and to silence a static analyzer false positive), initialize the array to NULL pointers.
	 */
	memset(events, 0, sizeof(events));
#endif

	count = spdk_ring_dequeue(reactor->events, events, SPDK_EVENT_BATCH_SIZE);
	if (count == 0) {
		return 0;
	}

	spdk_set_thread(thread);

	for (i = 0; i < count; i++) {
		struct spdk_event *event = events[i];

		assert(event != NULL);
		event->fn(event->arg1, event->arg2);
	}

	spdk_set_thread(NULL);

	spdk_mempool_put_bulk(g_spdk_event_mempool, events, count);

	return count;
}

#define CONTEXT_SWITCH_MONITOR_PERIOD 1000000

static int
get_rusage(struct spdk_reactor *reactor)
{
	struct rusage		rusage;

	if (getrusage(RUSAGE_THREAD, &rusage) != 0) {
		return -1;
	}

	if (rusage.ru_nvcsw != reactor->rusage.ru_nvcsw || rusage.ru_nivcsw != reactor->rusage.ru_nivcsw) {
		SPDK_INFOLOG(SPDK_LOG_REACTOR,
			     "Reactor %d: %ld voluntary context switches and %ld involuntary context switches in the last second.\n",
			     reactor->lcore, rusage.ru_nvcsw - reactor->rusage.ru_nvcsw,
			     rusage.ru_nivcsw - reactor->rusage.ru_nivcsw);
	}
	reactor->rusage = rusage;

	return -1;
}

void
spdk_reactor_enable_context_switch_monitor(bool enable)
{
	/* This global is being read by multiple threads, so this isn't
	 * strictly thread safe. However, we're toggling between true and
	 * false here, and if a thread sees the value update later than it
	 * should, it's no big deal. */
	g_context_switch_monitor_enabled = enable;
}

bool
spdk_reactor_context_switch_monitor_enabled(void)
{
	return g_context_switch_monitor_enabled;
}

static int
_spdk_reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;
	struct spdk_thread	*thread;
	struct spdk_lw_thread	*lw_thread, *tmp;
	uint64_t		sleep_cycles;
	uint64_t		last_rusage = 0;
	uint32_t		sleep_us;
	int			rc;
	char			thread_name[32];

	snprintf(thread_name, sizeof(thread_name), "reactor_%u", reactor->lcore);
	thread = spdk_thread_create(thread_name);
	if (!thread) {
		return -1;
	}

	lw_thread = calloc(1, sizeof(*lw_thread));
	if (!lw_thread) {
		spdk_thread_exit(thread);
		return -ENOMEM;
	}

	lw_thread->thread = thread;

	TAILQ_INSERT_TAIL(&reactor->threads, lw_thread, link);

	SPDK_NOTICELOG("Reactor started on core %u\n", reactor->lcore);

	sleep_cycles = reactor->max_delay_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;

	while (1) {
		uint64_t now;
		uint64_t next_timer_tick;

		/* For each loop through the reactor, capture the time. This time
		 * is used for all threads. */
		now = spdk_get_ticks();

		/* Set the next timer tick to far in the future */
		next_timer_tick = now + (2 * sleep_cycles);

		/* The default return code is 0 */
		rc = 0;

		TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
			_spdk_event_queue_run_batch(reactor, lw_thread->thread);

			rc = spdk_thread_poll(lw_thread->thread, 0, now);
			if (rc == 0 && sleep_cycles > 0) {
				next_timer_tick = spdk_min(spdk_thread_next_poller_expiration(thread),
							   next_timer_tick);
			}
		}

		/* Determine if the thread can sleep */
		if (sleep_cycles && rc == 0) {
			/* The current time may be sufficiently different than at the start of
			 * this reactor tick, so re-capture it to confirm we still want
			 * to sleep. */
			now = spdk_get_ticks();

			sleep_us = reactor->max_delay_us;

			/* There are timers registered, so don't sleep beyond
			 * when the next timer should fire */
			if (next_timer_tick > 0 && next_timer_tick < (now + sleep_cycles)) {
				if (next_timer_tick <= now) {
					sleep_us = 0;
				} else {
					sleep_us = ((next_timer_tick - now) *
						    SPDK_SEC_TO_USEC) / spdk_get_ticks_hz();
				}
			}

			if (sleep_us > 0) {
				usleep(sleep_us);
			}
		}

		if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {
			break;
		}

		if (g_context_switch_monitor_enabled) {
			if ((last_rusage + CONTEXT_SWITCH_MONITOR_PERIOD) < now) {
				get_rusage(reactor);
				last_rusage = now;
			}
		}
	}

	TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
		TAILQ_REMOVE(&reactor->threads, lw_thread, link);
		spdk_thread_exit(lw_thread->thread);
		free(lw_thread);
	}
	return 0;
}

static void
spdk_reactor_construct(struct spdk_reactor *reactor, uint32_t lcore, uint64_t max_delay_us)
{
	reactor->lcore = lcore;
	reactor->max_delay_us = max_delay_us;

	TAILQ_INIT(&reactor->threads);

	reactor->events = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_SOCKET_ID_ANY);
	assert(reactor->events != NULL);
}

int
spdk_app_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int ret;
	struct spdk_cpuset *validmask;

	ret = spdk_cpuset_parse(cpumask, mask);
	if (ret < 0) {
		return ret;
	}

	validmask = spdk_app_get_core_mask();
	spdk_cpuset_and(cpumask, validmask);

	return 0;
}

struct spdk_cpuset *
spdk_app_get_core_mask(void)
{
	return g_spdk_app_core_mask;
}

void
spdk_reactors_start(void)
{
	struct spdk_reactor *reactor;
	uint32_t i, current_core;
	int rc;

	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;
	g_spdk_app_core_mask = spdk_cpuset_alloc();

	current_core = spdk_env_get_current_core();
	SPDK_ENV_FOREACH_CORE(i) {
		if (i != current_core) {
			reactor = spdk_reactor_get(i);
			rc = spdk_env_thread_launch_pinned(reactor->lcore, _spdk_reactor_run, reactor);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to start reactor thread on core %u\n", reactor->lcore);
				assert(false);
				return;
			}
		}
		spdk_cpuset_set_cpu(g_spdk_app_core_mask, i, true);
	}

	/* Start the master reactor */
	reactor = spdk_reactor_get(current_core);
	_spdk_reactor_run(reactor);

	spdk_env_thread_wait_all();

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
	spdk_cpuset_free(g_spdk_app_core_mask);
	g_spdk_app_core_mask = NULL;
}

void
spdk_reactors_stop(void *arg1, void *arg2)
{
	g_reactor_state = SPDK_REACTOR_STATE_EXITING;
}

int
spdk_reactors_init(unsigned int max_delay_us)
{
	int rc;
	uint32_t i, last_core;
	struct spdk_reactor *reactor;
	char mempool_name[32];

	snprintf(mempool_name, sizeof(mempool_name), "evtpool_%d", getpid());
	g_spdk_event_mempool = spdk_mempool_create(mempool_name,
			       262144 - 1, /* Power of 2 minus 1 is optimal for memory consumption */
			       sizeof(struct spdk_event),
			       SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
			       SPDK_ENV_SOCKET_ID_ANY);

	if (g_spdk_event_mempool == NULL) {
		SPDK_ERRLOG("spdk_event_mempool creation failed\n");
		return -1;
	}

	/* struct spdk_reactor must be aligned on 64 byte boundary */
	last_core = spdk_env_get_last_core();
	rc = posix_memalign((void **)&g_reactors, 64,
			    (last_core + 1) * sizeof(struct spdk_reactor));
	if (rc != 0) {
		SPDK_ERRLOG("Could not allocate array size=%u for g_reactors\n",
			    last_core + 1);
		spdk_mempool_free(g_spdk_event_mempool);
		return -1;
	}

	memset(g_reactors, 0, (last_core + 1) * sizeof(struct spdk_reactor));

	spdk_thread_lib_init(NULL);

	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		spdk_reactor_construct(reactor, i, max_delay_us);
	}

	g_reactor_state = SPDK_REACTOR_STATE_INITIALIZED;

	return 0;
}

void
spdk_reactors_fini(void)
{
	uint32_t i;
	struct spdk_reactor *reactor;

	spdk_thread_lib_fini();

	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		if (spdk_likely(reactor != NULL) && reactor->events != NULL) {
			spdk_ring_free(reactor->events);
		}
	}

	spdk_mempool_free(g_spdk_event_mempool);

	free(g_reactors);
	g_reactors = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("reactor", SPDK_LOG_REACTOR)
