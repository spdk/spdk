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
#include "spdk/nvmf.h"
#include "spdk/rpc.h"

#include "spdk_internal/event.h"

#define SPDK_NVMF_DEFAULT_NAMESPACES	32
#define ACCEPT_TIMEOUT_US		10000 /* 10ms */
#define DEFAULT_CONN_SCHED		CONNECT_SCHED_ROUND_ROBIN

enum spdk_nvmf_connect_sched {
	CONNECT_SCHED_ROUND_ROBIN = 0,
	CONNECT_SCHED_HOST_IP,
	CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP,
};

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

struct nvmf_target_poll_group {
	struct spdk_nvmf_poll_group		*group;
	struct spdk_thread			*thread;

	TAILQ_ENTRY(nvmf_target_poll_group)	link;
};

struct nvmf_target_host_trid {
	struct spdk_nvme_transport_id		host_trid;
	struct nvmf_target_poll_group		*pg;
	uint32_t				ref;

	TAILQ_ENTRY(nvmf_target_host_trid)	link;
};

struct nvmf_target {
	struct spdk_nvmf_tgt		*tgt;
	struct nvmf_target_poll_group	*next_pg;

	struct target_opts {
		int max_subsystems;
		int acceptor_poll_rate;
		int conn_sched;
	} tgt_params;

	TAILQ_HEAD(, nvmf_target_poll_group) poll_groups;
	TAILQ_HEAD(, nvmf_target_host_trid) host_trids;
};

TAILQ_HEAD(, nvmf_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);

