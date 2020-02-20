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

#ifndef SPDK_THREAD_INTERNAL_H_
#define SPDK_THREAD_INTERNAL_H_

#include "spdk/stdinc.h"
#include "spdk/thread.h"

#define SPDK_MAX_THREAD_NAME_LEN	256

struct spdk_thread {
	TAILQ_HEAD(, spdk_io_channel)	io_channels;
	TAILQ_ENTRY(spdk_thread)	tailq;
	char				name[SPDK_MAX_THREAD_NAME_LEN + 1];
	uint64_t			id;
	bool				exit;
	struct spdk_cpuset		cpumask;
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
	uint32_t			io_device_delete_count;
	struct spdk_ring		*messages;
	SLIST_HEAD(, spdk_msg)		msg_cache;
	size_t				msg_cache_count;
	spdk_msg_fn			critical_msg;
	/* User context allocated at the end */
	uint8_t				ctx[0];
};

#endif /* SPDK_THREAD_INTERNAL_H_ */
