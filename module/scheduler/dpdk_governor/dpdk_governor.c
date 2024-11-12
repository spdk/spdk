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

#include <rte_common.h>
#include <rte_version.h>
#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
#include <rte_power_cpufreq.h>
#else
#include <rte_power.h>
#endif

static uint32_t
_get_core_avail_freqs(uint32_t lcore_id, uint32_t *freqs, uint32_t num)
{
	uint32_t rc;

	rc = rte_power_freqs(lcore_id, freqs, num);
	if (!rc) {
		SPDK_ERRLOG("Unable to get current core frequency array for core %d\n.", lcore_id);

		return 0;
	}

	return rc;
}

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
_dump_info_json(struct spdk_json_write_ctx *w)
{
	enum power_management_env env;

	env = rte_power_get_env();

	if (env == PM_ENV_ACPI_CPUFREQ) {
		spdk_json_write_named_string(w, "env", "acpi-cpufreq");
	} else if (env == PM_ENV_KVM_VM) {
		spdk_json_write_named_string(w, "env", "kvm");
	} else if (env == PM_ENV_PSTATE_CPUFREQ) {
		spdk_json_write_named_string(w, "env", "intel-pstate");
	} else if (env == PM_ENV_CPPC_CPUFREQ) {
		spdk_json_write_named_string(w, "env", "cppc-cpufreq");
#if RTE_VERSION >= RTE_VERSION_NUM(23, 11, 0, 0)
	} else if (env == PM_ENV_AMD_PSTATE_CPUFREQ) {
		spdk_json_write_named_string(w, "env", "amd-pstate");
#endif
	} else {
		spdk_json_write_named_string(w, "env", "unknown");
		return -EINVAL;
	}

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
	struct spdk_cpuset smt_mask, app_mask;
	uint32_t i, j;
	int rc = 0;

	if (!spdk_env_core_get_smt_cpuset(&smt_mask, UINT32_MAX)) {
		/* We could not get SMT status on this system, don't allow
		 * the governor to load since we cannot guarantee we are running
		 * on a subset of some SMT siblings.
		 */
		SPDK_ERRLOG("Cannot detect SMT status\n");
		return -1;
	}

	/* Verify that if our app mask includes any SMT siblings, that it has
	 * all of those siblings. Otherwise the governor cannot work.
	 */
	spdk_env_get_cpuset(&app_mask);
	spdk_cpuset_and(&app_mask, &smt_mask);
	if (!spdk_cpuset_equal(&app_mask, &smt_mask)) {
		SPDK_ERRLOG("App core mask contains some but not all of a set of SMT siblings\n");
		return -1;
	}

#if RTE_VERSION >= RTE_VERSION_NUM(23, 11, 0, 0)
	for (i = PM_ENV_ACPI_CPUFREQ; i <= PM_ENV_AMD_PSTATE_CPUFREQ; i++) {
#else
	for (i = PM_ENV_ACPI_CPUFREQ; i <= PM_ENV_CPPC_CPUFREQ; i++) {
#endif
		if (rte_power_check_env_supported(i) == 1) {
			rte_power_set_env(i);
			break;
		}
	}

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
	rte_power_unset_env();
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
	rte_power_unset_env();
}

static struct spdk_governor dpdk_governor = {
	.name = "dpdk_governor",
	.get_core_avail_freqs = _get_core_avail_freqs,
	.get_core_curr_freq = _get_core_curr_freq,
	.core_freq_up = _core_freq_up,
	.core_freq_down = _core_freq_down,
	.set_core_freq_max = _set_core_freq_max,
	.set_core_freq_min = _set_core_freq_min,
	.get_core_capabilities = _get_core_capabilities,
	.dump_info_json = _dump_info_json,
	.init = _init,
	.deinit = _deinit,
};

SPDK_GOVERNOR_REGISTER(dpdk_governor);
