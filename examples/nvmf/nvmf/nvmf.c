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
#include "spdk/likely.h"

#include "spdk_internal/event.h"

#define NVMF_DEFAULT_SUBSYSTEMS		32
#define ACCEPT_TIMEOUT_US		10000 /* 10ms */

static const char *g_rpc_addr = SPDK_DEFAULT_RPC_ADDR;
static uint32_t g_acceptor_poll_rate = ACCEPT_TIMEOUT_US;

enum nvmf_target_state {
	NVMF_INIT_SUBSYSTEM = 0,
	NVMF_INIT_TARGET,
	NVMF_INIT_POLL_GROUPS,
	NVMF_INIT_START_SUBSYSTEMS,
	NVMF_INIT_START_ACCEPTOR,
	NVMF_RUNNING,
	NVMF_FINI_STOP_SUBSYSTEMS,
	NVMF_FINI_POLL_GROUPS,
	NVMF_FINI_STOP_ACCEPTOR,
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

	int max_subsystems;
};

TAILQ_HEAD(, nvmf_reactor) g_reactors = TAILQ_HEAD_INITIALIZER(g_reactors);
TAILQ_HEAD(, nvmf_target_poll_group) g_poll_groups = TAILQ_HEAD_INITIALIZER(g_poll_groups);

static struct nvmf_reactor *g_master_reactor = NULL;
static struct nvmf_reactor *g_next_reactor = NULL;
static struct spdk_thread *g_init_thread = NULL;
static struct nvmf_target g_nvmf_tgt = {
	.max_subsystems = NVMF_DEFAULT_SUBSYSTEMS,
};
static struct spdk_poller *g_acceptor_poller = NULL;
static struct nvmf_target_poll_group *g_next_pg = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_reactors_exit = false;
static enum nvmf_target_state g_target_state;
static bool g_intr_received = false;

static void nvmf_target_advance_state(void);

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-h show this usage]\n");
	printf("\t[-i shared memory ID (optional)]\n");
	printf("\t[-m core mask for DPDK]\n");
	printf("\t[-n max subsystems for target(default: 32)]\n");
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

	while ((op = getopt(argc, argv, "i:m:n:p:r:s:u:h")) != -1) {
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
		case 'n':
			g_nvmf_tgt.max_subsystems = spdk_strtol(optarg, 10);
			if (g_nvmf_tgt.max_subsystems < 0) {
				fprintf(stderr, "converting a string to integer failed\n");
				return -EINVAL;
			}
			break;
		case 'p':
			value = spdk_strtol(optarg, 10);
			if (value < 0) {
				fprintf(stderr, "converting a string to integer failed\n");
				return -EINVAL;
			}
			g_acceptor_poll_rate = value;
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
	struct nvmf_reactor *nvmf_reactor = arg;
	struct nvmf_lw_thread *lw_thread, *tmp;
	struct spdk_thread *thread;

	/* foreach all the lightweight threads in this nvmf_reactor */
	do {
		pthread_mutex_lock(&nvmf_reactor->mutex);
		TAILQ_FOREACH_SAFE(lw_thread, &nvmf_reactor->threads, link, tmp) {
			thread = spdk_thread_get_from_ctx(lw_thread);

			spdk_thread_poll(thread, 0, 0);
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
		while (!spdk_thread_is_exited(thread)) {
			spdk_thread_poll(thread, 0, 0);
		}
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
	fprintf(stdout, "destroyed the nvmf target service\n");

	g_target_state = NVMF_FINI_SUBSYSTEM;
	nvmf_target_advance_state();
}

static void
nvmf_destroy_nvmf_tgt(void)
{
	if (g_nvmf_tgt.tgt) {
		spdk_nvmf_tgt_destroy(g_nvmf_tgt.tgt, nvmf_tgt_destroy_done, NULL);
	} else {
		g_target_state = NVMF_FINI_SUBSYSTEM;
	}
}

static void
nvmf_create_nvmf_tgt(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_target_opts tgt_opts;

	tgt_opts.max_subsystems = g_nvmf_tgt.max_subsystems;
	snprintf(tgt_opts.name, sizeof(tgt_opts.name), "%s", "nvmf_example");
	/* Construct the default NVMe-oF target
	 * An NVMe-oF target is a collection of subsystems, namespace, and poll
	 * groups, and defines the scope of the NVMe-oF discovery service.
	 */
	g_nvmf_tgt.tgt = spdk_nvmf_tgt_create(&tgt_opts);
	if (g_nvmf_tgt.tgt == NULL) {
		fprintf(stderr, "spdk_nvmf_tgt_create() failed\n");
		goto error;
	}

	/* Create and add discovery subsystem to the NVMe-oF target.
	 * NVMe-oF defines a discovery mechanism that a host uses to determine
	 * the NVM subsystems that expose namespaces that the host may access.
	 * It provides a host with following capabilities:
	 *	1,The ability to discover a list of NVM subsystems with namespaces
	 *	  that are accessible to the host.
	 *	2,The ability to discover multiple paths to an NVM subsystem.
	 *	3,The ability to discover controllers that are statically configured.
	 */
	subsystem = spdk_nvmf_subsystem_create(g_nvmf_tgt.tgt, SPDK_NVMF_DISCOVERY_NQN,
					       SPDK_NVMF_SUBTYPE_DISCOVERY, 0);
	if (subsystem == NULL) {
		fprintf(stderr, "failed to create discovery nvmf library subsystem\n");
		goto error;
	}

	/* Allow any host to access the discovery subsystem */
	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);

	fprintf(stdout, "created a nvmf target service\n");

	g_target_state = NVMF_INIT_POLL_GROUPS;
	return;

error:
	g_target_state = NVMF_FINI_TARGET;
}

static void
nvmf_tgt_subsystem_stop_next(struct spdk_nvmf_subsystem *subsystem,
			     void *cb_arg, int status)
{
	subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	if (subsystem) {
		spdk_nvmf_subsystem_stop(subsystem,
					 nvmf_tgt_subsystem_stop_next,
					 cb_arg);
		return;
	}

	fprintf(stdout, "all subsystems of target stopped\n");

	g_target_state = NVMF_FINI_POLL_GROUPS;
	nvmf_target_advance_state();
}

static void
nvmf_tgt_stop_subsystems(struct nvmf_target *nvmf_tgt)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = spdk_nvmf_subsystem_get_first(nvmf_tgt->tgt);
	if (spdk_likely(subsystem)) {
		spdk_nvmf_subsystem_stop(subsystem,
					 nvmf_tgt_subsystem_stop_next,
					 NULL);
	} else {
		g_target_state = NVMF_FINI_POLL_GROUPS;
	}
}

