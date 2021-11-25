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

#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/scheduler.h"
#include "spdk/string.h"
#include "spdk/fd_group.h"

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/eventfd.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#define SPDK_EVENT_BATCH_SIZE		8

static struct spdk_reactor *g_reactors;
static uint32_t g_reactor_count;
static struct spdk_cpuset g_reactor_core_mask;
static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_UNINITIALIZED;

static bool g_framework_context_switch_monitor_enabled = true;

static struct spdk_mempool *g_spdk_event_mempool = NULL;

TAILQ_HEAD(, spdk_scheduler) g_scheduler_list
	= TAILQ_HEAD_INITIALIZER(g_scheduler_list);

static struct spdk_scheduler *g_scheduler = NULL;
static struct spdk_reactor *g_scheduling_reactor;
bool g_scheduling_in_progress = false;
static uint64_t g_scheduler_period = 0;
static uint32_t g_scheduler_core_number;
static struct spdk_scheduler_core_info *g_core_infos = NULL;

TAILQ_HEAD(, spdk_governor) g_governor_list
	= TAILQ_HEAD_INITIALIZER(g_governor_list);

static struct spdk_governor *g_governor = NULL;

static int reactor_interrupt_init(struct spdk_reactor *reactor);
static void reactor_interrupt_fini(struct spdk_reactor *reactor);

static pthread_mutex_t g_stopping_reactors_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool g_stopping_reactors = false;

static struct spdk_scheduler *
_scheduler_find(const char *name)
{
	struct spdk_scheduler *tmp;

	TAILQ_FOREACH(tmp, &g_scheduler_list, link) {
		if (strcmp(name, tmp->name) == 0) {
			return tmp;
		}
	}

	return NULL;
}

int
spdk_scheduler_set(const char *name)
{
	struct spdk_scheduler *scheduler;
	int rc = 0;

	/* NULL scheduler was specifically requested */
	if (name == NULL) {
		if (g_scheduler) {
			g_scheduler->deinit();
		}
		g_scheduler = NULL;
		return 0;
	}

	scheduler = _scheduler_find(name);
	if (scheduler == NULL) {
		SPDK_ERRLOG("Requested scheduler is missing\n");
		return -EINVAL;
	}

	if (g_scheduler == scheduler) {
		return 0;
	}

	rc = scheduler->init();
	if (rc == 0) {
		if (g_scheduler) {
			g_scheduler->deinit();
		}
		g_scheduler = scheduler;
	}

	return rc;
}

struct spdk_scheduler *
spdk_scheduler_get(void)
{
	return g_scheduler;
}

uint64_t
spdk_scheduler_get_period(void)
{
	/* Convert from ticks to microseconds */
	return (g_scheduler_period * SPDK_SEC_TO_USEC / spdk_get_ticks_hz());
}

void
spdk_scheduler_set_period(uint64_t period)
{
	/* Convert microseconds to ticks */
	g_scheduler_period = period * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
}

void
spdk_scheduler_register(struct spdk_scheduler *scheduler)
{
	if (_scheduler_find(scheduler->name)) {
		SPDK_ERRLOG("scheduler named '%s' already registered.\n", scheduler->name);
		assert(false);
		return;
	}

	TAILQ_INSERT_TAIL(&g_scheduler_list, scheduler, link);
}

static void
reactor_construct(struct spdk_reactor *reactor, uint32_t lcore)
{
	reactor->lcore = lcore;
	reactor->flags.is_valid = true;

	TAILQ_INIT(&reactor->threads);
	reactor->thread_count = 0;
	spdk_cpuset_zero(&reactor->notify_cpuset);

	reactor->events = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_SOCKET_ID_ANY);
	if (reactor->events == NULL) {
		SPDK_ERRLOG("Failed to allocate events ring\n");
		assert(false);
	}

	/* Always initialize interrupt facilities for reactor */
	if (reactor_interrupt_init(reactor) != 0) {
		/* Reactor interrupt facilities are necessary if seting app to interrupt mode. */
		if (spdk_interrupt_mode_is_enabled()) {
			SPDK_ERRLOG("Failed to prepare intr facilities\n");
			assert(false);
		}
		return;
	}

	/* If application runs with full interrupt ability,
	 * all reactors are going to run in interrupt mode.
	 */
	if (spdk_interrupt_mode_is_enabled()) {
		uint32_t i;

		SPDK_ENV_FOREACH_CORE(i) {
			spdk_cpuset_set_cpu(&reactor->notify_cpuset, i, true);
		}
		reactor->in_interrupt = true;
	}
}

struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;

	if (g_reactors == NULL) {
		SPDK_WARNLOG("Called spdk_reactor_get() while the g_reactors array was NULL!\n");
		return NULL;
	}

	if (lcore >= g_reactor_count) {
		return NULL;
	}

	reactor = &g_reactors[lcore];

	if (reactor->flags.is_valid == false) {
		return NULL;
	}

	return reactor;
}

static int reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op);
static bool reactor_thread_op_supported(enum spdk_thread_op op);

