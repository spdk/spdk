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

#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "init_grp.h"
#include "nvmf_internal.h"
#include "port.h"
#include "spdk/conf.h"
#include "spdk/log.h"

#define PORTNUMSTRLEN 32

static int
spdk_nvmf_parse_nvmf_tgt(void)
{
	struct spdk_conf_section *sp;
	char *nodebase;
	int max_in_capsule_data;
	int max_sessions_per_subsystem;
	int max_queue_depth;
	int max_conn_per_sess;
	int max_recv_seg_len;
	int listen_port;
	int rc;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp == NULL) {
		SPDK_ERRLOG("No Nvmf section in configuration file.\n");
		return -1;
	}

	nodebase = spdk_conf_section_get_val(sp, "NodeBase");
	if (nodebase == NULL) {
		nodebase = SPDK_NVMF_DEFAULT_NODEBASE;
	}

	max_in_capsule_data = spdk_conf_section_get_intval(sp, "MaxInCapsuleData");
	if (max_in_capsule_data < 0) {
		max_in_capsule_data = SPDK_NVMF_DEFAULT_IN_CAPSULE_DATA_SIZE;
	}

	max_sessions_per_subsystem = spdk_conf_section_get_intval(sp, "MaxSessionsPerSubsystem");
	if (max_sessions_per_subsystem < 0) {
		max_sessions_per_subsystem = SPDK_NVMF_DEFAULT_MAX_SESSIONS_PER_SUBSYSTEM;
	}

	max_queue_depth = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (max_queue_depth < 0) {
		max_queue_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	}

	max_conn_per_sess = spdk_conf_section_get_intval(sp, "MaxConnectionsPerSession");
	if (max_conn_per_sess < 0) {
		max_conn_per_sess = SPDK_NVMF_DEFAULT_MAX_CONNECTIONS_PER_SESSION;
	}

	max_recv_seg_len = SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE;
	listen_port = SPDK_NVMF_DEFAULT_SIN_PORT;

	rc = nvmf_tgt_init(nodebase, max_in_capsule_data, max_sessions_per_subsystem,
			   max_queue_depth, max_conn_per_sess, max_recv_seg_len, listen_port);

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
		*host = malloc(n + 1);
		if (!*host) {
			return -1;
		}
		memcpy(*host, listen_addr, n);
		(*host)[n] = '\0';
		if (p[0] == '\0') {
			*port = malloc(PORTNUMSTRLEN);
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

			*port = malloc(len + 1);
			if (!*port) {
				free(*host);
				return -1;
			}
			memset(*port, 0, len + 1);
			memcpy(*port, p + 1, len);
		}
	} else {
		/* IPv4 */
		p = strchr(listen_addr, ':');
		if (p == NULL) {
			p = listen_addr + strlen(listen_addr);
		}
		n = p - listen_addr;
		*host = malloc(n + 1);
		if (!*host) {
			return -1;
		}
		memcpy(*host, listen_addr, n);
		(*host)[n] = '\0';
		if (p[0] == '\0') {
			*port = malloc(PORTNUMSTRLEN);
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
			*port = malloc(len + 1);
			if (!*port) {
				free(*host);
				return -1;
			}
			memset(*port, 0, len + 1);
			memcpy(*port, p + 1, len);

		}
	}

	return 0;
}

static int
spdk_nvmf_parse_port(struct spdk_conf_section *sp)
{
	struct spdk_nvmf_port		*port;
	struct spdk_nvmf_fabric_intf	*fabric_intf;
	char *listen_addr, *host, *listen_port;
	int i = 0, rc = 0;

	/* Create the Subsystem Port */
	port = spdk_nvmf_port_create(sp->num);
	if (!port) {
		SPDK_ERRLOG("Port create failed\n");
		return -1;
	}

	/* Loop over the fabric interfaces and add them to the port */
	for (i = 0; ; i++) {
		listen_addr = spdk_conf_section_get_nmval(sp, "FabricIntf", i, 0);
		if (listen_addr == NULL) {
			break;
		}
		rc = spdk_nvmf_parse_addr(listen_addr, &host, &listen_port);
		if (rc < 0) {
			continue;
		}
		fabric_intf = spdk_nvmf_fabric_intf_create(host, listen_port);
		if (!fabric_intf) {
			continue;
		}

		spdk_nvmf_port_add_fabric_intf(port, fabric_intf);
	}

	if (TAILQ_EMPTY(&port->head)) {
		SPDK_ERRLOG("No fabric interface found\n");
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_parse_ports(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Port")) {
			rc = spdk_nvmf_parse_port(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

static int
spdk_nvmf_parse_init_grp(struct spdk_conf_section *sp)
{
	int i;
	const char *mask;
	char **netmasks;
	int num_netmasks;
	struct spdk_nvmf_init_grp *init_grp;


	for (num_netmasks = 0; ; num_netmasks++) {
		mask = spdk_conf_section_get_nval(sp, "Netmask", num_netmasks);
		if (mask == NULL) {
			break;
		}
	}

	if (num_netmasks == 0) {
		return -1;
	}


	netmasks = calloc(num_netmasks, sizeof(char *));
	if (!netmasks) {
		return -1;
	}

	for (i = 0; i < num_netmasks; i++) {
		mask = spdk_conf_section_get_nval(sp, "Netmask", i);
		netmasks[i] = strdup(mask);
		if (!netmasks[i]) {
			free(netmasks);
			return -1;
		}
	}

	init_grp = spdk_nvmf_init_grp_create(sp->num, num_netmasks, netmasks);

	if (!init_grp) {
		free(netmasks);
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_parse_init_grps(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Host")) {
			rc = spdk_nvmf_parse_init_grp(sp);
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

	/* Port sections */
	rc = spdk_nvmf_parse_ports();
	if (rc < 0) {
		return rc;
	}

	/* Initiator Group sections */
	rc = spdk_nvmf_parse_init_grps();
	if (rc < 0) {
		return rc;
	}

	return 0;
}
