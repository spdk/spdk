/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/likely.h"

#include "spdk_internal/event.h"
#include "spdk/thread.h"

#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/scheduler.h"

static uint32_t g_max_threshold = 99;
static uint32_t g_adjust_threshold = 50;
static uint32_t g_min_threshold = 1;

static int
init(void)
{
	return spdk_governor_set("dpdk_governor");
}

static void
deinit(void)
{
	spdk_governor_set(NULL);
}

static uint32_t
calculate_busy_pct(struct spdk_scheduler_core_info *core)
{
	uint64_t total_tsc;

	total_tsc = core->current_busy_tsc + core->current_idle_tsc;
	if (total_tsc == 0) {
		return 0;
	} else {
		return core->current_busy_tsc * 100 / total_tsc;
	}
}

struct check_sibling_ctx {
	struct spdk_scheduler_core_info *cores;
	uint32_t busy_pct;
};

static void
check_sibling(void *_ctx, uint32_t i)
{
	struct check_sibling_ctx *ctx = _ctx;
	struct spdk_scheduler_core_info *core = &ctx->cores[i];
	uint32_t busy_pct = calculate_busy_pct(core);

	ctx->busy_pct = spdk_max(ctx->busy_pct, busy_pct);
}

static void
balance(struct spdk_scheduler_core_info *cores, uint32_t core_count)
{
	struct spdk_governor *governor;
	struct spdk_scheduler_core_info *core;
	struct spdk_governor_capabilities capabilities;
	uint32_t busy_pct;
	uint32_t i;
	int rc;

	governor = spdk_governor_get();
	assert(governor != NULL);

	/* Gather active/idle statistics */
	SPDK_ENV_FOREACH_CORE(i) {
		struct spdk_cpuset smt_siblings = {};
		core = &cores[i];

		rc = governor->get_core_capabilities(core->lcore, &capabilities);
		if (rc < 0) {
			SPDK_ERRLOG("failed to get capabilities for core: %u\n", core->lcore);
			return;
		}

		busy_pct = calculate_busy_pct(core);
		if (spdk_env_core_get_smt_cpuset(&smt_siblings, i)) {
			struct check_sibling_ctx ctx = {.cores = cores, .busy_pct = busy_pct};

			spdk_cpuset_for_each_cpu(&smt_siblings, check_sibling, &ctx);
			busy_pct = ctx.busy_pct;
		}

		if (busy_pct < g_min_threshold) {
			rc = governor->set_core_freq_min(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("setting to minimal frequency for core %u failed\n", core->lcore);
			}
		} else if (busy_pct < g_adjust_threshold) {
			rc = governor->core_freq_down(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("lowering frequency for core %u failed\n", core->lcore);
			}
		} else if (busy_pct >= g_max_threshold) {
			rc = governor->set_core_freq_max(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("setting to maximal frequency for core %u failed\n", core->lcore);
			}
		} else {
			rc = governor->core_freq_up(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("increasing frequency for core %u failed\n", core->lcore);
			}
		}
	}
}

static struct spdk_scheduler gscheduler = {
	.name = "gscheduler",
	.init = init,
	.deinit = deinit,
	.balance = balance,
};

SPDK_SCHEDULER_REGISTER(gscheduler);
