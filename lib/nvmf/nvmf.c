/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bit_array.h"
#include "spdk/thread.h"
#include "spdk/nvmf.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk_internal/usdt.h"

#include "nvmf_internal.h"
#include "transport.h"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

#define SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS 1024

static TAILQ_HEAD(, spdk_nvmf_tgt) g_nvmf_tgts = TAILQ_HEAD_INITIALIZER(g_nvmf_tgts);

typedef void (*nvmf_qpair_disconnect_cpl)(void *ctx, int status);

/* supplied to a single call to nvmf_qpair_disconnect */
struct nvmf_qpair_disconnect_ctx {
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_ctrlr *ctrlr;
	uint16_t qid;
};

/*
 * There are several times when we need to iterate through the list of all qpairs and selectively delete them.
 * In order to do this sequentially without overlap, we must provide a context to recover the next qpair from
 * to enable calling nvmf_qpair_disconnect on the next desired qpair.
 */
struct nvmf_qpair_disconnect_many_ctx {
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_poll_group *group;
	spdk_nvmf_poll_group_mod_done cpl_fn;
	void *cpl_ctx;
};

static struct spdk_nvmf_referral *
nvmf_tgt_find_referral(struct spdk_nvmf_tgt *tgt,
		       const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_referral *referral;

	TAILQ_FOREACH(referral, &tgt->referrals, link) {
		if (spdk_nvme_transport_id_compare(&referral->trid, trid) == 0) {
			return referral;
		}
	}

	return NULL;
}

int
spdk_nvmf_tgt_add_referral(struct spdk_nvmf_tgt *tgt,
			   const struct spdk_nvmf_referral_opts *uopts)
{
	struct spdk_nvmf_referral *referral;
	struct spdk_nvmf_referral_opts opts = {};
	struct spdk_nvme_transport_id *trid = &opts.trid;

	memcpy(&opts, uopts, spdk_min(uopts->size, sizeof(opts)));
	if (trid->subnqn[0] == '\0') {
		snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	}

	if (!nvmf_nqn_is_valid(trid->subnqn)) {
		SPDK_ERRLOG("Invalid subsystem NQN\n");
		return -EINVAL;
	}

	/* If the entry already exists, just ignore it. */
	if (nvmf_tgt_find_referral(tgt, trid)) {
		return 0;
	}

	referral = calloc(1, sizeof(*referral));
	if (!referral) {
		SPDK_ERRLOG("Failed to allocate memory for a referral\n");
		return -ENOMEM;
	}

	referral->entry.subtype = nvmf_nqn_is_discovery(trid->subnqn) ?
				  SPDK_NVMF_SUBTYPE_DISCOVERY :
				  SPDK_NVMF_SUBTYPE_NVME;
	referral->entry.treq.secure_channel = opts.secure_channel ?
					      SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED :
					      SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;
	referral->entry.cntlid = 0xffff;
	referral->entry.trtype = trid->trtype;
	referral->entry.adrfam = trid->adrfam;
	memcpy(&referral->trid, trid, sizeof(struct spdk_nvme_transport_id));
	spdk_strcpy_pad(referral->entry.subnqn, trid->subnqn, sizeof(trid->subnqn), '\0');
	spdk_strcpy_pad(referral->entry.trsvcid, trid->trsvcid, sizeof(referral->entry.trsvcid), ' ');
	spdk_strcpy_pad(referral->entry.traddr, trid->traddr, sizeof(referral->entry.traddr), ' ');

	TAILQ_INSERT_HEAD(&tgt->referrals, referral, link);
	spdk_nvmf_send_discovery_log_notice(tgt, NULL);

	return 0;
}

int
spdk_nvmf_tgt_remove_referral(struct spdk_nvmf_tgt *tgt,
			      const struct spdk_nvmf_referral_opts *uopts)
{
	struct spdk_nvmf_referral *referral;
	struct spdk_nvmf_referral_opts opts = {};
	struct spdk_nvme_transport_id *trid = &opts.trid;

	memcpy(&opts, uopts, spdk_min(uopts->size, sizeof(opts)));
	if (trid->subnqn[0] == '\0') {
		snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	}

	referral = nvmf_tgt_find_referral(tgt, &opts.trid);
	if (referral == NULL) {
		return -ENOENT;
	}

	TAILQ_REMOVE(&tgt->referrals, referral, link);
	spdk_nvmf_send_discovery_log_notice(tgt, NULL);

	free(referral);

	return 0;
}

void
nvmf_qpair_set_state(struct spdk_nvmf_qpair *qpair,
		     enum spdk_nvmf_qpair_state state)
{
	assert(qpair != NULL);
	assert(qpair->group->thread == spdk_get_thread());

	qpair->state = state;
}

/*
 * Reset and clean up the poll group (I/O channel code will actually free the
 * group).
 */
static void
nvmf_tgt_cleanup_poll_group(struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t sid, nsid;

	TAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp) {
		TAILQ_REMOVE(&group->tgroups, tgroup, link);
		nvmf_transport_poll_group_destroy(tgroup);
	}

	for (sid = 0; sid < group->num_sgroups; sid++) {
		sgroup = &group->sgroups[sid];

		assert(sgroup != NULL);

		for (nsid = 0; nsid < sgroup->num_ns; nsid++) {
			if (sgroup->ns_info[nsid].channel) {
				spdk_put_io_channel(sgroup->ns_info[nsid].channel);
				sgroup->ns_info[nsid].channel = NULL;
			}
		}

		free(sgroup->ns_info);
	}

	free(group->sgroups);

	if (group->destroy_cb_fn) {
		group->destroy_cb_fn(group->destroy_cb_arg, 0);
	}
}

/*
 * Callback to unregister a poll group from the target, and clean up its state.
 */
static void
nvmf_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_poll_group *group = ctx_buf;

	SPDK_DTRACE_PROBE1_TICKS(nvmf_destroy_poll_group, spdk_thread_get_id(group->thread));

	pthread_mutex_lock(&tgt->mutex);
	TAILQ_REMOVE(&tgt->poll_groups, group, link);
	tgt->num_poll_groups--;
	pthread_mutex_unlock(&tgt->mutex);

	assert(!(tgt->state == NVMF_TGT_PAUSING || tgt->state == NVMF_TGT_RESUMING));
	nvmf_tgt_cleanup_poll_group(group);
}

static int
nvmf_poll_group_add_transport(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(group, transport);

	if (tgroup != NULL) {
		/* Transport already in the poll group */
		return 0;
	}

	tgroup = nvmf_transport_poll_group_create(transport, group);
	if (!tgroup) {
		SPDK_ERRLOG("Unable to create poll group for transport\n");
		return -1;
	}
	SPDK_DTRACE_PROBE2_TICKS(nvmf_transport_poll_group_create, transport,
				 spdk_thread_get_id(group->thread));

	tgroup->group = group;
	TAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);

	return 0;
}

