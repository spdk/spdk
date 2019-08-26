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
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#include "spdk_internal/event.h"

static const char *g_config_file = NULL;
static const char *g_rpc_addr = SPDK_DEFAULT_RPC_ADDR;
static struct spdk_conf *g_config = NULL;

struct nvmf_thread {
	struct spdk_thread *thread;
	bool exit;

	TAILQ_ENTRY(nvmf_thread) link;
};

TAILQ_HEAD(, nvmf_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);

static struct nvmf_thread *g_master_thread = NULL;
static bool g_threads_done = false;

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-c config file(default none)\n");
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

	while ((op = getopt(argc, argv, "c:i:m:r:s:u:h")) != -1) {
		switch (op) {
		case 'c':
			g_config_file = optarg;
			break;
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
nvmf_set_config(void)
{
	int rc = 0;
	struct spdk_conf *config;

	/* Parse the configuration file */
	if (!g_config_file || !strlen(g_config_file)) {
		return 0;
	}

	config = spdk_conf_allocate();
	if (!config) {
		fprintf(stderr, "unable to allocate configuration file\n");
		return -ENOMEM;
	}

	rc = spdk_conf_read(config, g_config_file);
	if (rc != 0) {
		fprintf(stderr, "invalid configuration file format\n");
		spdk_conf_free(config);
		return rc;
	}

	if (spdk_conf_first_section(config) == NULL) {
		fprintf(stderr, "invalid configuration file format\n");
		spdk_conf_free(config);
		return -EINVAL;
	}
	spdk_conf_set_as_default(config);

	g_config = config;

	return 0;
}

static void
nvmf_bdev_init_done(void *cb_arg, int rc)
{
	*(bool *)cb_arg = true;

	fprintf(stdout, "bdev layer init done\n");
}

static void
nvmf_bdev_init(void)
{
	bool done = false;

	/* Initialize the bdev layer */
	spdk_bdev_initialize(nvmf_bdev_init_done, &done);

	while (!done) {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	};
}

static void
nvmf_bdev_fini_done(void *cb_arg)
{
	*(bool *)cb_arg = true;

	fprintf(stderr, "bdev layer finish done\n");
}

static void
nvmf_bdev_fini(void)
{
	bool done = false;

	/* Finalize the bdev layer */
	spdk_bdev_finish(nvmf_bdev_fini_done, &done);

	while (!done) {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	};
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

	/* Wait for all the nvmf threads destroy */
	if (nvmf_thread == g_master_thread) {
		while (!g_threads_done) {
			spdk_thread_poll(thread, 0, 0);
		};
	}

	spdk_thread_exit(thread);
	spdk_thread_destroy(thread);

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
	struct nvmf_thread *nvmf_thread;
	struct spdk_thread *thread = spdk_get_thread();

	TAILQ_FOREACH(nvmf_thread, &g_threads, link) {
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
nvmf_exit_threads(void)
{
	spdk_for_each_thread(nvmf_cleanup_thread,
			     &g_threads_done,
			     nvmf_cleanup_threads_done);

	while (!g_threads_done) {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	};

	spdk_thread_lib_fini();
}

static int
nvmf_init_threads(void)
{
	int rc;
	uint32_t i;
	char thread_name[32];
	struct nvmf_thread *nvmf_thread;
	struct spdk_thread *thread;
	struct spdk_cpuset tmp_cpumask;
	uint32_t master_core = spdk_env_get_current_core();

	spdk_thread_lib_init(NULL, 0);
	g_threads_done = false;

	SPDK_ENV_FOREACH_CORE(i) {
		snprintf(thread_name, sizeof(thread_name), "nvmf_thread_%u", i);

		spdk_cpuset_zero(&tmp_cpumask);
		spdk_cpuset_set_cpu(&tmp_cpumask, i, true);

		nvmf_thread = calloc(1, sizeof(struct nvmf_thread));
		if (!nvmf_thread) {
			fprintf(stderr, "fail to alloc nvmf thread\n");
			rc = -ENOMEM;
			goto err_exit;
		}

		thread = spdk_thread_create(thread_name, &tmp_cpumask);
		if (thread == NULL) {
			fprintf(stderr, "fail to create thread\n");
			free(nvmf_thread);
			rc = -EINVAL;
			goto err_exit;
		}

		nvmf_thread->exit = false;
		nvmf_thread->thread = thread;
		TAILQ_INSERT_TAIL(&g_threads, nvmf_thread, link);

		if (i == master_core) {
			g_master_thread = nvmf_thread;
			spdk_set_thread(thread);
		} else {
			rc = spdk_env_thread_launch_pinned(i,
							   nvmf_work_fn,
							   nvmf_thread);
			if (rc) {
				fprintf(stderr, "fail to pin thread launch\n");
				spdk_thread_destroy(thread);
				goto err_exit;
			}
		}
	}

	fprintf(stderr, "threads init done\n");
	return 0;
err_exit:
	nvmf_exit_threads();
	return rc;
}

static void
nvmf_destroy_threads(void)
{
	struct nvmf_thread *nvmf_thread, *tmp;

	TAILQ_FOREACH_SAFE(nvmf_thread, &g_threads, link, tmp) {
		free(nvmf_thread);
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

	rc = nvmf_set_config();
	if (rc < 0) {
		return rc;
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "unable to initialize SPDK env\n");
		spdk_conf_free(g_config);
		return -EINVAL;
	}

	/* Initialize the threads */
	rc = nvmf_init_threads();
	if (rc != 0) {
		fprintf(stderr, "failed to create initialization thread\n");
		goto cleanup;
	}

	/* Initialize the bdev layer */
	nvmf_bdev_init();

	/* Initialize the RPC service */
	spdk_rpc_initialize(g_rpc_addr);

	spdk_rpc_finish();
	nvmf_bdev_fini();
	nvmf_exit_threads();
cleanup:
	spdk_env_thread_wait_all();
	nvmf_destroy_threads();
	spdk_conf_free(g_config);
	return rc;
}
