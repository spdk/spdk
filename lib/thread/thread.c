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

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/fd_group.h"

#include "spdk/log.h"
#include "spdk_internal/thread.h"

#ifdef __linux__
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#endif

#define SPDK_MSG_BATCH_SIZE		8
#define SPDK_MAX_DEVICE_NAME_LEN	256
#define SPDK_THREAD_EXIT_TIMEOUT_SEC	5

static pthread_mutex_t g_devlist_mutex = PTHREAD_MUTEX_INITIALIZER;

static spdk_new_thread_fn g_new_thread_fn = NULL;
static spdk_thread_op_fn g_thread_op_fn = NULL;
static spdk_thread_op_supported_fn g_thread_op_supported_fn;
static size_t g_ctx_sz = 0;
/* Monotonic increasing ID is set to each created thread beginning at 1. Once the
 * ID exceeds UINT64_MAX, further thread creation is not allowed and restarting
 * SPDK application is required.
 */
static uint64_t g_thread_id = 1;

struct io_device {
	void				*io_device;
	char				name[SPDK_MAX_DEVICE_NAME_LEN + 1];
	spdk_io_channel_create_cb	create_cb;
	spdk_io_channel_destroy_cb	destroy_cb;
	spdk_io_device_unregister_cb	unregister_cb;
	struct spdk_thread		*unregister_thread;
	uint32_t			ctx_size;
	uint32_t			for_each_count;
	TAILQ_ENTRY(io_device)		tailq;

	uint32_t			refcnt;

	bool				unregistered;
};

static TAILQ_HEAD(, io_device) g_io_devices = TAILQ_HEAD_INITIALIZER(g_io_devices);

struct spdk_msg {
	spdk_msg_fn		fn;
	void			*arg;

	SLIST_ENTRY(spdk_msg)	link;
};

#define SPDK_MSG_MEMPOOL_CACHE_SIZE	1024
static struct spdk_mempool *g_spdk_msg_mempool = NULL;

static TAILQ_HEAD(, spdk_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);
static uint32_t g_thread_count = 0;

static __thread struct spdk_thread *tls_thread = NULL;

static inline struct spdk_thread *
_get_thread(void)
{
	return tls_thread;
}

static int
_thread_lib_init(size_t ctx_sz)
{
	char mempool_name[SPDK_MAX_MEMZONE_NAME_LEN];

	g_ctx_sz = ctx_sz;

	snprintf(mempool_name, sizeof(mempool_name), "msgpool_%d", getpid());
	g_spdk_msg_mempool = spdk_mempool_create(mempool_name,
			     262144 - 1, /* Power of 2 minus 1 is optimal for memory consumption */
			     sizeof(struct spdk_msg),
			     0, /* No cache. We do our own. */
			     SPDK_ENV_SOCKET_ID_ANY);

	if (!g_spdk_msg_mempool) {
		SPDK_ERRLOG("spdk_msg_mempool creation failed\n");
		return -1;
	}

	return 0;
}

int
spdk_thread_lib_init(spdk_new_thread_fn new_thread_fn, size_t ctx_sz)
{
	assert(g_new_thread_fn == NULL);
	assert(g_thread_op_fn == NULL);

	if (new_thread_fn == NULL) {
		SPDK_INFOLOG(thread, "new_thread_fn was not specified at spdk_thread_lib_init\n");
	} else {
		g_new_thread_fn = new_thread_fn;
	}

	return _thread_lib_init(ctx_sz);
}

int
spdk_thread_lib_init_ext(spdk_thread_op_fn thread_op_fn,
			 spdk_thread_op_supported_fn thread_op_supported_fn,
			 size_t ctx_sz)
{
	assert(g_new_thread_fn == NULL);
	assert(g_thread_op_fn == NULL);
	assert(g_thread_op_supported_fn == NULL);

	if ((thread_op_fn != NULL) != (thread_op_supported_fn != NULL)) {
		SPDK_ERRLOG("Both must be defined or undefined together.\n");
		return -EINVAL;
	}

	if (thread_op_fn == NULL && thread_op_supported_fn == NULL) {
		SPDK_INFOLOG(thread, "thread_op_fn and thread_op_supported_fn were not specified\n");
	} else {
		g_thread_op_fn = thread_op_fn;
		g_thread_op_supported_fn = thread_op_supported_fn;
	}

	return _thread_lib_init(ctx_sz);
}

void
spdk_thread_lib_fini(void)
{
	struct io_device *dev;

	TAILQ_FOREACH(dev, &g_io_devices, tailq) {
		SPDK_ERRLOG("io_device %s not unregistered\n", dev->name);
	}

	if (g_spdk_msg_mempool) {
		spdk_mempool_free(g_spdk_msg_mempool);
		g_spdk_msg_mempool = NULL;
	}

	g_new_thread_fn = NULL;
	g_thread_op_fn = NULL;
	g_thread_op_supported_fn = NULL;
	g_ctx_sz = 0;
}

static void thread_interrupt_destroy(struct spdk_thread *thread);
static int thread_interrupt_create(struct spdk_thread *thread);

static void
_free_thread(struct spdk_thread *thread)
{
	struct spdk_io_channel *ch;
	struct spdk_msg *msg;
	struct spdk_poller *poller, *ptmp;

	TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
		SPDK_ERRLOG("thread %s still has channel for io_device %s\n",
			    thread->name, ch->dev->name);
	}

	TAILQ_FOREACH_SAFE(poller, &thread->active_pollers, tailq, ptmp) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_WARNLOG("active_poller %s still registered at thread exit\n",
				     poller->name);
		}
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		free(poller);
	}

	TAILQ_FOREACH_SAFE(poller, &thread->timed_pollers, tailq, ptmp) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_WARNLOG("timed_poller %s still registered at thread exit\n",
				     poller->name);
		}
		TAILQ_REMOVE(&thread->timed_pollers, poller, tailq);
		free(poller);
	}

	TAILQ_FOREACH_SAFE(poller, &thread->paused_pollers, tailq, ptmp) {
		SPDK_WARNLOG("paused_poller %s still registered at thread exit\n", poller->name);
		TAILQ_REMOVE(&thread->paused_pollers, poller, tailq);
		free(poller);
	}

	pthread_mutex_lock(&g_devlist_mutex);
	assert(g_thread_count > 0);
	g_thread_count--;
	TAILQ_REMOVE(&g_threads, thread, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);

	msg = SLIST_FIRST(&thread->msg_cache);
	while (msg != NULL) {
		SLIST_REMOVE_HEAD(&thread->msg_cache, link);

		assert(thread->msg_cache_count > 0);
		thread->msg_cache_count--;
		spdk_mempool_put(g_spdk_msg_mempool, msg);

		msg = SLIST_FIRST(&thread->msg_cache);
	}

	assert(thread->msg_cache_count == 0);

	if (thread->interrupt_mode) {
		thread_interrupt_destroy(thread);
	}

	spdk_ring_free(thread->messages);
	free(thread);
}