int
spdk_reactors_init(void)
{
	struct spdk_reactor *reactor;
	int rc;
	uint32_t i, current_core;
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
	g_reactor_count = spdk_env_get_last_core() + 1;
	rc = posix_memalign((void **)&g_reactors, 64,
			    g_reactor_count * sizeof(struct spdk_reactor));
	if (rc != 0) {
		SPDK_ERRLOG("Could not allocate array size=%u for g_reactors\n",
			    g_reactor_count);
		spdk_mempool_free(g_spdk_event_mempool);
		return -1;
	}

	g_core_infos = calloc(g_reactor_count, sizeof(*g_core_infos));
	if (g_core_infos == NULL) {
		SPDK_ERRLOG("Could not allocate memory for g_core_infos\n");
		spdk_mempool_free(g_spdk_event_mempool);
		free(g_reactors);
		return -ENOMEM;
	}

	memset(g_reactors, 0, (g_reactor_count) * sizeof(struct spdk_reactor));

	spdk_thread_lib_init_ext(reactor_thread_op, reactor_thread_op_supported,
				 sizeof(struct spdk_lw_thread));

	SPDK_ENV_FOREACH_CORE(i) {
		reactor_construct(&g_reactors[i], i);
	}

	current_core = spdk_env_get_current_core();
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);
	g_scheduling_reactor = reactor;

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

		reactor_interrupt_fini(reactor);

		if (g_core_infos != NULL) {
			free(g_core_infos[i].thread_infos);
		}
	}

	spdk_mempool_free(g_spdk_event_mempool);

	free(g_reactors);
	g_reactors = NULL;
	free(g_core_infos);
	g_core_infos = NULL;
}

static void _reactor_set_interrupt_mode(void *arg1, void *arg2);

static void
_reactor_set_notify_cpuset(void *arg1, void *arg2)
{
	struct spdk_reactor *target = arg1;
	struct spdk_reactor *reactor = spdk_reactor_get(spdk_env_get_current_core());

	assert(reactor != NULL);
	spdk_cpuset_set_cpu(&reactor->notify_cpuset, target->lcore, target->new_in_interrupt);
}

static void
_reactor_set_notify_cpuset_cpl(void *arg1, void *arg2)
{
	struct spdk_reactor *target = arg1;

	if (target->new_in_interrupt == false) {
		target->set_interrupt_mode_in_progress = false;
		spdk_thread_send_msg(_spdk_get_app_thread(), target->set_interrupt_mode_cb_fn,
				     target->set_interrupt_mode_cb_arg);
	} else {
		struct spdk_event *ev;

		ev = spdk_event_allocate(target->lcore, _reactor_set_interrupt_mode, target, NULL);
		assert(ev);
		spdk_event_call(ev);
	}
}

static void
_reactor_set_thread_interrupt_mode(void *ctx)
{
	struct spdk_reactor *reactor = ctx;

	spdk_thread_set_interrupt_mode(reactor->in_interrupt);
}

static void
_reactor_set_interrupt_mode(void *arg1, void *arg2)
{
	struct spdk_reactor *target = arg1;
	struct spdk_thread *thread;
	struct spdk_lw_thread *lw_thread, *tmp;

	assert(target == spdk_reactor_get(spdk_env_get_current_core()));
	assert(target != NULL);
	assert(target->in_interrupt != target->new_in_interrupt);
	SPDK_DEBUGLOG(reactor, "Do reactor set on core %u from %s to state %s\n",
		      target->lcore, target->in_interrupt ? "intr" : "poll", target->new_in_interrupt ? "intr" : "poll");

	target->in_interrupt = target->new_in_interrupt;

	/* Align spdk_thread with reactor to interrupt mode or poll mode */
	TAILQ_FOREACH_SAFE(lw_thread, &target->threads, link, tmp) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		spdk_thread_send_msg(thread, _reactor_set_thread_interrupt_mode, target);
	}

	if (target->new_in_interrupt == false) {
		/* Reactor is no longer in interrupt mode. Refresh the tsc_last to accurately
		 * track reactor stats. */
		target->tsc_last = spdk_get_ticks();
		spdk_for_each_reactor(_reactor_set_notify_cpuset, target, NULL, _reactor_set_notify_cpuset_cpl);
	} else {
		uint64_t notify = 1;
		int rc = 0;

		/* Always trigger spdk_event and resched event in case of race condition */
		rc = write(target->events_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify event queue: %s.\n", spdk_strerror(errno));
		}
		rc = write(target->resched_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify reschedule: %s.\n", spdk_strerror(errno));
		}

		target->set_interrupt_mode_in_progress = false;
		spdk_thread_send_msg(_spdk_get_app_thread(), target->set_interrupt_mode_cb_fn,
				     target->set_interrupt_mode_cb_arg);
	}
}

