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

#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

bool
spdk_nvmf_subsystem_exists(const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;

	if (!subnqn) {
		return false;
	}

	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		if (strcmp(subnqn, subsystem->subnqn) == 0) {
			return true;
		}
	}

	return false;
}

struct spdk_nvmf_subsystem *
nvmf_find_subsystem(const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;

	if (!subnqn) {
		return NULL;
	}

	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		if (strcmp(subnqn, subsystem->subnqn) == 0) {
			return subsystem;
		}
	}

	return NULL;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_find_subsystem_with_cntlid(uint16_t cntlid)
{
	struct spdk_nvmf_subsystem	*subsystem;
	struct spdk_nvmf_session 	*session;

	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		TAILQ_FOREACH(session, &subsystem->sessions, link) {
			if (session->cntlid == cntlid) {
				return subsystem;
			}
		}
	}

	return NULL;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	if (!hostnqn) {
		return false;
	}

	if (subsystem->num_hosts == 0) {
		/* No hosts means any host can connect */
		return true;
	}

	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			return true;
		}
	}

	return false;
}

int
spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem)
{
	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		return subsystem->ops->attach(subsystem);
	}

	return 0;
}

void
spdk_nvmf_subsystem_poll(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_session *session;

	/* For NVMe subsystems, check the backing physical device for completions. */
	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		subsystem->ops->poll_for_completions(subsystem);
	}

	TAILQ_FOREACH(session, &subsystem->sessions, link) {
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
	TAILQ_INIT(&subsystem->allowed_listeners);
	TAILQ_INIT(&subsystem->hosts);
	TAILQ_INIT(&subsystem->sessions);

	if (mode == NVMF_SUBSYSTEM_MODE_DIRECT) {
		subsystem->ops = &spdk_nvmf_direct_ctrlr_ops;
	} else {
		subsystem->ops = &spdk_nvmf_virtual_ctrlr_ops;
	}

	TAILQ_INSERT_TAIL(&g_nvmf_tgt.subsystems, subsystem, entries);
	g_nvmf_tgt.discovery_genctr++;

	return subsystem;
}

void
spdk_nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_allowed_listener	*allowed_listener, *allowed_listener_tmp;
	struct spdk_nvmf_host		*host, *host_tmp;
	struct spdk_nvmf_session	*session, *session_tmp;

	if (!subsystem) {
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "subsystem is %p\n", subsystem);

	TAILQ_FOREACH_SAFE(allowed_listener,
			   &subsystem->allowed_listeners, link, allowed_listener_tmp) {
		TAILQ_REMOVE(&subsystem->allowed_listeners, allowed_listener, link);

		free(allowed_listener);
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

	TAILQ_REMOVE(&g_nvmf_tgt.subsystems, subsystem, entries);
	g_nvmf_tgt.discovery_genctr++;

	free(subsystem);
}

struct spdk_nvmf_listen_addr *
spdk_nvmf_tgt_listen(const char *trname, const char *traddr, const char *trsvcid)
{
	struct spdk_nvmf_listen_addr *listen_addr;
	const struct spdk_nvmf_transport *transport;
	int rc;

	TAILQ_FOREACH(listen_addr, &g_nvmf_tgt.listen_addrs, link) {
		if ((strcmp(listen_addr->trname, trname) == 0) &&
		    (strcmp(listen_addr->traddr, traddr) == 0) &&
		    (strcmp(listen_addr->trsvcid, trsvcid) == 0)) {
			return listen_addr;
		}
	}

	transport = spdk_nvmf_transport_get(trname);
	if (!transport) {
		return NULL;
	}

	listen_addr = spdk_nvmf_listen_addr_create(trname, traddr, trsvcid);
	if (!listen_addr) {
		return NULL;
	}

	rc = transport->listen_addr_add(listen_addr);
	if (rc < 0) {
		spdk_nvmf_listen_addr_cleanup(listen_addr);
		SPDK_ERRLOG("Unable to listen on address '%s'\n", traddr);
		return NULL;
	}

	TAILQ_INSERT_HEAD(&g_nvmf_tgt.listen_addrs, listen_addr, link);
	g_nvmf_tgt.discovery_genctr++;

	return listen_addr;
}

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_listen_addr *listen_addr)
{
	struct spdk_nvmf_subsystem_allowed_listener *allowed_listener;

	allowed_listener = calloc(1, sizeof(*allowed_listener));
	if (!allowed_listener) {
		return -1;
	}

	allowed_listener->listen_addr = listen_addr;

	TAILQ_INSERT_HEAD(&subsystem->allowed_listeners, allowed_listener, link);

	return 0;
}