struct spdk_thread *
spdk_thread_create(const char *name, struct spdk_cpuset *cpumask)
{
	struct spdk_thread *thread;
	struct spdk_msg *msgs[SPDK_MSG_MEMPOOL_CACHE_SIZE];
	int rc = 0, i;

	thread = calloc(1, sizeof(*thread) + g_ctx_sz);
	if (!thread) {
		SPDK_ERRLOG("Unable to allocate memory for thread\n");
		return NULL;
	}

	if (cpumask) {
		spdk_cpuset_copy(&thread->cpumask, cpumask);
	} else {
		spdk_cpuset_negate(&thread->cpumask);
	}

	TAILQ_INIT(&thread->io_channels);
	TAILQ_INIT(&thread->active_pollers);
	TAILQ_INIT(&thread->timed_pollers);
	TAILQ_INIT(&thread->paused_pollers);
	SLIST_INIT(&thread->msg_cache);
	thread->msg_cache_count = 0;

	thread->tsc_last = spdk_get_ticks();

	thread->messages = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_SOCKET_ID_ANY);
	if (!thread->messages) {
		SPDK_ERRLOG("Unable to allocate memory for message ring\n");
		free(thread);
		return NULL;
	}

	/* Fill the local message pool cache. */
	rc = spdk_mempool_get_bulk(g_spdk_msg_mempool, (void **)msgs, SPDK_MSG_MEMPOOL_CACHE_SIZE);
	if (rc == 0) {
		/* If we can't populate the cache it's ok. The cache will get filled
		 * up organically as messages are passed to the thread. */
		for (i = 0; i < SPDK_MSG_MEMPOOL_CACHE_SIZE; i++) {
			SLIST_INSERT_HEAD(&thread->msg_cache, msgs[i], link);
			thread->msg_cache_count++;
		}
	}

	if (name) {
		snprintf(thread->name, sizeof(thread->name), "%s", name);
	} else {
		snprintf(thread->name, sizeof(thread->name), "%p", thread);
	}

	pthread_mutex_lock(&g_devlist_mutex);
	if (g_thread_id == 0) {
		SPDK_ERRLOG("Thread ID rolled over. Further thread creation is not allowed.\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		_free_thread(thread);
		return NULL;
	}
	thread->id = g_thread_id++;
	TAILQ_INSERT_TAIL(&g_threads, thread, tailq);
	g_thread_count++;
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Allocating new thread (%" PRIu64 ", %s)\n",
		      thread->id, thread->name);

	if (spdk_interrupt_mode_is_enabled()) {
		thread->interrupt_mode = true;
		rc = thread_interrupt_create(thread);
		if (rc != 0) {
			_free_thread(thread);
			return NULL;
		}
	}

	if (g_new_thread_fn) {
		rc = g_new_thread_fn(thread);
	} else if (g_thread_op_supported_fn && g_thread_op_supported_fn(SPDK_THREAD_OP_NEW)) {
		rc = g_thread_op_fn(thread, SPDK_THREAD_OP_NEW);
	}

	if (rc != 0) {
		_free_thread(thread);
		return NULL;
	}

	thread->state = SPDK_THREAD_STATE_RUNNING;

	return thread;
}

void
spdk_set_thread(struct spdk_thread *thread)
{
	tls_thread = thread;
}

static void
thread_exit(struct spdk_thread *thread, uint64_t now)
{
	struct spdk_poller *poller;
	struct spdk_io_channel *ch;

	if (now >= thread->exit_timeout_tsc) {
		SPDK_ERRLOG("thread %s got timeout, and move it to the exited state forcefully\n",
			    thread->name);
		goto exited;
	}

	TAILQ_FOREACH(poller, &thread->active_pollers, tailq) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_INFOLOG(thread,
				     "thread %s still has active poller %s\n",
				     thread->name, poller->name);
			return;
		}
	}

	TAILQ_FOREACH(poller, &thread->timed_pollers, tailq) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_INFOLOG(thread,
				     "thread %s still has active timed poller %s\n",
				     thread->name, poller->name);
			return;
		}
	}

	TAILQ_FOREACH(poller, &thread->paused_pollers, tailq) {
		SPDK_INFOLOG(thread,
			     "thread %s still has paused poller %s\n",
			     thread->name, poller->name);
		return;
	}

	TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
		SPDK_INFOLOG(thread,
			     "thread %s still has channel for io_device %s\n",
			     thread->name, ch->dev->name);
		return;
	}

exited:
	thread->state = SPDK_THREAD_STATE_EXITED;
}

int
spdk_thread_exit(struct spdk_thread *thread)
{
	SPDK_DEBUGLOG(thread, "Exit thread %s\n", thread->name);

	assert(tls_thread == thread);

	if (thread->state >= SPDK_THREAD_STATE_EXITING) {
		SPDK_INFOLOG(thread,
			     "thread %s is already exiting\n",
			     thread->name);
		return 0;
	}

	thread->exit_timeout_tsc = spdk_get_ticks() + (spdk_get_ticks_hz() *
				   SPDK_THREAD_EXIT_TIMEOUT_SEC);
	thread->state = SPDK_THREAD_STATE_EXITING;
	return 0;
}

bool
spdk_thread_is_exited(struct spdk_thread *thread)
{
	return thread->state == SPDK_THREAD_STATE_EXITED;
}

void
spdk_thread_destroy(struct spdk_thread *thread)
{
	SPDK_DEBUGLOG(thread, "Destroy thread %s\n", thread->name);

	assert(thread->state == SPDK_THREAD_STATE_EXITED);

	if (tls_thread == thread) {
		tls_thread = NULL;
	}

	_free_thread(thread);
}

void *
spdk_thread_get_ctx(struct spdk_thread *thread)
{
	if (g_ctx_sz > 0) {
		return thread->ctx;
	}

	return NULL;
}

struct spdk_cpuset *
spdk_thread_get_cpumask(struct spdk_thread *thread)
{
	return &thread->cpumask;
}