static int
nvmf_tgt_create_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_poll_group *group = ctx_buf;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_thread *thread = spdk_get_thread();
	uint32_t i;
	int rc;

	group->tgt = tgt;
	TAILQ_INIT(&group->tgroups);
	TAILQ_INIT(&group->qpairs);
	group->thread = thread;
	pthread_mutex_init(&group->mutex, NULL);

	SPDK_DTRACE_PROBE1_TICKS(nvmf_create_poll_group, spdk_thread_get_id(thread));

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		rc = nvmf_poll_group_add_transport(group, transport);
		if (rc != 0) {
			nvmf_tgt_cleanup_poll_group(group);
			return rc;
		}
	}

	group->num_sgroups = tgt->max_subsystems;
	group->sgroups = calloc(tgt->max_subsystems, sizeof(struct spdk_nvmf_subsystem_poll_group));
	if (!group->sgroups) {
		nvmf_tgt_cleanup_poll_group(group);
		return -ENOMEM;
	}

	for (i = 0; i < tgt->max_subsystems; i++) {
		TAILQ_INIT(&group->sgroups[i].queued);
	}

	for (subsystem = spdk_nvmf_subsystem_get_first(tgt);
	     subsystem != NULL;
	     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
		if (nvmf_poll_group_add_subsystem(group, subsystem, NULL, NULL) != 0) {
			nvmf_tgt_cleanup_poll_group(group);
			return -1;
		}
	}

	pthread_mutex_lock(&tgt->mutex);
	tgt->num_poll_groups++;
	TAILQ_INSERT_TAIL(&tgt->poll_groups, group, link);
	pthread_mutex_unlock(&tgt->mutex);

	return 0;
}

static void
_nvmf_tgt_disconnect_qpairs(void *ctx)
{
	struct spdk_nvmf_qpair *qpair, *qpair_tmp;
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx;
	struct spdk_nvmf_poll_group *group = qpair_ctx->group;
	struct spdk_io_channel *ch;
	int rc;

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, qpair_tmp) {
		rc = spdk_nvmf_qpair_disconnect(qpair);
		if (rc && rc != -EINPROGRESS) {
			break;
		}
	}

	if (TAILQ_EMPTY(&group->qpairs)) {
		/* When the refcount from the channels reaches 0, nvmf_tgt_destroy_poll_group will be called. */
		ch = spdk_io_channel_from_ctx(group);
		spdk_put_io_channel(ch);
		free(qpair_ctx);
		return;
	}

	/* Some qpairs are in process of being disconnected. Send a message and try to remove them again */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_tgt_disconnect_qpairs, ctx);
}

static void
nvmf_tgt_destroy_poll_group_qpairs(struct spdk_nvmf_poll_group *group)
{
	struct nvmf_qpair_disconnect_many_ctx *ctx;

	SPDK_DTRACE_PROBE1_TICKS(nvmf_destroy_poll_group_qpairs, spdk_thread_get_id(group->thread));

	ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_many_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate memory for destroy poll group ctx\n");
		return;
	}

	ctx->group = group;
	_nvmf_tgt_disconnect_qpairs(ctx);
}

struct spdk_nvmf_tgt *
spdk_nvmf_tgt_create(struct spdk_nvmf_target_opts *_opts)
{
	struct spdk_nvmf_tgt *tgt, *tmp_tgt;
	struct spdk_nvmf_target_opts opts = {
		.max_subsystems = SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS,
		.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY,
	};

	memcpy(&opts, _opts, _opts->size);
	if (strnlen(opts.name, NVMF_TGT_NAME_MAX_LENGTH) == NVMF_TGT_NAME_MAX_LENGTH) {
		SPDK_ERRLOG("Provided target name exceeds the max length of %u.\n", NVMF_TGT_NAME_MAX_LENGTH);
		return NULL;
	}

	TAILQ_FOREACH(tmp_tgt, &g_nvmf_tgts, link) {
		if (!strncmp(opts.name, tmp_tgt->name, NVMF_TGT_NAME_MAX_LENGTH)) {
			SPDK_ERRLOG("Provided target name must be unique.\n");
			return NULL;
		}
	}

	tgt = calloc(1, sizeof(*tgt));
	if (!tgt) {
		return NULL;
	}

	snprintf(tgt->name, NVMF_TGT_NAME_MAX_LENGTH, "%s", opts.name);

	if (!opts.max_subsystems) {
		tgt->max_subsystems = SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS;
	} else {
		tgt->max_subsystems = opts.max_subsystems;
	}

	tgt->crdt[0] = opts.crdt[0];
	tgt->crdt[1] = opts.crdt[1];
	tgt->crdt[2] = opts.crdt[2];
	tgt->discovery_filter = opts.discovery_filter;
	tgt->discovery_genctr = 0;
	tgt->dhchap_digests = opts.dhchap_digests;
	tgt->dhchap_dhgroups = opts.dhchap_dhgroups;
	TAILQ_INIT(&tgt->transports);
	TAILQ_INIT(&tgt->poll_groups);
	TAILQ_INIT(&tgt->referrals);
	tgt->num_poll_groups = 0;

	tgt->subsystem_ids = spdk_bit_array_create(tgt->max_subsystems);
	if (tgt->subsystem_ids == NULL) {
		free(tgt);
		return NULL;
	}

	RB_INIT(&tgt->subsystems);

	pthread_mutex_init(&tgt->mutex, NULL);

	spdk_io_device_register(tgt,
				nvmf_tgt_create_poll_group,
				nvmf_tgt_destroy_poll_group,
				sizeof(struct spdk_nvmf_poll_group),
				tgt->name);

	tgt->state = NVMF_TGT_RUNNING;

	TAILQ_INSERT_HEAD(&g_nvmf_tgts, tgt, link);

	return tgt;
}

static void
_nvmf_tgt_destroy_next_transport(void *ctx)
{
	struct spdk_nvmf_tgt *tgt = ctx;
	struct spdk_nvmf_transport *transport;

	if (!TAILQ_EMPTY(&tgt->transports)) {
		transport = TAILQ_FIRST(&tgt->transports);
		TAILQ_REMOVE(&tgt->transports, transport, link);
		spdk_nvmf_transport_destroy(transport, _nvmf_tgt_destroy_next_transport, tgt);
	} else {
		spdk_nvmf_tgt_destroy_done_fn *destroy_cb_fn = tgt->destroy_cb_fn;
		void *destroy_cb_arg = tgt->destroy_cb_arg;

		pthread_mutex_destroy(&tgt->mutex);
		free(tgt);

		if (destroy_cb_fn) {
			destroy_cb_fn(destroy_cb_arg, 0);
		}
	}
}

