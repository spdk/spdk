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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "spdk/env.h"
#include "nvmf_tgt.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"

#define MAX_LISTEN_ADDRESSES 255
#define MAX_HOSTS 255
#define PORTNUMSTRLEN 32
#define SPDK_NVMF_DEFAULT_SIN_PORT ((uint16_t)4420)

#define ACCEPT_TIMEOUT_US		10000 /* 10ms */

struct spdk_nvmf_probe_ctx {
	struct nvmf_tgt_subsystem	*app_subsystem;
	bool				any;
	bool				found;
	struct spdk_nvme_transport_id	trid;
};

#define MAX_STRING_LEN 255

#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_DEFAULT 4
#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MIN 2
#define SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MAX 1024

#define SPDK_NVMF_CONFIG_QUEUE_DEPTH_DEFAULT 128
#define SPDK_NVMF_CONFIG_QUEUE_DEPTH_MIN 16
#define SPDK_NVMF_CONFIG_QUEUE_DEPTH_MAX 1024

#define SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_DEFAULT 4096
#define SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MIN 4096
#define SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MAX 131072

#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_DEFAULT 131072
#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_MIN 4096
#define SPDK_NVMF_CONFIG_MAX_IO_SIZE_MAX 131072

struct spdk_nvmf_tgt_conf g_spdk_nvmf_tgt_conf;
static int32_t g_last_core = -1;

static int
spdk_get_numa_node_value(const char *path)
{
	FILE *fd;
	int numa_node = -1;
	char buf[MAX_STRING_LEN];

	fd = fopen(path, "r");
	if (!fd) {
		return -1;
	}

	if (fgets(buf, sizeof(buf), fd) != NULL) {
		numa_node = strtoul(buf, NULL, 10);
	}
	fclose(fd);

	return numa_node;
}

static int
spdk_get_ifaddr_numa_node(const char *if_addr)
{
	int ret;
	struct ifaddrs *ifaddrs, *ifa;
	struct sockaddr_in addr, addr_in;
	char path[MAX_STRING_LEN];
	int numa_node = -1;

	addr_in.sin_addr.s_addr = inet_addr(if_addr);

	ret = getifaddrs(&ifaddrs);
	if (ret < 0)
		return -1;

	for (ifa = ifaddrs; ifa != NULL; ifa = ifa->ifa_next) {
		addr = *(struct sockaddr_in *)ifa->ifa_addr;
		if ((uint32_t)addr_in.sin_addr.s_addr != (uint32_t)addr.sin_addr.s_addr) {
			continue;
		}
		snprintf(path, MAX_STRING_LEN, "/sys/class/net/%s/device/numa_node", ifa->ifa_name);
		numa_node = spdk_get_numa_node_value(path);
		break;
	}
	freeifaddrs(ifaddrs);

	return numa_node;
}

static int
spdk_add_nvmf_discovery_subsystem(void)
{
	struct nvmf_tgt_subsystem *app_subsys;

	app_subsys = nvmf_tgt_create_subsystem(SPDK_NVMF_DISCOVERY_NQN, SPDK_NVMF_SUBTYPE_DISCOVERY,
					       NVMF_SUBSYSTEM_MODE_DIRECT,
					       spdk_get_master_lcore());
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		return -1;
	}

	nvmf_tgt_start_subsystem(app_subsys);

	return 0;
}

