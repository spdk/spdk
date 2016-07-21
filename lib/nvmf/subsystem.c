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

#include "nvmf_internal.h"
#include "session.h"
#include "subsystem.h"
#include "transport.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

static TAILQ_HEAD(, spdk_nvmf_subsystem) g_subsystems = TAILQ_HEAD_INITIALIZER(g_subsystems);

struct spdk_nvmf_subsystem *
nvmf_find_subsystem(const char *subnqn, const char *hostnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;
	struct spdk_nvmf_host		*host;

	if (!subnqn || !hostnqn) {
		return NULL;
	}

	TAILQ_FOREACH(subsystem, &g_subsystems, entries) {
		if (strcasecmp(subnqn, subsystem->subnqn) == 0) {
			if (subsystem->num_hosts == 0) {
				/* No hosts means any host can connect */
				return subsystem;
			}

			TAILQ_FOREACH(host, &subsystem->hosts, link) {
				if (strcasecmp(hostnqn, host->nqn) == 0) {
					return subsystem;
				}
			}
		}
	}

	return NULL;
}

static void
spdk_nvmf_subsystem_poller(void *arg)
{
	struct spdk_nvmf_subsystem *subsystem = arg;
	struct nvmf_session *session = subsystem->session;

	if (!session) {
		/* No active connections, so just return */
		return;
	}

	/* For NVMe subsystems, check the backing physical device for completions. */
	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_nvme_ctrlr_process_admin_completions(subsystem->ctrlr);
		spdk_nvme_qpair_process_completions(subsystem->io_qpair, 0);
	}

	/* For each connection in the session, check for RDMA completions */
	spdk_nvmf_session_poll(session);
}

struct spdk_nvmf_subsystem *
nvmf_create_subsystem(int num, const char *name,
		      enum spdk_nvmf_subtype subtype,
		      uint32_t lcore)
{
	struct spdk_nvmf_subsystem	*subsystem;

	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));
	if (subsystem == NULL) {
		return NULL;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_create_subsystem: allocated subsystem %p\n", subsystem);

	subsystem->num = num;
	subsystem->subtype = subtype;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", name);
	TAILQ_INIT(&subsystem->listen_addrs);
	TAILQ_INIT(&subsystem->hosts);

	subsystem->poller.fn = spdk_nvmf_subsystem_poller;
	subsystem->poller.arg = subsystem;
	spdk_poller_register(&subsystem->poller, lcore, NULL);

	TAILQ_INSERT_HEAD(&g_subsystems, subsystem, entries);

	return subsystem;
}

int
nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_listen_addr	*listen_addr, *listen_addr_tmp;
	struct spdk_nvmf_host		*host, *host_tmp;

	if (subsystem == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF,
			      "nvmf_delete_subsystem: there is no subsystem\n");
		return 0;
	}

	TAILQ_FOREACH_SAFE(listen_addr, &subsystem->listen_addrs, link, listen_addr_tmp) {
		TAILQ_REMOVE(&subsystem->listen_addrs, listen_addr, link);
		free(listen_addr->traddr);
		free(listen_addr->trsvc);
		free(listen_addr);
		subsystem->num_listen_addrs--;
	}

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		TAILQ_REMOVE(&subsystem->hosts, host, link);
		free(host->nqn);
		free(host);
		subsystem->num_hosts--;
	}

	if (subsystem->session) {
		spdk_nvmf_session_destruct(subsystem->session);
	}

	if (subsystem->ctrlr) {
		spdk_nvme_detach(subsystem->ctrlr);
	}

	TAILQ_REMOVE(&g_subsystems, subsystem, entries);

	free(subsystem);
	return 0;
}

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 const struct spdk_nvmf_transport *transport,
				 char *traddr, char *trsvc)
{
	struct spdk_nvmf_listen_addr *listen_addr;

	listen_addr = calloc(1, sizeof(*listen_addr));
	listen_addr->traddr = strdup(traddr);
	listen_addr->trsvc = strdup(trsvc);
	listen_addr->transport = transport;

	TAILQ_INSERT_HEAD(&subsystem->listen_addrs, listen_addr, link);
	subsystem->num_listen_addrs++;

	return 0;
}

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem, char *host_nqn)
{
	struct spdk_nvmf_host *host;

	host = calloc(1, sizeof(*host));
	host->nqn = strdup(host_nqn);

	TAILQ_INSERT_HEAD(&subsystem->hosts, host, link);
	subsystem->num_hosts++;

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
spdk_add_nvmf_discovery_subsystem(void)
{
	struct spdk_nvmf_subsystem *subsystem;
	char *name;

	name = strdup(SPDK_NVMF_DISCOVERY_NQN);
	if (name == NULL) {
		SPDK_ERRLOG("strdup ss_group->name error\n");
		return -1;
	}

	subsystem = nvmf_create_subsystem(0, name, SPDK_NVMF_SUBTYPE_DISCOVERY, rte_get_master_lcore());
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
	int numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_listen_addr *listen_addr;
	struct spdk_nvmf_discovery_log_page_entry *entry;

	TAILQ_FOREACH(subsystem, &g_subsystems, entries) {
		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
			continue;
		}

		TAILQ_FOREACH(listen_addr, &subsystem->listen_addrs, link) {
			/* include the discovery log entry */
			if (length > sizeof(struct spdk_nvmf_discovery_log_page)) {
				if (sizeof(struct spdk_nvmf_discovery_log_page) + (numrec + 1) * sizeof(
					    struct spdk_nvmf_discovery_log_page_entry) > length) {
					break;
				}
				entry = &disc_log->entries[numrec];
				entry->portid = subsystem->num;
				entry->cntlid = 0xffff;
				entry->subtype = subsystem->subtype;
				snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn);

				listen_addr->transport->listen_addr_discover(listen_addr, entry);
			}
			numrec++;
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
