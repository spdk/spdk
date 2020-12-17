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

#ifndef SPDK_INTERNAL_THREAD_H_
#define SPDK_INTERNAL_THREAD_H_

#include "spdk/stdinc.h"
#include "spdk/thread.h"

struct spdk_poller;

struct spdk_poller_stats {
	uint64_t	run_count;
	uint64_t	busy_count;
};

struct io_device;
struct spdk_thread;

const char *spdk_poller_get_name(struct spdk_poller *poller);
uint64_t spdk_poller_get_id(struct spdk_poller *poller);
const char *spdk_poller_get_state_str(struct spdk_poller *poller);
uint64_t spdk_poller_get_period_ticks(struct spdk_poller *poller);
void spdk_poller_get_stats(struct spdk_poller *poller, struct spdk_poller_stats *stats);

const char *spdk_io_channel_get_io_device_name(struct spdk_io_channel *ch);
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

#endif /* SPDK_INTERNAL_THREAD_H_ */
