/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
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
