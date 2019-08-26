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
#include "spdk/rpc.h"
#include "spdk/nvmf.h"

#include "spdk_internal/event.h"

#define SPDK_NVMF_DEFAULT_NAMESPACES	32
#define ACCEPT_TIMEOUT_US		10000 /* 10ms */
#define DEFAULT_CONN_SCHED		"RoundRobin"

enum spdk_nvmf_connect_sched {
	CONNECT_SCHED_ROUND_ROBIN = 0,
	CONNECT_SCHED_HOST_IP,
	CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP,
};

static const char *g_rpc_addr = SPDK_DEFAULT_RPC_ADDR;
static const char *g_conn_sched = DEFAULT_CONN_SCHED;
static int g_max_namespace = SPDK_NVMF_DEFAULT_NAMESPACES;
static int g_acceptor_rate = ACCEPT_TIMEOUT_US;

enum nvmf_target_state {
	NVMF_INIT_SUBSYSTEM = 0,
	NVMF_INIT_TARGET,
	NVMF_INIT_POLL_GROUPS,
	NVMF_FINI_POLL_GROUPS,
	NVMF_FINI_TARGET,
	NVMF_FINI_SUBSYSTEM,
};

struct nvmf_lw_thread {
	TAILQ_ENTRY(nvmf_lw_thread) link;
};

struct nvmf_reactor {
	uint32_t core;
	pthread_mutex_t mutex;

	TAILQ_HEAD(, nvmf_lw_thread)	threads;
	TAILQ_ENTRY(nvmf_reactor)	link;
};

struct nvmf_target_poll_group {
	struct spdk_nvmf_poll_group		*group;
	struct spdk_thread			*thread;
	TAILQ_ENTRY(nvmf_target_poll_group)	link;
};

struct nvmf_target {
	struct spdk_nvmf_tgt	*tgt;

	struct target_opts {
		int max_subsystems;
		int acceptor_poll_rate;
		int conn_sched;
	} tgt_params;

	TAILQ_HEAD(, nvmf_target_poll_group) poll_groups;
};

TAILQ_HEAD(, nvmf_reactor) g_reactors = TAILQ_HEAD_INITIALIZER(g_reactors);

static struct nvmf_reactor *g_master_reactor = NULL;
static struct nvmf_reactor *g_next_reactor = NULL;
static struct spdk_thread *g_init_thread = NULL;
static struct nvmf_target *g_nvmf_tgt = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_reactors_exit = false;
static enum nvmf_target_state g_target_state;

