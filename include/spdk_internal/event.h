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

#ifndef SPDK_INTERNAL_EVENT_H
#define SPDK_INTERNAL_EVENT_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/event.h"
#include "spdk/json.h"
#include "spdk/thread.h"
#include "spdk/util.h"

struct spdk_event {
	uint32_t		lcore;
	spdk_event_fn		fn;
	void			*arg1;
	void			*arg2;
};

enum spdk_reactor_state {
	SPDK_REACTOR_STATE_UNINITIALIZED = 0,
	SPDK_REACTOR_STATE_INITIALIZED = 1,
	SPDK_REACTOR_STATE_RUNNING = 2,
	SPDK_REACTOR_STATE_EXITING = 3,
	SPDK_REACTOR_STATE_SHUTDOWN = 4,
};

struct spdk_lw_thread {
	TAILQ_ENTRY(spdk_lw_thread)	link;
	uint64_t			tsc_start;
	uint32_t                        lcore;
	uint32_t                        new_lcore;
	bool				resched;
	struct spdk_thread_stats        current_stats;
};

struct spdk_reactor {
	/* Lightweight threads running on this reactor */
	TAILQ_HEAD(, spdk_lw_thread)			threads;
	uint32_t					thread_count;

	/* Logical core number for this reactor. */
	uint32_t					lcore;

	struct {
		uint32_t				is_valid : 1;
		uint32_t				is_scheduling : 1;
		uint32_t				reserved : 30;
	} flags;

	uint64_t					tsc_last;

	struct spdk_ring				*events;
	int						events_fd;

	/* The last known rusage values */
	struct rusage					rusage;
	uint64_t					last_rusage;

	uint64_t					busy_tsc;
	uint64_t					idle_tsc;

	bool						interrupt_mode;
	struct spdk_fd_group				*fgrp;
	int						resched_fd;
} __attribute__((aligned(SPDK_CACHE_LINE_SIZE)));

int spdk_reactors_init(void);
void spdk_reactors_fini(void);

void spdk_reactors_start(void);
void spdk_reactors_stop(void *arg1);

struct spdk_reactor *spdk_reactor_get(uint32_t lcore);

/**
 * Allocate and pass an event to each reactor, serially.
 *
 * The allocated event is processed asynchronously - i.e. spdk_for_each_reactor
 * will return prior to `fn` being called on each reactor.
 *
 * \param fn This is the function that will be called on each reactor.
 * \param arg1 Argument will be passed to fn when called.
 * \param arg2 Argument will be passed to fn when called.
 * \param cpl This will be called on the originating reactor after `fn` has been
 * called on each reactor.
 */
void spdk_for_each_reactor(spdk_event_fn fn, void *arg1, void *arg2, spdk_event_fn cpl);

struct spdk_subsystem {
	const char *name;
	/* User must call spdk_subsystem_init_next() when they are done with their initialization. */
	void (*init)(void);
	void (*fini)(void);

	/**
	 * Write JSON configuration handler.
	 *
	 * \param w JSON write context
	 */
	void (*write_config_json)(struct spdk_json_write_ctx *w);
	TAILQ_ENTRY(spdk_subsystem) tailq;
};

struct spdk_subsystem *spdk_subsystem_find(const char *name);
struct spdk_subsystem *spdk_subsystem_get_first(void);
struct spdk_subsystem *spdk_subsystem_get_next(struct spdk_subsystem *cur_subsystem);

struct spdk_subsystem_depend {
	const char *name;
	const char *depends_on;
	TAILQ_ENTRY(spdk_subsystem_depend) tailq;
};

struct spdk_subsystem_depend *spdk_subsystem_get_first_depend(void);
struct spdk_subsystem_depend *spdk_subsystem_get_next_depend(struct spdk_subsystem_depend
		*cur_depend);

void spdk_add_subsystem(struct spdk_subsystem *subsystem);
void spdk_add_subsystem_depend(struct spdk_subsystem_depend *depend);

typedef void (*spdk_subsystem_init_fn)(int rc, void *ctx);
void spdk_subsystem_init(spdk_subsystem_init_fn cb_fn, void *cb_arg);
void spdk_subsystem_fini(spdk_msg_fn cb_fn, void *cb_arg);
void spdk_subsystem_init_next(int rc);
void spdk_subsystem_fini_next(void);
void spdk_app_json_config_load(const char *json_config_file, const char *rpc_addr,
			       spdk_subsystem_init_fn cb_fn, void *cb_arg,
			       bool stop_on_error);

/**
 * Save pointed \c subsystem configuration to the JSON write context \c w. In case of
 * error \c null is written to the JSON context.
 *
 * \param w JSON write context
 * \param subsystem the subsystem to query
 */
void spdk_subsystem_config_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem);

void spdk_rpc_initialize(const char *listen_addr);
void spdk_rpc_finish(void);

struct spdk_governor_capabilities {
	bool freq_change;
	bool freq_getset;
	bool freq_up;
	bool freq_down;
	bool freq_max;
	bool freq_min;
	bool turbo_set;
	bool turbo_available;
	bool priority;
};

/** Cores governor */
struct spdk_governor {
	char *name;

	/* freqs - the buffer array to save the frequencies; num - the number of frequencies to get; return - the number of available frequencies */
	uint32_t (*get_core_freqs)(uint32_t lcore_id, uint32_t *freqs, uint32_t num);

	/* return - current frequency */
	uint32_t (*get_core_curr_freq)(uint32_t lcore_id);