static int
spdk_nvmf_parse_nvmf_tgt(void)
{
	struct spdk_conf_section *sp;
	int max_queue_depth;
	int max_queues_per_sess;
	int in_capsule_data_size;
	int max_io_size;
	int acceptor_lcore;
	int acceptor_poll_rate;
	int rc;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp == NULL) {
		SPDK_ERRLOG("No Nvmf section in configuration file.\n");
		return -1;
	}

	max_queue_depth = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (max_queue_depth < 0) {
		max_queue_depth = SPDK_NVMF_CONFIG_QUEUE_DEPTH_DEFAULT;
	}
	max_queue_depth = spdk_max(max_queue_depth, SPDK_NVMF_CONFIG_QUEUE_DEPTH_MIN);
	max_queue_depth = spdk_min(max_queue_depth, SPDK_NVMF_CONFIG_QUEUE_DEPTH_MAX);

	max_queues_per_sess = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (max_queues_per_sess < 0) {
		max_queues_per_sess = SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_DEFAULT;
	}
	max_queues_per_sess = spdk_max(max_queues_per_sess, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MIN);
	max_queues_per_sess = spdk_min(max_queues_per_sess, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MAX);

	in_capsule_data_size = spdk_conf_section_get_intval(sp, "InCapsuleDataSize");
	if (in_capsule_data_size < 0) {
		in_capsule_data_size = SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_DEFAULT;
	} else if ((in_capsule_data_size % 16) != 0) {
		SPDK_ERRLOG("InCapsuleDataSize must be a multiple of 16\n");
		return -1;
	}
	in_capsule_data_size = spdk_max(in_capsule_data_size, SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MIN);
	in_capsule_data_size = spdk_min(in_capsule_data_size, SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MAX);

	max_io_size = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (max_io_size < 0) {
		max_io_size = SPDK_NVMF_CONFIG_MAX_IO_SIZE_DEFAULT;
	} else if ((max_io_size % 4096) != 0) {
		SPDK_ERRLOG("MaxIOSize must be a multiple of 4096\n");
		return -1;
	}
	max_io_size = spdk_max(max_io_size, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MIN);
	max_io_size = spdk_min(max_io_size, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MAX);

	acceptor_lcore = spdk_conf_section_get_intval(sp, "AcceptorCore");
	if (acceptor_lcore < 0) {
		acceptor_lcore = spdk_lcore_id();
	}
	g_spdk_nvmf_tgt_conf.acceptor_lcore = acceptor_lcore;

	acceptor_poll_rate = spdk_conf_section_get_intval(sp, "AcceptorPollRate");
	if (acceptor_poll_rate < 0) {
		acceptor_poll_rate = ACCEPT_TIMEOUT_US;
	}
	g_spdk_nvmf_tgt_conf.acceptor_poll_rate = acceptor_poll_rate;

	rc = spdk_nvmf_tgt_init(max_queue_depth, max_queues_per_sess, in_capsule_data_size, max_io_size);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_nvmf_tgt_init() failed\n");
		return rc;
	}

	rc = spdk_add_nvmf_discovery_subsystem();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_add_nvmf_discovery_subsystem failed\n");
		return rc;
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvmf_probe_ctx *ctx = cb_ctx;

	if (ctx->any && !ctx->found) {
		ctx->found = true;
		return true;
	}

	if (strcmp(trid->traddr, ctx->trid.traddr) == 0) {
		ctx->found = true;
		return true;
	}

	return false;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvmf_probe_ctx *ctx = cb_ctx;
	int rc;
	int numa_node = -1;
	struct spdk_pci_addr pci_addr;
	struct spdk_pci_device *pci_dev;

	spdk_pci_addr_parse(&pci_addr, trid->traddr);

	SPDK_NOTICELOG("Attaching NVMe device %p at %s to subsystem %s\n",
		       ctrlr,
		       trid->traddr,
		       spdk_nvmf_subsystem_get_nqn(ctx->app_subsystem->subsystem));

	pci_dev = spdk_pci_get_device(&pci_addr);
	if (pci_dev) {
		numa_node = spdk_pci_device_get_socket_id(pci_dev);
	}
	if (numa_node >= 0) {
		/* Running subsystem and NVMe device is on the same socket or not */
		if (spdk_env_get_socket_id(ctx->app_subsystem->lcore) != (unsigned)numa_node) {
			SPDK_WARNLOG("Subsystem %s is configured to run on a CPU core %u belonging "
				     "to a different NUMA node than the associated NVMe device. "
				     "This may result in reduced performance.\n",
				     spdk_nvmf_subsystem_get_nqn(ctx->app_subsystem->subsystem),
				     ctx->app_subsystem->lcore);
			SPDK_WARNLOG("The NVMe device is on socket %u\n", numa_node);
			SPDK_WARNLOG("The Subsystem is on socket %u\n",
				     spdk_env_get_socket_id(ctx->app_subsystem->lcore));
		}
	}

	rc = nvmf_subsystem_add_ctrlr(ctx->app_subsystem->subsystem, ctrlr, &pci_addr);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to add controller to subsystem\n");
	}
}

