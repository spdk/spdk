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
#include "spdk_internal/thread.h"

#include "spdk/log.h"
#include "spdk/env.h"

static int
init(struct spdk_governor *governor)
{
	return _spdk_governor_set("dpdk_governor");
}

static int
deinit(struct spdk_governor *governor)
{
	uint32_t i;
	int rc = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		if (governor->deinit_core) {
			rc = governor->deinit_core(i);
			if (rc != 0) {
				return rc;
			}
		}
	}

	if (governor->deinit) {
		rc = governor->deinit();
	}

	return rc;
}

static void
balance(struct spdk_scheduler_core_info *cores, int core_count, struct spdk_governor *governor)
{
	struct spdk_scheduler_core_info *core;
	struct spdk_lw_thread *thread;
	struct spdk_governor_capabilities capabilities;
	uint32_t i, j;
	int rc;
	bool turbo_available = false;

	/* Gather active/idle statistics */
	SPDK_ENV_FOREACH_CORE(i) {
		core = &cores[i];

		for (j = 0; j < core->threads_count; j++) {
			thread = core->threads[j];

			/* do not change thread lcore */
			thread->new_lcore = thread->lcore;
		}

		rc = governor->get_core_capabilities(core->lcore, &capabilities);
		if (rc < 0) {
			SPDK_ERRLOG("failed to get capabilities for core: %u\n", core->lcore);
			return;
		}

		turbo_available = (capabilities.turbo_available && capabilities.turbo_set) ? true : false;

		if (core->core_busy_tsc < (core->core_idle_tsc / 1000)) {
			rc = governor->set_core_freq_min(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("setting to minimal frequency for core %u failed\n", core->lcore);
			}

			if (turbo_available) {
				rc = governor->disable_core_turbo(core->lcore);
				if (rc < 0) {
					SPDK_ERRLOG("setting to minimal frequency for core %u failed\n", core->lcore);
				}
			}

			SPDK_DEBUGLOG(reactor, "setting to minimal frequency for core: %u\n", core->lcore);
		} else if (core->core_idle_tsc > core->core_busy_tsc) {
			rc = governor->core_freq_down(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("lowering frequency for core %u failed\n", core->lcore);
			}

			if (turbo_available) {
				rc = governor->disable_core_turbo(core->lcore);
				if (rc < 0) {
					SPDK_ERRLOG("disabling turbo for core %u failed\n", core->lcore);
				}
			}

			SPDK_DEBUGLOG(reactor, "lowering frequency for core: %u\n", core->lcore);
		} else if (core->core_idle_tsc < (core->core_busy_tsc / 1000)) {
			rc = governor->set_core_freq_max(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("setting to maximal frequency for core %u failed\n", core->lcore);
			}

			if (turbo_available) {
				rc = governor->enable_core_turbo(core->lcore);
				if (rc < 0) {
					SPDK_ERRLOG("enabling turbo for core %u failed\n", core->lcore);
				}
			}

			SPDK_DEBUGLOG(reactor, "setting to maximum frequency for core: %u\n", core->lcore);
		} else {
			rc = governor->core_freq_up(core->lcore);
			if (rc < 0) {
				SPDK_ERRLOG("increasing frequency for core %u failed\n", core->lcore);
			}

			if (turbo_available) {
				rc = governor->disable_core_turbo(core->lcore);
				if (rc < 0) {
					SPDK_ERRLOG("disabling turbo for core %u failed\n", core->lcore);
				}
			}

			SPDK_DEBUGLOG(reactor, "increasing frequency for core: %u\n", core->lcore);
		}
	}
}

static struct spdk_scheduler gscheduler = {
	.name = "gscheduler",
	.init = init,
	.deinit = deinit,
	.balance = balance,
};

SPDK_SCHEDULER_REGISTER(&gscheduler);