int
spdk_thread_set_cpumask(struct spdk_cpuset *cpumask)
{
	struct spdk_thread *thread;

	if (!g_thread_op_supported_fn || !g_thread_op_supported_fn(SPDK_THREAD_OP_RESCHED)) {
		SPDK_ERRLOG("Framework does not support reschedule operation.\n");
		assert(false);
		return -ENOTSUP;
	}

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("Called from non-SPDK thread\n");
		assert(false);
		return -EINVAL;
	}

	spdk_cpuset_copy(&thread->cpumask, cpumask);

	/* Invoke framework's reschedule operation. If this function is called multiple times
	 * in a single spdk_thread_poll() context, the last cpumask will be used in the
	 * reschedule operation.
	 */
	g_thread_op_fn(thread, SPDK_THREAD_OP_RESCHED);

	return 0;
}

struct spdk_thread *
spdk_thread_get_from_ctx(void *ctx)
{
	if (ctx == NULL) {
		assert(false);
		return NULL;
	}

	assert(g_ctx_sz > 0);

	return SPDK_CONTAINEROF(ctx, struct spdk_thread, ctx);
}

static inline uint32_t
msg_queue_run_batch(struct spdk_thread *thread, uint32_t max_msgs)
{
	unsigned count, i;
	void *messages[SPDK_MSG_BATCH_SIZE];
	uint64_t notify = 1;
	int rc;

#ifdef DEBUG
	/*
	 * spdk_ring_dequeue() fills messages and returns how many entries it wrote,
	 * so we will never actually read uninitialized data from events, but just to be sure
	 * (and to silence a static analyzer false positive), initialize the array to NULL pointers.
	 */
	memset(messages, 0, sizeof(messages));
#endif

	if (max_msgs > 0) {
		max_msgs = spdk_min(max_msgs, SPDK_MSG_BATCH_SIZE);
	} else {
		max_msgs = SPDK_MSG_BATCH_SIZE;
	}
	if (thread->interrupt_mode) {
		/* There may be race between msg_acknowledge and another producer's msg_notify,
		 * so msg_acknowledge should be applied ahead. And then check for self's msg_notify.
		 * This can avoid msg notification missing.
		 */
		rc = read(thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0 && errno != EAGAIN) {
			SPDK_ERRLOG("failed to acknowledge msg_queue: %s.\n", spdk_strerror(errno));
		}
	}

	count = spdk_ring_dequeue(thread->messages, messages, max_msgs);
	if (thread->interrupt_mode && spdk_ring_count(thread->messages) != 0) {
		rc = write(thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify msg_queue: %s.\n", spdk_strerror(errno));
		}
	}
	if (count == 0) {
		return 0;
	}

	for (i = 0; i < count; i++) {
		struct spdk_msg *msg = messages[i];

		assert(msg != NULL);
		msg->fn(msg->arg);

		if (thread->msg_cache_count < SPDK_MSG_MEMPOOL_CACHE_SIZE) {
			/* Insert the messages at the head. We want to re-use the hot
			 * ones. */
			SLIST_INSERT_HEAD(&thread->msg_cache, msg, link);
			thread->msg_cache_count++;
		} else {
			spdk_mempool_put(g_spdk_msg_mempool, msg);
		}
	}

	return count;
}

static void
poller_insert_timer(struct spdk_thread *thread, struct spdk_poller *poller, uint64_t now)
{
	struct spdk_poller *iter;

	poller->next_run_tick = now + poller->period_ticks;

	/*
	 * Insert poller in the thread's timed_pollers list in sorted order by next scheduled
	 * run time.
	 */
	TAILQ_FOREACH_REVERSE(iter, &thread->timed_pollers, timed_pollers_head, tailq) {
		if (iter->next_run_tick <= poller->next_run_tick) {
			TAILQ_INSERT_AFTER(&thread->timed_pollers, iter, poller, tailq);
			return;
		}
	}

	/* No earlier pollers were found, so this poller must be the new head */
	TAILQ_INSERT_HEAD(&thread->timed_pollers, poller, tailq);
}

static void
thread_insert_poller(struct spdk_thread *thread, struct spdk_poller *poller)
{
	if (poller->period_ticks) {
		poller_insert_timer(thread, poller, spdk_get_ticks());
	} else {
		TAILQ_INSERT_TAIL(&thread->active_pollers, poller, tailq);
	}
}

static inline void
thread_update_stats(struct spdk_thread *thread, uint64_t end,
		    uint64_t start, int rc)
{
	if (rc == 0) {
		/* Poller status idle */
		thread->stats.idle_tsc += end - start;
	} else if (rc > 0) {
		/* Poller status busy */
		thread->stats.busy_tsc += end - start;
	}
	/* Store end time to use it as start time of the next spdk_thread_poll(). */
	thread->tsc_last = end;
}

static int
thread_poll(struct spdk_thread *thread, uint32_t max_msgs, uint64_t now)
{
	uint32_t msg_count;
	struct spdk_poller *poller, *tmp;
	spdk_msg_fn critical_msg;
	int rc = 0;

	thread->tsc_last = now;

	critical_msg = thread->critical_msg;
	if (spdk_unlikely(critical_msg != NULL)) {
		critical_msg(NULL);
		thread->critical_msg = NULL;
	}

	msg_count = msg_queue_run_batch(thread, max_msgs);
	if (msg_count) {
		rc = 1;
	}

	TAILQ_FOREACH_REVERSE_SAFE(poller, &thread->active_pollers,
				   active_pollers_head, tailq, tmp) {
		int poller_rc;

		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
			free(poller);
			continue;
		} else if (poller->state == SPDK_POLLER_STATE_PAUSING) {
			TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
			TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
			poller->state = SPDK_POLLER_STATE_PAUSED;
			continue;
		}

		poller->state = SPDK_POLLER_STATE_RUNNING;
		poller_rc = poller->fn(poller->arg);

		poller->run_count++;
		if (poller_rc > 0) {
			poller->busy_count++;
		}

#ifdef DEBUG
		if (poller_rc == -1) {
			SPDK_DEBUGLOG(thread, "Poller %s returned -1\n", poller->name);
		}
#endif

		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
			free(poller);
		} else if (poller->state != SPDK_POLLER_STATE_PAUSED) {
			poller->state = SPDK_POLLER_STATE_WAITING;
		}

		if (poller_rc > rc) {
			rc = poller_rc;
		}
	}

	TAILQ_FOREACH_SAFE(poller, &thread->timed_pollers, tailq, tmp) {
		int timer_rc = 0;

		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			TAILQ_REMOVE(&thread->timed_pollers, poller, tailq);
			free(poller);
			continue;
		} else if (poller->state == SPDK_POLLER_STATE_PAUSING) {
			TAILQ_REMOVE(&thread->timed_pollers, poller, tailq);
			TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
			poller->state = SPDK_POLLER_STATE_PAUSED;
			continue;
		}

		if (now < poller->next_run_tick) {
			break;
		}

		poller->state = SPDK_POLLER_STATE_RUNNING;
		timer_rc = poller->fn(poller->arg);

		poller->run_count++;
		if (timer_rc > 0) {
			poller->busy_count++;
		}

