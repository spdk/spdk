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

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#define SPDK_EVENT_BATCH_SIZE		8

static struct spdk_reactor *g_reactors;
static struct spdk_cpuset g_reactor_core_mask;
static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_UNINITIALIZED;

static bool g_framework_context_switch_monitor_enabled = true;

static struct spdk_mempool *g_spdk_event_mempool = NULL;

static void
spdk_reactor_construct(struct spdk_reactor *reactor, uint32_t lcore)
{
	reactor->lcore = lcore;
	reactor->flags.is_valid = true;

	TAILQ_INIT(&reactor->threads);
	reactor->thread_count = 0;

	reactor->events = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_SOCKET_ID_ANY);
	assert(reactor->events != NULL);
}

struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;

	if (g_reactors == NULL) {
		SPDK_WARNLOG("Called spdk_reactor_get() while the g_reactors array was NULL!\n");
		return NULL;
	}

	reactor = &g_reactors[lcore];

	if (reactor->flags.is_valid == false) {
		return NULL;
	}

	return reactor;
}

static int spdk_reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op);
static bool spdk_reactor_thread_op_supported(enum spdk_thread_op op);

int
spdk_reactors_init(void)
{
	int rc;
	uint32_t i, last_core;
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

	spdk_thread_lib_init_ext(spdk_reactor_thread_op, spdk_reactor_thread_op_supported,
				 sizeof(struct spdk_lw_thread));

	SPDK_ENV_FOREACH_CORE(i) {
		spdk_reactor_construct(&g_reactors[i], i);
	}

	g_reactor_state = SPDK_REACTOR_STATE_INITIALIZED;

	return 0;
}

void
spdk_reactors_fini(void)
{
	uint32_t i;
	struct spdk_reactor *reactor;

	if (g_reactor_state == SPDK_REACTOR_STATE_UNINITIALIZED) {
		return;
	}

	spdk_thread_lib_fini();

	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		assert(reactor != NULL);
		assert(reactor->thread_count == 0);
		if (reactor->events != NULL) {
			spdk_ring_free(reactor->events);
		}
	}

	spdk_mempool_free(g_spdk_event_mempool);

	free(g_reactors);
	g_reactors = NULL;
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

	assert(reactor != NULL);
	assert(reactor->events != NULL);

	rc = spdk_ring_enqueue(reactor->events, (void **)&event, 1, NULL);
	if (rc != 1) {
		assert(false);
	}
}

