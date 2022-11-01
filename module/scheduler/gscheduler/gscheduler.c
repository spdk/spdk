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

static void
balance(struct spdk_scheduler_core_info *cores, uint32_t core_count)
{
	struct spdk_governor *governor;
	struct spdk_scheduler_core_info *core;
	struct spdk_governor_capabilities capabilities;
	uint32_t i;
	int rc;

	governor = spdk_governor_get();
	assert(governor != NULL);

	/* Gather active/idle statistics */
	SPDK_ENV_FOREACH_CORE(i) {
		core = &cores[i];

		rc = governor->get_core_capabilities(core->lcore, &capabilities);
		if (rc < 0) {
			SPDK_ERRLOG("failed to get capabilities for core: %u\n", core->lcore);
			return;
		}

		if (core->current_busy_tsc < (core->current_idle_tsc / 1000)) {
			rc = governor->set_core_freq_min(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("setting to minimal frequency for core %u failed\n", core->lcore);
			}
		} else if (core->current_idle_tsc > core->current_busy_tsc) {
			rc = governor->core_freq_down(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("lowering frequency for core %u failed\n", core->lcore);
			}
		} else if (core->current_idle_tsc < (core->current_busy_tsc / 1000)) {
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
