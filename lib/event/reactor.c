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

#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h"

#define SPDK_MAX_SOCKET		64

#define SPDK_EVENT_BATCH_SIZE		8

enum spdk_poller_state {
	/* The poller is registered with a reactor but not currently executing its fn. */
	SPDK_POLLER_STATE_WAITING,

	/* The poller is currently running its fn. */
	SPDK_POLLER_STATE_RUNNING,

	/* The poller was unregistered during the execution of its fn. */
	SPDK_POLLER_STATE_UNREGISTERED,
};

struct spdk_poller {
	TAILQ_ENTRY(spdk_poller)	tailq;
	uint32_t			lcore;

	/* Current state of the poller; should only be accessed from the poller's thread. */
	enum spdk_poller_state		state;

	uint64_t			period_ticks;
	uint64_t			next_run_tick;
	spdk_poller_fn			fn;
	void				*arg;
};

enum spdk_reactor_state {
	SPDK_REACTOR_STATE_INVALID = 0,
	SPDK_REACTOR_STATE_INITIALIZED = 1,
	SPDK_REACTOR_STATE_RUNNING = 2,
	SPDK_REACTOR_STATE_EXITING = 3,
	SPDK_REACTOR_STATE_SHUTDOWN = 4,
};

struct spdk_reactor {
	/* Logical core number for this reactor. */
	uint32_t					lcore;

	/* Socket ID for this reactor. */
	uint32_t					socket_id;

	/* Poller for get the rusage for the reactor. */
	struct spdk_poller				*rusage_poller;

	/* Reactor tsc stats */
	struct spdk_reactor_tsc_stats			tsc_stats;

	uint64_t					tsc_last;

	/* The last known rusage values */
	struct rusage					rusage;

	/*
	 * Contains pollers actively running on this reactor.  Pollers
	 *  are run round-robin. The reactor takes one poller from the head
	 *  of the ring, executes it, then puts it back at the tail of
	 *  the ring.
	 */
	TAILQ_HEAD(, spdk_poller)			active_pollers;

	/**
	 * Contains pollers running on this reactor with a periodic timer.
	 */
	TAILQ_HEAD(timer_pollers_head, spdk_poller)	timer_pollers;

	struct spdk_ring				*events;

	/* Pointer to the per-socket g_spdk_event_mempool for this reactor. */
	struct spdk_mempool				*event_mempool;

	uint64_t					max_delay_us;
} __attribute__((aligned(64)));

static struct spdk_reactor *g_reactors;

static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_INVALID;

static bool g_context_switch_monitor_enabled = true;

static void spdk_reactor_construct(struct spdk_reactor *w, uint32_t lcore,
				   uint64_t max_delay_us);

static struct spdk_mempool *g_spdk_event_mempool[SPDK_MAX_SOCKET];

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

	event = spdk_mempool_get(reactor->event_mempool);
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
_spdk_event_queue_run_batch(struct spdk_reactor *reactor)
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

	for (i = 0; i < count; i++) {
		struct spdk_event *event = events[i];

		assert(event != NULL);
		event->fn(event->arg1, event->arg2);
	}

	spdk_mempool_put_bulk(reactor->event_mempool, events, count);

	return count;
}

static void
_spdk_reactor_msg_passed(void *arg1, void *arg2)
{
	spdk_thread_fn fn = arg1;

	fn(arg2);
}

static void
_spdk_reactor_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	struct spdk_event *event;
	struct spdk_reactor *reactor;

	reactor = thread_ctx;

	event = spdk_event_allocate(reactor->lcore, _spdk_reactor_msg_passed, fn, ctx);

	spdk_event_call(event);
}

static void
_spdk_poller_insert_timer(struct spdk_reactor *reactor, struct spdk_poller *poller, uint64_t now)
{
	struct spdk_poller *iter;
	uint64_t next_run_tick;

	next_run_tick = now + poller->period_ticks;
	poller->next_run_tick = next_run_tick;

	/*
	 * Insert poller in the reactor's timer_pollers list in sorted order by next scheduled
	 * run time.
	 */
	TAILQ_FOREACH_REVERSE(iter, &reactor->timer_pollers, timer_pollers_head, tailq) {
		if (iter->next_run_tick <= next_run_tick) {
			TAILQ_INSERT_AFTER(&reactor->timer_pollers, iter, poller, tailq);
			return;
		}
	}

	/* No earlier pollers were found, so this poller must be the new head */
	TAILQ_INSERT_HEAD(&reactor->timer_pollers, poller, tailq);
}

