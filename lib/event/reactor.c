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

#include "spdk/event.h"

#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#include <rte_config.h>
#include <rte_ring.h>

#include "reactor.h"

#include "spdk/log.h"
#include "spdk/io_channel.h"
#include "spdk/env.h"

#define SPDK_MAX_SOCKET		64

#define SPDK_REACTOR_SPIN_TIME_US	1

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

	struct rte_ring					*events;

	uint64_t					max_delay_us;
};

static struct spdk_reactor g_reactors[RTE_MAX_LCORE];
static uint64_t	g_reactor_mask  = 0;
static int	g_reactor_count = 0;

static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_INVALID;

static void spdk_reactor_construct(struct spdk_reactor *w, uint32_t lcore,
				   uint64_t max_delay_us);

struct spdk_mempool *g_spdk_event_mempool[SPDK_MAX_SOCKET];

/** \file

*/

static struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;
	reactor = &g_reactors[lcore];
	return reactor;
}

spdk_event_t
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2,
		    spdk_event_t next)
{
	struct spdk_event *event = NULL;
	unsigned socket_id = rte_lcore_to_socket_id(lcore);

	assert(socket_id < SPDK_MAX_SOCKET);

	event = spdk_mempool_get(g_spdk_event_mempool[socket_id]);
	if (event == NULL) {
		assert(false);
		return NULL;
	}

	event->lcore = lcore;
	event->fn = fn;
	event->arg1 = arg1;
	event->arg2 = arg2;
	event->next = next;

	return event;
}

void
spdk_event_call(spdk_event_t event)
{
	int rc;
	struct spdk_reactor *reactor;

	reactor = spdk_reactor_get(event->lcore);

	assert(reactor->events != NULL);
	rc = rte_ring_enqueue(reactor->events, event);
	if (rc != 0) {
		assert(false);
	}
}

uint32_t
spdk_event_queue_run_batch(uint32_t lcore)
{
	struct spdk_reactor *reactor;
	unsigned socket_id;
	unsigned count, i;
	void *events[SPDK_EVENT_BATCH_SIZE];

	reactor = spdk_reactor_get(lcore);
	assert(reactor->events != NULL);

	socket_id = rte_lcore_to_socket_id(lcore);
	assert(socket_id < SPDK_MAX_SOCKET);

	count = rte_ring_dequeue_burst(reactor->events, events, SPDK_EVENT_BATCH_SIZE);
	if (count == 0) {
		return 0;
	}

	for (i = 0; i < count; i++) {
		struct spdk_event *event = events[i];

		event->fn(event);
	}

	spdk_mempool_put_bulk(g_spdk_event_mempool[socket_id], events, count);

	return count;
}

