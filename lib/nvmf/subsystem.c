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

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include <uuid/uuid.h>

static bool
spdk_nvmf_valid_nqn(const char *nqn)
{
	size_t len;
	uuid_t uuid_value;
	uint i;
	const uint SPDK_NVMF_NQN_MIN_LEN = 11;
	const uint UUID_PREFIX_LEN = 32;
	const uint UUID_STRING_LEN = 36;
	char uuid_cmp_buffer[UUID_STRING_LEN];
	char *uuid_prefix = "nqn.2014-08.org.nvmexpress:uuid:";
	bool matches_uuid_prefix = true;

	/* Check for length requirements */
	len = strlen(nqn);
	if (len > SPDK_NVMF_NQN_MAX_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu > max %d\n", nqn, len, SPDK_NVMF_NQN_MAX_LEN);
		return false;
	}

	/* The nqn must be at least as long as SPDK_NVMF_NQN_MIN_LEN to contain the necessary prefix. */
	if (len < SPDK_NVMF_NQN_MIN_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu < min %d\n", nqn, len, SPDK_NVMF_NQN_MIN_LEN);
		return false;
	}

	/* Check for equality with the generic nqn structure of the form "nqn.2014-08.org.nvmexpress:uuid:11111111-2222-3333-4444-555555555555" */
	if (len >= UUID_PREFIX_LEN) {
		i = 0;
		while (i < UUID_PREFIX_LEN && matches_uuid_prefix) {
			if (nqn[i] != uuid_prefix[i]) {
				matches_uuid_prefix = false;
			}
			i++;
		}

		if (matches_uuid_prefix) {
			if (len != UUID_PREFIX_LEN + UUID_STRING_LEN) {
				SPDK_ERRLOG("Invalid NQN \"%s\": uuid is not the correct length\n", nqn);
				return false;
			}

			snprintf(uuid_cmp_buffer, UUID_STRING_LEN + 1, "%s", &nqn[UUID_PREFIX_LEN]);
			if (uuid_parse(uuid_cmp_buffer, uuid_value) == -1) {
				SPDK_ERRLOG("Invalid NQN \"%s\": uuid is not formatted correctly\n", nqn);
				return false;
			}
			return true;
		}
	}

	/* If the nqn does not match the uuid structure, check for the form "nqn.yyyy-mm.reverse.domain:user-string" */
	if (strncmp(nqn, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", nqn);
		return false;
	}

	/* Check for yyyy-mm. */
	if (!(isdigit(nqn[4]) && isdigit(nqn[5]) && isdigit(nqn[6]) && isdigit(nqn[7]) &&
	      nqn[8] == '-' && isdigit(nqn[9]) && isdigit(nqn[10]) && nqn[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", nqn);
		return false;
	}

	/* Check for valid reverse domain (i.e. something.something) */
	i = 12;
	while (i < len && nqn[i] != '.') {
		i++;
	}

	/* Check for user defined colon prefixed string */
	while (i < len) {
		if (nqn[i] == ':' && i < len - 1) {
			return true;
		}
		i++;
	}

	return false;
}

static void
spdk_nvmf_subsystem_create_done(struct spdk_io_channel_iter *i, int status)
{
}

static void
spdk_nvmf_subsystem_add_to_poll_group(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_subsystem *subsystem = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_nvmf_poll_group_add_subsystem(group, subsystem);
	spdk_for_each_channel_continue(i, rc);
}

struct spdk_nvmf_subsystem *
spdk_nvmf_create_subsystem(struct spdk_nvmf_tgt *tgt,
			   const char *nqn,
			   enum spdk_nvmf_subtype type,
			   uint32_t num_ns)
{
	struct spdk_nvmf_subsystem	*subsystem;
	uint32_t			sid;

	if (!spdk_nvmf_valid_nqn(nqn)) {
		return NULL;
	}

	if (type == SPDK_NVMF_SUBTYPE_DISCOVERY && num_ns != 0) {
		SPDK_ERRLOG("Discovery subsystem cannot have namespaces.\n");
		return NULL;
	}

	/* Find a free subsystem id (sid) */
	for (sid = 0; sid < tgt->max_sid; sid++) {
		if (tgt->subsystems[sid] == NULL) {
			break;
		}
	}
	if (sid == tgt->max_sid) {
		struct spdk_nvmf_subsystem **subsys_array;
		/* No free slots. Add more. */
		tgt->max_sid++;
		subsys_array = realloc(tgt->subsystems, tgt->max_sid * sizeof(struct spdk_nvmf_subsystem *));
		if (!subsys_array) {
			tgt->max_sid--;
			return NULL;
		}
		tgt->subsystems = subsys_array;
	}

	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));
	if (subsystem == NULL) {
		return NULL;
	}

	subsystem->tgt = tgt;
	subsystem->id = sid;
	subsystem->subtype = type;
	subsystem->max_nsid = num_ns;
	subsystem->num_allocated_nsid = 0;
	subsystem->next_cntlid = 0;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", nqn);
	TAILQ_INIT(&subsystem->listeners);
	TAILQ_INIT(&subsystem->hosts);
	TAILQ_INIT(&subsystem->ctrlrs);

	if (num_ns != 0) {
		subsystem->ns = calloc(num_ns, sizeof(struct spdk_nvmf_ns));
		if (subsystem->ns == NULL) {
			SPDK_ERRLOG("Namespace memory allocation failed\n");
			free(subsystem);
			return NULL;
		}
	}

	tgt->subsystems[sid] = subsystem;
	tgt->discovery_genctr++;

	/* Send a message to each poll group to notify it that a new subsystem
	 * is available.
	 * TODO: This call does not currently allow the user to wait for these
	 * messages to propagate. It also does not protect against two calls
	 * to this function overlapping
	 */
	spdk_for_each_channel(tgt,
			      spdk_nvmf_subsystem_add_to_poll_group,
			      subsystem,
			      spdk_nvmf_subsystem_create_done);

	return subsystem;
}

static void
spdk_nvmf_subsystem_delete_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_tgt *tgt = spdk_io_channel_iter_get_io_device(i);
	struct spdk_nvmf_subsystem *subsystem = spdk_io_channel_iter_get_ctx(i);
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}
		spdk_bdev_close(ns->desc);
	}

	free(subsystem->ns);

	tgt->subsystems[subsystem->id] = NULL;
	tgt->discovery_genctr++;

	free(subsystem);
}