static struct spdk_poller *
_spdk_reactor_start_poller(void *thread_ctx,
			   spdk_poller_fn fn,
			   void *arg,
			   uint64_t period_microseconds)
{
	struct spdk_poller *poller;
	struct spdk_reactor *reactor;
	uint64_t quotient, remainder, ticks;

	reactor = thread_ctx;

	poller = calloc(1, sizeof(*poller));
	if (poller == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return NULL;
	}

	poller->lcore = reactor->lcore;
	poller->state = SPDK_POLLER_STATE_WAITING;
	poller->fn = fn;
	poller->arg = arg;

	if (period_microseconds) {
		quotient = period_microseconds / SPDK_SEC_TO_USEC;
		remainder = period_microseconds % SPDK_SEC_TO_USEC;
		ticks = spdk_get_ticks_hz();

		poller->period_ticks = ticks * quotient + (ticks * remainder) / SPDK_SEC_TO_USEC;
	} else {
		poller->period_ticks = 0;
	}

	if (poller->period_ticks) {
		_spdk_poller_insert_timer(reactor, poller, spdk_get_ticks());
	} else {
		TAILQ_INSERT_TAIL(&reactor->active_pollers, poller, tailq);
	}

	return poller;
}

static void
_spdk_reactor_stop_poller(struct spdk_poller *poller, void *thread_ctx)
{
	struct spdk_reactor *reactor;

	reactor = thread_ctx;

	assert(poller->lcore == spdk_env_get_current_core());

	if (poller->state == SPDK_POLLER_STATE_RUNNING) {
		/*
		 * We are being called from the poller_fn, so set the state to unregistered
		 * and let the reactor loop free the poller.
		 */
		poller->state = SPDK_POLLER_STATE_UNREGISTERED;
	} else {
		/* Poller is not running currently, so just free it. */
		if (poller->period_ticks) {
			TAILQ_REMOVE(&reactor->timer_pollers, poller, tailq);
		} else {
			TAILQ_REMOVE(&reactor->active_pollers, poller, tailq);
		}

		free(poller);
	}
}