/**

\brief Set current reactor thread name to "reactor <cpu #>".

This makes the reactor threads distinguishable in top and gdb.

*/
static void set_reactor_thread_name(void)
{
	char thread_name[16];

	snprintf(thread_name, sizeof(thread_name), "reactor %d",
		 rte_lcore_id());

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

\brief This is the main function of the reactor thread.

\code

while (1)
	if (new work items to be scheduled)
		dequeue work item from new work item ring
		enqueue work item to active work item ring
	else if (active work item count > 0)
		dequeue work item from active work item ring
		invoke work item function pointer
		if (work item state == RUNNING)
			enqueue work item to active work item ring
	else if (application state != RUNNING)
		# exit the reactor loop
		break
	else
		sleep for 100ms

\endcode

Note that new work items are posted to a separate ring so that the
active work item ring can be kept single producer/single consumer and
only be touched by reactor itself.  This avoids atomic operations
on the active work item ring which would hurt performance.

*/
static int
_spdk_reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;
	struct spdk_poller	*poller;
	uint32_t		event_count;
	uint64_t		last_action, now;
	uint64_t		spin_cycles, sleep_cycles;
	uint32_t		sleep_us;

	spdk_allocate_thread();
	set_reactor_thread_name();
	SPDK_NOTICELOG("Reactor started on core %u on socket %u\n", rte_lcore_id(),
		       rte_lcore_to_socket_id(rte_lcore_id()));

	spin_cycles = SPDK_REACTOR_SPIN_TIME_US * spdk_get_ticks_hz() / 1000000ULL;
	sleep_cycles = reactor->max_delay_us * spdk_get_ticks_hz() / 1000000ULL;
	last_action = spdk_get_ticks();

	while (1) {
		event_count = spdk_event_queue_run_batch(rte_lcore_id());
		if (event_count > 0) {
			last_action = spdk_get_ticks();
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
			last_action = spdk_get_ticks();
		}

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
			}
		}

		/* Determine if the thread can sleep */
		if (sleep_cycles > 0) {
			now = spdk_get_ticks();
			if (now >= (last_action + spin_cycles)) {
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
	char	ring_name[64];

	reactor->lcore = lcore;
	reactor->max_delay_us = max_delay_us;

	TAILQ_INIT(&reactor->active_pollers);
	TAILQ_INIT(&reactor->timer_pollers);

	snprintf(ring_name, sizeof(ring_name) - 1, "spdk_event_queue_%u", lcore);
	reactor->events =
		rte_ring_create(ring_name, 65536, rte_lcore_to_socket_id(lcore), RING_F_SC_DEQ);
	assert(reactor->events != NULL);
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
	return g_reactor_count;
}

uint32_t
spdk_app_get_current_core(void)
{
	return rte_lcore_id();
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

static int
spdk_reactor_parse_mask(const char *mask)
{
	int i;
	int ret = 0;
	uint32_t master_core = rte_get_master_lcore();

	if (g_reactor_state >= SPDK_REACTOR_STATE_INITIALIZED) {
		SPDK_ERRLOG("cannot set reactor mask after application has started\n");
		return -1;
	}

	g_reactor_mask = 0;

	if (mask == NULL) {
		/* No mask specified so use the same mask as DPDK. */
		RTE_LCORE_FOREACH(i) {
			g_reactor_mask |= (1ULL << i);
		}
	} else {
		ret = spdk_app_parse_core_mask(mask, &g_reactor_mask);
		if (ret != 0) {
			SPDK_ERRLOG("reactor mask %s specified on command line "
				    "is invalid\n", mask);
			return ret;
		}
		if (!(g_reactor_mask & (1ULL << master_core))) {
			SPDK_ERRLOG("master_core %d must be set in core mask\n", master_core);
			return -1;
		}
	}

	return 0;
}

uint64_t
spdk_app_get_core_mask(void)
{
	return g_reactor_mask;
}


static uint64_t
spdk_reactor_get_socket_mask(void)
{
	int i;
	uint32_t socket_id;
	uint64_t socket_info = 0;

	RTE_LCORE_FOREACH(i) {
		if (((1ULL << i) & g_reactor_mask)) {
			socket_id = rte_lcore_to_socket_id(i);
			socket_info |= (1ULL << socket_id);
		}
	}

	return socket_info;
}

void
spdk_reactors_start(void)
{
	struct spdk_reactor *reactor;
	uint32_t i;

	assert(rte_get_master_lcore() == rte_lcore_id());

	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;

	RTE_LCORE_FOREACH_SLAVE(i) {
		if (((1ULL << i) & spdk_app_get_core_mask())) {
			reactor = spdk_reactor_get(i);
			spdk_reactor_start(reactor);
		}
	}

	/* Start the master reactor */
	reactor = spdk_reactor_get(rte_get_master_lcore());
	spdk_reactor_start(reactor);

	rte_eal_mp_wait_lcore();

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
}

void spdk_reactors_stop(void)
{
	g_reactor_state = SPDK_REACTOR_STATE_EXITING;
}

int
spdk_reactors_init(const char *mask, unsigned int max_delay_us)
{
	uint32_t i;
	int rc;
	struct spdk_reactor *reactor;
	uint64_t socket_mask = 0x0;
	uint8_t socket_count = 0;
	char mempool_name[32];

	rc = spdk_reactor_parse_mask(mask);
	if (rc < 0) {
		return rc;
	}

	printf("Occupied cpu core mask is 0x%lx\n", spdk_app_get_core_mask());

	RTE_LCORE_FOREACH(i) {
		if (((1ULL << i) & spdk_app_get_core_mask())) {
			reactor = spdk_reactor_get(i);
			spdk_reactor_construct(reactor, i, max_delay_us);
			g_reactor_count++;
		}
	}

	socket_mask = spdk_reactor_get_socket_mask();
	printf("Occupied cpu socket mask is 0x%lx\n", socket_mask);

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			socket_count++;
		}
	}

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			snprintf(mempool_name, sizeof(mempool_name), "spdk_event_mempool_%d", i);
			g_spdk_event_mempool[i] = spdk_mempool_create(mempool_name,
						  (262144 / socket_count),
						  sizeof(struct spdk_event), -1);

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
								  sizeof(struct spdk_event), -1);

				/* TODO: in DPDK 16.04, free mempool API is avaialbe. */
				if (g_spdk_event_mempool[i] == NULL) {
					SPDK_ERRLOG("spdk_event_mempool creation failed\n");
					return -1;
				}
			}
		}
	}

	g_reactor_state = SPDK_REACTOR_STATE_INITIALIZED;

	return rc;
}

