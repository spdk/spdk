/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/fd_group.h"

#include "spdk/log.h"
#include "spdk_internal/thread.h"
#include "spdk_internal/usdt.h"
#include "thread_internal.h"

#include "spdk_internal/trace_defs.h"

#ifdef __linux__
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#endif

#ifdef SPDK_HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#define SPDK_MSG_BATCH_SIZE		8
#define SPDK_MAX_DEVICE_NAME_LEN	256
#define SPDK_THREAD_EXIT_TIMEOUT_SEC	5
#define SPDK_MAX_POLLER_NAME_LEN	256
#define SPDK_MAX_THREAD_NAME_LEN	256

static struct spdk_thread *g_app_thread;

struct spdk_interrupt {
	int			efd;
	struct spdk_fd_group	*fgrp;
	struct spdk_thread	*thread;
	spdk_interrupt_fn	fn;
	void			*arg;
	char			name[SPDK_MAX_POLLER_NAME_LEN + 1];
};

enum spdk_poller_state {
	/* The poller is registered with a thread but not currently executing its fn. */
	SPDK_POLLER_STATE_WAITING,

	/* The poller is currently running its fn. */
	SPDK_POLLER_STATE_RUNNING,

	/* The poller was unregistered during the execution of its fn. */
	SPDK_POLLER_STATE_UNREGISTERED,

	/* The poller is in the process of being paused.  It will be paused
	 * during the next time it's supposed to be executed.
	 */
	SPDK_POLLER_STATE_PAUSING,

	/* The poller is registered but currently paused.  It's on the
	 * paused_pollers list.
	 */
	SPDK_POLLER_STATE_PAUSED,
};

struct spdk_poller {
	TAILQ_ENTRY(spdk_poller)	tailq;
	RB_ENTRY(spdk_poller)		node;

	/* Current state of the poller; should only be accessed from the poller's thread. */
	enum spdk_poller_state		state;

	uint64_t			period_ticks;
	uint64_t			next_run_tick;
	uint64_t			run_count;
	uint64_t			busy_count;
	uint64_t			id;
	spdk_poller_fn			fn;
	void				*arg;
	struct spdk_thread		*thread;
	struct spdk_interrupt		*intr;
	spdk_poller_set_interrupt_mode_cb set_intr_cb_fn;
	void				*set_intr_cb_arg;

	char				name[SPDK_MAX_POLLER_NAME_LEN + 1];
};

enum spdk_thread_state {
	/* The thread is processing poller and message by spdk_thread_poll(). */
	SPDK_THREAD_STATE_RUNNING,

	/* The thread is in the process of termination. It reaps unregistering
	 * poller are releasing I/O channel.
	 */
	SPDK_THREAD_STATE_EXITING,

	/* The thread is exited. It is ready to call spdk_thread_destroy(). */
	SPDK_THREAD_STATE_EXITED,
};

struct spdk_thread_post_poller_handler {
	spdk_post_poller_fn fn;
	void *fn_arg;
};

#define SPDK_THREAD_MAX_POST_POLLER_HANDLERS (4)

struct spdk_thread {
	uint64_t			tsc_last;
	struct spdk_thread_stats	stats;
	/*
	 * Contains pollers actively running on this thread.  Pollers
	 *  are run round-robin. The thread takes one poller from the head
	 *  of the ring, executes it, then puts it back at the tail of
	 *  the ring.
	 */
	TAILQ_HEAD(active_pollers_head, spdk_poller)	active_pollers;
	/**
	 * Contains pollers running on this thread with a periodic timer.
	 */
	RB_HEAD(timed_pollers_tree, spdk_poller)	timed_pollers;
	struct spdk_poller				*first_timed_poller;
	/*
	 * Contains paused pollers.  Pollers on this queue are waiting until
	 * they are resumed (in which case they're put onto the active/timer
	 * queues) or unregistered.
	 */
	TAILQ_HEAD(paused_pollers_head, spdk_poller)	paused_pollers;
	struct spdk_thread_post_poller_handler		pp_handlers[SPDK_THREAD_MAX_POST_POLLER_HANDLERS];
	struct spdk_ring		*messages;
	uint8_t				num_pp_handlers;
	int				msg_fd;
	SLIST_HEAD(, spdk_msg)		msg_cache;
	size_t				msg_cache_count;
	spdk_msg_fn			critical_msg;
	uint64_t			id;
	uint64_t			next_poller_id;
	enum spdk_thread_state		state;
	int				pending_unregister_count;
	uint32_t			for_each_count;

	RB_HEAD(io_channel_tree, spdk_io_channel)	io_channels;
	TAILQ_ENTRY(spdk_thread)			tailq;

	char				name[SPDK_MAX_THREAD_NAME_LEN + 1];
	struct spdk_cpuset		cpumask;
	uint64_t			exit_timeout_tsc;

	int32_t				lock_count;

	/* spdk_thread is bound to current CPU core. */
	bool				is_bound;

	/* Indicates whether this spdk_thread currently runs in interrupt. */
	bool				in_interrupt;
	bool				poller_unregistered;
	struct spdk_fd_group		*fgrp;

	uint16_t			trace_id;

	uint8_t				reserved[6];

	/* User context allocated at the end */
	uint8_t				ctx[0];
};

/*
 * Assert that spdk_thread struct is 8 byte aligned to ensure
 * the user ctx is also 8-byte aligned.
 */
SPDK_STATIC_ASSERT((sizeof(struct spdk_thread)) % 8 == 0, "Incorrect size");

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

enum spin_error {
	SPIN_ERR_NONE,
	/* Trying to use an SPDK lock while not on an SPDK thread */
	SPIN_ERR_NOT_SPDK_THREAD,
	/* Trying to lock a lock already held by this SPDK thread */
	SPIN_ERR_DEADLOCK,
	/* Trying to unlock a lock not held by this SPDK thread */
	SPIN_ERR_WRONG_THREAD,
	/* pthread_spin_*() returned an error */
	SPIN_ERR_PTHREAD,
	/* Trying to destroy a lock that is held */
	SPIN_ERR_LOCK_HELD,
	/* lock_count is invalid */
	SPIN_ERR_LOCK_COUNT,
	/*
	 * An spdk_thread may migrate to another pthread. A spinlock held across migration leads to
	 * undefined behavior. A spinlock held when an SPDK thread goes off CPU would lead to
	 * deadlock when another SPDK thread on the same pthread tries to take that lock.
	 */
	SPIN_ERR_HOLD_DURING_SWITCH,
	/* Trying to use a lock that was destroyed (but not re-initialized) */
	SPIN_ERR_DESTROYED,
	/* Trying to use a lock that is not initialized */
	SPIN_ERR_NOT_INITIALIZED,

	/* Must be last, not an actual error code */
	SPIN_ERR_LAST
};

static const char *spin_error_strings[] = {
	[SPIN_ERR_NONE]			= "No error",
	[SPIN_ERR_NOT_SPDK_THREAD]	= "Not an SPDK thread",
	[SPIN_ERR_DEADLOCK]		= "Deadlock detected",
	[SPIN_ERR_WRONG_THREAD]		= "Unlock on wrong SPDK thread",
	[SPIN_ERR_PTHREAD]		= "Error from pthread_spinlock",
	[SPIN_ERR_LOCK_HELD]		= "Destroying a held spinlock",
	[SPIN_ERR_LOCK_COUNT]		= "Lock count is invalid",
	[SPIN_ERR_HOLD_DURING_SWITCH]	= "Lock(s) held while SPDK thread going off CPU",
	[SPIN_ERR_DESTROYED]		= "Lock has been destroyed",
	[SPIN_ERR_NOT_INITIALIZED]	= "Lock has not been initialized",
};

#define SPIN_ERROR_STRING(err) (err < 0 || err >= SPDK_COUNTOF(spin_error_strings)) \
				? "Unknown error" : spin_error_strings[err]

static void
__posix_abort(enum spin_error err)
{
	abort();
}

typedef void (*spin_abort)(enum spin_error err);
spin_abort g_spin_abort_fn = __posix_abort;