static struct nvmf_target *g_nvmf_tgt = NULL;
static struct nvmf_thread *g_master_thread = NULL;
static struct nvmf_thread *g_next_thread = NULL;
static struct spdk_thread *init_thread = NULL;
static struct spdk_poller *acceptor_poller = NULL;
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
	struct spdk_cpuset cpumask;
	uint32_t master_core = spdk_env_get_current_core();

	/* scheduler function determine lightweight thread belong to which nvmf_thread
	 * when we call the spdk_thread_create
	 */
	spdk_thread_lib_init(nvmf_schedule_spdk_thread, sizeof(struct spdk_lw_thread));

	/* nvmf_thread equals to core
	 * nvmf_thread is a set of lightweight thread
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

		if (i == master_core) {
			g_master_thread = nvmf_thread;
			g_next_thread = g_master_thread;
		} else {
			rc = spdk_env_thread_launch_pinned(i,
							   nvmf_work_fn,
							   nvmf_thread);
			if (rc) {
				fprintf(stderr, "fail to pin thread launch\n");
				goto err_exit;
			}
		}
	}

	/* default we spawn a lightweight thread per core
	 * Also we set the init_thread in the master core
	 */
	SPDK_ENV_FOREACH_CORE(i) {
		snprintf(thread_name, sizeof(thread_name), "spdk_thread_%u", i);
		if (i == master_core) {
			spdk_cpuset_zero(&cpumask);
			spdk_cpuset_set_cpu(&cpumask, i, true);
			init_thread = spdk_thread_create(thread_name, &cpumask);
			spdk_set_thread(init_thread);
		} else {
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

static void
nvmf_tgt_destroy_done(void *ctx, int status)
{
	*(bool *)ctx = true;
}

static void
nvmf_destroy_nvmf_tgt(struct nvmf_target *nvmf_target)
{
	bool done = false;

	if (!nvmf_target) {
		return;
	}

	if (nvmf_target->tgt) {
		spdk_nvmf_tgt_destroy(nvmf_target->tgt, nvmf_tgt_destroy_done, &done);

		do {
			spdk_thread_poll(init_thread, 0, 0);
		} while (!done);
	}

	free(nvmf_target);

	fprintf(stdout, "destroyed the nvmf target service\n");
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
	g_nvmf_tgt->tgt_params.max_subsystems = SPDK_NVMF_DEFAULT_NAMESPACES;
	g_nvmf_tgt->tgt_params.acceptor_poll_rate = ACCEPT_TIMEOUT_US;
	g_nvmf_tgt->tgt_params.conn_sched = DEFAULT_CONN_SCHED;

	TAILQ_INIT(&g_nvmf_tgt->poll_groups);
	TAILQ_INIT(&g_nvmf_tgt->host_trids);

	return 0;
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
nvmf_create_nvmf_tgt(void)
{
	int rc;
	struct spdk_nvmf_target_opts tgt_opts;

	rc = nvmf_tgt_init();
	if (rc) {
		return rc;
	}

	tgt_opts.max_subsystems = g_nvmf_tgt->tgt_params.max_subsystems;
	snprintf(tgt_opts.name, sizeof(tgt_opts.name), "%s", "nvmf_example");
	g_nvmf_tgt->tgt = spdk_nvmf_tgt_create(&tgt_opts);
	if (g_nvmf_tgt->tgt == NULL) {
		fprintf(stderr, "spdk_nvmf_tgt_create() failed\n");
		free(g_nvmf_tgt);
		return -EINVAL;
	}

	/* create and add discovery subsystem */
	rc = nvmf_tgt_add_discovery_subsystem(g_nvmf_tgt);
	if (rc != 0) {
		fprintf(stderr, "spdk_add_nvmf_discovery_subsystem() failed\n");
		nvmf_destroy_nvmf_tgt(g_nvmf_tgt);
		return rc;
	}

	fprintf(stdout, "created a nvmf target service\n");

	return 0;
}

static void
nvmf_thread_poll(struct nvmf_thread *nvmf_thread)
{
	struct spdk_lw_thread *lw_thread;
	struct spdk_thread *thread;

	TAILQ_FOREACH(lw_thread, &nvmf_thread->threads, link) {
		thread = spdk_thread_get_from_ctx(lw_thread);
		spdk_set_thread(thread);
		spdk_thread_poll(thread, 0, 0);
	}
}

static void
nvmf_tgt_create_poll_groups_done(void *ctx)
{
	bool *done = ctx;

	*done = true;
	fprintf(stdout, "create targets's poll groups done\n");
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

	if (!g_nvmf_tgt->next_pg) {
		g_nvmf_tgt->next_pg = pg;
	}

	pg->thread = spdk_get_thread();
	pg->group = spdk_nvmf_poll_group_create(g_nvmf_tgt->tgt);
	TAILQ_INSERT_TAIL(&g_nvmf_tgt->poll_groups, pg, link);
}

static void
nvmf_tgt_create_poll_groups(void)
{
	bool done = false;

	spdk_for_each_thread(nvmf_tgt_create_poll_group,
			     &done,
			     nvmf_tgt_create_poll_groups_done);

	do {
		nvmf_thread_poll(g_master_thread);
	} while (!done);
}

static void
nvmf_tgt_destroy_poll_groups_done(void *ctx)
{
	bool *done = ctx;

	*done = true;
	fprintf(stdout, "destroy targets's poll groups done\n");
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
nvmf_tgt_destroy_poll_groups(void)
{
	bool done = false;

	/* Send a message to each thread and destroy the poll group */
	spdk_for_each_thread(nvmf_tgt_destroy_poll_group,
			     &done,
			     nvmf_tgt_destroy_poll_groups_done);

	do {
		nvmf_thread_poll(g_master_thread);
	} while (!done);
}

static void
nvmf_tgt_subsystem_start_next(struct spdk_nvmf_subsystem *subsystem,
			      void *cb_arg, int status)
{
	bool *done = cb_arg;

	subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	if (subsystem) {
		spdk_nvmf_subsystem_start(subsystem, nvmf_tgt_subsystem_start_next,
					  cb_arg);
		return;
	}

	fprintf(stdout, "all the subsystems of target started\n");
	*done = true;
}

static void
nvmf_tgt_start_subsystems(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	bool done = false;

	subsystem = spdk_nvmf_subsystem_get_first(g_nvmf_tgt->tgt);
	if (subsystem) {
		spdk_nvmf_subsystem_start(subsystem,
					  nvmf_tgt_subsystem_start_next,
					  &done);

		do {
			nvmf_thread_poll(g_master_thread);
		} while (!done);
	}
}

static void
nvmf_tgt_subsystem_stop_next(struct spdk_nvmf_subsystem *subsystem,
			     void *cb_arg, int status)
{
	bool *done = cb_arg;

	subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	if (subsystem) {
		spdk_nvmf_subsystem_stop(subsystem,
					 nvmf_tgt_subsystem_stop_next,
					 cb_arg);
		return;
	}

	fprintf(stdout, "all subsystems of target stoped\n");
	*done = true;
}

static void
nvmf_tgt_stop_subsystems(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	bool done = false;

	subsystem = spdk_nvmf_subsystem_get_first(g_nvmf_tgt->tgt);
	if (subsystem) {
		spdk_nvmf_subsystem_stop(subsystem,
					 nvmf_tgt_subsystem_stop_next,
					 &done);

		do {
			nvmf_thread_poll(g_master_thread);
		} while (!done);
	}
}

struct nvmf_target_pg_ctx {
	struct nvmf_target *nvmf_tgt;
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_target_poll_group *pg;
};

static void
nvmf_tgt_remove_host_trid(struct nvmf_target *nvmf_tgt, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid_to_remove;
	struct nvmf_target_host_trid *trid, *tmp_trid;

	if (nvmf_tgt->tgt_params.conn_sched != CONNECT_SCHED_HOST_IP) {
		return;
	}

	if (spdk_nvmf_qpair_get_peer_trid(qpair, &trid_to_remove) != 0) {
		return;
	}

	TAILQ_FOREACH_SAFE(trid, &nvmf_tgt->host_trids, link, tmp_trid) {
		if (!strncmp(trid->host_trid.traddr,
			     trid_to_remove.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1)) {
			trid->ref--;
			if (trid->ref == 0) {
				TAILQ_REMOVE(&nvmf_tgt->host_trids, trid, link);
				free(trid);
			}

			break;
		}
	}

	return;
}

static void
nvmf_tgt_pg_add_qpair(void *_ctx)
{
	struct nvmf_target_pg_ctx *ctx = _ctx;
	struct spdk_nvmf_qpair *qpair = ctx->qpair;
	struct nvmf_target_poll_group *pg = ctx->pg;
	struct nvmf_target *nvmf_tgt = ctx->nvmf_tgt;

	free(_ctx);

	if (spdk_nvmf_poll_group_add(pg->group, qpair) != 0) {
		fprintf(stderr, "unable to add the qpair to a poll group.\n");
		nvmf_tgt_remove_host_trid(nvmf_tgt, qpair);
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
	}
}

/* Round robin selection of poll groups */
static struct nvmf_target_poll_group *
nvmf_tgt_get_next_pg(struct nvmf_target *nvmf_tgt)
{
	struct nvmf_target_poll_group *pg;

	pg = nvmf_tgt->next_pg;
	nvmf_tgt->next_pg = TAILQ_NEXT(pg, link);
	if (nvmf_tgt->next_pg == NULL) {
		nvmf_tgt->next_pg = TAILQ_FIRST(&nvmf_tgt->poll_groups);
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

	TAILQ_FOREACH(pg, &nvmf_tgt->poll_groups, link) {
		if (pg->group == group) {
			_pg = pg;
			break;
		}
	}

end:
	assert(_pg != NULL);
	return _pg;
}

static struct nvmf_target_poll_group *
nvmf_qpair_get_pg(struct nvmf_target *nvmf_tgt, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid;
	struct nvmf_target_host_trid *tmp_trid, *new_trid;
	struct nvmf_target_poll_group *pg;
	int ret;

	switch (nvmf_tgt->tgt_params.conn_sched) {
	case CONNECT_SCHED_HOST_IP:
		ret = spdk_nvmf_qpair_get_peer_trid(qpair, &trid);
		if (ret) {
			pg = nvmf_tgt->next_pg;
			fprintf(stderr, "invalid host transport Id. Assigning to poll group %p\n", pg);
			break;
		}

		TAILQ_FOREACH(tmp_trid, &nvmf_tgt->host_trids, link) {
			if (!strncmp(tmp_trid->host_trid.traddr,
				     trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1)) {
				tmp_trid->ref++;
				pg = tmp_trid->pg;
				break;
			}
		}

		if (!tmp_trid) {
			new_trid = calloc(1, sizeof(*new_trid));
			if (!new_trid) {
				pg = nvmf_tgt->next_pg;
				fprintf(stderr, "insufficient memory. Assigning to poll group %p\n", pg);
				break;
			}
			/* Get the next available poll group for the new host */
			pg = nvmf_tgt_get_next_pg(nvmf_tgt);
			new_trid->pg = pg;
			memcpy(new_trid->host_trid.traddr, trid.traddr,
			       SPDK_NVMF_TRADDR_MAX_LEN + 1);
			TAILQ_INSERT_TAIL(&nvmf_tgt->host_trids, new_trid, link);
		}
		break;
	case CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP:
		pg = nvmf_get_optimal_pg(nvmf_tgt, qpair);
		break;
	case CONNECT_SCHED_ROUND_ROBIN:
	default:
		pg = nvmf_tgt_get_next_pg(nvmf_tgt);
		break;
	}

	return pg;
}

static void
new_qpair(struct spdk_nvmf_qpair *qpair, void *cb_arg)
{
	struct nvmf_target_poll_group *pg;
	struct nvmf_target_pg_ctx *ctx;
	struct nvmf_target *nvmf_tgt = g_nvmf_tgt;

	pg = nvmf_qpair_get_pg(nvmf_tgt, qpair);
	if (!pg) {
		nvmf_tgt_remove_host_trid(nvmf_tgt, qpair);
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		fprintf(stderr, "failed to allocate poll group context.\n");
		nvmf_tgt_remove_host_trid(nvmf_tgt, qpair);
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		return;
	}

	ctx->qpair = qpair;
	ctx->pg = pg;
	ctx->nvmf_tgt = nvmf_tgt;

	spdk_thread_send_msg(pg->thread, nvmf_tgt_pg_add_qpair, ctx);
}

static int
acceptor_poll(void *arg)
{
	struct nvmf_target *nvmf_tgt = arg;

	spdk_nvmf_tgt_accept(nvmf_tgt->tgt, new_qpair, NULL);

	return -1;
}

static void
nvmf_tgts_run(void)
{
	spdk_set_thread(init_thread);
	acceptor_poller = spdk_poller_register(acceptor_poll, g_nvmf_tgt,
					       g_nvmf_tgt->tgt_params.acceptor_poll_rate);

	fprintf(stdout, "start master work function\n");
	spdk_rpc_initialize(g_rpc_addr);
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	nvmf_work_fn(g_master_thread);
	spdk_rpc_finish();
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
		nvmf_work_fn(g_master_thread);
		goto exit;
	}

	/* Initialize the bdev layer */
	nvmf_bdev_init_start();

	/* Initialize the nvmf tgt */
	rc = nvmf_create_nvmf_tgt();
	if (rc != 0) {
		fprintf(stderr, "failed to create nvmf target\n");
		nvmf_bdev_fini_start();
		g_threads_exit = true;
		nvmf_work_fn(g_master_thread);
		goto exit;
	}

	/* Create poll groups */
	nvmf_tgt_create_poll_groups();

	/* Start all the subsystems */
	nvmf_tgt_start_subsystems();

	nvmf_tgts_run();

	nvmf_tgt_stop_subsystems();
	nvmf_tgt_destroy_poll_groups();
	nvmf_destroy_nvmf_tgt(g_nvmf_tgt);
	nvmf_bdev_fini_start();
exit:
	spdk_env_thread_wait_all();
	nvmf_destroy_threads();
	return rc;
}
