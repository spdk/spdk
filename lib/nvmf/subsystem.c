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
#include "spdk/uuid.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"
#include "spdk_internal/utf.h"

/*
 * States for parsing valid domains in NQNs according to RFC 1034
 */
enum spdk_nvmf_nqn_domain_states {
	/* First character of a domain must be a letter */
	SPDK_NVMF_DOMAIN_ACCEPT_LETTER = 0,

	/* Subsequent characters can be any of letter, digit, or hyphen */
	SPDK_NVMF_DOMAIN_ACCEPT_LDH = 1,

	/* A domain label must end with either a letter or digit */
	SPDK_NVMF_DOMAIN_ACCEPT_ANY = 2
};

/* Returns true if is a valid ASCII string as defined by the NVMe spec */
static bool
spdk_nvmf_valid_ascii_string(const void *buf, size_t size)
{
	const uint8_t *str = buf;
	size_t i;

	for (i = 0; i < size; i++) {
		if (str[i] < 0x20 || str[i] > 0x7E) {
			return false;
		}
	}

	return true;
}

static bool
spdk_nvmf_valid_nqn(const char *nqn)
{
	size_t len;
	struct spdk_uuid uuid_value;
	uint32_t i;
	int bytes_consumed;
	uint32_t domain_label_length;
	char *reverse_domain_end;
	uint32_t reverse_domain_end_index;
	enum spdk_nvmf_nqn_domain_states domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LETTER;

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

	/* Check for discovery controller nqn */
	if (!strcmp(nqn, SPDK_NVMF_DISCOVERY_NQN)) {
		return true;
	}

	/* Check for equality with the generic nqn structure of the form "nqn.2014-08.org.nvmexpress:uuid:11111111-2222-3333-4444-555555555555" */
	if (!strncmp(nqn, SPDK_NVMF_NQN_UUID_PRE, SPDK_NVMF_NQN_UUID_PRE_LEN)) {
		if (len != SPDK_NVMF_NQN_UUID_PRE_LEN + SPDK_NVMF_UUID_STRING_LEN) {
			SPDK_ERRLOG("Invalid NQN \"%s\": uuid is not the correct length\n", nqn);
			return false;
		}

		if (spdk_uuid_parse(&uuid_value, &nqn[SPDK_NVMF_NQN_UUID_PRE_LEN])) {
			SPDK_ERRLOG("Invalid NQN \"%s\": uuid is not formatted correctly\n", nqn);
			return false;
		}
		return true;
	}

	/* If the nqn does not match the uuid structure, the next several checks validate the form "nqn.yyyy-mm.reverse.domain:user-string" */

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

	reverse_domain_end = strchr(nqn, ':');
	if (reverse_domain_end != NULL && (reverse_domain_end_index = reverse_domain_end - nqn) < len - 1) {
	} else {
		SPDK_ERRLOG("Invalid NQN \"%s\". NQN must contain user specified name with a ':' as a prefix.\n",
			    nqn);
		return false;
	}

	/* Check for valid reverse domain */
	domain_label_length = 0;
	for (i = 12; i < reverse_domain_end_index; i++) {
		if (domain_label_length > SPDK_DOMAIN_LABEL_MAX_LEN) {
			SPDK_ERRLOG("Invalid domain name in NQN \"%s\". At least one Label is too long.\n", nqn);
			return false;
		}

		switch (domain_state) {

		case SPDK_NVMF_DOMAIN_ACCEPT_LETTER: {
			if (isalpha(nqn[i])) {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_ANY;
				domain_label_length++;
				break;
			} else {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must start with a letter.\n", nqn);
				return false;
			}
		}

		case SPDK_NVMF_DOMAIN_ACCEPT_LDH: {
			if (isalpha(nqn[i]) || isdigit(nqn[i])) {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_ANY;
				domain_label_length++;
				break;
			} else if (nqn[i] == '-') {
				if (i == reverse_domain_end_index - 1) {
					SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must end with an alphanumeric symbol.\n",
						    nqn);
					return false;
				}
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LDH;
				domain_label_length++;
				break;
			} else if (nqn[i] == '.') {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must end with an alphanumeric symbol.\n",
					    nqn);
				return false;
			} else {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must contain only [a-z,A-Z,0-9,'-','.'].\n",
					    nqn);
				return false;
			}
		}

		case SPDK_NVMF_DOMAIN_ACCEPT_ANY: {
			if (isalpha(nqn[i]) || isdigit(nqn[i])) {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_ANY;
				domain_label_length++;
				break;
			} else if (nqn[i] == '-') {
				if (i == reverse_domain_end_index - 1) {
					SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must end with an alphanumeric symbol.\n",
						    nqn);
					return false;
				}
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LDH;
				domain_label_length++;
				break;
			} else if (nqn[i] == '.') {
				domain_state = SPDK_NVMF_DOMAIN_ACCEPT_LETTER;
				domain_label_length = 0;
				break;
			} else {
				SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must contain only [a-z,A-Z,0-9,'-','.'].\n",
					    nqn);
				return false;
			}
		}
		}
	}

	i = reverse_domain_end_index + 1;
	while (i < len) {
		bytes_consumed = utf8_valid(&nqn[i], &nqn[len]);
		if (bytes_consumed <= 0) {
			SPDK_ERRLOG("Invalid domain name in NQN \"%s\". Label names must contain only valid utf-8.\n", nqn);
			return false;
		}

		i += bytes_consumed;
	}
	return true;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_create(struct spdk_nvmf_tgt *tgt,
			   const char *nqn,
			   enum spdk_nvmf_subtype type,
			   uint32_t num_ns)
{
	struct spdk_nvmf_subsystem	*subsystem;
	uint32_t			sid;

	if (spdk_nvmf_tgt_find_subsystem(tgt, nqn)) {
		SPDK_ERRLOG("Subsystem NQN '%s' already exists\n", nqn);
		return NULL;
	}

	if (!spdk_nvmf_valid_nqn(nqn)) {
		return NULL;
	}

	if (type == SPDK_NVMF_SUBTYPE_DISCOVERY && num_ns != 0) {
		SPDK_ERRLOG("Discovery subsystem cannot have namespaces.\n");
		return NULL;
	}

	/* Find a free subsystem id (sid) */
	for (sid = 0; sid < tgt->max_subsystems; sid++) {
		if (tgt->subsystems[sid] == NULL) {
			break;
		}
	}
	if (sid >= tgt->max_subsystems) {
		return NULL;
	}

	subsystem = calloc(1, sizeof(struct spdk_nvmf_subsystem));
	if (subsystem == NULL) {
		return NULL;
	}

	subsystem->thread = spdk_get_thread();
	subsystem->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;
	subsystem->tgt = tgt;
	subsystem->id = sid;
	subsystem->subtype = type;
	subsystem->max_nsid = num_ns;
	subsystem->max_allowed_nsid = num_ns;
	subsystem->next_cntlid = 0;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", nqn);
	TAILQ_INIT(&subsystem->listeners);
	TAILQ_INIT(&subsystem->hosts);
	TAILQ_INIT(&subsystem->ctrlrs);

	if (num_ns != 0) {
		subsystem->ns = calloc(num_ns, sizeof(struct spdk_nvmf_ns *));
		if (subsystem->ns == NULL) {
			SPDK_ERRLOG("Namespace memory allocation failed\n");
			free(subsystem);
			return NULL;
		}
	}

	memset(subsystem->sn, '0', sizeof(subsystem->sn) - 1);
	subsystem->sn[sizeof(subsystem->sn) - 1] = '\0';

	tgt->subsystems[sid] = subsystem;
	tgt->discovery_genctr++;

	return subsystem;
}