static int
spdk_nvmf_validate_sn(const char *sn)
{
	size_t len;

	len = strlen(sn);
	if (len > MAX_SN_LEN) {
		SPDK_ERRLOG("Invalid sn \"%s\": length %zu > max %d\n", sn, len, MAX_SN_LEN);
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_allocate_lcore(uint64_t mask, uint32_t lcore)
{
	uint32_t end;

	if (lcore == 0) {
		end = 0;
	} else {
		end = lcore - 1;
	}

	do {
		if (((mask >> lcore) & 1U) == 1U) {
			break;
		}
		lcore = (lcore + 1) % 64;
	} while (lcore != end);

	return lcore;
}

static int
spdk_nvmf_parse_subsystem(struct spdk_conf_section *sp)
{
	const char *nqn, *mode_str;
	int i, ret;
	int lcore;
	int num_listen_addrs;
	struct rpc_listen_address listen_addrs[MAX_LISTEN_ADDRESSES];
	char *listen_addrs_str[MAX_LISTEN_ADDRESSES] = {};
	int num_hosts;
	char *hosts[MAX_HOSTS];
	const char *bdf;
	const char *sn;
	int num_devs;
	char *devs[MAX_VIRTUAL_NAMESPACE];

	nqn = spdk_conf_section_get_val(sp, "NQN");
	mode_str = spdk_conf_section_get_val(sp, "Mode");
	lcore = spdk_conf_section_get_intval(sp, "Core");

	/* Parse Listen sections */
	num_listen_addrs = 0;
	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		listen_addrs[num_listen_addrs].transport =
			spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		if (!listen_addrs[num_listen_addrs].transport) {
			break;
		}

		listen_addrs_str[i] = spdk_conf_section_get_nmval(sp, "Listen", i, 1);
		if (!listen_addrs_str[i]) {
			break;
		}

		listen_addrs_str[i] = strdup(listen_addrs_str[i]);

		ret = spdk_parse_ip_addr(listen_addrs_str[i], &listen_addrs[num_listen_addrs].traddr,
					 &listen_addrs[num_listen_addrs].trsvcid);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse listen address '%s'\n", listen_addrs_str[i]);
			free(listen_addrs_str[i]);
			listen_addrs_str[i] = NULL;
			continue;
		}

		num_listen_addrs++;
	}

	/* Parse Host sections */
	for (i = 0; i < MAX_HOSTS; i++) {
		hosts[i] = spdk_conf_section_get_nval(sp, "Host", i);
		if (!hosts[i]) {
			break;
		}
	}
	num_hosts = i;

	bdf = spdk_conf_section_get_val(sp, "NVMe");
	sn = spdk_conf_section_get_val(sp, "SN");

	num_devs = 0;
	for (i = 0; i < MAX_VIRTUAL_NAMESPACE; i++) {
		devs[i] = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
		if (!devs[i]) {
			break;
		}

		num_devs++;
	}

	ret = spdk_nvmf_construct_subsystem(nqn, mode_str, lcore,
					    num_listen_addrs, listen_addrs,
					    num_hosts, hosts,
					    bdf, sn,
					    num_devs, devs);

	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		free(listen_addrs_str[i]);
	}

	return ret;
}

