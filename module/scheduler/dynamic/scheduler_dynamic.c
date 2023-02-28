/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/env.h"

#include "spdk/thread.h"
#include "spdk_internal/event.h"
#include "spdk/scheduler.h"
#include "spdk_internal/usdt.h"

static uint32_t g_main_lcore;

struct core_stats {
	uint64_t busy;
	uint64_t idle;
	uint32_t thread_count;
};

static struct core_stats *g_cores;

uint8_t g_scheduler_load_limit = 20;
uint8_t g_scheduler_core_limit = 80;
uint8_t g_scheduler_core_busy = 95;

static uint8_t
_busy_pct(uint64_t busy, uint64_t idle)
{
	if ((busy + idle) == 0) {
		return 0;
	}

	return busy * 100 / (busy + idle);
}

static uint8_t
_get_thread_load(struct spdk_scheduler_thread_info *thread_info)
{
	uint64_t busy, idle;

	busy = thread_info->current_stats.busy_tsc;
	idle = thread_info->current_stats.idle_tsc;

	/* return percentage of time thread was busy */
	return _busy_pct(busy, idle);
}

typedef void (*_foreach_fn)(struct spdk_scheduler_thread_info *thread_info);

static void
_foreach_thread(struct spdk_scheduler_core_info *cores_info, _foreach_fn fn)
{
	struct spdk_scheduler_core_info *core;
	uint32_t i, j;

	SPDK_ENV_FOREACH_CORE(i) {
		core = &cores_info[i];
		for (j = 0; j < core->threads_count; j++) {
			fn(&core->thread_infos[j]);
		}
	}
}

static void
_move_thread(struct spdk_scheduler_thread_info *thread_info, uint32_t dst_core)
{
	struct core_stats *dst = &g_cores[dst_core];
	struct core_stats *src = &g_cores[thread_info->lcore];
	uint64_t busy_tsc = thread_info->current_stats.busy_tsc;
	uint8_t busy_pct = _busy_pct(src->busy, src->idle);
	uint64_t tsc;

	SPDK_DTRACE_PROBE2(dynsched_move, thread_info, dst_core);

	if (src == dst) {
		/* Don't modify stats if thread is already on that core. */
		return;
	}

	dst->busy += spdk_min(UINT64_MAX - dst->busy, busy_tsc);
	dst->idle -= spdk_min(dst->idle, busy_tsc);
	dst->thread_count++;

	/* Adjust busy/idle from core as if thread was not present on it.
	 * Core load will reflect the sum of all remaining threads on it. */
	src->busy -= spdk_min(src->busy, busy_tsc);
	src->idle += spdk_min(UINT64_MAX - src->idle, busy_tsc);

	if (busy_pct >= g_scheduler_core_busy &&
	    _busy_pct(src->busy, src->idle) < g_scheduler_core_limit) {
		/* This core was so busy that we cannot assume all of busy_tsc
		 * consumed by the moved thread will now be idle_tsc - it's
		 * very possible the remaining threads will use these cycles
		 * as busy_tsc.
		 *
		 * So make sure we don't drop the updated estimate below
		 * g_scheduler_core_limit, so that other cores can't
		 * move threads to this core during this scheduling
		 * period.
		 */
		tsc = src->busy + src->idle;
		src->busy = tsc * g_scheduler_core_limit / 100;
		src->idle = tsc - src->busy;
	}
	assert(src->thread_count > 0);
	src->thread_count--;

	thread_info->lcore = dst_core;
}

static bool
_is_core_at_limit(uint32_t core_id)
{
	struct core_stats *core = &g_cores[core_id];
	uint64_t busy, idle;

	/* Core with no or single thread cannot be over the limit. */
	if (core->thread_count <= 1) {
		return false;
	}

	busy = core->busy;
	idle = core->idle;

	/* No work was done, exit before possible division by 0. */
	if (busy == 0) {
		return false;
	}

	/* Work done was less than the limit */
	if (_busy_pct(busy, idle) < g_scheduler_core_limit) {
		return false;
	}

	return true;
}