static void
_spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *host)
{
	TAILQ_REMOVE(&subsystem->hosts, host, link);
	free(host->nqn);
	free(host);
}

static int _spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid);

static void
_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_listener *listener)
{
	struct spdk_nvmf_transport *transport;

	transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, listener->trid.trtype);
	if (transport != NULL) {
		spdk_nvmf_transport_stop_listen(transport, &listener->trid);
	}

	TAILQ_REMOVE(&subsystem->listeners, listener, link);
	free(listener);
}

void
spdk_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_listener	*listener, *listener_tmp;
	struct spdk_nvmf_host		*host, *host_tmp;
	struct spdk_nvmf_ctrlr		*ctrlr, *ctrlr_tmp;
	struct spdk_nvmf_ns		*ns;

	if (!subsystem) {
		return;
	}

	assert(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "subsystem is %p\n", subsystem);

	TAILQ_FOREACH_SAFE(listener, &subsystem->listeners, link, listener_tmp) {
		_nvmf_subsystem_remove_listener(subsystem, listener);
	}

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		_spdk_nvmf_subsystem_remove_host(subsystem, host);
	}

	TAILQ_FOREACH_SAFE(ctrlr, &subsystem->ctrlrs, link, ctrlr_tmp) {
		spdk_nvmf_ctrlr_destruct(ctrlr);
	}

	ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	while (ns != NULL) {
		struct spdk_nvmf_ns *next_ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns);

		_spdk_nvmf_subsystem_remove_ns(subsystem, ns->opts.nsid);
		ns = next_ns;
	}

	free(subsystem->ns);

	subsystem->tgt->subsystems[subsystem->id] = NULL;
	subsystem->tgt->discovery_genctr++;

	free(subsystem);
}

static int
spdk_nvmf_subsystem_set_state(struct spdk_nvmf_subsystem *subsystem,
			      enum spdk_nvmf_subsystem_state state)
{
	enum spdk_nvmf_subsystem_state actual_old_state, expected_old_state;

	switch (state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_DEACTIVATING;
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVATING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_INACTIVE;
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_PAUSING;
		break;
	case SPDK_NVMF_SUBSYSTEM_RESUMING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_PAUSED;
		break;
	case SPDK_NVMF_SUBSYSTEM_DEACTIVATING:
		expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
		break;
	default:
		assert(false);
		return -1;
	}

	actual_old_state = __sync_val_compare_and_swap(&subsystem->state, expected_old_state, state);
	if (actual_old_state != expected_old_state) {
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_RESUMING &&
		    state == SPDK_NVMF_SUBSYSTEM_ACTIVE) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_RESUMING;
		}
		/* This is for the case when activating the subsystem fails. */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_ACTIVATING &&
		    state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		}
		actual_old_state = __sync_val_compare_and_swap(&subsystem->state, expected_old_state, state);
	}
	assert(actual_old_state == expected_old_state);
	return actual_old_state - expected_old_state;
}

struct subsystem_state_change_ctx {
	struct spdk_nvmf_subsystem *subsystem;

	enum spdk_nvmf_subsystem_state requested_state;

	spdk_nvmf_subsystem_state_change_done cb_fn;
	void *cb_arg;
};

static void
subsystem_state_change_done(struct spdk_io_channel_iter *i, int status)
{
	struct subsystem_state_change_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (status == 0) {
		status = spdk_nvmf_subsystem_set_state(ctx->subsystem, ctx->requested_state);
		if (status) {
			status = -1;
		}
	}

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->subsystem, ctx->cb_arg, status);
	}
	free(ctx);
}

static void
subsystem_state_change_continue(void *ctx, int status)
{
	struct spdk_io_channel_iter *i = ctx;
	spdk_for_each_channel_continue(i, status);
}

static void
subsystem_state_change_on_pg(struct spdk_io_channel_iter *i)
{
	struct subsystem_state_change_ctx *ctx;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	switch (ctx->requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		spdk_nvmf_poll_group_remove_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_ACTIVATING) {
			spdk_nvmf_poll_group_add_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		} else if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_RESUMING) {
			spdk_nvmf_poll_group_resume_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		}
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		spdk_nvmf_poll_group_pause_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		break;
	default:
		assert(false);
		break;
	}
}

