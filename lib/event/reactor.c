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

#include "spdk_internal/event.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#include <rte_config.h>
#include <rte_launch.h>
#include <rte_lcore.h>

#include "spdk/log.h"
#include "spdk/io_channel.h"
#include "spdk/env.h"

#define SPDK_MAX_SOCKET		64

#define SPDK_REACTOR_SPIN_TIME_US	1000
#define SPDK_TIMER_POLL_ITERATIONS	5
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

	struct spdk_event		*unregister_complete_event;
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

static struct spdk_reactor g_reactors[RTE_MAX_LCORE];

static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_INVALID;

static void spdk_reactor_construct(struct spdk_reactor *w, uint32_t lcore,
				   uint64_t max_delay_us);

static struct spdk_mempool *g_spdk_event_mempool[SPDK_MAX_SOCKET];

static struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;
	reactor = &g_reactors[lcore];
	return reactor;
}

struct spdk_event *
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2)
{
	struct spdk_event *event = NULL;
	struct spdk_reactor *reactor = spdk_reactor_get(lcore);

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

uint32_t
spdk_event_queue_run_batch(uint32_t lcore)
{
	return _spdk_event_queue_run_batch(spdk_reactor_get(lcore));
}

/**
 *
 * \brief Set current reactor thread name to "reactor <cpu #>".
 *
 * This makes the reactor threads distinguishable in top and gdb.
 */
static void set_reactor_thread_name(uint32_t lcore)
{
	char thread_name[16];

	snprintf(thread_name, sizeof(thread_name), "reactor_%u", lcore);

#if defined(__linux__)
	prctl(PR_SET_NAME, thread_name, 0, 0, 0);
#elif defined(__FreeBSD__)
	pthread_set_name_np(pthread_self(), thread_name);
#else
#error missing platform support for thread name
#endif
}

static void
spdk_poller_insert_timer(struct spdk_reactor *reactor, struct spdk_poller *poller, uint64_t now)
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

static void
_spdk_poller_unregister_complete(struct spdk_poller *poller)
{
	if (poller->unregister_complete_event) {
		spdk_event_call(poller->unregister_complete_event);
	}

	free(poller);
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
 *	if (idle for at least SPDK_REACTOR_SPIN_TIME_US)
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
	uint64_t		idle_started, now;
	uint64_t		spin_cycles, sleep_cycles;
	uint32_t		sleep_us;
	uint32_t 		timer_poll_count;

	spdk_allocate_thread();
	set_reactor_thread_name(reactor->lcore);
	SPDK_NOTICELOG("Reactor started on core %u on socket %u\n", reactor->lcore,
		       reactor->socket_id);

	spin_cycles = SPDK_REACTOR_SPIN_TIME_US * spdk_get_ticks_hz() / 1000000ULL;
	sleep_cycles = reactor->max_delay_us * spdk_get_ticks_hz() / 1000000ULL;
	idle_started = 0;
	timer_poll_count = 0;

	while (1) {
		bool took_action = false;

		event_count = _spdk_event_queue_run_batch(reactor);
		if (event_count > 0) {
			took_action = true;
		}

		poller = TAILQ_FIRST(&reactor->active_pollers);
		if (poller) {
			TAILQ_REMOVE(&reactor->active_pollers, poller, tailq);
			poller->state = SPDK_POLLER_STATE_RUNNING;
			poller->fn(poller->arg);
			if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
				_spdk_poller_unregister_complete(poller);
			} else {
				poller->state = SPDK_POLLER_STATE_WAITING;
				TAILQ_INSERT_TAIL(&reactor->active_pollers, poller, tailq);
			}
			took_action = true;
		}

		if (timer_poll_count >= SPDK_TIMER_POLL_ITERATIONS) {
			poller = TAILQ_FIRST(&reactor->timer_pollers);
			if (poller) {
				now = spdk_get_ticks();

				if (now >= poller->next_run_tick) {
					TAILQ_REMOVE(&reactor->timer_pollers, poller, tailq);
					poller->state = SPDK_POLLER_STATE_RUNNING;
					poller->fn(poller->arg);
					if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
						_spdk_poller_unregister_complete(poller);
					} else {
						poller->state = SPDK_POLLER_STATE_WAITING;
						spdk_poller_insert_timer(reactor, poller, now);
					}
					took_action = true;
				}
			}
			timer_poll_count = 0;
		} else {
			timer_poll_count++;
		}

		if (took_action) {
			/* We were busy this loop iteration. Reset the idle timer. */
			idle_started = 0;
		} else if (idle_started == 0) {
			/* We were previously busy, but this loop we took no actions. */
			idle_started = spdk_get_ticks();
		}

		/* Determine if the thread can sleep */
		if (sleep_cycles && idle_started) {
			now = spdk_get_ticks();
			if (now >= (idle_started + spin_cycles)) {
				sleep_us = reactor->max_delay_us;

				poller = TAILQ_FIRST(&reactor->timer_pollers);
				if (poller) {
					/* There are timers registered, so don't sleep beyond
					 * when the next timer should fire */
					if (poller->next_run_tick < (now + sleep_cycles)) {
						if (poller->next_run_tick <= now) {
							sleep_us = 0;
						} else {
							sleep_us = ((poller->next_run_tick - now) * 1000000ULL) / spdk_get_ticks_hz();
						}
					}
				}

				if (sleep_us > 0) {
					usleep(sleep_us);
				}

				/* After sleeping, always poll for timers */
				timer_poll_count = SPDK_TIMER_POLL_ITERATIONS;
			}
		}

		if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {
			break;
		}
	}

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

	reactor->events = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536,
					   sizeof(struct event *),
					   reactor->socket_id);
	assert(reactor->events != NULL);

	reactor->event_mempool = g_spdk_event_mempool[reactor->socket_id];
}