static inline uint32_t
_spdk_event_queue_run_batch(struct spdk_reactor *reactor)
{
	unsigned count, i;
	void *events[SPDK_EVENT_BATCH_SIZE];
	struct spdk_thread *thread;
	struct spdk_lw_thread *lw_thread;

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

	/* Execute the events. There are still some remaining events
	 * that must occur on an SPDK thread. To accomodate those, try to
	 * run them on the first thread in the list, if it exists. */
	lw_thread = TAILQ_FIRST(&reactor->threads);
	if (lw_thread) {
		thread = spdk_thread_get_from_ctx(lw_thread);
	} else {
		thread = NULL;
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

/* 1s */
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
spdk_framework_enable_context_switch_monitor(bool enable)
{
	/* This global is being read by multiple threads, so this isn't
	 * strictly thread safe. However, we're toggling between true and
	 * false here, and if a thread sees the value update later than it
	 * should, it's no big deal. */
	g_framework_context_switch_monitor_enabled = enable;
}

bool
spdk_framework_context_switch_monitor_enabled(void)
{
	return g_framework_context_switch_monitor_enabled;
}

static void
_set_thread_name(const char *thread_name)
{
#if defined(__linux__)
	prctl(PR_SET_NAME, thread_name, 0, 0, 0);
#elif defined(__FreeBSD__)
	pthread_set_name_np(pthread_self(), thread_name);
#else
#error missing platform support for thread name
#endif
}

static int _reactor_schedule_thread(struct spdk_thread *thread);
static uint64_t g_rusage_period;

static void
reactor_run(struct spdk_reactor *reactor)
{
	struct spdk_thread	*thread;
	struct spdk_lw_thread	*lw_thread, *tmp;
	uint64_t		now;
	int			rc;

	_spdk_event_queue_run_batch(reactor);

	TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		rc = spdk_thread_poll(thread, 0, reactor->tsc_last);

		now = spdk_thread_get_last_tsc(thread);
		if (rc == 0) {
			reactor->idle_tsc += now - reactor->tsc_last;
		} else if (rc > 0) {
			reactor->busy_tsc += now - reactor->tsc_last;
		}
		reactor->tsc_last = now;

		if (spdk_unlikely(lw_thread->resched)) {
			lw_thread->resched = false;
			TAILQ_REMOVE(&reactor->threads, lw_thread, link);
			assert(reactor->thread_count > 0);
			reactor->thread_count--;
			_reactor_schedule_thread(thread);
			continue;
		}

		if (spdk_unlikely(spdk_thread_is_exited(thread) &&
				  spdk_thread_is_idle(thread))) {
			TAILQ_REMOVE(&reactor->threads, lw_thread, link);
			assert(reactor->thread_count > 0);
			reactor->thread_count--;
			spdk_thread_destroy(thread);
			continue;
		}
	}

	if (g_framework_context_switch_monitor_enabled) {
		if ((reactor->last_rusage + g_rusage_period) < reactor->tsc_last) {
			get_rusage(reactor);
			reactor->last_rusage = reactor->tsc_last;
		}
	}
}

static int
_spdk_reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;
	struct spdk_thread	*thread;
	struct spdk_lw_thread	*lw_thread, *tmp;
	char			thread_name[32];

	SPDK_NOTICELOG("Reactor started on core %u\n", reactor->lcore);

	/* Rename the POSIX thread because the reactor is tied to the POSIX
	 * thread in the SPDK event library.
	 */
	snprintf(thread_name, sizeof(thread_name), "reactor_%u", reactor->lcore);
	_set_thread_name(thread_name);

	reactor->tsc_last = spdk_get_ticks();

	while (1) {
		reactor_run(reactor);

		if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {
			break;
		}
	}

	TAILQ_FOREACH(lw_thread, &reactor->threads, link) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		spdk_set_thread(thread);
		spdk_thread_exit(thread);
	}

	while (!TAILQ_EMPTY(&reactor->threads)) {
		TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
			thread = spdk_thread_get_from_ctx(lw_thread);
			spdk_set_thread(thread);
			if (spdk_thread_is_exited(thread)) {
				TAILQ_REMOVE(&reactor->threads, lw_thread, link);
				assert(reactor->thread_count > 0);
				reactor->thread_count--;
				spdk_thread_destroy(thread);
			} else {
				spdk_thread_poll(thread, 0, 0);
			}
		}
	}

	return 0;
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
	return &g_reactor_core_mask;
}

void
spdk_reactors_start(void)
{
	struct spdk_reactor *reactor;
	struct spdk_cpuset tmp_cpumask = {};
	uint32_t i, current_core;
	int rc;
	char thread_name[32];

	g_rusage_period = (CONTEXT_SWITCH_MONITOR_PERIOD * spdk_get_ticks_hz()) / SPDK_SEC_TO_USEC;
	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;

	current_core = spdk_env_get_current_core();
	SPDK_ENV_FOREACH_CORE(i) {
		if (i != current_core) {
			reactor = spdk_reactor_get(i);
			if (reactor == NULL) {
				continue;
			}

			rc = spdk_env_thread_launch_pinned(reactor->lcore, _spdk_reactor_run, reactor);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to start reactor thread on core %u\n", reactor->lcore);
				assert(false);
				return;
			}

			/* For now, for each reactor spawn one thread. */
			snprintf(thread_name, sizeof(thread_name), "reactor_%u", reactor->lcore);

			spdk_cpuset_zero(&tmp_cpumask);
			spdk_cpuset_set_cpu(&tmp_cpumask, i, true);

			spdk_thread_create(thread_name, &tmp_cpumask);
		}
		spdk_cpuset_set_cpu(&g_reactor_core_mask, i, true);
	}

	/* Start the master reactor */
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);
	_spdk_reactor_run(reactor);

	spdk_env_thread_wait_all();

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
}

void
spdk_reactors_stop(void *arg1)
{
	g_reactor_state = SPDK_REACTOR_STATE_EXITING;
}

static pthread_mutex_t g_scheduler_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_core = UINT32_MAX;

static void
_schedule_thread(void *arg1, void *arg2)
{
	struct spdk_lw_thread *lw_thread = arg1;
	struct spdk_thread *thread;
	struct spdk_cpuset *cpumask;
	struct spdk_reactor *reactor;
	uint32_t current_core;

	current_core = spdk_env_get_current_core();

	thread = spdk_thread_get_from_ctx(lw_thread);
	cpumask = spdk_thread_get_cpumask(thread);
	if (!spdk_cpuset_get_cpu(cpumask, current_core)) {
		SPDK_ERRLOG("Thread was scheduled to the wrong core %d\n", current_core);
		assert(false);
	}

	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);

	TAILQ_INSERT_TAIL(&reactor->threads, lw_thread, link);
	reactor->thread_count++;
}

