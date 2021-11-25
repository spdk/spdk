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
 * \param cb_fn This will be called on spdk application thread after setting interrupt mode.
 * \param cb_arg Argument will be passed to cb_fn when called.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_reactor_set_interrupt_mode(uint32_t lcore, bool new_in_interrupt,
				    spdk_reactor_set_interrupt_mode_cb cb_fn, void *cb_arg);

/**
 * Get a handle to spdk application thread.
 *
 * \return a pointer to spdk application thread on success or NULL on failure.
 */
struct spdk_thread *_spdk_get_app_thread(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_INTERNAL_EVENT_H */