static int
spdk_nvmf_parse_subsystems(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Subsystem")) {
			rc = spdk_nvmf_parse_subsystem(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

int
spdk_nvmf_parse_conf(void)
{
	int rc;

	/* NVMf section */
	rc = spdk_nvmf_parse_nvmf_tgt();
	if (rc < 0) {
		return rc;
	}

	/* Subsystem sections */
	rc = spdk_nvmf_parse_subsystems();
	if (rc < 0) {
		return rc;
	}

	return 0;
}

int
spdk_nvmf_construct_subsystem(const char *name,
			      const char *mode_str, int32_t lcore,
			      int num_listen_addresses, struct rpc_listen_address *addresses,
			      int num_hosts, char *hosts[], const char *bdf,
			      const char *sn, int num_devs, char *dev_list[])
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_tgt_subsystem *app_subsys;
	struct spdk_nvmf_listen_addr *listen_addr;
	enum spdk_nvmf_subsystem_mode mode;
	int i;
	uint64_t mask;

	if (name == NULL) {
		SPDK_ERRLOG("No NQN specified for subsystem\n");
		return -1;
	}

	if (num_listen_addresses > MAX_LISTEN_ADDRESSES) {
		SPDK_ERRLOG("invalid listen adresses number\n");
		return -1;
	}

	if (num_hosts > MAX_HOSTS) {
		SPDK_ERRLOG("invalid hosts number\n");
		return -1;
	}

	if (lcore < 0) {
		lcore = ++g_last_core;
	}

	/* Determine which core to assign to the subsystem */
	mask = spdk_app_get_core_mask();
	lcore = spdk_nvmf_allocate_lcore(mask, lcore);
	g_last_core = lcore;

	/* Determine the mode the subsysem will operate in */
	if (mode_str == NULL) {
		SPDK_ERRLOG("No Mode specified for Subsystem %s\n", name);
		return -1;
	}

	if (strcasecmp(mode_str, "Direct") == 0) {
		mode = NVMF_SUBSYSTEM_MODE_DIRECT;
	} else if (strcasecmp(mode_str, "Virtual") == 0) {
		mode = NVMF_SUBSYSTEM_MODE_VIRTUAL;
	} else {
		SPDK_ERRLOG("Invalid Subsystem mode: %s\n", mode_str);
		return -1;
	}

	app_subsys = nvmf_tgt_create_subsystem(name, SPDK_NVMF_SUBTYPE_NVME,
					       mode, lcore);
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Subsystem creation failed\n");
		return -1;
	}
	subsystem = app_subsys->subsystem;

	/* Parse Listen sections */
	for (i = 0; i < num_listen_addresses; i++) {
		int nic_numa_node = spdk_get_ifaddr_numa_node(addresses[i].traddr);
		unsigned subsys_numa_node = spdk_env_get_socket_id(app_subsys->lcore);

		if (nic_numa_node >= 0) {
			if (subsys_numa_node != (unsigned)nic_numa_node) {
				SPDK_WARNLOG("Subsystem %s is configured to run on a CPU core %d belonging "
					     "to a different NUMA node than the associated NIC. "
					     "This may result in reduced performance.\n",
					     name, lcore);
				SPDK_WARNLOG("The NIC is on socket %d\n", nic_numa_node);
				SPDK_WARNLOG("The Subsystem is on socket %u\n",
					     subsys_numa_node);
			}
		}

		listen_addr = spdk_nvmf_tgt_listen(addresses[i].transport,
						   addresses[i].traddr, addresses[i].trsvcid);
		if (listen_addr == NULL) {
			SPDK_ERRLOG("Failed to listen on transport %s, traddr %s, trsvcid %s\n",
				    addresses[i].transport,
				    addresses[i].traddr,
				    addresses[i].trsvcid);
			goto error;
		}
		spdk_nvmf_subsystem_add_listener(subsystem, listen_addr);
	}

	/* Parse Host sections */
	for (i = 0; i < num_hosts; i++) {
		spdk_nvmf_subsystem_add_host(subsystem, hosts[i]);
	}

	if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
		struct spdk_nvmf_probe_ctx ctx = { 0 };
		struct spdk_nvme_transport_id trid = {};
		struct spdk_pci_addr pci_addr = {};

		if (bdf == NULL) {
			SPDK_ERRLOG("Subsystem %s: missing NVMe directive\n", name);
			goto error;
		}

		if (num_devs != 0) {
			SPDK_ERRLOG("Subsystem %s: Namespaces not allowed for Direct mode\n", name);
			goto error;
		}

		trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
		ctx.app_subsystem = app_subsys;
		ctx.found = false;
		if (strcmp(bdf, "*") == 0) {
			ctx.any = true;
		} else {
			if (spdk_pci_addr_parse(&pci_addr, bdf) < 0) {
				SPDK_ERRLOG("Invalid format for NVMe BDF: %s\n", bdf);
				goto error;
			}
			ctx.any = false;
			spdk_pci_addr_fmt(trid.traddr, sizeof(trid.traddr), &pci_addr);
			ctx.trid = trid;
		}

		if (spdk_nvme_probe(&trid, &ctx, probe_cb, attach_cb, NULL)) {
			SPDK_ERRLOG("One or more controllers failed in spdk_nvme_probe()\n");
		}

		if (!ctx.found) {
			SPDK_ERRLOG("Could not find NVMe controller at PCI address %04x:%02x:%02x.%x\n",
				    pci_addr.domain, pci_addr.bus, pci_addr.dev, pci_addr.func);
			goto error;
		}
	} else {
		struct spdk_bdev *bdev;
		const char *namespace;

		if (sn == NULL) {
			SPDK_ERRLOG("Subsystem %s: missing serial number\n", name);
			goto error;
		}
		if (spdk_nvmf_validate_sn(sn) != 0) {
			goto error;
		}

		if (num_devs > MAX_VIRTUAL_NAMESPACE) {
			goto error;
		}

		subsystem->dev.virt.ns_count = 0;
		snprintf(subsystem->dev.virt.sn, MAX_SN_LEN, "%s", sn);

		for (i = 0; i < num_devs; i++) {
			namespace = dev_list[i];
			if (!namespace) {
				SPDK_ERRLOG("Namespace %d: missing block device\n", i);
				goto error;
			}
			bdev = spdk_bdev_get_by_name(namespace);
			if (bdev == NULL) {
				SPDK_ERRLOG("Could not find namespace bdev '%s'\n", namespace);
				goto error;
			}
			if (spdk_nvmf_subsystem_add_ns(subsystem, bdev)) {
				goto error;
			}

			SPDK_NOTICELOG("Attaching block device %s to subsystem %s\n",
				       bdev->name, subsystem->subnqn);

		}
	}

	nvmf_tgt_start_subsystem(app_subsys);

	return 0;

error:
	spdk_nvmf_delete_subsystem(app_subsys->subsystem);
	app_subsys->subsystem = NULL;
	return -1;
}