int
spdk_reactors_fini(void)
{
	/* TODO: free rings and mempool */
	return 0;
}

static void
_spdk_poller_register(struct spdk_reactor *reactor, struct spdk_poller *poller,
		      struct spdk_event *next)
{
	if (poller->period_ticks) {
		spdk_poller_insert_timer(reactor, poller, spdk_get_ticks());
	} else {
		TAILQ_INSERT_TAIL(&reactor->active_pollers, poller, tailq);
	}

	if (next) {
		spdk_event_call(next);
	}
}

static void
_spdk_event_add_poller(spdk_event_t event)
{
	struct spdk_reactor *reactor = spdk_event_get_arg1(event);
	struct spdk_poller *poller = spdk_event_get_arg2(event);
	struct spdk_event *next = spdk_event_get_next(event);

	_spdk_poller_register(reactor, poller, next);
}

void
spdk_poller_register(struct spdk_poller **ppoller, spdk_poller_fn fn, void *arg,
		     uint32_t lcore, struct spdk_event *complete, uint64_t period_microseconds)
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

	*ppoller = poller;
	reactor = spdk_reactor_get(lcore);

	if (lcore == spdk_app_get_current_core()) {
		/*
		 * The poller is registered to run on the current core, so call the add function
		 * directly.
		 */
		_spdk_poller_register(reactor, poller, complete);
	} else {
		/*
		 * The poller is registered to run on a different core.
		 * Schedule an event to run on the poller's core that will add the poller.
		 */
		spdk_event_call(spdk_event_allocate(lcore, _spdk_event_add_poller, reactor, poller,
						    complete));
	}
}

static void
_spdk_poller_unregister(struct spdk_reactor *reactor, struct spdk_poller *poller,
			struct spdk_event *next)
{
	assert(poller->lcore == reactor->lcore);
	assert(poller->lcore == spdk_app_get_current_core());

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
_spdk_event_remove_poller(spdk_event_t event)
{
	struct spdk_poller *poller = spdk_event_get_arg1(event);
	struct spdk_reactor *reactor = spdk_reactor_get(poller->lcore);
	struct spdk_event *next = spdk_event_get_next(event);

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

	if (lcore == spdk_app_get_current_core()) {
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
		spdk_event_call(spdk_event_allocate(lcore, _spdk_event_remove_poller, poller, NULL,
						    complete));
	}
}
