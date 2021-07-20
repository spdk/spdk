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
	bool				resched;
	/* stats over a lifetime of a thread */
	struct spdk_thread_stats	total_stats;
	/* stats during the last scheduling period */
	struct spdk_thread_stats	current_stats;
};

/**
 * Completion callback to set reactor into interrupt mode or poll mode.
 *
 * \param cb_arg Argument to pass to the callback function.
 */
typedef void (*spdk_reactor_set_interrupt_mode_cb)(void *cb_arg);

struct spdk_reactor {
	/* Lightweight threads running on this reactor */
	TAILQ_HEAD(, spdk_lw_thread)			threads;
	uint32_t					thread_count;

	/* Logical core number for this reactor. */
	uint32_t					lcore;

	struct {
		uint32_t				is_valid : 1;
		uint32_t				reserved : 31;
	} flags;

	uint64_t					tsc_last;

	struct spdk_ring				*events;
	int						events_fd;

	/* The last known rusage values */
	struct rusage					rusage;
	uint64_t					last_rusage;

	uint64_t					busy_tsc;
	uint64_t					idle_tsc;

	/* Each bit of cpuset indicates whether a reactor probably requires event notification */
	struct spdk_cpuset				notify_cpuset;
	/* Indicate whether this reactor currently runs in interrupt */
	bool						in_interrupt;
	bool						set_interrupt_mode_in_progress;
	bool						new_in_interrupt;
	spdk_reactor_set_interrupt_mode_cb		set_interrupt_mode_cb_fn;
	void						*set_interrupt_mode_cb_arg;

	struct spdk_fd_group				*fgrp;
	int						resched_fd;
} __attribute__((aligned(SPDK_CACHE_LINE_SIZE)));

int spdk_reactors_init(void);
void spdk_reactors_fini(void);

void spdk_reactors_start(void);
void spdk_reactors_stop(void *arg1);

struct spdk_reactor *spdk_reactor_get(uint32_t lcore);

extern bool g_scheduling_in_progress;

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

/**
 * Set reactor into interrupt mode or back to poll mode.
 *
 * Currently, this function is only permitted within spdk application thread.
 * Also it requires the corresponding reactor does not have any spdk_thread.
 *
 * \param lcore CPU core index of specified reactor.
 * \param new_in_interrupt Set interrupt mode for true, or poll mode for false.
 * \param cb_fn This will be called on spdk application thread after setting interupt mode.
 * \param cb_arg Argument will be passed to cb_fn when called.
 *
 * \return 0 on success, negtive errno on failure.
 */
int spdk_reactor_set_interrupt_mode(uint32_t lcore, bool new_in_interrupt,
				    spdk_reactor_set_interrupt_mode_cb cb_fn, void *cb_arg);

/**
 * Get a handle to spdk application thread.
 *
 * \return a pointer to spdk application thread on success or NULL on failure.
 */
struct spdk_thread *_spdk_get_app_thread(void);

struct spdk_governor_capabilities {
	bool priority; /* Core with higher base frequency */
};

/**
 * Cores governor
 * Implements core frequency control for schedulers. Functions from this structure
 * are invoked from scheduling reactor.
 */
struct spdk_governor {
	const char *name;

	/**
	 * Get current frequency of a given core.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return Currently set core frequency.
	 */
	uint32_t (*get_core_curr_freq)(uint32_t lcore_id);

	/**
	 * Increase core frequency to next available one.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at max frequency, negative on error.
	 */
	int (*core_freq_up)(uint32_t lcore_id);

	/**
	 * Decrease core frequency to next available one.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at min frequency, negative on error.
	 */
	int (*core_freq_down)(uint32_t lcore_id);

	/**
	 * Set core frequency to maximum available.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at max frequency, negative on error.
	 */
	int (*set_core_freq_max)(uint32_t lcore_id);

	/**
	 * Set core frequency to minimum available.
	 *
	 * \param lcore_id Core number.
	 *
	 * \return 1 on success, 0 already at min frequency, negative on error.
	 */
	int (*set_core_freq_min)(uint32_t lcore_id);

	/**
	 * Get capabilities of a given core.
	 *
	 * \param lcore_id Core number.
	 * \param capabilities Structure to fill with capabilities data.
	 *
	 * \return 0 on success, negative on error.
	 */
	int (*get_core_capabilities)(uint32_t lcore_id, struct spdk_governor_capabilities *capabilities);

