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
#include "spdk/rpc.h"
#include "spdk/nvmf.h"

#include "spdk_internal/event.h"

#define SPDK_NVMF_MAX_NAMESPACES	(1 << 14)
#define SPDK_NVMF_DEFAULT_NAMESPACES	32
#define ACCEPT_TIMEOUT_US		10000 /* 10ms */
#define DEFAULT_CONN_SCHED		CONNECT_SCHED_ROUND_ROBIN

enum spdk_nvmf_connect_sched {
	CONNECT_SCHED_ROUND_ROBIN = 0,
	CONNECT_SCHED_HOST_IP,
	CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP,
};

static const char *g_config_file = NULL;
static const char *g_rpc_addr = SPDK_DEFAULT_RPC_ADDR;
static struct spdk_conf *g_config = NULL;

struct nvmf_thread {
	struct spdk_thread *thread;
	bool exit;

	TAILQ_ENTRY(nvmf_thread) link;
};

struct nvmf_target {
	struct spdk_nvmf_tgt	*tgt;

	struct target_opts {
		int max_subsystems;
		int acceptor_poll_rate;
		int conn_sched;
	} tgt_params;
};

TAILQ_HEAD(, nvmf_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);

static struct nvmf_thread *g_master_thread = NULL;
static struct nvmf_target *g_nvmf_tgt = NULL;
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

	spdk_thread_lib_fini();
}

static void
nvmf_tgt_add_transport_done(void *cb_arg, int status)
{
	/* TODO: Config parsing should wait for this operation to finish. */

	if (status) {
		fprintf(stderr, "Failed to add the transport\n");
	}
}

static int
nvmf_parse_and_create_transport(struct spdk_conf_section *sp)
{
	const char *type;
	struct spdk_nvmf_transport_opts opts = { 0 };
	enum spdk_nvme_transport_type trtype;
	struct spdk_nvmf_transport *transport;
	bool bval;
	int val;

	type = spdk_conf_section_get_val(sp, "Type");
	if (type == NULL) {
		fprintf(stderr, "Transport missing Type\n");
		return -1;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trtype, type)) {
		fprintf(stderr, "Invalid transport type '%s'\n", type);
		return -1;
	}

	if (spdk_nvmf_tgt_get_transport(g_nvmf_tgt->tgt, trtype)) {
		fprintf(stderr, "Duplicate transport type '%s'\n", type);
		return -1;
	}

	if (!spdk_nvmf_transport_opts_init(trtype, &opts)) {
		fprintf(stderr, "spdk_nvmf_transport_opts_init() failed\n");
		return -1;
	}

	val = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (val >= 0) {
		opts.max_queue_depth = val;
	}
	val = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (val >= 0) {
		opts.max_qpairs_per_ctrlr = val;
	}
	val = spdk_conf_section_get_intval(sp, "InCapsuleDataSize");
	if (val >= 0) {
		opts.in_capsule_data_size = val;
	}
	val = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (val >= 0) {
		opts.max_io_size = val;
	}
	val = spdk_conf_section_get_intval(sp, "IOUnitSize");
	if (val >= 0) {
		opts.io_unit_size = val;
	}
	val = spdk_conf_section_get_intval(sp, "MaxAQDepth");
	if (val >= 0) {
		opts.max_aq_depth = val;
	}
	val = spdk_conf_section_get_intval(sp, "NumSharedBuffers");
	if (val >= 0) {
		opts.num_shared_buffers = val;
	}
	val = spdk_conf_section_get_intval(sp, "BufCacheSize");
	if (val >= 0) {
		opts.buf_cache_size = val;
	}

	val = spdk_conf_section_get_intval(sp, "MaxSRQDepth");
	if (val >= 0) {
		if (trtype == SPDK_NVME_TRANSPORT_RDMA) {
			opts.max_srq_depth = val;
		} else {
			fprintf(stderr, "MaxSRQDepth is relevant only for RDMA transport '%s'\n",
				type);
			return -1;
		}
	}

	if (trtype == SPDK_NVME_TRANSPORT_TCP) {
		bval = spdk_conf_section_get_boolval(sp, "C2HSuccess", true);
		opts.c2h_success = bval;
	}

	transport = spdk_nvmf_transport_create(trtype, &opts);
	if (transport) {
		spdk_nvmf_tgt_add_transport(g_nvmf_tgt->tgt, transport,
					    nvmf_tgt_add_transport_done,
					    NULL);
	} else {
		return -1;
	}

	return 0;
}