#define SPIN_ASSERT_IMPL(cond, err, extra_log, ret) \
	do { \
		if (spdk_unlikely(!(cond))) { \
			SPDK_ERRLOG("unrecoverable spinlock error %d: %s (%s)\n", err, \
				    SPIN_ERROR_STRING(err), #cond); \
			extra_log; \
			g_spin_abort_fn(err); \
			ret; \
		} \
	} while (0)
#define SPIN_ASSERT_LOG_STACKS(cond, err, lock) \
	SPIN_ASSERT_IMPL(cond, err, sspin_stacks_print(sspin), return)
#define SPIN_ASSERT_RETURN(cond, err, ret)	SPIN_ASSERT_IMPL(cond, err, , return ret)
#define SPIN_ASSERT(cond, err)			SPIN_ASSERT_IMPL(cond, err, ,)

struct io_device {
	void				*io_device;
	char				name[SPDK_MAX_DEVICE_NAME_LEN + 1];
	spdk_io_channel_create_cb	create_cb;
	spdk_io_channel_destroy_cb	destroy_cb;
	spdk_io_device_unregister_cb	unregister_cb;
	struct spdk_thread		*unregister_thread;
	uint32_t			ctx_size;
	uint32_t			for_each_count;
	RB_ENTRY(io_device)		node;

	uint32_t			refcnt;

	bool				pending_unregister;
	bool				unregistered;
};

static RB_HEAD(io_device_tree, io_device) g_io_devices = RB_INITIALIZER(g_io_devices);

static int
io_device_cmp(struct io_device *dev1, struct io_device *dev2)
{
	return (dev1->io_device < dev2->io_device ? -1 : dev1->io_device > dev2->io_device);
}

RB_GENERATE_STATIC(io_device_tree, io_device, node, io_device_cmp);

static int
io_channel_cmp(struct spdk_io_channel *ch1, struct spdk_io_channel *ch2)
{
	return (ch1->dev < ch2->dev ? -1 : ch1->dev > ch2->dev);
}

RB_GENERATE_STATIC(io_channel_tree, spdk_io_channel, node, io_channel_cmp);

struct spdk_msg {
	spdk_msg_fn		fn;
	void			*arg;

	SLIST_ENTRY(spdk_msg)	link;
};

static struct spdk_mempool *g_spdk_msg_mempool = NULL;

static TAILQ_HEAD(, spdk_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);
static uint32_t g_thread_count = 0;

static __thread struct spdk_thread *tls_thread = NULL;

static void
thread_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"THREAD_IOCH_GET", TRACE_THREAD_IOCH_GET,
			OWNER_TYPE_NONE, OBJECT_NONE, 0,
			{{ "refcnt", SPDK_TRACE_ARG_TYPE_INT, 4 }}
		},
		{
			"THREAD_IOCH_PUT", TRACE_THREAD_IOCH_PUT,
			OWNER_TYPE_NONE, OBJECT_NONE, 0,
			{{ "refcnt", SPDK_TRACE_ARG_TYPE_INT, 4 }}
		}
	};

	spdk_trace_register_owner_type(OWNER_TYPE_THREAD, 't');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));
}
SPDK_TRACE_REGISTER_FN(thread_trace, "thread", TRACE_GROUP_THREAD)

/*
 * If this compare function returns zero when two next_run_ticks are equal,
 * the macro RB_INSERT() returns a pointer to the element with the same
 * next_run_tick.
 *
 * Fortunately, the macro RB_REMOVE() takes not a key but a pointer to the element
 * to remove as a parameter.
 *
 * Hence we allow RB_INSERT() to insert elements with the same keys on the right
 * side by returning 1 when two next_run_ticks are equal.
 */
static inline int
timed_poller_compare(struct spdk_poller *poller1, struct spdk_poller *poller2)
{
	if (poller1->next_run_tick < poller2->next_run_tick) {
		return -1;
	} else {
		return 1;
	}
}

RB_GENERATE_STATIC(timed_pollers_tree, spdk_poller, node, timed_poller_compare);

static inline struct spdk_thread *
_get_thread(void)
{
	return tls_thread;
}

static int
_thread_lib_init(size_t ctx_sz, size_t msg_mempool_sz)
{
	char mempool_name[SPDK_MAX_MEMZONE_NAME_LEN];

	g_ctx_sz = ctx_sz;

	snprintf(mempool_name, sizeof(mempool_name), "msgpool_%d", getpid());
	g_spdk_msg_mempool = spdk_mempool_create(mempool_name, msg_mempool_sz,
			     sizeof(struct spdk_msg),
			     0, /* No cache. We do our own. */
			     SPDK_ENV_NUMA_ID_ANY);

	SPDK_DEBUGLOG(thread, "spdk_msg_mempool was created with size: %zu\n",
		      msg_mempool_sz);

	if (!g_spdk_msg_mempool) {
		SPDK_ERRLOG("spdk_msg_mempool creation failed\n");
		return -ENOMEM;
	}

	return 0;
}

static void thread_interrupt_destroy(struct spdk_thread *thread);
static int thread_interrupt_create(struct spdk_thread *thread);

static void
_free_thread(struct spdk_thread *thread)
{
	struct spdk_io_channel *ch;
	struct spdk_msg *msg;
	struct spdk_poller *poller, *ptmp;

	RB_FOREACH(ch, io_channel_tree, &thread->io_channels) {
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

	RB_FOREACH_SAFE(poller, timed_pollers_tree, &thread->timed_pollers, ptmp) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_WARNLOG("timed_poller %s still registered at thread exit\n",
				     poller->name);
		}
		RB_REMOVE(timed_pollers_tree, &thread->timed_pollers, poller);
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

	if (spdk_interrupt_mode_is_enabled()) {
		thread_interrupt_destroy(thread);
	}

	spdk_ring_free(thread->messages);
	free(thread);
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

	return _thread_lib_init(ctx_sz, SPDK_DEFAULT_MSG_MEMPOOL_SIZE);
}

int
spdk_thread_lib_init_ext(spdk_thread_op_fn thread_op_fn,
			 spdk_thread_op_supported_fn thread_op_supported_fn,
			 size_t ctx_sz, size_t msg_mempool_sz)
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

	return _thread_lib_init(ctx_sz, msg_mempool_sz);
}

void
spdk_thread_lib_fini(void)
{
	struct io_device *dev;

	RB_FOREACH(dev, io_device_tree, &g_io_devices) {
		SPDK_ERRLOG("io_device %s not unregistered\n", dev->name);
	}

	g_new_thread_fn = NULL;
	g_thread_op_fn = NULL;
	g_thread_op_supported_fn = NULL;
	g_ctx_sz = 0;
	if (g_app_thread != NULL) {
		_free_thread(g_app_thread);
		g_app_thread = NULL;
	}

	if (g_spdk_msg_mempool) {
		spdk_mempool_free(g_spdk_msg_mempool);
		g_spdk_msg_mempool = NULL;
	}
}

struct spdk_thread *
spdk_thread_create(const char *name, const struct spdk_cpuset *cpumask)
{
	struct spdk_thread *thread, *null_thread;
	size_t size = SPDK_ALIGN_CEIL(sizeof(*thread) + g_ctx_sz, SPDK_CACHE_LINE_SIZE);
	struct spdk_msg *msgs[SPDK_MSG_MEMPOOL_CACHE_SIZE];
	int rc = 0, i;

	/* Since this spdk_thread object will be used by another core, ensure that it won't share a
	 * cache line with any other object allocated on this core */
	rc = posix_memalign((void **)&thread, SPDK_CACHE_LINE_SIZE, size);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to allocate memory for thread\n");
		return NULL;
	}
	memset(thread, 0, size);

	if (cpumask) {
		spdk_cpuset_copy(&thread->cpumask, cpumask);
	} else {
		spdk_cpuset_negate(&thread->cpumask);
	}

	RB_INIT(&thread->io_channels);
	TAILQ_INIT(&thread->active_pollers);
	RB_INIT(&thread->timed_pollers);
	TAILQ_INIT(&thread->paused_pollers);
	SLIST_INIT(&thread->msg_cache);
	thread->msg_cache_count = 0;

	thread->tsc_last = spdk_get_ticks();

	/* Monotonic increasing ID is set to each created poller beginning at 1. Once the
	 * ID exceeds UINT64_MAX a warning message is logged
	 */
	thread->next_poller_id = 1;

	thread->messages = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 65536, SPDK_ENV_NUMA_ID_ANY);
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

	thread->trace_id = spdk_trace_register_owner(OWNER_TYPE_THREAD, thread->name);

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
		thread->in_interrupt = true;
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

	/* If this is the first thread, save it as the app thread.  Use an atomic
	 * compare + exchange to guard against crazy users who might try to
	 * call spdk_thread_create() simultaneously on multiple threads.
	 */
	null_thread = NULL;
	__atomic_compare_exchange_n(&g_app_thread, &null_thread, thread, false,
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

	return thread;
}

struct spdk_thread *
spdk_thread_get_app_thread(void)
{
	return g_app_thread;
}