#ifdef DEBUG
		if (timer_rc == -1) {
			SPDK_DEBUGLOG(thread, "Timed poller %s returned -1\n", poller->name);
		}
#endif

		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			TAILQ_REMOVE(&thread->timed_pollers, poller, tailq);
			free(poller);
		} else if (poller->state != SPDK_POLLER_STATE_PAUSED) {
			poller->state = SPDK_POLLER_STATE_WAITING;
			TAILQ_REMOVE(&thread->timed_pollers, poller, tailq);
			poller_insert_timer(thread, poller, now);
		}

		if (timer_rc > rc) {
			rc = timer_rc;
		}
	}

	return rc;
}

int
spdk_thread_poll(struct spdk_thread *thread, uint32_t max_msgs, uint64_t now)
{
	struct spdk_thread *orig_thread;
	int rc;

	orig_thread = _get_thread();
	tls_thread = thread;

	if (now == 0) {
		now = spdk_get_ticks();
	}

	if (!thread->interrupt_mode) {
		rc = thread_poll(thread, max_msgs, now);
	} else {
		/* Non-block wait on thread's fd_group */
		rc = spdk_fd_group_wait(thread->fgrp, 0);
	}


	if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITING)) {
		thread_exit(thread, now);
	}

	thread_update_stats(thread, spdk_get_ticks(), now, rc);

	tls_thread = orig_thread;

	return rc;
}

uint64_t
spdk_thread_next_poller_expiration(struct spdk_thread *thread)
{
	struct spdk_poller *poller;

	poller = TAILQ_FIRST(&thread->timed_pollers);
	if (poller) {
		return poller->next_run_tick;
	}

	return 0;
}

int
spdk_thread_has_active_pollers(struct spdk_thread *thread)
{
	return !TAILQ_EMPTY(&thread->active_pollers);
}

static bool
thread_has_unpaused_pollers(struct spdk_thread *thread)
{
	if (TAILQ_EMPTY(&thread->active_pollers) &&
	    TAILQ_EMPTY(&thread->timed_pollers)) {
		return false;
	}

	return true;
}

bool
spdk_thread_has_pollers(struct spdk_thread *thread)
{
	if (!thread_has_unpaused_pollers(thread) &&
	    TAILQ_EMPTY(&thread->paused_pollers)) {
		return false;
	}

	return true;
}

bool
spdk_thread_is_idle(struct spdk_thread *thread)
{
	if (spdk_ring_count(thread->messages) ||
	    thread_has_unpaused_pollers(thread) ||
	    thread->critical_msg != NULL) {
		return false;
	}

	return true;
}

uint32_t
spdk_thread_get_count(void)
{
	/*
	 * Return cached value of the current thread count.  We could acquire the
	 *  lock and iterate through the TAILQ of threads to count them, but that
	 *  count could still be invalidated after we release the lock.
	 */
	return g_thread_count;
}

struct spdk_thread *
spdk_get_thread(void)
{
	return _get_thread();
}

const char *
spdk_thread_get_name(const struct spdk_thread *thread)
{
	return thread->name;
}

uint64_t
spdk_thread_get_id(const struct spdk_thread *thread)
{
	return thread->id;
}

struct spdk_thread *
spdk_thread_get_by_id(uint64_t id)
{
	struct spdk_thread *thread;

	if (id == 0 || id >= g_thread_id) {
		SPDK_ERRLOG("invalid thread id: %" PRIu64 ".\n", id);
		return NULL;
	}
	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(thread, &g_threads, tailq) {
		if (thread->id == id) {
			break;
		}
	}
	pthread_mutex_unlock(&g_devlist_mutex);
	return thread;
}

int
spdk_thread_get_stats(struct spdk_thread_stats *stats)
{
	struct spdk_thread *thread;

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		return -EINVAL;
	}

	if (stats == NULL) {
		return -EINVAL;
	}

	*stats = thread->stats;

	return 0;
}

uint64_t
spdk_thread_get_last_tsc(struct spdk_thread *thread)
{
	if (thread == NULL) {
		thread = _get_thread();
	}

	return thread->tsc_last;
}

int
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx)
{
	struct spdk_thread *local_thread;
	struct spdk_msg *msg;
	int rc;

	assert(thread != NULL);

	if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITED)) {
		SPDK_ERRLOG("Thread %s is marked as exited.\n", thread->name);
		return -EIO;
	}

	local_thread = _get_thread();

	msg = NULL;
	if (local_thread != NULL) {
		if (local_thread->msg_cache_count > 0) {
			msg = SLIST_FIRST(&local_thread->msg_cache);
			assert(msg != NULL);
			SLIST_REMOVE_HEAD(&local_thread->msg_cache, link);
			local_thread->msg_cache_count--;
		}
	}

	if (msg == NULL) {
		msg = spdk_mempool_get(g_spdk_msg_mempool);
		if (!msg) {
			SPDK_ERRLOG("msg could not be allocated\n");
			return -ENOMEM;
		}
	}

	msg->fn = fn;
	msg->arg = ctx;

	rc = spdk_ring_enqueue(thread->messages, (void **)&msg, 1, NULL);
	if (rc != 1) {
		SPDK_ERRLOG("msg could not be enqueued\n");
		spdk_mempool_put(g_spdk_msg_mempool, msg);
		return -EIO;
	}

	if (thread->interrupt_mode) {
		uint64_t notify = 1;

		rc = write(thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify msg_queue: %s.\n", spdk_strerror(errno));
			return -EIO;
		}
	}

	return 0;
}