int
spdk_reactor_set_interrupt_mode(uint32_t lcore, bool new_in_interrupt,
				spdk_reactor_set_interrupt_mode_cb cb_fn, void *cb_arg)
{
	struct spdk_reactor *target;

	target = spdk_reactor_get(lcore);
	if (target == NULL) {
		return -EINVAL;
	}

	/* Eventfd has to be supported in order to use interrupt functionality. */
	if (target->fgrp == NULL) {
		return -ENOTSUP;
	}

	if (spdk_get_thread() != _spdk_get_app_thread()) {
		SPDK_ERRLOG("It is only permitted within spdk application thread.\n");
		return -EPERM;
	}

	if (target->in_interrupt == new_in_interrupt) {
		cb_fn(cb_arg);
		return 0;
	}

	if (target->set_interrupt_mode_in_progress) {
		SPDK_NOTICELOG("Reactor(%u) is already in progress to set interrupt mode\n", lcore);
		return -EBUSY;
	}
	target->set_interrupt_mode_in_progress = true;

	target->new_in_interrupt = new_in_interrupt;
	target->set_interrupt_mode_cb_fn = cb_fn;
	target->set_interrupt_mode_cb_arg = cb_arg;

	SPDK_DEBUGLOG(reactor, "Starting reactor event from %d to %d\n",
		      spdk_env_get_current_core(), lcore);

	if (new_in_interrupt == false) {
		/* For potential race cases, when setting the reactor to poll mode,
		 * first change the mode of the reactor and then clear the corresponding
		 * bit of the notify_cpuset of each reactor.
		 */
		struct spdk_event *ev;

		ev = spdk_event_allocate(lcore, _reactor_set_interrupt_mode, target, NULL);
		assert(ev);
		spdk_event_call(ev);
	} else {
		/* For race cases, when setting the reactor to interrupt mode, first set the
		 * corresponding bit of the notify_cpuset of each reactor and then change the mode.
		 */
		spdk_for_each_reactor(_reactor_set_notify_cpuset, target, NULL, _reactor_set_notify_cpuset_cpl);
	}

	return 0;
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
	struct spdk_reactor *local_reactor = NULL;
	uint32_t current_core = spdk_env_get_current_core();

	reactor = spdk_reactor_get(event->lcore);

	assert(reactor != NULL);
	assert(reactor->events != NULL);

	rc = spdk_ring_enqueue(reactor->events, (void **)&event, 1, NULL);
	if (rc != 1) {
		assert(false);
	}

	if (current_core != SPDK_ENV_LCORE_ID_ANY) {
		local_reactor = spdk_reactor_get(current_core);
	}

	/* If spdk_event_call isn't called on a reactor, always send a notification.
	 * If it is called on a reactor, send a notification if the destination reactor
	 * is indicated in interrupt mode state.
	 */
	if (spdk_unlikely(local_reactor == NULL) ||
	    spdk_unlikely(spdk_cpuset_get_cpu(&local_reactor->notify_cpuset, event->lcore))) {
		uint64_t notify = 1;

		rc = write(reactor->events_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify event queue: %s.\n", spdk_strerror(errno));
		}
	}
}

static inline int
event_queue_run_batch(void *arg)
{
	struct spdk_reactor *reactor = arg;
	size_t count, i;
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

	/* Operate event notification if this reactor currently runs in interrupt state */
	if (spdk_unlikely(reactor->in_interrupt)) {
		uint64_t notify = 1;
		int rc;

		/* There may be race between event_acknowledge and another producer's event_notify,
		 * so event_acknowledge should be applied ahead. And then check for self's event_notify.
		 * This can avoid event notification missing.
		 */
		rc = read(reactor->events_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to acknowledge event queue: %s.\n", spdk_strerror(errno));
			return -errno;
		}

		count = spdk_ring_dequeue(reactor->events, events, SPDK_EVENT_BATCH_SIZE);

		if (spdk_ring_count(reactor->events) != 0) {
			/* Trigger new notification if there are still events in event-queue waiting for processing. */
			rc = write(reactor->events_fd, &notify, sizeof(notify));
			if (rc < 0) {
				SPDK_ERRLOG("failed to notify event queue: %s.\n", spdk_strerror(errno));
				return -errno;
			}
		}
	} else {
		count = spdk_ring_dequeue(reactor->events, events, SPDK_EVENT_BATCH_SIZE);
	}

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

	for (i = 0; i < count; i++) {
		struct spdk_event *event = events[i];

		assert(event != NULL);
		spdk_set_thread(thread);
		event->fn(event->arg1, event->arg2);
		spdk_set_thread(NULL);
	}

	spdk_mempool_put_bulk(g_spdk_event_mempool, events, count);

	return (int)count;
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
		SPDK_INFOLOG(reactor,
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
	pthread_setname_np(pthread_self(), thread_name);
#endif
}

static void
_init_thread_stats(struct spdk_reactor *reactor, struct spdk_lw_thread *lw_thread)
{
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);
	struct spdk_thread_stats prev_total_stats;

	/* Read total_stats before updating it to calculate stats during the last scheduling period. */
	prev_total_stats = lw_thread->total_stats;

	spdk_set_thread(thread);
	spdk_thread_get_stats(&lw_thread->total_stats);
	spdk_set_thread(NULL);

	lw_thread->current_stats.busy_tsc = lw_thread->total_stats.busy_tsc - prev_total_stats.busy_tsc;
	lw_thread->current_stats.idle_tsc = lw_thread->total_stats.idle_tsc - prev_total_stats.idle_tsc;
}

