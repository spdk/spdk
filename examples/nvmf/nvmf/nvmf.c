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
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/rpc.h"

#include "spdk_internal/event.h"

#define SPDK_NVMF_MAX_NAMESPACES	(1 << 14)
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

struct nvmf_target_opts {
	int max_subsystems;
	int acceptor_poll_rate;
	int conn_sched;
};

struct nvmf_transport_params {
	enum spdk_nvme_transport_type trtype;
	struct spdk_nvmf_transport_opts opts;

	TAILQ_ENTRY(nvmf_transport_params) link;
};

struct nvmf_transport_id {
	struct spdk_nvme_transport_id trid;

	TAILQ_ENTRY(nvmf_transport_id) link;
};

struct nvmf_thread {
	struct spdk_thread *thread;
	bool exit;

	TAILQ_ENTRY(nvmf_thread) link;
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
	struct spdk_poller		*acceptor_poller;
	struct nvmf_target_poll_group	*next_pg;

	TAILQ_HEAD(, nvmf_transport_id) tgt_trids;
	TAILQ_HEAD(, nvmf_target_poll_group) poll_groups;
	TAILQ_HEAD(, nvmf_target_host_trid) host_trids;
	TAILQ_ENTRY(nvmf_target) link;
};

TAILQ_HEAD(, nvmf_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);
TAILQ_HEAD(, nvmf_target) g_nvmf_tgts = TAILQ_HEAD_INITIALIZER(g_nvmf_tgts);
TAILQ_HEAD(, nvmf_transport_params) g_nvmf_transport_types =
	TAILQ_HEAD_INITIALIZER(g_nvmf_transport_types);

static struct nvmf_target *current_acceptor_tgt = NULL;
static struct nvmf_target_opts g_nvmf_tgt_opts;
static struct nvmf_thread *g_master_thread = NULL;
static bool g_threads_done = false;
static uint32_t g_tgt_counter = 0;

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

static int
init_nvmf_target_opts(struct nvmf_target_opts *opts)
{
	if (!opts) {
		return -1;
	}

	opts->acceptor_poll_rate = ACCEPT_TIMEOUT_US;
	opts->conn_sched = DEFAULT_CONN_SCHED;

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

static int
nvmf_tgt_add_discovery_subsystem(struct nvmf_target *nvmf_tgt)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = spdk_nvmf_subsystem_create(nvmf_tgt->tgt, SPDK_NVMF_DISCOVERY_NQN,
					       SPDK_NVMF_SUBTYPE_DISCOVERY, 0);
	if (subsystem == NULL) {
		fprintf(stderr, "Failed creating discovery nvmf library subsystem\n");
		return -EINVAL;
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, true);

	return 0;
}

static void
nvmf_destroy_spdk_nvmf_tgt_done(void *ctx, int status)
{
	*(bool *)ctx = true;
}

static void
nvmf_destroy_spdk_nvmf_tgt(struct spdk_nvmf_tgt *tgt)
{
	bool done = false;

	spdk_nvmf_tgt_destroy(tgt, nvmf_destroy_spdk_nvmf_tgt_done, &done);
	do {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	} while (!done);
}