static int
_reactor_schedule_thread(struct spdk_thread *thread)
{
	uint32_t core;
	struct spdk_lw_thread *lw_thread;
	struct spdk_event *evt = NULL;
	struct spdk_cpuset *cpumask;
	uint32_t i;

	cpumask = spdk_thread_get_cpumask(thread);

	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);
	memset(lw_thread, 0, sizeof(*lw_thread));

	pthread_mutex_lock(&g_scheduler_mtx);
	for (i = 0; i < spdk_env_get_core_count(); i++) {
		if (g_next_core > spdk_env_get_last_core()) {
			g_next_core = spdk_env_get_first_core();
		}
		core = g_next_core;
		g_next_core = spdk_env_get_next_core(g_next_core);

		if (spdk_cpuset_get_cpu(cpumask, core)) {
			evt = spdk_event_allocate(core, _schedule_thread, lw_thread, NULL);
			break;
		}
	}
	pthread_mutex_unlock(&g_scheduler_mtx);

	assert(evt != NULL);
	if (evt == NULL) {
		SPDK_ERRLOG("Unable to schedule thread on requested core mask.\n");
		return -1;
	}

	lw_thread->tsc_start = spdk_get_ticks();

	spdk_event_call(evt);

	return 0;
}

static void
_reactor_request_thread_reschedule(struct spdk_thread *thread)
{
	struct spdk_lw_thread *lw_thread;

	assert(thread == spdk_get_thread());

	lw_thread = spdk_thread_get_ctx(thread);

	assert(lw_thread != NULL);

	lw_thread->resched = true;
}

static int
spdk_reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op)
{
	switch (op) {
	case SPDK_THREAD_OP_NEW:
		return _reactor_schedule_thread(thread);
	case SPDK_THREAD_OP_RESCHED:
		_reactor_request_thread_reschedule(thread);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static bool
spdk_reactor_thread_op_supported(enum spdk_thread_op op)
{
	switch (op) {
	case SPDK_THREAD_OP_NEW:
	case SPDK_THREAD_OP_RESCHED:
		return true;
	default:
		return false;
	}
}

struct call_reactor {
	uint32_t cur_core;
	spdk_event_fn fn;
	void *arg1;
	void *arg2;

	uint32_t orig_core;
	spdk_event_fn cpl;
};

static void
spdk_on_reactor(void *arg1, void *arg2)
{
	struct call_reactor *cr = arg1;
	struct spdk_event *evt;

	cr->fn(cr->arg1, cr->arg2);

	cr->cur_core = spdk_env_get_next_core(cr->cur_core);

	if (cr->cur_core > spdk_env_get_last_core()) {
		SPDK_DEBUGLOG(SPDK_LOG_REACTOR, "Completed reactor iteration\n");

		evt = spdk_event_allocate(cr->orig_core, cr->cpl, cr->arg1, cr->arg2);
		free(cr);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_REACTOR, "Continuing reactor iteration to %d\n",
			      cr->cur_core);

		evt = spdk_event_allocate(cr->cur_core, spdk_on_reactor, arg1, NULL);
	}
	assert(evt != NULL);
	spdk_event_call(evt);
}

void
spdk_for_each_reactor(spdk_event_fn fn, void *arg1, void *arg2, spdk_event_fn cpl)
{
	struct call_reactor *cr;
	struct spdk_event *evt;

	cr = calloc(1, sizeof(*cr));
	if (!cr) {
		SPDK_ERRLOG("Unable to perform reactor iteration\n");
		cpl(arg1, arg2);
		return;
	}

	cr->fn = fn;
	cr->arg1 = arg1;
	cr->arg2 = arg2;
	cr->cpl = cpl;
	cr->orig_core = spdk_env_get_current_core();
	cr->cur_core = spdk_env_get_first_core();

	SPDK_DEBUGLOG(SPDK_LOG_REACTOR, "Starting reactor iteration from %d\n", cr->orig_core);

	evt = spdk_event_allocate(cr->cur_core, spdk_on_reactor, cr, NULL);
	assert(evt != NULL);

	spdk_event_call(evt);
}

SPDK_LOG_REGISTER_COMPONENT("reactor", SPDK_LOG_REACTOR)