static void nvmf_target_advance_state(void);

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-c ConnectionScheduler(Should be:RoundRobin, Host, Transport) default: RoundRobin]\n");
	printf("\t[-h show this usage]\n");
	printf("\t[-i shared memory ID (optional)]\n");
	printf("\t[-m core mask for DPDK]\n");
	printf("\t[-n max namespaces for target(default: 32)]\n");
	printf("\t[-p acceptor poller rate in us for target(default: 10000us)]\n");
	printf("\t[-r RPC listen address (default /var/tmp/spdk.sock)]\n");
	printf("\t[-s memory size in MB for DPDK (default: 0MB)]\n");
	printf("\t[-u disable PCI access]\n");
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *opts)
{
	int op;
	long int value;

	while ((op = getopt(argc, argv, "c:i:m:n:p:r:s:u:h")) != -1) {
		switch (op) {
		case 'c':
			g_conn_sched = optarg;
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
		case 'n':
			g_max_namespace = spdk_strtol(optarg, 10);
			if (g_max_namespace < 0) {
				fprintf(stderr, "converting a string to integer failed\n");
				return -EINVAL;
			}
			break;
		case 'p':
			g_acceptor_rate = spdk_strtol(optarg, 10);
			if (g_acceptor_rate < 0) {
				fprintf(stderr, "converting a string to integer failed\n");
				return -EINVAL;
			}
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
nvmf_reactor_run(void *arg)
{
	int rc;
	struct nvmf_reactor *nvmf_reactor = arg;
	struct nvmf_lw_thread *lw_thread, *tmp;
	struct spdk_thread *thread;

	/* foreach all the lightweight threads in this nvmf_reactor */
	do {
		pthread_mutex_lock(&nvmf_reactor->mutex);
		TAILQ_FOREACH_SAFE(lw_thread, &nvmf_reactor->threads, link, tmp) {
			thread = spdk_thread_get_from_ctx(lw_thread);

			rc = spdk_thread_poll(thread, 0, 0);
			if (rc < 0) {
				TAILQ_REMOVE(&nvmf_reactor->threads, lw_thread, link);
				spdk_thread_destroy(thread);
			}
		}
		pthread_mutex_unlock(&nvmf_reactor->mutex);
	} while (!g_reactors_exit);

	/* free all the lightweight threads */
	pthread_mutex_lock(&nvmf_reactor->mutex);
	TAILQ_FOREACH_SAFE(lw_thread, &nvmf_reactor->threads, link, tmp) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		TAILQ_REMOVE(&nvmf_reactor->threads, lw_thread, link);
		spdk_set_thread(thread);
		spdk_thread_exit(thread);
		spdk_thread_destroy(thread);
	}
	pthread_mutex_unlock(&nvmf_reactor->mutex);

	return 0;
}

static int
nvmf_schedule_spdk_thread(struct spdk_thread *thread)
{
	struct nvmf_reactor *nvmf_reactor;
	struct nvmf_lw_thread *lw_thread;
	struct spdk_cpuset *cpumask;
	uint32_t i;

	/* Lightweight threads may have a requested cpumask.
	 * This is a request only - the scheduler does not have to honor it.
	 * For this scheduler implementation, each reactor is pinned to
	 * a particular core so honoring the request is reasonably easy.
	 */
	cpumask = spdk_thread_get_cpumask(thread);

	lw_thread = spdk_thread_get_ctx(thread);
	assert(lw_thread != NULL);
	memset(lw_thread, 0, sizeof(*lw_thread));

	/* assign lightweight threads to nvmf reactor(core)
	 * Here we use the mutex.The way the actual SPDK event framework
	 * solves this is by using internal rings for messages between reactors
	 */
	for (i = 0; i < spdk_env_get_core_count(); i++) {
		pthread_mutex_lock(&g_mutex);
		if (g_next_reactor == NULL) {
			g_next_reactor = TAILQ_FIRST(&g_reactors);
		}
		nvmf_reactor = g_next_reactor;
		g_next_reactor = TAILQ_NEXT(g_next_reactor, link);
		pthread_mutex_unlock(&g_mutex);

		/* each spdk_thread has the core affinity */
		if (spdk_cpuset_get_cpu(cpumask, nvmf_reactor->core)) {
			pthread_mutex_lock(&nvmf_reactor->mutex);
			TAILQ_INSERT_TAIL(&nvmf_reactor->threads, lw_thread, link);
			pthread_mutex_unlock(&nvmf_reactor->mutex);
			break;
		}
	}

	if (i == spdk_env_get_core_count()) {
		fprintf(stderr, "failed to schedule spdk thread\n");
		return -1;
	}
	return 0;
}

static int
nvmf_init_threads(void)
{
	int rc;
	uint32_t i;
	char thread_name[32];
	struct nvmf_reactor *nvmf_reactor;
	struct spdk_thread *thread;
	struct spdk_cpuset cpumask;
	uint32_t master_core = spdk_env_get_current_core();

	/* Whenever SPDK creates a new lightweight thread it will call
	 * nvmf_schedule_spdk_thread asking for the application to begin
	 * polling it via spdk_thread_poll(). Each lightweight thread in
	 * SPDK optionally allocates extra memory to be used by the application
	 * framework. The size of the extra memory allocated is the second parameter.
	 */
	spdk_thread_lib_init(nvmf_schedule_spdk_thread, sizeof(struct nvmf_lw_thread));

	/* Spawn one system thread per CPU core. The system thread is called a reactor.
	 * SPDK will spawn lightweight threads that must be mapped to reactors in
	 * nvmf_schedule_spdk_thread. Using a single system thread per CPU core is a
	 * choice unique to this application. SPDK itself does not require this specific
	 * threading model. For example, another viable threading model would be
	 * dynamically scheduling the lightweight threads onto a thread pool using a
	 * work queue.
	 */
	SPDK_ENV_FOREACH_CORE(i) {
		nvmf_reactor = calloc(1, sizeof(struct nvmf_reactor));
		if (!nvmf_reactor) {
			fprintf(stderr, "failed to alloc nvmf reactor\n");
			rc = -ENOMEM;
			goto err_exit;
		}

		nvmf_reactor->core = i;
		pthread_mutex_init(&nvmf_reactor->mutex, NULL);
		TAILQ_INIT(&nvmf_reactor->threads);
		TAILQ_INSERT_TAIL(&g_reactors, nvmf_reactor, link);

		if (i == master_core) {
			g_master_reactor = nvmf_reactor;
			g_next_reactor = g_master_reactor;
		} else {
			rc = spdk_env_thread_launch_pinned(i,
							   nvmf_reactor_run,
							   nvmf_reactor);
			if (rc) {
				fprintf(stderr, "failed to pin reactor launch\n");
				goto err_exit;
			}
		}
	}

	/* Some SPDK libraries assume that there is at least some number of lightweight
	 * threads that exist from the beginning of time. That assumption is currently
	 * being removed from the SPDK libraries, but until that work is completed spawn
	 * one lightweight thread per reactor here.
	 */
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_zero(&cpumask);
		spdk_cpuset_set_cpu(&cpumask, i, true);
		snprintf(thread_name, sizeof(thread_name), "spdk_thread_%u", i);
		thread = spdk_thread_create(thread_name, &cpumask);
		if (!thread) {
			fprintf(stderr, "failed to create spdk thread\n");
			return -1;
		}
	}

	fprintf(stdout, "nvmf threads initlize successfully\n");
	return 0;

err_exit:
	return rc;
}

static void
nvmf_destroy_threads(void)
{
	struct nvmf_reactor *nvmf_reactor, *tmp;

	TAILQ_FOREACH_SAFE(nvmf_reactor, &g_reactors, link, tmp) {
		pthread_mutex_destroy(&nvmf_reactor->mutex);
		free(nvmf_reactor);
	}

	pthread_mutex_destroy(&g_mutex);
	spdk_thread_lib_fini();
	fprintf(stdout, "nvmf threads destroy successfully\n");
}

static void
nvmf_tgt_destroy_done(void *ctx, int status)
{
	struct nvmf_target *nvmf_target = ctx;

	free(nvmf_target);
	fprintf(stdout, "destroyed the nvmf target service\n");

	g_target_state = NVMF_FINI_SUBSYSTEM;
	nvmf_target_advance_state();
}

static void
nvmf_destroy_nvmf_tgt(struct nvmf_target *nvmf_target)
{
	if (!nvmf_target) {
		return;
	}

	if (nvmf_target->tgt) {
		spdk_nvmf_tgt_destroy(nvmf_target->tgt, nvmf_tgt_destroy_done, nvmf_target);
	} else {
		free(nvmf_target);
		g_target_state = NVMF_FINI_SUBSYSTEM;
	}
}

static void
nvmf_tgt_destroy_poll_groups_done(void *ctx)
{
	fprintf(stdout, "destroy targets's poll groups done\n");

	g_target_state = NVMF_FINI_TARGET;
	nvmf_target_advance_state();
}

static void
nvmf_tgt_destroy_poll_group(void *ctx)
{
	struct nvmf_target_poll_group *pg, *tmp;
	struct spdk_thread *thread;

	thread = spdk_get_thread();

	TAILQ_FOREACH_SAFE(pg, &g_nvmf_tgt->poll_groups, link, tmp) {
		if (pg->thread == thread) {
			TAILQ_REMOVE(&g_nvmf_tgt->poll_groups, pg, link);
			spdk_nvmf_poll_group_destroy(pg->group);
			free(pg);
			break;
		}
	}
}

static void
nvmf_tgt_create_poll_groups_done(void *ctx)
{
	fprintf(stdout, "create targets's poll groups done\n");

	g_target_state = NVMF_FINI_POLL_GROUPS;
	nvmf_target_advance_state();
}

static void
nvmf_tgt_create_poll_group(void *ctx)
{
	struct nvmf_target_poll_group *pg;

	pg = calloc(1, sizeof(struct nvmf_target_poll_group));
	if (!pg) {
		fprintf(stderr, "failed to allocate poll group\n");
		return;
	}

	pg->thread = spdk_get_thread();
	pg->group = spdk_nvmf_poll_group_create(g_nvmf_tgt->tgt);
	TAILQ_INSERT_TAIL(&g_nvmf_tgt->poll_groups, pg, link);
}

static int
nvmf_tgt_add_discovery_subsystem(struct nvmf_target *nvmf_tgt)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = spdk_nvmf_subsystem_create(nvmf_tgt->tgt, SPDK_NVMF_DISCOVERY_NQN,
					       SPDK_NVMF_SUBTYPE_DISCOVERY, 0);
	if (subsystem == NULL) {
		fprintf(stderr, "failed to create discovery nvmf library subsystem\n");
		return -EINVAL;
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);
	return 0;
}

