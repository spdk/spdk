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

#include <rte_config.h>
#include <rte_lcore.h>

#include "nvmf_tgt.h"

#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"

#define MAX_LISTEN_ADDRESSES 255
#define MAX_HOSTS 255
#define PORTNUMSTRLEN 32
#define SPDK_NVMF_DEFAULT_SIN_PORT ((uint16_t)4420)

#define ACCEPT_TIMEOUT_US		10000 /* 10ms */

struct spdk_nvmf_probe_ctx {
	struct nvmf_tgt_subsystem	*app_subsystem;
	bool				any;
	bool				found;
	struct spdk_pci_addr		pci_addr;
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

static int
spdk_get_numa_node_value(char *path)
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
spdk_get_ifaddr_numa_node(char *if_addr)
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
					       rte_get_master_lcore());
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
	max_queue_depth = nvmf_max(max_queue_depth, SPDK_NVMF_CONFIG_QUEUE_DEPTH_MIN);
	max_queue_depth = nvmf_min(max_queue_depth, SPDK_NVMF_CONFIG_QUEUE_DEPTH_MAX);

	max_queues_per_sess = spdk_conf_section_get_intval(sp, "MaxQueuesPerSession");
	if (max_queues_per_sess < 0) {
		max_queues_per_sess = SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_DEFAULT;
	}
	max_queues_per_sess = nvmf_max(max_queues_per_sess, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MIN);
	max_queues_per_sess = nvmf_min(max_queues_per_sess, SPDK_NVMF_CONFIG_QUEUES_PER_SESSION_MAX);

	in_capsule_data_size = spdk_conf_section_get_intval(sp, "InCapsuleDataSize");
	if (in_capsule_data_size < 0) {
		in_capsule_data_size = SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_DEFAULT;
	} else if ((in_capsule_data_size % 16) != 0) {
		SPDK_ERRLOG("InCapsuleDataSize must be a multiple of 16\n");
		return -1;
	}
	in_capsule_data_size = nvmf_max(in_capsule_data_size, SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MIN);
	in_capsule_data_size = nvmf_min(in_capsule_data_size, SPDK_NVMF_CONFIG_IN_CAPSULE_DATA_SIZE_MAX);

	max_io_size = spdk_conf_section_get_intval(sp, "MaxIOSize");
	if (max_io_size < 0) {
		max_io_size = SPDK_NVMF_CONFIG_MAX_IO_SIZE_DEFAULT;
	} else if ((max_io_size % 4096) != 0) {
		SPDK_ERRLOG("MaxIOSize must be a multiple of 4096\n");
		return -1;
	}
	max_io_size = nvmf_max(max_io_size, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MIN);
	max_io_size = nvmf_min(max_io_size, SPDK_NVMF_CONFIG_MAX_IO_SIZE_MAX);

	acceptor_lcore = spdk_conf_section_get_intval(sp, "AcceptorCore");
	if (acceptor_lcore < 0) {
		acceptor_lcore = rte_lcore_id();
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

static int
spdk_nvmf_parse_addr(char *listen_addr, char **host, char **port)
{
	int n, len;
	const char *p, *q;

	if (listen_addr == NULL) {
		SPDK_ERRLOG("Invalid listen addr for Fabric Interface (NULL)\n");
		return -1;
	}

	*host = NULL;
	*port = NULL;

	if (listen_addr[0] == '[') {
		/* IPv6 */
		p = strchr(listen_addr + 1, ']');
		if (p == NULL) {
			return -1;
		}
		p++;
		n = p - listen_addr;
		*host = calloc(1, n + 1);
		if (!*host) {
			return -1;
		}
		memcpy(*host, listen_addr, n);
		(*host)[n] = '\0';
		if (p[0] == '\0') {
			*port = calloc(1, PORTNUMSTRLEN);
			if (!*port) {
				free(*host);
				return -1;
			}
			snprintf(*port, PORTNUMSTRLEN, "%d", SPDK_NVMF_DEFAULT_SIN_PORT);
		} else {
			if (p[0] != ':') {
				free(*host);
				return -1;
			}
			q = strchr(listen_addr, '@');
			if (q == NULL) {
				q = listen_addr + strlen(listen_addr);
			}
			len = q - p - 1;

			*port = calloc(1, len + 1);
			if (!*port) {
				free(*host);
				return -1;
			}
			memcpy(*port, p + 1, len);
		}
	} else {
		/* IPv4 */
		p = strchr(listen_addr, ':');
		if (p == NULL) {
			p = listen_addr + strlen(listen_addr);
		}
		n = p - listen_addr;
		*host = calloc(1, n + 1);
		if (!*host) {
			return -1;
		}
		memcpy(*host, listen_addr, n);
		(*host)[n] = '\0';
		if (p[0] == '\0') {
			*port = calloc(1, PORTNUMSTRLEN);
			if (!*port) {
				free(*host);
				return -1;
			}
			snprintf(*port, PORTNUMSTRLEN, "%d", SPDK_NVMF_DEFAULT_SIN_PORT);
		} else {
			if (p[0] != ':') {
				free(*host);
				return -1;
			}
			q = strchr(listen_addr, '@');
			if (q == NULL) {
				q = listen_addr + strlen(listen_addr);
			}

			if (q == p) {
				free(*host);
				return -1;
			}

			len = q - p - 1;
			*port = calloc(1, len + 1);
			if (!*port) {
				free(*host);
				return -1;
			}
			memcpy(*port, p + 1, len);

		}
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvmf_probe_ctx *ctx = cb_ctx;
	struct spdk_pci_addr pci_addr;

	if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
		return false;
	}

	if (ctx->any && !ctx->found) {
		ctx->found = true;
		return true;
	}

	if (spdk_pci_addr_compare(&pci_addr, &ctx->pci_addr) == 0) {
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
	char path[MAX_STRING_LEN];
	int numa_node = -1;
	struct spdk_pci_addr pci_addr;

	spdk_pci_addr_parse(&pci_addr, trid->traddr);

	SPDK_NOTICELOG("Attaching NVMe device %p at %s to subsystem %s\n",
		       ctrlr,
		       trid->traddr,
		       spdk_nvmf_subsystem_get_nqn(ctx->app_subsystem->subsystem));

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/numa_node",
		 trid->traddr);

	numa_node = spdk_get_numa_node_value(path);
	if (numa_node >= 0) {
		/* Running subsystem and NVMe device is on the same socket or not */
		if (rte_lcore_to_socket_id(ctx->app_subsystem->lcore) != (unsigned)numa_node) {
			SPDK_WARNLOG("Subsystem %s is configured to run on a CPU core %u belonging "
				     "to a different NUMA node than the associated NVMe device. "
				     "This may result in reduced performance.\n",
				     spdk_nvmf_subsystem_get_nqn(ctx->app_subsystem->subsystem),
				     ctx->app_subsystem->lcore);
			SPDK_WARNLOG("The NVMe device is on socket %u\n", numa_node);
			SPDK_WARNLOG("The Subsystem is on socket %u\n",
				     rte_lcore_to_socket_id(ctx->app_subsystem->lcore));
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
	struct nvmf_tgt_subsystem *app_subsys;
	struct spdk_nvmf_subsystem *subsystem;
	enum spdk_nvmf_subsystem_mode mode;
	int i, ret;
	uint64_t mask;
	int lcore = 0;
	int num = spdk_conf_section_get_num(sp);

	nqn = spdk_conf_section_get_val(sp, "NQN");
	if (nqn == NULL) {
		SPDK_ERRLOG("No NQN specified for Subsystem %d\n", num);
		return -1;
	}

	/* Determine which core to assign to the subsystem */
	mask = spdk_app_get_core_mask();
	lcore = spdk_conf_section_get_intval(sp, "Core");
	if (lcore < 0) {
		lcore = 0;
		for (i = 0; i < num; i++) {
			lcore = spdk_nvmf_allocate_lcore(mask, lcore);
			lcore++;
		}
	}
	lcore = spdk_nvmf_allocate_lcore(mask, lcore);

	mode_str = spdk_conf_section_get_val(sp, "Mode");
	if (mode_str == NULL) {
		SPDK_ERRLOG("No Mode specified for Subsystem %d\n", num);
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

	app_subsys = nvmf_tgt_create_subsystem(nqn, SPDK_NVMF_SUBTYPE_NVME, mode, lcore);
	if (app_subsys == NULL) {
		SPDK_ERRLOG("Subsystem createion failed\n");
		return -1;
	}
	subsystem = app_subsys->subsystem;

	/* Parse Listen sections */
	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		char *transport_name, *listen_addr;
		char *traddr, *trsvcid;
		int numa_node = -1;

		transport_name = spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		listen_addr = spdk_conf_section_get_nmval(sp, "Listen", i, 1);

		if (!transport_name || !listen_addr) {
			break;
		}

		ret = spdk_nvmf_parse_addr(listen_addr, &traddr, &trsvcid);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse transport address '%s'\n", listen_addr);
			continue;
		}

		numa_node = spdk_get_ifaddr_numa_node(traddr);
		if (numa_node >= 0) {
			if (rte_lcore_to_socket_id(app_subsys->lcore) != (unsigned)numa_node) {
				SPDK_WARNLOG("Subsystem %s is configured to run on a CPU core %u belonging "
					     "to a different NUMA node than the associated NIC. "
					     "This may result in reduced performance.\n",
					     spdk_nvmf_subsystem_get_nqn(app_subsys->subsystem), app_subsys->lcore);
				SPDK_WARNLOG("The NIC is on socket %u\n", numa_node);
				SPDK_WARNLOG("The Subsystem is on socket %u\n",
					     rte_lcore_to_socket_id(app_subsys->lcore));
			}
		}
		spdk_nvmf_subsystem_add_listener(subsystem, transport_name, traddr, trsvcid);

		free(traddr);
		free(trsvcid);
	}

	/* Parse Host sections */
	for (i = 0; i < MAX_HOSTS; i++) {
		char *host_nqn;

		host_nqn = spdk_conf_section_get_nval(sp, "Host", i);
		if (!host_nqn) {
			break;
		}

		spdk_nvmf_subsystem_add_host(subsystem, host_nqn);
	}

	if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
		const char *bdf;
		struct spdk_nvmf_probe_ctx ctx = { 0 };

		/* Parse NVMe section */
		bdf = spdk_conf_section_get_val(sp, "NVMe");
		if (bdf == NULL) {
			SPDK_ERRLOG("Subsystem %d: missing NVMe directive\n", num);
			return -1;
		}

		ctx.app_subsystem = app_subsys;
		ctx.found = false;
		if (strcmp(bdf, "*") == 0) {
			ctx.any = true;
		} else {
			if (spdk_pci_addr_parse(&ctx.pci_addr, bdf) < 0) {
				SPDK_ERRLOG("Invalid format for NVMe BDF: %s\n", bdf);
				return -1;
			}
			ctx.any = false;
		}

		if (spdk_nvme_probe(NULL, &ctx, probe_cb, attach_cb, NULL)) {
			SPDK_ERRLOG("One or more controllers failed in spdk_nvme_probe()\n");
		}

		if (!ctx.found) {
			SPDK_ERRLOG("Could not find NVMe controller for Subsystem%d\n", num);
			return -1;
		}
	} else {
		struct spdk_bdev *bdev;
		const char *namespace, *sn, *val;

		sn = spdk_conf_section_get_val(sp, "SN");
		if (sn == NULL) {
			SPDK_ERRLOG("Subsystem %d: missing serial number\n", num);
			return -1;
		}
		if (spdk_nvmf_validate_sn(sn) != 0) {
			return -1;
		}

		namespace = spdk_conf_section_get_val(sp, "Namespace");
		if (namespace == NULL) {
			SPDK_ERRLOG("Subsystem %d: missing Namespace directive\n", num);
			return -1;
		}

		spdk_nvmf_subsystem_set_sn(subsystem, sn);

		for (i = 0; i < MAX_VIRTUAL_NAMESPACE; i++) {
			val = spdk_conf_section_get_nval(sp, "Namespace", i);
			if (val == NULL) {
				break;
			}
			namespace = spdk_conf_section_get_nmval(sp, "Namespace", i, 0);
			if (!namespace) {
				SPDK_ERRLOG("Namespace %d: missing block device\n", i);
				return -1;
			}

			bdev = spdk_bdev_get_by_name(namespace);
			if (!bdev) {
				SPDK_ERRLOG("bdev is NULL\n");
				return -1;
			}

			if (spdk_nvmf_subsystem_add_ns(subsystem, bdev)) {
				return -1;
			}

			SPDK_NOTICELOG("Attaching block device %s to subsystem %p\n",
				       bdev->name, subsystem);

		}
	}

	nvmf_tgt_start_subsystem(app_subsys);

	return 0;
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
spdk_nvmf_parse_subsystem_for_rpc(const char *name,
				  const char *mode_str, uint32_t lcore,
				  int num_listen_addresses, struct rpc_listen_address *addresses,
				  int num_hosts, char *hosts[], const char *bdf,
				  const char *sn, int num_devs, char *dev_list[])
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_tgt_subsystem *app_subsys;
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

	/* Determine which core to assign to the subsystem */
	mask = spdk_app_get_core_mask();
	lcore = spdk_nvmf_allocate_lcore(mask, lcore);

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
		spdk_nvmf_subsystem_add_listener(subsystem, addresses[i].transport, addresses[i].traddr,
						 addresses[i].trsvcid);
	}

	/* Parse Host sections */
	for (i = 0; i < num_hosts; i++) {
		spdk_nvmf_subsystem_add_host(subsystem, hosts[i]);
	}

	if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
		struct spdk_nvmf_probe_ctx ctx = { 0 };

		if (bdf == NULL) {
			SPDK_ERRLOG("Subsystem %s: missing NVMe directive\n", name);
			goto error;
		}

		if (num_devs != 0) {
			SPDK_ERRLOG("Subsystem %s: Namespaces not allowed for Direct mode\n", name);
			goto error;
		}

		ctx.app_subsystem = app_subsys;
		ctx.found = false;
		if (strcmp(bdf, "*") == 0) {
			ctx.any = true;
		} else {
			if (spdk_pci_addr_parse(&ctx.pci_addr, bdf) < 0) {
				SPDK_ERRLOG("Invalid format for NVMe BDF: %s\n", bdf);
				goto error;
			}
			ctx.any = false;
		}

		if (spdk_nvme_probe(NULL, &ctx, probe_cb, attach_cb, NULL)) {
			SPDK_ERRLOG("One or more controllers failed in spdk_nvme_probe()\n");
		}

		if (!ctx.found) {
			SPDK_ERRLOG("Could not find NVMe controller at PCI address %04x:%02x:%02x.%x\n",
				    ctx.pci_addr.domain, ctx.pci_addr.bus, ctx.pci_addr.dev, ctx.pci_addr.func);
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
