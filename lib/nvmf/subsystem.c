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
#include <assert.h>

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
		if (strcmp(subnqn, subsystem->subnqn) == 0) {
			if (subsystem->num_hosts == 0) {
				/* No hosts means any host can connect */
				return subsystem;
			}

			TAILQ_FOREACH(host, &subsystem->hosts, link) {
				if (strcmp(hostnqn, host->nqn) == 0) {
					return subsystem;
				}
			}
		}
	}

	return NULL;
}

void
spdk_nvmf_subsystem_poll(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_session *session;

	TAILQ_FOREACH(session, &subsystem->sessions, link) {
		/* For NVMe subsystems, check the backing physical device for completions. */
		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
			session->subsys->ops->poll_for_completions(session);
		}

		/* For each connection in the session, check for completions */
		spdk_nvmf_session_poll(session);
	}
}

static bool
spdk_nvmf_valid_nqn(const char *nqn)
{
	size_t len;

	len = strlen(nqn);
	if (len >= SPDK_NVMF_NQN_MAX_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu > max %d\n", nqn, len, SPDK_NVMF_NQN_MAX_LEN - 1);
		return false;
	}

	if (strncmp(nqn, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", nqn);
		return false;
	}

	/* yyyy-mm. */
	if (!(isdigit(nqn[4]) && isdigit(nqn[5]) && isdigit(nqn[6]) && isdigit(nqn[7]) &&
	      nqn[8] == '-' && isdigit(nqn[9]) && isdigit(nqn[10]) && nqn[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", nqn);
		return false;
	}

	return true;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_create_subsystem(const char *nqn,
			   enum spdk_nvmf_subtype type,
			   enum spdk_nvmf_subsystem_mode mode,
			   void *cb_ctx,
			   spdk_nvmf_subsystem_connect_fn connect_cb,
			   spdk_nvmf_subsystem_disconnect_fn disconnect_cb)
{
	struct spdk_nvmf_subsystem	*subsystem;

	if (!spdk_nvmf_valid_nqn(nqn)) {
		return NULL;
	}

	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));
	if (subsystem == NULL) {
		return NULL;
	}

	subsystem->subtype = type;
	subsystem->mode = mode;
	subsystem->cb_ctx = cb_ctx;
	subsystem->connect_cb = connect_cb;
	subsystem->disconnect_cb = disconnect_cb;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", nqn);
	TAILQ_INIT(&subsystem->listen_addrs);
	TAILQ_INIT(&subsystem->hosts);
	TAILQ_INIT(&subsystem->sessions);

	if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
		subsystem->ops = &spdk_nvmf_direct_ctrlr_ops;
	} else {
		subsystem->ops = &spdk_nvmf_virtual_ctrlr_ops;
	}

	TAILQ_INSERT_HEAD(&g_subsystems, subsystem, entries);

	return subsystem;
}

void
spdk_nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_listen_addr	*listen_addr, *listen_addr_tmp;
	struct spdk_nvmf_host		*host, *host_tmp;
	struct spdk_nvmf_session	*session, *session_tmp;

	if (!subsystem) {
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "subsystem is %p\n", subsystem);

	TAILQ_FOREACH_SAFE(listen_addr, &subsystem->listen_addrs, link, listen_addr_tmp) {
		TAILQ_REMOVE(&subsystem->listen_addrs, listen_addr, link);
		free(listen_addr->traddr);
		free(listen_addr->trsvcid);
		free(listen_addr->trname);
		free(listen_addr);
		subsystem->num_listen_addrs--;
	}

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		TAILQ_REMOVE(&subsystem->hosts, host, link);
		free(host->nqn);
		free(host);
		subsystem->num_hosts--;
	}

	TAILQ_FOREACH_SAFE(session, &subsystem->sessions, link, session_tmp) {
		spdk_nvmf_session_destruct(session);
	}

	if (subsystem->ops->detach) {
		subsystem->ops->detach(subsystem);
	}

	TAILQ_REMOVE(&g_subsystems, subsystem, entries);

	free(subsystem);
}

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 char *trname, char *traddr, char *trsvcid)
{
	struct spdk_nvmf_listen_addr *listen_addr;
	const struct spdk_nvmf_transport *transport;
	int rc;

	transport = spdk_nvmf_transport_get(trname);
	if (!transport) {
		return -1;
	}

	listen_addr = calloc(1, sizeof(*listen_addr));
	if (!listen_addr) {
		return -1;
	}

	listen_addr->traddr = strdup(traddr);
	if (!listen_addr->traddr) {
		free(listen_addr);
		return -1;
	}

	listen_addr->trsvcid = strdup(trsvcid);
	if (!listen_addr->trsvcid) {
		free(listen_addr->traddr);
		free(listen_addr);
		return -1;
	}

	listen_addr->trname = strdup(trname);
	if (!listen_addr->trname) {
		free(listen_addr->traddr);
		free(listen_addr->trsvcid);
		free(listen_addr);
		return -1;
	}

	TAILQ_INSERT_HEAD(&subsystem->listen_addrs, listen_addr, link);
	subsystem->num_listen_addrs++;

	rc = transport->listen_addr_add(listen_addr);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to listen on address '%s'\n", traddr);
		return -1;
	}

	return 0;
}

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem, char *host_nqn)
{
	struct spdk_nvmf_host *host;

	host = calloc(1, sizeof(*host));
	if (!host) {
		return -1;
	}
	host->nqn = strdup(host_nqn);
	if (!host->nqn) {
		free(host);
		return -1;
	}

	TAILQ_INSERT_HEAD(&subsystem->hosts, host, link);
	subsystem->num_hosts++;

	return 0;
}

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_ctrlr *ctrlr, const struct spdk_pci_addr *pci_addr)
{
	subsystem->dev.direct.ctrlr = ctrlr;
	subsystem->dev.direct.pci_addr = *pci_addr;
	/* Assume that all I/O will be handled on one thread for now */
	subsystem->dev.direct.io_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);
	if (subsystem->dev.direct.io_qpair == NULL) {
		SPDK_ERRLOG("spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -1;
	}
	return 0;
}

