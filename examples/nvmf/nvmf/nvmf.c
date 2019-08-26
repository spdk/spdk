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

#include "spdk/nvmf.h"
#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#define SPDK_NVMF_MAX_NAMESPACES (1 << 14)
#define ACCEPT_TIMEOUT_US	10000 /* 10ms */
#define DEFAULT_CONN_SCHED CONNECT_SCHED_ROUND_ROBIN

enum spdk_nvmf_connect_sched {
	CONNECT_SCHED_ROUND_ROBIN = 0,
	CONNECT_SCHED_HOST_IP,
	CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP,
};

static const char *g_config_file;
static const char *g_core_mask;
static int g_shm_id;
static int g_dpdk_mem;
static bool g_no_pci;
static struct spdk_conf *g_config = NULL;

struct nvmf_thread {
	struct spdk_thread *thread;

	bool failed;
	bool exit;
	TAILQ_ENTRY(nvmf_thread) link;
};

struct nvmf_tgt_poll_group {
	struct spdk_nvmf_poll_group		*group;
	struct spdk_thread			*thread;
	TAILQ_ENTRY(nvmf_tgt_poll_group)	link;
};

struct nvmf_target {
	struct spdk_nvmf_tgt	*tgt;

	struct {
		int max_subsystems;
		int acceptor_poll_rate;
		int conn_sched;
	} tgt_params;

	TAILQ_HEAD(, nvmf_thread) threads;
	TAILQ_HEAD(, nvmf_tgt_poll_group) poll_groups;
	uint32_t poll_group_counter;
};