static void
_threads_reschedule_thread(struct spdk_scheduler_thread_info *thread_info)
{
	struct spdk_lw_thread *lw_thread;
	struct spdk_thread *thread;

	thread = spdk_thread_get_by_id(thread_info->thread_id);
	if (thread == NULL) {
		/* Thread no longer exists. */
		return;
	}
	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);

	lw_thread->lcore = thread_info->lcore;
	lw_thread->resched = true;
}

static void
_threads_reschedule(struct spdk_scheduler_core_info *cores_info)
{
	struct spdk_scheduler_core_info *core;
	struct spdk_scheduler_thread_info *thread_info;
	uint32_t i, j;

	SPDK_ENV_FOREACH_CORE(i) {
		core = &cores_info[i];
		for (j = 0; j < core->threads_count; j++) {
			thread_info = &core->thread_infos[j];
			if (thread_info->lcore != i) {
				_threads_reschedule_thread(thread_info);
			}
		}
		core->threads_count = 0;
		free(core->thread_infos);
		core->thread_infos = NULL;
	}
}

static void
_reactors_scheduler_fini(void)
{
	/* Reschedule based on the balancing output */
	_threads_reschedule(g_core_infos);

	g_scheduling_in_progress = false;
}

static void
_reactors_scheduler_update_core_mode(void *ctx)
{
	struct spdk_reactor *reactor;
	uint32_t i;
	int rc = 0;

	for (i = g_scheduler_core_number; i < SPDK_ENV_LCORE_ID_ANY; i = spdk_env_get_next_core(i)) {
		reactor = spdk_reactor_get(i);
		assert(reactor != NULL);
		if (reactor->in_interrupt != g_core_infos[i].interrupt_mode) {
			/* Switch next found reactor to new state */
			rc = spdk_reactor_set_interrupt_mode(i, g_core_infos[i].interrupt_mode,
							     _reactors_scheduler_update_core_mode, NULL);
			if (rc == 0) {
				/* Set core to start with after callback completes */
				g_scheduler_core_number = spdk_env_get_next_core(i);
				return;
			}
		}
	}
	_reactors_scheduler_fini();
}

static void
_reactors_scheduler_cancel(void *arg1, void *arg2)
{
	struct spdk_scheduler_core_info *core;
	uint32_t i;

	SPDK_ENV_FOREACH_CORE(i) {
		core = &g_core_infos[i];
		core->threads_count = 0;
		free(core->thread_infos);
		core->thread_infos = NULL;
	}

	g_scheduling_in_progress = false;
}

static void
_reactors_scheduler_balance(void *arg1, void *arg2)
{
	struct spdk_scheduler *scheduler = spdk_scheduler_get();

	if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING || scheduler == NULL) {
		_reactors_scheduler_cancel(NULL, NULL);
		return;
	}

	scheduler->balance(g_core_infos, g_reactor_count);

	g_scheduler_core_number = spdk_env_get_first_core();
	_reactors_scheduler_update_core_mode(NULL);
}

/* Phase 1 of thread scheduling is to gather metrics on the existing threads */
static void
_reactors_scheduler_gather_metrics(void *arg1, void *arg2)
{
	struct spdk_scheduler_core_info *core_info;
	struct spdk_lw_thread *lw_thread;
	struct spdk_thread *thread;
	struct spdk_reactor *reactor;
	struct spdk_event *evt;
	uint32_t next_core;
	uint32_t i = 0;

	reactor = spdk_reactor_get(spdk_env_get_current_core());
	assert(reactor != NULL);
	core_info = &g_core_infos[reactor->lcore];
	core_info->lcore = reactor->lcore;
	core_info->current_idle_tsc = reactor->idle_tsc - core_info->total_idle_tsc;
	core_info->total_idle_tsc = reactor->idle_tsc;
	core_info->current_busy_tsc = reactor->busy_tsc - core_info->total_busy_tsc;
	core_info->total_busy_tsc = reactor->busy_tsc;
	core_info->interrupt_mode = reactor->in_interrupt;
	core_info->threads_count = 0;

	SPDK_DEBUGLOG(reactor, "Gathering metrics on %u\n", reactor->lcore);

	if (reactor->thread_count > 0) {
		core_info->thread_infos = calloc(reactor->thread_count, sizeof(*core_info->thread_infos));
		if (core_info->thread_infos == NULL) {
			SPDK_ERRLOG("Failed to allocate memory when gathering metrics on %u\n", reactor->lcore);

			/* Cancel this round of schedule work */
			evt = spdk_event_allocate(g_scheduling_reactor->lcore, _reactors_scheduler_cancel, NULL, NULL);
			spdk_event_call(evt);
			return;
		}

		TAILQ_FOREACH(lw_thread, &reactor->threads, link) {
			_init_thread_stats(reactor, lw_thread);

			core_info->thread_infos[i].lcore = lw_thread->lcore;
			thread = spdk_thread_get_from_ctx(lw_thread);
			assert(thread != NULL);
			core_info->thread_infos[i].thread_id = spdk_thread_get_id(thread);
			core_info->thread_infos[i].total_stats = lw_thread->total_stats;
			core_info->thread_infos[i].current_stats = lw_thread->current_stats;
			core_info->threads_count++;
			assert(core_info->threads_count <= reactor->thread_count);
			i++;
		}
	}

	next_core = spdk_env_get_next_core(reactor->lcore);
	if (next_core == UINT32_MAX) {
		next_core = spdk_env_get_first_core();
	}

	/* If we've looped back around to the scheduler thread, move to the next phase */
	if (next_core == g_scheduling_reactor->lcore) {
		/* Phase 2 of scheduling is rebalancing - deciding which threads to move where */
		evt = spdk_event_allocate(next_core, _reactors_scheduler_balance, NULL, NULL);
		spdk_event_call(evt);
		return;
	}

	evt = spdk_event_allocate(next_core, _reactors_scheduler_gather_metrics, NULL, NULL);
	spdk_event_call(evt);
}

