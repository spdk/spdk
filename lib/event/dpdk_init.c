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

#include "spdk/event.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_per_lcore.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_common.h>

#include "spdk/string.h"

enum dpdk_eal_args {
	EAL_PROGNAME_ARG = 0,
	EAL_COREMASK_ARG,
	EAL_MEMCHAN_ARG,
	EAL_MEMSIZE_ARG,
	EAL_MASTER_CORE_ARG,
	EAL_FILE_PREFIX_ARG,
	EAL_PROC_TYPE_ARG,
	EAL_ARG_COUNT
};

/* g_arg_strings contains the original pointers allocated via
 * spdk_sprintf_alloc().  These pointers are copied to g_ealargs
 * for passing to DPDK rte_eal_init().  Since DPDK may modify the
 * pointer values, we use g_arg_strings() to free the strings after
 * rte_eal_init() completes.
 */
static char *g_arg_strings[EAL_ARG_COUNT];
static char *g_ealargs[EAL_ARG_COUNT];

static void
spdk_free_ealargs(void)
{
	int i;

	for (i = 0; i < EAL_ARG_COUNT; i++)
		free(g_arg_strings[i]);
}

static unsigned long long
spdk_get_eal_coremask(const char *coremask)
{
	unsigned long long core_mask, max_coremask = 0;
	int num_cores_online;

	num_cores_online = sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cores_online > 0) {
		if (num_cores_online > RTE_MAX_LCORE) {
			num_cores_online = RTE_MAX_LCORE;
		}
		if (num_cores_online >= 64) {
			max_coremask = ~0ULL;
		} else {
			max_coremask = (1ULL << num_cores_online) - 1;
		}
	}

	core_mask = strtoull(coremask, NULL, 16);
	core_mask &= max_coremask;

	return core_mask;
}

static void
spdk_build_eal_cmdline(struct spdk_app_opts *opts)
{
	unsigned long long core_mask;

	/* set the program name */
	g_arg_strings[EAL_PROGNAME_ARG] = spdk_sprintf_alloc("%s", opts->name);
	if (g_arg_strings[EAL_PROGNAME_ARG] == NULL) {
		rte_exit(EXIT_FAILURE, "g_arg_strings spdk_sprintf_alloc");
	}

	/*set the coremask */
	core_mask = spdk_get_eal_coremask(opts->reactor_mask);
	g_arg_strings[EAL_COREMASK_ARG] = spdk_sprintf_alloc("-c %llx", core_mask);
	if (g_arg_strings[EAL_COREMASK_ARG] == NULL) {
		spdk_free_ealargs();
		rte_exit(EXIT_FAILURE, "g_arg_strings spdk_sprintf_alloc");
	}

	/* set the memory channel number */
	g_arg_strings[EAL_MEMCHAN_ARG] = spdk_sprintf_alloc("-n %d", opts->dpdk_mem_channel);
	if (g_arg_strings[EAL_MEMCHAN_ARG] == NULL) {
		spdk_free_ealargs();
		rte_exit(EXIT_FAILURE, "g_arg_strings spdk_sprintf_alloc");
	}

	/* set the memory size */
	if (opts->dpdk_mem_size == -1)
		opts->dpdk_mem_size = SPDK_APP_DPDK_DEFAULT_MEM_SIZE;
	g_arg_strings[EAL_MEMSIZE_ARG] = spdk_sprintf_alloc("-m %d", opts->dpdk_mem_size);
	if (g_arg_strings[EAL_MEMSIZE_ARG] == NULL) {
		spdk_free_ealargs();
		rte_exit(EXIT_FAILURE, "g_arg_strings spdk_sprintf_alloc");
	}

	/* set the master core */
	g_arg_strings[EAL_MASTER_CORE_ARG] = spdk_sprintf_alloc("--master-lcore=%d",
					     opts->dpdk_master_core);
	if (g_arg_strings[EAL_MASTER_CORE_ARG] == NULL) {
		spdk_free_ealargs();
		rte_exit(EXIT_FAILURE, "g_arg_strings spdk_sprintf_alloc");
	}

#ifdef __linux__
	/* set the hugepage file prefix */
	g_arg_strings[EAL_FILE_PREFIX_ARG] = spdk_sprintf_alloc("--file-prefix=rte%d",
					     opts->instance_id);
#else
	/* --file-prefix is not required on FreeBSD */
	g_arg_strings[EAL_FILE_PREFIX_ARG] = strdup("");
#endif
	if (g_arg_strings[EAL_FILE_PREFIX_ARG] == NULL) {
		spdk_free_ealargs();
		rte_exit(EXIT_FAILURE, "ealargs spdk_sprintf_alloc");
	}

#ifdef __linux__
	/* set the process type */
	g_arg_strings[EAL_PROC_TYPE_ARG] = spdk_sprintf_alloc("--proc-type=auto");
#else
	/* --proc-type is not required on FreeBSD */
	/* TODO: to enable the support on FreeBSD once it supports process shared mutex */
	g_arg_strings[EAL_PROC_TYPE_ARG] = strdup("");
#endif
	if (g_arg_strings[EAL_PROC_TYPE_ARG] == NULL) {
		spdk_free_ealargs();
		rte_exit(EXIT_FAILURE, "ealargs spdk_sprintf_alloc");
	}

	memcpy(g_ealargs, g_arg_strings, sizeof(g_arg_strings));
}

static void
spdk_init_dpdk(struct spdk_app_opts *opts)
{
	int i, rc;
	static bool g_dpdk_initialized = false;

	/* to make sure DPDK is only initialized once */
	if (g_dpdk_initialized)
		rte_exit(EXIT_FAILURE, "DPDK is already initialized\n");

	spdk_build_eal_cmdline(opts);

	printf("Starting Intel(R) DPDK initialization ... \n");
	printf("[ DPDK EAL parameters: ");
	for (i = 0; i < EAL_ARG_COUNT; i++) {
		printf("%s ", g_ealargs[i]);
	}
	printf("]\n");

	fflush(stdout);
	rc = rte_eal_init(EAL_ARG_COUNT, g_ealargs);
	spdk_free_ealargs();

	if (rc < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments for DPDK\n");

	g_dpdk_initialized = true;

	printf("done.\n");
}

__attribute__((weak))
void spdk_dpdk_framework_init(struct spdk_app_opts *opts)
{
	spdk_init_dpdk(opts);
}