static struct nvmf_target *
nvmf_create_nvmf_tgt(void)
{
	struct spdk_nvmf_target_opts tgt_opts;
	struct nvmf_target *nvmf_tgt;
	int rc;

	tgt_opts.max_subsystems = g_nvmf_tgt_opts.max_subsystems;
	snprintf(tgt_opts.name, sizeof(tgt_opts.name), "%s_%d", "nvmf_example",
		 g_tgt_counter++);

	nvmf_tgt = calloc(1, sizeof(struct nvmf_target));
	if (!nvmf_tgt) {
		fprintf(stderr, "failed to allocate the nvmf target\n");
		return NULL;
	}

	nvmf_tgt->tgt = spdk_nvmf_tgt_create(&tgt_opts);
	if (nvmf_tgt->tgt == NULL) {
		fprintf(stderr, "spdk_nvmf_tgt_create() failed\n");
		free(nvmf_tgt);
		return NULL;
	}

	/* Create and add discovery subsystem */
	rc = nvmf_tgt_add_discovery_subsystem(nvmf_tgt);
	if (rc != 0) {
		fprintf(stderr, "spdk_add_nvmf_discovery_subsystem() failed\n");
		nvmf_destroy_spdk_nvmf_tgt(nvmf_tgt->tgt);
		free(nvmf_tgt);
		return NULL;
	}

	TAILQ_INSERT_TAIL(&g_nvmf_tgts, nvmf_tgt, link);
	TAILQ_INIT(&nvmf_tgt->tgt_trids);
	TAILQ_INIT(&nvmf_tgt->poll_groups);
	TAILQ_INIT(&nvmf_tgt->host_trids);

	return nvmf_tgt;
}

static void
nvmf_destroy_nvmf_tgt(struct nvmf_target *nvmf_tgt)
{
	struct nvmf_transport_id *trid, *tmp;
	struct nvmf_target_host_trid *host_trid, *host_tmp;

	if (nvmf_tgt) {
		TAILQ_REMOVE(&g_nvmf_tgts, nvmf_tgt, link);

		if (nvmf_tgt->tgt) {
			nvmf_destroy_spdk_nvmf_tgt(nvmf_tgt->tgt);
		}

		TAILQ_FOREACH_SAFE(trid, &nvmf_tgt->tgt_trids, link, tmp) {
			free(trid);
		}

		TAILQ_FOREACH_SAFE(host_trid, &nvmf_tgt->host_trids, link, host_tmp) {
			free(host_trid);
		}

		free(nvmf_tgt);
	}
}

static void
nvmf_tgt_add_transport_done(void *cb_arg, int status)
{
	bool *result = cb_arg;

	/* TODO: Config parsing should wait for this operation to finish. */
	if (status) {
		fprintf(stderr, "failed to add the transport\n");
	}

	*result = true;
}

static int
nvmf_tgt_create_and_add_transports(struct nvmf_target *nvmf_tgt)
{
	struct nvmf_transport_params *param;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_transport_opts opts;
	bool done;

	if (nvmf_tgt == NULL || nvmf_tgt->tgt == NULL) {
		return -1;
	}

	TAILQ_FOREACH(param, &g_nvmf_transport_types, link) {
		done = false;

		opts = param->opts;
		transport = spdk_nvmf_transport_create(nvmf_tgt->tgt, param->trtype, &opts);
		if (transport) {
			spdk_nvmf_tgt_add_transport(nvmf_tgt->tgt, transport,
						    nvmf_tgt_add_transport_done,
						    &done);

			while (!done) {
				spdk_thread_poll(g_master_thread->thread, 0, 0);
			}
		} else {
			fprintf(stderr, "failed to create transport\n");
			return -1;
		}
	}

	return 0;
}

static void
nvmf_tgt_listen_done(void *cb_arg, int status)
{
	/* TODO: Config parsing should wait for this operation to finish. */

	if (status) {
		fprintf(stderr, "Failed to listen on transport address\n");
	}
}