static int
spdk_nvmf_subsystem_state_change(struct spdk_nvmf_subsystem *subsystem,
				 enum spdk_nvmf_subsystem_state requested_state,
				 spdk_nvmf_subsystem_state_change_done cb_fn,
				 void *cb_arg)
{
	struct subsystem_state_change_ctx *ctx;
	enum spdk_nvmf_subsystem_state intermediate_state;
	int rc;

	switch (requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		intermediate_state = SPDK_NVMF_SUBSYSTEM_DEACTIVATING;
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED) {
			intermediate_state = SPDK_NVMF_SUBSYSTEM_RESUMING;
		} else {
			intermediate_state = SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		}
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		intermediate_state = SPDK_NVMF_SUBSYSTEM_PAUSING;
		break;
	default:
		assert(false);
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	rc = spdk_nvmf_subsystem_set_state(subsystem, intermediate_state);
	if (rc) {
		free(ctx);
		return rc;
	}

	ctx->subsystem = subsystem;
	ctx->requested_state = requested_state;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(subsystem->tgt,
			      subsystem_state_change_on_pg,
			      ctx,
			      subsystem_state_change_done);

	return 0;
}

int
spdk_nvmf_subsystem_start(struct spdk_nvmf_subsystem *subsystem,
			  spdk_nvmf_subsystem_state_change_done cb_fn,
			  void *cb_arg)
{
	return spdk_nvmf_subsystem_state_change(subsystem, SPDK_NVMF_SUBSYSTEM_ACTIVE, cb_fn, cb_arg);
}

int
spdk_nvmf_subsystem_stop(struct spdk_nvmf_subsystem *subsystem,
			 spdk_nvmf_subsystem_state_change_done cb_fn,
			 void *cb_arg)
{
	return spdk_nvmf_subsystem_state_change(subsystem, SPDK_NVMF_SUBSYSTEM_INACTIVE, cb_fn, cb_arg);
}

int
spdk_nvmf_subsystem_pause(struct spdk_nvmf_subsystem *subsystem,
			  spdk_nvmf_subsystem_state_change_done cb_fn,
			  void *cb_arg)
{
	return spdk_nvmf_subsystem_state_change(subsystem, SPDK_NVMF_SUBSYSTEM_PAUSED, cb_fn, cb_arg);
}

int
spdk_nvmf_subsystem_resume(struct spdk_nvmf_subsystem *subsystem,
			   spdk_nvmf_subsystem_state_change_done cb_fn,
			   void *cb_arg)
{
	return spdk_nvmf_subsystem_state_change(subsystem, SPDK_NVMF_SUBSYSTEM_ACTIVE, cb_fn, cb_arg);
}

struct spdk_nvmf_subsystem *
spdk_nvmf_subsystem_get_first(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_subsystem	*subsystem;
	uint32_t sid;

	for (sid = 0; sid < tgt->max_subsystems; sid++) {
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

	for (sid = subsystem->id + 1; sid < tgt->max_subsystems; sid++) {
		subsystem = tgt->subsystems[sid];
		if (subsystem) {
			return subsystem;
		}
	}

	return NULL;
}

static struct spdk_nvmf_host *
_spdk_nvmf_subsystem_find_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host = NULL;

	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		if (strcmp(hostnqn, host->nqn) == 0) {
			return host;
		}
	}

	return NULL;
}

int
spdk_nvmf_subsystem_add_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	if (!spdk_nvmf_valid_nqn(hostnqn)) {
		return -EINVAL;
	}

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -EAGAIN;
	}

	if (_spdk_nvmf_subsystem_find_host(subsystem, hostnqn)) {
		/* This subsystem already allows the specified host. */
		return 0;
	}

	host = calloc(1, sizeof(*host));
	if (!host) {
		return -ENOMEM;
	}
	host->nqn = strdup(hostnqn);
	if (!host->nqn) {
		free(host);
		return -ENOMEM;
	}

	TAILQ_INSERT_HEAD(&subsystem->hosts, host, link);
	subsystem->tgt->discovery_genctr++;

	return 0;
}

int
spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -EAGAIN;
	}

	host = _spdk_nvmf_subsystem_find_host(subsystem, hostnqn);
	if (host == NULL) {
		return -ENOENT;
	}

	_spdk_nvmf_subsystem_remove_host(subsystem, host);
	return 0;
}

int
spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem, bool allow_any_host)
{
	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -EAGAIN;
	}

	subsystem->allow_any_host = allow_any_host;

	return 0;
}

bool
spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->allow_any_host;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	if (!hostnqn) {
		return false;
	}

	if (subsystem->allow_any_host) {
		return true;
	}

	return _spdk_nvmf_subsystem_find_host(subsystem, hostnqn) != NULL;
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

static struct spdk_nvmf_listener *
_spdk_nvmf_subsystem_find_listener(struct spdk_nvmf_subsystem *subsystem,
				   const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (spdk_nvme_transport_id_compare(&listener->trid, trid) == 0) {
			return listener;
		}
	}

	return NULL;
}

int
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_listener *listener;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -EAGAIN;
	}

	if (_spdk_nvmf_subsystem_find_listener(subsystem, trid)) {
		/* Listener already exists in this subsystem */
		return 0;
	}

	transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, trid->trtype);
	if (transport == NULL) {
		SPDK_ERRLOG("Unknown transport type %d\n", trid->trtype);
		return -EINVAL;
	}

	listener = calloc(1, sizeof(*listener));
	if (!listener) {
		return -ENOMEM;
	}

	listener->trid = *trid;
	listener->transport = transport;

	TAILQ_INSERT_HEAD(&subsystem->listeners, listener, link);

	return 0;
}