static int _reactor_schedule_thread(struct spdk_thread *thread);
static uint64_t g_rusage_period;

static void
_reactor_remove_lw_thread(struct spdk_reactor *reactor, struct spdk_lw_thread *lw_thread)
{
	struct spdk_thread	*thread = spdk_thread_get_from_ctx(lw_thread);
	int efd;

	TAILQ_REMOVE(&reactor->threads, lw_thread, link);
	assert(reactor->thread_count > 0);
	reactor->thread_count--;

	/* Operate thread intr if running with full interrupt ability */
	if (spdk_interrupt_mode_is_enabled()) {
		efd = spdk_thread_get_interrupt_fd(thread);
		spdk_fd_group_remove(reactor->fgrp, efd);
	}
}

static bool
reactor_post_process_lw_thread(struct spdk_reactor *reactor, struct spdk_lw_thread *lw_thread)
{
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);

	if (spdk_unlikely(lw_thread->resched)) {
		lw_thread->resched = false;
		_reactor_remove_lw_thread(reactor, lw_thread);
		_reactor_schedule_thread(thread);
		return true;
	}

	if (spdk_unlikely(spdk_thread_is_exited(thread) &&
			  spdk_thread_is_idle(thread))) {
		_reactor_remove_lw_thread(reactor, lw_thread);
		spdk_thread_destroy(thread);
		return true;
	}

	return false;
}

static void
reactor_interrupt_run(struct spdk_reactor *reactor)
{
	int block_timeout = -1; /* _EPOLL_WAIT_FOREVER */

	spdk_fd_group_wait(reactor->fgrp, block_timeout);
}

static void
_reactor_run(struct spdk_reactor *reactor)
{
	struct spdk_thread	*thread;
	struct spdk_lw_thread	*lw_thread, *tmp;
	uint64_t		now;
	int			rc;

	event_queue_run_batch(reactor);

	/* If no threads are present on the reactor,
	 * tsc_last gets outdated. Update it to track
	 * thread execution time correctly. */
	if (spdk_unlikely(TAILQ_EMPTY(&reactor->threads))) {
		now = spdk_get_ticks();
		reactor->idle_tsc += now - reactor->tsc_last;
		reactor->tsc_last = now;
		return;
	}

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

		reactor_post_process_lw_thread(reactor, lw_thread);
	}
}

static int
reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;
	struct spdk_thread	*thread;
	struct spdk_lw_thread	*lw_thread, *tmp;
	char			thread_name[32];
	uint64_t		last_sched = 0;

	SPDK_NOTICELOG("Reactor started on core %u\n", reactor->lcore);

	/* Rename the POSIX thread because the reactor is tied to the POSIX
	 * thread in the SPDK event library.
	 */
	snprintf(thread_name, sizeof(thread_name), "reactor_%u", reactor->lcore);
	_set_thread_name(thread_name);

	reactor->tsc_last = spdk_get_ticks();

	while (1) {
		/* Execute interrupt process fn if this reactor currently runs in interrupt state */
		if (spdk_unlikely(reactor->in_interrupt)) {
			reactor_interrupt_run(reactor);
		} else {
			_reactor_run(reactor);
		}

		if (g_framework_context_switch_monitor_enabled) {
			if ((reactor->last_rusage + g_rusage_period) < reactor->tsc_last) {
				get_rusage(reactor);
				reactor->last_rusage = reactor->tsc_last;
			}
		}

		if (spdk_unlikely(g_scheduler_period > 0 &&
				  (reactor->tsc_last - last_sched) > g_scheduler_period &&
				  reactor == g_scheduling_reactor &&
				  !g_scheduling_in_progress)) {
			last_sched = reactor->tsc_last;
			g_scheduling_in_progress = true;
			_reactors_scheduler_gather_metrics(NULL, NULL);
		}

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
				_reactor_remove_lw_thread(reactor, lw_thread);
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
	const struct spdk_cpuset *validmask;

	ret = spdk_cpuset_parse(cpumask, mask);
	if (ret < 0) {
		return ret;
	}

	validmask = spdk_app_get_core_mask();
	spdk_cpuset_and(cpumask, validmask);

	return 0;
}