static int
nvmf_parse_and_create_subsystem(struct spdk_conf_section *sp)
{
	TAILQ_HEAD(, nvmf_transport_id) subsystem_trids;
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_transport_id *trid, *tmp, *tmp1;
	struct nvmf_target *nvmf_tgt;
	const char *nqn, *mode;
	const char *sn;
	const char *mn;
	int i, ret, lcore, num_ns;
	bool allow_any_host;

	TAILQ_INIT(&subsystem_trids);

	nqn = spdk_conf_section_get_val(sp, "NQN");
	if (nqn == NULL) {
		fprintf(stderr, "subsystem missing NQN\n");
		return -1;
	}

	mode = spdk_conf_section_get_val(sp, "Mode");
	lcore = spdk_conf_section_get_intval(sp, "Core");
	num_ns = spdk_conf_section_get_intval(sp, "MaxNamespaces");

	if (num_ns < 1) {
		num_ns = 0;
	} else if (num_ns > SPDK_NVMF_MAX_NAMESPACES) {
		num_ns = SPDK_NVMF_MAX_NAMESPACES;
	}

	/* Mode is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (mode) {
		if (strcasecmp(mode, "Virtual") == 0) {
			fprintf(stdout, "your mode value is 'Virtual' which is now the only possible mode.\n"
				"Your configuration file will work as expected.\n");
		} else {
			fprintf(stdout, "please remove Mode from your configuration file.\n");
			return -1;
		}
	}

	/* Core is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (lcore >= 0) {
		fprintf(stdout, "core present in the [Subsystem] section of the config file.\n"
			"Core was removed as an option. Subsystems can now run on all available cores.\n");
		fprintf(stdout, "please remove Core from your configuration file. Ignoring it and continuing.\n");
	}

	sn = spdk_conf_section_get_val(sp, "SN");
	if (sn == NULL) {
		fprintf(stderr, "subsystem %s: missing serial number\n", nqn);
		return -1;
	}

	/* Parse Listen sections first to identify the intersection
	 * If no listen interface is used repeatedly
	 * Then this subsystem can construct a nvmf_target independently
	 */
	for (i = 0; ; i++) {
		const char *transport;
		const char *address;
		char *address_dup;
		char *host;
		char *port;

		transport = spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		if (!transport) {
			break;
		}

		trid = calloc(1, sizeof(struct nvmf_transport_id));
		if (!trid) {
			fprintf(stderr, "failed to alloc trid\n");
			goto cleanup;
		}

		if (spdk_nvme_transport_id_parse_trtype(&trid->trid.trtype, transport)) {
			fprintf(stderr, "invalid listen address transport type '%s'\n", transport);
			free(trid);
			continue;
		}

		address = spdk_conf_section_get_nmval(sp, "Listen", i, 1);
		if (!address) {
			free(trid);
			break;
		}

		address_dup = strdup(address);
		if (!address_dup) {
			free(trid);
			break;
		}

		ret = spdk_parse_ip_addr(address_dup, &host, &port);
		if (ret < 0) {
			fprintf(stderr, "unable to parse listen address '%s'\n", address);
			free(address_dup);
			free(trid);
			continue;
		}

		if (strchr(host, ':')) {
			trid->trid.adrfam = SPDK_NVMF_ADRFAM_IPV6;
		} else {
			trid->trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
		}

		snprintf(trid->trid.traddr, sizeof(trid->trid.traddr), "%s", host);
		if (port) {
			snprintf(trid->trid.trsvcid, sizeof(trid->trid.trsvcid), "%s", port);
		}
		free(address_dup);

		/* Reject the same trid */
		TAILQ_FOREACH(tmp, &subsystem_trids, link) {
			if (!spdk_nvme_transport_id_compare(&trid->trid, &tmp->trid)) {
				fprintf(stderr, "this trid already existed\n");
				break;
			}
		}

		/* A different trid then save it */
		if (tmp == NULL) {
			TAILQ_INSERT_TAIL(&subsystem_trids, trid, link);
		} else {
			free(trid);
		}
	}

	/* Traver all the targets to find the same trid
	 * If find the same trid then add this subsystem to this target
	 * If not can create an indenpent target
	 */
	TAILQ_FOREACH(nvmf_tgt, &g_nvmf_tgts, link) {
		TAILQ_FOREACH(trid, &subsystem_trids, link) {
			TAILQ_FOREACH(tmp, &nvmf_tgt->tgt_trids, link) {
				if (!spdk_nvme_transport_id_compare(&trid->trid, &tmp->trid)) {
					break;
				}
			}

			if (tmp != NULL) {
				break;
			}
		}

		if (trid != NULL) {
			break;
		}
	}

	/* Without intersection can create a new target */
	if (nvmf_tgt == NULL) {
		/* Each subsystem create a spdk_nvmf_tgt and add the transports independently */
		nvmf_tgt = nvmf_create_nvmf_tgt();
		if (!nvmf_tgt) {
			fprintf(stderr, "failed to create nvmf target\n");
			goto cleanup;
		}

		ret = nvmf_tgt_create_and_add_transports(nvmf_tgt);
		if (ret) {
			fprintf(stderr, "faied to create and add transport\n");
			goto cleanup;
		}
	}

	/* each subsystem is a target and has the discovery subsystem */
	subsystem = spdk_nvmf_subsystem_create(nvmf_tgt->tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, num_ns);
	if (subsystem == NULL) {
		goto done;
	}

	if (spdk_nvmf_subsystem_set_sn(subsystem, sn)) {
		fprintf(stderr, "subsystem %s: invalid serial number '%s'\n", nqn, sn);
		goto done;
	}

	mn = spdk_conf_section_get_val(sp, "MN");
	if (mn == NULL) {
		fprintf(stdout,
			"subsystem %s: missing model number, will use default\n",
			nqn);
	}

	if (mn != NULL) {
		if (spdk_nvmf_subsystem_set_mn(subsystem, mn)) {
			fprintf(stderr, "subsystem %s: invalid model number '%s'\n", nqn, mn);
			goto done;
		}
	}

	/* Parse Namespace sections */
	for (i = 0; ; i++) {
		struct spdk_nvmf_ns_opts ns_opts;
		struct spdk_bdev *bdev;
		const char *bdev_name;
		const char *uuid_str;
		char *nsid_str;

		bdev_name = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
		if (!bdev_name) {
			break;
		}

		bdev = spdk_bdev_get_by_name(bdev_name);
		if (bdev == NULL) {
			fprintf(stderr, "could not find namespace bdev '%s'\n", bdev_name);
			nvmf_destroy_nvmf_tgt(nvmf_tgt);
			goto done;
		}

		spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));

		nsid_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 1);
		if (nsid_str) {
			char *end;
			unsigned long nsid_ul = strtoul(nsid_str, &end, 0);

			if (*end != '\0' || nsid_ul == 0 || nsid_ul >= UINT32_MAX) {
				fprintf(stderr, "invalid NSID %s\n", nsid_str);
				goto done;
			}

			ns_opts.nsid = (uint32_t)nsid_ul;
		}

		uuid_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 2);
		if (uuid_str) {
			if (spdk_uuid_parse(&ns_opts.uuid, uuid_str)) {
				fprintf(stderr, "invalid UUID %s\n", uuid_str);
				goto done;
			}
		}

		if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, &ns_opts, sizeof(ns_opts), NULL) == 0) {
			fprintf(stderr, "unable to add namespace\n");
			goto done;
		}

		fprintf(stderr, "attaching block device %s to subsystem %s\n",
			spdk_bdev_get_name(bdev), spdk_nvmf_subsystem_get_nqn(subsystem));
	}

	/* Add the listen port */
	TAILQ_FOREACH_SAFE(trid, &subsystem_trids, link, tmp1) {
		spdk_nvmf_subsystem_add_listener(subsystem, &trid->trid);

		TAILQ_FOREACH(tmp, &nvmf_tgt->tgt_trids, link) {
			if (!spdk_nvme_transport_id_compare(&trid->trid, &tmp->trid)) {
				break;
			}
		}

		TAILQ_REMOVE(&subsystem_trids, trid, link);

		if (tmp == NULL) {
			spdk_nvmf_tgt_listen(nvmf_tgt->tgt, &trid->trid, nvmf_tgt_listen_done, NULL);
			TAILQ_INSERT_TAIL(&nvmf_tgt->tgt_trids, trid, link);
		} else {
			free(trid);
		}
	}

	/* Parse Host sections */
	for (i = 0; ; i++) {
		const char *host = spdk_conf_section_get_nval(sp, "Host", i);
		if (!host) {
			break;
		}

		spdk_nvmf_subsystem_add_host(subsystem, host);
	}

	allow_any_host = spdk_conf_section_get_boolval(sp, "AllowAnyHost", false);
	spdk_nvmf_subsystem_set_allow_any_host(subsystem, allow_any_host);

	return 0;