/*
 * TODO: this is the whitelist and will be called during connection setup
 */
bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvmf_listen_addr *listen_addr)
{
	struct spdk_nvmf_subsystem_allowed_listener *allowed_listener;

	if (TAILQ_EMPTY(&subsystem->allowed_listeners)) {
		return true;
	}

	TAILQ_FOREACH(allowed_listener, &subsystem->allowed_listeners, link) {
		if (allowed_listener->listen_addr == listen_addr) {
			return true;
		}
	}

	return false;
}

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem, const char *host_nqn)
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
	g_nvmf_tgt.discovery_genctr++;

	return 0;
}

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			 struct spdk_nvme_ctrlr *ctrlr, const struct spdk_pci_addr *pci_addr)
{
	subsystem->dev.direct.ctrlr = ctrlr;
	subsystem->dev.direct.pci_addr = *pci_addr;

	return 0;
}

static void
nvmf_update_discovery_log(void)
{
	uint64_t numrec = 0;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_allowed_listener *allowed_listener;
	struct spdk_nvmf_listen_addr *listen_addr;
	struct spdk_nvmf_discovery_log_page_entry *entry;
	const struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_discovery_log_page *disc_log;
	size_t cur_size;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Generating log page for genctr %" PRIu64 "\n",
		      g_nvmf_tgt.discovery_genctr);

	cur_size = sizeof(struct spdk_nvmf_discovery_log_page);
	disc_log = calloc(1, cur_size);
	if (disc_log == NULL) {
		SPDK_ERRLOG("Discovery log page memory allocation error\n");
		return;
	}

	TAILQ_FOREACH(subsystem, &g_nvmf_tgt.subsystems, entries) {
		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
			continue;
		}

		TAILQ_FOREACH(allowed_listener, &subsystem->allowed_listeners, link) {
			size_t new_size = cur_size + sizeof(*entry);
			void *new_log_page = realloc(disc_log, new_size);

			if (new_log_page == NULL) {
				SPDK_ERRLOG("Discovery log page memory allocation error\n");
				break;
			}

			listen_addr = allowed_listener->listen_addr;

			disc_log = new_log_page;
			cur_size = new_size;

			entry = &disc_log->entries[numrec];
			memset(entry, 0, sizeof(*entry));
			entry->portid = numrec;
			entry->cntlid = 0xffff;
			entry->asqsz = g_nvmf_tgt.max_queue_depth;
			entry->subtype = subsystem->subtype;
			snprintf(entry->subnqn, sizeof(entry->subnqn), "%s", subsystem->subnqn);

			transport = spdk_nvmf_transport_get(listen_addr->trname);
			assert(transport != NULL);

			transport->listen_addr_discover(listen_addr, entry);

			numrec++;
		}
	}

	disc_log->numrec = numrec;
	disc_log->genctr = g_nvmf_tgt.discovery_genctr;

	free(g_nvmf_tgt.discovery_log_page);

	g_nvmf_tgt.discovery_log_page = disc_log;
	g_nvmf_tgt.discovery_log_page_size = cur_size;
}

void
spdk_nvmf_get_discovery_log_page(void *buffer, uint64_t offset, uint32_t length)
{
	size_t copy_len = 0;
	size_t zero_len = length;

	if (g_nvmf_tgt.discovery_log_page == NULL ||
	    g_nvmf_tgt.discovery_log_page->genctr != g_nvmf_tgt.discovery_genctr) {
		nvmf_update_discovery_log();
	}

	/* Copy the valid part of the discovery log page, if any */
	if (g_nvmf_tgt.discovery_log_page && offset < g_nvmf_tgt.discovery_log_page_size) {
		copy_len = spdk_min(g_nvmf_tgt.discovery_log_page_size - offset, length);
		zero_len -= copy_len;
		memcpy(buffer, (char *)g_nvmf_tgt.discovery_log_page + offset, copy_len);
	}

	/* Zero out the rest of the buffer */
	if (zero_len) {
		memset((char *)buffer + copy_len, 0, zero_len);
	}

	/* We should have copied or zeroed every byte of the output buffer. */
	assert(copy_len + zero_len == length);
}

int
spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev)
{
	int i = 0;

	if (!spdk_bdev_claim(bdev, NULL, NULL)) {
		SPDK_ERRLOG("Subsystem %s: bdev %s is already claimed\n",
			    subsystem->subnqn, bdev->name);
		return -1;
	}

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