int
spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				    const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -EAGAIN;
	}

	listener = _spdk_nvmf_subsystem_find_listener(subsystem, trid);
	if (listener == NULL) {
		return -ENOENT;
	}

	_nvmf_subsystem_remove_listener(subsystem, listener);

	return 0;
}

bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	if (!strcmp(subsystem->subnqn, SPDK_NVMF_DISCOVERY_NQN)) {
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

struct subsystem_update_ns_ctx {
	struct spdk_nvmf_subsystem *subsystem;

	spdk_nvmf_subsystem_state_change_done cb_fn;
	void *cb_arg;
};

static void
subsystem_update_ns_done(struct spdk_io_channel_iter *i, int status)
{
	struct subsystem_update_ns_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->subsystem, ctx->cb_arg, status);
	}
	free(ctx);
}

static void
subsystem_update_ns_on_pg(struct spdk_io_channel_iter *i)
{
	int rc;
	struct subsystem_update_ns_ctx *ctx;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_subsystem *subsystem;

	ctx = spdk_io_channel_iter_get_ctx(i);
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));
	subsystem = ctx->subsystem;

	rc = spdk_nvmf_poll_group_update_subsystem(group, subsystem);
	spdk_for_each_channel_continue(i, rc);
}

static int
spdk_nvmf_subsystem_update_ns(struct spdk_nvmf_subsystem *subsystem, spdk_channel_for_each_cpl cpl,
			      void *ctx)
{
	spdk_for_each_channel(subsystem->tgt,
			      subsystem_update_ns_on_pg,
			      ctx,
			      cpl);

	return 0;
}

static void
spdk_nvmf_subsystem_ns_changed(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ctrlr *ctrlr;

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		spdk_nvmf_ctrlr_ns_changed(ctrlr, nsid);
	}
}

static int
_spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_registrant *reg, *reg_tmp;

	assert(subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED ||
	       subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE);

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		return -1;
	}

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -1;
	}

	ns = subsystem->ns[nsid - 1];
	if (!ns) {
		return -1;
	}

	subsystem->ns[nsid - 1] = NULL;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, reg_tmp) {
		TAILQ_REMOVE(&ns->registrants, reg, link);
		free(reg);
	}
	spdk_bdev_module_release_bdev(ns->bdev);
	spdk_bdev_close(ns->desc);
	free(ns);

	spdk_nvmf_subsystem_ns_changed(subsystem, nsid);

	return 0;
}

int
spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid,
			      spdk_nvmf_subsystem_state_change_done cb_fn, void *cb_arg)
{
	int rc;
	struct subsystem_update_ns_ctx *ctx;

	rc = _spdk_nvmf_subsystem_remove_ns(subsystem, nsid);
	if (rc < 0) {
		return rc;
	}

	ctx = calloc(1, sizeof(*ctx));

	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->subsystem = subsystem;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_nvmf_subsystem_update_ns(subsystem, subsystem_update_ns_done, ctx);

	return 0;
}

static void
_spdk_nvmf_ns_hot_remove_done(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	if (status != 0) {
		SPDK_ERRLOG("Failed to make changes to NVMe-oF subsystem with id %u\n", subsystem->id);
	}
	spdk_nvmf_subsystem_resume(subsystem, NULL, NULL);
}

static void
_spdk_nvmf_ns_hot_remove(struct spdk_nvmf_subsystem *subsystem,
			 void *cb_arg, int status)
{
	struct spdk_nvmf_ns *ns = cb_arg;

	spdk_nvmf_subsystem_remove_ns(subsystem, ns->opts.nsid, _spdk_nvmf_ns_hot_remove_done,
				      subsystem);
}

static void
spdk_nvmf_ns_hot_remove(void *remove_ctx)
{
	struct spdk_nvmf_ns *ns = remove_ctx;
	int rc;

	rc = spdk_nvmf_subsystem_pause(ns->subsystem, _spdk_nvmf_ns_hot_remove, ns);
	if (rc) {
		SPDK_ERRLOG("Unable to pause subsystem to process namespace removal!\n");
	}
}

void
spdk_nvmf_ns_opts_get_defaults(struct spdk_nvmf_ns_opts *opts, size_t opts_size)
{
	/* All current fields are set to 0 by default. */
	memset(opts, 0, opts_size);
}

/* Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module ns_bdev_module = {
	.name	= "NVMe-oF Target",
};

uint32_t
spdk_nvmf_subsystem_add_ns(struct spdk_nvmf_subsystem *subsystem, struct spdk_bdev *bdev,
			   const struct spdk_nvmf_ns_opts *user_opts, size_t opts_size)
{
	struct spdk_nvmf_ns_opts opts;
	struct spdk_nvmf_ns *ns;
	int rc;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return 0;
	}

	spdk_nvmf_ns_opts_get_defaults(&opts, sizeof(opts));
	if (user_opts) {
		memcpy(&opts, user_opts, spdk_min(sizeof(opts), opts_size));
	}

	if (spdk_mem_all_zero(&opts.uuid, sizeof(opts.uuid))) {
		opts.uuid = *spdk_bdev_get_uuid(bdev);
	}

	if (opts.nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		SPDK_ERRLOG("Invalid NSID %" PRIu32 "\n", opts.nsid);
		return 0;
	}

	if (opts.nsid == 0) {
		/*
		 * NSID not specified - find a free index.
		 *
		 * If no free slots are found, opts.nsid will be subsystem->max_nsid + 1, which will
		 * expand max_nsid if possible.
		 */
		for (opts.nsid = 1; opts.nsid <= subsystem->max_nsid; opts.nsid++) {
			if (_spdk_nvmf_subsystem_get_ns(subsystem, opts.nsid) == NULL) {
				break;
			}
		}
	}

	if (_spdk_nvmf_subsystem_get_ns(subsystem, opts.nsid)) {
		SPDK_ERRLOG("Requested NSID %" PRIu32 " already in use\n", opts.nsid);
		return 0;
	}

	if (opts.nsid > subsystem->max_nsid) {
		struct spdk_nvmf_ns **new_ns_array;

		/* If MaxNamespaces was specified, we can't extend max_nsid beyond it. */
		if (subsystem->max_allowed_nsid > 0 && opts.nsid > subsystem->max_allowed_nsid) {
			SPDK_ERRLOG("Can't extend NSID range above MaxNamespaces\n");
			return 0;
		}

		/* If a controller is connected, we can't change NN. */
		if (!TAILQ_EMPTY(&subsystem->ctrlrs)) {
			SPDK_ERRLOG("Can't extend NSID range while controllers are connected\n");
			return 0;
		}

		new_ns_array = realloc(subsystem->ns, sizeof(struct spdk_nvmf_ns *) * opts.nsid);
		if (new_ns_array == NULL) {
			SPDK_ERRLOG("Memory allocation error while resizing namespace array.\n");
			return 0;
		}

		memset(new_ns_array + subsystem->max_nsid, 0,
		       sizeof(struct spdk_nvmf_ns *) * (opts.nsid - subsystem->max_nsid));
		subsystem->ns = new_ns_array;
		subsystem->max_nsid = opts.nsid;
	}

	ns = calloc(1, sizeof(*ns));
	if (ns == NULL) {
		SPDK_ERRLOG("Namespace allocation failed\n");
		return 0;
	}

	ns->bdev = bdev;
	ns->opts = opts;
	ns->subsystem = subsystem;
	rc = spdk_bdev_open(bdev, true, spdk_nvmf_ns_hot_remove, ns, &ns->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Subsystem %s: bdev %s cannot be opened, error=%d\n",
			    subsystem->subnqn, spdk_bdev_get_name(bdev), rc);
		free(ns);
		return 0;
	}
	rc = spdk_bdev_module_claim_bdev(bdev, ns->desc, &ns_bdev_module);
	if (rc != 0) {
		spdk_bdev_close(ns->desc);
		free(ns);
		return 0;
	}
	subsystem->ns[opts.nsid - 1] = ns;
	ns->nsid = opts.nsid;
	TAILQ_INIT(&ns->registrants);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Subsystem %s: bdev %s assigned nsid %" PRIu32 "\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem),
		      spdk_bdev_get_name(bdev),
		      opts.nsid);

	spdk_nvmf_subsystem_ns_changed(subsystem, opts.nsid);

	return opts.nsid;
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
		if (subsystem->ns[nsid - 1]) {
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

	next_nsid = spdk_nvmf_subsystem_get_next_allocated_nsid(subsystem, prev_ns->opts.nsid);
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
	return ns->opts.nsid;
}

struct spdk_bdev *
spdk_nvmf_ns_get_bdev(struct spdk_nvmf_ns *ns)
{
	return ns->bdev;
}

void
spdk_nvmf_ns_get_opts(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_ns_opts *opts,
		      size_t opts_size)
{
	memset(opts, 0, opts_size);
	memcpy(opts, &ns->opts, spdk_min(sizeof(ns->opts), opts_size));
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

	if (!spdk_nvmf_valid_ascii_string(sn, len)) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Non-ASCII sn\n");
		SPDK_LOGDUMP(SPDK_LOG_NVMF, "sn", sn, len);
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

uint32_t
spdk_nvmf_subsystem_get_max_namespaces(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_allowed_nsid;
}

static struct spdk_nvmf_registrant *
nvmf_ns_reservation_get_registrant(struct spdk_nvmf_ns *ns,
				   struct spdk_uuid *uuid)
{
	struct spdk_nvmf_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (spdk_uuid_compare(&reg->hostid, uuid) == 0) {
			return reg;
		}
	}

	return NULL;
}

/* Generate reservation notice log to registered HostID controllers */
static void
nvmf_subsystem_gen_ctrlr_notification(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_ns *ns,
				      struct spdk_uuid *hostid_list,
				      uint32_t num_hostid,
				      enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_ctrlr *ctrlr;
	uint32_t i;

	for (i = 0; i < num_hostid; i++) {
		TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
			if (spdk_uuid_compare(&ctrlr->hostid, &hostid_list[i]) == 0) {
				spdk_nvmf_ctrlr_reservation_notice_log(ctrlr, ns, type);
			}
		}
	}
}

/* Get all registrants' hostid other than the controller who issued the command */
static uint32_t
nvmf_ns_reservation_get_all_other_hostid(struct spdk_nvmf_ns *ns,
		struct spdk_uuid *hostid_list,
		uint32_t max_num_hostid,
		struct spdk_uuid *current_hostid)
{
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t num_hostid = 0;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (spdk_uuid_compare(&reg->hostid, current_hostid)) {
			hostid_list[num_hostid++] = reg->hostid;
			if (num_hostid == max_num_hostid) {
				num_hostid = 0;
				break;
			}
		}
	}

	return num_hostid;
}

/* Caculate the unregistered HostID list according to list
 * prior to execute preempt command and list after executing
 * preempt command.
 */
static uint32_t
nvmf_ns_reservation_get_unregistered_hostid(struct spdk_uuid *old_hostid_list,
		uint32_t old_num_hostid,
		struct spdk_uuid *remanent_hostid_list,
		uint32_t remanent_num_hostid)
{
	struct spdk_uuid temp_hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t i, j, num_hostid = 0;
	bool found;

	if (!remanent_num_hostid) {
		return old_num_hostid;
	}

	for (i = 0; i < old_num_hostid; i++) {
		found = false;
		for (j = 0; j < remanent_num_hostid; j++) {
			if (!spdk_uuid_compare(&old_hostid_list[i], &remanent_hostid_list[j])) {
				found = true;
				break;
			}
		}
		if (!found) {
			spdk_uuid_copy(&temp_hostid_list[num_hostid++], &old_hostid_list[i]);
		}
	}

	if (num_hostid) {
		memcpy(old_hostid_list, temp_hostid_list, sizeof(struct spdk_uuid) * num_hostid);
	}

	return num_hostid;
}