bool
spdk_thread_is_app_thread(struct spdk_thread *thread)
{
	if (thread == NULL) {
		thread = _get_thread();
	}

	return g_app_thread == thread;
}

void
spdk_thread_bind(struct spdk_thread *thread, bool bind)
{
	thread->is_bound = bind;
}

bool
spdk_thread_is_bound(struct spdk_thread *thread)
{
	return thread->is_bound;
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

	if (spdk_ring_count(thread->messages) > 0) {
		SPDK_INFOLOG(thread, "thread %s still has messages\n", thread->name);
		return;
	}

	if (thread->for_each_count > 0) {
		SPDK_INFOLOG(thread, "thread %s is still executing %u for_each_channels/threads\n",
			     thread->name, thread->for_each_count);
		return;
	}

	TAILQ_FOREACH(poller, &thread->active_pollers, tailq) {
		if (poller->state != SPDK_POLLER_STATE_UNREGISTERED) {
			SPDK_INFOLOG(thread,
				     "thread %s still has active poller %s\n",
				     thread->name, poller->name);
			return;
		}
	}

	RB_FOREACH(poller, timed_pollers_tree, &thread->timed_pollers) {
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

	RB_FOREACH(ch, io_channel_tree, &thread->io_channels) {
		SPDK_INFOLOG(thread,
			     "thread %s still has channel for io_device %s\n",
			     thread->name, ch->dev->name);
		return;
	}

	if (thread->pending_unregister_count > 0) {
		SPDK_INFOLOG(thread,
			     "thread %s is still unregistering io_devices\n",
			     thread->name);
		return;
	}

exited:
	thread->state = SPDK_THREAD_STATE_EXITED;
	if (spdk_unlikely(thread->in_interrupt)) {
		g_thread_op_fn(thread, SPDK_THREAD_OP_RESCHED);
	}
}

static void _thread_exit(void *ctx);

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

	if (spdk_interrupt_mode_is_enabled()) {
		spdk_thread_send_msg(thread, _thread_exit, thread);
	}

	return 0;
}

bool
spdk_thread_is_running(struct spdk_thread *thread)
{
	return thread->state == SPDK_THREAD_STATE_RUNNING;
}

bool
spdk_thread_is_exited(struct spdk_thread *thread)
{
	return thread->state == SPDK_THREAD_STATE_EXITED;
}

void
spdk_thread_destroy(struct spdk_thread *thread)
{
	assert(thread != NULL);
	SPDK_DEBUGLOG(thread, "Destroy thread %s\n", thread->name);

	assert(thread->state == SPDK_THREAD_STATE_EXITED);

	if (tls_thread == thread) {
		tls_thread = NULL;
	}

	/* To be safe, do not free the app thread until spdk_thread_lib_fini(). */
	if (thread != g_app_thread) {
		_free_thread(thread);
	}
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

	count = spdk_ring_dequeue(thread->messages, messages, max_msgs);
	if (spdk_unlikely(thread->in_interrupt) &&
	    spdk_ring_count(thread->messages) != 0) {
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

		SPDK_DTRACE_PROBE2(msg_exec, msg->fn, msg->arg);

		msg->fn(msg->arg);

		SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

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
	struct spdk_poller *tmp __attribute__((unused));

	poller->next_run_tick = now + poller->period_ticks;

	/*
	 * Insert poller in the thread's timed_pollers tree by next scheduled run time
	 * as its key.
	 */
	tmp = RB_INSERT(timed_pollers_tree, &thread->timed_pollers, poller);
	assert(tmp == NULL);

	/* Update the cache only if it is empty or the inserted poller is earlier than it.
	 * RB_MIN() is not necessary here because all pollers, which has exactly the same
	 * next_run_tick as the existing poller, are inserted on the right side.
	 */
	if (thread->first_timed_poller == NULL ||
	    poller->next_run_tick < thread->first_timed_poller->next_run_tick) {
		thread->first_timed_poller = poller;
	}
}

static inline void
poller_remove_timer(struct spdk_thread *thread, struct spdk_poller *poller)
{
	struct spdk_poller *tmp __attribute__((unused));

	tmp = RB_REMOVE(timed_pollers_tree, &thread->timed_pollers, poller);
	assert(tmp != NULL);

	/* This function is not used in any case that is performance critical.
	 * Update the cache simply by RB_MIN() if it needs to be changed.
	 */
	if (thread->first_timed_poller == poller) {
		thread->first_timed_poller = RB_MIN(timed_pollers_tree, &thread->timed_pollers);
	}
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

static inline int
thread_execute_poller(struct spdk_thread *thread, struct spdk_poller *poller)
{
	int rc;

	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		free(poller);
		return 0;
	case SPDK_POLLER_STATE_PAUSING:
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		return 0;
	case SPDK_POLLER_STATE_WAITING:
		break;
	default:
		assert(false);
		break;
	}

	poller->state = SPDK_POLLER_STATE_RUNNING;
	rc = poller->fn(poller->arg);

	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

	poller->run_count++;
	if (rc > 0) {
		poller->busy_count++;
	}

#ifdef DEBUG
	if (rc == -1) {
		SPDK_DEBUGLOG(thread, "Poller %s returned -1\n", poller->name);
	}
#endif

	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		free(poller);
		break;
	case SPDK_POLLER_STATE_PAUSING:
		TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		break;
	case SPDK_POLLER_STATE_PAUSED:
	case SPDK_POLLER_STATE_WAITING:
		break;
	case SPDK_POLLER_STATE_RUNNING:
		poller->state = SPDK_POLLER_STATE_WAITING;
		break;
	default:
		assert(false);
		break;
	}

	return rc;
}

static inline int
thread_execute_timed_poller(struct spdk_thread *thread, struct spdk_poller *poller,
			    uint64_t now)
{
	int rc;

	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		free(poller);
		return 0;
	case SPDK_POLLER_STATE_PAUSING:
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		return 0;
	case SPDK_POLLER_STATE_WAITING:
		break;
	default:
		assert(false);
		break;
	}

	poller->state = SPDK_POLLER_STATE_RUNNING;
	rc = poller->fn(poller->arg);

	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

	poller->run_count++;
	if (rc > 0) {
		poller->busy_count++;
	}

#ifdef DEBUG
	if (rc == -1) {
		SPDK_DEBUGLOG(thread, "Timed poller %s returned -1\n", poller->name);
	}
#endif

	switch (poller->state) {
	case SPDK_POLLER_STATE_UNREGISTERED:
		free(poller);
		break;
	case SPDK_POLLER_STATE_PAUSING:
		TAILQ_INSERT_TAIL(&thread->paused_pollers, poller, tailq);
		poller->state = SPDK_POLLER_STATE_PAUSED;
		break;
	case SPDK_POLLER_STATE_PAUSED:
		break;
	case SPDK_POLLER_STATE_RUNNING:
		poller->state = SPDK_POLLER_STATE_WAITING;
	/* fallthrough */
	case SPDK_POLLER_STATE_WAITING:
		poller_insert_timer(thread, poller, now);
		break;
	default:
		assert(false);
		break;
	}

	return rc;
}

static inline void
thread_run_pp_handlers(struct spdk_thread *thread)
{
	uint8_t i, count = thread->num_pp_handlers;

	/* Set to max value to prevent new handlers registration within the callback */
	thread->num_pp_handlers = SPDK_THREAD_MAX_POST_POLLER_HANDLERS;

	for (i = 0; i < count; i++) {
		thread->pp_handlers[i].fn(thread->pp_handlers[i].fn_arg);
		thread->pp_handlers[i].fn = NULL;
	}

	thread->num_pp_handlers = 0;
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
		rc = 1;
	}

	msg_count = msg_queue_run_batch(thread, max_msgs);
	if (msg_count) {
		rc = 1;
	}

	TAILQ_FOREACH_REVERSE_SAFE(poller, &thread->active_pollers,
				   active_pollers_head, tailq, tmp) {
		int poller_rc;

		poller_rc = thread_execute_poller(thread, poller);
		if (poller_rc > rc) {
			rc = poller_rc;
		}
		if (thread->num_pp_handlers) {
			thread_run_pp_handlers(thread);
		}
	}

	poller = thread->first_timed_poller;
	while (poller != NULL) {
		int timer_rc = 0;

		if (now < poller->next_run_tick) {
			break;
		}

		tmp = RB_NEXT(timed_pollers_tree, &thread->timed_pollers, poller);
		RB_REMOVE(timed_pollers_tree, &thread->timed_pollers, poller);

		/* Update the cache to the next timed poller in the list
		 * only if the current poller is still the closest, otherwise,
		 * do nothing because the cache has been already updated.
		 */
		if (thread->first_timed_poller == poller) {
			thread->first_timed_poller = tmp;
		}

		timer_rc = thread_execute_timed_poller(thread, poller, now);
		if (timer_rc > rc) {
			rc = timer_rc;
		}

		poller = tmp;
	}

	return rc;
}