void
spdk_format_discovery_log(struct spdk_nvmf_discovery_log_page *disc_log, uint32_t length)
{
	int numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_listen_addr *listen_addr;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	const struct spdk_nvmf_transport *transport;

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
				entry->portid = numrec;
				entry->cntlid = 0xffff;
				entry->asqsz = g_nvmf_tgt.max_queue_depth;
				entry->subtype = subsystem->subtype;
				snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn);

				transport = spdk_nvmf_transport_get(listen_addr->trname);
				assert(transport != NULL);

				transport->listen_addr_discover(listen_addr, entry);
			}
			numrec++;
		}
	}

	disc_log->numrec = numrec;
}

int
spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev)
{
	int i = 0;

	assert(subsystem->mode == NVMF_SUBSYSTEM_MODE_VIRTUAL);
	while (i < MAX_VIRTUAL_NAMESPACE && subsystem->dev.virt.ns_list[i]) {
		i++;
	}
	if (i == MAX_VIRTUAL_NAMESPACE) {
		SPDK_ERRLOG("spdk_nvmf_subsystem_add_ns() failed\n");
		return -1;
	}
	subsystem->dev.virt.ns_list[i] = bdev;
	subsystem->dev.virt.ns_count++;
	return 0;
}

int
spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn)
{
	if (subsystem->mode != NVMF_SUBSYSTEM_MODE_VIRTUAL) {
		return -1;
	}

	snprintf(subsystem->dev.virt.sn, sizeof(subsystem->dev.virt.sn), "%s", sn);

	return 0;
}

const char *
spdk_nvmf_subsystem_get_nqn(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subnqn;
}

/* Workaround for astyle formatting bug */
typedef enum spdk_nvmf_subtype nvmf_subtype_t;

nvmf_subtype_t
spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subtype;
}

/* Workaround for astyle formatting bug */
typedef enum spdk_nvmf_subsystem_mode nvmf_mode_t;

nvmf_mode_t
spdk_nvmf_subsystem_get_mode(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->mode;
}