static void
nvmf_tgt_destroy_cb(void *io_device)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_subsystem *subsystem, *subsystem_next;
	int rc;
	struct spdk_nvmf_referral *referral;

	while ((referral = TAILQ_FIRST(&tgt->referrals))) {
		TAILQ_REMOVE(&tgt->referrals, referral, link);
		free(referral);
	}

	nvmf_tgt_stop_mdns_prr(tgt);

	/* We will be freeing subsystems in this loop, so we always need to get the next one
	 * ahead of time, since we can't call get_next() on a subsystem that's been freed.
	 */
	for (subsystem = spdk_nvmf_subsystem_get_first(tgt),
	     subsystem_next = spdk_nvmf_subsystem_get_next(subsystem);
	     subsystem != NULL;
	     subsystem = subsystem_next,
	     subsystem_next = spdk_nvmf_subsystem_get_next(subsystem_next)) {
		nvmf_subsystem_remove_all_listeners(subsystem, true);

		rc = spdk_nvmf_subsystem_destroy(subsystem, nvmf_tgt_destroy_cb, tgt);
		if (rc) {
			if (rc == -EINPROGRESS) {
				/* If rc is -EINPROGRESS, nvmf_tgt_destroy_cb will be called again when subsystem #i
				 * is destroyed, nvmf_tgt_destroy_cb will continue to destroy other subsystems if any */
				return;
			} else {
				SPDK_ERRLOG("Failed to destroy subsystem %s, rc %d\n", subsystem->subnqn, rc);
			}
		}
	}
	spdk_bit_array_free(&tgt->subsystem_ids);
	_nvmf_tgt_destroy_next_transport(tgt);
}

void
spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt,
		      spdk_nvmf_tgt_destroy_done_fn cb_fn,
		      void *cb_arg)
{
	assert(!(tgt->state == NVMF_TGT_PAUSING || tgt->state == NVMF_TGT_RESUMING));

	tgt->destroy_cb_fn = cb_fn;
	tgt->destroy_cb_arg = cb_arg;

	TAILQ_REMOVE(&g_nvmf_tgts, tgt, link);

	spdk_io_device_unregister(tgt, nvmf_tgt_destroy_cb);
}

const char *
spdk_nvmf_tgt_get_name(struct spdk_nvmf_tgt *tgt)
{
	return tgt->name;
}

struct spdk_nvmf_tgt *
spdk_nvmf_get_tgt(const char *name)
{
	struct spdk_nvmf_tgt *tgt;
	uint32_t num_targets = 0;

	TAILQ_FOREACH(tgt, &g_nvmf_tgts, link) {
		if (name) {
			if (!strncmp(tgt->name, name, NVMF_TGT_NAME_MAX_LENGTH)) {
				return tgt;
			}
		}
		num_targets++;
	}

	/*
	 * special case. If there is only one target and
	 * no name was specified, return the only available
	 * target. If there is more than one target, name must
	 * be specified.
	 */
	if (!name && num_targets == 1) {
		return TAILQ_FIRST(&g_nvmf_tgts);
	}

	return NULL;
}

struct spdk_nvmf_tgt *
spdk_nvmf_get_first_tgt(void)
{
	return TAILQ_FIRST(&g_nvmf_tgts);
}

struct spdk_nvmf_tgt *
spdk_nvmf_get_next_tgt(struct spdk_nvmf_tgt *prev)
{
	return TAILQ_NEXT(prev, link);
}

static void
nvmf_write_nvme_subsystem_config(struct spdk_json_write_ctx *w,
				 struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host *host;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t max_namespaces;
	struct spdk_nvmf_transport *transport;

	assert(spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME);

	/* { */
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "nvmf_create_subsystem");

	/*     "params" : { */
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
	spdk_json_write_named_bool(w, "allow_any_host", spdk_nvmf_subsystem_get_allow_any_host(subsystem));
	spdk_json_write_named_string(w, "serial_number", spdk_nvmf_subsystem_get_sn(subsystem));
	spdk_json_write_named_string(w, "model_number", spdk_nvmf_subsystem_get_mn(subsystem));

	max_namespaces = spdk_nvmf_subsystem_get_max_namespaces(subsystem);
	if (max_namespaces != 0) {
		spdk_json_write_named_uint32(w, "max_namespaces", max_namespaces);
	}

	spdk_json_write_named_uint32(w, "min_cntlid", spdk_nvmf_subsystem_get_min_cntlid(subsystem));
	spdk_json_write_named_uint32(w, "max_cntlid", spdk_nvmf_subsystem_get_max_cntlid(subsystem));
	spdk_json_write_named_bool(w, "ana_reporting", spdk_nvmf_subsystem_get_ana_reporting(subsystem));

	/*     } "params" */
	spdk_json_write_object_end(w);

	/* } */
	spdk_json_write_object_end(w);

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_host");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
		spdk_json_write_named_string(w, "host", spdk_nvmf_host_get_nqn(host));
		if (host->dhchap_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_key",
						     spdk_key_get_name(host->dhchap_key));
		}
		if (host->dhchap_ctrlr_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_ctrlr_key",
						     spdk_key_get_name(host->dhchap_ctrlr_key));
		}
		TAILQ_FOREACH(transport, &subsystem->tgt->transports, link) {
			if (transport->ops->subsystem_dump_host != NULL) {
				transport->ops->subsystem_dump_host(transport, subsystem, host->nqn, w);
			}
		}

		/*     } "params" */
		spdk_json_write_object_end(w);

		/* } */
		spdk_json_write_object_end(w);
	}

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts));

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_ns");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));

		/*     "namespace" : { */
		spdk_json_write_named_object_begin(w, "namespace");

		spdk_json_write_named_uint32(w, "nsid", spdk_nvmf_ns_get_id(ns));
		spdk_json_write_named_string(w, "bdev_name", spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));

		if (ns->ptpl_file != NULL) {
			spdk_json_write_named_string(w, "ptpl_file", ns->ptpl_file);
		}

		if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) {
			SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(uint64_t) * 2, "size mismatch");
			spdk_json_write_named_string_fmt(w, "nguid", "%016"PRIX64"%016"PRIX64, from_be64(&ns_opts.nguid[0]),
							 from_be64(&ns_opts.nguid[8]));
		}

		if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) {
			SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(uint64_t), "size mismatch");
			spdk_json_write_named_string_fmt(w, "eui64", "%016"PRIX64, from_be64(&ns_opts.eui64));
		}

		if (!spdk_uuid_is_null(&ns_opts.uuid)) {
			spdk_json_write_named_uuid(w, "uuid",  &ns_opts.uuid);
		}

		if (spdk_nvmf_subsystem_get_ana_reporting(subsystem)) {
			spdk_json_write_named_uint32(w, "anagrpid", ns_opts.anagrpid);
		}

		spdk_json_write_named_bool(w, "no_auto_visible", !ns->always_visible);

		/*     "namespace" */
		spdk_json_write_object_end(w);

		/*     } "params" */
		spdk_json_write_object_end(w);

		/* } */
		spdk_json_write_object_end(w);

		TAILQ_FOREACH(host, &ns->hosts, link) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "nvmf_ns_add_host");
			spdk_json_write_named_object_begin(w, "params");
			spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
			spdk_json_write_named_uint32(w, "nsid", spdk_nvmf_ns_get_id(ns));
			spdk_json_write_named_string(w, "host", spdk_nvmf_host_get_nqn(host));
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		}
	}
}