cleanup:
	TAILQ_FOREACH_SAFE(trid, &subsystem_trids, link, tmp) {
		free(trid);
	}
done:
	return -1;
}

static int
nvmf_parse_and_create_subsystems(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Subsystem")) {
			rc = nvmf_parse_and_create_subsystem(sp);
			if (rc < 0) {
				return rc;
			}
		}
		sp = spdk_conf_next_section(sp);
	}

	return 0;
}

static void
nvmf_delete_transports(void)
{
	struct nvmf_transport_params *param, *temp;

	TAILQ_FOREACH_SAFE(param, &g_nvmf_transport_types, link, temp) {
		free(param);
	}
}

static int
nvmf_parse_transport(struct spdk_conf_section *sp)
{
	const char *type;
	enum spdk_nvme_transport_type trtype;
	struct nvmf_transport_params *params;
	bool bval;
	int val;

	type = spdk_conf_section_get_val(sp, "Type");
	if (type == NULL) {
		fprintf(stderr, "transport missing Type\n");
		return -1;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trtype, type)) {
		fprintf(stderr, "invalid transport type '%s'\n", type);
		return -1;
	}

	TAILQ_FOREACH(params, &g_nvmf_transport_types, link) {
		if (params->trtype == trtype) {
			fprintf(stderr, "trtype already exist\n");
			return -1;
		}
	}

	params = calloc(1, sizeof(struct nvmf_transport_params));
	if (!params) {
		fprintf(stderr, "failed to alloc transport parameter\n");
		return -1;
	}

	params->trtype = trtype;

	if (!spdk_nvmf_transport_opts_init(trtype, &params->opts)) {
		fprintf(stderr, "spdk_nvmf_transport_opts_init() failed\n");
		free(params);
		return -1;
	}

	val = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (val >= 0) {
		params->opts.max_queue_depth = val;
	}
	val = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (val >= 0) {
		params->opts.max_qpairs_per_ctrlr = val;
	}
	val = spdk_conf_section_get_intval(sp, "InCapsuleDataSize");
	if (val >= 0) {
		params->opts.in_capsule_data_size = val;
	}
	val = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (val >= 0) {
		params->opts.max_io_size = val;
	}
	val = spdk_conf_section_get_intval(sp, "IOUnitSize");
	if (val >= 0) {
		params->opts.io_unit_size = val;
	}
	val = spdk_conf_section_get_intval(sp, "MaxAQDepth");
	if (val >= 0) {
		params->opts.max_aq_depth = val;
	}
	val = spdk_conf_section_get_intval(sp, "NumSharedBuffers");
	if (val >= 0) {
		params->opts.num_shared_buffers = val;
	}
	val = spdk_conf_section_get_intval(sp, "BufCacheSize");
	if (val >= 0) {
		params->opts.buf_cache_size = val;
	}

	val = spdk_conf_section_get_intval(sp, "MaxSRQDepth");
	if (val >= 0) {
		if (trtype == SPDK_NVME_TRANSPORT_RDMA) {
			params->opts.max_srq_depth = val;
		} else {
			fprintf(stderr, "MaxSRQDepth is relevant only for RDMA transport '%s'\n", type);
			free(params);
			return -1;
		}
	}

	if (trtype == SPDK_NVME_TRANSPORT_TCP) {
		bval = spdk_conf_section_get_boolval(sp, "C2HSuccess", true);
		params->opts.c2h_success = bval;
	}

	TAILQ_INSERT_TAIL(&g_nvmf_transport_types, params, link);

	return 0;
}

