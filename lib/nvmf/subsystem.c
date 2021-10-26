/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "spdk/assert.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/nvmf_spec.h"
#include "spdk/uuid.h"
#include "spdk/json.h"
#include "spdk/file.h"
#include "spdk/bit_array.h"

#define __SPDK_BDEV_MODULE_ONLY
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk_internal/utf.h"
#include "spdk_internal/usdt.h"

#define MODEL_NUMBER_DEFAULT "SPDK bdev Controller"
#define NVMF_SUBSYSTEM_DEFAULT_NAMESPACES 32

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

static int _nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem);

/* Returns true if is a valid ASCII string as defined by the NVMe spec */
static bool
nvmf_valid_ascii_string(const void *buf, size_t size)
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
nvmf_valid_nqn(const char *nqn)
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

static void subsystem_state_change_on_pg(struct spdk_io_channel_iter *i);

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

	if (!nvmf_valid_nqn(nqn)) {
		return NULL;
	}

	if (type == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		if (num_ns != 0) {
			SPDK_ERRLOG("Discovery subsystem cannot have namespaces.\n");
			return NULL;
		}
	} else if (num_ns == 0) {
		num_ns = NVMF_SUBSYSTEM_DEFAULT_NAMESPACES;
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
	subsystem->next_cntlid = 0;
	subsystem->min_cntlid = NVMF_MIN_CNTLID;
	subsystem->max_cntlid = NVMF_MAX_CNTLID;
	snprintf(subsystem->subnqn, sizeof(subsystem->subnqn), "%s", nqn);
	pthread_mutex_init(&subsystem->mutex, NULL);
	TAILQ_INIT(&subsystem->listeners);
	TAILQ_INIT(&subsystem->hosts);
	TAILQ_INIT(&subsystem->ctrlrs);
	subsystem->used_listener_ids = spdk_bit_array_create(NVMF_MAX_LISTENERS_PER_SUBSYSTEM);
	if (subsystem->used_listener_ids == NULL) {
		pthread_mutex_destroy(&subsystem->mutex);
		free(subsystem);
		return NULL;
	}

	if (num_ns != 0) {
		subsystem->ns = calloc(num_ns, sizeof(struct spdk_nvmf_ns *));
		if (subsystem->ns == NULL) {
			SPDK_ERRLOG("Namespace memory allocation failed\n");
			pthread_mutex_destroy(&subsystem->mutex);
			spdk_bit_array_free(&subsystem->used_listener_ids);
			free(subsystem);
			return NULL;
		}
		subsystem->ana_group = calloc(num_ns, sizeof(uint32_t));
		if (subsystem->ana_group == NULL) {
			SPDK_ERRLOG("ANA group memory allocation failed\n");
			pthread_mutex_destroy(&subsystem->mutex);
			free(subsystem->ns);
			spdk_bit_array_free(&subsystem->used_listener_ids);
			free(subsystem);
			return NULL;
		}
	}

	memset(subsystem->sn, '0', sizeof(subsystem->sn) - 1);
	subsystem->sn[sizeof(subsystem->sn) - 1] = '\0';

	snprintf(subsystem->mn, sizeof(subsystem->mn), "%s",
		 MODEL_NUMBER_DEFAULT);

	tgt->subsystems[sid] = subsystem;

	SPDK_DTRACE_PROBE1(nvmf_subsystem_create, subsystem->subnqn);

	return subsystem;
}

/* Must hold subsystem->mutex while calling this function */
static void
nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *host)
{
	TAILQ_REMOVE(&subsystem->hosts, host, link);
	free(host);
}

static void
_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_subsystem_listener *listener,
				bool stop)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_ctrlr *ctrlr;

	if (stop) {
		transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, listener->trid->trstring);
		if (transport != NULL) {
			spdk_nvmf_transport_stop_listen(transport, listener->trid);
		}
	}

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		if (ctrlr->listener == listener) {
			ctrlr->listener = NULL;
		}
	}

	TAILQ_REMOVE(&subsystem->listeners, listener, link);
	nvmf_update_discovery_log(listener->subsystem->tgt, NULL);
	free(listener->ana_state);
	spdk_bit_array_clear(subsystem->used_listener_ids, listener->id);
	free(listener);
}

static void
_nvmf_subsystem_destroy_msg(void *cb_arg)
{
	struct spdk_nvmf_subsystem *subsystem = cb_arg;

	_nvmf_subsystem_destroy(subsystem);
}

static int
_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns		*ns;
	nvmf_subsystem_destroy_cb	async_destroy_cb = NULL;
	void				*async_destroy_cb_arg = NULL;
	int				rc;

	if (!TAILQ_EMPTY(&subsystem->ctrlrs)) {
		SPDK_DEBUGLOG(nvmf, "subsystem %p %s has active controllers\n", subsystem, subsystem->subnqn);
		subsystem->async_destroy = true;
		rc = spdk_thread_send_msg(subsystem->thread, _nvmf_subsystem_destroy_msg, subsystem);
		if (rc) {
			SPDK_ERRLOG("Failed to send thread msg, rc %d\n", rc);
			assert(0);
			return rc;
		}
		return -EINPROGRESS;
	}

	ns = spdk_nvmf_subsystem_get_first_ns(subsystem);
	while (ns != NULL) {
		struct spdk_nvmf_ns *next_ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns);

		spdk_nvmf_subsystem_remove_ns(subsystem, ns->opts.nsid);
		ns = next_ns;
	}

	free(subsystem->ns);
	free(subsystem->ana_group);

	subsystem->tgt->subsystems[subsystem->id] = NULL;

	pthread_mutex_destroy(&subsystem->mutex);

	spdk_bit_array_free(&subsystem->used_listener_ids);

	if (subsystem->async_destroy) {
		async_destroy_cb = subsystem->async_destroy_cb;
		async_destroy_cb_arg = subsystem->async_destroy_cb_arg;
	}

	free(subsystem);

	if (async_destroy_cb) {
		async_destroy_cb(async_destroy_cb_arg);
	}

	return 0;
}

int
spdk_nvmf_subsystem_destroy(struct spdk_nvmf_subsystem *subsystem, nvmf_subsystem_destroy_cb cpl_cb,
			    void *cpl_cb_arg)
{
	struct spdk_nvmf_host *host, *host_tmp;

	if (!subsystem) {
		return -EINVAL;
	}

	SPDK_DTRACE_PROBE1(nvmf_subsystem_destroy, subsystem->subnqn);

	assert(spdk_get_thread() == subsystem->thread);

	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		SPDK_ERRLOG("Subsystem can only be destroyed in inactive state\n");
		assert(0);
		return -EAGAIN;
	}
	if (subsystem->destroying) {
		SPDK_ERRLOG("Subsystem destruction is already started\n");
		assert(0);
		return -EALREADY;
	}

	subsystem->destroying = true;

	SPDK_DEBUGLOG(nvmf, "subsystem is %p %s\n", subsystem, subsystem->subnqn);

	nvmf_subsystem_remove_all_listeners(subsystem, false);

	pthread_mutex_lock(&subsystem->mutex);

	TAILQ_FOREACH_SAFE(host, &subsystem->hosts, link, host_tmp) {
		nvmf_subsystem_remove_host(subsystem, host);
	}

	pthread_mutex_unlock(&subsystem->mutex);

	subsystem->async_destroy_cb = cpl_cb;
	subsystem->async_destroy_cb_arg = cpl_cb_arg;

	return _nvmf_subsystem_destroy(subsystem);
}

/* we have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvmf_subsystem_state spdk_nvmf_subsystem_state_t;

static spdk_nvmf_subsystem_state_t
nvmf_subsystem_get_intermediate_state(enum spdk_nvmf_subsystem_state current_state,
				      enum spdk_nvmf_subsystem_state requested_state)
{
	switch (requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		return SPDK_NVMF_SUBSYSTEM_DEACTIVATING;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (current_state == SPDK_NVMF_SUBSYSTEM_PAUSED) {
			return SPDK_NVMF_SUBSYSTEM_RESUMING;
		} else {
			return SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		}
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		return SPDK_NVMF_SUBSYSTEM_PAUSING;
	default:
		assert(false);
		return SPDK_NVMF_SUBSYSTEM_NUM_STATES;
	}
}

static int
nvmf_subsystem_set_state(struct spdk_nvmf_subsystem *subsystem,
			 enum spdk_nvmf_subsystem_state state)
{
	enum spdk_nvmf_subsystem_state actual_old_state, expected_old_state;
	bool exchanged;

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

	actual_old_state = expected_old_state;
	exchanged = __atomic_compare_exchange_n(&subsystem->state, &actual_old_state, state, false,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED);
	if (spdk_unlikely(exchanged == false)) {
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_RESUMING &&
		    state == SPDK_NVMF_SUBSYSTEM_ACTIVE) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_RESUMING;
		}
		/* This is for the case when activating the subsystem fails. */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_ACTIVATING &&
		    state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_ACTIVATING;
		}
		/* This is for the case when resuming the subsystem fails. */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_RESUMING &&
		    state == SPDK_NVMF_SUBSYSTEM_PAUSING) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_RESUMING;
		}
		/* This is for the case when stopping paused subsystem */
		if (actual_old_state == SPDK_NVMF_SUBSYSTEM_PAUSED &&
		    state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING) {
			expected_old_state = SPDK_NVMF_SUBSYSTEM_PAUSED;
		}
		actual_old_state = expected_old_state;
		__atomic_compare_exchange_n(&subsystem->state, &actual_old_state, state, false,
					    __ATOMIC_RELAXED, __ATOMIC_RELAXED);
	}
	assert(actual_old_state == expected_old_state);
	return actual_old_state - expected_old_state;
}