const struct spdk_cpuset *
spdk_app_get_core_mask(void)
{
	return &g_reactor_core_mask;
}

void
spdk_reactors_start(void)
{
	struct spdk_reactor *reactor;
	uint32_t i, current_core;
	int rc;

	g_rusage_period = (CONTEXT_SWITCH_MONITOR_PERIOD * spdk_get_ticks_hz()) / SPDK_SEC_TO_USEC;
	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;
	/* Reinitialize to false, in case the app framework is restarting in the same process. */
	g_stopping_reactors = false;

	current_core = spdk_env_get_current_core();
	SPDK_ENV_FOREACH_CORE(i) {
		if (i != current_core) {
			reactor = spdk_reactor_get(i);
			if (reactor == NULL) {
				continue;
			}

			rc = spdk_env_thread_launch_pinned(reactor->lcore, reactor_run, reactor);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to start reactor thread on core %u\n", reactor->lcore);
				assert(false);
				return;
			}
		}
		spdk_cpuset_set_cpu(&g_reactor_core_mask, i, true);
	}

	/* Start the main reactor */
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);
	reactor_run(reactor);

	spdk_env_thread_wait_all();

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
}

static void
_reactors_stop(void *arg1, void *arg2)
{
	uint32_t i;
	int rc;
	struct spdk_reactor *reactor;
	struct spdk_reactor *local_reactor;
	uint64_t notify = 1;

	g_reactor_state = SPDK_REACTOR_STATE_EXITING;
	local_reactor = spdk_reactor_get(spdk_env_get_current_core());

	SPDK_ENV_FOREACH_CORE(i) {
		/* If spdk_event_call isn't called  on a reactor, always send a notification.
		 * If it is called on a reactor, send a notification if the destination reactor
		 * is indicated in interrupt mode state.
		 */
		if (local_reactor == NULL || spdk_cpuset_get_cpu(&local_reactor->notify_cpuset, i)) {
			reactor = spdk_reactor_get(i);
			assert(reactor != NULL);
			rc = write(reactor->events_fd, &notify, sizeof(notify));
			if (rc < 0) {
				SPDK_ERRLOG("failed to notify event queue for reactor(%u): %s.\n", i, spdk_strerror(errno));
				continue;
			}
		}
	}
}

static void
nop(void *arg1, void *arg2)
{
}

void
spdk_reactors_stop(void *arg1)
{
	spdk_for_each_reactor(nop, NULL, NULL, _reactors_stop);
}

static pthread_mutex_t g_scheduler_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_core = UINT32_MAX;

static int
thread_process_interrupts(void *arg)
{
	struct spdk_thread *thread = arg;
	struct spdk_reactor *reactor = spdk_reactor_get(spdk_env_get_current_core());
	uint64_t now;
	int rc;

	assert(reactor != NULL);

	/* Update idle_tsc between the end of last intr_fn and the start of this intr_fn. */
	now = spdk_get_ticks();
	reactor->idle_tsc += now - reactor->tsc_last;
	reactor->tsc_last = now;

	rc = spdk_thread_poll(thread, 0, now);

	/* Update tsc between the start and the end of this intr_fn. */
	now = spdk_thread_get_last_tsc(thread);
	if (rc == 0) {
		reactor->idle_tsc += now - reactor->tsc_last;
	} else if (rc > 0) {
		reactor->busy_tsc += now - reactor->tsc_last;
	}
	reactor->tsc_last = now;

	return rc;
}

static void
_schedule_thread(void *arg1, void *arg2)
{
	struct spdk_lw_thread *lw_thread = arg1;
	struct spdk_thread *thread;
	struct spdk_reactor *reactor;
	uint32_t current_core;
	int efd;

	current_core = spdk_env_get_current_core();
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);

	/* Update total_stats to reflect state of thread
	* at the end of the move. */
	thread = spdk_thread_get_from_ctx(lw_thread);
	spdk_set_thread(thread);
	spdk_thread_get_stats(&lw_thread->total_stats);
	spdk_set_thread(NULL);

	lw_thread->lcore = current_core;

	TAILQ_INSERT_TAIL(&reactor->threads, lw_thread, link);
	reactor->thread_count++;

	/* Operate thread intr if running with full interrupt ability */
	if (spdk_interrupt_mode_is_enabled()) {
		int rc;

		efd = spdk_thread_get_interrupt_fd(thread);
		rc = SPDK_FD_GROUP_ADD(reactor->fgrp, efd,
				       thread_process_interrupts, thread);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to schedule spdk_thread: %s.\n", spdk_strerror(-rc));
		}

		/* Align spdk_thread with reactor to interrupt mode or poll mode */
		spdk_thread_send_msg(thread, _reactor_set_thread_interrupt_mode, reactor);
	}
}

