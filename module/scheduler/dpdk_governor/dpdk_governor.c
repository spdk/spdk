/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/scheduler.h"

#include "spdk_internal/event.h"

#include <rte_power.h>

static uint32_t
_get_core_curr_freq(uint32_t lcore_id)
{
	uint32_t freqs[SPDK_MAX_LCORE_FREQS];
	uint32_t freq_index;
	int rc;

	rc = rte_power_freqs(lcore_id, freqs, SPDK_MAX_LCORE_FREQS);
	if (!rc) {
		SPDK_ERRLOG("Unable to get current core frequency array for core %d\n.", lcore_id);

		return 0;
	}
	freq_index = rte_power_get_freq(lcore_id);
	if (freq_index >= SPDK_MAX_LCORE_FREQS) {
		SPDK_ERRLOG("Unable to get current core frequency for core %d\n.", lcore_id);

		return 0;
	}

	return freqs[freq_index];
}

static int
_core_freq_up(uint32_t lcore_id)
{
	return rte_power_freq_up(lcore_id);
}

static int
_core_freq_down(uint32_t lcore_id)
{
	return rte_power_freq_down(lcore_id);
}

static int
_set_core_freq_max(uint32_t lcore_id)
{
	return rte_power_freq_max(lcore_id);
}

static int
_set_core_freq_min(uint32_t lcore_id)
{
	return rte_power_freq_min(lcore_id);
}

static int
_get_core_capabilities(uint32_t lcore_id, struct spdk_governor_capabilities *capabilities)
{
	struct rte_power_core_capabilities caps;
	int rc;

	rc = rte_power_get_capabilities(lcore_id, &caps);
	if (rc != 0) {
		return rc;
	}

	capabilities->priority = caps.priority == 0 ? false : true;

	return 0;
}

static int
_init_core(uint32_t lcore_id)
{
	struct rte_power_core_capabilities caps;
	int rc;

	rc = rte_power_init(lcore_id);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize on core%d\n", lcore_id);
		return rc;
	}

	rc = rte_power_get_capabilities(lcore_id, &caps);
	if (rc != 0) {
		SPDK_ERRLOG("Failed retrieve capabilities of core%d\n", lcore_id);
		return rc;
	}

	if (caps.turbo) {
		rc = rte_power_freq_enable_turbo(lcore_id);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to set turbo on core%d\n", lcore_id);
			return rc;
		}
	}

	return rc;
}

static int
_init(void)
{
	uint32_t i, j;
	int rc = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		rc = _init_core(i);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to initialize on core%d\n", i);
			break;
		}
	}

	if (rc == 0) {
		return rc;
	}

	/* When initialization of a core failed, deinitialize prior cores. */
	SPDK_ENV_FOREACH_CORE(j) {
		if (j >= i) {
			break;
		}
		if (rte_power_exit(j) != 0) {
			SPDK_ERRLOG("Failed to deinitialize on core%d\n", j);
		}
	}
	return rc;
}

static void
_deinit(void)
{
	uint32_t i;

	SPDK_ENV_FOREACH_CORE(i) {
		if (rte_power_exit(i) != 0) {
			SPDK_ERRLOG("Failed to deinitialize on core%d\n", i);
		}
	}
}

static struct spdk_governor dpdk_governor = {
	.name = "dpdk_governor",
	.get_core_curr_freq = _get_core_curr_freq,
	.core_freq_up = _core_freq_up,
	.core_freq_down = _core_freq_down,
	.set_core_freq_max = _set_core_freq_max,
	.set_core_freq_min = _set_core_freq_min,
	.get_core_capabilities = _get_core_capabilities,
	.init = _init,
	.deinit = _deinit,
};

SPDK_GOVERNOR_REGISTER(dpdk_governor);