static void
spdk_nvmf_subsystem_remove_from_poll_group(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_subsystem *subsystem = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_nvmf_poll_group_remove_subsystem(group, subsystem);

	spdk_for_each_channel_continue(i, rc);
}

void
spdk_nvmf_delete_subsystem(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_listener	*listener, *listener_tmp;
	struct spdk_nvmf_host		*host, *host_tmp;
	struct spdk_nvmf_ctrlr		*ctrlr, *ctrlr_tmp;

	if (!subsystem) {
		return;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "subsystem is %p\n", subsystem);

	TAILQ_FOREACH_SAFE(listener, &subsystem->listeners, link, listener_tmp) {
		TAILQ_REMOVE(&subsystem->listeners, listener, link);
		free(listener);
	}

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		TAILQ_REMOVE(&subsystem->hosts, host, link);
		free(host->nqn);
		free(host);
	}

	TAILQ_FOREACH_SAFE(ctrlr, &subsystem->ctrlrs, link, ctrlr_tmp) {
		spdk_nvmf_ctrlr_destruct(ctrlr);
	}

	/* Send a message to each poll group to notify it that a subsystem
	 * is no longer available.
	 * TODO: This call does not currently allow the user to wait for these
	 * messages to propagate. It also does not protect against two calls
	 * to this function overlapping
	 */
	spdk_for_each_channel(subsystem->tgt,
			      spdk_nvmf_subsystem_remove_from_poll_group,
			      subsystem,
			      spdk_nvmf_subsystem_delete_done);
}

struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_get_first(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_subsystem	*subsystem;
	uint32_t sid;

	for (sid = 0; sid < tgt->max_sid; sid++) {
		subsystem = tgt->subsystems[sid];
		if (subsystem) {
			return subsystem;
		}
	}

	return NULL;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_get_next(struct spdk_nvmf_subsystem *subsystem)
{
	uint32_t sid;
	struct spdk_nvmf_tgt *tgt;

	if (!subsystem) {
		return NULL;
	}

	tgt = subsystem->tgt;

	for (sid = subsystem->id + 1; sid < tgt->max_sid; sid++) {
		subsystem = tgt->subsystems[sid];
		if (subsystem) {
			return subsystem;
		}
	}

	return NULL;
}

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	if (!spdk_nvmf_valid_nqn(hostnqn)) {
		return -1;
	}

	host = calloc(1, sizeof(*host));
	if (!host) {
		return -1;
	}
	host->nqn = strdup(hostnqn);
	if (!host->nqn) {
		free(host);
		return -1;
	}

	TAILQ_INSERT_HEAD(&subsystem->hosts, host, link);
	subsystem->tgt->discovery_genctr++;

	return 0;
}

void
spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem, bool allow_any_host)
{
	subsystem->allow_any_host = allow_any_host;
}

bool
spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->allow_any_host;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	if (!hostnqn) {
		return false;
	}

	if (subsystem->allow_any_host) {
		return true;
	}

	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			return true;
		}
	}

	return false;
}

struct spdk_nvmf_host *
spdk_nvmf_subsystem_get_first_host(struct spdk_nvmf_subsystem *subsystem)
{
	return TAILQ_FIRST(&subsystem->hosts);
}


struct spdk_nvmf_host *
spdk_nvmf_subsystem_get_next_host(struct spdk_nvmf_subsystem *subsystem,
				  struct spdk_nvmf_host *prev_host)
{
	return TAILQ_NEXT(prev_host, link);
}

const char *
spdk_nvmf_host_get_nqn(struct spdk_nvmf_host *host)
{
	return host->nqn;
}

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_listener *listener;

	transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, trid->trtype);
	if (transport == NULL) {
		SPDK_ERRLOG("Unknown transport type %d\n", trid->trtype);
		return -1;
	}

	listener = calloc(1, sizeof(*listener));
	if (!listener) {
		return -1;
	}

	listener->trid = *trid;
	listener->transport = transport;

	TAILQ_INSERT_HEAD(&subsystem->listeners, listener, link);

	return 0;
}

/*
 * TODO: this is the whitelist and will be called during connection setup
 */
bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	if (TAILQ_EMPTY(&subsystem->listeners)) {
		return true;
	}

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (spdk_nvme_transport_id_compare(&listener->trid, trid) == 0) {
			return true;
		}
	}

	return false;
}

struct spdk_nvmf_listener *
spdk_nvmf_subsystem_get_first_listener(struct spdk_nvmf_subsystem *subsystem)
{
	return TAILQ_FIRST(&subsystem->listeners);
}

struct spdk_nvmf_listener *
spdk_nvmf_subsystem_get_next_listener(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_listener *prev_listener)
{
	return TAILQ_NEXT(prev_listener, link);
}


const struct spdk_nvme_transport_id *
spdk_nvmf_listener_get_trid(struct spdk_nvmf_listener *listener)
{
	return &listener->trid;
}

static void
spdk_nvmf_remove_ns_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ns *ns = spdk_io_channel_iter_get_ctx(i);
	struct spdk_nvmf_subsystem *subsystem = ns->subsystem;

	spdk_bdev_close(ns->desc);
	ns->allocated = false;
	subsystem->num_allocated_nsid--;
}

static void
spdk_nvmf_subsystem_remove_ns_from_poll_group(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_nvmf_poll_group_remove_ns(group, ns);
	spdk_for_each_channel_continue(i, rc);
}

int
spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		return -1;
	}

	ns = &subsystem->ns[nsid - 1];
	if (ns->allocated == false) {
		return -1;
	}

	spdk_for_each_channel(ns->subsystem->tgt,
			      spdk_nvmf_subsystem_remove_ns_from_poll_group,
			      ns,
			      spdk_nvmf_remove_ns_done);
	return 0;
}

