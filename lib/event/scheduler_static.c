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
#include "spdk/string.h"
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

static const struct spdk_json_object_decoder static_sched_decoders[] = {
	{"mappings", 0, spdk_json_decode_string, true},
};

static int
set_opts_static(const struct spdk_json_val *opts)
{
	char *tok, *mappings = NULL, *copy, *sp = NULL;
	bool valid;

	if (opts != NULL) {
		if (spdk_json_decode_object_relaxed(opts, static_sched_decoders,
						    SPDK_COUNTOF(static_sched_decoders),
						    &mappings)) {
			SPDK_ERRLOG("Decoding scheduler opts JSON failed\n");
			return -EINVAL;
		}
	}

	if (mappings == NULL) {
		return 0;
	}

	copy = strdup(mappings);
	if (copy == NULL) {
		free(mappings);
		return -ENOMEM;
	}

	valid = true;
	tok = strtok_r(mappings, ":", &sp);
	while (tok) {
		struct spdk_lw_thread *lw_thread = NULL;
		struct spdk_thread *thread;
		int thread_id, core;

		thread_id = spdk_strtol(tok, 10);
		if (thread_id > 0) {
			thread = spdk_thread_get_by_id(thread_id);
			if (thread != NULL) {
				lw_thread = spdk_thread_get_ctx(thread);
			}
		}
		if (lw_thread == NULL) {
			SPDK_ERRLOG("invalid thread ID '%s' in mappings '%s'\n", tok, copy);
			valid = false;
			break;
		}

		tok = strtok_r(NULL, ",", &sp);
		core = spdk_strtol(tok, 10);
		if (core < 0 || spdk_reactor_get(core) == NULL) {
			SPDK_ERRLOG("invalid core number '%s' in mappings '%s'\n", tok, copy);
			valid = false;
			break;
		}

		if (!spdk_cpuset_get_cpu(spdk_thread_get_cpumask(thread), core)) {
			SPDK_ERRLOG("core %d not in thread %d cpumask\n", core, thread_id);
			valid = false;
			break;
		}

		tok = strtok_r(NULL, ":", &sp);
	}
	free(mappings);
	if (!valid) {
		free(copy);
		return -EINVAL;
	}

	tok = strtok_r(copy, ":", &sp);
	while (tok) {
		struct spdk_lw_thread *lw_thread = NULL;
		struct spdk_thread *thread;
		int thread_id, core;

		thread_id = spdk_strtol(tok, 10);
		thread = spdk_thread_get_by_id(thread_id);
		lw_thread = spdk_thread_get_ctx(thread);
		tok = strtok_r(NULL, ",", &sp);
		core = spdk_strtol(tok, 10);
		/* initial_lcore saves the static scheduler's lcore mapping.
		 * This is used to restore the previous mapping if we
		 * change to another scheduler and then back. So we can just
		 * change the ->initial_lcore here and kick the scheduler to
		 * put the new mapping into effect.
		 */
		lw_thread->initial_lcore = core;
		tok = strtok_r(NULL, ":", &sp);
	}
	free(copy);

	/* We have updated some core placements, so kick the scheduler to
	 * apply those new placements.
	 */
	spdk_scheduler_set_period(1);
	return 0;
}

static struct spdk_scheduler scheduler = {
	.name = "static",
	.init = init_static,
	.deinit = deinit_static,
	.balance = balance_static,
	.set_opts = set_opts_static,
};
SPDK_SCHEDULER_REGISTER(scheduler);