struct subsystem_state_change_ctx {
	struct spdk_nvmf_subsystem		*subsystem;
	uint16_t				nsid;

	enum spdk_nvmf_subsystem_state		original_state;
	enum spdk_nvmf_subsystem_state		requested_state;

	spdk_nvmf_subsystem_state_change_done	cb_fn;
	void					*cb_arg;
};

static void
subsystem_state_change_revert_done(struct spdk_io_channel_iter *i, int status)
{
	struct subsystem_state_change_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	/* Nothing to be done here if the state setting fails, we are just screwed. */
	if (nvmf_subsystem_set_state(ctx->subsystem, ctx->requested_state)) {
		SPDK_ERRLOG("Unable to revert the subsystem state after operation failure.\n");
	}

	ctx->subsystem->changing_state = false;
	if (ctx->cb_fn) {
		/* return a failure here. This function only exists in an error path. */
		ctx->cb_fn(ctx->subsystem, ctx->cb_arg, -1);
	}
	free(ctx);
}

static void
subsystem_state_change_done(struct spdk_io_channel_iter *i, int status)
{
	struct subsystem_state_change_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	enum spdk_nvmf_subsystem_state intermediate_state;

	SPDK_DTRACE_PROBE4(nvmf_subsystem_change_state_done, ctx->subsystem->subnqn,
			   ctx->requested_state, ctx->original_state, status);

	if (status == 0) {
		status = nvmf_subsystem_set_state(ctx->subsystem, ctx->requested_state);
		if (status) {
			status = -1;
		}
	}

	if (status) {
		intermediate_state = nvmf_subsystem_get_intermediate_state(ctx->requested_state,
				     ctx->original_state);
		assert(intermediate_state != SPDK_NVMF_SUBSYSTEM_NUM_STATES);

		if (nvmf_subsystem_set_state(ctx->subsystem, intermediate_state)) {
			goto out;
		}
		ctx->requested_state = ctx->original_state;
		spdk_for_each_channel(ctx->subsystem->tgt,
				      subsystem_state_change_on_pg,
				      ctx,
				      subsystem_state_change_revert_done);
		return;
	}

out:
	ctx->subsystem->changing_state = false;
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->subsystem, ctx->cb_arg, status);
	}
	free(ctx);
}

static void
subsystem_state_change_continue(void *ctx, int status)
{
	struct spdk_io_channel_iter *i = ctx;
	struct subsystem_state_change_ctx *_ctx __attribute__((unused));

	_ctx = spdk_io_channel_iter_get_ctx(i);
	SPDK_DTRACE_PROBE3(nvmf_pg_change_state_done, _ctx->subsystem->subnqn,
			   _ctx->requested_state, spdk_thread_get_id(spdk_get_thread()));

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

	SPDK_DTRACE_PROBE3(nvmf_pg_change_state, ctx->subsystem->subnqn,
			   ctx->requested_state, spdk_thread_get_id(spdk_get_thread()));
	switch (ctx->requested_state) {
	case SPDK_NVMF_SUBSYSTEM_INACTIVE:
		nvmf_poll_group_remove_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		break;
	case SPDK_NVMF_SUBSYSTEM_ACTIVE:
		if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_ACTIVATING) {
			nvmf_poll_group_add_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		} else if (ctx->subsystem->state == SPDK_NVMF_SUBSYSTEM_RESUMING) {
			nvmf_poll_group_resume_subsystem(group, ctx->subsystem, subsystem_state_change_continue, i);
		}
		break;
	case SPDK_NVMF_SUBSYSTEM_PAUSED:
		nvmf_poll_group_pause_subsystem(group, ctx->subsystem, ctx->nsid, subsystem_state_change_continue,
						i);
		break;
	default:
		assert(false);
		break;
	}
}

static int
nvmf_subsystem_state_change(struct spdk_nvmf_subsystem *subsystem,
			    uint32_t nsid,
			    enum spdk_nvmf_subsystem_state requested_state,
			    spdk_nvmf_subsystem_state_change_done cb_fn,
			    void *cb_arg)
{
	struct subsystem_state_change_ctx *ctx;
	enum spdk_nvmf_subsystem_state intermediate_state;
	int rc;

	if (__sync_val_compare_and_swap(&subsystem->changing_state, false, true)) {
		return -EBUSY;
	}

	SPDK_DTRACE_PROBE3(nvmf_subsystem_change_state, subsystem->subnqn,
			   requested_state, subsystem->state);
	/* If we are already in the requested state, just call the callback immediately. */
	if (subsystem->state == requested_state) {
		subsystem->changing_state = false;
		if (cb_fn) {
			cb_fn(subsystem, cb_arg, 0);
		}
		return 0;
	}

	intermediate_state = nvmf_subsystem_get_intermediate_state(subsystem->state, requested_state);
	assert(intermediate_state != SPDK_NVMF_SUBSYSTEM_NUM_STATES);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		subsystem->changing_state = false;
		return -ENOMEM;
	}

	ctx->original_state = subsystem->state;
	rc = nvmf_subsystem_set_state(subsystem, intermediate_state);
	if (rc) {
		free(ctx);
		subsystem->changing_state = false;
		return rc;
	}

	ctx->subsystem = subsystem;
	ctx->nsid = nsid;
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
	return nvmf_subsystem_state_change(subsystem, 0, SPDK_NVMF_SUBSYSTEM_ACTIVE, cb_fn, cb_arg);
}

int
spdk_nvmf_subsystem_stop(struct spdk_nvmf_subsystem *subsystem,
			 spdk_nvmf_subsystem_state_change_done cb_fn,
			 void *cb_arg)
{
	return nvmf_subsystem_state_change(subsystem, 0, SPDK_NVMF_SUBSYSTEM_INACTIVE, cb_fn, cb_arg);
}

int
spdk_nvmf_subsystem_pause(struct spdk_nvmf_subsystem *subsystem,
			  uint32_t nsid,
			  spdk_nvmf_subsystem_state_change_done cb_fn,
			  void *cb_arg)
{
	return nvmf_subsystem_state_change(subsystem, nsid, SPDK_NVMF_SUBSYSTEM_PAUSED, cb_fn, cb_arg);
}

int
spdk_nvmf_subsystem_resume(struct spdk_nvmf_subsystem *subsystem,
			   spdk_nvmf_subsystem_state_change_done cb_fn,
			   void *cb_arg)
{
	return nvmf_subsystem_state_change(subsystem, 0, SPDK_NVMF_SUBSYSTEM_ACTIVE, cb_fn, cb_arg);
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

/* Must hold subsystem->mutex while calling this function */
static struct spdk_nvmf_host *
nvmf_subsystem_find_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
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

	if (!nvmf_valid_nqn(hostnqn)) {
		return -EINVAL;
	}

	pthread_mutex_lock(&subsystem->mutex);

	if (nvmf_subsystem_find_host(subsystem, hostnqn)) {
		/* This subsystem already allows the specified host. */
		pthread_mutex_unlock(&subsystem->mutex);
		return 0;
	}

	host = calloc(1, sizeof(*host));
	if (!host) {
		pthread_mutex_unlock(&subsystem->mutex);
		return -ENOMEM;
	}

	snprintf(host->nqn, sizeof(host->nqn), "%s", hostnqn);

	SPDK_DTRACE_PROBE2(nvmf_subsystem_add_host, subsystem->subnqn, host->nqn);

	TAILQ_INSERT_HEAD(&subsystem->hosts, host, link);

	if (!TAILQ_EMPTY(&subsystem->listeners)) {
		nvmf_update_discovery_log(subsystem->tgt, hostnqn);
	}

	pthread_mutex_unlock(&subsystem->mutex);

	return 0;
}