static void
nvmf_write_subsystem_config_json(struct spdk_json_write_ctx *w,
				 struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_listener *listener;
	struct spdk_nvmf_transport *transport;
	const struct spdk_nvme_transport_id *trid;

	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		nvmf_write_nvme_subsystem_config(w, subsystem);
	}

	for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem); listener != NULL;
	     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {
		transport = listener->transport;
		trid = spdk_nvmf_subsystem_listener_get_trid(listener);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_listener");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));

		spdk_json_write_named_object_begin(w, "listen_address");
		nvmf_transport_listen_dump_trid(trid, w);
		spdk_json_write_object_end(w);
		if (transport->ops->listen_dump_opts) {
			transport->ops->listen_dump_opts(transport, trid, w);
		}

		spdk_json_write_named_bool(w, "secure_channel", listener->opts.secure_channel);

		if (listener->opts.sock_impl) {
			spdk_json_write_named_string(w, "sock_impl", listener->opts.sock_impl);
		}

		/*     } "params" */
		spdk_json_write_object_end(w);

		/* } */
		spdk_json_write_object_end(w);
	}
}

void
spdk_nvmf_tgt_write_config_json(struct spdk_json_write_ctx *w, struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_referral *referral;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "nvmf_set_max_subsystems");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "max_subsystems", tgt->max_subsystems);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "nvmf_set_crdt");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "crdt1", tgt->crdt[0]);
	spdk_json_write_named_uint32(w, "crdt2", tgt->crdt[1]);
	spdk_json_write_named_uint32(w, "crdt3", tgt->crdt[2]);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);

	/* write transports */
	TAILQ_FOREACH(transport, &tgt->transports, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_create_transport");
		nvmf_transport_dump_opts(transport, w, true);
		spdk_json_write_object_end(w);
	}

	TAILQ_FOREACH(referral, &tgt->referrals, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_discovery_add_referral");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_object_begin(w, "address");
		nvmf_transport_listen_dump_trid(&referral->trid, w);
		spdk_json_write_object_end(w);
		spdk_json_write_named_bool(w, "secure_channel",
					   referral->entry.treq.secure_channel ==
					   SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED);
		spdk_json_write_named_string(w, "subnqn", referral->trid.subnqn);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	subsystem = spdk_nvmf_subsystem_get_first(tgt);
	while (subsystem) {
		nvmf_write_subsystem_config_json(w, subsystem);
		subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	}
}

static void
nvmf_listen_opts_copy(struct spdk_nvmf_listen_opts *opts,
		      const struct spdk_nvmf_listen_opts *opts_src, size_t opts_size)
{
	assert(opts);
	assert(opts_src);

	opts->opts_size = opts_size;

#define SET_FIELD(field) \
    if (offsetof(struct spdk_nvmf_listen_opts, field) + sizeof(opts->field) <= opts_size) { \
                 opts->field = opts_src->field; \
    } \

	SET_FIELD(transport_specific);
	SET_FIELD(secure_channel);
	SET_FIELD(ana_state);
	SET_FIELD(sock_impl);
#undef SET_FIELD

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listen_opts) == 32, "Incorrect size");
}

void
spdk_nvmf_listen_opts_init(struct spdk_nvmf_listen_opts *opts, size_t opts_size)
{
	struct spdk_nvmf_listen_opts opts_local = {};

	/* local version of opts should have defaults set here */
	opts_local.ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	nvmf_listen_opts_copy(opts, &opts_local, opts_size);
}

int
spdk_nvmf_tgt_listen_ext(struct spdk_nvmf_tgt *tgt, const struct spdk_nvme_transport_id *trid,
			 struct spdk_nvmf_listen_opts *opts)
{
	struct spdk_nvmf_transport *transport;
	int rc;
	struct spdk_nvmf_listen_opts opts_local = {};

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return -EINVAL;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("The opts_size in opts structure should not be zero\n");
		return -EINVAL;
	}

	transport = spdk_nvmf_tgt_get_transport(tgt, trid->trstring);
	if (!transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    trid->trstring);
		return -EINVAL;
	}

	nvmf_listen_opts_copy(&opts_local, opts, opts->opts_size);
	rc = spdk_nvmf_transport_listen(transport, trid, &opts_local);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to listen on address '%s'\n", trid->traddr);
	}

	return rc;
}

int
spdk_nvmf_tgt_stop_listen(struct spdk_nvmf_tgt *tgt,
			  const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_transport *transport;
	int rc;

	transport = spdk_nvmf_tgt_get_transport(tgt, trid->trstring);
	if (!transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    trid->trstring);
		return -EINVAL;
	}

	rc = spdk_nvmf_transport_stop_listen(transport, trid);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to stop listening on address '%s'\n", trid->traddr);
		return rc;
	}
	return 0;
}

struct spdk_nvmf_tgt_add_transport_ctx {
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_transport *transport;
	spdk_nvmf_tgt_add_transport_done_fn cb_fn;
	void *cb_arg;
	int status;
};

static void
_nvmf_tgt_remove_transport_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->cb_fn(ctx->cb_arg, ctx->status);
	free(ctx);
}

static void
_nvmf_tgt_remove_transport(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	struct spdk_nvmf_transport_poll_group *tgroup, *tmp;

	TAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp) {
		if (tgroup->transport == ctx->transport) {
			TAILQ_REMOVE(&group->tgroups, tgroup, link);
			nvmf_transport_poll_group_destroy(tgroup);
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
_nvmf_tgt_add_transport_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	if (status) {
		ctx->status = status;
		spdk_for_each_channel(ctx->tgt,
				      _nvmf_tgt_remove_transport,
				      ctx,
				      _nvmf_tgt_remove_transport_done);
		return;
	}

	ctx->transport->tgt = ctx->tgt;
	TAILQ_INSERT_TAIL(&ctx->tgt->transports, ctx->transport, link);
	ctx->cb_fn(ctx->cb_arg, status);
	free(ctx);
}

static void
_nvmf_tgt_add_transport(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = nvmf_poll_group_add_transport(group, ctx->transport);
	spdk_for_each_channel_continue(i, rc);
}

void
spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
			    struct spdk_nvmf_transport *transport,
			    spdk_nvmf_tgt_add_transport_done_fn cb_fn,
			    void *cb_arg)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx;

	SPDK_DTRACE_PROBE2_TICKS(nvmf_tgt_add_transport, transport, tgt->name);

	if (spdk_nvmf_tgt_get_transport(tgt, transport->ops->name)) {
		cb_fn(cb_arg, -EEXIST);
		return; /* transport already created */
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->tgt = tgt;
	ctx->transport = transport;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(tgt,
			      _nvmf_tgt_add_transport,
			      ctx,
			      _nvmf_tgt_add_transport_done);
}