static void
spdk_nvmf_subsystem_add_ns_done(struct spdk_io_channel_iter *i, int status)
{
	return;
}

static void
spdk_nvmf_subsystem_ns_update_poll_group(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_ns *ns = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_nvmf_poll_group_add_ns(group, ns->subsystem, ns);
	spdk_for_each_channel_continue(i, rc);
}

static void
_spdk_nvmf_ns_hot_remove(void *ctx)
{
	struct spdk_nvmf_ns *ns = ctx;

	spdk_nvmf_subsystem_remove_ns(ns->subsystem, ns->id);
}

static void
spdk_nvmf_ns_hot_remove(void *remove_ctx)
{
	struct spdk_nvmf_ns *ns = remove_ctx;

	ns->is_removed = true;
	spdk_thread_send_msg(ns->subsystem->tgt->master_thread,
			     _spdk_nvmf_ns_hot_remove,
			     ns);
}

uint32_t
spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
			   uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;
	uint32_t i;
	int rc;

	if (nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		SPDK_ERRLOG("Invalid NSID %" PRIu32 "\n", nsid);
		return 0;
	}

	if (nsid > subsystem->max_nsid ||
	    (nsid == 0 && subsystem->num_allocated_nsid == subsystem->max_nsid)) {
		struct spdk_nvmf_ns *new_ns_array;
		uint32_t new_max_nsid;

		if (nsid > subsystem->max_nsid) {
			new_max_nsid = nsid;
		} else {
			new_max_nsid = subsystem->max_nsid + 1;
		}

		if (!TAILQ_EMPTY(&subsystem->ctrlrs)) {
			SPDK_ERRLOG("Can't extend NSID range with active connections\n");
			return 0;
		}

		new_ns_array = realloc(subsystem->ns, sizeof(struct spdk_nvmf_ns) * new_max_nsid);
		if (new_ns_array == NULL) {
			SPDK_ERRLOG("Memory allocation error while resizing namespace array.\n");
			return 0;
		}

		memset(new_ns_array + subsystem->max_nsid, 0,
		       sizeof(struct spdk_nvmf_ns) * (new_max_nsid - subsystem->max_nsid));
		subsystem->ns = new_ns_array;
		subsystem->max_nsid = new_max_nsid;
	}

	if (nsid == 0) {
		/* NSID not specified - find a free index */
		for (i = 0; i < subsystem->max_nsid; i++) {
			if (_spdk_nvmf_subsystem_get_ns(subsystem, i + 1) == NULL) {
				nsid = i + 1;
				break;
			}
		}
		if (nsid == 0) {
			SPDK_ERRLOG("All available NSIDs in use\n");
			return 0;
		}
	} else {
		/* Specific NSID requested */
		if (_spdk_nvmf_subsystem_get_ns(subsystem, nsid)) {
			SPDK_ERRLOG("Requested NSID %" PRIu32 " already in use\n", nsid);
			return 0;
		}
	}

	ns = &subsystem->ns[nsid - 1];
	memset(ns, 0, sizeof(*ns));
	ns->bdev = bdev;
	ns->id = nsid;
	ns->subsystem = subsystem;
	rc = spdk_bdev_open(bdev, true, spdk_nvmf_ns_hot_remove, ns, &ns->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Subsystem %s: bdev %s cannot be opened, error=%d\n",
			    subsystem->subnqn, spdk_bdev_get_name(bdev), rc);
		return 0;
	}
	ns->allocated = true;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Subsystem %s: bdev %s assigned nsid %" PRIu32 "\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem),
		      spdk_bdev_get_name(bdev),
		      nsid);

	subsystem->max_nsid = spdk_max(subsystem->max_nsid, nsid);
	subsystem->num_allocated_nsid++;

	/* Send a message to each poll group to notify it that a new namespace
	 * is available.
	 * TODO: This call does not currently allow the user to wait for these
	 * messages to propagate. It also does not protect against two calls
	 * to this function overlapping
	 */
	spdk_for_each_channel(subsystem->tgt,
			      spdk_nvmf_subsystem_ns_update_poll_group,
			      ns,
			      spdk_nvmf_subsystem_add_ns_done);

	return nsid;
}