static int
nvmf_parse_transports(void)
{
	int rc;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);

	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Transport")) {
			rc = nvmf_parse_transport(sp);
			if (rc < 0) {
				return rc;
			}
		}
		sp = spdk_conf_next_section(sp);
	}

	return 0;
}

static int
nvmf_read_config_file_nvmf_section(struct spdk_conf_section *sp)
{
	int val;
	char *conn_scheduler;

	val = spdk_conf_section_get_intval(sp, "MaxSubsystems");
	if (val >= 0) {
		g_nvmf_tgt_opts.max_subsystems = val;
	}

	val = spdk_conf_section_get_intval(sp, "AcceptorPollRate");
	if (val >= 0) {
		g_nvmf_tgt_opts.acceptor_poll_rate = val;
	}

	conn_scheduler = spdk_conf_section_get_val(sp, "ConnectionScheduler");

	if (conn_scheduler) {
		if (strcasecmp(conn_scheduler, "RoundRobin") == 0) {
			g_nvmf_tgt_opts.conn_sched = CONNECT_SCHED_ROUND_ROBIN;
		} else if (strcasecmp(conn_scheduler, "Host") == 0) {
			g_nvmf_tgt_opts.conn_sched = CONNECT_SCHED_HOST_IP;
		} else if (strcasecmp(conn_scheduler, "Transport") == 0) {
			g_nvmf_tgt_opts.conn_sched =
				CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP;
		} else {
			fprintf(stderr, "the valid value of ConnectionScheduler should be:\n"
				"\t RoundRobin\n"
				"\t Host\n"
				"\t Transport\n");
			return -1;
		}

	} else {
		fprintf(stderr, "the value of ConnectionScheduler is not configured,\n"
			"we will use RoundRobin as the default scheduler\n");
	}

	return 0;
}