int
spdk_nvmf_subsystem_remove_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	struct spdk_nvmf_host *host;

	pthread_mutex_lock(&subsystem->mutex);

	host = nvmf_subsystem_find_host(subsystem, hostnqn);
	if (host == NULL) {
		pthread_mutex_unlock(&subsystem->mutex);
		return -ENOENT;
	}

	SPDK_DTRACE_PROBE2(nvmf_subsystem_remove_host, subsystem->subnqn, host->nqn);

	nvmf_subsystem_remove_host(subsystem, host);

	if (!TAILQ_EMPTY(&subsystem->listeners)) {
		nvmf_update_discovery_log(subsystem->tgt, hostnqn);
	}

	pthread_mutex_unlock(&subsystem->mutex);

	return 0;
}

struct nvmf_subsystem_disconnect_host_ctx {
	struct spdk_nvmf_subsystem		*subsystem;
	char					*hostnqn;
	spdk_nvmf_tgt_subsystem_listen_done_fn	cb_fn;
	void					*cb_arg;
};

static void
nvmf_subsystem_disconnect_host_fini(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;

	ctx = spdk_io_channel_iter_get_ctx(i);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status);
	}
	free(ctx->hostnqn);
	free(ctx);
}

static void
nvmf_subsystem_disconnect_qpairs_by_host(struct spdk_io_channel_iter *i)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;
	struct spdk_nvmf_poll_group *group;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_qpair *qpair, *tmp_qpair;
	struct spdk_nvmf_ctrlr *ctrlr;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp_qpair) {
		ctrlr = qpair->ctrlr;

		if (ctrlr == NULL || ctrlr->subsys != ctx->subsystem) {
			continue;
		}

		if (strncmp(ctrlr->hostnqn, ctx->hostnqn, sizeof(ctrlr->hostnqn)) == 0) {
			/* Right now this does not wait for the queue pairs to actually disconnect. */
			spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		}
	}
	spdk_for_each_channel_continue(i, 0);
}

int
spdk_nvmf_subsystem_disconnect_host(struct spdk_nvmf_subsystem *subsystem,
				    const char *hostnqn,
				    spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				    void *cb_arg)
{
	struct nvmf_subsystem_disconnect_host_ctx *ctx;

	ctx = calloc(1, sizeof(struct nvmf_subsystem_disconnect_host_ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->hostnqn = strdup(hostnqn);
	if (ctx->hostnqn == NULL) {
		free(ctx);
		return -ENOMEM;
	}

	ctx->subsystem = subsystem;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(subsystem->tgt, nvmf_subsystem_disconnect_qpairs_by_host, ctx,
			      nvmf_subsystem_disconnect_host_fini);

	return 0;
}

int
spdk_nvmf_subsystem_set_allow_any_host(struct spdk_nvmf_subsystem *subsystem, bool allow_any_host)
{
	pthread_mutex_lock(&subsystem->mutex);
	subsystem->flags.allow_any_host = allow_any_host;
	if (!TAILQ_EMPTY(&subsystem->listeners)) {
		nvmf_update_discovery_log(subsystem->tgt, NULL);
	}
	pthread_mutex_unlock(&subsystem->mutex);

	return 0;
}

bool
spdk_nvmf_subsystem_get_allow_any_host(const struct spdk_nvmf_subsystem *subsystem)
{
	bool allow_any_host;
	struct spdk_nvmf_subsystem *sub;

	/* Technically, taking the mutex modifies data in the subsystem. But the const
	 * is still important to convey that this doesn't mutate any other data. Cast
	 * it away to work around this. */
	sub = (struct spdk_nvmf_subsystem *)subsystem;

	pthread_mutex_lock(&sub->mutex);
	allow_any_host = sub->flags.allow_any_host;
	pthread_mutex_unlock(&sub->mutex);

	return allow_any_host;
}

bool
spdk_nvmf_subsystem_host_allowed(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{
	bool allowed;

	if (!hostnqn) {
		return false;
	}

	pthread_mutex_lock(&subsystem->mutex);

	if (subsystem->flags.allow_any_host) {
		pthread_mutex_unlock(&subsystem->mutex);
		return true;
	}

	allowed =  nvmf_subsystem_find_host(subsystem, hostnqn) != NULL;
	pthread_mutex_unlock(&subsystem->mutex);

	return allowed;
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
spdk_nvmf_host_get_nqn(const struct spdk_nvmf_host *host)
{
	return host->nqn;
}

struct spdk_nvmf_subsystem_listener *
nvmf_subsystem_find_listener(struct spdk_nvmf_subsystem *subsystem,
			     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (spdk_nvme_transport_id_compare(listener->trid, trid) == 0) {
			return listener;
		}
	}

	return NULL;
}

/**
 * Function to be called once the target is listening.
 *
 * \param ctx Context argument passed to this function.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
static void
_nvmf_subsystem_add_listener_done(void *ctx, int status)
{
	struct spdk_nvmf_subsystem_listener *listener = ctx;

	if (status) {
		listener->cb_fn(listener->cb_arg, status);
		free(listener);
		return;
	}

	TAILQ_INSERT_HEAD(&listener->subsystem->listeners, listener, link);
	nvmf_update_discovery_log(listener->subsystem->tgt, NULL);
	listener->cb_fn(listener->cb_arg, status);
}

void
spdk_nvmf_subsystem_add_listener(struct spdk_nvmf_subsystem *subsystem,
				 struct spdk_nvme_transport_id *trid,
				 spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				 void *cb_arg)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_listener *tr_listener;
	uint32_t i;
	uint32_t id;
	int rc = 0;

	assert(cb_fn != NULL);

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		cb_fn(cb_arg, -EAGAIN);
		return;
	}

	if (nvmf_subsystem_find_listener(subsystem, trid)) {
		/* Listener already exists in this subsystem */
		cb_fn(cb_arg, 0);
		return;
	}

	transport = spdk_nvmf_tgt_get_transport(subsystem->tgt, trid->trstring);
	if (!transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    trid->trstring);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	tr_listener = nvmf_transport_find_listener(transport, trid);
	if (!tr_listener) {
		SPDK_ERRLOG("Cannot find transport listener for %s\n", trid->traddr);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	listener = calloc(1, sizeof(*listener));
	if (!listener) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	listener->trid = &tr_listener->trid;
	listener->transport = transport;
	listener->cb_fn = cb_fn;
	listener->cb_arg = cb_arg;
	listener->subsystem = subsystem;
	listener->ana_state = calloc(subsystem->max_nsid, sizeof(enum spdk_nvme_ana_state));
	if (!listener->ana_state) {
		free(listener);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	id = spdk_bit_array_find_first_clear(subsystem->used_listener_ids, 0);
	if (id == UINT32_MAX) {
		SPDK_ERRLOG("Cannot add any more listeners\n");
		free(listener->ana_state);
		free(listener);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	spdk_bit_array_set(subsystem->used_listener_ids, id);
	listener->id = id;

	for (i = 0; i < subsystem->max_nsid; i++) {
		listener->ana_state[i] = SPDK_NVME_ANA_OPTIMIZED_STATE;
	}

	if (transport->ops->listen_associate != NULL) {
		rc = transport->ops->listen_associate(transport, subsystem, trid);
	}

	SPDK_DTRACE_PROBE4(nvmf_subsystem_add_listener, subsystem->subnqn, listener->trid->trtype,
			   listener->trid->traddr, listener->trid->trsvcid);

	_nvmf_subsystem_add_listener_done(listener, rc);
}

int
spdk_nvmf_subsystem_remove_listener(struct spdk_nvmf_subsystem *subsystem,
				    const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return -EAGAIN;
	}

	listener = nvmf_subsystem_find_listener(subsystem, trid);
	if (listener == NULL) {
		return -ENOENT;
	}

	SPDK_DTRACE_PROBE4(nvmf_subsystem_remove_listener, subsystem->subnqn, listener->trid->trtype,
			   listener->trid->traddr, listener->trid->trsvcid);

	_nvmf_subsystem_remove_listener(subsystem, listener, false);

	return 0;
}

void
nvmf_subsystem_remove_all_listeners(struct spdk_nvmf_subsystem *subsystem,
				    bool stop)
{
	struct spdk_nvmf_subsystem_listener *listener, *listener_tmp;

	TAILQ_FOREACH_SAFE(listener, &subsystem->listeners, link, listener_tmp) {
		_nvmf_subsystem_remove_listener(subsystem, listener, stop);
	}
}

bool
spdk_nvmf_subsystem_listener_allowed(struct spdk_nvmf_subsystem *subsystem,
				     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_subsystem_listener *listener;

	if (!strcmp(subsystem->subnqn, SPDK_NVMF_DISCOVERY_NQN)) {
		return true;
	}

	TAILQ_FOREACH(listener, &subsystem->listeners, link) {
		if (spdk_nvme_transport_id_compare(listener->trid, trid) == 0) {
			return true;
		}
	}

	return false;
}

struct spdk_nvmf_subsystem_listener *
spdk_nvmf_subsystem_get_first_listener(struct spdk_nvmf_subsystem *subsystem)
{
	return TAILQ_FIRST(&subsystem->listeners);
}

struct spdk_nvmf_subsystem_listener *
spdk_nvmf_subsystem_get_next_listener(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_subsystem_listener *prev_listener)
{
	return TAILQ_NEXT(prev_listener, link);
}

const struct spdk_nvme_transport_id *
spdk_nvmf_subsystem_listener_get_trid(struct spdk_nvmf_subsystem_listener *listener)
{
	return listener->trid;
}

void
spdk_nvmf_subsystem_allow_any_listener(struct spdk_nvmf_subsystem *subsystem,
				       bool allow_any_listener)
{
	subsystem->flags.allow_any_listener = allow_any_listener;
}

bool
spdk_nvmf_subsytem_any_listener_allowed(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->flags.allow_any_listener;
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

	rc = nvmf_poll_group_update_subsystem(group, subsystem);
	spdk_for_each_channel_continue(i, rc);
}

static int
nvmf_subsystem_update_ns(struct spdk_nvmf_subsystem *subsystem, spdk_channel_for_each_cpl cpl,
			 void *ctx)
{
	spdk_for_each_channel(subsystem->tgt,
			      subsystem_update_ns_on_pg,
			      ctx,
			      cpl);

	return 0;
}

static void
nvmf_subsystem_ns_changed(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ctrlr *ctrlr;

	TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
		nvmf_ctrlr_ns_changed(ctrlr, nsid);
	}
}

static uint32_t
nvmf_ns_reservation_clear_all_registrants(struct spdk_nvmf_ns *ns);

int
spdk_nvmf_subsystem_remove_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_ns *ns;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		assert(false);
		return -1;
	}

	if (nsid == 0 || nsid > subsystem->max_nsid) {
		return -1;
	}

	ns = subsystem->ns[nsid - 1];
	if (!ns) {
		return -1;
	}

	subsystem->ns[nsid - 1] = NULL;

	assert(ns->anagrpid - 1 < subsystem->max_nsid);
	assert(subsystem->ana_group[ns->anagrpid - 1] > 0);

	subsystem->ana_group[ns->anagrpid - 1]--;

	free(ns->ptpl_file);
	nvmf_ns_reservation_clear_all_registrants(ns);
	spdk_bdev_module_release_bdev(ns->bdev);
	spdk_bdev_close(ns->desc);
	free(ns);

	for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
	     transport = spdk_nvmf_transport_get_next(transport)) {
		if (transport->ops->subsystem_remove_ns) {
			transport->ops->subsystem_remove_ns(transport, subsystem, nsid);
		}
	}

	nvmf_subsystem_ns_changed(subsystem, nsid);

	return 0;
}