int
spdk_thread_send_critical_msg(struct spdk_thread *thread, spdk_msg_fn fn)
{
	spdk_msg_fn expected = NULL;

	if (__atomic_compare_exchange_n(&thread->critical_msg, &expected, fn, false, __ATOMIC_SEQ_CST,
					__ATOMIC_SEQ_CST)) {
		if (thread->interrupt_mode) {
			uint64_t notify = 1;
			int rc;

			rc = write(thread->msg_fd, &notify, sizeof(notify));
			if (rc < 0) {
				SPDK_ERRLOG("failed to notify msg_queue: %s.\n", spdk_strerror(errno));
				return -EIO;
			}
		}

		return 0;
	}

	return -EIO;
}

#ifdef __linux__
static int
interrupt_timerfd_prepare(uint64_t period_microseconds)
{
	int timerfd;
	int ret;
	struct itimerspec new_tv;
	uint64_t period_seconds;
	uint64_t period_nanoseconds;

	if (period_microseconds == 0) {
		return -EINVAL;
	}

	period_seconds = period_microseconds / SPDK_SEC_TO_USEC;
	period_nanoseconds = period_microseconds % SPDK_SEC_TO_USEC * 1000;

	new_tv.it_value.tv_sec = period_seconds;
	new_tv.it_value.tv_nsec = period_nanoseconds;

	new_tv.it_interval.tv_sec = period_seconds;
	new_tv.it_interval.tv_nsec = period_nanoseconds;

	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK & TFD_CLOEXEC);
	if (timerfd < 0) {
		return -errno;
	}

	ret = timerfd_settime(timerfd, 0, &new_tv, NULL);
	if (ret < 0) {
		close(timerfd);
		return -errno;
	}

	return timerfd;
}
#else
static int
interrupt_timerfd_prepare(uint64_t period_microseconds)
{
	return -ENOTSUP;
}
#endif

static int
interrupt_timerfd_process(void *arg)
{
	struct spdk_poller *poller = arg;
	uint64_t exp;
	int rc;

	/* clear the level of interval timer */
	rc = read(poller->timerfd, &exp, sizeof(exp));
	if (rc < 0) {
		if (rc == -EAGAIN) {
			return 0;
		}

		return rc;
	}

	return poller->fn(poller->arg);
}

static int
thread_interrupt_register_timerfd(struct spdk_fd_group *fgrp,
				  uint64_t period_microseconds,
				  struct spdk_poller *poller)
{
	int timerfd;
	int rc;

	timerfd = interrupt_timerfd_prepare(period_microseconds);
	if (timerfd < 0) {
		return timerfd;
	}

	rc = spdk_fd_group_add(fgrp, timerfd,
			       interrupt_timerfd_process, poller);
	if (rc < 0) {
		close(timerfd);
		return rc;
	}

	return timerfd;
}

static struct spdk_poller *
poller_register(spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds,
		const char *name)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller;
	uint64_t quotient, remainder, ticks;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return NULL;
	}

	if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITED)) {
		SPDK_ERRLOG("thread %s is marked as exited\n", thread->name);
		return NULL;
	}

	poller = calloc(1, sizeof(*poller));
	if (poller == NULL) {
		SPDK_ERRLOG("Poller memory allocation failed\n");
		return NULL;
	}

	if (name) {
		snprintf(poller->name, sizeof(poller->name), "%s", name);
	} else {
		snprintf(poller->name, sizeof(poller->name), "%p", fn);
	}

	poller->state = SPDK_POLLER_STATE_WAITING;
	poller->fn = fn;
	poller->arg = arg;
	poller->thread = thread;

	if (period_microseconds) {
		quotient = period_microseconds / SPDK_SEC_TO_USEC;
		remainder = period_microseconds % SPDK_SEC_TO_USEC;
		ticks = spdk_get_ticks_hz();

		poller->period_ticks = ticks * quotient + (ticks * remainder) / SPDK_SEC_TO_USEC;
	} else {
		poller->period_ticks = 0;
	}

	if (thread->interrupt_mode && period_microseconds != 0) {
		int rc;

		rc = thread_interrupt_register_timerfd(thread->fgrp, period_microseconds, poller);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to register timerfd for periodic poller: %s\n", spdk_strerror(-rc));
			free(poller);
			return NULL;
		}
		poller->timerfd = rc;
	}

	thread_insert_poller(thread, poller);

	return poller;
}

struct spdk_poller *
spdk_poller_register(spdk_poller_fn fn,
		     void *arg,
		     uint64_t period_microseconds)
{
	return poller_register(fn, arg, period_microseconds, NULL);
}

struct spdk_poller *
spdk_poller_register_named(spdk_poller_fn fn,
			   void *arg,
			   uint64_t period_microseconds,
			   const char *name)
{
	return poller_register(fn, arg, period_microseconds, name);
}

void
spdk_poller_unregister(struct spdk_poller **ppoller)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller;

	poller = *ppoller;
	if (poller == NULL) {
		return;
	}

	*ppoller = NULL;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (poller->thread != thread) {
		SPDK_ERRLOG("different from the thread that called spdk_poller_register()\n");
		assert(false);
		return;
	}

	if (thread->interrupt_mode && poller->timerfd) {
		spdk_fd_group_remove(thread->fgrp, poller->timerfd);
		close(poller->timerfd);
		poller->timerfd = 0;
	}

	/* If the poller was paused, put it on the active_pollers list so that
	 * its unregistration can be processed by spdk_thread_poll().
	 */
	if (poller->state == SPDK_POLLER_STATE_PAUSED) {
		TAILQ_REMOVE(&thread->paused_pollers, poller, tailq);
		TAILQ_INSERT_TAIL(&thread->active_pollers, poller, tailq);
		poller->period_ticks = 0;
	}

	/* Simply set the state to unregistered. The poller will get cleaned up
	 * in a subsequent call to spdk_thread_poll().
	 */
	poller->state = SPDK_POLLER_STATE_UNREGISTERED;
}

void
spdk_poller_pause(struct spdk_poller *poller)
{
	struct spdk_thread *thread;

	if (poller->state == SPDK_POLLER_STATE_PAUSED ||
	    poller->state == SPDK_POLLER_STATE_PAUSING) {
		return;
	}

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	/* If a poller is paused from within itself, we can immediately move it
	 * on the paused_pollers list.  Otherwise we just set its state to
	 * SPDK_POLLER_STATE_PAUSING and let spdk_thread_poll() move it.  It
	 * allows a poller to be paused from another one's context without
	 * breaking the TAILQ_FOREACH_REVERSE_SAFE iteration.
	 */
	if (poller->state != SPDK_POLLER_STATE_RUNNING) {
		poller->state = SPDK_POLLER_STATE_PAUSING;
	} else {
		if (poller->period_ticks > 0) {
			TAILQ_REMOVE(&thread->timed_pollers, poller, tailq);
		} else {
			TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		}

		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
	}
}