static int
nvmf_destroy_nvmf_tgts(void)
{
	struct nvmf_target *nvmf_tgt, *temp;

	/* Delete all the transport types */
	nvmf_delete_transports();

	TAILQ_FOREACH_SAFE(nvmf_tgt, &g_nvmf_tgts, link, temp) {
		nvmf_destroy_nvmf_tgt(nvmf_tgt);
	}

	return 0;
}

static int
nvmf_parse_and_create_nvmf_tgts(void)
{
	int rc;
	struct spdk_conf_section *sp;

	/* Parse nvmf section */
	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp) {
		rc = nvmf_read_config_file_nvmf_section(sp);
		if (rc < 0) {
			fprintf(stderr, "fail to parse the Nvmf section\n");
			return rc;
		}
	}

	/* Parse transports configuration */
	rc = nvmf_parse_transports();
	if (rc != 0) {
		fprintf(stderr, "create transports failed\n");
		nvmf_delete_transports();
		return rc;
	}

	/* Parse subsystems and create targets */
	rc = nvmf_parse_and_create_subsystems();
	if (rc != 0) {
		fprintf(stderr, "fail to create subsystems\n");
		nvmf_destroy_nvmf_tgts();
		return rc;
	}

	return 0;
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
	struct nvmf_target *nvmf_tgt;

	TAILQ_FOREACH(nvmf_tgt, &g_nvmf_tgts, link) {
		pg = calloc(1, sizeof(struct nvmf_target_poll_group));
		if (!pg) {
			fprintf(stderr, "failed to allocate poll group\n");
			return;
		}

		pg->thread = spdk_get_thread();
		pg->group = spdk_nvmf_poll_group_create(nvmf_tgt->tgt);
		TAILQ_INSERT_TAIL(&nvmf_tgt->poll_groups, pg, link);

		if (!nvmf_tgt->next_pg) {
			nvmf_tgt->next_pg = pg;
		}
	}
}