/* current reservation type is all registrants or not */
static bool
nvmf_ns_reservation_all_registrants_type(struct spdk_nvmf_ns *ns)
{
	return (ns->rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
		ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS);
}

/* current registrant is reservation holder or not */
static bool
nvmf_ns_reservation_registrant_is_holder(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	if (!reg) {
		return false;
	}

	if (nvmf_ns_reservation_all_registrants_type(ns)) {
		return true;
	}

	return (ns->holder == reg);
}

static int
nvmf_ns_reservation_add_registrant(struct spdk_nvmf_ns *ns,
				   struct spdk_nvmf_ctrlr *ctrlr,
				   uint64_t nrkey)
{
	struct spdk_nvmf_registrant *reg;

	reg = calloc(1, sizeof(*reg));
	if (!reg) {
		return -ENOMEM;
	}

	reg->rkey = nrkey;
	/* set hostid for the registrant */
	spdk_uuid_copy(&reg->hostid, &ctrlr->hostid);
	TAILQ_INSERT_TAIL(&ns->registrants, reg, link);
	ns->gen++;

	return 0;
}

static void
nvmf_ns_reservation_release_reservation(struct spdk_nvmf_ns *ns)
{
	ns->rtype = 0;
	ns->crkey = 0;
	ns->holder = NULL;
}

/* release the reservation if the last registrant was removed */
static void
nvmf_ns_reservation_check_release_on_remove_registrant(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *next_reg;

	/* no reservation holder */
	if (!ns->holder) {
		assert(ns->rtype == 0);
		return;
	}

	next_reg = TAILQ_FIRST(&ns->registrants);
	if (next_reg && nvmf_ns_reservation_all_registrants_type(ns)) {
		/* the next valid registrant is the new holder now */
		ns->holder = next_reg;
	} else if (nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
		/* release the reservation */
		nvmf_ns_reservation_release_reservation(ns);
	}
}

static void
nvmf_ns_reservation_remove_registrant(struct spdk_nvmf_ns *ns,
				      struct spdk_nvmf_registrant *reg)
{
	TAILQ_REMOVE(&ns->registrants, reg, link);
	nvmf_ns_reservation_check_release_on_remove_registrant(ns, reg);
	free(reg);
	ns->gen++;
	return;
}

static uint32_t
nvmf_ns_reservation_remove_registrants_by_key(struct spdk_nvmf_ns *ns,
		uint64_t rkey)
{
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (reg->rkey == rkey) {
			nvmf_ns_reservation_remove_registrant(ns, reg);
			count++;
		}
	}
	return count;
}

static uint32_t
nvmf_ns_reservation_remove_all_other_registrants(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_registrant *reg_tmp, *reg_tmp2;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg_tmp, &ns->registrants, link, reg_tmp2) {
		if (reg_tmp != reg) {
			nvmf_ns_reservation_remove_registrant(ns, reg_tmp);
			count++;
		}
	}
	return count;
}

static uint32_t
nvmf_ns_reservation_clear_all_registrants(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_registrant *reg, *reg_tmp;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, reg_tmp) {
		nvmf_ns_reservation_remove_registrant(ns, reg);
		count++;
	}
	return count;
}

static void
nvmf_ns_reservation_acquire_reservation(struct spdk_nvmf_ns *ns, uint64_t rkey,
					enum spdk_nvme_reservation_type rtype,
					struct spdk_nvmf_registrant *holder)
{
	ns->rtype = rtype;
	ns->crkey = rkey;
	assert(ns->holder == NULL);
	ns->holder = holder;
}