struct nvmf_target_pg_ctx {
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_target_poll_group *pg;
};

static void
nvmf_tgt_pg_add_qpair(void *_ctx)
{
	struct nvmf_target_pg_ctx *ctx = _ctx;
	struct spdk_nvmf_qpair *qpair = ctx->qpair;
	struct nvmf_target_poll_group *pg = ctx->pg;

	free(_ctx);

	if (spdk_nvmf_poll_group_add(pg->group, qpair) != 0) {
		fprintf(stderr, "unable to add the qpair to a poll group.\n");
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
	}
}

static struct nvmf_target_poll_group *
nvmf_tgt_get_next_pg(struct nvmf_target *nvmf_tgt)
{
	struct nvmf_target_poll_group *pg;

	pg = g_next_pg;
	g_next_pg = TAILQ_NEXT(pg, link);
	if (g_next_pg == NULL) {
		g_next_pg = TAILQ_FIRST(&g_poll_groups);
	}

	return pg;
}

static struct nvmf_target_poll_group *
nvmf_get_optimal_pg(struct nvmf_target *nvmf_tgt, struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_target_poll_group *pg, *_pg = NULL;
	struct spdk_nvmf_poll_group *group = spdk_nvmf_get_optimal_poll_group(qpair);

	if (group == NULL) {
		_pg = nvmf_tgt_get_next_pg(nvmf_tgt);
		goto end;
	}

	TAILQ_FOREACH(pg, &g_poll_groups, link) {
		if (pg->group == group) {
			_pg = pg;
			break;
		}
	}

end:
	assert(_pg != NULL);
	return _pg;
}

