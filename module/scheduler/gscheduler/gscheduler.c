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
