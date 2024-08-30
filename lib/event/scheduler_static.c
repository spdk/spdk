/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/scheduler.h"

#include "spdk_internal/event.h"
#include "event_internal.h"

static bool g_first_load = true;

static int
init_static(void)
{
	if (g_first_load) {
		/* There is no scheduling performed by static scheduler,
		 * do not set the scheduling period. */
		spdk_scheduler_set_period(0);
	} else {
		/* Schedule a balance to happen immediately, so that
		 * we can reset each thread's lcore back to its original
		 * state.
		 */
		spdk_scheduler_set_period(1);
	}

	return 0;
}

static void
deinit_static(void)
{
	g_first_load = false;
}

static void
balance_static(struct spdk_scheduler_core_info *cores, uint32_t core_count)
{
	struct spdk_scheduler_core_info *core_info;
	struct spdk_scheduler_thread_info *thread_info;
	struct spdk_lw_thread *lw_thread;
	struct spdk_thread *thread;
	uint32_t i, j;

	for (i = 0; i < core_count; i++) {
		core_info = &cores[i];
		core_info->interrupt_mode = false;
		for (j = 0; j < core_info->threads_count; j++) {
			thread_info = &core_info->thread_infos[j];
			thread = spdk_thread_get_by_id(thread_info->thread_id);
			lw_thread = spdk_thread_get_ctx(thread);
			thread_info->lcore = lw_thread->initial_lcore;
		}
	}

	/* We've restored the original state now, so we don't need to
	 * balance() anymore.
	 */
	spdk_scheduler_set_period(0);
}

static struct spdk_scheduler scheduler = {
	.name = "static",
	.init = init_static,
	.deinit = deinit_static,
	.balance = balance_static,
};
SPDK_SCHEDULER_REGISTER(scheduler);