static int
get_rusage(void *arg)
{
	struct spdk_reactor	*reactor = arg;
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

static void
_spdk_reactor_context_switch_monitor_start(void *arg1, void *arg2)
{
	struct spdk_reactor *reactor = arg1;

	if (reactor->rusage_poller == NULL) {
		getrusage(RUSAGE_THREAD, &reactor->rusage);
		reactor->rusage_poller = spdk_poller_register(get_rusage, reactor, 1000000);
	}
}

static void
_spdk_reactor_context_switch_monitor_stop(void *arg1, void *arg2)
{
	struct spdk_reactor *reactor = arg1;

	if (reactor->rusage_poller != NULL) {
		spdk_poller_unregister(&reactor->rusage_poller);
	}
}

static size_t
_spdk_reactor_get_max_event_cnt(uint8_t socket_count)
{
	size_t cnt;

	/* Try to make event ring fill at most 2MB of memory,
	 * as some ring implementations may require physical address
	 * contingency. We don't want to introduce a requirement of
	 * at least 2 physically contiguous 2MB hugepages.
	 */
	cnt = spdk_min(262144 / socket_count, 262144 / 2);
	/* Take into account one extra element required by
	 * some ring implementations.
	 */
	cnt -= 1;
	return cnt;
}

void
spdk_reactor_enable_context_switch_monitor(bool enable)
{
	struct spdk_reactor *reactor;
	spdk_event_fn fn;
	uint32_t core;

	if (enable != g_context_switch_monitor_enabled) {
		g_context_switch_monitor_enabled = enable;
		if (enable) {
			fn = _spdk_reactor_context_switch_monitor_start;
		} else {
			fn = _spdk_reactor_context_switch_monitor_stop;
		}
		SPDK_ENV_FOREACH_CORE(core) {
			reactor = spdk_reactor_get(core);
			spdk_event_call(spdk_event_allocate(core, fn, reactor, NULL));
		}
	}
}

bool
spdk_reactor_context_switch_monitor_enabled(void)
{
	return g_context_switch_monitor_enabled;
}

static void
spdk_reactor_add_tsc_stats(void *arg, int rc, uint64_t now)
{
	struct spdk_reactor *reactor = arg;
	struct spdk_reactor_tsc_stats *tsc_stats = &reactor->tsc_stats;

	if (rc == 0) {
		/* Poller status idle */
		tsc_stats->idle_tsc += now - reactor->tsc_last;
	} else if (rc > 0) {
		/* Poller status busy */
		tsc_stats->busy_tsc += now - reactor->tsc_last;
	} else {
		/* Poller status unknown */
		tsc_stats->unknown_tsc += now - reactor->tsc_last;
	}

	reactor->tsc_last = now;
}

int
spdk_reactor_get_tsc_stats(struct spdk_reactor_tsc_stats *tsc_stats, uint32_t core)
{
	struct spdk_reactor *reactor;

	if (!spdk_cpuset_get_cpu(g_spdk_app_core_mask, core)) {
		return -1;
	}

	reactor = spdk_reactor_get(core);
	*tsc_stats = reactor->tsc_stats;

	return 0;
}

/**
 *
 * \brief This is the main function of the reactor thread.
 *
 * \code
 *
 * while (1)
 *	if (events to run)
 *		dequeue and run a batch of events
 *
 *	if (active pollers)
 *		run the first poller in the list and move it to the back
 *
 *	if (first timer poller has expired)
 *		run the first timer poller and reinsert it in the timer list
 *
 *	if (no action taken and sleep enabled)
 *		sleep until next timer poller is scheduled to expire
 * \endcode
 *
 */
static int
_spdk_reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;
	struct spdk_poller	*poller;
	uint32_t		event_count;
	uint64_t		now;
	uint64_t		sleep_cycles;
	uint32_t		sleep_us;
	int			rc = -1;
	char			thread_name[32];

	snprintf(thread_name, sizeof(thread_name), "reactor_%u", reactor->lcore);
	if (spdk_allocate_thread(_spdk_reactor_send_msg,
				 _spdk_reactor_start_poller,
				 _spdk_reactor_stop_poller,
				 reactor, thread_name) == NULL) {
		return -1;
	}
	SPDK_NOTICELOG("Reactor started on core %u on socket %u\n", reactor->lcore,
		       reactor->socket_id);

	sleep_cycles = reactor->max_delay_us * spdk_get_ticks_hz() / SPDK_SEC_TO_USEC;
	if (g_context_switch_monitor_enabled) {
		_spdk_reactor_context_switch_monitor_start(reactor, NULL);
	}
	now = spdk_get_ticks();
	reactor->tsc_last = now;

	while (1) {
		bool took_action = false;

		event_count = _spdk_event_queue_run_batch(reactor);
		if (event_count > 0) {
			rc = 1;
			now = spdk_get_ticks();
			spdk_reactor_add_tsc_stats(reactor, rc, now);
			took_action = true;
		}

		poller = TAILQ_FIRST(&reactor->active_pollers);
		if (poller) {
			TAILQ_REMOVE(&reactor->active_pollers, poller, tailq);
			poller->state = SPDK_POLLER_STATE_RUNNING;
			rc = poller->fn(poller->arg);
			now = spdk_get_ticks();
			spdk_reactor_add_tsc_stats(reactor, rc, now);
			if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
				free(poller);
			} else {
				poller->state = SPDK_POLLER_STATE_WAITING;
				TAILQ_INSERT_TAIL(&reactor->active_pollers, poller, tailq);
			}
			took_action = true;
		}

		poller = TAILQ_FIRST(&reactor->timer_pollers);
		if (poller) {
			if (took_action == false) {
				now = spdk_get_ticks();
			}

			if (now >= poller->next_run_tick) {
				uint64_t tmp_timer_tsc;

				TAILQ_REMOVE(&reactor->timer_pollers, poller, tailq);
				poller->state = SPDK_POLLER_STATE_RUNNING;
				rc = poller->fn(poller->arg);
				/* Save the tsc value from before poller->fn was executed. We want to
				 * use the current time for idle/busy tsc value accounting, but want to
				 * use the older time to reinsert to the timer poller below. */
				tmp_timer_tsc = now;
				now = spdk_get_ticks();
				spdk_reactor_add_tsc_stats(reactor, rc, now);
				if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
					free(poller);
				} else {
					poller->state = SPDK_POLLER_STATE_WAITING;
					_spdk_poller_insert_timer(reactor, poller, tmp_timer_tsc);
				}
				took_action = true;
			}
		}

		/* Determine if the thread can sleep */
		if (sleep_cycles && !took_action) {
			now = spdk_get_ticks();
			sleep_us = reactor->max_delay_us;

			poller = TAILQ_FIRST(&reactor->timer_pollers);
			if (poller) {
				/* There are timers registered, so don't sleep beyond
				 * when the next timer should fire */
				if (poller->next_run_tick < (now + sleep_cycles)) {
					if (poller->next_run_tick <= now) {
						sleep_us = 0;
					} else {
						sleep_us = ((poller->next_run_tick - now) *
							    SPDK_SEC_TO_USEC) / spdk_get_ticks_hz();
					}
				}
			}

			if (sleep_us > 0) {
				usleep(sleep_us);
			}
		}

		if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {
			break;
		}
	}

	_spdk_reactor_context_switch_monitor_stop(reactor, NULL);
	spdk_free_thread();
	return 0;
}

