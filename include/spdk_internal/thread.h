/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
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

#ifndef SPDK_THREAD_INTERNAL_H_
#define SPDK_THREAD_INTERNAL_H_

#include "spdk/stdinc.h"
#include "spdk/thread.h"

#define SPDK_MAX_THREAD_NAME_LEN	256

struct spdk_poller;

struct spdk_poller_stats {
	uint64_t	run_count;
	uint64_t	busy_count;
};

enum spdk_thread_state {
	/* The thread is pocessing poller and message by spdk_thread_poll(). */
	SPDK_THREAD_STATE_RUNNING,

	/* The thread is in the process of termination. It reaps unregistering
	 * poller are releasing I/O channel.
	 */
	SPDK_THREAD_STATE_EXITING,

	/* The thread is exited. It is ready to call spdk_thread_destroy(). */
	SPDK_THREAD_STATE_EXITED,
};

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
	TAILQ_HEAD(timed_pollers_head, spdk_poller)	timed_pollers;
	/*
	 * Contains paused pollers.  Pollers on this queue are waiting until
	 * they are resumed (in which case they're put onto the active/timer
	 * queues) or unregistered.
	 */
	TAILQ_HEAD(paused_pollers_head, spdk_poller)	paused_pollers;
	struct spdk_ring		*messages;
	int				msg_fd;
	SLIST_HEAD(, spdk_msg)		msg_cache;
	size_t				msg_cache_count;
	spdk_msg_fn			critical_msg;
	uint64_t			id;
	enum spdk_thread_state		state;
	int				pending_unregister_count;

	TAILQ_HEAD(, spdk_io_channel)	io_channels;
	TAILQ_ENTRY(spdk_thread)	tailq;

	char				name[SPDK_MAX_THREAD_NAME_LEN + 1];
	struct spdk_cpuset		cpumask;
	uint64_t			exit_timeout_tsc;

	/* Indicates whether this spdk_thread currently runs in interrupt. */
	bool				in_interrupt;
	struct spdk_fd_group		*fgrp;

	/* User context allocated at the end */
	uint8_t				ctx[0];
};

const char *spdk_poller_get_name(struct spdk_poller *poller);
const char *spdk_poller_get_state_str(struct spdk_poller *poller);
uint64_t spdk_poller_get_period_ticks(struct spdk_poller *poller);
void spdk_poller_get_stats(struct spdk_poller *poller, struct spdk_poller_stats *stats);

int spdk_io_channel_get_ref_count(struct spdk_io_channel *ch);

const char *spdk_io_device_get_name(struct io_device *dev);

struct spdk_poller *spdk_thread_get_first_active_poller(struct spdk_thread *thread);
struct spdk_poller *spdk_thread_get_next_active_poller(struct spdk_poller *prev);
struct spdk_poller *spdk_thread_get_first_timed_poller(struct spdk_thread *thread);
struct spdk_poller *spdk_thread_get_next_timed_poller(struct spdk_poller *prev);
struct spdk_poller *spdk_thread_get_first_paused_poller(struct spdk_thread *thread);
struct spdk_poller *spdk_thread_get_next_paused_poller(struct spdk_poller *prev);

struct spdk_io_channel *spdk_thread_get_first_io_channel(struct spdk_thread *thread);
struct spdk_io_channel *spdk_thread_get_next_io_channel(struct spdk_io_channel *prev);

#endif /* SPDK_THREAD_INTERNAL_H_ */