struct nvmf_tgt_pause_ctx {
	struct spdk_nvmf_tgt *tgt;
	spdk_nvmf_tgt_pause_polling_cb_fn cb_fn;
	void *cb_arg;
};

static void
_nvmf_tgt_pause_polling_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_tgt_pause_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->tgt->state = NVMF_TGT_PAUSED;

	ctx->cb_fn(ctx->cb_arg, status);
	free(ctx);
}

static void
_nvmf_tgt_pause_polling(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		nvmf_transport_poll_group_pause(tgroup);
	}

	spdk_for_each_channel_continue(i, 0);
}

int
spdk_nvmf_tgt_pause_polling(struct spdk_nvmf_tgt *tgt, spdk_nvmf_tgt_pause_polling_cb_fn cb_fn,
			    void *cb_arg)
{
	struct nvmf_tgt_pause_ctx *ctx;

	SPDK_DTRACE_PROBE2_TICKS(nvmf_tgt_pause_polling, tgt, tgt->name);

	switch (tgt->state) {
	case NVMF_TGT_PAUSING:
	case NVMF_TGT_RESUMING:
		return -EBUSY;
	case NVMF_TGT_RUNNING:
		break;
	default:
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}


	tgt->state = NVMF_TGT_PAUSING;

	ctx->tgt = tgt;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(tgt,
			      _nvmf_tgt_pause_polling,
			      ctx,
			      _nvmf_tgt_pause_polling_done);
	return 0;
}

static void
_nvmf_tgt_resume_polling_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_tgt_pause_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->tgt->state = NVMF_TGT_RUNNING;

	ctx->cb_fn(ctx->cb_arg, status);
	free(ctx);
}

static void
_nvmf_tgt_resume_polling(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		nvmf_transport_poll_group_resume(tgroup);
	}

	spdk_for_each_channel_continue(i, 0);
}

int
spdk_nvmf_tgt_resume_polling(struct spdk_nvmf_tgt *tgt, spdk_nvmf_tgt_resume_polling_cb_fn cb_fn,
			     void *cb_arg)
{
	struct nvmf_tgt_pause_ctx *ctx;

	SPDK_DTRACE_PROBE2_TICKS(nvmf_tgt_resume_polling, tgt, tgt->name);

	switch (tgt->state) {
	case NVMF_TGT_PAUSING:
	case NVMF_TGT_RESUMING:
		return -EBUSY;
	case NVMF_TGT_PAUSED:
		break;
	default:
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	tgt->state = NVMF_TGT_RESUMING;

	ctx->tgt = tgt;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(tgt,
			      _nvmf_tgt_resume_polling,
			      ctx,
			      _nvmf_tgt_resume_polling_done);
	return 0;
}

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	struct spdk_nvmf_subsystem subsystem;

	if (!subnqn) {
		return NULL;
	}

	/* Ensure that subnqn is null terminated */
	if (!memchr(subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1)) {
		SPDK_ERRLOG("Connect SUBNQN is not null terminated\n");
		return NULL;
	}

	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "%s", subnqn);
	return RB_FIND(subsystem_tree, &tgt->subsystems, &subsystem);
}

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, const char *transport_name)
{
	struct spdk_nvmf_transport *transport;

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		if (!strncasecmp(transport->ops->name, transport_name, SPDK_NVMF_TRSTRING_MAX_LEN)) {
			return transport;
		}
	}
	return NULL;
}

struct nvmf_new_qpair_ctx {
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_poll_group *group;
};

static void
_nvmf_poll_group_add(void *_ctx)
{
	struct nvmf_new_qpair_ctx *ctx = _ctx;
	struct spdk_nvmf_qpair *qpair = ctx->qpair;
	struct spdk_nvmf_poll_group *group = ctx->group;

	free(_ctx);

	if (spdk_nvmf_poll_group_add(group, qpair) != 0) {
		SPDK_ERRLOG("Unable to add the qpair to a poll group.\n");

		assert(qpair->state == SPDK_NVMF_QPAIR_UNINITIALIZED);
		pthread_mutex_lock(&group->mutex);
		assert(group->current_unassociated_qpairs > 0);
		group->current_unassociated_qpairs--;
		pthread_mutex_unlock(&group->mutex);

		spdk_nvmf_qpair_disconnect(qpair);
	}
}

void
spdk_nvmf_tgt_new_qpair(struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_poll_group *group;
	struct nvmf_new_qpair_ctx *ctx;

	group = spdk_nvmf_get_optimal_poll_group(qpair);
	if (group == NULL) {
		if (tgt->next_poll_group == NULL) {
			tgt->next_poll_group = TAILQ_FIRST(&tgt->poll_groups);
			if (tgt->next_poll_group == NULL) {
				SPDK_ERRLOG("No poll groups exist.\n");
				spdk_nvmf_qpair_disconnect(qpair);
				return;
			}
		}
		group = tgt->next_poll_group;
		tgt->next_poll_group = TAILQ_NEXT(group, link);
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Unable to send message to poll group.\n");
		spdk_nvmf_qpair_disconnect(qpair);
		return;
	}

	ctx->qpair = qpair;
	ctx->group = group;

	pthread_mutex_lock(&group->mutex);
	group->current_unassociated_qpairs++;
	pthread_mutex_unlock(&group->mutex);

	spdk_thread_send_msg(group->thread, _nvmf_poll_group_add, ctx);
}

struct spdk_nvmf_poll_group *
spdk_nvmf_poll_group_create(struct spdk_nvmf_tgt *tgt)
{
	struct spdk_io_channel *ch;

	ch = spdk_get_io_channel(tgt);
	if (!ch) {
		SPDK_ERRLOG("Unable to get I/O channel for target\n");
		return NULL;
	}

	return spdk_io_channel_get_ctx(ch);
}

void
spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group,
			     spdk_nvmf_poll_group_destroy_done_fn cb_fn,
			     void *cb_arg)
{
	assert(group->destroy_cb_fn == NULL);
	group->destroy_cb_fn = cb_fn;
	group->destroy_cb_arg = cb_arg;

	/* This function will put the io_channel associated with this poll group */
	nvmf_tgt_destroy_poll_group_qpairs(group);
}

int
spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	int rc;
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_INIT(&qpair->outstanding);
	qpair->group = group;
	qpair->ctrlr = NULL;
	qpair->disconnect_started = false;

	tgroup = nvmf_get_transport_poll_group(group, qpair->transport);
	if (tgroup == NULL) {
		return -1;
	}

	rc = nvmf_transport_poll_group_add(tgroup, qpair);

	/* We add the qpair to the group only it is successfully added into the tgroup */
	if (rc == 0) {
		SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_add_qpair, qpair, spdk_thread_get_id(group->thread));
		TAILQ_INSERT_TAIL(&group->qpairs, qpair, link);
		nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_CONNECTING);
	}

	return rc;
}