void
spdk_poller_resume(struct spdk_poller *poller)
{
	struct spdk_thread *thread;

	if (poller->state != SPDK_POLLER_STATE_PAUSED &&
	    poller->state != SPDK_POLLER_STATE_PAUSING) {
		return;
	}

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	/* If a poller is paused it has to be removed from the paused pollers
	 * list and put on the active / timer list depending on its
	 * period_ticks.  If a poller is still in the process of being paused,
	 * we just need to flip its state back to waiting, as it's already on
	 * the appropriate list.
	 */
	if (poller->state == SPDK_POLLER_STATE_PAUSED) {
		TAILQ_REMOVE(&thread->paused_pollers, poller, tailq);
		thread_insert_poller(thread, poller);
	}

	poller->state = SPDK_POLLER_STATE_WAITING;
}

const char *
spdk_poller_state_str(enum spdk_poller_state state)
{
	switch (state) {
	case SPDK_POLLER_STATE_WAITING:
		return "waiting";
	case SPDK_POLLER_STATE_RUNNING:
		return "running";
	case SPDK_POLLER_STATE_UNREGISTERED:
		return "unregistered";
	case SPDK_POLLER_STATE_PAUSING:
		return "pausing";
	case SPDK_POLLER_STATE_PAUSED:
		return "paused";
	default:
		return NULL;
	}
}

struct call_thread {
	struct spdk_thread *cur_thread;
	spdk_msg_fn fn;
	void *ctx;

	struct spdk_thread *orig_thread;
	spdk_msg_fn cpl;
};

static void
_on_thread(void *ctx)
{
	struct call_thread *ct = ctx;
	int rc __attribute__((unused));

	ct->fn(ct->ctx);

	pthread_mutex_lock(&g_devlist_mutex);
	ct->cur_thread = TAILQ_NEXT(ct->cur_thread, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);

	if (!ct->cur_thread) {
		SPDK_DEBUGLOG(thread, "Completed thread iteration\n");

		rc = spdk_thread_send_msg(ct->orig_thread, ct->cpl, ct->ctx);
		free(ctx);
	} else {
		SPDK_DEBUGLOG(thread, "Continuing thread iteration to %s\n",
			      ct->cur_thread->name);

		rc = spdk_thread_send_msg(ct->cur_thread, _on_thread, ctx);
	}
	assert(rc == 0);
}

void
spdk_for_each_thread(spdk_msg_fn fn, void *ctx, spdk_msg_fn cpl)
{
	struct call_thread *ct;
	struct spdk_thread *thread;
	int rc __attribute__((unused));

	ct = calloc(1, sizeof(*ct));
	if (!ct) {
		SPDK_ERRLOG("Unable to perform thread iteration\n");
		cpl(ctx);
		return;
	}

	ct->fn = fn;
	ct->ctx = ctx;
	ct->cpl = cpl;

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		free(ct);
		cpl(ctx);
		return;
	}
	ct->orig_thread = thread;

	pthread_mutex_lock(&g_devlist_mutex);
	ct->cur_thread = TAILQ_FIRST(&g_threads);
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Starting thread iteration from %s\n",
		      ct->orig_thread->name);

	rc = spdk_thread_send_msg(ct->cur_thread, _on_thread, ct);
	assert(rc == 0);
}

void
spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size,
			const char *name)
{
	struct io_device *dev, *tmp;
	struct spdk_thread *thread;

	assert(io_device != NULL);
	assert(create_cb != NULL);
	assert(destroy_cb != NULL);

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	dev = calloc(1, sizeof(struct io_device));
	if (dev == NULL) {
		SPDK_ERRLOG("could not allocate io_device\n");
		return;
	}

	dev->io_device = io_device;
	if (name) {
		snprintf(dev->name, sizeof(dev->name), "%s", name);
	} else {
		snprintf(dev->name, sizeof(dev->name), "%p", dev);
	}
	dev->create_cb = create_cb;
	dev->destroy_cb = destroy_cb;
	dev->unregister_cb = NULL;
	dev->ctx_size = ctx_size;
	dev->for_each_count = 0;
	dev->unregistered = false;
	dev->refcnt = 0;

	SPDK_DEBUGLOG(thread, "Registering io_device %s (%p) on thread %s\n",
		      dev->name, dev->io_device, thread->name);

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(tmp, &g_io_devices, tailq) {
		if (tmp->io_device == io_device) {
			SPDK_ERRLOG("io_device %p already registered (old:%s new:%s)\n",
				    io_device, tmp->name, dev->name);
			free(dev);
			pthread_mutex_unlock(&g_devlist_mutex);
			return;
		}
	}
	TAILQ_INSERT_TAIL(&g_io_devices, dev, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);
}

static void
_finish_unregister(void *arg)
{
	struct io_device *dev = arg;

	SPDK_DEBUGLOG(thread, "Finishing unregistration of io_device %s (%p) on thread %s\n",
		      dev->name, dev->io_device, dev->unregister_thread->name);

	dev->unregister_cb(dev->io_device);
	free(dev);
}

static void
io_device_free(struct io_device *dev)
{
	int rc __attribute__((unused));

	if (dev->unregister_cb == NULL) {
		free(dev);
	} else {
		assert(dev->unregister_thread != NULL);
		SPDK_DEBUGLOG(thread, "io_device %s (%p) needs to unregister from thread %s\n",
			      dev->name, dev->io_device, dev->unregister_thread->name);
		rc = spdk_thread_send_msg(dev->unregister_thread, _finish_unregister, dev);
		assert(rc == 0);
	}
}

