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

#include "spdk/thread.h"
#include "spdk_internal/event.h"

static uint32_t g_next_lcore = SPDK_ENV_LCORE_ID_ANY;
static uint32_t g_main_lcore;
static bool g_core_mngmnt_available;

struct core_stats {
	uint64_t busy;
	uint64_t idle;
	uint32_t thread_count;
};

static struct core_stats *g_cores;

#define SCHEDULER_THREAD_BUSY 100
#define SCHEDULER_LOAD_LIMIT 50

static uint32_t
_get_next_target_core(void)
{
	uint32_t target_lcore;

	if (g_next_lcore == SPDK_ENV_LCORE_ID_ANY) {
		g_next_lcore = spdk_env_get_first_core();
	}

	target_lcore = g_next_lcore;
	g_next_lcore = spdk_env_get_next_core(g_next_lcore);

	return target_lcore;
}

static uint8_t
_get_thread_load(struct spdk_lw_thread *lw_thread)
{
	uint64_t busy, idle;

	busy = lw_thread->current_stats.busy_tsc;
	idle = lw_thread->current_stats.idle_tsc;

	if (busy == 0) {
		/* No work was done, exit before possible division by 0. */
		return 0;
	}
	/* return percentage of time thread was busy */
	return busy  * 100 / (busy + idle);
}

typedef void (*_foreach_fn)(struct spdk_lw_thread *lw_thread);

static void
_foreach_thread(struct spdk_scheduler_core_info *cores_info, _foreach_fn fn)
{
	struct spdk_scheduler_core_info *core;
	uint32_t i, j;

	SPDK_ENV_FOREACH_CORE(i) {
		core = &cores_info[i];
		for (j = 0; j < core->threads_count; j++) {
			fn(core->threads[j]);
		}
	}
}

static void
_move_thread(struct spdk_lw_thread *lw_thread, uint32_t dst_core)
{
	struct core_stats *dst = &g_cores[dst_core];
	struct core_stats *src = &g_cores[lw_thread->lcore];
	uint64_t busy_tsc = lw_thread->current_stats.busy_tsc;

	if (src == dst) {
		/* Don't modify stats if thread is already on that core. */
		return;
	}

	dst->busy += spdk_min(UINT64_MAX - dst->busy, busy_tsc);
	dst->idle -= spdk_min(dst->idle, busy_tsc);
	dst->thread_count++;

	src->busy -= spdk_min(src->busy, busy_tsc);
	src->idle += spdk_min(UINT64_MAX - src->busy, busy_tsc);
	assert(src->thread_count > 0);
	src->thread_count--;

	lw_thread->lcore = dst_core;
}

static bool
_can_core_fit_thread(struct spdk_lw_thread *lw_thread, uint32_t dst_core)
{
	struct core_stats *dst = &g_cores[dst_core];

	/* Thread can always fit on the core it's currently on. */
	if (lw_thread->lcore == dst_core) {
		return true;
	}

	/* Reactors in interrupt mode do not update stats,
	 * a thread can always fit into reactor in interrupt mode. */
	if (dst->busy + dst->idle == 0) {
		return true;
	}

	/* Core has no threads. */
	if (dst->thread_count == 0) {
		return true;
	}

	if (lw_thread->current_stats.busy_tsc <= dst->idle) {
		return true;
	}
	return false;
}

static uint32_t
_find_optimal_core(struct spdk_lw_thread *lw_thread)
{
	uint32_t i;
	uint32_t target_lcore;
	uint32_t current_lcore = lw_thread->lcore;
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);
	struct spdk_cpuset *cpumask = spdk_thread_get_cpumask(thread);

	/* Find a core that can fit the thread. */
	for (i = 0; i < spdk_env_get_core_count(); i++) {
		target_lcore = _get_next_target_core();

		/* Ignore cores outside cpumask. */
		if (!spdk_cpuset_get_cpu(cpumask, target_lcore)) {
			continue;
		}

		/* Skip cores that cannot fit the thread and current one. */
		if (!_can_core_fit_thread(lw_thread, target_lcore) || target_lcore == current_lcore) {
			continue;
		}

		return target_lcore;
	}

	/* If no better core is found, remain on the same one. */
	return current_lcore;
}