static void
_nvmf_ctrlr_destruct(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;

	nvmf_ctrlr_destruct(ctrlr);
}

static void
_nvmf_ctrlr_free_from_qpair(void *ctx)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx;
	struct spdk_nvmf_ctrlr *ctrlr = qpair_ctx->ctrlr;
	uint32_t count;

	spdk_bit_array_clear(ctrlr->qpair_mask, qpair_ctx->qid);
	SPDK_DEBUGLOG(nvmf, "qpair_mask cleared, qid %u\n", qpair_ctx->qid);
	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	if (count == 0) {
		assert(!ctrlr->in_destruct);
		SPDK_DEBUGLOG(nvmf, "Last qpair %u, destroy ctrlr 0x%hx\n", qpair_ctx->qid, ctrlr->cntlid);
		ctrlr->in_destruct = true;
		spdk_thread_send_msg(ctrlr->subsys->thread, _nvmf_ctrlr_destruct, ctrlr);
	}
	free(qpair_ctx);
}

static void
_nvmf_transport_qpair_fini_complete(void *cb_ctx)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = cb_ctx;
	struct spdk_nvmf_ctrlr *ctrlr;

	ctrlr = qpair_ctx->ctrlr;
	SPDK_DEBUGLOG(nvmf, "Finish destroying qid %u\n", qpair_ctx->qid);

	if (ctrlr) {
		if (qpair_ctx->qid == 0) {
			/* Admin qpair is removed, so set the pointer to NULL.
			 * This operation is safe since we are on ctrlr thread now, admin qpair's thread is the same
			 * as controller's thread */
			assert(ctrlr->thread == spdk_get_thread());
			ctrlr->admin_qpair = NULL;
		}
		/* Free qpair id from controller's bit mask and destroy the controller if it is the last qpair */
		if (ctrlr->thread) {
			spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_free_from_qpair, qpair_ctx);
		} else {
			_nvmf_ctrlr_free_from_qpair(qpair_ctx);
		}
	} else {
		free(qpair_ctx);
	}
}

void
spdk_nvmf_poll_group_remove(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;
	int rc;

	SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_remove_qpair, qpair,
				 spdk_thread_get_id(qpair->group->thread));
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ERROR);

	/* Find the tgroup and remove the qpair from the tgroup */
	tgroup = nvmf_get_transport_poll_group(qpair->group, qpair->transport);
	if (tgroup != NULL) {
		rc = nvmf_transport_poll_group_remove(tgroup, qpair);
		if (rc && (rc != ENOTSUP)) {
			SPDK_ERRLOG("Cannot remove qpair=%p from transport group=%p\n",
				    qpair, tgroup);
		}
	}

	TAILQ_REMOVE(&qpair->group->qpairs, qpair, link);
	qpair->group = NULL;
}

static void
_nvmf_qpair_sgroup_req_clean(struct spdk_nvmf_subsystem_poll_group *sgroup,
			     const struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_request *req, *tmp;
	TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) {
		if (req->qpair == qpair) {
			TAILQ_REMOVE(&sgroup->queued, req, link);
			if (nvmf_transport_req_free(req)) {
				SPDK_ERRLOG("Transport request free error!\n");
			}
		}
	}
}

static void
_nvmf_qpair_destroy(void *ctx, int status)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx;
	struct spdk_nvmf_qpair *qpair = qpair_ctx->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t sid;

	assert(qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING);
	qpair_ctx->qid = qpair->qid;

	if (qpair->connect_received) {
		if (0 == qpair->qid) {
			assert(qpair->group->stat.current_admin_qpairs > 0);
			qpair->group->stat.current_admin_qpairs--;
		} else {
			assert(qpair->group->stat.current_io_qpairs > 0);
			qpair->group->stat.current_io_qpairs--;
		}
	} else {
		pthread_mutex_lock(&qpair->group->mutex);
		assert(qpair->group->current_unassociated_qpairs > 0);
		qpair->group->current_unassociated_qpairs--;
		pthread_mutex_unlock(&qpair->group->mutex);
	}

	if (ctrlr) {
		sgroup = &qpair->group->sgroups[ctrlr->subsys->id];
		_nvmf_qpair_sgroup_req_clean(sgroup, qpair);
	} else {
		for (sid = 0; sid < qpair->group->num_sgroups; sid++) {
			sgroup = &qpair->group->sgroups[sid];
			assert(sgroup != NULL);
			_nvmf_qpair_sgroup_req_clean(sgroup, qpair);
		}
	}

	nvmf_qpair_auth_destroy(qpair);
	qpair_ctx->ctrlr = ctrlr;
	spdk_nvmf_poll_group_remove(qpair);
	nvmf_transport_qpair_fini(qpair, _nvmf_transport_qpair_fini_complete, qpair_ctx);
}

static void
_nvmf_qpair_disconnect_msg(void *ctx)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx;

	spdk_nvmf_qpair_disconnect(qpair_ctx->qpair);
	free(ctx);
}

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_poll_group *group = qpair->group;
	struct nvmf_qpair_disconnect_ctx *qpair_ctx;

	if (__atomic_test_and_set(&qpair->disconnect_started, __ATOMIC_RELAXED)) {
		return -EINPROGRESS;
	}

	/* If we get a qpair in the uninitialized state, we can just destroy it immediately */
	if (qpair->state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		nvmf_transport_qpair_fini(qpair, NULL, NULL);
		return 0;
	}

	assert(group != NULL);
	if (spdk_get_thread() != group->thread) {
		/* clear the atomic so we can set it on the next call on the proper thread. */
		__atomic_clear(&qpair->disconnect_started, __ATOMIC_RELAXED);
		qpair_ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_ctx));
		if (!qpair_ctx) {
			SPDK_ERRLOG("Unable to allocate context for nvmf_qpair_disconnect\n");
			return -ENOMEM;
		}
		qpair_ctx->qpair = qpair;
		spdk_thread_send_msg(group->thread, _nvmf_qpair_disconnect_msg, qpair_ctx);
		return 0;
	}

	SPDK_DTRACE_PROBE2_TICKS(nvmf_qpair_disconnect, qpair, spdk_thread_get_id(group->thread));
	assert(spdk_nvmf_qpair_is_active(qpair));
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_DEACTIVATING);

	qpair_ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_ctx));
	if (!qpair_ctx) {
		SPDK_ERRLOG("Unable to allocate context for nvmf_qpair_disconnect\n");
		return -ENOMEM;
	}

	qpair_ctx->qpair = qpair;

	/* Check for outstanding I/O */
	if (!TAILQ_EMPTY(&qpair->outstanding)) {
		SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_drain_qpair, qpair, spdk_thread_get_id(group->thread));
		qpair->state_cb = _nvmf_qpair_destroy;
		qpair->state_cb_arg = qpair_ctx;
		nvmf_qpair_abort_pending_zcopy_reqs(qpair);
		nvmf_qpair_free_aer(qpair);
		return 0;
	}

	_nvmf_qpair_destroy(qpair_ctx, 0);

	return 0;
}