static int
nvmf_tgt_init(void)
{
	g_nvmf_tgt = calloc(1, sizeof(struct nvmf_target));
	if (g_nvmf_tgt == NULL) {
		fprintf(stderr, "fail to allocate g_nvmf_tgt\n");
		return -ENOMEM;
	}

	/* set the default value */
	g_nvmf_tgt->tgt_params.max_subsystems = g_max_namespace;
	g_nvmf_tgt->tgt_params.acceptor_poll_rate = g_acceptor_rate;

	if (strcasecmp(g_conn_sched, "Host") == 0) {
		g_nvmf_tgt->tgt_params.conn_sched = CONNECT_SCHED_HOST_IP;
	} else if (strcasecmp(g_conn_sched, "Transport") == 0) {
		g_nvmf_tgt->tgt_params.conn_sched = CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP;
	} else {
		g_nvmf_tgt->tgt_params.conn_sched = CONNECT_SCHED_ROUND_ROBIN;
	}

	TAILQ_INIT(&g_nvmf_tgt->poll_groups);

	return 0;
}

static void
nvmf_create_nvmf_tgt(void)
{
	int rc;
	struct spdk_nvmf_target_opts tgt_opts;

	rc = nvmf_tgt_init();
	if (rc) {
		goto error;
	}

	tgt_opts.max_subsystems = g_nvmf_tgt->tgt_params.max_subsystems;
	snprintf(tgt_opts.name, sizeof(tgt_opts.name), "%s", "nvmf_example");
	g_nvmf_tgt->tgt = spdk_nvmf_tgt_create(&tgt_opts);
	if (g_nvmf_tgt->tgt == NULL) {
		fprintf(stderr, "spdk_nvmf_tgt_create() failed\n");
		goto error;
	}

	/* create and add discovery subsystem */
	rc = nvmf_tgt_add_discovery_subsystem(g_nvmf_tgt);
	if (rc != 0) {
		fprintf(stderr, "spdk_add_nvmf_discovery_subsystem() failed\n");
		goto error;
	}

	fprintf(stdout, "created a nvmf target service\n");
	g_target_state = NVMF_INIT_POLL_GROUPS;
	return;
error:
	g_target_state = NVMF_FINI_TARGET;
}