static uint32_t
spdk_nvmf_subsystem_get_next_allocated_nsid(struct spdk_nvmf_subsystem *subsystem,
		uint32_t prev_nsid)
{
	uint32_t nsid;

	if (prev_nsid >= subsystem->max_nsid) {
		return 0;
	}

	for (nsid = prev_nsid + 1; nsid <= subsystem->max_nsid; nsid++) {
		if (subsystem->ns[nsid - 1].allocated) {
			return nsid;
		}
	}

	return 0;
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_first_ns(struct spdk_nvmf_subsystem *subsystem)
{
	uint32_t first_nsid;

	first_nsid = spdk_nvmf_subsystem_get_next_allocated_nsid(subsystem, 0);
	return _spdk_nvmf_subsystem_get_ns(subsystem, first_nsid);
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_ns *prev_ns)
{
	uint32_t next_nsid;

	next_nsid = spdk_nvmf_subsystem_get_next_allocated_nsid(subsystem, prev_ns->id);
	return _spdk_nvmf_subsystem_get_ns(subsystem, next_nsid);
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	return _spdk_nvmf_subsystem_get_ns(subsystem, nsid);
}

uint32_t
spdk_nvmf_ns_get_id(const struct spdk_nvmf_ns *ns)
{
	return ns->id;
}

struct spdk_bdev *
spdk_nvmf_ns_get_bdev(struct spdk_nvmf_ns *ns)
{
	return ns->bdev;
}

const char *
spdk_nvmf_subsystem_get_sn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->sn;
}

int
spdk_nvmf_subsystem_set_sn(struct spdk_nvmf_subsystem *subsystem, const char *sn)
{
	size_t len, max_len;

	max_len = sizeof(subsystem->sn) - 1;
	len = strlen(sn);
	if (len > max_len) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Invalid sn \"%s\": length %zu > max %zu\n",
			      sn, len, max_len);
		return -1;
	}

	snprintf(subsystem->sn, sizeof(subsystem->sn), "%s", sn);

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

static uint16_t
spdk_nvmf_subsystem_gen_cntlid(struct spdk_nvmf_subsystem *subsystem)
{
	int count;

	/*
	 * In the worst case, we might have to try all CNTLID values between 1 and 0xFFF0 - 1
	 * before we find one that is unused (or find that all values are in use).
	 */
	for (count = 0; count < 0xFFF0 - 1; count++) {
		subsystem->next_cntlid++;
		if (subsystem->next_cntlid >= 0xFFF0) {
			/* The spec reserves cntlid values in the range FFF0h to FFFFh. */
			subsystem->next_cntlid = 1;
		}

		/* Check if a controller with this cntlid currently exists. */
		if (spdk_nvmf_subsystem_get_ctrlr(subsystem, subsystem->next_cntlid) == NULL) {
			/* Found unused cntlid */
			return subsystem->next_cntlid;
		}
	}

	/* All valid cntlid values are in use. */
	return 0xFFFF;
}

int
spdk_nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr)
{
	ctrlr->cntlid = spdk_nvmf_subsystem_gen_cntlid(subsystem);
	if (ctrlr->cntlid == 0xFFFF) {
		/* Unable to get a cntlid */
		SPDK_ERRLOG("Reached max simultaneous ctrlrs\n");
		return -EBUSY;
	}

	TAILQ_INSERT_TAIL(&subsystem->ctrlrs, ctrlr, link);

	return 0;
}

void
spdk_nvmf_subsystem_remove_ctrlr(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvmf_ctrlr *ctrlr)
{
	assert(subsystem == ctrlr->subsys);
	TAILQ_REMOVE(&subsystem->ctrlrs, ctrlr, link);
}

struct spdk_nvmf_ctrlr *
spdk_nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid)
{
	struct spdk_nvmf_ctrlr *ctrlr;

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (ctrlr->cntlid == cntlid) {
			return ctrlr;
		}
	}

	return NULL;
}