static void
_thread_remove_pollers(void *ctx)
{
	struct spdk_thread *thread = ctx;
	struct spdk_poller *poller, *tmp;

	TAILQ_FOREACH_REVERSE_SAFE(poller, &thread->active_pollers,
				   active_pollers_head, tailq, tmp) {
		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			TAILQ_REMOVE(&thread->active_pollers, poller, tailq);
			free(poller);
		}
	}

	RB_FOREACH_SAFE(poller, timed_pollers_tree, &thread->timed_pollers, tmp) {
		if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
			poller_remove_timer(thread, poller);
			free(poller);
		}
	}

	thread->poller_unregistered = false;
}

static void
_thread_exit(void *ctx)
{
	struct spdk_thread *thread = ctx;

	assert(thread->state == SPDK_THREAD_STATE_EXITING);

	thread_exit(thread, spdk_get_ticks());

	if (thread->state != SPDK_THREAD_STATE_EXITED) {
		spdk_thread_send_msg(thread, _thread_exit, thread);
	}
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

	if (spdk_likely(!thread->in_interrupt)) {
		rc = thread_poll(thread, max_msgs, now);
		if (spdk_unlikely(thread->in_interrupt)) {
			/* The thread transitioned to interrupt mode during the above poll.
			 * Poll it one more time in case that during the transition time
			 * there is msg received without notification.
			 */
			rc = thread_poll(thread, max_msgs, now);
		}

		if (spdk_unlikely(thread->state == SPDK_THREAD_STATE_EXITING)) {
			thread_exit(thread, now);
		}
	} else {
		/* Non-block wait on thread's fd_group */
		rc = spdk_fd_group_wait(thread->fgrp, 0);
	}

	thread_update_stats(thread, spdk_get_ticks(), now, rc);

	tls_thread = orig_thread;

	return rc;
}

uint64_t
spdk_thread_next_poller_expiration(struct spdk_thread *thread)
{
	struct spdk_poller *poller;

	poller = thread->first_timed_poller;
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
	    RB_EMPTY(&thread->timed_pollers)) {
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

static inline int
thread_send_msg_notification(const struct spdk_thread *target_thread)
{
	uint64_t notify = 1;
	int rc;

	/* Not necessary to do notification if interrupt facility is not enabled */
	if (spdk_likely(!spdk_interrupt_mode_is_enabled())) {
		return 0;
	}

	/* When each spdk_thread can switch between poll and interrupt mode dynamically,
	 * after sending thread msg, it is necessary to check whether target thread runs in
	 * interrupt mode and then decide whether do event notification.
	 */
	if (spdk_unlikely(target_thread->in_interrupt)) {
		rc = write(target_thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify msg_queue: %s.\n", spdk_strerror(errno));
			return -EIO;
		}
	}

	return 0;
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

	return thread_send_msg_notification(thread);
}

int
spdk_thread_send_critical_msg(struct spdk_thread *thread, spdk_msg_fn fn)
{
	spdk_msg_fn expected = NULL;

	if (!__atomic_compare_exchange_n(&thread->critical_msg, &expected, fn, false, __ATOMIC_SEQ_CST,
					 __ATOMIC_SEQ_CST)) {
		return -EIO;
	}

	return thread_send_msg_notification(thread);
}

#ifdef __linux__
static int
interrupt_timerfd_process(void *arg)
{
	struct spdk_poller *poller = arg;
	uint64_t exp;
	int rc;

	/* clear the level of interval timer */
	rc = read(poller->intr->efd, &exp, sizeof(exp));
	if (rc < 0) {
		if (rc == -EAGAIN) {
			return 0;
		}

		return rc;
	}

	SPDK_DTRACE_PROBE2(timerfd_exec, poller->fn, poller->arg);

	return poller->fn(poller->arg);
}

static int
period_poller_interrupt_init(struct spdk_poller *poller)
{
	int timerfd;

	SPDK_DEBUGLOG(thread, "timerfd init for periodic poller %s\n", poller->name);
	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (timerfd < 0) {
		return -errno;
	}

	poller->intr = spdk_interrupt_register(timerfd, interrupt_timerfd_process, poller, poller->name);
	if (poller->intr == NULL) {
		close(timerfd);
		return -1;
	}

	return 0;
}

static void
period_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	int timerfd;
	uint64_t now_tick = spdk_get_ticks();
	uint64_t ticks = spdk_get_ticks_hz();
	int ret;
	struct itimerspec new_tv = {};
	struct itimerspec old_tv = {};

	assert(poller->intr != NULL);
	assert(poller->period_ticks != 0);

	timerfd = poller->intr->efd;

	assert(timerfd >= 0);

	SPDK_DEBUGLOG(thread, "timerfd set poller %s into %s mode\n", poller->name,
		      interrupt_mode ? "interrupt" : "poll");

	if (interrupt_mode) {
		/* Set repeated timer expiration */
		new_tv.it_interval.tv_sec = poller->period_ticks / ticks;
		new_tv.it_interval.tv_nsec = poller->period_ticks % ticks * SPDK_SEC_TO_NSEC / ticks;

		/* Update next timer expiration */
		if (poller->next_run_tick == 0) {
			poller->next_run_tick = now_tick + poller->period_ticks;
		} else if (poller->next_run_tick < now_tick) {
			poller->next_run_tick = now_tick;
		}

		new_tv.it_value.tv_sec = (poller->next_run_tick - now_tick) / ticks;
		new_tv.it_value.tv_nsec = (poller->next_run_tick - now_tick) % ticks * SPDK_SEC_TO_NSEC / ticks;

		ret = timerfd_settime(timerfd, 0, &new_tv, NULL);
		if (ret < 0) {
			SPDK_ERRLOG("Failed to arm timerfd: error(%d)\n", errno);
			assert(false);
		}
	} else {
		/* Disarm the timer */
		ret = timerfd_settime(timerfd, 0, &new_tv, &old_tv);
		if (ret < 0) {
			/* timerfd_settime's failure indicates that the timerfd is in error */
			SPDK_ERRLOG("Failed to disarm timerfd: error(%d)\n", errno);
			assert(false);
		}

		/* In order to reuse poller_insert_timer, fix now_tick, so next_run_tick would be
		 * now_tick + ticks * old_tv.it_value.tv_sec + (ticks * old_tv.it_value.tv_nsec) / SPDK_SEC_TO_NSEC
		 */
		now_tick = now_tick - poller->period_ticks + ticks * old_tv.it_value.tv_sec + \
			   (ticks * old_tv.it_value.tv_nsec) / SPDK_SEC_TO_NSEC;
		poller_remove_timer(poller->thread, poller);
		poller_insert_timer(poller->thread, poller, now_tick);
	}
}

static void
poller_interrupt_fini(struct spdk_poller *poller)
{
	int fd;

	SPDK_DEBUGLOG(thread, "interrupt fini for poller %s\n", poller->name);
	assert(poller->intr != NULL);
	fd = poller->intr->efd;
	spdk_interrupt_unregister(&poller->intr);
	close(fd);
}

static int
busy_poller_interrupt_init(struct spdk_poller *poller)
{
	int busy_efd;

	SPDK_DEBUGLOG(thread, "busy_efd init for busy poller %s\n", poller->name);
	busy_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (busy_efd < 0) {
		SPDK_ERRLOG("Failed to create eventfd for Poller(%s).\n", poller->name);
		return -errno;
	}

	poller->intr = spdk_interrupt_register(busy_efd, poller->fn, poller->arg, poller->name);
	if (poller->intr == NULL) {
		close(busy_efd);
		return -1;
	}

	return 0;
}

static void
busy_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	int busy_efd = poller->intr->efd;
	uint64_t notify = 1;
	int rc __attribute__((unused));

	assert(busy_efd >= 0);

	if (interrupt_mode) {
		/* Write without read on eventfd will get it repeatedly triggered. */
		if (write(busy_efd, &notify, sizeof(notify)) < 0) {
			SPDK_ERRLOG("Failed to set busy wait for Poller(%s).\n", poller->name);
		}
	} else {
		/* Read on eventfd will clear its level triggering. */
		rc = read(busy_efd, &notify, sizeof(notify));
	}
}

#else

static int
period_poller_interrupt_init(struct spdk_poller *poller)
{
	return -ENOTSUP;
}