int
spdk_nvmf_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	memset(trid, 0, sizeof(*trid));
	return nvmf_transport_qpair_get_peer_trid(qpair, trid);
}

int
spdk_nvmf_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	memset(trid, 0, sizeof(*trid));
	return nvmf_transport_qpair_get_local_trid(qpair, trid);
}

int
spdk_nvmf_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				struct spdk_nvme_transport_id *trid)
{
	memset(trid, 0, sizeof(*trid));
	return nvmf_transport_qpair_get_listen_trid(qpair, trid);
}

static int
poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t i;
	struct spdk_nvmf_ns *ns;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	struct spdk_nvmf_ctrlr *ctrlr;
	bool ns_changed, ana_changed;

	/* Make sure our poll group has memory for this subsystem allocated */
	if (subsystem->id >= group->num_sgroups) {
		return -ENOMEM;
	}

	sgroup = &group->sgroups[subsystem->id];

	/* Make sure the array of namespace information is the correct size */
	if (sgroup->num_ns == 0 && subsystem->max_nsid > 0) {
		/* First allocation */
		sgroup->ns_info = calloc(subsystem->max_nsid, sizeof(struct spdk_nvmf_subsystem_pg_ns_info));
		if (!sgroup->ns_info) {
			return -ENOMEM;
		}
		sgroup->num_ns = subsystem->max_nsid;
	}

	ns_changed = false;
	ana_changed = false;

	/* Detect bdevs that were added or removed */
	for (i = 0; i < sgroup->num_ns; i++) {
		ns = subsystem->ns[i];
		ns_info = &sgroup->ns_info[i];
		ch = ns_info->channel;

		if (ns == NULL && ch == NULL) {
			/* Both NULL. Leave empty */
		} else if (ns == NULL && ch != NULL) {
			/* There was a channel here, but the namespace is gone. */
			ns_changed = true;
			spdk_put_io_channel(ch);
			ns_info->channel = NULL;
		} else if (ns != NULL && ch == NULL) {
			/* A namespace appeared but there is no channel yet */
			ns_changed = true;
			ch = spdk_bdev_get_io_channel(ns->desc);
			if (ch == NULL) {
				SPDK_ERRLOG("Could not allocate I/O channel.\n");
				return -ENOMEM;
			}
			ns_info->channel = ch;
		} else if (spdk_uuid_compare(&ns_info->uuid, spdk_bdev_get_uuid(ns->bdev)) != 0) {
			/* A namespace was here before, but was replaced by a new one. */
			ns_changed = true;
			spdk_put_io_channel(ns_info->channel);
			memset(ns_info, 0, sizeof(*ns_info));

			ch = spdk_bdev_get_io_channel(ns->desc);
			if (ch == NULL) {
				SPDK_ERRLOG("Could not allocate I/O channel.\n");
				return -ENOMEM;
			}
			ns_info->channel = ch;
		} else if (ns_info->num_blocks != spdk_bdev_get_num_blocks(ns->bdev)) {
			/* Namespace is still there but size has changed */
			SPDK_DEBUGLOG(nvmf, "Namespace resized: subsystem_id %u,"
				      " nsid %u, pg %p, old %" PRIu64 ", new %" PRIu64 "\n",
				      subsystem->id,
				      ns->nsid,
				      group,
				      ns_info->num_blocks,
				      spdk_bdev_get_num_blocks(ns->bdev));
			ns_changed = true;
		} else if (ns_info->anagrpid != ns->anagrpid) {
			/* Namespace is still there but ANA group ID has changed */
			SPDK_DEBUGLOG(nvmf, "ANA group ID changed: subsystem_id %u,"
				      "nsid %u, pg %p, old %u, new %u\n",
				      subsystem->id,
				      ns->nsid,
				      group,
				      ns_info->anagrpid,
				      ns->anagrpid);
			ana_changed = true;
		}

		if (ns == NULL) {
			memset(ns_info, 0, sizeof(*ns_info));
		} else {
			ns_info->uuid = *spdk_bdev_get_uuid(ns->bdev);
			ns_info->num_blocks = spdk_bdev_get_num_blocks(ns->bdev);
			ns_info->anagrpid = ns->anagrpid;
			nvmf_subsystem_poll_group_update_ns_reservation(ns, ns_info);
		}
	}

	if (ns_changed || ana_changed) {
		TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
			if (ctrlr->thread != spdk_get_thread()) {
				continue;
			}
			/* It is possible that a ctrlr was added but the admin_qpair hasn't been
			 * assigned yet.
			 */
			if (!ctrlr->admin_qpair) {
				continue;
			}
			if (ctrlr->admin_qpair->group == group) {
				if (ns_changed) {
					nvmf_ctrlr_async_event_ns_notice(ctrlr);
				}
				if (ana_changed) {
					nvmf_ctrlr_async_event_ana_change_notice(ctrlr);
				}
			}
		}
	}

	return 0;
}

int
nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem)
{
	return poll_group_update_subsystem(group, subsystem);
}

int
nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_subsystem *subsystem,
			      spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	int rc = 0;
	struct spdk_nvmf_subsystem_poll_group *sgroup = &group->sgroups[subsystem->id];
	struct spdk_nvmf_request *req, *tmp;
	uint32_t i;

	if (!TAILQ_EMPTY(&sgroup->queued)) {
		SPDK_ERRLOG("sgroup->queued not empty when adding subsystem\n");
		TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) {
			TAILQ_REMOVE(&sgroup->queued, req, link);
			if (nvmf_transport_req_free(req)) {
				SPDK_ERRLOG("Transport request free error!\n");
			}
		}
	}

	rc = poll_group_update_subsystem(group, subsystem);
	if (rc) {
		nvmf_poll_group_remove_subsystem(group, subsystem, NULL, NULL);
		goto fini;
	}

	sgroup->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;

	for (i = 0; i < sgroup->num_ns; i++) {
		sgroup->ns_info[i].state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	}

fini:
	if (cb_fn) {
		cb_fn(cb_arg, rc);
	}

	SPDK_DTRACE_PROBE2_TICKS(nvmf_poll_group_add_subsystem, spdk_thread_get_id(group->thread),
				 subsystem->subnqn);

	return rc;
}

static void
_nvmf_poll_group_remove_subsystem_cb(void *ctx, int status)
{
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	spdk_nvmf_poll_group_mod_done cpl_fn = NULL;
	void *cpl_ctx = NULL;
	uint32_t nsid;

	group = qpair_ctx->group;
	subsystem = qpair_ctx->subsystem;
	cpl_fn = qpair_ctx->cpl_fn;
	cpl_ctx = qpair_ctx->cpl_ctx;
	sgroup = &group->sgroups[subsystem->id];

	if (status) {
		goto fini;
	}

	for (nsid = 0; nsid < sgroup->num_ns; nsid++) {
		if (sgroup->ns_info[nsid].channel) {
			spdk_put_io_channel(sgroup->ns_info[nsid].channel);
			sgroup->ns_info[nsid].channel = NULL;
		}
	}

	sgroup->num_ns = 0;
	free(sgroup->ns_info);
	sgroup->ns_info = NULL;
fini:
	free(qpair_ctx);
	if (cpl_fn) {
		cpl_fn(cpl_ctx, status);
	}
}