void
spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb)
{
	struct io_device *dev;
	uint32_t refcnt;
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(dev, &g_io_devices, tailq) {
		if (dev->io_device == io_device) {
			break;
		}
	}

	if (!dev) {
		SPDK_ERRLOG("io_device %p not found\n", io_device);
		assert(false);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	if (dev->for_each_count > 0) {
		SPDK_ERRLOG("io_device %s (%p) has %u for_each calls outstanding\n",
			    dev->name, io_device, dev->for_each_count);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	dev->unregister_cb = unregister_cb;
	dev->unregistered = true;
	TAILQ_REMOVE(&g_io_devices, dev, tailq);
	refcnt = dev->refcnt;
	dev->unregister_thread = thread;
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Unregistering io_device %s (%p) from thread %s\n",
		      dev->name, dev->io_device, thread->name);

	if (refcnt > 0) {
		/* defer deletion */
		return;
	}

	io_device_free(dev);
}

const char *
spdk_io_device_get_name(struct io_device *dev)
{
	return dev->name;
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	struct spdk_io_channel *ch;
	struct spdk_thread *thread;
	struct io_device *dev;
	int rc;

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(dev, &g_io_devices, tailq) {
		if (dev->io_device == io_device) {
			break;
		}
	}
	if (dev == NULL) {
		SPDK_ERRLOG("could not find io_device %p\n", io_device);
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITED)) {
		SPDK_ERRLOG("Thread %s is marked as exited\n", thread->name);
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
		if (ch->dev == dev) {
			ch->ref++;

			SPDK_DEBUGLOG(thread, "Get io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
				      ch, dev->name, dev->io_device, thread->name, ch->ref);

			/*
			 * An I/O channel already exists for this device on this
			 *  thread, so return it.
			 */
			pthread_mutex_unlock(&g_devlist_mutex);
			return ch;
		}
	}

	ch = calloc(1, sizeof(*ch) + dev->ctx_size);
	if (ch == NULL) {
		SPDK_ERRLOG("could not calloc spdk_io_channel\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	ch->dev = dev;
	ch->destroy_cb = dev->destroy_cb;
	ch->thread = thread;
	ch->ref = 1;
	ch->destroy_ref = 0;
	TAILQ_INSERT_TAIL(&thread->io_channels, ch, tailq);

	SPDK_DEBUGLOG(thread, "Get io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
		      ch, dev->name, dev->io_device, thread->name, ch->ref);

	dev->refcnt++;

	pthread_mutex_unlock(&g_devlist_mutex);

	rc = dev->create_cb(io_device, (uint8_t *)ch + sizeof(*ch));
	if (rc != 0) {
		pthread_mutex_lock(&g_devlist_mutex);
		TAILQ_REMOVE(&ch->thread->io_channels, ch, tailq);
		dev->refcnt--;
		free(ch);
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	return ch;
}

static void
put_io_channel(void *arg)
{
	struct spdk_io_channel *ch = arg;
	bool do_remove_dev = true;
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	SPDK_DEBUGLOG(thread,
		      "Releasing io_channel %p for io_device %s (%p) on thread %s\n",
		      ch, ch->dev->name, ch->dev->io_device, thread->name);

	assert(ch->thread == thread);

	ch->destroy_ref--;

	if (ch->ref > 0 || ch->destroy_ref > 0) {
		/*
		 * Another reference to the associated io_device was requested
		 *  after this message was sent but before it had a chance to
		 *  execute.
		 */
		return;
	}

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_REMOVE(&ch->thread->io_channels, ch, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);

	/* Don't hold the devlist mutex while the destroy_cb is called. */
	ch->destroy_cb(ch->dev->io_device, spdk_io_channel_get_ctx(ch));

	pthread_mutex_lock(&g_devlist_mutex);
	ch->dev->refcnt--;

	if (!ch->dev->unregistered) {
		do_remove_dev = false;
	}

	if (ch->dev->refcnt > 0) {
		do_remove_dev = false;
	}

	pthread_mutex_unlock(&g_devlist_mutex);

	if (do_remove_dev) {
		io_device_free(ch->dev);
	}
	free(ch);
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	struct spdk_thread *thread;
	int rc __attribute__((unused));

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	if (ch->thread != thread) {
		SPDK_ERRLOG("different from the thread that called get_io_channel()\n");
		assert(false);
		return;
	}

	SPDK_DEBUGLOG(thread,
		      "Putting io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
		      ch, ch->dev->name, ch->dev->io_device, thread->name, ch->ref);

	ch->ref--;

	if (ch->ref == 0) {
		ch->destroy_ref++;
		rc = spdk_thread_send_msg(thread, put_io_channel, ch);
		assert(rc == 0);
	}
}

struct spdk_io_channel *
spdk_io_channel_from_ctx(void *ctx)
{
	return (struct spdk_io_channel *)((uint8_t *)ctx - sizeof(struct spdk_io_channel));
}

struct spdk_thread *
spdk_io_channel_get_thread(struct spdk_io_channel *ch)
{
	return ch->thread;
}

struct spdk_io_channel_iter {
	void *io_device;
	struct io_device *dev;
	spdk_channel_msg fn;
	int status;
	void *ctx;
	struct spdk_io_channel *ch;

	struct spdk_thread *cur_thread;

	struct spdk_thread *orig_thread;
	spdk_channel_for_each_cpl cpl;
};

void *
spdk_io_channel_iter_get_io_device(struct spdk_io_channel_iter *i)
{
	return i->io_device;
}

struct spdk_io_channel *
spdk_io_channel_iter_get_channel(struct spdk_io_channel_iter *i)
{
	return i->ch;
}

void *
spdk_io_channel_iter_get_ctx(struct spdk_io_channel_iter *i)
{
	return i->ctx;
}

static void
_call_completion(void *ctx)
{
	struct spdk_io_channel_iter *i = ctx;

	if (i->cpl != NULL) {
		i->cpl(i, i->status);
	}
	free(i);
}

static void
_call_channel(void *ctx)
{
	struct spdk_io_channel_iter *i = ctx;
	struct spdk_io_channel *ch;

	/*
	 * It is possible that the channel was deleted before this
	 *  message had a chance to execute.  If so, skip calling
	 *  the fn() on this thread.
	 */
	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(ch, &i->cur_thread->io_channels, tailq) {
		if (ch->dev->io_device == i->io_device) {
			break;
		}
	}
	pthread_mutex_unlock(&g_devlist_mutex);

	if (ch) {
		i->fn(i);
	} else {
		spdk_for_each_channel_continue(i, 0);
	}
}

void
spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
		      spdk_channel_for_each_cpl cpl)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch;
	struct spdk_io_channel_iter *i;
	int rc __attribute__((unused));

	i = calloc(1, sizeof(*i));
	if (!i) {
		SPDK_ERRLOG("Unable to allocate iterator\n");
		return;
	}

	i->io_device = io_device;
	i->fn = fn;
	i->ctx = ctx;
	i->cpl = cpl;

	pthread_mutex_lock(&g_devlist_mutex);
	i->orig_thread = _get_thread();

	TAILQ_FOREACH(thread, &g_threads, tailq) {
		TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
			if (ch->dev->io_device == io_device) {
				ch->dev->for_each_count++;
				i->dev = ch->dev;
				i->cur_thread = thread;
				i->ch = ch;
				pthread_mutex_unlock(&g_devlist_mutex);
				rc = spdk_thread_send_msg(thread, _call_channel, i);
				assert(rc == 0);
				return;
			}
		}
	}

	pthread_mutex_unlock(&g_devlist_mutex);

	rc = spdk_thread_send_msg(i->orig_thread, _call_completion, i);
	assert(rc == 0);
}