static bool
nvmf_ns_reservation_register(struct spdk_nvmf_ns *ns,
			     struct spdk_nvmf_ctrlr *ctrlr,
			     struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint8_t rrega, iekey, cptpl, rtype;
	struct spdk_nvme_reservation_register_data key;
	struct spdk_nvmf_registrant *reg;
	uint8_t status = SPDK_NVME_SC_SUCCESS;
	bool update_sgroup = false;
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t num_hostid = 0;
	int rc;

	rrega = cmd->cdw10 & 0x7u;
	iekey = (cmd->cdw10 >> 3) & 0x1u;
	cptpl = (cmd->cdw10 >> 30) & 0x3u;
	memcpy(&key, req->data, sizeof(key));

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "REGISTER: RREGA %u, IEKEY %u, CPTPL %u, "
		      "NRKEY 0x%"PRIx64", NRKEY 0x%"PRIx64"\n",
		      rrega, iekey, cptpl, key.crkey, key.nrkey);

	/* TODO: doesn't support for now */
	if (cptpl == SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS) {
		SPDK_ERRLOG("Can't change persist through power loss for now\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	/* current Host Identifier has registrant or not */
	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);

	switch (rrega) {
	case SPDK_NVME_RESERVE_REGISTER_KEY:
		if (!reg) {
			/* register new controller */
			if (key.nrkey == 0) {
				SPDK_ERRLOG("Can't register zeroed new key\n");
				status = SPDK_NVME_SC_INVALID_FIELD;
				goto exit;
			}
			rc = nvmf_ns_reservation_add_registrant(ns, ctrlr, key.nrkey);
			if (rc < 0) {
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				goto exit;
			}
		} else {
			/* register with same key is not an error */
			if (reg->rkey != key.nrkey) {
				SPDK_ERRLOG("The same host already register a "
					    "key with 0x%"PRIx64"\n",
					    reg->rkey);
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
		}
		break;
	case SPDK_NVME_RESERVE_UNREGISTER_KEY:
		if (!reg || (!iekey && reg->rkey != key.crkey)) {
			SPDK_ERRLOG("No registrant or current key doesn't match "
				    "with existing registrant key\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}

		rtype = ns->rtype;
		num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
				SPDK_NVMF_MAX_NUM_REGISTRANTS,
				&ctrlr->hostid);

		nvmf_ns_reservation_remove_registrant(ns, reg);

		if (!ns->rtype && num_hostid && (rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY ||
						 rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY)) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);
		}
		update_sgroup = true;
		break;
	case SPDK_NVME_RESERVE_REPLACE_KEY:
		if (!reg || (!iekey && reg->rkey != key.crkey)) {
			SPDK_ERRLOG("No registrant or current key doesn't match "
				    "with existing registrant key\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (key.nrkey == 0) {
			SPDK_ERRLOG("Can't register zeroed new key\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		}
		reg->rkey = key.nrkey;
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return update_sgroup;
}

static bool
nvmf_ns_reservation_acquire(struct spdk_nvmf_ns *ns,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint8_t racqa, iekey, rtype;
	struct spdk_nvme_reservation_acquire_data key;
	struct spdk_nvmf_registrant *reg;
	bool all_regs = false;
	uint32_t count = 0;
	bool update_sgroup = true;
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t num_hostid = 0;
	struct spdk_uuid new_hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t new_num_hostid = 0;
	bool reservation_released = false;
	uint8_t status = SPDK_NVME_SC_SUCCESS;

	racqa = cmd->cdw10 & 0x7u;
	iekey = (cmd->cdw10 >> 3) & 0x1u;
	rtype = (cmd->cdw10 >> 8) & 0xffu;
	memcpy(&key, req->data, sizeof(key));

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ACQUIIRE: RACQA %u, IEKEY %u, RTYPE %u, "
		      "NRKEY 0x%"PRIx64", PRKEY 0x%"PRIx64"\n",
		      racqa, iekey, rtype, key.crkey, key.prkey);

	if (iekey) {
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);
	/* must be registrant and CRKEY must match */
	if (!reg || reg->rkey != key.crkey) {
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		update_sgroup = false;
		goto exit;
	}

	all_regs = nvmf_ns_reservation_all_registrants_type(ns);

	switch (racqa) {
	case SPDK_NVME_RESERVE_ACQUIRE:
		/* it's not an error for the holder to acquire same reservation type again */
		if (nvmf_ns_reservation_registrant_is_holder(ns, reg) && ns->rtype == rtype) {
			/* do nothing */
			update_sgroup = false;
		} else if (ns->holder == NULL) {
			/* fisrt time to acquire the reservation */
			nvmf_ns_reservation_acquire_reservation(ns, key.crkey, rtype, reg);
		} else {
			SPDK_ERRLOG("Invalid rtype or current registrant is not holder\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			update_sgroup = false;
			goto exit;
		}
		break;
	case SPDK_NVME_RESERVE_PREEMPT:
		/* no reservation holder */
		if (!ns->holder) {
			/* unregister with PRKEY */
			nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
			break;
		}
		num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
				SPDK_NVMF_MAX_NUM_REGISTRANTS,
				&ctrlr->hostid);

		/* only 1 reservation holder and reservation key is valid */
		if (!all_regs) {
			/* preempt itself */
			if (nvmf_ns_reservation_registrant_is_holder(ns, reg) &&
			    ns->crkey == key.prkey) {
				ns->rtype = rtype;
				reservation_released = true;
				break;
			}

			if (ns->crkey == key.prkey) {
				nvmf_ns_reservation_remove_registrant(ns, ns->holder);
				nvmf_ns_reservation_acquire_reservation(ns, key.crkey, rtype, reg);
				reservation_released = true;
			} else if (key.prkey != 0) {
				nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
			} else {
				/* PRKEY is zero */
				SPDK_ERRLOG("Current PRKEY is zero\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				update_sgroup = false;
				goto exit;
			}
		} else {
			/* release all other registrants except for the current one */
			if (key.prkey == 0) {
				nvmf_ns_reservation_remove_all_other_registrants(ns, reg);
				assert(ns->holder == reg);
			} else {
				count = nvmf_ns_reservation_remove_registrants_by_key(ns, key.prkey);
				if (count == 0) {
					SPDK_ERRLOG("PRKEY doesn't match any registrant\n");
					status = SPDK_NVME_SC_RESERVATION_CONFLICT;
					update_sgroup = false;
					goto exit;
				}
			}
		}
		break;
	default:
		update_sgroup = false;
		break;
	}

exit:
	if (update_sgroup && racqa == SPDK_NVME_RESERVE_PREEMPT) {
		new_num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, new_hostid_list,
				 SPDK_NVMF_MAX_NUM_REGISTRANTS,
				 &ctrlr->hostid);
		/* Preempt notification occurs on the unregistered controller
		 * other than the controller who issued the command.
		 */
		num_hostid = nvmf_ns_reservation_get_unregistered_hostid(hostid_list,
				num_hostid,
				new_hostid_list,
				new_num_hostid);
		if (num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_REGISTRATION_PREEMPTED);

		}
		/* Reservation released notificaiton occurs on the
		 * controllers which are the remanent registrants other than
		 * the controller who issued the command.
		 */
		if (reservation_released && new_num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      new_hostid_list,
							      new_num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);

		}
	}
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return update_sgroup;
}

static bool
nvmf_ns_reservation_release(struct spdk_nvmf_ns *ns,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint8_t rrela, iekey, rtype;
	struct spdk_nvmf_registrant *reg;
	uint64_t crkey;
	uint8_t status = SPDK_NVME_SC_SUCCESS;
	bool update_sgroup = true;
	struct spdk_uuid hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t num_hostid = 0;

	rrela = cmd->cdw10 & 0x7u;
	iekey = (cmd->cdw10 >> 3) & 0x1u;
	rtype = (cmd->cdw10 >> 8) & 0xffu;
	memcpy(&crkey, req->data, sizeof(crkey));

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "RELEASE: RRELA %u, IEKEY %u, RTYPE %u, "
		      "CRKEY 0x%"PRIx64"\n",  rrela, iekey, rtype, crkey);

	if (iekey) {
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

	reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr->hostid);
	if (!reg || reg->rkey != crkey) {
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		update_sgroup = false;
		goto exit;
	}

	num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, hostid_list,
			SPDK_NVMF_MAX_NUM_REGISTRANTS,
			&ctrlr->hostid);

	switch (rrela) {
	case SPDK_NVME_RESERVE_RELEASE:
		if (!ns->holder) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "RELEASE: no holder\n");
			update_sgroup = false;
			goto exit;
		}
		if (ns->rtype != rtype) {
			SPDK_ERRLOG("Type doesn't match\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			update_sgroup = false;
			goto exit;
		}
		if (!nvmf_ns_reservation_registrant_is_holder(ns, reg)) {
			/* not the reservation holder, this isn't an error */
			update_sgroup = false;
			goto exit;
		}

		rtype = ns->rtype;
		nvmf_ns_reservation_release_reservation(ns);

		if (num_hostid && rtype != SPDK_NVME_RESERVE_WRITE_EXCLUSIVE &&
		    rtype != SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);
		}
		break;
	case SPDK_NVME_RESERVE_CLEAR:
		nvmf_ns_reservation_clear_all_registrants(ns);
		if (num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      hostid_list,
							      num_hostid,
							      SPDK_NVME_RESERVATION_PREEMPTED);
		}
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return update_sgroup;
}

