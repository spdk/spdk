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

#include <rte_config.h>
#include <rte_lcore.h>

#include "conf.h"
#include "controller.h"
#include "nvmf_internal.h"
#include "subsystem.h"
#include "transport.h"
#include "spdk/conf.h"
#include "spdk/log.h"

#define MAX_LISTEN_ADDRESSES 255
#define MAX_HOSTS 255

#define PORTNUMSTRLEN 32

static int
spdk_nvmf_parse_nvmf_tgt(void)
{
	struct spdk_conf_section *sp;
	int max_queue_depth;
	int max_conn_per_sess;
	int rc;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp == NULL) {
		SPDK_ERRLOG("No Nvmf section in configuration file.\n");
		return -1;
	}

	max_queue_depth = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (max_queue_depth < 0) {
		max_queue_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	}

	max_conn_per_sess = spdk_conf_section_get_intval(sp, "MaxConnectionsPerSession");
	if (max_conn_per_sess < 0) {
		max_conn_per_sess = SPDK_NVMF_DEFAULT_MAX_CONNECTIONS_PER_SESSION;
	}

	rc = nvmf_tgt_init(max_queue_depth, max_conn_per_sess);
	return rc;
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

static int
spdk_nvmf_parse_nvme(void)
{
	struct spdk_conf_section *sp;
	struct nvme_bdf_whitelist *whitelist = NULL;
	const char *val;
	bool claim_all = false;
	bool unbind_from_kernel = false;
	int i = 0;
	int rc;

	sp = spdk_conf_find_section(NULL, "Nvme");
	if (sp == NULL) {
		SPDK_ERRLOG("NVMe device section in config file not found!\n");
		return -1;
	}

	val = spdk_conf_section_get_val(sp, "ClaimAllDevices");
	if (val != NULL) {
		if (!strcmp(val, "Yes")) {
			claim_all = true;
		}
	}

	val = spdk_conf_section_get_val(sp, "UnbindFromKernel");
	if (val != NULL) {
		if (!strcmp(val, "Yes")) {
			unbind_from_kernel = true;
		}
	}

	if (!claim_all) {
		for (i = 0; ; i++) {
			unsigned int domain, bus, dev, func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 0);
			if (val == NULL) {
				break;
			}

			whitelist = realloc(whitelist, sizeof(*whitelist) * (i + 1));

			rc = sscanf(val, "%x:%x:%x.%x", &domain, &bus, &dev, &func);
			if (rc != 4) {
				SPDK_ERRLOG("Invalid format for BDF: %s\n", val);
				free(whitelist);
				return -1;
			}

			whitelist[i].domain = domain;
			whitelist[i].bus = bus;
			whitelist[i].dev = dev;
			whitelist[i].func = func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 1);
			if (val == NULL) {
				SPDK_ERRLOG("BDF section with no device name\n");
				free(whitelist);
				return -1;
			}

			snprintf(whitelist[i].name, MAX_NVME_NAME_LENGTH, "%s", val);
		}

		if (i == 0) {
			SPDK_ERRLOG("No BDF section\n");
			return -1;
		}
	}

	rc = spdk_nvmf_init_nvme(whitelist, i,
				 claim_all, unbind_from_kernel);

	free(whitelist);

	return rc;
}

static int
spdk_nvmf_validate_nqn(const char *nqn)
{
	size_t len;

	len = strlen(nqn);
	if (len > SPDK_NVMF_NQN_MAX_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu > max %d\n", nqn, len, SPDK_NVMF_NQN_MAX_LEN);
		return -1;
	}

	if (strncasecmp(nqn, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", nqn);
		return -1;
	}

	/* yyyy-mm. */
	if (!(isdigit(nqn[4]) && isdigit(nqn[5]) && isdigit(nqn[6]) && isdigit(nqn[7]) &&
	      nqn[8] == '-' && isdigit(nqn[9]) && isdigit(nqn[10]) && nqn[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", nqn);
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
	const char *val, *nqn;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_ctrlr *nvmf_ctrlr;
	int i, ret;
	uint64_t mask;
	uint32_t lcore;

	nqn = spdk_conf_section_get_val(sp, "NQN");
	if (nqn == NULL) {
		SPDK_ERRLOG("No NQN specified for Subsystem %d\n", sp->num);
		return -1;
	}

	if (spdk_nvmf_validate_nqn(nqn) != 0) {
		return -1;
	}

	/* Determine which core to assign to the subsystem using round robin */
	mask = spdk_app_get_core_mask();
	lcore = 0;
	for (i = 0; i < sp->num; i++) {
		lcore = spdk_nvmf_allocate_lcore(mask, lcore);
		lcore++;
	}
	lcore = spdk_nvmf_allocate_lcore(mask, lcore);

	subsystem = nvmf_create_subsystem(sp->num, nqn, SPDK_NVMF_SUB_NVME, lcore);
	if (subsystem == NULL) {
		return -1;
	}

	/* Parse Listen sections */
	for (i = 0; i < MAX_LISTEN_ADDRESSES; i++) {
		char *transport_name, *listen_addr;
		char *traddr, *trsvc;
		const struct spdk_nvmf_transport *transport;

		transport_name = spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		listen_addr = spdk_conf_section_get_nmval(sp, "Listen", i, 1);

		if (!transport_name || !listen_addr) {
			break;
		}

		transport = spdk_nvmf_transport_get(transport_name);
		if (transport == NULL) {
			SPDK_ERRLOG("Unknown transport type '%s'\n", transport_name);
			continue;
		}

		ret = spdk_nvmf_parse_addr(listen_addr, &traddr, &trsvc);
		if (ret < 0) {
			SPDK_ERRLOG("Unable to parse transport address '%s'\n", listen_addr);
			continue;
		}

		spdk_nvmf_subsystem_add_listener(subsystem, transport, traddr, trsvc);
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

	val = spdk_conf_section_get_val(sp, "Controller");
	if (val == NULL) {
		SPDK_ERRLOG("Subsystem %d: missing Controller\n", sp->num);
		nvmf_delete_subsystem(subsystem);
		return -1;
	}

	/* claim this controller from the available controller list */
	nvmf_ctrlr = spdk_nvmf_ctrlr_claim(val);
	if (nvmf_ctrlr == NULL) {
		SPDK_ERRLOG("Subsystem %d: NVMe controller %s not found\n", sp->num, val);
		nvmf_delete_subsystem(subsystem);
		return -1;
	}

	ret = nvmf_subsystem_add_ctrlr(subsystem, nvmf_ctrlr->ctrlr);
	if (ret < 0) {
		SPDK_ERRLOG("Subsystem %d: adding controller %s failed\n", sp->num, val);
		nvmf_delete_subsystem(subsystem);
		return -1;
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "    NVMf Subsystem: Nvme Controller: %s , %p\n",
		      nvmf_ctrlr->name, nvmf_ctrlr->ctrlr);

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

	/* NVMe sections */
	rc = spdk_nvmf_parse_nvme();
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