static int
init(struct spdk_governor *governor)
{
	int rc;

	g_main_lcore = spdk_env_get_current_core();

	rc = _spdk_governor_set("dpdk_governor");
	g_core_mngmnt_available = !rc;

	g_cores = calloc(spdk_env_get_last_core() + 1, sizeof(struct core_stats));
	if (g_cores == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for dynamic scheduler core stats.\n");
		return -ENOMEM;
	}

	return 0;
}

static int
deinit(struct spdk_governor *governor)
{
	uint32_t i;
	int rc = 0;

	free(g_cores);
	g_cores = NULL;

	if (!g_core_mngmnt_available) {
		return 0;
	}

	if (governor->deinit_core) {
		SPDK_ENV_FOREACH_CORE(i) {
			rc = governor->deinit_core(i);
			if (rc != 0) {
				SPDK_ERRLOG("Failed to deinitialize governor for core %d\n", i);
			}
		}
	}

	if (governor->deinit) {
		rc = governor->deinit();
	}

	return rc;
}

static void
_balance_idle(struct spdk_lw_thread *lw_thread)
{
	if (_get_thread_load(lw_thread) >= SCHEDULER_LOAD_LIMIT) {
		return;
	}
	/* This thread is idle, move it to the main core. */
	_move_thread(lw_thread, g_main_lcore);
}

static void
_balance_active(struct spdk_lw_thread *lw_thread)
{
	uint32_t target_lcore;

	if (_get_thread_load(lw_thread) < SCHEDULER_LOAD_LIMIT) {
		return;
	}

	/* This thread is active. */
	target_lcore = _find_optimal_core(lw_thread);
	_move_thread(lw_thread, target_lcore);
}

static void
balance(struct spdk_scheduler_core_info *cores_info, int cores_count,
	struct spdk_governor *governor)
{
	struct spdk_reactor *reactor;
	struct spdk_scheduler_core_info *core;
	struct core_stats *main_core;
	uint32_t i;
	int rc;
	bool busy_threads_present = false;

	SPDK_ENV_FOREACH_CORE(i) {
		g_cores[i].thread_count = cores_info[i].threads_count;
		g_cores[i].busy = cores_info[i].current_busy_tsc;
		g_cores[i].idle = cores_info[i].current_idle_tsc;
	}
	main_core = &g_cores[g_main_lcore];

	/* Distribute threads in two passes, to make sure updated core stats are considered on each pass.
	 * 1) Move all idle threads to main core. */
	_foreach_thread(cores_info, _balance_idle);
	/* 2) Distribute active threads across all cores. */
	_foreach_thread(cores_info, _balance_active);

	/* Switch unused cores to interrupt mode and switch cores to polled mode
	 * if they will be used after rebalancing */
	SPDK_ENV_FOREACH_CORE(i) {
		reactor = spdk_reactor_get(i);
		core = &cores_info[i];
		/* We can switch mode only if reactor already does not have any threads */
		if (g_cores[i].thread_count == 0 && TAILQ_EMPTY(&reactor->threads)) {
			core->interrupt_mode = true;
		} else if (g_cores[i].thread_count != 0) {
			core->interrupt_mode = false;
			if (i != g_main_lcore) {
				/* If a thread is present on non g_main_lcore,
				 * it has to be busy. */
				busy_threads_present = true;
			}
		}
	}

	if (!g_core_mngmnt_available) {
		return;
	}

	/* Change main core frequency if needed */
	if (busy_threads_present) {
		rc = governor->set_core_freq_max(g_main_lcore);
		if (rc < 0) {
			SPDK_ERRLOG("setting default frequency for core %u failed\n", g_main_lcore);
		}
	} else if (main_core->busy > main_core->idle) {
		rc = governor->core_freq_up(g_main_lcore);
		if (rc < 0) {
			SPDK_ERRLOG("increasing frequency for core %u failed\n", g_main_lcore);
		}
	} else {
		rc = governor->core_freq_down(g_main_lcore);
		if (rc < 0) {
			SPDK_ERRLOG("lowering frequency for core %u failed\n", g_main_lcore);
		}
	}
}

static struct spdk_scheduler scheduler_dynamic = {
	.name = "dynamic",
	.init = init,
	.deinit = deinit,
	.balance = balance,
};

SPDK_SCHEDULER_REGISTER(scheduler_dynamic);