static void
spdk_reactor_start(struct spdk_reactor *reactor)
{
	if (reactor->lcore != rte_get_master_lcore()) {
		switch (rte_eal_get_lcore_state(reactor->lcore)) {
		case FINISHED:
			rte_eal_wait_lcore(reactor->lcore);
		/* drop through */
		case WAIT:
			rte_eal_remote_launch(_spdk_reactor_run, (void *)reactor, reactor->lcore);
			break;
		case RUNNING:
			printf("Something already running on lcore %d\n", reactor->lcore);
			break;
		}
	} else {
		_spdk_reactor_run(reactor);
	}
}

int
spdk_app_get_core_count(void)
{
	return spdk_env_get_core_count();
}

uint32_t
spdk_app_get_current_core(void)
{
	return spdk_env_get_current_core();
}

int
spdk_app_parse_core_mask(const char *mask, uint64_t *cpumask)
{
	unsigned int i;
	char *end;

	if (mask == NULL || cpumask == NULL) {
		return -1;
	}

	errno = 0;
	*cpumask = strtoull(mask, &end, 16);
	if (*end != '\0' || errno) {
		return -1;
	}

	for (i = 0; i < RTE_MAX_LCORE && i < 64; i++) {
		if ((*cpumask & (1ULL << i)) && !rte_lcore_is_enabled(i)) {
			*cpumask &= ~(1ULL << i);
		}
	}

	return 0;
}

uint64_t
spdk_app_get_core_mask(void)
{
	uint32_t i;
	uint64_t mask = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		mask |= 1ULL << i;
	}

	return mask;
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

	assert(rte_get_master_lcore() == rte_lcore_id());

	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;

	current_core = spdk_env_get_current_core();
	SPDK_ENV_FOREACH_CORE(i) {
		if (i != current_core) {
			reactor = spdk_reactor_get(i);
			spdk_reactor_start(reactor);
		}
	}

	/* Start the master reactor */
	reactor = spdk_reactor_get(current_core);
	spdk_reactor_start(reactor);

	rte_eal_mp_wait_lcore();

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
}

void spdk_reactors_stop(void)
{
	g_reactor_state = SPDK_REACTOR_STATE_EXITING;
}

int
spdk_reactors_init(unsigned int max_delay_us)
{
	uint32_t i, j;
	struct spdk_reactor *reactor;
	uint64_t socket_mask = 0x0;
	uint8_t socket_count = 0;
	char mempool_name[32];

	socket_mask = spdk_reactor_get_socket_mask();
	printf("Occupied cpu socket mask is 0x%lx\n", socket_mask);

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			socket_count++;
		}
	}
	if (socket_count == 0) {
		printf("No sockets occupied (internal error)\n");
		return -1;
	}

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			snprintf(mempool_name, sizeof(mempool_name), "spdk_event_mempool_%d", i);
			g_spdk_event_mempool[i] = spdk_mempool_create(mempool_name,
						  (262144 / socket_count),
						  sizeof(struct spdk_event), -1, i);

			if (g_spdk_event_mempool[i] == NULL) {
				SPDK_ERRLOG("spdk_event_mempool creation failed on socket %d\n", i);

				/*
				 * Instead of failing the operation directly, try to create
				 * the mempool on any available sockets in the case that
				 * memory is not evenly installed on all sockets. If still
				 * fails, free all allocated memory and exits.
				 */
				g_spdk_event_mempool[i] = spdk_mempool_create(
								  mempool_name,
								  (262144 / socket_count),
								  sizeof(struct spdk_event), -1,
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
		}
	}

	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		spdk_reactor_construct(reactor, i, max_delay_us);
	}

	g_reactor_state = SPDK_REACTOR_STATE_INITIALIZED;

	return 0;
}