struct nvmf_thread *g_master_thread;
struct nvmf_target *g_nvmf_tgt;
bool g_threads_done = false;

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-c config file\n");
	printf("\t[-h show this usage]\n");
	printf("\t[-i shared memory ID (optional)]\n");
	printf("\t[-m core mask for DPDK]\n");
	printf("\t[-s memory size in MB for DPDK (default: 0MB)]\n");
	printf("\t[-u disable PCI access]\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	/* default value */
	g_config_file = NULL;
	g_core_mask = NULL;

	while ((op = getopt(argc, argv, "c:i:m:s:u:h")) != -1) {
		switch (op) {
		case 'c':
			g_config_file = optarg;
			break;
		case 'i':
			g_shm_id = spdk_strtol(optarg, 10);
			if (g_shm_id < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return -EINVAL;
			}
			break;
		case 'm':
			g_core_mask = optarg;
			break;
		case 's':
			g_dpdk_mem = spdk_strtol(optarg, 10);
			if (g_dpdk_mem < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return -EINVAL;
			}
			break;
		case 'u':
			g_no_pci = true;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (g_config_file == NULL) {
		usage(argv[0]);
		return -EINVAL;
	}

	return 0;
}

static int
nvmf_set_config(void)
{
	int rc = 0;
	struct spdk_conf *config;

	/* Parse the configuration file */
	if (!g_config_file || !strlen(g_config_file)) {
		fprintf(stderr, "No configuration file provided\n");
		return -EINVAL;
	}

	config = spdk_conf_allocate();
	if (!config) {
		fprintf(stderr, "Unable to allocate configuration file\n");
		return -ENOMEM;
	}

	rc = spdk_conf_read(config, g_config_file);
	if (rc != 0) {
		fprintf(stderr, "Invalid configuration file format\n");
		spdk_conf_free(config);
		return rc;
	}

	if (spdk_conf_first_section(config) == NULL) {
		fprintf(stderr, "Invalid configuration file format\n");
		spdk_conf_free(config);
		return -EINVAL;
	}
	spdk_conf_set_as_default(config);

	g_config = config;

	return 0;
}

static void
destroy_nvmf_tgt(struct nvmf_target *nvmf_tgt)
{
	struct nvmf_thread *thread, *next;

	if (!nvmf_tgt) {
		return;
	}

	fprintf(stdout, "%s:destroy nvmf_tgt\n", __func__);

	TAILQ_FOREACH_SAFE(thread, &nvmf_tgt->threads, link, next) {
		free(thread);
	}

	free(nvmf_tgt);
}

static struct nvmf_target *
	construct_nvmf_tgt(void)
{
	struct nvmf_target *nvmf_tgt;

	nvmf_tgt = calloc(1, sizeof(*nvmf_tgt));
	if (nvmf_tgt == NULL) {
		fprintf(stderr, "fail to allocate memory\n");
		return NULL;
	}

	nvmf_tgt->tgt_params.acceptor_poll_rate = ACCEPT_TIMEOUT_US;
	nvmf_tgt->tgt_params.conn_sched = DEFAULT_CONN_SCHED;

	TAILQ_INIT(&nvmf_tgt->threads);
	TAILQ_INIT(&nvmf_tgt->poll_groups);

	return nvmf_tgt;
}

static void
nvmf_bdev_init_done(void *cb_arg, int rc)
{
	*(bool *)cb_arg = true;
}

static void
nvmf_bdev_init(void)
{
	bool done = false;

	/* Initialize the copy engine */
	spdk_copy_engine_initialize();

	/* Initialize the bdev layer */
	spdk_bdev_initialize(nvmf_bdev_init_done, &done);

	do {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	} while (!done);
}

static void
nvmf_bdev_fini_done(void *cb_arg)
{
	*(bool *)cb_arg = true;
}

static void
nvmf_copy_fini_start(void *arg)
{
	bool *done = arg;

	spdk_copy_engine_finish(nvmf_bdev_fini_done, done);
}

static void
nvmf_bdev_fini_start(void *arg)
{
	bool *done = arg;

	spdk_bdev_finish(nvmf_copy_fini_start, done);
}

static void
nvmf_bdev_fini(void)
{
	bool done = false;

	/* Finalize the bdev layer */
	nvmf_bdev_fini_start(&done);

	do {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	} while (!done);
}

static int
nvmf_work_fn(void *arg)
{
	int rc;
	uint64_t now;
	struct nvmf_thread *nvmf_thread = arg;
	struct spdk_thread *thread = nvmf_thread->thread;

	spdk_set_thread(thread);

	do {
		now = spdk_get_ticks();
		rc = spdk_thread_poll(thread, 0, now);
		if (rc < 0) {
			fprintf(stderr, "thread poll failed\n");
			spdk_thread_destroy(thread);
			return rc;
		}
	} while (!nvmf_thread->exit);

	/* wait for all the nvmf threads destroy */
	if (nvmf_thread == g_master_thread) {
		while (!g_threads_done) {
			spdk_thread_poll(thread, 0, 0);
		};
	}

	spdk_thread_exit(thread);
	spdk_thread_destroy(thread);

	return 0;
}

static int
nvmf_init_threads(void)
{
	int rc;
	uint32_t i;
	char thread_name[32];
	struct nvmf_thread *nvmf_thread;
	struct spdk_thread *thread;
	struct spdk_cpuset *tmp_cpumask;
	uint32_t master_core = spdk_env_get_current_core();

	spdk_unaffinitize_thread();
	spdk_thread_lib_init(NULL, 0);
	g_threads_done = false;

	tmp_cpumask = spdk_cpuset_alloc();
	if (tmp_cpumask == NULL) {
		fprintf(stderr, "spdk_cpuset_alloc() failed\n");
		return -ENOMEM;
	}

	SPDK_ENV_FOREACH_CORE(i) {
		snprintf(thread_name, sizeof(thread_name), "nvmf_thread_%u", i);

		spdk_cpuset_zero(tmp_cpumask);
		spdk_cpuset_set_cpu(tmp_cpumask, i, true);

		nvmf_thread = calloc(1, sizeof(struct nvmf_thread));
		if (!nvmf_thread) {
			fprintf(stderr, "fail to alloc nvmf thread\n");
			spdk_cpuset_free(tmp_cpumask);
			return -ENOMEM;
		}

		thread = spdk_thread_create(thread_name, tmp_cpumask);
		if (thread == NULL) {
			fprintf(stderr, "fail to create thread\n");
			spdk_cpuset_free(tmp_cpumask);
			return -EINVAL;
		}

		nvmf_thread->exit = false;
		nvmf_thread->thread = thread;
		TAILQ_INSERT_TAIL(&g_nvmf_tgt->threads, nvmf_thread, link);

		if (i == master_core) {
			g_master_thread = nvmf_thread;
			spdk_set_thread(thread);
		} else {
			rc = spdk_env_thread_launch_pinned(i,
							   nvmf_work_fn,
							   nvmf_thread);
			if (rc) {
				fprintf(stderr, "fail to pin thread launch\n");
				spdk_cpuset_free(tmp_cpumask);
				return rc;
			}
		}
	}

	spdk_cpuset_free(tmp_cpumask);

	return 0;
}

static void
nvmf_cleanup_threads_done(void *arg)
{
	g_threads_done = true;
	fprintf(stdout, "threads cleanup done\n");
}

static void
nvmf_cleanup_thread(void *arg)
{
	struct nvmf_thread *nvmf_thread = NULL;
	struct spdk_thread *thread = spdk_get_thread();

	TAILQ_FOREACH(nvmf_thread, &g_nvmf_tgt->threads, link) {
		if (nvmf_thread->thread == thread) {
			break;
		}
	}

	if (nvmf_thread) {
		nvmf_thread->exit = true;
	} else {
		fprintf(stderr, "thread doesn't exist\n");
		assert(false);
	}
}

static void
nvmf_cleanup_threads(void)
{
	spdk_for_each_thread(nvmf_cleanup_thread,
			     &g_threads_done,
			     nvmf_cleanup_threads_done);
}

int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	rc = nvmf_set_config();
	if (rc < 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "nvmf";
	opts.shm_id = g_shm_id;
	if (g_core_mask) {
		opts.core_mask = g_core_mask;
	}
	if (g_dpdk_mem) {
		opts.mem_size = g_dpdk_mem;
	}
	if (g_no_pci) {
		opts.no_pci = g_no_pci;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		spdk_conf_free(g_config);
		return -EINVAL;
	}

	g_nvmf_tgt = construct_nvmf_tgt();
	if (g_nvmf_tgt == NULL) {
		fprintf(stderr, "fail to construct the nvmf_tgt\n");
		spdk_conf_free(g_config);
		return -ENOMEM;
	}

	/* Initialize the threads */
	rc = nvmf_init_threads();
	if (rc != 0) {
		fprintf(stderr, "Failed to create initialization thread\n");
		goto cleanup;
	}

	/* Initialize the bdev layer */
	nvmf_bdev_init();

	nvmf_bdev_fini();
	nvmf_cleanup_threads();
	nvmf_work_fn(g_master_thread);
	spdk_thread_lib_fini();
	spdk_env_thread_wait_all();
cleanup:
	destroy_nvmf_tgt(g_nvmf_tgt);
	spdk_conf_free(g_config);
	return rc;
}