static void
spdk_reactor_construct(struct spdk_reactor *reactor, uint32_t lcore, uint64_t max_delay_us)
{
	reactor->lcore = lcore;
	reactor->socket_id = spdk_env_get_socket_id(lcore);
	assert(reactor->socket_id < SPDK_MAX_SOCKET);
	reactor->max_delay_us = max_delay_us;

	TAILQ_INIT(&reactor->active_pollers);
	TAILQ_INIT(&reactor->timer_pollers);

	reactor->events = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, reactor->socket_id);
	if (!reactor->events) {
		SPDK_NOTICELOG("Ring creation failed on preferred socket %d. Try other sockets.\n",
			       reactor->socket_id);

		reactor->events = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536,
						   SPDK_ENV_SOCKET_ID_ANY);
	}
	assert(reactor->events != NULL);

	reactor->event_mempool = g_spdk_event_mempool[reactor->socket_id];
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


static uint64_t
spdk_reactor_get_socket_mask(void)
{
	uint32_t i;
	uint32_t socket_id;
	uint64_t socket_info = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		socket_id = spdk_env_get_socket_id(i);
		socket_info |= (1ULL << socket_id);
	}

	return socket_info;
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
	uint32_t i, j, last_core;
	struct spdk_reactor *reactor;
	uint64_t socket_mask = 0x0;
	uint8_t socket_count = 0;
	char mempool_name[32];

	socket_mask = spdk_reactor_get_socket_mask();
	SPDK_NOTICELOG("Occupied cpu socket mask is 0x%lx\n", socket_mask);

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			socket_count++;
		}
	}
	if (socket_count == 0) {
		SPDK_ERRLOG("No sockets occupied (internal error)\n");
		return -1;
	}

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			snprintf(mempool_name, sizeof(mempool_name), "evtpool%d_%d", i, getpid());
			g_spdk_event_mempool[i] = spdk_mempool_create(mempool_name,
						  _spdk_reactor_get_max_event_cnt(socket_count),
						  sizeof(struct spdk_event),
						  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE, i);

			if (g_spdk_event_mempool[i] == NULL) {
				SPDK_NOTICELOG("Event_mempool creation failed on preferred socket %d.\n", i);

				/*
				 * Instead of failing the operation directly, try to create
				 * the mempool on any available sockets in the case that
				 * memory is not evenly installed on all sockets. If still
				 * fails, free all allocated memory and exits.
				 */
				g_spdk_event_mempool[i] = spdk_mempool_create(
								  mempool_name,
								  _spdk_reactor_get_max_event_cnt(socket_count),
								  sizeof(struct spdk_event),
								  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
								  SPDK_ENV_SOCKET_ID_ANY);

				if (g_spdk_event_mempool[i] == NULL) {
					for (j = i - 1; j < i; j--) {
						if (g_spdk_event_mempool[j] != NULL) {
							spdk_mempool_free(g_spdk_event_mempool[j]);
						}
					}
					SPDK_ERRLOG("spdk_event_mempool creation failed\n");
					return -1;
				}
			}
		} else {
			g_spdk_event_mempool[i] = NULL;
		}
	}

	/* struct spdk_reactor must be aligned on 64 byte boundary */
	last_core = spdk_env_get_last_core();
	rc = posix_memalign((void **)&g_reactors, 64,
			    (last_core + 1) * sizeof(struct spdk_reactor));
	if (rc != 0) {
		SPDK_ERRLOG("Could not allocate array size=%u for g_reactors\n",
			    last_core + 1);
		for (i = 0; i < SPDK_MAX_SOCKET; i++) {
			if (g_spdk_event_mempool[i] != NULL) {
				spdk_mempool_free(g_spdk_event_mempool[i]);
			}
		}
		return -1;
	}

	memset(g_reactors, 0, (last_core + 1) * sizeof(struct spdk_reactor));

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

	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		if (spdk_likely(reactor != NULL) && reactor->events != NULL) {
			spdk_ring_free(reactor->events);
		}
	}

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if (g_spdk_event_mempool[i] != NULL) {
			spdk_mempool_free(g_spdk_event_mempool[i]);
		}
	}

	free(g_reactors);
	g_reactors = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("reactor", SPDK_LOG_REACTOR)