static void
nvmf_ns_reservation_report(struct spdk_nvmf_ns *ns,
			   struct spdk_nvmf_ctrlr *ctrlr,
			   struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ctrlr *ctrlr_tmp;
	struct spdk_nvmf_registrant *reg, *tmp;
	struct spdk_nvme_reservation_status_extended_data *status_data;
	struct spdk_nvme_registered_ctrlr_extended_data *ctrlr_data;
	uint8_t *payload;
	uint32_t len, count = 0;
	uint32_t regctl = 0;
	uint8_t status = SPDK_NVME_SC_SUCCESS;

	/* NVMeoF uses Extended Data Structure */
	if ((cmd->cdw11 & 0x00000001u) == 0) {
		SPDK_ERRLOG("NVMeoF uses extended controller data structure, "
			    "please set EDS bit in cdw11 and try again\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	/* Get number of registerd controllers, one Host may have more than
	 * one controller based on different ports.
	 */
	TAILQ_FOREACH(ctrlr_tmp, &subsystem->ctrlrs, link) {
		reg = nvmf_ns_reservation_get_registrant(ns, &ctrlr_tmp->hostid);
		if (reg) {
			regctl++;
		}
	}

	len = sizeof(*status_data) + sizeof(*ctrlr_data) * regctl;
	payload = calloc(1, len);
	if (!payload) {
		status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto exit;
	}

	status_data = (struct spdk_nvme_reservation_status_extended_data *)payload;
	status_data->data.gen = ns->gen;
	status_data->data.rtype = ns->rtype;
	status_data->data.regctl = regctl;
	/* TODO: Don't support Persist Through Power Loss State for now */
	status_data->data.ptpls = 0;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		assert(count <= regctl);
		ctrlr_data = (struct spdk_nvme_registered_ctrlr_extended_data *)
			     (payload + sizeof(*status_data) + sizeof(*ctrlr_data) * count);
		/* Set to 0xffffh for dynamic controller */
		ctrlr_data->cntlid = 0xffff;
		ctrlr_data->rcsts.status = (ns->holder == reg) ? true : false;
		ctrlr_data->rkey = reg->rkey;
		spdk_uuid_copy((struct spdk_uuid *)ctrlr_data->hostid, &reg->hostid);
		count++;
	}

	memcpy(req->data, payload, spdk_min(len, (cmd->cdw10 + 1) * sizeof(uint32_t)));
	free(payload);

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return;
}

static void
spdk_nvmf_ns_reservation_complete(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;

	spdk_nvmf_request_complete(req);
}

static void
_nvmf_ns_reservation_update_done(struct spdk_nvmf_subsystem *subsystem,
				 void *cb_arg, int status)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)cb_arg;
	struct spdk_nvmf_poll_group *group = req->qpair->group;

	spdk_thread_send_msg(group->thread, spdk_nvmf_ns_reservation_complete, req);
}

void
spdk_nvmf_ns_reservation_request(void *ctx)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)ctx;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct subsystem_update_ns_ctx *update_ctx;
	uint32_t nsid;
	struct spdk_nvmf_ns *ns;
	bool update_sgroup = false;

	nsid = cmd->nsid;
	ns = _spdk_nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	assert(ns != NULL);

	switch (cmd->opc) {
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
		update_sgroup = nvmf_ns_reservation_register(ns, ctrlr, req);
		break;
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		update_sgroup = nvmf_ns_reservation_acquire(ns, ctrlr, req);
		break;
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		update_sgroup = nvmf_ns_reservation_release(ns, ctrlr, req);
		break;
	case SPDK_NVME_OPC_RESERVATION_REPORT:
		nvmf_ns_reservation_report(ns, ctrlr, req);
		break;
	default:
		break;
	}

	/* update reservation information to subsystem's poll group */
	if (update_sgroup) {
		update_ctx = calloc(1, sizeof(*update_ctx));
		if (update_ctx == NULL) {
			SPDK_ERRLOG("Can't alloc subsystem poll group update context\n");
			goto update_done;
		}
		update_ctx->subsystem = ctrlr->subsys;
		update_ctx->cb_fn = _nvmf_ns_reservation_update_done;
		update_ctx->cb_arg = req;

		spdk_nvmf_subsystem_update_ns(ctrlr->subsys, subsystem_update_ns_done, update_ctx);
		return;
	}

update_done:
	_nvmf_ns_reservation_update_done(ctrlr->subsys, (void *)req, 0);
}