struct subsystem_ns_change_ctx {
	struct spdk_nvmf_subsystem		*subsystem;
	spdk_nvmf_subsystem_state_change_done	cb_fn;
	uint32_t				nsid;
};

static void
_nvmf_ns_hot_remove(struct spdk_nvmf_subsystem *subsystem,
		    void *cb_arg, int status)
{
	struct subsystem_ns_change_ctx *ctx = cb_arg;
	int rc;

	rc = spdk_nvmf_subsystem_remove_ns(subsystem, ctx->nsid);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to make changes to NVME-oF subsystem with id: %u\n", subsystem->id);
	}

	spdk_nvmf_subsystem_resume(subsystem, NULL, NULL);

	free(ctx);
}

static void
nvmf_ns_change_msg(void *ns_ctx)
{
	struct subsystem_ns_change_ctx *ctx = ns_ctx;
	int rc;

	SPDK_DTRACE_PROBE2(nvmf_ns_change, ctx->nsid, ctx->subsystem->subnqn);

	rc = spdk_nvmf_subsystem_pause(ctx->subsystem, ctx->nsid, ctx->cb_fn, ctx);
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ctx);
		} else {
			free(ctx);
			SPDK_ERRLOG("Unable to pause subsystem to process namespace removal!\n");
		}
	}
}

static void
nvmf_ns_hot_remove(void *remove_ctx)
{
	struct spdk_nvmf_ns *ns = remove_ctx;
	struct subsystem_ns_change_ctx *ns_ctx;
	int rc;

	/* We have to allocate a new context because this op
	 * is asynchronous and we could lose the ns in the middle.
	 */
	ns_ctx = calloc(1, sizeof(struct subsystem_ns_change_ctx));
	if (!ns_ctx) {
		SPDK_ERRLOG("Unable to allocate context to process namespace removal!\n");
		return;
	}

	ns_ctx->subsystem = ns->subsystem;
	ns_ctx->nsid = ns->opts.nsid;
	ns_ctx->cb_fn = _nvmf_ns_hot_remove;

	rc = spdk_nvmf_subsystem_pause(ns->subsystem, ns_ctx->nsid, _nvmf_ns_hot_remove, ns_ctx);
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ns_ctx);
		} else {
			SPDK_ERRLOG("Unable to pause subsystem to process namespace removal!\n");
			free(ns_ctx);
		}
	}
}

static void
_nvmf_ns_resize(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	struct subsystem_ns_change_ctx *ctx = cb_arg;

	nvmf_subsystem_ns_changed(subsystem, ctx->nsid);
	spdk_nvmf_subsystem_resume(subsystem, NULL, NULL);

	free(ctx);
}

static void
nvmf_ns_resize(void *event_ctx)
{
	struct spdk_nvmf_ns *ns = event_ctx;
	struct subsystem_ns_change_ctx *ns_ctx;
	int rc;

	/* We have to allocate a new context because this op
	 * is asynchronous and we could lose the ns in the middle.
	 */
	ns_ctx = calloc(1, sizeof(struct subsystem_ns_change_ctx));
	if (!ns_ctx) {
		SPDK_ERRLOG("Unable to allocate context to process namespace removal!\n");
		return;
	}

	ns_ctx->subsystem = ns->subsystem;
	ns_ctx->nsid = ns->opts.nsid;
	ns_ctx->cb_fn = _nvmf_ns_resize;

	/* Specify 0 for the nsid here, because we do not need to pause the namespace.
	 * Namespaces can only be resized bigger, so there is no need to quiesce I/O.
	 */
	rc = spdk_nvmf_subsystem_pause(ns->subsystem, 0, _nvmf_ns_resize, ns_ctx);
	if (rc) {
		if (rc == -EBUSY) {
			/* Try again, this is not a permanent situation. */
			spdk_thread_send_msg(spdk_get_thread(), nvmf_ns_change_msg, ns_ctx);
		} else {
			SPDK_ERRLOG("Unable to pause subsystem to process namespace resize!\n");
			free(ns_ctx);
		}
	}
}

static void
nvmf_ns_event(enum spdk_bdev_event_type type,
	      struct spdk_bdev *bdev,
	      void *event_ctx)
{
	SPDK_DEBUGLOG(nvmf, "Bdev event: type %d, name %s, subsystem_id %d, ns_id %d\n",
		      type,
		      spdk_bdev_get_name(bdev),
		      ((struct spdk_nvmf_ns *)event_ctx)->subsystem->id,
		      ((struct spdk_nvmf_ns *)event_ctx)->nsid);

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		nvmf_ns_hot_remove(event_ctx);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		nvmf_ns_resize(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

void
spdk_nvmf_ns_opts_get_defaults(struct spdk_nvmf_ns_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL.\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero.\n");
		return;
	}

	memset(opts, 0, opts_size);
	opts->opts_size = opts_size;

#define FIELD_OK(field) \
	offsetof(struct spdk_nvmf_ns_opts, field) + sizeof(opts->field) <= opts_size

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		opts->field = value; \
	} \

	/* All current fields are set to 0 by default. */
	SET_FIELD(nsid, 0);
	if (FIELD_OK(nguid)) {
		memset(opts->nguid, 0, sizeof(opts->nguid));
	}
	if (FIELD_OK(eui64)) {
		memset(opts->eui64, 0, sizeof(opts->eui64));
	}
	if (FIELD_OK(uuid)) {
		memset(&opts->uuid, 0, sizeof(opts->uuid));
	}
	SET_FIELD(anagrpid, 0);

#undef FIELD_OK
#undef SET_FIELD
}

static void
nvmf_ns_opts_copy(struct spdk_nvmf_ns_opts *opts,
		  const struct spdk_nvmf_ns_opts *user_opts,
		  size_t opts_size)
{
#define FIELD_OK(field)	\
	offsetof(struct spdk_nvmf_ns_opts, field) + sizeof(opts->field) <= user_opts->opts_size

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		opts->field = user_opts->field;	\
	} \

	SET_FIELD(nsid);
	if (FIELD_OK(nguid)) {
		memcpy(opts->nguid, user_opts->nguid, sizeof(opts->nguid));
	}
	if (FIELD_OK(eui64)) {
		memcpy(opts->eui64, user_opts->eui64, sizeof(opts->eui64));
	}
	if (FIELD_OK(uuid)) {
		memcpy(&opts->uuid, &user_opts->uuid, sizeof(opts->uuid));
	}
	SET_FIELD(anagrpid);

	opts->opts_size = user_opts->opts_size;

	/* We should not remove this statement, but need to update the assert statement
	 * if we add a new field, and also add a corresponding SET_FIELD statement.
	 */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ns_opts) == 64, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
}

/* Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module ns_bdev_module = {
	.name	= "NVMe-oF Target",
};

static int
nvmf_ns_load_reservation(const char *file, struct spdk_nvmf_reservation_info *info);
static int
nvmf_ns_reservation_restore(struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info);

uint32_t
spdk_nvmf_subsystem_add_ns_ext(struct spdk_nvmf_subsystem *subsystem, const char *bdev_name,
			       const struct spdk_nvmf_ns_opts *user_opts, size_t opts_size,
			       const char *ptpl_file)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_ns_opts opts;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_reservation_info info = {0};
	int rc;

	if (!(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	      subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED)) {
		return 0;
	}

	spdk_nvmf_ns_opts_get_defaults(&opts, sizeof(opts));
	if (user_opts) {
		nvmf_ns_opts_copy(&opts, user_opts, opts_size);
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
			if (_nvmf_subsystem_get_ns(subsystem, opts.nsid) == NULL) {
				break;
			}
		}
	}

	if (_nvmf_subsystem_get_ns(subsystem, opts.nsid)) {
		SPDK_ERRLOG("Requested NSID %" PRIu32 " already in use\n", opts.nsid);
		return 0;
	}

	if (opts.nsid > subsystem->max_nsid) {
		SPDK_ERRLOG("NSID greater than maximum not allowed\n");
		return 0;
	}

	if (opts.anagrpid == 0) {
		opts.anagrpid = opts.nsid;
	}

	if (opts.anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANAGRPID greater than maximum NSID not allowed\n");
		return 0;
	}

	ns = calloc(1, sizeof(*ns));
	if (ns == NULL) {
		SPDK_ERRLOG("Namespace allocation failed\n");
		return 0;
	}

	rc = spdk_bdev_open_ext(bdev_name, true, nvmf_ns_event, ns, &ns->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Subsystem %s: bdev %s cannot be opened, error=%d\n",
			    subsystem->subnqn, bdev_name, rc);
		free(ns);
		return 0;
	}

	ns->bdev = spdk_bdev_desc_get_bdev(ns->desc);

	if (spdk_bdev_get_md_size(ns->bdev) != 0 && !spdk_bdev_is_md_interleaved(ns->bdev)) {
		SPDK_ERRLOG("Can't attach bdev with separate metadata.\n");
		spdk_bdev_close(ns->desc);
		free(ns);
		return 0;
	}

	rc = spdk_bdev_module_claim_bdev(ns->bdev, ns->desc, &ns_bdev_module);
	if (rc != 0) {
		spdk_bdev_close(ns->desc);
		free(ns);
		return 0;
	}

	/* Cache the zcopy capability of the bdev device */
	ns->zcopy = spdk_bdev_io_type_supported(ns->bdev, SPDK_BDEV_IO_TYPE_ZCOPY);

	if (spdk_mem_all_zero(&opts.uuid, sizeof(opts.uuid))) {
		opts.uuid = *spdk_bdev_get_uuid(ns->bdev);
	}

	/* if nguid descriptor is supported by bdev module (nvme) then uuid = nguid */
	if (spdk_mem_all_zero(opts.nguid, sizeof(opts.nguid))) {
		SPDK_STATIC_ASSERT(sizeof(opts.nguid) == sizeof(opts.uuid), "size mismatch");
		memcpy(opts.nguid, spdk_bdev_get_uuid(ns->bdev), sizeof(opts.nguid));
	}

	ns->opts = opts;
	ns->subsystem = subsystem;
	subsystem->ns[opts.nsid - 1] = ns;
	ns->nsid = opts.nsid;
	ns->anagrpid = opts.anagrpid;
	subsystem->ana_group[ns->anagrpid - 1]++;
	TAILQ_INIT(&ns->registrants);
	if (ptpl_file) {
		rc = nvmf_ns_load_reservation(ptpl_file, &info);
		if (!rc) {
			rc = nvmf_ns_reservation_restore(ns, &info);
			if (rc) {
				SPDK_ERRLOG("Subsystem restore reservation failed\n");
				goto err_ns_reservation_restore;
			}
		}
		ns->ptpl_file = strdup(ptpl_file);
		if (!ns->ptpl_file) {
			SPDK_ERRLOG("Namespace ns->ptpl_file allocation failed\n");
			goto err_strdup;
		}
	}

	for (transport = spdk_nvmf_transport_get_first(subsystem->tgt); transport;
	     transport = spdk_nvmf_transport_get_next(transport)) {
		if (transport->ops->subsystem_add_ns) {
			rc = transport->ops->subsystem_add_ns(transport, subsystem, ns);
			if (rc) {
				SPDK_ERRLOG("Namespace attachment is not allowed by %s transport\n", transport->ops->name);
				goto err_subsystem_add_ns;
			}
		}
	}

	SPDK_DEBUGLOG(nvmf, "Subsystem %s: bdev %s assigned nsid %" PRIu32 "\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem),
		      bdev_name,
		      opts.nsid);

	nvmf_subsystem_ns_changed(subsystem, opts.nsid);

	SPDK_DTRACE_PROBE2(nvmf_subsystem_add_ns, subsystem->subnqn, ns->nsid);

	return opts.nsid;

err_subsystem_add_ns:
	free(ns->ptpl_file);
err_strdup:
	nvmf_ns_reservation_clear_all_registrants(ns);
err_ns_reservation_restore:
	subsystem->ns[opts.nsid - 1] = NULL;
	spdk_bdev_module_release_bdev(ns->bdev);
	spdk_bdev_close(ns->desc);
	free(ns);

	return 0;
}

static uint32_t
nvmf_subsystem_get_next_allocated_nsid(struct spdk_nvmf_subsystem *subsystem,
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

	first_nsid = nvmf_subsystem_get_next_allocated_nsid(subsystem, 0);
	return _nvmf_subsystem_get_ns(subsystem, first_nsid);
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_next_ns(struct spdk_nvmf_subsystem *subsystem,
				struct spdk_nvmf_ns *prev_ns)
{
	uint32_t next_nsid;

	next_nsid = nvmf_subsystem_get_next_allocated_nsid(subsystem, prev_ns->opts.nsid);
	return _nvmf_subsystem_get_ns(subsystem, next_nsid);
}

struct spdk_nvmf_ns *
spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	return _nvmf_subsystem_get_ns(subsystem, nsid);
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
		SPDK_DEBUGLOG(nvmf, "Invalid sn \"%s\": length %zu > max %zu\n",
			      sn, len, max_len);
		return -1;
	}

	if (!nvmf_valid_ascii_string(sn, len)) {
		SPDK_DEBUGLOG(nvmf, "Non-ASCII sn\n");
		SPDK_LOGDUMP(nvmf, "sn", sn, len);
		return -1;
	}

	snprintf(subsystem->sn, sizeof(subsystem->sn), "%s", sn);

	return 0;
}

const char *
spdk_nvmf_subsystem_get_mn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->mn;
}

int
spdk_nvmf_subsystem_set_mn(struct spdk_nvmf_subsystem *subsystem, const char *mn)
{
	size_t len, max_len;

	if (mn == NULL) {
		mn = MODEL_NUMBER_DEFAULT;
	}
	max_len = sizeof(subsystem->mn) - 1;
	len = strlen(mn);
	if (len > max_len) {
		SPDK_DEBUGLOG(nvmf, "Invalid mn \"%s\": length %zu > max %zu\n",
			      mn, len, max_len);
		return -1;
	}

	if (!nvmf_valid_ascii_string(mn, len)) {
		SPDK_DEBUGLOG(nvmf, "Non-ASCII mn\n");
		SPDK_LOGDUMP(nvmf, "mn", mn, len);
		return -1;
	}

	snprintf(subsystem->mn, sizeof(subsystem->mn), "%s", mn);

	return 0;
}

const char *
spdk_nvmf_subsystem_get_nqn(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subnqn;
}

enum spdk_nvmf_subtype spdk_nvmf_subsystem_get_type(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->subtype;
}

uint32_t
spdk_nvmf_subsystem_get_max_nsid(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_nsid;
}