static void
nvmf_subsystem_fini_done(void *cb_arg)
{
	fprintf(stdout, "bdev subsystem finish successfully\n");
	spdk_rpc_finish();
	g_reactors_exit = true;
}

static void
nvmf_subsystem_init_done(int rc, void *cb_arg)
{
	fprintf(stdout, "bdev subsystem init successfully\n");
	spdk_rpc_initialize(g_rpc_addr);
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	g_target_state = NVMF_INIT_TARGET;
	nvmf_target_advance_state();
}

static void
nvmf_target_advance_state(void)
{
	enum nvmf_target_state prev_state;

	do {
		prev_state = g_target_state;

		switch (g_target_state) {
		case NVMF_INIT_SUBSYSTEM:
			/* initlize the bdev layer */
			spdk_subsystem_init(nvmf_subsystem_init_done, NULL);
			break;
		case NVMF_INIT_TARGET:
			nvmf_create_nvmf_tgt();
			break;
		case NVMF_INIT_POLL_GROUPS:
			/* create poll groups */
			spdk_for_each_thread(nvmf_tgt_create_poll_group,
					     NULL,
					     nvmf_tgt_create_poll_groups_done);
			break;
		case NVMF_FINI_POLL_GROUPS:
			/* destroy poll groups */
			spdk_for_each_thread(nvmf_tgt_destroy_poll_group,
					     NULL,
					     nvmf_tgt_destroy_poll_groups_done);
			break;
		case NVMF_FINI_TARGET:
			nvmf_destroy_nvmf_tgt(g_nvmf_tgt);
			break;
		case NVMF_FINI_SUBSYSTEM:
			spdk_subsystem_fini(nvmf_subsystem_fini_done, NULL);
			break;
		}
	} while (g_target_state != prev_state);
}

static void
nvmf_target_app_start(void *arg)
{
	g_target_state = NVMF_INIT_SUBSYSTEM;
	nvmf_target_advance_state();
}

int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;
	struct nvmf_lw_thread *lw_thread;

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
	assert(rc == 0);

	/* Send a message to the thread assigned to the master reactor
	 * that continues initialization. This is how we bootstrap the
	 * program so that all code from here on is running on an SPDK thread.
	 */
	lw_thread = TAILQ_FIRST(&g_master_reactor->threads);
	g_init_thread = spdk_thread_get_from_ctx(lw_thread);
	assert(g_init_thread != NULL);
	spdk_thread_send_msg(g_init_thread, nvmf_target_app_start, NULL);

	nvmf_reactor_run(g_master_reactor);

	spdk_env_thread_wait_all();
	nvmf_destroy_threads();
	return rc;
}
