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
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#include "spdk_internal/event.h"

static const char *g_rpc_addr = SPDK_DEFAULT_RPC_ADDR;

struct spdk_lw_thread {
	TAILQ_ENTRY(spdk_lw_thread) link;
};

struct nvmf_thread {
	bool exit;
	uint32_t core;

	TAILQ_HEAD(, spdk_lw_thread)	threads;
	TAILQ_ENTRY(nvmf_thread)	link;
};
TAILQ_HEAD(, nvmf_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);

static struct nvmf_thread *g_master_thread = NULL;
static struct nvmf_thread *g_next_thread = NULL;
static struct spdk_thread *init_thread = NULL;
static bool g_threads_exit = false;

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-h show this usage]\n");
	printf("\t[-i shared memory ID (optional)]\n");
	printf("\t[-m core mask for DPDK]\n");
	printf("\t[-r RPC listen address (default /var/tmp/spdk.sock)]\n");
	printf("\t[-s memory size in MB for DPDK (default: 0MB)]\n");
	printf("\t[-u disable PCI access]\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *opts)
{
	int op;
	long int value;

	while ((op = getopt(argc, argv, "i:m:r:s:u:h")) != -1) {
		switch (op) {
		case 'i':
			value = spdk_strtol(optarg, 10);
			if (value < 0) {
				fprintf(stderr, "converting a string to integer failed\n");
				return -EINVAL;
			}
			opts->shm_id = value;
			break;
		case 'm':
			opts->core_mask = optarg;
			break;
		case 'r':
			g_rpc_addr = optarg;
			break;
		case 's':
			value = spdk_strtol(optarg, 10);
			if (value < 0) {
				fprintf(stderr, "converting a string to integer failed\n");
				return -EINVAL;
			}
			opts->mem_size = value;
			break;
		case 'u':
			opts->no_pci = true;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

static int
nvmf_work_fn(void *arg)
{
	int rc;
	struct nvmf_thread *nvmf_thread = arg;
	struct spdk_lw_thread *lw_thread, *tmp;
	struct spdk_thread *thread;

	/* we foreach all the lightweight threads in this nvmf_thread */
	do {
		TAILQ_FOREACH_SAFE(lw_thread, &nvmf_thread->threads, link, tmp) {
			thread = spdk_thread_get_from_ctx(lw_thread);

			spdk_set_thread(thread);
			rc = spdk_thread_poll(thread, 0, 0);
			if (rc < 0) {
				TAILQ_REMOVE(&nvmf_thread->threads, lw_thread, link);
				spdk_thread_destroy(thread);
			}
		}
	} while (!g_threads_exit);

	/* free all the lightweight threads */
	TAILQ_FOREACH_SAFE(lw_thread, &nvmf_thread->threads, link, tmp) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		TAILQ_REMOVE(&nvmf_thread->threads, lw_thread, link);
		spdk_set_thread(thread);
		spdk_thread_exit(thread);
		spdk_thread_destroy(thread);
	}

	return 0;
}

static int
nvmf_schedule_spdk_thread(struct spdk_thread *thread)
{
	struct nvmf_thread *nvmf_thread;
	struct spdk_lw_thread *lw_thread;
	struct spdk_cpuset *cpumask;
	uint32_t i;

	cpumask = spdk_thread_get_cpumask(thread);

	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);
	memset(lw_thread, 0, sizeof(*lw_thread));

	/* assign lightweight threads to nvmf thread(core) */
	for (i = 0; i < spdk_env_get_core_count(); i++) {
		if (g_next_thread == NULL) {
			g_next_thread = TAILQ_FIRST(&g_threads);
		}
		nvmf_thread = g_next_thread;
		g_next_thread = TAILQ_NEXT(g_next_thread, link);

		/* each spdk_thread has the core affinity */
		if (spdk_cpuset_get_cpu(cpumask, nvmf_thread->core)) {
			TAILQ_INSERT_TAIL(&nvmf_thread->threads, lw_thread, link);
			break;
		}
	}

	return 0;
}

static int
nvmf_init_threads(void)
{
	int rc;
	uint32_t i;
	char thread_name[32];
	struct nvmf_thread *nvmf_thread;
	uint32_t master_core = spdk_env_get_current_core();

	/* scheduler function determine lightweight thread belong to which nvmf_thread
	 * when we call the spdk_thread_create
	 */
	spdk_thread_lib_init(nvmf_schedule_spdk_thread, sizeof(struct spdk_lw_thread));

	/* nvmf_thread equals to core
	 * nvmf_thread is a set of lightweight thread
	 * default we spawn a lightweight thread per core
	 * Also we set the init_thread in the master core
	 */
	SPDK_ENV_FOREACH_CORE(i) {
		nvmf_thread = calloc(1, sizeof(struct nvmf_thread));
		if (!nvmf_thread) {
			fprintf(stderr, "failed to alloc nvmf thread\n");
			rc = -ENOMEM;
			goto err_exit;
		}

		nvmf_thread->exit = false;
		nvmf_thread->core = i;
		TAILQ_INIT(&nvmf_thread->threads);
		TAILQ_INSERT_TAIL(&g_threads, nvmf_thread, link);
		g_next_thread = nvmf_thread;

		if (i == master_core) {
			g_master_thread = nvmf_thread;
			snprintf(thread_name, sizeof(thread_name), "init_thread");
			/* we can also use cpuset to set the core affinity */
			init_thread = spdk_thread_create(thread_name, NULL);
			if (!init_thread) {
				fprintf(stderr, "failed to create init thread\n");
				goto err_exit;
			}
			spdk_set_thread(init_thread);
		} else {
			rc = spdk_env_thread_launch_pinned(i,
							   nvmf_work_fn,
							   nvmf_thread);
			if (rc) {
				fprintf(stderr, "fail to pin thread launch\n");
				goto err_exit;
			}

			snprintf(thread_name, sizeof(thread_name), "spdk_thread_%u", i);
			spdk_thread_create(thread_name, NULL);
		}
	}

	fprintf(stdout, "nvmf threads init done\n");
	return 0;
err_exit:
	return rc;
}

static void
nvmf_destroy_threads(void)
{
	struct nvmf_thread *nvmf_thread, *tmp;

	TAILQ_FOREACH_SAFE(nvmf_thread, &g_threads, link, tmp) {
		free(nvmf_thread);
	}

	spdk_thread_lib_fini();
	fprintf(stdout, "nvmf threads destroy done\n");
}

static void
nvmf_bdev_init_done(int rc, void *cb_arg)
{
	*(bool *)cb_arg = true;

	fprintf(stdout, "bdev layer init done\n");
}

static void
nvmf_bdev_init_start(void)
{
	bool done = false;

	spdk_subsystem_init(nvmf_bdev_init_done, &done);

	while (!done) {
		spdk_thread_poll(init_thread, 0, 0);
	}
}

static void
nvmf_bdev_fini_done(void *cb_arg)
{
	*(bool *)cb_arg = true;

	fprintf(stdout, "bdev layer finish done\n");
}

static void
nvmf_bdev_fini_start(void)
{
	bool done = false;

	spdk_subsystem_fini(nvmf_bdev_fini_done, &done);

	while (!done) {
		spdk_thread_poll(init_thread, 0, 0);
	}
}

int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;

	spdk_env_opts_init(&opts);
	opts.name = "nvmf-example";

	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "unable to initialize SPDK env\n");
		return -EINVAL;
	}

	/* Initialize the threads */
	rc = nvmf_init_threads();
	if (rc != 0) {
		fprintf(stderr, "failed to create initialization thread\n");
		g_threads_exit = true;
	}

	/* Initialize the bdev layer */
	nvmf_bdev_init_start();

	nvmf_work_fn(g_master_thread);
	nvmf_bdev_fini_start();

	spdk_env_thread_wait_all();
	nvmf_destroy_threads();
	return rc;
}