static void
period_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
}

static void
poller_interrupt_fini(struct spdk_poller *poller)
{
}

static int
busy_poller_interrupt_init(struct spdk_poller *poller)
{
	return -ENOTSUP;
}

static void
busy_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
}

#endif

void
spdk_poller_register_interrupt(struct spdk_poller *poller,
			       spdk_poller_set_interrupt_mode_cb cb_fn,
			       void *cb_arg)
{
	assert(poller != NULL);
	assert(spdk_get_thread() == poller->thread);

	if (!spdk_interrupt_mode_is_enabled()) {
		return;
	}

	/* If this poller already had an interrupt, clean the old one up. */
	if (poller->intr != NULL) {
		poller_interrupt_fini(poller);
	}

	poller->set_intr_cb_fn = cb_fn;
	poller->set_intr_cb_arg = cb_arg;

	/* Set poller into interrupt mode if thread is in interrupt. */
	if (poller->thread->in_interrupt && poller->set_intr_cb_fn) {
		poller->set_intr_cb_fn(poller, poller->set_intr_cb_arg, true);
	}
}

static uint64_t
convert_us_to_ticks(uint64_t us)
{
	uint64_t quotient, remainder, ticks;

	if (us) {
		quotient = us / SPDK_SEC_TO_USEC;
		remainder = us % SPDK_SEC_TO_USEC;
		ticks = spdk_get_ticks_hz();

		return ticks * quotient + (ticks * remainder) / SPDK_SEC_TO_USEC;
	} else {
		return 0;
	}
}

static struct spdk_poller *
poller_register(spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds,
		const char *name)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller;

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
	poller->intr = NULL;
	if (thread->next_poller_id == 0) {
		SPDK_WARNLOG("Poller ID rolled over. Poller ID is duplicated.\n");
		thread->next_poller_id = 1;
	}
	poller->id = thread->next_poller_id++;

	poller->period_ticks = convert_us_to_ticks(period_microseconds);

	if (spdk_interrupt_mode_is_enabled()) {
		int rc;

		if (period_microseconds) {
			rc = period_poller_interrupt_init(poller);
			if (rc < 0) {
				SPDK_ERRLOG("Failed to register interruptfd for periodic poller: %s\n", spdk_strerror(-rc));
				free(poller);
				return NULL;
			}

			poller->set_intr_cb_fn = period_poller_set_interrupt_mode;
			poller->set_intr_cb_arg = NULL;

		} else {
			/* If the poller doesn't have a period, create interruptfd that's always
			 * busy automatically when running in interrupt mode.
			 */
			rc = busy_poller_interrupt_init(poller);
			if (rc > 0) {
				SPDK_ERRLOG("Failed to register interruptfd for busy poller: %s\n", spdk_strerror(-rc));
				free(poller);
				return NULL;
			}

			poller->set_intr_cb_fn = busy_poller_set_interrupt_mode;
			poller->set_intr_cb_arg = NULL;
		}

		/* Set poller into interrupt mode if thread is in interrupt. */
		if (poller->thread->in_interrupt) {
			poller->set_intr_cb_fn(poller, poller->set_intr_cb_arg, true);
		}
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

static void
wrong_thread(const char *func, const char *name, struct spdk_thread *thread,
	     struct spdk_thread *curthread)
{
	if (thread == NULL) {
		SPDK_ERRLOG("%s(%s) called with NULL thread\n", func, name);
		abort();
	}
	SPDK_ERRLOG("%s(%s) called from wrong thread %s:%" PRIu64 " (should be "
		    "%s:%" PRIu64 ")\n", func, name, curthread->name, curthread->id,
		    thread->name, thread->id);
	assert(false);
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
		wrong_thread(__func__, poller->name, poller->thread, thread);
		return;
	}

	if (spdk_interrupt_mode_is_enabled()) {
		/* Release the interrupt resource for period or busy poller */
		if (poller->intr != NULL) {
			poller_interrupt_fini(poller);
		}

		/* If there is not already a pending poller removal, generate
		 * a message to go process removals. */
		if (!thread->poller_unregistered) {
			thread->poller_unregistered = true;
			spdk_thread_send_msg(thread, _thread_remove_pollers, thread);
		}
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

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (poller->thread != thread) {
		wrong_thread(__func__, poller->name, poller->thread, thread);
		return;
	}

	/* We just set its state to SPDK_POLLER_STATE_PAUSING and let
	 * spdk_thread_poll() move it. It allows a poller to be paused from
	 * another one's context without breaking the TAILQ_FOREACH_REVERSE_SAFE
	 * iteration, or from within itself without breaking the logic to always
	 * remove the closest timed poller in the TAILQ_FOREACH_SAFE iteration.
	 */
	switch (poller->state) {
	case SPDK_POLLER_STATE_PAUSED:
	case SPDK_POLLER_STATE_PAUSING:
		break;
	case SPDK_POLLER_STATE_RUNNING:
	case SPDK_POLLER_STATE_WAITING:
		poller->state = SPDK_POLLER_STATE_PAUSING;
		break;
	default:
		assert(false);
		break;
	}
}

void
spdk_poller_resume(struct spdk_poller *poller)
{
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return;
	}

	if (poller->thread != thread) {
		wrong_thread(__func__, poller->name, poller->thread, thread);
		return;
	}

	/* If a poller is paused it has to be removed from the paused pollers
	 * list and put on the active list or timer tree depending on its
	 * period_ticks.  If a poller is still in the process of being paused,
	 * we just need to flip its state back to waiting, as it's already on
	 * the appropriate list or tree.
	 */
	switch (poller->state) {
	case SPDK_POLLER_STATE_PAUSED:
		TAILQ_REMOVE(&thread->paused_pollers, poller, tailq);
		thread_insert_poller(thread, poller);
	/* fallthrough */
	case SPDK_POLLER_STATE_PAUSING:
		poller->state = SPDK_POLLER_STATE_WAITING;
		break;
	case SPDK_POLLER_STATE_RUNNING:
	case SPDK_POLLER_STATE_WAITING:
		break;
	default:
		assert(false);
		break;
	}
}

const char *
spdk_poller_get_name(struct spdk_poller *poller)
{
	return poller->name;
}

uint64_t
spdk_poller_get_id(struct spdk_poller *poller)
{
	return poller->id;
}

