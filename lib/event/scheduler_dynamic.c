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
#include "spdk/likely.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/env.h"

#include "spdk_internal/thread.h"
#include "spdk_internal/event.h"

static uint32_t g_next_lcore = SPDK_ENV_LCORE_ID_ANY;
static uint32_t g_main_lcore;

#define SCHEDULER_THREAD_BUSY 100

static uint8_t
_get_thread_load(struct spdk_lw_thread *lw_thread)
{
	uint64_t busy, idle;

	if (lw_thread->last_stats.busy_tsc == 0 && lw_thread->last_stats.idle_tsc == 0) {
		lw_thread->last_stats.busy_tsc = lw_thread->snapshot_stats.busy_tsc;
		lw_thread->last_stats.idle_tsc = lw_thread->snapshot_stats.idle_tsc;
		return SCHEDULER_THREAD_BUSY;
	}

	busy = lw_thread->snapshot_stats.busy_tsc - lw_thread->last_stats.busy_tsc;
	idle = lw_thread->snapshot_stats.idle_tsc - lw_thread->last_stats.idle_tsc;

	lw_thread->last_stats.busy_tsc = lw_thread->snapshot_stats.busy_tsc;
	lw_thread->last_stats.idle_tsc = lw_thread->snapshot_stats.idle_tsc;

	/* return percentage of time thread was busy */
	return busy  * 100 / (busy + idle);
}

static int
init(struct spdk_governor *governor)
{
	g_main_lcore = spdk_env_get_current_core();

	return 0;
}

static void
balance(struct spdk_scheduler_core_info *cores_info, int cores_count,
	struct spdk_governor *governor)
{
	struct spdk_lw_thread *lw_thread;
	struct spdk_thread *thread;
	struct spdk_scheduler_core_info *core;
	struct spdk_cpuset *cpumask;
	uint32_t target_lcore;
	uint32_t i, j, k;

	/* Distribute active threads across all cores except first one
	 * and move idle threads to first core */
	SPDK_ENV_FOREACH_CORE(i) {
		core = &cores_info[i];
		for (j = 0; j < core->threads_count; j++) {
			lw_thread = core->threads[j];
			lw_thread->new_lcore = lw_thread->lcore;
			thread = spdk_thread_get_from_ctx(lw_thread);
			cpumask = spdk_thread_get_cpumask(thread);

			if (_get_thread_load(lw_thread) < 50) {
				/* Continue searching for active threads */
				lw_thread->new_lcore = g_main_lcore;
				continue;
			}

			if (i != g_main_lcore) {
				/* Do not move active thread if it is not on the main core */
				continue;
			}

			/* Find a suitable reactor */
			for (k = 0; k < spdk_env_get_core_count(); k++) {
				if (g_next_lcore == SPDK_ENV_LCORE_ID_ANY) {
					g_next_lcore = spdk_env_get_first_core();
				}

				target_lcore = g_next_lcore;
				g_next_lcore = spdk_env_get_next_core(g_next_lcore);

				if (spdk_cpuset_get_cpu(cpumask, target_lcore)) {
					lw_thread->new_lcore = target_lcore;
					break;
				}
			}
		}
	}
}

static struct spdk_scheduler scheduler_dynamic = {
	.name = "dynamic",
	.init = init,
	.deinit = NULL,
	.balance = balance,
};

SPDK_SCHEDULER_REGISTER(scheduler_dynamic);
