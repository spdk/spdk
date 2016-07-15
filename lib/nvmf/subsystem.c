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

#include "controller.h"
#include "port.h"
#include "host.h"
#include "nvmf_internal.h"
#include "session.h"
#include "subsystem.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

static TAILQ_HEAD(, spdk_nvmf_subsystem) g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);

struct spdk_nvmf_subsystem *
nvmf_find_subsystem(const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subs;

	if (subnqn == NULL)
		return NULL;

	TAILQ_FOREACH(subs, &g_subsystems, entries) {
		if (strcasecmp(subnqn, subs->subnqn) == 0) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "found subsystem group with name: %s\n",
				      subnqn);
			return subs;
		}
	}

	fprintf(stderr, "can't find subsystem %s\n", subnqn);
	return NULL;
}

struct spdk_nvmf_subsystem *
nvmf_create_subsystem(int num, const char *name, enum spdk_nvmf_subsystem_types sub_type)
{
	struct spdk_nvmf_subsystem	*subsystem;

	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));
	if (subsystem == NULL) {
		return NULL;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_create_subsystem: allocated subsystem %p\n", subsystem);

	subsystem->num = num;
	subsystem->subtype = sub_type;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", name);

	TAILQ_INSERT_HEAD(&g_subsystems, subsystem, entries);

	return subsystem;
}

int
nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem)
{
	int i;

	if (subsystem == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF,
			      "nvmf_delete_subsystem: there is no subsystem\n");
		return 0;
	}

	for (i = 0; i < subsystem->map_count; i++) {
		subsystem->map[i].host->ref--;
	}

	if (subsystem->session) {
		spdk_nvmf_session_destruct(subsystem->session);
	}

	TAILQ_REMOVE(&g_subsystems, subsystem, entries);

	free(subsystem);
	return 0;
}

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_ctrlr *ctrlr)
{
	subsystem->ctrlr = ctrlr;

	/* Assume that all I/O will be handled on one thread for now */
	subsystem->io_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);
	if (subsystem->io_qpair == NULL) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -1;
	}

	return 0;
}

int
spdk_nvmf_subsystem_add_map(struct spdk_nvmf_subsystem *subsystem,
			    int port_tag, int host_tag)
{
	struct spdk_nvmf_access_map	*map;
	struct spdk_nvmf_port		*port;
	struct spdk_nvmf_host		*host;

	port = spdk_nvmf_port_find_by_tag(port_tag);
	if (port == NULL) {
		SPDK_ERRLOG("%s: Port%d not found\n", subsystem->subnqn, port_tag);
		return -1;
	}
	if (port->state != GROUP_READY) {
		SPDK_ERRLOG("%s: Port%d not active\n", subsystem->subnqn, port_tag);
		return -1;
	}
	host = spdk_nvmf_host_find_by_tag(host_tag);
	if (host == NULL) {
		SPDK_ERRLOG("%s: Host%d not found\n", subsystem->subnqn, host_tag);
		return -1;
	}
	if (host->state != GROUP_READY) {
		SPDK_ERRLOG("%s: Host%d not active\n", subsystem->subnqn, host_tag);
		return -1;
	}
	host->ref++;
	map = &subsystem->map[subsystem->map_count];
	map->port = port;
	map->host = host;
	subsystem->map_count++;

	return 0;
}

int
spdk_add_nvmf_discovery_subsystem(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	char *name;

	name = strdup(SPDK_NVMF_DISCOVERY_NQN);
	if (name == NULL) {
		SPDK_ERRLOG("strdup ss_group->name error\n");
		return -1;
	}

	subsystem = nvmf_create_subsystem(0, name, SPDK_NVMF_SUB_DISCOVERY);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Failed creating discovery nvmf library subsystem\n");
		free(name);
		return -1;
	}

	free(name);

	return 0;
}

void
spdk_format_discovery_log(struct spdk_nvmf_discovery_log_page *disc_log, uint32_t length)
{
	int i, numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_access_map *map;
	struct spdk_nvmf_port *port;
	struct spdk_nvmf_fabric_intf *fabric_intf;
	struct spdk_nvmf_discovery_log_page_entry *entry;

	TAILQ_FOREACH(subsystem, &g_subsystems, entries) {
		if (subsystem->subtype == SPDK_NVMF_SUB_DISCOVERY) {
			continue;
		}

		for (i = 0; i < subsystem->map_count; i++) {
			map = &subsystem->map[i];
			port = map->port;
			if (port != NULL) {
				TAILQ_FOREACH(fabric_intf, &port->head, tailq) {
					/* include the discovery log entry */
					if (length > sizeof(struct spdk_nvmf_discovery_log_page)) {
						if (sizeof(struct spdk_nvmf_discovery_log_page) + (numrec + 1) * sizeof(
							    struct spdk_nvmf_discovery_log_page_entry) > length) {
							break;
						}
						entry = &disc_log->entries[numrec];
						entry->trtype = fabric_intf->trtype;
						entry->adrfam = fabric_intf->adrfam;
						entry->treq = fabric_intf->treq;
						entry->portid = port->tag;
						/* Dynamic controllers */
						entry->cntlid = 0xffff;
						entry->subtype = subsystem->subtype;
						snprintf(entry->trsvcid, 32, "%s", fabric_intf->sin_port);
						snprintf(entry->traddr, 256, "%s", fabric_intf->host);
						snprintf(entry->subnqn, 256, "%s", subsystem->subnqn);
						entry->tsas.rdma.rdma_qptype = port->rdma.rdma_qptype;
						entry->tsas.rdma.rdma_prtype = port->rdma.rdma_prtype;
						entry->tsas.rdma.rdma_cms = port->rdma.rdma_cms;
					}
					numrec++;
				}
			}
		}
	}

	disc_log->numrec = numrec;
}

int
spdk_shutdown_nvmf_subsystems(void)
{
	struct spdk_nvmf_subsystem *subsystem;

	while (!TAILQ_EMPTY(&g_subsystems)) {
		subsystem = TAILQ_FIRST(&g_subsystems);
		TAILQ_REMOVE(&g_subsystems, subsystem, entries);
		nvmf_delete_subsystem(subsystem);
	}

	return 0;
}
