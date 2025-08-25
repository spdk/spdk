/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "env_internal.h"

#include <rte_config.h>
#include <rte_lcore.h>

#include "spdk/cpuset.h"
#include "spdk/log.h"
#include "spdk/string.h"

#define THREAD_SIBLINGS_FILE \
	"/sys/devices/system/cpu/cpu%d/topology/thread_siblings"

uint32_t
spdk_env_get_core_count(void)
{
	return rte_lcore_count();
}

uint32_t
spdk_env_get_current_core(void)
{
	return rte_lcore_id();
}

uint32_t
spdk_env_get_main_core(void)
{
	return rte_get_main_lcore();
}

uint32_t
spdk_env_get_first_core(void)
{
	return rte_get_next_lcore(-1, 0, 0);
}

uint32_t
spdk_env_get_last_core(void)
{
	uint32_t i;
	uint32_t last_core = UINT32_MAX;

	SPDK_ENV_FOREACH_CORE(i) {
		last_core = i;
	}

	assert(last_core != UINT32_MAX);

	return last_core;
}

uint32_t
spdk_env_get_next_core(uint32_t prev_core)
{
	unsigned lcore;

	lcore = rte_get_next_lcore(prev_core, 0, 0);
	if (lcore == RTE_MAX_LCORE) {
		return UINT32_MAX;
	}
	return lcore;
}

int32_t
spdk_env_get_numa_id(uint32_t core)
{
	if (core >= RTE_MAX_LCORE) {
		return SPDK_ENV_NUMA_ID_ANY;
	}

	return rte_lcore_to_socket_id(core);
}

int32_t
spdk_env_get_first_numa_id(void)
{
	assert(rte_socket_count() > 0);

	return rte_socket_id_by_idx(0);
}

int32_t
spdk_env_get_last_numa_id(void)
{
	assert(rte_socket_count() > 0);

	return rte_socket_id_by_idx(rte_socket_count() - 1);
}

int32_t
spdk_env_get_next_numa_id(int32_t prev_numa_id)
{
	uint32_t i;

	for (i = 0; i < rte_socket_count(); i++) {
		if (rte_socket_id_by_idx(i) == prev_numa_id) {
			break;
		}
	}

	if ((i + 1) < rte_socket_count()) {
		return rte_socket_id_by_idx(i + 1);
	} else {
		return INT32_MAX;
	}
}

void
spdk_env_get_cpuset(struct spdk_cpuset *cpuset)
{
	uint32_t i;

	spdk_cpuset_zero(cpuset);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(cpuset, i, true);
	}
}

static bool
env_core_get_smt_cpuset(struct spdk_cpuset *cpuset, uint32_t core)
{
#ifdef __linux__
	struct spdk_cpuset smt_siblings;
	char path[PATH_MAX];
	FILE *f;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	bool valid = false;

	snprintf(path, sizeof(path), THREAD_SIBLINGS_FILE, core);
	f = fopen(path, "r");
	if (f == NULL) {
		SPDK_ERRLOG("Could not fopen('%s'): %s\n", path, spdk_strerror(errno));
		return false;
	}
	read = getline(&line, &len, f);
	if (read == -1) {
		SPDK_ERRLOG("Could not getline() for '%s': %s\n", path, spdk_strerror(errno));
		goto ret;
	}

	/* Remove trailing newline */
	line[strlen(line) - 1] = 0;
	if (spdk_cpuset_parse(&smt_siblings, line)) {
		SPDK_ERRLOG("Could not parse '%s' from '%s'\n", line, path);
		goto ret;
	}

	valid = true;
	spdk_cpuset_or(cpuset, &smt_siblings);
ret:
	free(line);
	fclose(f);
	return valid;
#else
	return false;
#endif
}

bool
spdk_env_core_get_smt_cpuset(struct spdk_cpuset *cpuset, uint32_t core)
{
	uint32_t i;

	spdk_cpuset_zero(cpuset);

	if (core != UINT32_MAX) {
		return env_core_get_smt_cpuset(cpuset, core);
	}

	SPDK_ENV_FOREACH_CORE(i) {
		if (!env_core_get_smt_cpuset(cpuset, i)) {
			return false;
		}
	}

	return true;
}

int
spdk_env_thread_launch_pinned(uint32_t core, thread_start_fn fn, void *arg)
{
	int rc;

	rc = rte_eal_remote_launch(fn, arg, core);

	return rc;
}

void
spdk_env_thread_wait_all(void)
{
	rte_eal_mp_wait_lcore();
}