	/**
	 * Initialize a governor.
	 *
	 * \return 0 on success, non-zero on error.
	 */
	int (*init)(void);

	/**
	 * Deinitialize a governor.
	 */
	void (*deinit)(void);

	TAILQ_ENTRY(spdk_governor) link;
};

/**
 * Set the current governor.
 *
 * Always deinitalizes previously set governor.
 * No governor will be set if name parameter is NULL.
 * This function should be invoked on scheduling reactor.
 *
 * \param name Name of the governor to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
int _spdk_governor_set(const char *name);

/**
 * Get currently set governor.
 *
 * \return a pointer to spdk governor or NULL if none is set.
 */
struct spdk_governor *_spdk_governor_get(void);

/**
 * Add the given governor to the list of registered governors.
 * This function should be invoked by referencing the macro
 * SPDK_GOVERNOR_REGISTER in the governor c file.
 *
 * \param governor Governor to be added.
 *
 * \return 0 on success or non-zero on failure.
 */
void _spdk_governor_register(struct spdk_governor *governor);

/**
 * Macro used to register new governors.
 */
#define SPDK_GOVERNOR_REGISTER(governor) \
	static void __attribute__((constructor)) _spdk_governor_register_ ## governor(void) \
	{ \
		_spdk_governor_register(&governor); \
	} \

/**
 * Structure representing thread used for scheduling.
 */
struct spdk_scheduler_thread_info {
	uint32_t lcore;
	uint64_t thread_id;
	/* stats over a lifetime of a thread */
	struct spdk_thread_stats total_stats;
	/* stats during the last scheduling period */
	struct spdk_thread_stats current_stats;
};

/**
 * A list of cores and threads which is used for scheduling.
 */
struct spdk_scheduler_core_info {
	/* stats over a lifetime of a core */
	uint64_t total_idle_tsc;
	uint64_t total_busy_tsc;
	/* stats during the last scheduling period */
	uint64_t current_idle_tsc;
	uint64_t current_busy_tsc;

	uint32_t lcore;
	uint32_t threads_count;
	bool interrupt_mode;
	struct spdk_scheduler_thread_info *thread_infos;
};

/**
 * Thread scheduler.
 * Functions from this structure are invoked from scheduling reactor.
 */
struct spdk_scheduler {
	const char *name;

	/**
	 * This function is called to initialize a scheduler.
	 *
	 * \return 0 on success or non-zero on failure.
	 */
	int (*init)(void);

	/**
	 * This function is called to deinitialize a scheduler.
	 */
	void (*deinit)(void);

	/**
	 * Function to balance threads across cores by modifying
	 * the value of their lcore field.
	 *
	 * \param core_info Structure describing cores and threads on them.
	 * \param count Size of the core_info array.
	 */
	void (*balance)(struct spdk_scheduler_core_info *core_info, uint32_t count);

	TAILQ_ENTRY(spdk_scheduler)	link;
};

/**
 * Change current scheduler. If another scheduler was used prior,
 * it will be deinitalized. No scheduler will be set if name parameter
 * is NULL.
 * This function should be invoked from scheduling reactor.
 *
 * \param name Name of the scheduler to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
int _spdk_scheduler_set(const char *name);

/**
 * Get currently set scheduler.
 *
 * \return a pointer to spdk scheduler or NULL if none is set.
 */
struct spdk_scheduler *_spdk_scheduler_get(void);

/**
 * Change current scheduling period.
 * Setting period to 0 disables scheduling.
 *
 * \param period Period to set in microseconds.
 */
void _spdk_scheduler_set_period(uint64_t period);

/**
 * Get scheduling period of currently set scheduler.
 *
 * \return Scheduling period in microseconds.
 */
uint64_t _spdk_scheduler_get_period(void);

/**
 * Add the given scheduler to the list of registered schedulers.
 * This function should be invoked by referencing the macro
 * SPDK_SCHEDULER_REGISTER in the scheduler c file.
 *
 * \param scheduler Scheduler to be added.
 */
void _spdk_scheduler_register(struct spdk_scheduler *scheduler);

/*
 * Macro used to register new scheduler.
 */
#define SPDK_SCHEDULER_REGISTER(scheduler) \
static void __attribute__((constructor)) _spdk_scheduler_register_ ## scheduler (void) \
{ \
	_spdk_scheduler_register(&scheduler); \
} \

#ifdef __cplusplus
}
#endif

#endif /* SPDK_INTERNAL_EVENT_H */
