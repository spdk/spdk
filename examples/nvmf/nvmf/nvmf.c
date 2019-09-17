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
static struct spdk_conf *g_config;

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

static struct nvmf_target *g_nvmf_tgt = NULL;
struct nvmf_thread *g_master_thread;
struct nvmf_tgt_poll_group *g_next_poll_group = NULL;
static bool g_threads_done = false;
static struct spdk_poller *g_acceptor_poller = NULL;

struct nvmf_tgt_host_trid {
	struct spdk_nvme_transport_id host_trid;
	struct nvmf_tgt_poll_group *pg;
	uint32_t ref;
	TAILQ_ENTRY(nvmf_tgt_host_trid) link;
};

/* List of host trids that are connected to the target */
static TAILQ_HEAD(, nvmf_tgt_host_trid) g_nvmf_tgt_host_trids =
	TAILQ_HEAD_INITIALIZER(g_nvmf_tgt_host_trids);

static void nvmf_spdk_tgt_destroy(struct spdk_nvmf_tgt *tgt);
static void nvmf_cleanup_threads(void);
static void nvmf_exit_threads(void);

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
	fprintf(stdout, "bdev layer init done\n");
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
			rc = -ENOMEM;
			goto err_exit;
		}

		thread = spdk_thread_create(thread_name, tmp_cpumask);
		if (thread == NULL) {
			fprintf(stderr, "fail to create thread\n");
			free(nvmf_thread);
			rc = -EINVAL;
			goto err_exit;
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
				spdk_thread_destroy(thread);
				goto err_exit;
			}
		}
	}

	spdk_cpuset_free(tmp_cpumask);
	return 0;
err_exit:
	spdk_cpuset_free(tmp_cpumask);
	nvmf_exit_threads();
	return rc;
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