int
nvmf_subsystem_set_cntlid_range(struct spdk_nvmf_subsystem *subsystem,
				uint16_t min_cntlid, uint16_t max_cntlid)
{
	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		return -EAGAIN;
	}

	if (min_cntlid > max_cntlid) {
		return -EINVAL;
	}
	/* The spec reserves cntlid values in the range FFF0h to FFFFh. */
	if (min_cntlid < NVMF_MIN_CNTLID || min_cntlid > NVMF_MAX_CNTLID ||
	    max_cntlid < NVMF_MIN_CNTLID || max_cntlid > NVMF_MAX_CNTLID) {
		return -EINVAL;
	}
	subsystem->min_cntlid = min_cntlid;
	subsystem->max_cntlid = max_cntlid;
	if (subsystem->next_cntlid < min_cntlid || subsystem->next_cntlid > max_cntlid - 1) {
		subsystem->next_cntlid = min_cntlid - 1;
	}

	return 0;
}

static uint16_t
nvmf_subsystem_gen_cntlid(struct spdk_nvmf_subsystem *subsystem)
{
	int count;

	/*
	 * In the worst case, we might have to try all CNTLID values between min_cntlid and max_cntlid
	 * before we find one that is unused (or find that all values are in use).
	 */
	for (count = 0; count < subsystem->max_cntlid - subsystem->min_cntlid + 1; count++) {
		subsystem->next_cntlid++;
		if (subsystem->next_cntlid > subsystem->max_cntlid) {
			subsystem->next_cntlid = subsystem->min_cntlid;
		}

		/* Check if a controller with this cntlid currently exists. */
		if (nvmf_subsystem_get_ctrlr(subsystem, subsystem->next_cntlid) == NULL) {
			/* Found unused cntlid */
			return subsystem->next_cntlid;
		}
	}

	/* All valid cntlid values are in use. */
	return 0xFFFF;
}

int
nvmf_subsystem_add_ctrlr(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ctrlr *ctrlr)
{

	if (ctrlr->dynamic_ctrlr) {
		ctrlr->cntlid = nvmf_subsystem_gen_cntlid(subsystem);
		if (ctrlr->cntlid == 0xFFFF) {
			/* Unable to get a cntlid */
			SPDK_ERRLOG("Reached max simultaneous ctrlrs\n");
			return -EBUSY;
		}
	} else if (nvmf_subsystem_get_ctrlr(subsystem, ctrlr->cntlid) != NULL) {
		SPDK_ERRLOG("Ctrlr with cntlid %u already exist\n", ctrlr->cntlid);
		return -EEXIST;
	}

	TAILQ_INSERT_TAIL(&subsystem->ctrlrs, ctrlr, link);

	SPDK_DTRACE_PROBE3(nvmf_subsystem_add_ctrlr, subsystem->subnqn, ctrlr, ctrlr->hostnqn);

	return 0;
}

void
nvmf_subsystem_remove_ctrlr(struct spdk_nvmf_subsystem *subsystem,
			    struct spdk_nvmf_ctrlr *ctrlr)
{
	SPDK_DTRACE_PROBE3(nvmf_subsystem_remove_ctrlr, subsystem->subnqn, ctrlr, ctrlr->hostnqn);

	assert(spdk_get_thread() == subsystem->thread);
	assert(subsystem == ctrlr->subsys);
	SPDK_DEBUGLOG(nvmf, "remove ctrlr %p id 0x%x from subsys %p %s\n", ctrlr, ctrlr->cntlid, subsystem,
		      subsystem->subnqn);
	TAILQ_REMOVE(&subsystem->ctrlrs, ctrlr, link);
}

struct spdk_nvmf_ctrlr *
nvmf_subsystem_get_ctrlr(struct spdk_nvmf_subsystem *subsystem, uint16_t cntlid)
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
	return subsystem->max_nsid;
}

uint16_t
spdk_nvmf_subsystem_get_min_cntlid(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->min_cntlid;
}

uint16_t
spdk_nvmf_subsystem_get_max_cntlid(const struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->max_cntlid;
}

struct _nvmf_ns_registrant {
	uint64_t		rkey;
	char			*host_uuid;
};

struct _nvmf_ns_registrants {
	size_t				num_regs;
	struct _nvmf_ns_registrant	reg[SPDK_NVMF_MAX_NUM_REGISTRANTS];
};

struct _nvmf_ns_reservation {
	bool					ptpl_activated;
	enum spdk_nvme_reservation_type		rtype;
	uint64_t				crkey;
	char					*bdev_uuid;
	char					*holder_uuid;
	struct _nvmf_ns_registrants		regs;
};

static const struct spdk_json_object_decoder nvmf_ns_pr_reg_decoders[] = {
	{"rkey", offsetof(struct _nvmf_ns_registrant, rkey), spdk_json_decode_uint64},
	{"host_uuid", offsetof(struct _nvmf_ns_registrant, host_uuid), spdk_json_decode_string},
};

static int
nvmf_decode_ns_pr_reg(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrant *reg = out;

	return spdk_json_decode_object(val, nvmf_ns_pr_reg_decoders,
				       SPDK_COUNTOF(nvmf_ns_pr_reg_decoders), reg);
}

static int
nvmf_decode_ns_pr_regs(const struct spdk_json_val *val, void *out)
{
	struct _nvmf_ns_registrants *regs = out;

	return spdk_json_decode_array(val, nvmf_decode_ns_pr_reg, regs->reg,
				      SPDK_NVMF_MAX_NUM_REGISTRANTS, &regs->num_regs,
				      sizeof(struct _nvmf_ns_registrant));
}

static const struct spdk_json_object_decoder nvmf_ns_pr_decoders[] = {
	{"ptpl", offsetof(struct _nvmf_ns_reservation, ptpl_activated), spdk_json_decode_bool, true},
	{"rtype", offsetof(struct _nvmf_ns_reservation, rtype), spdk_json_decode_uint32, true},
	{"crkey", offsetof(struct _nvmf_ns_reservation, crkey), spdk_json_decode_uint64, true},
	{"bdev_uuid", offsetof(struct _nvmf_ns_reservation, bdev_uuid), spdk_json_decode_string},
	{"holder_uuid", offsetof(struct _nvmf_ns_reservation, holder_uuid), spdk_json_decode_string, true},
	{"registrants", offsetof(struct _nvmf_ns_reservation, regs), nvmf_decode_ns_pr_regs},
};

static int
nvmf_ns_load_reservation(const char *file, struct spdk_nvmf_reservation_info *info)
{
	FILE *fd;
	size_t json_size;
	ssize_t values_cnt, rc;
	void *json = NULL, *end;
	struct spdk_json_val *values = NULL;
	struct _nvmf_ns_reservation res = {};
	uint32_t i;

	fd = fopen(file, "r");
	/* It's not an error if the file does not exist */
	if (!fd) {
		SPDK_NOTICELOG("File %s does not exist\n", file);
		return -ENOENT;
	}

	/* Load all persist file contents into a local buffer */
	json = spdk_posix_file_load(fd, &json_size);
	fclose(fd);
	if (!json) {
		SPDK_ERRLOG("Load persit file %s failed\n", file);
		return -ENOMEM;
	}

	rc = spdk_json_parse(json, json_size, NULL, 0, &end, 0);
	if (rc < 0) {
		SPDK_NOTICELOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto exit;
	}

	values_cnt = rc;
	values = calloc(values_cnt, sizeof(struct spdk_json_val));
	if (values == NULL) {
		goto exit;
	}

	rc = spdk_json_parse(json, json_size, values, values_cnt, &end, 0);
	if (rc != values_cnt) {
		SPDK_ERRLOG("Parsing JSON configuration failed (%zd)\n", rc);
		goto exit;
	}

	/* Decode json */
	if (spdk_json_decode_object(values, nvmf_ns_pr_decoders,
				    SPDK_COUNTOF(nvmf_ns_pr_decoders),
				    &res)) {
		SPDK_ERRLOG("Invalid objects in the persist file %s\n", file);
		rc = -EINVAL;
		goto exit;
	}

	if (res.regs.num_regs > SPDK_NVMF_MAX_NUM_REGISTRANTS) {
		SPDK_ERRLOG("Can only support up to %u registrants\n", SPDK_NVMF_MAX_NUM_REGISTRANTS);
		rc = -ERANGE;
		goto exit;
	}

	rc = 0;
	info->ptpl_activated = res.ptpl_activated;
	info->rtype = res.rtype;
	info->crkey = res.crkey;
	snprintf(info->bdev_uuid, sizeof(info->bdev_uuid), "%s", res.bdev_uuid);
	snprintf(info->holder_uuid, sizeof(info->holder_uuid), "%s", res.holder_uuid);
	info->num_regs = res.regs.num_regs;
	for (i = 0; i < res.regs.num_regs; i++) {
		info->registrants[i].rkey = res.regs.reg[i].rkey;
		snprintf(info->registrants[i].host_uuid, sizeof(info->registrants[i].host_uuid), "%s",
			 res.regs.reg[i].host_uuid);
	}

exit:
	free(json);
	free(values);
	free(res.bdev_uuid);
	free(res.holder_uuid);
	for (i = 0; i < res.regs.num_regs; i++) {
		free(res.regs.reg[i].host_uuid);
	}

	return rc;
}