static int
_reactor_schedule_thread(struct spdk_thread *thread)
{
	uint32_t core;
	struct spdk_lw_thread *lw_thread;
	struct spdk_event *evt = NULL;
	struct spdk_cpuset *cpumask;
	uint32_t i;
	struct spdk_reactor *local_reactor = NULL;
	uint32_t current_lcore = spdk_env_get_current_core();
	struct spdk_cpuset polling_cpumask;
	struct spdk_cpuset valid_cpumask;

	cpumask = spdk_thread_get_cpumask(thread);

	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);
	core = lw_thread->lcore;
	memset(lw_thread, 0, sizeof(*lw_thread));

	if (current_lcore != SPDK_ENV_LCORE_ID_ANY) {
		local_reactor = spdk_reactor_get(current_lcore);
		assert(local_reactor);
	}

	/* When interrupt ability of spdk_thread is not enabled and the current
	 * reactor runs on DPDK thread, skip reactors which are in interrupt mode.
	 */
	if (!spdk_interrupt_mode_is_enabled() && local_reactor != NULL) {
		/* Get the cpumask of all reactors in polling */
		spdk_cpuset_zero(&polling_cpumask);
		SPDK_ENV_FOREACH_CORE(i) {
			spdk_cpuset_set_cpu(&polling_cpumask, i, true);
		}
		spdk_cpuset_xor(&polling_cpumask, &local_reactor->notify_cpuset);

		if (core == SPDK_ENV_LCORE_ID_ANY) {
			/* Get the cpumask of all valid reactors which are suggested and also in polling */
			spdk_cpuset_copy(&valid_cpumask, &polling_cpumask);
			spdk_cpuset_and(&valid_cpumask, spdk_thread_get_cpumask(thread));

			/* If there are any valid reactors, spdk_thread should be scheduled
			 * into one of the valid reactors.
			 * If there is no valid reactors, spdk_thread should be scheduled
			 * into one of the polling reactors.
			 */
			if (spdk_cpuset_count(&valid_cpumask) != 0) {
				cpumask = &valid_cpumask;
			} else {
				cpumask = &polling_cpumask;
			}
		} else if (!spdk_cpuset_get_cpu(&polling_cpumask, core)) {
			/* If specified reactor is not in polling, spdk_thread should be scheduled
			 * into one of the polling reactors.
			 */
			core = SPDK_ENV_LCORE_ID_ANY;
			cpumask = &polling_cpumask;
		}
	}

	pthread_mutex_lock(&g_scheduler_mtx);
	if (core == SPDK_ENV_LCORE_ID_ANY) {
		for (i = 0; i < spdk_env_get_core_count(); i++) {
			if (g_next_core >= g_reactor_count) {
				g_next_core = spdk_env_get_first_core();
			}
			core = g_next_core;
			g_next_core = spdk_env_get_next_core(g_next_core);

			if (spdk_cpuset_get_cpu(cpumask, core)) {
				break;
			}
		}
	}

	evt = spdk_event_allocate(core, _schedule_thread, lw_thread, NULL);

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
	struct spdk_reactor *reactor;
	uint32_t current_core;

	assert(thread == spdk_get_thread());

	lw_thread = spdk_thread_get_ctx(thread);

	assert(lw_thread != NULL);
	lw_thread->resched = true;
	lw_thread->lcore = SPDK_ENV_LCORE_ID_ANY;

	current_core = spdk_env_get_current_core();
	reactor = spdk_reactor_get(current_core);
	assert(reactor != NULL);

	/* Send a notification if the destination reactor is indicated in intr mode state */
	if (spdk_unlikely(spdk_cpuset_get_cpu(&reactor->notify_cpuset, reactor->lcore))) {
		uint64_t notify = 1;

		if (write(reactor->resched_fd, &notify, sizeof(notify)) < 0) {
			SPDK_ERRLOG("failed to notify reschedule: %s.\n", spdk_strerror(errno));
		}
	}
}