void
spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch;
	int rc __attribute__((unused));

	assert(i->cur_thread == spdk_get_thread());

	i->status = status;

	pthread_mutex_lock(&g_devlist_mutex);
	if (status) {
		goto end;
	}
	thread = TAILQ_NEXT(i->cur_thread, tailq);
	while (thread) {
		TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
			if (ch->dev->io_device == i->io_device) {
				i->cur_thread = thread;
				i->ch = ch;
				pthread_mutex_unlock(&g_devlist_mutex);
				rc = spdk_thread_send_msg(thread, _call_channel, i);
				assert(rc == 0);
				return;
			}
		}
		thread = TAILQ_NEXT(thread, tailq);
	}

end:
	i->dev->for_each_count--;
	i->ch = NULL;
	pthread_mutex_unlock(&g_devlist_mutex);

	rc = spdk_thread_send_msg(i->orig_thread, _call_completion, i);
	assert(rc == 0);
}

struct spdk_interrupt {
	int			efd;
	struct spdk_thread	*thread;
	char			name[SPDK_MAX_POLLER_NAME_LEN + 1];
};

static void
thread_interrupt_destroy(struct spdk_thread *thread)
{
	struct spdk_fd_group *fgrp = thread->fgrp;

	SPDK_INFOLOG(thread, "destroy fgrp for thread (%s)\n", thread->name);

	if (thread->msg_fd <= 0) {
		return;
	}

	spdk_fd_group_remove(fgrp, thread->msg_fd);
	close(thread->msg_fd);

	spdk_fd_group_destroy(fgrp);
	thread->fgrp = NULL;
}

#ifdef __linux__
static int
thread_interrupt_msg_process(void *arg)
{
	struct spdk_thread *thread = arg;
	struct spdk_thread *orig_thread;
	uint32_t msg_count;
	spdk_msg_fn critical_msg;
	int rc = 0;
	uint64_t now = spdk_get_ticks();

	orig_thread = _get_thread();
	tls_thread = thread;

	critical_msg = thread->critical_msg;
	if (spdk_unlikely(critical_msg != NULL)) {
		critical_msg(NULL);
		thread->critical_msg = NULL;
	}

	msg_count = msg_queue_run_batch(thread, 0);
	if (msg_count) {
		rc = 1;
	}

	thread_update_stats(thread, spdk_get_ticks(), now, rc);

	tls_thread = orig_thread;

	return rc;
}

static int
thread_interrupt_create(struct spdk_thread *thread)
{
	int rc;

	SPDK_INFOLOG(thread, "Create fgrp for thread (%s)\n", thread->name);

	rc = spdk_fd_group_create(&thread->fgrp);
	if (rc) {
		return rc;
	}

	thread->msg_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (thread->msg_fd < 0) {
		rc = -errno;
		spdk_fd_group_destroy(thread->fgrp);
		thread->fgrp = NULL;

		return rc;
	}

	return spdk_fd_group_add(thread->fgrp, thread->msg_fd, thread_interrupt_msg_process, thread);
}
#else
static int
thread_interrupt_create(struct spdk_thread *thread)
{
	return -ENOTSUP;
}
#endif

struct spdk_interrupt *
spdk_interrupt_register(int efd, spdk_interrupt_fn fn,
			void *arg, const char *name)
{
	struct spdk_thread *thread;
	struct spdk_interrupt *intr;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return NULL;
	}

	if (spdk_unlikely(thread->state != SPDK_THREAD_STATE_RUNNING)) {
		SPDK_ERRLOG("thread %s is marked as exited\n", thread->name);
		return NULL;
	}

	if (spdk_fd_group_add(thread->fgrp, efd, fn, arg)) {
		return NULL;
	}

	intr = calloc(1, sizeof(*intr));
	if (intr == NULL) {
		SPDK_ERRLOG("Interrupt handler allocation failed\n");
		return NULL;
	}

	if (name) {
		snprintf(intr->name, sizeof(intr->name), "%s", name);
	} else {
		snprintf(intr->name, sizeof(intr->name), "%p", fn);
	}

	intr->efd = efd;
	intr->thread = thread;

	return intr;
}

void
spdk_interrupt_unregister(struct spdk_interrupt **pintr)
{
	struct spdk_thread *thread;
	struct spdk_interrupt *intr;

	intr = *pintr;
	if (intr == NULL) {
		return;
	}

	*pintr = NULL;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (intr->thread != thread) {
		SPDK_ERRLOG("different from the thread that called spdk_interrupt_register()\n");
		assert(false);
		return;
	}

	spdk_fd_group_remove(thread->fgrp, intr->efd);
	free(intr);
}

int
spdk_interrupt_set_event_types(struct spdk_interrupt *intr,
			       enum spdk_interrupt_event_types event_types)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return -EINVAL;
	}

	if (intr->thread != thread) {
		SPDK_ERRLOG("different from the thread that called spdk_interrupt_register()\n");
		assert(false);
		return -EINVAL;
	}

	return spdk_fd_group_event_modify(thread->fgrp, intr->efd, event_types);
}

int
spdk_thread_get_interrupt_fd(struct spdk_thread *thread)
{
	return spdk_fd_group_get_fd(thread->fgrp);
}

static bool g_interrupt_mode = false;

int
spdk_interrupt_mode_enable(void)
{
#ifdef __linux__
	SPDK_NOTICELOG("Set SPDK running in interrupt mode.\n");
	g_interrupt_mode = true;
	return 0;
#else
	SPDK_ERRLOG("SPDK interrupt mode supports only Linux platform now.\n");
	g_interrupt_mode = false;
	return -ENOTSUP;
#endif
}

bool
spdk_interrupt_mode_is_enabled(void)
{
	return g_interrupt_mode;
}

SPDK_LOG_REGISTER_COMPONENT(thread)