static bool
nvmf_ns_reservation_all_registrants_type(struct spdk_nvmf_ns *ns);

static int
nvmf_ns_reservation_restore(struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info)
{
	uint32_t i;
	struct spdk_nvmf_registrant *reg, *holder = NULL;
	struct spdk_uuid bdev_uuid, holder_uuid;
	bool rkey_flag = false;

	SPDK_DEBUGLOG(nvmf, "NSID %u, PTPL %u, Number of registrants %u\n",
		      ns->nsid, info->ptpl_activated, info->num_regs);

	/* it's not an error */
	if (!info->ptpl_activated || !info->num_regs) {
		return 0;
	}

	/* Check info->crkey exist or not in info->registrants[i].rkey */
	for (i = 0; i < info->num_regs; i++) {
		if (info->crkey == info->registrants[i].rkey) {
			rkey_flag = true;
		}
	}
	if (!rkey_flag) {
		return -EINVAL;
	}

	spdk_uuid_parse(&bdev_uuid, info->bdev_uuid);
	if (spdk_uuid_compare(&bdev_uuid, spdk_bdev_get_uuid(ns->bdev))) {
		SPDK_ERRLOG("Existing bdev UUID is not same with configuration file\n");
		return -EINVAL;
	}

	ns->crkey = info->crkey;
	ns->rtype = info->rtype;
	ns->ptpl_activated = info->ptpl_activated;
	spdk_uuid_parse(&holder_uuid, info->holder_uuid);

	SPDK_DEBUGLOG(nvmf, "Bdev UUID %s\n", info->bdev_uuid);
	if (info->rtype) {
		SPDK_DEBUGLOG(nvmf, "Holder UUID %s, RTYPE %u, RKEY 0x%"PRIx64"\n",
			      info->holder_uuid, info->rtype, info->crkey);
	}

	for (i = 0; i < info->num_regs; i++) {
		reg = calloc(1, sizeof(*reg));
		if (!reg) {
			return -ENOMEM;
		}
		spdk_uuid_parse(&reg->hostid, info->registrants[i].host_uuid);
		reg->rkey = info->registrants[i].rkey;
		TAILQ_INSERT_TAIL(&ns->registrants, reg, link);
		if (!spdk_uuid_compare(&holder_uuid, &reg->hostid)) {
			holder = reg;
		}
		SPDK_DEBUGLOG(nvmf, "Registrant RKEY 0x%"PRIx64", Host UUID %s\n",
			      info->registrants[i].rkey, info->registrants[i].host_uuid);
	}

	if (nvmf_ns_reservation_all_registrants_type(ns)) {
		ns->holder = TAILQ_FIRST(&ns->registrants);
	} else {
		ns->holder = holder;
	}

	return 0;
}

static int
nvmf_ns_json_write_cb(void *cb_ctx, const void *data, size_t size)
{
	char *file = cb_ctx;
	size_t rc;
	FILE *fd;

	fd = fopen(file, "w");
	if (!fd) {
		SPDK_ERRLOG("Can't open file %s for write\n", file);
		return -ENOENT;
	}
	rc = fwrite(data, 1, size, fd);
	fclose(fd);

	return rc == size ? 0 : -1;
}

static int
nvmf_ns_reservation_update(const char *file, struct spdk_nvmf_reservation_info *info)
{
	struct spdk_json_write_ctx *w;
	uint32_t i;
	int rc = 0;

	w = spdk_json_write_begin(nvmf_ns_json_write_cb, (void *)file, 0);
	if (w == NULL) {
		return -ENOMEM;
	}
	/* clear the configuration file */
	if (!info->ptpl_activated) {
		goto exit;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_bool(w, "ptpl", info->ptpl_activated);
	spdk_json_write_named_uint32(w, "rtype", info->rtype);
	spdk_json_write_named_uint64(w, "crkey", info->crkey);
	spdk_json_write_named_string(w, "bdev_uuid", info->bdev_uuid);
	spdk_json_write_named_string(w, "holder_uuid", info->holder_uuid);

	spdk_json_write_named_array_begin(w, "registrants");
	for (i = 0; i < info->num_regs; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "rkey", info->registrants[i].rkey);
		spdk_json_write_named_string(w, "host_uuid", info->registrants[i].host_uuid);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

exit:
	rc = spdk_json_write_end(w);
	return rc;
}

static int
nvmf_ns_update_reservation_info(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_reservation_info info;
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t i = 0;

	assert(ns != NULL);

	if (!ns->bdev || !ns->ptpl_file) {
		return 0;
	}

	memset(&info, 0, sizeof(info));
	spdk_uuid_fmt_lower(info.bdev_uuid, sizeof(info.bdev_uuid), spdk_bdev_get_uuid(ns->bdev));

	if (ns->rtype) {
		info.rtype = ns->rtype;
		info.crkey = ns->crkey;
		if (!nvmf_ns_reservation_all_registrants_type(ns)) {
			assert(ns->holder != NULL);
			spdk_uuid_fmt_lower(info.holder_uuid, sizeof(info.holder_uuid), &ns->holder->hostid);
		}
	}

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		spdk_uuid_fmt_lower(info.registrants[i].host_uuid, sizeof(info.registrants[i].host_uuid),
				    &reg->hostid);
		info.registrants[i++].rkey = reg->rkey;
	}

	info.num_regs = i;
	info.ptpl_activated = ns->ptpl_activated;

	return nvmf_ns_reservation_update(ns->ptpl_file, &info);
}

static struct spdk_nvmf_registrant *
nvmf_ns_reservation_get_registrant(struct spdk_nvmf_ns *ns,
				   struct spdk_uuid *uuid)
{
	struct spdk_nvmf_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		if (!spdk_uuid_compare(&reg->hostid, uuid)) {
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
			if (!spdk_uuid_compare(&ctrlr->hostid, &hostid_list[i])) {
				nvmf_ctrlr_reservation_notice_log(ctrlr, ns, type);
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
			if (num_hostid == max_num_hostid) {
				assert(false);
				return max_num_hostid;
			}
			hostid_list[num_hostid++] = reg->hostid;
		}
	}

	return num_hostid;
}

/* Calculate the unregistered HostID list according to list
 * prior to execute preempt command and list after executing
 * preempt command.
 */