static int
reactor_thread_op(struct spdk_thread *thread, enum spdk_thread_op op)
{
	struct spdk_lw_thread *lw_thread;

	switch (op) {
	case SPDK_THREAD_OP_NEW:
		lw_thread = spdk_thread_get_ctx(thread);
		lw_thread->lcore = SPDK_ENV_LCORE_ID_ANY;
		return _reactor_schedule_thread(thread);
	case SPDK_THREAD_OP_RESCHED:
		_reactor_request_thread_reschedule(thread);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static bool
reactor_thread_op_supported(enum spdk_thread_op op)
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
on_reactor(void *arg1, void *arg2)
{
	struct call_reactor *cr = arg1;
	struct spdk_event *evt;

	cr->fn(cr->arg1, cr->arg2);

	cr->cur_core = spdk_env_get_next_core(cr->cur_core);

	if (cr->cur_core >= g_reactor_count) {
		SPDK_DEBUGLOG(reactor, "Completed reactor iteration\n");

		evt = spdk_event_allocate(cr->orig_core, cr->cpl, cr->arg1, cr->arg2);
		free(cr);
	} else {
		SPDK_DEBUGLOG(reactor, "Continuing reactor iteration to %d\n",
			      cr->cur_core);

		evt = spdk_event_allocate(cr->cur_core, on_reactor, arg1, NULL);
	}
	assert(evt != NULL);
	spdk_event_call(evt);
}

void
spdk_for_each_reactor(spdk_event_fn fn, void *arg1, void *arg2, spdk_event_fn cpl)
{
	struct call_reactor *cr;
	struct spdk_event *evt;

	/* When the application framework is shutting down, we will send one
	 * final for_each_reactor operation with completion callback _reactors_stop,
	 * to flush any existing for_each_reactor operations to avoid any memory
	 * leaks. We use a mutex here to protect a boolean flag that will ensure
	 * we don't start any more operations once we've started shutting down.
	 */
	pthread_mutex_lock(&g_stopping_reactors_mtx);
	if (g_stopping_reactors) {
		pthread_mutex_unlock(&g_stopping_reactors_mtx);
		return;
	} else if (cpl == _reactors_stop) {
		g_stopping_reactors = true;
	}
	pthread_mutex_unlock(&g_stopping_reactors_mtx);

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

	SPDK_DEBUGLOG(reactor, "Starting reactor iteration from %d\n", cr->orig_core);

	evt = spdk_event_allocate(cr->cur_core, on_reactor, cr, NULL);
	assert(evt != NULL);

	spdk_event_call(evt);
}

#ifdef __linux__
static int
reactor_schedule_thread_event(void *arg)
{
	struct spdk_reactor *reactor = arg;
	struct spdk_lw_thread *lw_thread, *tmp;
	uint32_t count = 0;
	uint64_t notify = 1;

	assert(reactor->in_interrupt);

	if (read(reactor->resched_fd, &notify, sizeof(notify)) < 0) {
		SPDK_ERRLOG("failed to acknowledge reschedule: %s.\n", spdk_strerror(errno));
		return -errno;
	}

	TAILQ_FOREACH_SAFE(lw_thread, &reactor->threads, link, tmp) {
		count += reactor_post_process_lw_thread(reactor, lw_thread) ? 1 : 0;
	}

	return count;
}

static int
reactor_interrupt_init(struct spdk_reactor *reactor)
{
	int rc;

	rc = spdk_fd_group_create(&reactor->fgrp);
	if (rc != 0) {
		return rc;
	}

	reactor->resched_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (reactor->resched_fd < 0) {
		rc = -EBADF;
		goto err;
	}

	rc = SPDK_FD_GROUP_ADD(reactor->fgrp, reactor->resched_fd, reactor_schedule_thread_event,
			       reactor);
	if (rc) {
		close(reactor->resched_fd);
		goto err;
	}

	reactor->events_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (reactor->events_fd < 0) {
		spdk_fd_group_remove(reactor->fgrp, reactor->resched_fd);
		close(reactor->resched_fd);

		rc = -EBADF;
		goto err;
	}

	rc = SPDK_FD_GROUP_ADD(reactor->fgrp, reactor->events_fd,
			       event_queue_run_batch, reactor);
	if (rc) {
		spdk_fd_group_remove(reactor->fgrp, reactor->resched_fd);
		close(reactor->resched_fd);
		close(reactor->events_fd);
		goto err;
	}

	return 0;

err:
	spdk_fd_group_destroy(reactor->fgrp);
	reactor->fgrp = NULL;
	return rc;
}
#else
static int
reactor_interrupt_init(struct spdk_reactor *reactor)
{
	return -ENOTSUP;
}
#endif

static void
reactor_interrupt_fini(struct spdk_reactor *reactor)
{
	struct spdk_fd_group *fgrp = reactor->fgrp;

	if (!fgrp) {
		return;
	}

	spdk_fd_group_remove(fgrp, reactor->events_fd);
	spdk_fd_group_remove(fgrp, reactor->resched_fd);

	close(reactor->events_fd);
	close(reactor->resched_fd);

	spdk_fd_group_destroy(fgrp);
	reactor->fgrp = NULL;
}

static struct spdk_governor *
_governor_find(const char *name)
{
	struct spdk_governor *governor, *tmp;

	TAILQ_FOREACH_SAFE(governor, &g_governor_list, link, tmp) {
		if (strcmp(name, governor->name) == 0) {
			return governor;
		}
	}

	return NULL;
}

int
spdk_governor_set(const char *name)
{
	struct spdk_governor *governor;
	int rc = 0;

	/* NULL governor was specifically requested */
	if (name == NULL) {
		if (g_governor) {
			g_governor->deinit();
		}
		g_governor = NULL;
		return 0;
	}

	governor = _governor_find(name);
	if (governor == NULL) {
		return -EINVAL;
	}

	if (g_governor == governor) {
		return 0;
	}

	rc = governor->init();
	if (rc == 0) {
		if (g_governor) {
			g_governor->deinit();
		}
		g_governor = governor;
	}

	return rc;
}

struct spdk_governor *
spdk_governor_get(void)
{
	return g_governor;
}

void
spdk_governor_register(struct spdk_governor *governor)
{
	if (_governor_find(governor->name)) {
		SPDK_ERRLOG("governor named '%s' already registered.\n", governor->name);
		assert(false);
		return;
	}

	TAILQ_INSERT_TAIL(&g_governor_list, governor, link);
}

SPDK_LOG_REGISTER_COMPONENT(reactor)