static bool
_can_core_fit_thread(struct spdk_scheduler_thread_info *thread_info, uint32_t dst_core)
{
	struct core_stats *dst = &g_cores[dst_core];
	uint64_t new_busy_tsc, new_idle_tsc;

	/* Thread can always fit on the core it's currently on. */
	if (thread_info->lcore == dst_core) {
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

	/* Core doesn't have enough idle_tsc to take this thread. */
	if (dst->idle < thread_info->current_stats.busy_tsc) {
		return false;
	}

	new_busy_tsc = dst->busy + thread_info->current_stats.busy_tsc;
	new_idle_tsc = dst->idle - thread_info->current_stats.busy_tsc;

	/* Core cannot fit this thread if it would put it over the
	 * g_scheduler_core_limit. */
	return _busy_pct(new_busy_tsc, new_idle_tsc) < g_scheduler_core_limit;
}

static uint32_t
_find_optimal_core(struct spdk_scheduler_thread_info *thread_info)
{
	uint32_t i;
	uint32_t current_lcore = thread_info->lcore;
	uint32_t least_busy_lcore = thread_info->lcore;
	struct spdk_thread *thread;
	struct spdk_cpuset *cpumask;
	bool core_at_limit = _is_core_at_limit(current_lcore);

	thread = spdk_thread_get_by_id(thread_info->thread_id);
	if (thread == NULL) {
		return current_lcore;
	}
	cpumask = spdk_thread_get_cpumask(thread);

	/* Find a core that can fit the thread. */
	SPDK_ENV_FOREACH_CORE(i) {
		/* Ignore cores outside cpumask. */
		if (!spdk_cpuset_get_cpu(cpumask, i)) {
			continue;
		}

		/* Search for least busy core. */
		if (g_cores[i].busy < g_cores[least_busy_lcore].busy) {
			least_busy_lcore = i;
		}

		/* Skip cores that cannot fit the thread and current one. */
		if (!_can_core_fit_thread(thread_info, i) || i == current_lcore) {
			continue;
		}
		if (i == g_main_lcore) {
			/* First consider g_main_lcore, consolidate threads on main lcore if possible. */
			return i;
		} else if (i < current_lcore && current_lcore != g_main_lcore) {
			/* Lower core id was found, move to consolidate threads on lowest core ids. */
			return i;
		} else if (core_at_limit) {
			/* When core is over the limit, any core id is better than current one. */
			return i;
		}
	}

	/* For cores over the limit, place the thread on least busy core
	 * to balance threads. */
	if (core_at_limit) {
		return least_busy_lcore;
	}

	/* If no better core is found, remain on the same one. */
	return current_lcore;
}

static int
init(void)
{
	g_main_lcore = spdk_env_get_current_core();

	if (spdk_governor_set("dpdk_governor") != 0) {
		SPDK_NOTICELOG("Unable to initialize dpdk governor\n");
	}

	g_cores = calloc(spdk_env_get_last_core() + 1, sizeof(struct core_stats));
	if (g_cores == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for dynamic scheduler core stats.\n");
		return -ENOMEM;
	}

	if (spdk_scheduler_get_period() == 0) {
		/* set default scheduling period to one second */
		spdk_scheduler_set_period(SPDK_SEC_TO_USEC);
	}

	return 0;
}

static void
deinit(void)
{
	free(g_cores);
	g_cores = NULL;
	spdk_governor_set(NULL);
}

static void
_balance_idle(struct spdk_scheduler_thread_info *thread_info)
{
	if (_get_thread_load(thread_info) >= g_scheduler_load_limit) {
		return;
	}
	/* This thread is idle, move it to the main core. */
	_move_thread(thread_info, g_main_lcore);
}

static void
_balance_active(struct spdk_scheduler_thread_info *thread_info)
{
	uint32_t target_lcore;

	if (_get_thread_load(thread_info) < g_scheduler_load_limit) {
		return;
	}

	/* This thread is active. */
	target_lcore = _find_optimal_core(thread_info);
	_move_thread(thread_info, target_lcore);
}

static void
balance(struct spdk_scheduler_core_info *cores_info, uint32_t cores_count)
{
	struct spdk_reactor *reactor;
	struct spdk_governor *governor;
	struct spdk_scheduler_core_info *core;
	struct core_stats *main_core;
	uint32_t i;
	int rc;
	bool busy_threads_present = false;

	SPDK_DTRACE_PROBE1(dynsched_balance, cores_count);

	SPDK_ENV_FOREACH_CORE(i) {
		g_cores[i].thread_count = cores_info[i].threads_count;
		g_cores[i].busy = cores_info[i].current_busy_tsc;
		g_cores[i].idle = cores_info[i].current_idle_tsc;
		SPDK_DTRACE_PROBE2(dynsched_core_info, i, &cores_info[i]);
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

	governor = spdk_governor_get();
	if (governor == NULL) {
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

struct json_scheduler_opts {
	uint8_t load_limit;
	uint8_t core_limit;
	uint8_t core_busy;
};

static const struct spdk_json_object_decoder sched_decoders[] = {
	{"load_limit", offsetof(struct json_scheduler_opts, load_limit), spdk_json_decode_uint8, true},
	{"core_limit", offsetof(struct json_scheduler_opts, core_limit), spdk_json_decode_uint8, true},
	{"core_busy", offsetof(struct json_scheduler_opts, core_busy), spdk_json_decode_uint8, true},
};

static int
set_opts(const struct spdk_json_val *opts)
{
	struct json_scheduler_opts scheduler_opts;

	scheduler_opts.load_limit = g_scheduler_load_limit;
	scheduler_opts.core_limit = g_scheduler_core_limit;
	scheduler_opts.core_busy = g_scheduler_core_busy;

	if (opts != NULL) {
		if (spdk_json_decode_object_relaxed(opts, sched_decoders,
						    SPDK_COUNTOF(sched_decoders), &scheduler_opts)) {
			SPDK_ERRLOG("Decoding scheduler opts JSON failed\n");
			return -1;
		}
	}

	SPDK_NOTICELOG("Setting scheduler load limit to %d\n", scheduler_opts.load_limit);
	g_scheduler_load_limit = scheduler_opts.load_limit;
	SPDK_NOTICELOG("Setting scheduler core limit to %d\n", scheduler_opts.core_limit);
	g_scheduler_core_limit = scheduler_opts.core_limit;
	SPDK_NOTICELOG("Setting scheduler core busy to %d\n", scheduler_opts.core_busy);
	g_scheduler_core_busy = scheduler_opts.core_busy;

	return 0;
}

static void
get_opts(struct spdk_json_write_ctx *ctx)
{
	spdk_json_write_named_uint8(ctx, "load_limit", g_scheduler_load_limit);
	spdk_json_write_named_uint8(ctx, "core_limit", g_scheduler_core_limit);
	spdk_json_write_named_uint8(ctx, "core_busy", g_scheduler_core_busy);
}

static struct spdk_scheduler scheduler_dynamic = {
	.name = "dynamic",
	.init = init,
	.deinit = deinit,
	.balance = balance,
	.set_opts = set_opts,
	.get_opts = get_opts,
};

SPDK_SCHEDULER_REGISTER(scheduler_dynamic);