static uint32_t
nvmf_ns_reservation_get_unregistered_hostid(struct spdk_uuid *old_hostid_list,
		uint32_t old_num_hostid,
		struct spdk_uuid *remaining_hostid_list,
		uint32_t remaining_num_hostid)
{
	struct spdk_uuid temp_hostid_list[SPDK_NVMF_MAX_NUM_REGISTRANTS];
	uint32_t i, j, num_hostid = 0;
	bool found;

	if (!remaining_num_hostid) {
		return old_num_hostid;
	}

	for (i = 0; i < old_num_hostid; i++) {
		found = false;
		for (j = 0; j < remaining_num_hostid; j++) {
			if (!spdk_uuid_compare(&old_hostid_list[i], &remaining_hostid_list[j])) {
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

	rrega = cmd->cdw10_bits.resv_register.rrega;
	iekey = cmd->cdw10_bits.resv_register.iekey;
	cptpl = cmd->cdw10_bits.resv_register.cptpl;

	if (req->data && req->length >= sizeof(key)) {
		memcpy(&key, req->data, sizeof(key));
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	SPDK_DEBUGLOG(nvmf, "REGISTER: RREGA %u, IEKEY %u, CPTPL %u, "
		      "NRKEY 0x%"PRIx64", NRKEY 0x%"PRIx64"\n",
		      rrega, iekey, cptpl, key.crkey, key.nrkey);

	if (cptpl == SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON) {
		/* Ture to OFF state, and need to be updated in the configuration file */
		if (ns->ptpl_activated) {
			ns->ptpl_activated = 0;
			update_sgroup = true;
		}
	} else if (cptpl == SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS) {
		if (ns->ptpl_file == NULL) {
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		} else if (ns->ptpl_activated == 0) {
			ns->ptpl_activated = 1;
			update_sgroup = true;
		}
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
			update_sgroup = true;
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
		if (key.nrkey == 0) {
			SPDK_ERRLOG("Can't register zeroed new key\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		}
		/* Registrant exists */
		if (reg) {
			if (!iekey && reg->rkey != key.crkey) {
				SPDK_ERRLOG("Current key doesn't match "
					    "existing registrant key\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
			if (reg->rkey == key.nrkey) {
				goto exit;
			}
			reg->rkey = key.nrkey;
		} else if (iekey) { /* No registrant but IEKEY is set */
			/* new registrant */
			rc = nvmf_ns_reservation_add_registrant(ns, ctrlr, key.nrkey);
			if (rc < 0) {
				status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				goto exit;
			}
		} else { /* No registrant */
			SPDK_ERRLOG("No registrant\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;

		}
		update_sgroup = true;
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

exit:
	if (update_sgroup) {
		rc = nvmf_ns_update_reservation_info(ns);
		if (rc != 0) {
			status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}
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

	racqa = cmd->cdw10_bits.resv_acquire.racqa;
	iekey = cmd->cdw10_bits.resv_acquire.iekey;
	rtype = cmd->cdw10_bits.resv_acquire.rtype;

	if (req->data && req->length >= sizeof(key)) {
		memcpy(&key, req->data, sizeof(key));
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	SPDK_DEBUGLOG(nvmf, "ACQUIRE: RACQA %u, IEKEY %u, RTYPE %u, "
		      "NRKEY 0x%"PRIx64", PRKEY 0x%"PRIx64"\n",
		      racqa, iekey, rtype, key.crkey, key.prkey);

	if (iekey || rtype > SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) {
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
			/* first time to acquire the reservation */
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
		status = SPDK_NVME_SC_INVALID_FIELD;
		update_sgroup = false;
		break;
	}

exit:
	if (update_sgroup && racqa == SPDK_NVME_RESERVE_PREEMPT) {
		new_num_hostid = nvmf_ns_reservation_get_all_other_hostid(ns, new_hostid_list,
				 SPDK_NVMF_MAX_NUM_REGISTRANTS,
				 &ctrlr->hostid);
		/* Preempt notification occurs on the unregistered controllers
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
		/* Reservation released notification occurs on the
		 * controllers which are the remaining registrants other than
		 * the controller who issued the command.
		 */
		if (reservation_released && new_num_hostid) {
			nvmf_subsystem_gen_ctrlr_notification(ns->subsystem, ns,
							      new_hostid_list,
							      new_num_hostid,
							      SPDK_NVME_RESERVATION_RELEASED);

		}
	}
	if (update_sgroup && ns->ptpl_activated) {
		if (nvmf_ns_update_reservation_info(ns)) {
			status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
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

	rrela = cmd->cdw10_bits.resv_release.rrela;
	iekey = cmd->cdw10_bits.resv_release.iekey;
	rtype = cmd->cdw10_bits.resv_release.rtype;

	if (req->data && req->length >= sizeof(crkey)) {
		memcpy(&crkey, req->data, sizeof(crkey));
	} else {
		SPDK_ERRLOG("No key provided. Failing request.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	SPDK_DEBUGLOG(nvmf, "RELEASE: RRELA %u, IEKEY %u, RTYPE %u, "
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
			SPDK_DEBUGLOG(nvmf, "RELEASE: no holder\n");
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
	if (update_sgroup && ns->ptpl_activated) {
		if (nvmf_ns_update_reservation_info(ns)) {
			status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
	}
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
	struct spdk_nvmf_registrant *reg, *tmp;
	struct spdk_nvme_reservation_status_extended_data *status_data;
	struct spdk_nvme_registered_ctrlr_extended_data *ctrlr_data;
	uint8_t *payload;
	uint32_t transfer_len, payload_len = 0;
	uint32_t regctl = 0;
	uint8_t status = SPDK_NVME_SC_SUCCESS;

	if (req->data == NULL) {
		SPDK_ERRLOG("No data transfer specified for request. "
			    " Unable to transfer back response.\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	if (!cmd->cdw11_bits.resv_report.eds) {
		SPDK_ERRLOG("NVMeoF uses extended controller data structure, "
			    "please set EDS bit in cdw11 and try again\n");
		status = SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT;
		goto exit;
	}

	/* Number of Dwords of the Reservation Status data structure to transfer */
	transfer_len = (cmd->cdw10 + 1) * sizeof(uint32_t);
	payload = req->data;

	if (transfer_len < sizeof(struct spdk_nvme_reservation_status_extended_data)) {
		status = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		goto exit;
	}

	status_data = (struct spdk_nvme_reservation_status_extended_data *)payload;
	status_data->data.gen = ns->gen;
	status_data->data.rtype = ns->rtype;
	status_data->data.ptpls = ns->ptpl_activated;
	payload_len += sizeof(struct spdk_nvme_reservation_status_extended_data);

	TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
		payload_len += sizeof(struct spdk_nvme_registered_ctrlr_extended_data);
		if (payload_len > transfer_len) {
			break;
		}

		ctrlr_data = (struct spdk_nvme_registered_ctrlr_extended_data *)
			     (payload + sizeof(*status_data) + sizeof(*ctrlr_data) * regctl);
		/* Set to 0xffffh for dynamic controller */
		ctrlr_data->cntlid = 0xffff;
		ctrlr_data->rcsts.status = (ns->holder == reg) ? true : false;
		ctrlr_data->rkey = reg->rkey;
		spdk_uuid_copy((struct spdk_uuid *)ctrlr_data->hostid, &reg->hostid);
		regctl++;
	}
	status_data->data.regctl = regctl;

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	return;
}

static void
nvmf_ns_reservation_complete(void *ctx)
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

	spdk_thread_send_msg(group->thread, nvmf_ns_reservation_complete, req);
}

void
nvmf_ns_reservation_request(void *ctx)
{
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)ctx;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct subsystem_update_ns_ctx *update_ctx;
	uint32_t nsid;
	struct spdk_nvmf_ns *ns;
	bool update_sgroup = false;

	nsid = cmd->nsid;
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
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

		nvmf_subsystem_update_ns(ctrlr->subsys, subsystem_update_ns_done, update_ctx);
		return;
	}

update_done:
	_nvmf_ns_reservation_update_done(ctrlr->subsys, (void *)req, 0);
}

int
spdk_nvmf_subsystem_set_ana_reporting(struct spdk_nvmf_subsystem *subsystem,
				      bool ana_reporting)
{
	if (subsystem->state != SPDK_NVMF_SUBSYSTEM_INACTIVE) {
		return -EAGAIN;
	}

	subsystem->flags.ana_reporting = ana_reporting;

	return 0;
}

bool
nvmf_subsystem_get_ana_reporting(struct spdk_nvmf_subsystem *subsystem)
{
	return subsystem->flags.ana_reporting;
}

struct subsystem_listener_update_ctx {
	struct spdk_nvmf_subsystem_listener *listener;

	spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn;
	void *cb_arg;
};

static void
subsystem_listener_update_done(struct spdk_io_channel_iter *i, int status)
{
	struct subsystem_listener_update_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, status);
	}
	free(ctx);
}

static void
subsystem_listener_update_on_pg(struct spdk_io_channel_iter *i)
{
	struct subsystem_listener_update_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_ctrlr *ctrlr;

	listener = ctx->listener;
	group = spdk_io_channel_get_ctx(spdk_io_channel_iter_get_channel(i));

	TAILQ_FOREACH(ctrlr, &listener->subsystem->ctrlrs, link) {
		if (ctrlr->admin_qpair && ctrlr->admin_qpair->group == group && ctrlr->listener == listener) {
			nvmf_ctrlr_async_event_ana_change_notice(ctrlr);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

void
nvmf_subsystem_set_ana_state(struct spdk_nvmf_subsystem *subsystem,
			     const struct spdk_nvme_transport_id *trid,
			     enum spdk_nvme_ana_state ana_state, uint32_t anagrpid,
			     spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg)
{
	struct spdk_nvmf_subsystem_listener *listener;
	struct subsystem_listener_update_ctx *ctx;
	uint32_t i;

	assert(cb_fn != NULL);
	assert(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE ||
	       subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED);

	if (!subsystem->flags.ana_reporting) {
		SPDK_ERRLOG("ANA reporting is disabled\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	/* ANA Change state is not used, ANA Persistent Loss state
	 * is not supported yet.
	 */
	if (!(ana_state == SPDK_NVME_ANA_OPTIMIZED_STATE ||
	      ana_state == SPDK_NVME_ANA_NON_OPTIMIZED_STATE ||
	      ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE)) {
		SPDK_ERRLOG("ANA state %d is not supported\n", ana_state);
		cb_fn(cb_arg, -ENOTSUP);
		return;
	}

	if (anagrpid > subsystem->max_nsid) {
		SPDK_ERRLOG("ANA group ID %" PRIu32 " is more than maximum\n", anagrpid);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	listener = nvmf_subsystem_find_listener(subsystem, trid);
	if (!listener) {
		SPDK_ERRLOG("Unable to find listener.\n");
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (anagrpid != 0 && listener->ana_state[anagrpid - 1] == ana_state) {
		cb_fn(cb_arg, 0);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Unable to allocate context\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	for (i = 1; i <= subsystem->max_nsid; i++) {
		if (anagrpid == 0 || i == anagrpid) {
			listener->ana_state[i - 1] = ana_state;
		}
	}
	listener->ana_state_change_count++;

	ctx->listener = listener;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(subsystem->tgt,
			      subsystem_listener_update_on_pg,
			      ctx,
			      subsystem_listener_update_done);
}