static void
nvmf_tgt_create_poll_groups(void)
{
	bool done = false;

	spdk_for_each_thread(nvmf_tgt_create_poll_group,
			     &done,
			     nvmf_tgt_create_poll_groups_done);

	do {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
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
	struct nvmf_target *nvmf_tgt;
	struct spdk_thread *thread;

	thread = spdk_get_thread();

	TAILQ_FOREACH(nvmf_tgt, &g_nvmf_tgts, link) {
		TAILQ_FOREACH_SAFE(pg, &nvmf_tgt->poll_groups, link, tmp) {
			if (pg->thread == thread) {
				TAILQ_REMOVE(&nvmf_tgt->poll_groups, pg, link);
				spdk_nvmf_poll_group_destroy(pg->group);
				free(pg);
				break;
			}
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
		spdk_thread_poll(g_master_thread->thread, 0, 0);
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
	struct nvmf_target *nvmf_tgt;
	bool done = false;

	TAILQ_FOREACH(nvmf_tgt, &g_nvmf_tgts, link) {
		subsystem = spdk_nvmf_subsystem_get_first(nvmf_tgt->tgt);
		if (subsystem) {
			spdk_nvmf_subsystem_start(subsystem,
						  nvmf_tgt_subsystem_start_next,
						  &done);

			do {
				spdk_thread_poll(g_master_thread->thread, 0, 0);
			} while (!done);
		}
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
	struct nvmf_target *nvmf_tgt;
	bool done = false;

	TAILQ_FOREACH(nvmf_tgt, &g_nvmf_tgts, link) {
		subsystem = spdk_nvmf_subsystem_get_first(nvmf_tgt->tgt);
		if (subsystem) {
			spdk_nvmf_subsystem_stop(subsystem,
						 nvmf_tgt_subsystem_stop_next,
						 &done);

			do {
				spdk_thread_poll(g_master_thread->thread, 0, 0);
			} while (!done);
		}
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

	if (g_nvmf_tgt_opts.conn_sched != CONNECT_SCHED_HOST_IP) {
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

	switch (g_nvmf_tgt_opts.conn_sched) {
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
	struct nvmf_target *nvmf_tgt = current_acceptor_tgt;

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

	current_acceptor_tgt = nvmf_tgt;
	spdk_nvmf_tgt_accept(nvmf_tgt->tgt, new_qpair, NULL);

	return -1;
}

static void
nvmf_tgts_run(void)
{
	struct nvmf_target *nvmf_tgt;

	/*
	 * Register poller for each target or only have one poller
	 * and foreach targets?
	 * perfer the first choice. But it seems there is no RPC cmd
	 * for registering acceptor poller.
	 */
	TAILQ_FOREACH(nvmf_tgt, &g_nvmf_tgts, link) {
		nvmf_tgt->acceptor_poller = spdk_poller_register(acceptor_poll, nvmf_tgt,
					    g_nvmf_tgt_opts.acceptor_poll_rate);
	}

	fprintf(stdout, "start master work function\n");

	nvmf_work_fn(g_master_thread);
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
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	rc = init_nvmf_target_opts(&g_nvmf_tgt_opts);
	if (rc) {
		fprintf(stderr, "failed to init nvmf target option\n");
		goto exit;
	}

	/* Initialize the nvmf tgt */
	rc = nvmf_parse_and_create_nvmf_tgts();
	if (rc != 0) {
		fprintf(stderr, "create nvmf_tgts failed\n");
		goto exit;
	}

	/* Create poll groups of each target */
	nvmf_tgt_create_poll_groups();

	/* Start all the subsystems */
	nvmf_tgt_start_subsystems();

	nvmf_tgts_run();

	nvmf_tgt_stop_subsystems();
	nvmf_tgt_destroy_poll_groups();
	nvmf_destroy_nvmf_tgts();
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