static void
nvmf_exit_threads(void)
{
	nvmf_cleanup_threads();
	while (!g_threads_done) {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	};
	spdk_thread_lib_fini();
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
	const char *nqn, *mode;
	size_t i;
	int ret;
	int lcore;
	bool allow_any_host;
	const char *sn;
	const char *mn;
	struct spdk_nvmf_subsystem *subsystem;
	int num_ns;

	nqn = spdk_conf_section_get_val(sp, "NQN");
	if (nqn == NULL) {
		fprintf(stderr, "Subsystem missing NQN\n");
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
			fprintf(stdout, "Your mode value is 'Virtual' which is now the only possible mode.\n"
				"Your configuration file will work as expected.\n");
		} else {
			fprintf(stdout, "Please remove Mode from your configuration file.\n");
			return -1;
		}
	}

	/* Core is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (lcore >= 0) {
		fprintf(stdout, "Core present in the [Subsystem] section of the config file.\n"
			"Core was removed as an option. Subsystems can now run on all available cores.\n");
		fprintf(stdout, "Please remove Core from your configuration file. Ignoring it and continuing.\n");
	}

	sn = spdk_conf_section_get_val(sp, "SN");
	if (sn == NULL) {
		fprintf(stderr, "Subsystem %s: missing serial number\n", nqn);
		return -1;
	}

	subsystem = spdk_nvmf_subsystem_create(g_nvmf_tgt->tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, num_ns);
	if (subsystem == NULL) {
		goto done;
	}

	if (spdk_nvmf_subsystem_set_sn(subsystem, sn)) {
		fprintf(stderr, "Subsystem %s: invalid serial number '%s'\n", nqn, sn);
		spdk_nvmf_subsystem_destroy(subsystem);
		subsystem = NULL;
		goto done;
	}

	mn = spdk_conf_section_get_val(sp, "MN");
	if (mn == NULL) {
		fprintf(stdout,
			"Subsystem %s: missing model number, will use default\n",
			nqn);
	}

	if (mn != NULL) {
		if (spdk_nvmf_subsystem_set_mn(subsystem, mn)) {
			fprintf(stderr, "Subsystem %s: invalid model number '%s'\n", nqn, mn);
			spdk_nvmf_subsystem_destroy(subsystem);
			subsystem = NULL;
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
			fprintf(stderr, "Could not find namespace bdev '%s'\n", bdev_name);
			spdk_nvmf_subsystem_destroy(subsystem);
			subsystem = NULL;
			goto done;
		}

		spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));

		nsid_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 1);
		if (nsid_str) {
			char *end;
			unsigned long nsid_ul = strtoul(nsid_str, &end, 0);

			if (*end != '\0' || nsid_ul == 0 || nsid_ul >= UINT32_MAX) {
				fprintf(stderr, "Invalid NSID %s\n", nsid_str);
				spdk_nvmf_subsystem_destroy(subsystem);
				subsystem = NULL;
				goto done;
			}

			ns_opts.nsid = (uint32_t)nsid_ul;
		}

		uuid_str = spdk_conf_section_get_nmval(sp, "Namespace", i, 2);
		if (uuid_str) {
			if (spdk_uuid_parse(&ns_opts.uuid, uuid_str)) {
				fprintf(stderr, "Invalid UUID %s\n", uuid_str);
				spdk_nvmf_subsystem_destroy(subsystem);
				subsystem = NULL;
				goto done;
			}
		}

		if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, &ns_opts, sizeof(ns_opts), NULL) == 0) {
			fprintf(stderr, "Unable to add namespace\n");
			spdk_nvmf_subsystem_destroy(subsystem);
			subsystem = NULL;
			goto done;
		}

		fprintf(stderr, "Attaching block device %s to subsystem %s\n",
			spdk_bdev_get_name(bdev), spdk_nvmf_subsystem_get_nqn(subsystem));
	}

	/* Parse Listen sections */
	for (i = 0; ; i++) {
		struct spdk_nvme_transport_id trid = {0};
		const char *transport;
		const char *address;
		char *address_dup;
		char *host;
		char *port;

		transport = spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		if (!transport) {
			break;
		}

		if (spdk_nvme_transport_id_parse_trtype(&trid.trtype, transport)) {
			fprintf(stderr, "Invalid listen address transport type '%s'\n", transport);
			continue;
		}

		address = spdk_conf_section_get_nmval(sp, "Listen", i, 1);
		if (!address) {
			break;
		}

		address_dup = strdup(address);
		if (!address_dup) {
			break;
		}

		ret = spdk_parse_ip_addr(address_dup, &host, &port);
		if (ret < 0) {
			fprintf(stderr, "Unable to parse listen address '%s'\n", address);
			free(address_dup);
			continue;
		}

		if (strchr(host, ':')) {
			trid.adrfam = SPDK_NVMF_ADRFAM_IPV6;
		} else {
			trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
		}

		snprintf(trid.traddr, sizeof(trid.traddr), "%s", host);
		if (port) {
			snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", port);
		}
		free(address_dup);

		spdk_nvmf_tgt_listen(g_nvmf_tgt->tgt, &trid, nvmf_tgt_listen_done, NULL);

		spdk_nvmf_subsystem_add_listener(subsystem, &trid);
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

done:
	return (subsystem != NULL);
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
			fprintf(stderr, "MaxSRQDepth is relevant only for RDMA transport '%s'\n", type);
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
		fprintf(stderr, "Failed creating discovery nvmf library subsystem\n");
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
	if (val >= 0) {
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

static int
nvmf_parse_and_create_nvmf_tgt(void)
{
	int rc;
	struct spdk_conf_section *sp;
	struct spdk_nvmf_target_opts tgt_opts;

	/* parse nvmf section */
	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp) {
		rc = nvmf_read_config_file_nvmf_section(sp);
		if (rc < 0) {
			fprintf(stderr, "fail to parse the Nvmf section\n");
			return rc;
		}
	}

	tgt_opts.max_subsystems = g_nvmf_tgt->tgt_params.max_subsystems;
	snprintf(tgt_opts.name, sizeof(tgt_opts.name), "%s", "nvmf_example");
	g_nvmf_tgt->tgt = spdk_nvmf_tgt_create(&tgt_opts);
	if (g_nvmf_tgt->tgt == NULL) {
		fprintf(stderr, "spdk_nvmf_tgt_create() failed\n");
		return -EINVAL;
	}

	/* create and add discovery subsystem */
	rc = nvmf_tgt_add_discovery_subsystem(g_nvmf_tgt);
	if (rc != 0) {
		fprintf(stderr, "spdk_add_nvmf_discovery_subsystem() failed\n");
		nvmf_spdk_tgt_destroy(g_nvmf_tgt->tgt);
		return rc;
	}

	/* parse and add transports */
	rc = nvmf_parse_and_create_transports();
	if (rc != 0) {
		fprintf(stderr, "create transports failed\n");
		nvmf_spdk_tgt_destroy(g_nvmf_tgt->tgt);
		return rc;
	}

	/* parse and add subsystems */
	rc = nvmf_parse_and_create_subsystems();
	if (rc != 0) {
		fprintf(stderr, "fail to create subsystems\n");
		nvmf_spdk_tgt_destroy(g_nvmf_tgt->tgt);
		return rc;
	}

	return 0;
}

static void
nvmf_tgt_destroy_done(void *ctx, int status)
{
	*(bool *)ctx = true;
}

static void
nvmf_spdk_tgt_destroy(struct spdk_nvmf_tgt *tgt)
{
	bool done = false;

	spdk_nvmf_tgt_destroy(tgt, nvmf_tgt_destroy_done, &done);

	do {
		spdk_thread_poll(g_master_thread->thread, 0, 0);
	} while (!done);
}

static void
nvmf_tgt_create_poll_groups_done(void *ctx)
{
	bool *done = ctx;

	*done = true;
	fprintf(stdout, "create target channels done\n");
}

static void
nvmf_tgt_create_poll_group(void *ctx)
{
	struct nvmf_tgt_poll_group *pg;
	struct spdk_io_channel *ch;

	pg = calloc(1, sizeof(struct nvmf_tgt_poll_group));
	if (!pg) {
		fprintf(stderr, "Not enough memory to allocate poll groups\n");
		return;
	}

	pg->thread = spdk_get_thread();
	ch = spdk_get_io_channel(g_nvmf_tgt->tgt);
	if (!ch) {
		fprintf(stderr, "Unable to get I/O channel for target\n");
		free(pg);
		return;
	}

	pg->group = spdk_io_channel_get_ctx(ch);
	TAILQ_INSERT_TAIL(&g_nvmf_tgt->poll_groups, pg, link);
	g_nvmf_tgt->poll_group_counter++;

	if (g_next_poll_group == NULL) {
		g_next_poll_group = pg;
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
	fprintf(stdout, "destroy target channels done\n");
}

static void
nvmf_tgt_destroy_poll_group(void *ctx)
{
	struct nvmf_tgt_poll_group *pg, *tpg;
	struct spdk_thread *thread;

	thread = spdk_get_thread();

	TAILQ_FOREACH_SAFE(pg, &g_nvmf_tgt->poll_groups, link, tpg) {
		if (pg->thread == thread) {
			TAILQ_REMOVE(&g_nvmf_tgt->poll_groups, pg, link);
			spdk_nvmf_poll_group_destroy(pg->group);
			free(pg);
			assert(g_nvmf_tgt->poll_group_counter > 0);
			g_nvmf_tgt->poll_group_counter--;
			return;
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

	fprintf(stdout, "all the subsystems started\n");
	*done = true;
}

static void
nvmf_tgt_start_subsystems(void)
{
	bool done = false;
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = spdk_nvmf_subsystem_get_first(g_nvmf_tgt->tgt);
	if (subsystem) {
		spdk_nvmf_subsystem_start(subsystem,
					  nvmf_tgt_subsystem_start_next,
					  &done);

		do {
			spdk_thread_poll(g_master_thread->thread, 0, 0);
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

	fprintf(stdout, "all subsystems stoped\n");
	*done = true;
}

static void
nvmf_tgt_stop_subsystems(void)
{
	bool done = false;
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = spdk_nvmf_subsystem_get_first(g_nvmf_tgt->tgt);
	if (subsystem) {
		spdk_nvmf_subsystem_stop(subsystem,
					 nvmf_tgt_subsystem_stop_next,
					 &done);

		do {
			spdk_thread_poll(g_master_thread->thread, 0, 0);
		} while (!done);
	}
}

struct nvmf_tgt_pg_ctx {
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_tgt_poll_group *pg;
};

static void
nvmf_tgt_poll_group_add(void *_ctx)
{
	struct nvmf_tgt_pg_ctx *ctx = _ctx;
	struct spdk_nvmf_qpair *qpair = ctx->qpair;
	struct nvmf_tgt_poll_group *pg = ctx->pg;

	free(_ctx);

	if (spdk_nvmf_poll_group_add(pg->group, qpair) != 0) {
		fprintf(stderr, "Unable to add the qpair to a poll group.\n");
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
	}
}

/* Round robin selection of poll groups */
static struct nvmf_tgt_poll_group *
nvmf_tgt_get_next_pg(void)
{
	struct nvmf_tgt_poll_group *pg;

	pg = g_next_poll_group;
	g_next_poll_group = TAILQ_NEXT(pg, link);
	if (g_next_poll_group == NULL) {
		g_next_poll_group = TAILQ_FIRST(&g_nvmf_tgt->poll_groups);
	}

	return pg;
}

static struct nvmf_tgt_poll_group *
nvmf_get_optimal_pg(struct spdk_nvmf_qpair *qpair)
{
	struct nvmf_tgt_poll_group *pg, *_pg = NULL;
	struct spdk_nvmf_poll_group *group = spdk_nvmf_get_optimal_poll_group(qpair);

	if (group == NULL) {
		_pg = nvmf_tgt_get_next_pg();
		goto end;
	}

	TAILQ_FOREACH(pg, &g_nvmf_tgt->poll_groups, link) {
		if (pg->group == group) {
			_pg = pg;
			break;
		}
	}

end:
	assert(_pg != NULL);
	return _pg;
}

static struct nvmf_tgt_poll_group *
nvmf_tgt_get_pg(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid;
	struct nvmf_tgt_host_trid *tmp_trid = NULL, *new_trid = NULL;
	struct nvmf_tgt_poll_group *pg;
	int ret;

	switch (g_nvmf_tgt->tgt_params.conn_sched) {
	case CONNECT_SCHED_HOST_IP:
		ret = spdk_nvmf_qpair_get_peer_trid(qpair, &trid);
		if (ret) {
			pg = g_next_poll_group;
			fprintf(stderr, "Invalid host transport Id. Assigning to poll group %p\n", pg);
			break;
		}

		TAILQ_FOREACH(tmp_trid, &g_nvmf_tgt_host_trids, link) {
			if (tmp_trid && !strncmp(tmp_trid->host_trid.traddr,
						 trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1)) {
				tmp_trid->ref++;
				pg = tmp_trid->pg;
				break;
			}
		}
		if (!tmp_trid) {
			new_trid = calloc(1, sizeof(*new_trid));
			if (!new_trid) {
				pg = g_next_poll_group;
				fprintf(stderr, "Insufficient memory. Assigning to poll group %p\n", pg);
				break;
			}
			/* Get the next available poll group for the new host */
			pg = nvmf_tgt_get_next_pg();
			new_trid->pg = pg;
			memcpy(new_trid->host_trid.traddr, trid.traddr,
			       SPDK_NVMF_TRADDR_MAX_LEN + 1);
			TAILQ_INSERT_TAIL(&g_nvmf_tgt_host_trids, new_trid, link);
		}
		break;
	case CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP:
		pg = nvmf_get_optimal_pg(qpair);
		break;
	case CONNECT_SCHED_ROUND_ROBIN:
	default:
		pg = nvmf_tgt_get_next_pg();
		break;
	}

	return pg;
}

static void
nvmf_tgt_remove_host_trid(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid_to_remove;
	struct nvmf_tgt_host_trid *trid = NULL, *tmp_trid = NULL;

	if (g_nvmf_tgt->tgt_params.conn_sched != CONNECT_SCHED_HOST_IP) {
		return;
	}

	if (spdk_nvmf_qpair_get_peer_trid(qpair, &trid_to_remove) != 0) {
		return;
	}

	TAILQ_FOREACH_SAFE(trid, &g_nvmf_tgt_host_trids, link, tmp_trid) {
		if (trid && !strncmp(trid->host_trid.traddr,
				     trid_to_remove.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1)) {
			trid->ref--;
			if (trid->ref == 0) {
				TAILQ_REMOVE(&g_nvmf_tgt_host_trids, trid, link);
				free(trid);
			}

			break;
		}
	}

	return;
}

static void
new_qpair(struct spdk_nvmf_qpair *qpair)
{
	uint32_t attempts;
	struct nvmf_tgt_pg_ctx *ctx;
	struct nvmf_tgt_poll_group *pg;

	for (attempts = 0; attempts < g_nvmf_tgt->poll_group_counter; attempts++) {
		pg = nvmf_tgt_get_pg(qpair);
		if (pg->group != NULL) {
			break;
		} else {
			nvmf_tgt_remove_host_trid(qpair);
		}
	}

	if (attempts == g_nvmf_tgt->poll_group_counter) {
		fprintf(stderr, "No poll groups exist.\n");
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		fprintf(stderr, "Unable to send message to poll group.\n");
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		return;
	}

	ctx->qpair = qpair;
	ctx->pg = pg;

	spdk_thread_send_msg(pg->thread, nvmf_tgt_poll_group_add, ctx);
}

static int
acceptor_poll(void *arg)
{
	spdk_nvmf_tgt_accept(g_nvmf_tgt->tgt, new_qpair);

	return -1;
}

static void
nvmf_tgt_run(void)
{
	g_acceptor_poller = spdk_poller_register(acceptor_poll, g_nvmf_tgt->tgt,
			    g_nvmf_tgt->tgt_params.acceptor_poll_rate);

	nvmf_work_fn(g_master_thread);
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
		rc = -ENOMEM;
		goto cleanup;
	}

	/* Initialize the threads */
	rc = nvmf_init_threads();
	if (rc != 0) {
		fprintf(stderr, "Failed to create initialization thread\n");
		goto cleanup;
	}

	/* Initialize the bdev layer */
	nvmf_bdev_init();

	/* Initialize the nvmf tgt */
	rc = nvmf_parse_and_create_nvmf_tgt();
	if (rc != 0) {
		fprintf(stderr, "create nvmf_tgt failed\n");
		nvmf_bdev_fini();
		nvmf_exit_threads();
		goto cleanup;
	}

	/* allocate the IO_Channels of the target */
	nvmf_tgt_create_poll_groups();

	nvmf_tgt_start_subsystems();

	nvmf_tgt_run();

	nvmf_tgt_stop_subsystems();
	nvmf_tgt_destroy_poll_groups();
	nvmf_spdk_tgt_destroy(g_nvmf_tgt->tgt);
	nvmf_bdev_fini();
	nvmf_exit_threads();
cleanup:
	spdk_env_thread_wait_all();
	destroy_nvmf_tgt(g_nvmf_tgt);
	spdk_conf_free(g_config);
	return rc;
}