static void
new_qpair(struct spdk_nvmf_qpair *qpair, void *cb_arg)
{
	struct nvmf_target_poll_group *pg;
	struct nvmf_target_pg_ctx *ctx;
	struct nvmf_target *nvmf_tgt = &g_nvmf_tgt;

	/* In SPDK we support three methods to get poll group: RoundRobin, Host and Transport.
	 * In this example we only support the "Transport" which gets the optimal poll group.
	 */
	pg = nvmf_get_optimal_pg(nvmf_tgt, qpair);
	if (!pg) {
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		fprintf(stderr, "failed to allocate poll group context.\n");
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		return;
	}

	ctx->qpair = qpair;
	ctx->pg = pg;

	spdk_thread_send_msg(pg->thread, nvmf_tgt_pg_add_qpair, ctx);
}

static int
nvmf_tgt_acceptor_poll(void *arg)
{
	struct nvmf_target *nvmf_tgt = arg;

	spdk_nvmf_tgt_accept(nvmf_tgt->tgt, new_qpair, NULL);

	return -1;
}

static void
nvmf_tgt_subsystem_start_next(struct spdk_nvmf_subsystem *subsystem,
			      void *cb_arg, int status)
{
	subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	if (subsystem) {
		spdk_nvmf_subsystem_start(subsystem, nvmf_tgt_subsystem_start_next,
					  cb_arg);
		return;
	}

	fprintf(stdout, "all subsystems of target started\n");

	g_target_state = NVMF_INIT_START_ACCEPTOR;
	nvmf_target_advance_state();
}

static void
nvmf_tgt_start_subsystems(struct nvmf_target *nvmf_tgt)
{
	struct spdk_nvmf_subsystem *subsystem;

	/* Subsystem is the NVM subsystem which is a combine of namespaces
	 * except the discovery subsystem which is used for discovery service.
	 * It also controls the hosts that means the subsystem determines whether
	 * the host can access this subsystem.
	 */
	subsystem = spdk_nvmf_subsystem_get_first(nvmf_tgt->tgt);
	if (spdk_likely(subsystem)) {
		/* In SPDK there are three states in subsystem: Inactive, Active, Paused.
		 * Start subsystem means make it from inactive to active that means
		 * subsystem start to work or it can be accessed.
		 */
		spdk_nvmf_subsystem_start(subsystem,
					  nvmf_tgt_subsystem_start_next,
					  NULL);
	} else {
		g_target_state = NVMF_INIT_START_ACCEPTOR;
	}
}

static void
nvmf_tgt_create_poll_groups_done(void *ctx)
{
	fprintf(stdout, "create targets's poll groups done\n");

	g_target_state = NVMF_INIT_START_SUBSYSTEMS;
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
	pg->group = spdk_nvmf_poll_group_create(g_nvmf_tgt.tgt);
	if (!pg->group) {
		fprintf(stderr, "failed to create poll group of the target\n");
		free(pg);
		return;
	}

	if (!g_next_pg) {
		g_next_pg = pg;
	}

	/* spdk_for_each_channel is asynchronous, but runs on each thread in serial.
	 * Since this is the only operation occurring on the g_poll_groups list,
	 * we don't need to take a lock.
	 */
	TAILQ_INSERT_TAIL(&g_poll_groups, pg, link);
}

static void
nvmf_poll_groups_create(void)
{
	/* Send a message to each thread and create a poll group.
	 * Pgs are used to handle all the connections from the host so we
	 * would like to create one pg in each core. We use the spdk_for_each
	 * _thread because we have allocated one lightweight thread per core in
	 * thread layer. You can also do this by traversing reactors
	 * or SPDK_ENV_FOREACH_CORE().
	 */
	spdk_for_each_thread(nvmf_tgt_create_poll_group,
			     NULL,
			     nvmf_tgt_create_poll_groups_done);
}

static void
nvmf_tgt_destroy_poll_groups_done(struct spdk_io_channel_iter *i, int status)
{
	fprintf(stdout, "destroy targets's poll groups done\n");

	g_target_state = NVMF_FINI_STOP_ACCEPTOR;
	nvmf_target_advance_state();
}