int
spdk_reactors_fini(void)
{
	uint32_t i;
	uint64_t socket_mask;
	struct spdk_reactor *reactor;

	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		if (reactor->events != NULL) {
			spdk_ring_free(reactor->events);
		}
	}

	socket_mask = spdk_reactor_get_socket_mask();
	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask && g_spdk_event_mempool[i] != NULL) {
			spdk_mempool_free(g_spdk_event_mempool[i]);
		}
	}

	return 0;
}

static void
_spdk_poller_register(struct spdk_reactor *reactor, struct spdk_poller *poller)
{
	if (poller->period_ticks) {
		spdk_poller_insert_timer(reactor, poller, spdk_get_ticks());
	} else {
		TAILQ_INSERT_TAIL(&reactor->active_pollers, poller, tailq);
	}
}

static void
_spdk_event_add_poller(void *arg1, void *arg2)
{
	struct spdk_reactor *reactor = arg1;
	struct spdk_poller *poller = arg2;

	_spdk_poller_register(reactor, poller);
}

void
spdk_poller_register(struct spdk_poller **ppoller, spdk_poller_fn fn, void *arg,
		     uint32_t lcore, uint64_t period_microseconds)
{
	struct spdk_poller *poller;
	struct spdk_reactor *reactor;

	poller = calloc(1, sizeof(*poller));
	if (poller == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		abort();
	}

	poller->lcore = lcore;
	poller->state = SPDK_POLLER_STATE_WAITING;
	poller->fn = fn;
	poller->arg = arg;

	if (period_microseconds) {
		poller->period_ticks = (spdk_get_ticks_hz() * period_microseconds) / 1000000ULL;
	} else {
		poller->period_ticks = 0;
	}

	if (*ppoller != NULL) {
		SPDK_ERRLOG("Attempted reuse of poller pointer\n");
		abort();
	}

	if (lcore >= RTE_MAX_LCORE) {
		SPDK_ERRLOG("Attempted use lcore %u larger than max lcore %u\n",
			    lcore, RTE_MAX_LCORE - 1);
		abort();
	}

	*ppoller = poller;
	reactor = spdk_reactor_get(lcore);

	if (lcore == spdk_env_get_current_core()) {
		/*
		 * The poller is registered to run on the current core, so call the add function
		 * directly.
		 */
		_spdk_poller_register(reactor, poller);
	} else {
		/*
		 * The poller is registered to run on a different core.
		 * Schedule an event to run on the poller's core that will add the poller.
		 */
		spdk_event_call(spdk_event_allocate(lcore, _spdk_event_add_poller, reactor, poller));
	}
}

static void
_spdk_poller_unregister(struct spdk_reactor *reactor, struct spdk_poller *poller,
			struct spdk_event *next)
{
	assert(poller->lcore == reactor->lcore);
	assert(poller->lcore == spdk_env_get_current_core());

	poller->unregister_complete_event = next;

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

		_spdk_poller_unregister_complete(poller);
	}
}

static void
_spdk_event_remove_poller(void *arg1, void *arg2)
{
	struct spdk_poller *poller = arg1;
	struct spdk_reactor *reactor = spdk_reactor_get(poller->lcore);
	struct spdk_event *next = arg2;

	_spdk_poller_unregister(reactor, poller, next);
}

void
spdk_poller_unregister(struct spdk_poller **ppoller,
		       struct spdk_event *complete)
{
	struct spdk_poller *poller;
	uint32_t lcore;

	poller = *ppoller;

	*ppoller = NULL;

	if (poller == NULL) {
		if (complete) {
			spdk_event_call(complete);
		}
		return;
	}

	lcore = poller->lcore;

	if (lcore == spdk_env_get_current_core()) {
		/*
		 * The poller is registered on the current core, so call the remove function
		 * directly.
		 */
		_spdk_poller_unregister(spdk_reactor_get(lcore), poller, complete);
	} else {
		/*
		 * The poller is registered on a different core.
		 * Schedule an event to run on the poller's core that will remove the poller.
		 */
		spdk_event_call(spdk_event_allocate(lcore, _spdk_event_remove_poller, poller, complete));
	}
}
