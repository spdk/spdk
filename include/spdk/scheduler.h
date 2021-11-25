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

#ifndef SPDK_SCHEDULER_H
#define SPDK_SCHEDULER_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/event.h"
#include "spdk/json.h"
#include "spdk/thread.h"
#include "spdk/util.h"

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
 * Always deinitializes previously set governor.
 * No governor will be set if name parameter is NULL.
 * This function should be invoked on scheduling reactor.
 *
 * \param name Name of the governor to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
int spdk_governor_set(const char *name);

/**
 * Get currently set governor.
 *
 * \return a pointer to spdk governor or NULL if none is set.
 */
struct spdk_governor *spdk_governor_get(void);

/**
 * Add the given governor to the list of registered governors.
 * This function should be invoked by referencing the macro
 * SPDK_GOVERNOR_REGISTER in the governor c file.
 *
 * \param governor Governor to be added.
 *
 * \return 0 on success or non-zero on failure.
 */
void spdk_governor_register(struct spdk_governor *governor);

/**
 * Macro used to register new governors.
 */
#define SPDK_GOVERNOR_REGISTER(governor) \
	static void __attribute__((constructor)) _spdk_governor_register_ ## governor(void) \
	{ \
		spdk_governor_register(&governor); \
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
 * it will be deinitialized. No scheduler will be set if name parameter
 * is NULL.
 * This function should be invoked from scheduling reactor.
 *
 * \param name Name of the scheduler to be used.
 *
 * \return 0 on success or non-zero on failure.
 */
int spdk_scheduler_set(const char *name);

/**
 * Get currently set scheduler.
 *
 * \return a pointer to spdk scheduler or NULL if none is set.
 */
struct spdk_scheduler *spdk_scheduler_get(void);

/**
 * Change current scheduling period.
 * Setting period to 0 disables scheduling.
 *
 * \param period Period to set in microseconds.
 */
void spdk_scheduler_set_period(uint64_t period);

/**
 * Get scheduling period of currently set scheduler.
 *
 * \return Scheduling period in microseconds.
 */
uint64_t spdk_scheduler_get_period(void);

/**
 * Add the given scheduler to the list of registered schedulers.
 * This function should be invoked by referencing the macro
 * SPDK_SCHEDULER_REGISTER in the scheduler c file.
 *
 * \param scheduler Scheduler to be added.
 */
void spdk_scheduler_register(struct spdk_scheduler *scheduler);

/*
 * Macro used to register new scheduler.
 */
#define SPDK_SCHEDULER_REGISTER(scheduler) \
static void __attribute__((constructor)) _spdk_scheduler_register_ ## scheduler (void) \
{ \
	spdk_scheduler_register(&scheduler); \
} \

#ifdef __cplusplus
}
#endif

#endif /* SPDK_SCHEDULER_H */