	/**
	 * freq_index - index of available frequencies returned from get_core_freqs call
	 *
	 * return
	 *  - 1 on success with frequency changed.
	 *  - 0 on success without frequency changed.
	 *  - Negative on error.
	 */
	int (*set_core_freq)(uint32_t lcore_id, uint32_t freq_index);
	int (*core_freq_up)(uint32_t lcore_id);
	int (*core_freq_down)(uint32_t lcore_id);
	int (*set_core_freq_max)(uint32_t lcore_id);
	int (*set_core_freq_min)(uint32_t lcore_id);

	/**
	 * return
	 *  - 1 Turbo Boost is enabled for this lcore.
	 *  - 0 Turbo Boost is disabled for this lcore.
	 *  - Negative on error.
	 */
	int (*get_core_turbo_status)(uint32_t lcore_id);

	/* return - 0 on success; negative on error */
	int (*enable_core_turbo)(uint32_t lcore_id);
	int (*disable_core_turbo)(uint32_t lcore_id);
	int (*get_core_capabilities)(uint32_t lcore_id, struct spdk_governor_capabilities *capabilities);
	int (*init_core)(uint32_t lcore_id);
	int (*deinit_core)(uint32_t lcore_id);
	int (*init)(void);
	int (*deinit)(void);

	TAILQ_ENTRY(spdk_governor) link;
};

/**
 * Add the given governor to the list of registered governors.
 * This function should be invoked by referencing the macro
 * SPDK_GOVERNOR_REGISTER in the governor c file.
 *
 * \param governor Governor to be added.
 *
 * \return 0 on success or non-zero on failure.
 */
void _spdk_governor_list_add(struct spdk_governor *governor);

/**
 * Change current governor.
 *
 * \param name Name of the governor to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
int _spdk_governor_set(char *name);

/**
 * Macro used to register new cores governor.
 */
#define SPDK_GOVERNOR_REGISTER(governor) \
	static void __attribute__((constructor)) _spdk_governor_register_##name(void) \
	{ \
		_spdk_governor_list_add(governor); \
	} \

/**
 * A list of cores and threads which is used for scheduling.
 */
struct spdk_scheduler_core_info {
	uint64_t core_idle_tsc;
	uint64_t core_busy_tsc;
	uint32_t lcore;
	uint32_t threads_count;
	struct spdk_lw_thread **threads;
};

/**
 * Scheduler balance function type.
 * Accepts array of core_info which is of size 'count' and returns updated array.
 */
typedef void (*spdk_scheduler_balance_fn)(struct spdk_scheduler_core_info *core_info, int count,
		struct spdk_governor *governor);

/**
 * Scheduler init function type.
 * Called on scheduler module initialization.
 */
typedef int (*spdk_scheduler_init_fn)(struct spdk_governor *governor);

/**
 * Scheduler deinitialization function type.
 * Called on reactor fini.
 */
typedef int (*spdk_scheduler_deinit_fn)(struct spdk_governor *governor);

/** Thread scheduler */
struct spdk_scheduler {
	char                        *name;
	spdk_scheduler_init_fn       init;
	spdk_scheduler_deinit_fn     deinit;
	spdk_scheduler_balance_fn    balance;
	TAILQ_ENTRY(spdk_scheduler)  link;
};

/**
 * Add the given scheduler to the list of registered schedulers.
 * This function should be invoked by referencing the macro
 * SPDK_SCHEDULER_REGISTER in the scheduler c file.
 *
 * \param scheduler Scheduler to be added.
 */
void _spdk_scheduler_list_add(struct spdk_scheduler *scheduler);

/**
 * Change current scheduler.
 *
 * \param name Name of the scheduler to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
int _spdk_scheduler_set(char *name);

/**
 * Change current scheduling period.
 *
 * \param period New period (ticks).
 *               Use spdk_get_ticks_hz() to translate seconds to ticks.
 */
void _spdk_scheduler_period_set(uint32_t period);

/*
 * Macro used to register new reactor balancer.
 */
#define SPDK_SCHEDULER_REGISTER(scheduler) \
static void __attribute__((constructor)) _spdk_scheduler_register_##name(void) \
{ \
	_spdk_scheduler_list_add(scheduler); \
} \

/**
 * Set new CPU core index. Used for scheduling, assigns new CPU core index and marks it =
 * for rescheduling - does not actually change it. Can be used with SPDK_ENV_LCORE_ID_ANY
 *
 * \param thread thread to change core.
 * \param lcore new CPU core index.
 */
void _spdk_lw_thread_set_core(struct spdk_lw_thread *thread, uint32_t lcore);

/**
 * Get threads stats
 *
 * \param thread thread that stats regards to.
 * \param stats Output parameter for accumulated TSC counts while the thread was busy.
 */
void _spdk_lw_thread_get_current_stats(struct spdk_lw_thread *thread,
				       struct spdk_thread_stats *stats);

/**
 * \brief Register a new subsystem
 */
#define SPDK_SUBSYSTEM_REGISTER(_name) \
	__attribute__((constructor)) static void _name ## _register(void)	\
	{									\
		spdk_add_subsystem(&_name);					\
	}

/**
 * \brief Declare that a subsystem depends on another subsystem.
 */
#define SPDK_SUBSYSTEM_DEPEND(_name, _depends_on)						\
	static struct spdk_subsystem_depend __subsystem_ ## _name ## _depend_on ## _depends_on = { \
	.name = #_name,										\
	.depends_on = #_depends_on,								\
	};											\
	__attribute__((constructor)) static void _name ## _depend_on ## _depends_on(void)	\
	{											\
		spdk_add_subsystem_depend(&__subsystem_ ## _name ## _depend_on ## _depends_on); \
	}

#ifdef __cplusplus
}
#endif

#endif /* SPDK_INTERNAL_EVENT_H */