static void
nvmf_tgt_destroy_poll_group(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *io_ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(io_ch);
	struct nvmf_target_poll_group *pg, *tmp;

	/* Spdk_for_each_channel is asynchronous but executes serially.
	 * That means only a single thread is executing this callback at a time,
	 * so we can safely touch the g_poll_groups list without a lock.
	 */
	TAILQ_FOREACH_SAFE(pg, &g_poll_groups, link, tmp) {
		if (pg->group == group) {
			TAILQ_REMOVE(&g_poll_groups, pg, link);
			spdk_nvmf_poll_group_destroy(group, NULL, NULL);
			free(pg);
			break;
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
nvmf_poll_groups_destroy(void)
{
	/* Send a message to each channel and destroy the poll group.
	 * Poll groups are I/O channels associated with the spdk_nvmf_tgt object.
	 * To iterate all poll groups, we can use spdk_for_each_channel.
	 */
	spdk_for_each_channel(g_nvmf_tgt.tgt,
			      nvmf_tgt_destroy_poll_group,
			      NULL,
			      nvmf_tgt_destroy_poll_groups_done);
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
			return;
		case NVMF_INIT_TARGET:
			nvmf_create_nvmf_tgt();
			break;
		case NVMF_INIT_POLL_GROUPS:
			nvmf_poll_groups_create();
			break;
		case NVMF_INIT_START_SUBSYSTEMS:
			nvmf_tgt_start_subsystems(&g_nvmf_tgt);
			break;
		case NVMF_INIT_START_ACCEPTOR:
			g_acceptor_poller = SPDK_POLLER_REGISTER(nvmf_tgt_acceptor_poll, &g_nvmf_tgt,
					    g_acceptor_poll_rate);
			fprintf(stdout, "Acceptor running\n");
			g_target_state = NVMF_RUNNING;
			break;
		case NVMF_RUNNING:
			fprintf(stdout, "nvmf target is running\n");
			break;
		case NVMF_FINI_STOP_SUBSYSTEMS:
			nvmf_tgt_stop_subsystems(&g_nvmf_tgt);
			break;
		case NVMF_FINI_POLL_GROUPS:
			nvmf_poll_groups_destroy();
			break;
		case NVMF_FINI_STOP_ACCEPTOR:
			spdk_poller_unregister(&g_acceptor_poller);
			g_target_state = NVMF_FINI_TARGET;
			break;
		case NVMF_FINI_TARGET:
			nvmf_destroy_nvmf_tgt();
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

static void
_nvmf_shutdown_cb(void *ctx)
{
	/* Still in initialization state, defer shutdown operation */
	if (g_target_state < NVMF_RUNNING) {
		spdk_thread_send_msg(spdk_get_thread(), _nvmf_shutdown_cb, NULL);
		return;
	} else if (g_target_state > NVMF_RUNNING) {
		/* Already in Shutdown status, ignore the signal */
		return;
	}

	g_target_state = NVMF_FINI_STOP_SUBSYSTEMS;
	nvmf_target_advance_state();
}

static void
nvmf_shutdown_cb(int signo)
{
	if (!g_intr_received) {
		g_intr_received = true;
		spdk_thread_send_msg(g_init_thread, _nvmf_shutdown_cb, NULL);
	}
}

static int
nvmf_setup_signal_handlers(void)
{
	struct sigaction	sigact;
	sigset_t		sigmask;
	int			signals[] = {SIGINT, SIGTERM};
	int			num_signals = sizeof(signals) / sizeof(int);
	int			rc, i;

	rc = sigemptyset(&sigmask);
	if (rc) {
		fprintf(stderr, "errno:%d--failed to empty signal set\n", errno);
		return rc;
	}
	memset(&sigact, 0, sizeof(sigact));
	rc = sigemptyset(&sigact.sa_mask);
	if (rc) {
		fprintf(stderr, "errno:%d--failed to empty signal set\n", errno);
		return rc;
	}

	/* Install the same handler for SIGINT and SIGTERM */
	sigact.sa_handler = nvmf_shutdown_cb;

	for (i = 0; i < num_signals; i++) {
		rc = sigaction(signals[i], &sigact, NULL);
		if (rc < 0) {
			fprintf(stderr, "errno:%d--sigaction() failed\n", errno);
			return rc;
		}
		rc = sigaddset(&sigmask, signals[i]);
		if (rc) {
			fprintf(stderr, "errno:%d--failed to add set\n", errno);
			return rc;
		}
	}

	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);

	return 0;
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

	rc = nvmf_setup_signal_handlers();
	assert(rc == 0);

	spdk_thread_send_msg(g_init_thread, nvmf_target_app_start, NULL);

	nvmf_reactor_run(g_master_reactor);

	spdk_env_thread_wait_all();
	nvmf_destroy_threads();
	return rc;
}