static int
nvmf_parse_and_create_transports(void)
{
	int rc;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);

	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Transport")) {
			rc = nvmf_parse_and_create_transport(sp);
			if (rc < 0) {
				return rc;
			}
		}
		sp = spdk_conf_next_section(sp);
	}

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
nvmf_read_config_file_nvmf_section(struct spdk_conf_section *sp)
{
	int val;
	char *conn_scheduler;

	val = spdk_conf_section_get_intval(sp, "MaxSubsystems");
	if (val >= 0 && val <= SPDK_NVMF_MAX_NAMESPACES) {
		g_nvmf_tgt->tgt_params.max_subsystems = val;
	}

	val = spdk_conf_section_get_intval(sp, "AcceptorPollRate");
	if (val >= 0) {
		g_nvmf_tgt->tgt_params.acceptor_poll_rate = val;
	}

	conn_scheduler = spdk_conf_section_get_val(sp, "ConnectionScheduler");

	if (conn_scheduler) {
		if (strcasecmp(conn_scheduler, "RoundRobin") == 0) {
			g_nvmf_tgt->tgt_params.conn_sched = CONNECT_SCHED_ROUND_ROBIN;
		} else if (strcasecmp(conn_scheduler, "Host") == 0) {
			g_nvmf_tgt->tgt_params.conn_sched = CONNECT_SCHED_HOST_IP;
		} else if (strcasecmp(conn_scheduler, "Transport") == 0) {
			g_nvmf_tgt->tgt_params.conn_sched = CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP;
		} else {
			fprintf(stderr, "The valid value of ConnectionScheduler should be:\n"
				"\t RoundRobin\n"
				"\t Host\n"
				"\t Transport\n");
			return -1;
		}

	} else {
		fprintf(stderr, "The value of ConnectionScheduler is not configured,\n"
			"we will use RoundRobin as the default scheduler\n");
	}

	return 0;
}

static void
nvmf_tgt_destroy_done(void *ctx, int status)
{
	*(bool *)ctx = true;
}

static void
nvmf_tgt_destroy(struct nvmf_target *nvmf_target)
{
	bool done = false;

	if (!nvmf_target) {
		return;
	}

	if (nvmf_target->tgt) {
		spdk_nvmf_tgt_destroy(nvmf_target->tgt, nvmf_tgt_destroy_done, &done);

		do {
			spdk_thread_poll(g_master_thread->thread, 0, 0);
		} while (!done);
	}

	free(nvmf_target);
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

	return 0;
}

static int
nvmf_parse_and_create_nvmf_tgt(void)
{
	int rc;
	struct spdk_conf_section *sp;
	struct spdk_nvmf_target_opts tgt_opts;

	rc = nvmf_tgt_init();
	if (rc) {
		return rc;
	}

	/* parse nvmf section */
	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp) {
		rc = nvmf_read_config_file_nvmf_section(sp);
		if (rc < 0) {
			fprintf(stderr, "fail to parse the Nvmf section\n");
			free(g_nvmf_tgt);
			return rc;
		}
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
		nvmf_tgt_destroy(g_nvmf_tgt);
		return rc;
	}

	/* prase and create transports */
	rc = nvmf_parse_and_create_transports();
	if (rc != 0) {
		fprintf(stderr, "failed to create and add transports in target\n");
		nvmf_tgt_destroy(g_nvmf_tgt);
		return rc;
	}

	return 0;
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

	/* Initialize and start the RPC service */
	spdk_rpc_initialize(g_rpc_addr);
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	/* Initialize the nvmf tgt */
	rc = nvmf_parse_and_create_nvmf_tgt();
	if (rc != 0) {
		fprintf(stderr, "failed to create nvmf target\n");
		goto exit;
	}

	nvmf_tgt_destroy(g_nvmf_tgt);
exit:
	spdk_rpc_finish();
	nvmf_bdev_fini();
	nvmf_exit_threads();
cleanup:
	spdk_env_thread_wait_all();
	nvmf_destroy_threads();
	spdk_conf_free(g_config);
	return rc;
}