const char *
spdk_poller_get_state_str(struct spdk_poller *poller)
{
	switch (poller->state) {
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

uint64_t
spdk_poller_get_period_ticks(struct spdk_poller *poller)
{
	return poller->period_ticks;
}

void
spdk_poller_get_stats(struct spdk_poller *poller, struct spdk_poller_stats *stats)
{
	stats->run_count = poller->run_count;
	stats->busy_count = poller->busy_count;
}

struct spdk_poller *
spdk_thread_get_first_active_poller(struct spdk_thread *thread)
{
	return TAILQ_FIRST(&thread->active_pollers);
}

struct spdk_poller *
spdk_thread_get_next_active_poller(struct spdk_poller *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

struct spdk_poller *
spdk_thread_get_first_timed_poller(struct spdk_thread *thread)
{
	return RB_MIN(timed_pollers_tree, &thread->timed_pollers);
}

struct spdk_poller *
spdk_thread_get_next_timed_poller(struct spdk_poller *prev)
{
	return RB_NEXT(timed_pollers_tree, &thread->timed_pollers, prev);
}

struct spdk_poller *
spdk_thread_get_first_paused_poller(struct spdk_thread *thread)
{
	return TAILQ_FIRST(&thread->paused_pollers);
}

struct spdk_poller *
spdk_thread_get_next_paused_poller(struct spdk_poller *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

struct spdk_io_channel *
spdk_thread_get_first_io_channel(struct spdk_thread *thread)
{
	return RB_MIN(io_channel_tree, &thread->io_channels);
}

struct spdk_io_channel *
spdk_thread_get_next_io_channel(struct spdk_io_channel *prev)
{
	return RB_NEXT(io_channel_tree, &thread->io_channels, prev);
}

uint16_t
spdk_thread_get_trace_id(struct spdk_thread *thread)
{
	return thread->trace_id;
}

struct call_thread {
	struct spdk_thread *cur_thread;
	spdk_msg_fn fn;
	void *ctx;

	struct spdk_thread *orig_thread;
	spdk_msg_fn cpl;
};

static void
_back_to_orig_thread(void *ctx)
{
	struct call_thread *ct = ctx;

	assert(ct->orig_thread->for_each_count > 0);
	ct->orig_thread->for_each_count--;

	ct->cpl(ct->ctx);
	free(ctx);
}

static void
_on_thread(void *ctx)
{
	struct call_thread *ct = ctx;
	int rc __attribute__((unused));

	ct->fn(ct->ctx);

	pthread_mutex_lock(&g_devlist_mutex);
	ct->cur_thread = TAILQ_NEXT(ct->cur_thread, tailq);
	while (ct->cur_thread && ct->cur_thread->state != SPDK_THREAD_STATE_RUNNING) {
		SPDK_DEBUGLOG(thread, "thread %s is not running but still not destroyed.\n",
			      ct->cur_thread->name);
		ct->cur_thread = TAILQ_NEXT(ct->cur_thread, tailq);
	}
	pthread_mutex_unlock(&g_devlist_mutex);

	if (!ct->cur_thread) {
		SPDK_DEBUGLOG(thread, "Completed thread iteration\n");

		rc = spdk_thread_send_msg(ct->orig_thread, _back_to_orig_thread, ctx);
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

	ct->orig_thread->for_each_count++;

	pthread_mutex_lock(&g_devlist_mutex);
	ct->cur_thread = TAILQ_FIRST(&g_threads);
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Starting thread iteration from %s\n",
		      ct->orig_thread->name);

	rc = spdk_thread_send_msg(ct->cur_thread, _on_thread, ct);
	assert(rc == 0);
}

static inline void
poller_set_interrupt_mode(struct spdk_poller *poller, bool interrupt_mode)
{
	if (poller->state == SPDK_POLLER_STATE_UNREGISTERED) {
		return;
	}

	if (poller->set_intr_cb_fn) {
		poller->set_intr_cb_fn(poller, poller->set_intr_cb_arg, interrupt_mode);
	}
}

void
spdk_thread_set_interrupt_mode(bool enable_interrupt)
{
	struct spdk_thread *thread = _get_thread();
	struct spdk_poller *poller, *tmp;

	assert(thread);
	assert(spdk_interrupt_mode_is_enabled());

	SPDK_NOTICELOG("Set spdk_thread (%s) to %s mode from %s mode.\n",
		       thread->name,  enable_interrupt ? "intr" : "poll",
		       thread->in_interrupt ? "intr" : "poll");

	if (thread->in_interrupt == enable_interrupt) {
		return;
	}

	/* Set pollers to expected mode */
	RB_FOREACH_SAFE(poller, timed_pollers_tree, &thread->timed_pollers, tmp) {
		poller_set_interrupt_mode(poller, enable_interrupt);
	}
	TAILQ_FOREACH_SAFE(poller, &thread->active_pollers, tailq, tmp) {
		poller_set_interrupt_mode(poller, enable_interrupt);
	}
	/* All paused pollers will go to work in interrupt mode */
	TAILQ_FOREACH_SAFE(poller, &thread->paused_pollers, tailq, tmp) {
		poller_set_interrupt_mode(poller, enable_interrupt);
	}

	thread->in_interrupt = enable_interrupt;
	return;
}

static struct io_device *
io_device_get(void *io_device)
{
	struct io_device find = {};

	find.io_device = io_device;
	return RB_FIND(io_device_tree, &g_io_devices, &find);
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
	tmp = RB_INSERT(io_device_tree, &g_io_devices, dev);
	if (tmp != NULL) {
		SPDK_ERRLOG("io_device %p already registered (old:%s new:%s)\n",
			    io_device, tmp->name, dev->name);
		free(dev);
	}

	pthread_mutex_unlock(&g_devlist_mutex);
}

static void
_finish_unregister(void *arg)
{
	struct io_device *dev = arg;
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	assert(thread == dev->unregister_thread);

	SPDK_DEBUGLOG(thread, "Finishing unregistration of io_device %s (%p) on thread %s\n",
		      dev->name, dev->io_device, thread->name);

	assert(thread->pending_unregister_count > 0);
	thread->pending_unregister_count--;

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
	dev = io_device_get(io_device);
	if (!dev) {
		SPDK_ERRLOG("io_device %p not found\n", io_device);
		assert(false);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	/* The for_each_count check differentiates the user attempting to unregister the
	 * device a second time, from the internal call to this function that occurs
	 * after the for_each_count reaches 0.
	 */
	if (dev->pending_unregister && dev->for_each_count > 0) {
		SPDK_ERRLOG("io_device %p already has a pending unregister\n", io_device);
		assert(false);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	dev->unregister_cb = unregister_cb;
	dev->unregister_thread = thread;

	if (dev->for_each_count > 0) {
		SPDK_WARNLOG("io_device %s (%p) has %u for_each calls outstanding\n",
			     dev->name, io_device, dev->for_each_count);
		dev->pending_unregister = true;
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	dev->unregistered = true;
	RB_REMOVE(io_device_tree, &g_io_devices, dev);
	refcnt = dev->refcnt;
	pthread_mutex_unlock(&g_devlist_mutex);

	SPDK_DEBUGLOG(thread, "Unregistering io_device %s (%p) from thread %s\n",
		      dev->name, dev->io_device, thread->name);

	if (unregister_cb) {
		thread->pending_unregister_count++;
	}

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

static struct spdk_io_channel *
thread_get_io_channel(struct spdk_thread *thread, struct io_device *dev)
{
	struct spdk_io_channel find = {};

	find.dev = dev;
	return RB_FIND(io_channel_tree, &thread->io_channels, &find);
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	struct spdk_io_channel *ch;
	struct spdk_thread *thread;
	struct io_device *dev;
	int rc;
	bool do_remove_dev = false;

	pthread_mutex_lock(&g_devlist_mutex);
	dev = io_device_get(io_device);
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

	ch = thread_get_io_channel(thread, dev);
	if (ch != NULL) {
		ch->ref++;

		SPDK_DEBUGLOG(thread, "Get io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
			      ch, dev->name, dev->io_device, thread->name, ch->ref);

		/*
		 * An I/O channel already exists for this device on this
		 *  thread, so return it.
		 */
		pthread_mutex_unlock(&g_devlist_mutex);
		spdk_trace_record(TRACE_THREAD_IOCH_GET, 0, 0,
				  (uint64_t)spdk_io_channel_get_ctx(ch), ch->ref);
		return ch;
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
	RB_INSERT(io_channel_tree, &thread->io_channels, ch);

	SPDK_DEBUGLOG(thread, "Get io_channel %p for io_device %s (%p) on thread %s refcnt %u\n",
		      ch, dev->name, dev->io_device, thread->name, ch->ref);

	dev->refcnt++;

	pthread_mutex_unlock(&g_devlist_mutex);

	rc = dev->create_cb(io_device, (uint8_t *)ch + sizeof(*ch));
	if (rc != 0) {
		pthread_mutex_lock(&g_devlist_mutex);
		RB_REMOVE(io_channel_tree, &ch->thread->io_channels, ch);
		dev->refcnt--;
		free(ch);
		SPDK_ERRLOG("could not create io_channel for io_device %s (%p): %s (rc=%d)\n",
			    dev->name, io_device, spdk_strerror(-rc), rc);
		if (dev->unregistered && dev->refcnt == 0) {
			/* During invokation of create_cb dev was unregistered, but was not removed due to refcnt */
			do_remove_dev = true;
		}
		pthread_mutex_unlock(&g_devlist_mutex);
		if (do_remove_dev) {
			io_device_free(dev);
		}
		return NULL;
	}

	spdk_trace_record(TRACE_THREAD_IOCH_GET, 0, 0, (uint64_t)spdk_io_channel_get_ctx(ch), 1);
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
	RB_REMOVE(io_channel_tree, &ch->thread->io_channels, ch);
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

	spdk_trace_record(TRACE_THREAD_IOCH_PUT, 0, 0,
			  (uint64_t)spdk_io_channel_get_ctx(ch), ch->ref);

	thread = spdk_get_thread();
	if (!thread) {
		SPDK_ERRLOG("called from non-SPDK thread\n");
		assert(false);
		return;
	}

	if (ch->thread != thread) {
		wrong_thread(__func__, "ch", ch->thread, thread);
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

void *
spdk_io_channel_get_io_device(struct spdk_io_channel *ch)
{
	return ch->dev->io_device;
}

const char *
spdk_io_channel_get_io_device_name(struct spdk_io_channel *ch)
{
	return spdk_io_device_get_name(ch->dev);
}

int
spdk_io_channel_get_ref_count(struct spdk_io_channel *ch)
{
	return ch->ref;
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

	assert(i->orig_thread->for_each_count > 0);
	i->orig_thread->for_each_count--;

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
	ch = thread_get_io_channel(i->cur_thread, i->dev);
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
		assert(false);
		return;
	}

	i->io_device = io_device;
	i->fn = fn;
	i->ctx = ctx;
	i->cpl = cpl;
	i->orig_thread = _get_thread();

	i->orig_thread->for_each_count++;

	pthread_mutex_lock(&g_devlist_mutex);
	i->dev = io_device_get(io_device);
	if (i->dev == NULL) {
		SPDK_ERRLOG("could not find io_device %p\n", io_device);
		assert(false);
		i->status = -ENODEV;
		goto end;
	}

	/* Do not allow new for_each operations if we are already waiting to unregister
	 * the device for other for_each operations to complete.
	 */
	if (i->dev->pending_unregister) {
		SPDK_ERRLOG("io_device %p has a pending unregister\n", io_device);
		i->status = -ENODEV;
		goto end;
	}

	TAILQ_FOREACH(thread, &g_threads, tailq) {
		ch = thread_get_io_channel(thread, i->dev);
		if (ch != NULL) {
			ch->dev->for_each_count++;
			i->cur_thread = thread;
			i->ch = ch;
			rc = spdk_thread_send_msg(thread, _call_channel, i);
			pthread_mutex_unlock(&g_devlist_mutex);
			assert(rc == 0);
			return;
		}
	}

end:
	pthread_mutex_unlock(&g_devlist_mutex);

	rc = spdk_thread_send_msg(i->orig_thread, _call_completion, i);
	assert(rc == 0);
}

static void
__pending_unregister(void *arg)
{
	struct io_device *dev = arg;

	assert(dev->pending_unregister);
	assert(dev->for_each_count == 0);
	spdk_io_device_unregister(dev->io_device, dev->unregister_cb);
}

void
spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch;
	struct io_device *dev;
	int rc __attribute__((unused));

	assert(i->cur_thread == spdk_get_thread());

	i->status = status;

	pthread_mutex_lock(&g_devlist_mutex);
	dev = i->dev;
	if (status) {
		goto end;
	}

	thread = TAILQ_NEXT(i->cur_thread, tailq);
	while (thread) {
		ch = thread_get_io_channel(thread, dev);
		if (ch != NULL) {
			i->cur_thread = thread;
			i->ch = ch;
			rc = spdk_thread_send_msg(thread, _call_channel, i);
			pthread_mutex_unlock(&g_devlist_mutex);
			assert(rc == 0);
			return;
		}
		thread = TAILQ_NEXT(thread, tailq);
	}

end:
	dev->for_each_count--;
	i->ch = NULL;
	pthread_mutex_unlock(&g_devlist_mutex);

	rc = spdk_thread_send_msg(i->orig_thread, _call_completion, i);
	assert(rc == 0);

	pthread_mutex_lock(&g_devlist_mutex);
	if (dev->pending_unregister && dev->for_each_count == 0) {
		rc = spdk_thread_send_msg(dev->unregister_thread, __pending_unregister, dev);
		assert(rc == 0);
	}
	pthread_mutex_unlock(&g_devlist_mutex);
}

static void
thread_interrupt_destroy(struct spdk_thread *thread)
{
	struct spdk_fd_group *fgrp = thread->fgrp;

	SPDK_INFOLOG(thread, "destroy fgrp for thread (%s)\n", thread->name);

	if (thread->msg_fd < 0) {
		return;
	}

	spdk_fd_group_remove(fgrp, thread->msg_fd);
	close(thread->msg_fd);
	thread->msg_fd = -1;

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
	uint64_t notify = 1;

	assert(spdk_interrupt_mode_is_enabled());

	orig_thread = spdk_get_thread();
	spdk_set_thread(thread);

	critical_msg = thread->critical_msg;
	if (spdk_unlikely(critical_msg != NULL)) {
		critical_msg(NULL);
		thread->critical_msg = NULL;
		rc = 1;
	}

	msg_count = msg_queue_run_batch(thread, 0);
	if (msg_count) {
		rc = 1;
	}

	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);
	if (spdk_unlikely(!thread->in_interrupt)) {
		/* The thread transitioned to poll mode in a msg during the above processing.
		 * Clear msg_fd since thread messages will be polled directly in poll mode.
		 */
		rc = read(thread->msg_fd, &notify, sizeof(notify));
		if (rc < 0 && errno != EAGAIN) {
			SPDK_ERRLOG("failed to acknowledge msg queue: %s.\n", spdk_strerror(errno));
		}
	}

	spdk_set_thread(orig_thread);
	return rc;
}

static int
thread_interrupt_create(struct spdk_thread *thread)
{
	struct spdk_event_handler_opts opts = {};
	int rc;

	SPDK_INFOLOG(thread, "Create fgrp for thread (%s)\n", thread->name);

	rc = spdk_fd_group_create(&thread->fgrp);
	if (rc) {
		thread->msg_fd = -1;
		return rc;
	}

	thread->msg_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (thread->msg_fd < 0) {
		rc = -errno;
		spdk_fd_group_destroy(thread->fgrp);
		thread->fgrp = NULL;

		return rc;
	}

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
	opts.fd_type = SPDK_FD_TYPE_EVENTFD;

	return SPDK_FD_GROUP_ADD_EXT(thread->fgrp, thread->msg_fd,
				     thread_interrupt_msg_process, thread, &opts);
}
#else
static int
thread_interrupt_create(struct spdk_thread *thread)
{
	return -ENOTSUP;
}
#endif

static int
_interrupt_wrapper(void *ctx)
{
	struct spdk_interrupt *intr = ctx;
	struct spdk_thread *orig_thread, *thread;
	int rc;

	orig_thread = spdk_get_thread();
	thread = intr->thread;

	spdk_set_thread(thread);

	SPDK_DTRACE_PROBE4(interrupt_fd_process, intr->name, intr->efd,
			   intr->fn, intr->arg);

	rc = intr->fn(intr->arg);

	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);

	spdk_set_thread(orig_thread);

	return rc;
}

struct spdk_interrupt *
spdk_interrupt_register(int efd, spdk_interrupt_fn fn,
			void *arg, const char *name)
{
	return spdk_interrupt_register_for_events(efd, SPDK_INTERRUPT_EVENT_IN, fn, arg, name);
}

struct spdk_interrupt *
spdk_interrupt_register_for_events(int efd, uint32_t events, spdk_interrupt_fn fn, void *arg,
				   const char *name)
{
	struct spdk_event_handler_opts opts = {};

	spdk_fd_group_get_default_event_handler_opts(&opts, sizeof(opts));
	opts.events = events;
	opts.fd_type = SPDK_FD_TYPE_DEFAULT;

	return spdk_interrupt_register_ext(efd, fn, arg, name, &opts);
}

static struct spdk_interrupt *
alloc_interrupt(int efd, struct spdk_fd_group *fgrp, spdk_interrupt_fn fn, void *arg,
		const char *name)
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

	assert(efd < 0 || fgrp == NULL);
	intr->efd = efd;
	intr->fgrp = fgrp;
	intr->thread = thread;
	intr->fn = fn;
	intr->arg = arg;

	return intr;
}

struct spdk_interrupt *
spdk_interrupt_register_ext(int efd, spdk_interrupt_fn fn, void *arg, const char *name,
			    struct spdk_event_handler_opts *opts)
{
	struct spdk_interrupt *intr;
	int ret;

	intr = alloc_interrupt(efd, NULL, fn, arg, name);
	if (intr == NULL) {
		return NULL;
	}

	ret = spdk_fd_group_add_ext(intr->thread->fgrp, efd,
				    _interrupt_wrapper, intr, intr->name, opts);
	if (ret != 0) {
		SPDK_ERRLOG("thread %s: failed to add fd %d: %s\n",
			    intr->thread->name, efd, spdk_strerror(-ret));
		free(intr);
		return NULL;
	}

	return intr;
}

static int
interrupt_fd_group_wrapper(void *wrap_ctx, spdk_fd_fn cb_fn, void *cb_ctx)
{
	struct spdk_interrupt *intr = wrap_ctx;
	struct spdk_thread *orig_thread, *thread;
	int rc;

	orig_thread = spdk_get_thread();
	thread = intr->thread;

	spdk_set_thread(thread);
	rc = cb_fn(cb_ctx);
	SPIN_ASSERT(thread->lock_count == 0, SPIN_ERR_HOLD_DURING_SWITCH);
	spdk_set_thread(orig_thread);

	return rc;
}

struct spdk_interrupt *
spdk_interrupt_register_fd_group(struct spdk_fd_group *fgrp, const char *name)
{
	struct spdk_interrupt *intr;
	int rc;

	intr = alloc_interrupt(-1, fgrp, NULL, NULL, name);
	if (intr == NULL) {
		return NULL;
	}

	rc = spdk_fd_group_set_wrapper(fgrp, interrupt_fd_group_wrapper, intr);
	if (rc != 0) {
		SPDK_ERRLOG("thread %s: failed to set wrapper for fd_group %d: %s\n",
			    intr->thread->name, spdk_fd_group_get_fd(fgrp), spdk_strerror(-rc));
		free(intr);
		return NULL;
	}

	rc = spdk_fd_group_nest(intr->thread->fgrp, fgrp);
	if (rc != 0) {
		SPDK_ERRLOG("thread %s: failed to nest fd_group %d: %s\n",
			    intr->thread->name, spdk_fd_group_get_fd(fgrp), spdk_strerror(-rc));
		spdk_fd_group_set_wrapper(fgrp, NULL, NULL);
		free(intr);
		return NULL;
	}

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
		wrong_thread(__func__, intr->name, intr->thread, thread);
		return;
	}

	if (intr->fgrp != NULL) {
		assert(intr->efd < 0);
		spdk_fd_group_unnest(thread->fgrp, intr->fgrp);
		spdk_fd_group_set_wrapper(thread->fgrp, NULL, NULL);
	} else {
		spdk_fd_group_remove(thread->fgrp, intr->efd);
	}

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
		wrong_thread(__func__, intr->name, intr->thread, thread);
		return -EINVAL;
	}

	if (intr->efd < 0) {
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

struct spdk_fd_group *
spdk_thread_get_interrupt_fd_group(struct spdk_thread *thread)
{
	return thread->fgrp;
}

static bool g_interrupt_mode = false;

int
spdk_interrupt_mode_enable(void)
{
	/* It must be called once prior to initializing the threading library.
	 * g_spdk_msg_mempool will be valid if thread library is initialized.
	 */
	if (g_spdk_msg_mempool) {
		SPDK_ERRLOG("Failed due to threading library is already initialized.\n");
		return -1;
	}

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

#define SSPIN_DEBUG_STACK_FRAMES 16

struct sspin_stack {
	void *addrs[SSPIN_DEBUG_STACK_FRAMES];
	uint32_t depth;
};

struct spdk_spinlock_internal {
	struct sspin_stack init_stack;
	struct sspin_stack lock_stack;
	struct sspin_stack unlock_stack;
};

static void
sspin_init_internal(struct spdk_spinlock *sspin)
{
#ifdef DEBUG
	sspin->internal = calloc(1, sizeof(*sspin->internal));
#endif
}

static void
sspin_fini_internal(struct spdk_spinlock *sspin)
{
#ifdef DEBUG
	free(sspin->internal);
	sspin->internal = NULL;
#endif
}

#if defined(DEBUG) && defined(SPDK_HAVE_EXECINFO_H)
#define SSPIN_GET_STACK(sspin, which) \
	do { \
		if (sspin->internal != NULL) { \
			struct sspin_stack *stack = &sspin->internal->which ## _stack; \
			stack->depth = backtrace(stack->addrs, SPDK_COUNTOF(stack->addrs)); \
		} \
	} while (0)
#else
#define SSPIN_GET_STACK(sspin, which) do { } while (0)
#endif

static void
sspin_stack_print(const char *title, const struct sspin_stack *sspin_stack)
{
#ifdef SPDK_HAVE_EXECINFO_H
	char **stack;
	size_t i;

	stack = backtrace_symbols(sspin_stack->addrs, sspin_stack->depth);
	if (stack == NULL) {
		SPDK_ERRLOG("Out of memory while allocate stack for %s\n", title);
		return;
	}
	SPDK_ERRLOG("  %s:\n", title);
	for (i = 0; i < sspin_stack->depth; i++) {
		/*
		 * This does not print line numbers. In gdb, use something like "list *0x444b6b" or
		 * "list *sspin_stack->addrs[0]".  Or more conveniently, load the spdk gdb macros
		 * and use use "print *sspin" or "print sspin->internal.lock_stack".  See
		 * gdb_macros.md in the docs directory for details.
		 */
		SPDK_ERRLOG("    #%" PRIu64 ": %s\n", i, stack[i]);
	}
	free(stack);
#endif /* SPDK_HAVE_EXECINFO_H */
}

static void
sspin_stacks_print(const struct spdk_spinlock *sspin)
{
	if (sspin->internal == NULL) {
		return;
	}
	SPDK_ERRLOG("spinlock %p\n", sspin);
	sspin_stack_print("Lock initialized at", &sspin->internal->init_stack);
	sspin_stack_print("Last locked at", &sspin->internal->lock_stack);
	sspin_stack_print("Last unlocked at", &sspin->internal->unlock_stack);
}

void
spdk_spin_init(struct spdk_spinlock *sspin)
{
	int rc;

	memset(sspin, 0, sizeof(*sspin));
	rc = pthread_spin_init(&sspin->spinlock, PTHREAD_PROCESS_PRIVATE);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);
	sspin_init_internal(sspin);
	SSPIN_GET_STACK(sspin, init);
	sspin->initialized = true;
}

void
spdk_spin_destroy(struct spdk_spinlock *sspin)
{
	int rc;

	SPIN_ASSERT_LOG_STACKS(!sspin->destroyed, SPIN_ERR_DESTROYED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->initialized, SPIN_ERR_NOT_INITIALIZED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->thread == NULL, SPIN_ERR_LOCK_HELD, sspin);

	rc = pthread_spin_destroy(&sspin->spinlock);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);

	sspin_fini_internal(sspin);
	sspin->initialized = false;
	sspin->destroyed = true;
}

void
spdk_spin_lock(struct spdk_spinlock *sspin)
{
	struct spdk_thread *thread = spdk_get_thread();
	int rc;

	SPIN_ASSERT_LOG_STACKS(!sspin->destroyed, SPIN_ERR_DESTROYED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->initialized, SPIN_ERR_NOT_INITIALIZED, sspin);
	SPIN_ASSERT_LOG_STACKS(thread != NULL, SPIN_ERR_NOT_SPDK_THREAD, sspin);
	SPIN_ASSERT_LOG_STACKS(thread != sspin->thread, SPIN_ERR_DEADLOCK, sspin);

	rc = pthread_spin_lock(&sspin->spinlock);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);

	sspin->thread = thread;
	sspin->thread->lock_count++;

	SSPIN_GET_STACK(sspin, lock);
}

void
spdk_spin_unlock(struct spdk_spinlock *sspin)
{
	struct spdk_thread *thread = spdk_get_thread();
	int rc;

	SPIN_ASSERT_LOG_STACKS(!sspin->destroyed, SPIN_ERR_DESTROYED, sspin);
	SPIN_ASSERT_LOG_STACKS(sspin->initialized, SPIN_ERR_NOT_INITIALIZED, sspin);
	SPIN_ASSERT_LOG_STACKS(thread != NULL, SPIN_ERR_NOT_SPDK_THREAD, sspin);
	SPIN_ASSERT_LOG_STACKS(thread == sspin->thread, SPIN_ERR_WRONG_THREAD, sspin);

	SPIN_ASSERT_LOG_STACKS(thread->lock_count > 0, SPIN_ERR_LOCK_COUNT, sspin);
	thread->lock_count--;
	sspin->thread = NULL;

	SSPIN_GET_STACK(sspin, unlock);

	rc = pthread_spin_unlock(&sspin->spinlock);
	SPIN_ASSERT_LOG_STACKS(rc == 0, SPIN_ERR_PTHREAD, sspin);
}

bool
spdk_spin_held(struct spdk_spinlock *sspin)
{
	struct spdk_thread *thread = spdk_get_thread();

	SPIN_ASSERT_RETURN(thread != NULL, SPIN_ERR_NOT_SPDK_THREAD, false);

	return sspin->thread == thread;
}

void
spdk_thread_register_post_poller_handler(spdk_post_poller_fn fn, void *fn_arg)
{
	struct spdk_thread *thr;

	thr = _get_thread();
	assert(thr);
	if (spdk_unlikely(thr->num_pp_handlers == SPDK_THREAD_MAX_POST_POLLER_HANDLERS)) {
		SPDK_ERRLOG("Too many handlers registered");
		return;
	}

	thr->pp_handlers[thr->num_pp_handlers].fn = fn;
	thr->pp_handlers[thr->num_pp_handlers].fn_arg = fn_arg;
	thr->num_pp_handlers++;
}

SPDK_LOG_REGISTER_COMPONENT(thread)