static void nvmf_poll_group_remove_subsystem_msg(void *ctx);

static void
nvmf_poll_group_remove_subsystem_msg(void *ctx)
{
	struct spdk_nvmf_qpair *qpair, *qpair_tmp;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_poll_group *group;
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx;
	bool qpairs_found = false;
	int rc = 0;

	group = qpair_ctx->group;
	subsystem = qpair_ctx->subsystem;

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, qpair_tmp) {
		if ((qpair->ctrlr != NULL) && (qpair->ctrlr->subsys == subsystem)) {
			qpairs_found = true;
			rc = spdk_nvmf_qpair_disconnect(qpair);
			if (rc && rc != -EINPROGRESS) {
				break;
			}
		}
	}

	if (!qpairs_found) {
		_nvmf_poll_group_remove_subsystem_cb(ctx, 0);
		return;
	}

	/* Some qpairs are in process of being disconnected. Send a message and try to remove them again */
	spdk_thread_send_msg(spdk_get_thread(), nvmf_poll_group_remove_subsystem_msg, ctx);
}

void
nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct nvmf_qpair_disconnect_many_ctx *ctx;
	uint32_t i;

	SPDK_DTRACE_PROBE3_TICKS(nvmf_poll_group_remove_subsystem, group, spdk_thread_get_id(group->thread),
				 subsystem->subnqn);

	ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_many_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Unable to allocate memory for context to remove poll subsystem\n");
		if (cb_fn) {
			cb_fn(cb_arg, -1);
		}
		return;
	}

	ctx->group = group;
	ctx->subsystem = subsystem;
	ctx->cpl_fn = cb_fn;
	ctx->cpl_ctx = cb_arg;

	sgroup = &group->sgroups[subsystem->id];
	sgroup->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;

	for (i = 0; i < sgroup->num_ns; i++) {
		sgroup->ns_info[i].state = SPDK_NVMF_SUBSYSTEM_INACTIVE;
	}

	nvmf_poll_group_remove_subsystem_msg(ctx);
}

void
nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				struct spdk_nvmf_subsystem *subsystem,
				uint32_t nsid,
				spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info = NULL;
	int rc = 0;
	uint32_t i;

	if (subsystem->id >= group->num_sgroups) {
		rc = -1;
		goto fini;
	}

	sgroup = &group->sgroups[subsystem->id];
	if (sgroup->state == SPDK_NVMF_SUBSYSTEM_PAUSED) {
		goto fini;
	}
	sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSING;

	if (nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		for (i = 0; i < sgroup->num_ns; i++) {
			ns_info = &sgroup->ns_info[i];
			ns_info->state = SPDK_NVMF_SUBSYSTEM_PAUSING;
		}
	} else {
		/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
		if (nsid - 1 < sgroup->num_ns) {
			ns_info  = &sgroup->ns_info[nsid - 1];
			ns_info->state = SPDK_NVMF_SUBSYSTEM_PAUSING;
		}
	}

	if (sgroup->mgmt_io_outstanding > 0) {
		assert(sgroup->cb_fn == NULL);
		sgroup->cb_fn = cb_fn;
		assert(sgroup->cb_arg == NULL);
		sgroup->cb_arg = cb_arg;
		return;
	}

	if (nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		for (i = 0; i < sgroup->num_ns; i++) {
			ns_info = &sgroup->ns_info[i];

			if (ns_info->io_outstanding > 0) {
				assert(sgroup->cb_fn == NULL);
				sgroup->cb_fn = cb_fn;
				assert(sgroup->cb_arg == NULL);
				sgroup->cb_arg = cb_arg;
				return;
			}
		}
	} else {
		if (ns_info != NULL && ns_info->io_outstanding > 0) {
			assert(sgroup->cb_fn == NULL);
			sgroup->cb_fn = cb_fn;
			assert(sgroup->cb_arg == NULL);
			sgroup->cb_arg = cb_arg;
			return;
		}
	}

	assert(sgroup->mgmt_io_outstanding == 0);
	sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSED;
fini:
	if (cb_fn) {
		cb_fn(cb_arg, rc);
	}
}

void
nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	int rc = 0;
	uint32_t i;

	if (subsystem->id >= group->num_sgroups) {
		rc = -1;
		goto fini;
	}

	sgroup = &group->sgroups[subsystem->id];

	if (sgroup->state == SPDK_NVMF_SUBSYSTEM_ACTIVE) {
		goto fini;
	}

	rc = poll_group_update_subsystem(group, subsystem);
	if (rc) {
		goto fini;
	}

	for (i = 0; i < sgroup->num_ns; i++) {
		sgroup->ns_info[i].state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	}

	sgroup->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;

	/* Release all queued requests */
	TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) {
		TAILQ_REMOVE(&sgroup->queued, req, link);
		if (spdk_nvmf_request_using_zcopy(req)) {
			spdk_nvmf_request_zcopy_start(req);
		} else {
			spdk_nvmf_request_exec(req);
		}

	}
fini:
	if (cb_fn) {
		cb_fn(cb_arg, rc);
	}
}


struct spdk_nvmf_poll_group *
spdk_nvmf_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	tgroup = nvmf_transport_get_optimal_poll_group(qpair->transport, qpair);

	if (tgroup == NULL) {
		return NULL;
	}

	return tgroup->group;
}

void
spdk_nvmf_poll_group_dump_stat(struct spdk_nvmf_poll_group *group, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", spdk_thread_get_name(spdk_get_thread()));
	spdk_json_write_named_uint32(w, "admin_qpairs", group->stat.admin_qpairs);
	spdk_json_write_named_uint32(w, "io_qpairs", group->stat.io_qpairs);
	spdk_json_write_named_uint32(w, "current_admin_qpairs", group->stat.current_admin_qpairs);
	spdk_json_write_named_uint32(w, "current_io_qpairs", group->stat.current_io_qpairs);
	spdk_json_write_named_uint64(w, "pending_bdev_io", group->stat.pending_bdev_io);
	spdk_json_write_named_uint64(w, "completed_nvme_io", group->stat.completed_nvme_io);

	spdk_json_write_named_array_begin(w, "transports");

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		spdk_json_write_object_begin(w);
		/*
		 * The trtype field intentionally contains a transport name as this is more informative.
		 * The field has not been renamed for backward compatibility.
		 */
		spdk_json_write_named_string(w, "trtype", spdk_nvmf_get_transport_name(tgroup->transport));

		if (tgroup->transport->ops->poll_group_dump_stat) {
			tgroup->transport->ops->poll_group_dump_stat(tgroup, w);
		}

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
}
