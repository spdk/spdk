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

#include "spdk/bdev.h"
#include "spdk/bit_array.h"
#include "spdk/conf.h"
#include "spdk/thread.h"
#include "spdk/nvmf.h"
#include "spdk/trace.h"
#include "spdk/endian.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

#include "nvmf_internal.h"
#include "transport.h"

SPDK_LOG_REGISTER_COMPONENT("nvmf", SPDK_LOG_NVMF)

#define SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_DEFAULT_MAX_QPAIRS_PER_CTRLR 64
#define SPDK_NVMF_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_DEFAULT_MAX_IO_SIZE 131072
#define SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS 1024
#define SPDK_NVMF_DEFAULT_IO_UNIT_SIZE 131072

void
spdk_nvmf_tgt_opts_init(struct spdk_nvmf_tgt_opts *opts)
{
	opts->max_queue_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr = SPDK_NVMF_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size = SPDK_NVMF_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size = SPDK_NVMF_DEFAULT_MAX_IO_SIZE;
	opts->max_subsystems = SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS;
	opts->io_unit_size = SPDK_NVMF_DEFAULT_IO_UNIT_SIZE;
}

static int
spdk_nvmf_poll_group_poll(void *ctx)
{
	struct spdk_nvmf_poll_group *group = ctx;
	int rc;
	int count = 0;
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		rc = spdk_nvmf_transport_poll_group_poll(tgroup);
		if (rc < 0) {
			return -1;
		}
		count += rc;
	}

	return count;
}

static int
spdk_nvmf_tgt_create_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_poll_group *group = ctx_buf;
	struct spdk_nvmf_transport *transport;
	uint32_t sid;

	TAILQ_INIT(&group->tgroups);
	TAILQ_INIT(&group->qpairs);

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		spdk_nvmf_poll_group_add_transport(group, transport);
	}

	group->num_sgroups = tgt->opts.max_subsystems;
	group->sgroups = calloc(tgt->opts.max_subsystems, sizeof(struct spdk_nvmf_subsystem_poll_group));
	if (!group->sgroups) {
		return -1;
	}

	for (sid = 0; sid < tgt->opts.max_subsystems; sid++) {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = tgt->subsystems[sid];
		if (!subsystem) {
			continue;
		}

		spdk_nvmf_poll_group_add_subsystem(group, subsystem);
	}

	group->poller = spdk_poller_register(spdk_nvmf_poll_group_poll, group, 0);
	group->thread = spdk_get_thread();

	return 0;
}

static void
spdk_nvmf_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_poll_group *group = ctx_buf;
	struct spdk_nvmf_qpair *qpair, *qptmp;
	struct spdk_nvmf_transport_poll_group *tgroup, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t sid, nsid;

	spdk_poller_unregister(&group->poller);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, qptmp) {
		spdk_nvmf_qpair_disconnect(qpair);
	}

	TAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp) {
		TAILQ_REMOVE(&group->tgroups, tgroup, link);
		spdk_nvmf_transport_poll_group_destroy(tgroup);
	}

	for (sid = 0; sid < group->num_sgroups; sid++) {
		sgroup = &group->sgroups[sid];

		for (nsid = 0; nsid < sgroup->num_channels; nsid++) {
			if (sgroup->channels[nsid]) {
				spdk_put_io_channel(sgroup->channels[nsid]);
				sgroup->channels[nsid] = NULL;
			}
		}

		free(sgroup->channels);
	}

	free(group->sgroups);
}

struct spdk_nvmf_tgt *
spdk_nvmf_tgt_create(struct spdk_nvmf_tgt_opts *opts)
{
	struct spdk_nvmf_tgt *tgt;

	tgt = calloc(1, sizeof(*tgt));
	if (!tgt) {
		return NULL;
	}

	if (!opts) {
		spdk_nvmf_tgt_opts_init(&tgt->opts);
	} else {
		tgt->opts = *opts;
	}

	if ((tgt->opts.max_io_size % tgt->opts.io_unit_size != 0) ||
	    (tgt->opts.max_io_size / tgt->opts.io_unit_size > SPDK_NVMF_MAX_SGL_ENTRIES)) {
		SPDK_ERRLOG("Unsupported IO size, MaxIO:%d, UnitIO:%d\n", tgt->opts.max_io_size,
			    tgt->opts.io_unit_size);
		free(tgt);
		return NULL;
	}

	tgt->discovery_genctr = 0;
	tgt->discovery_log_page = NULL;
	tgt->discovery_log_page_size = 0;
	TAILQ_INIT(&tgt->transports);

	tgt->subsystems = calloc(tgt->opts.max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	if (!tgt->subsystems) {
		free(tgt);
		return NULL;
	}

	spdk_io_device_register(tgt,
				spdk_nvmf_tgt_create_poll_group,
				spdk_nvmf_tgt_destroy_poll_group,
				sizeof(struct spdk_nvmf_poll_group));

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Max Queue Pairs Per Controller: %d\n",
		      tgt->opts.max_qpairs_per_ctrlr);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Max Queue Depth: %d\n", tgt->opts.max_queue_depth);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Max In Capsule Data: %d bytes\n",
		      tgt->opts.in_capsule_data_size);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Max I/O Size: %d bytes\n", tgt->opts.max_io_size);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "I/O Unit Size: %d bytes\n", tgt->opts.io_unit_size);

	return tgt;
}

static void
spdk_nvmf_tgt_destroy_cb(void *io_device)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_transport *transport, *transport_tmp;
	spdk_nvmf_tgt_destroy_done_fn		*destroy_cb_fn;
	void					*destroy_cb_arg;
	uint32_t i;

	if (tgt->discovery_log_page) {
		free(tgt->discovery_log_page);
	}

	if (tgt->subsystems) {
		for (i = 0; i < tgt->opts.max_subsystems; i++) {
			if (tgt->subsystems[i]) {
				spdk_nvmf_subsystem_destroy(tgt->subsystems[i]);
			}
		}
		free(tgt->subsystems);
	}

	TAILQ_FOREACH_SAFE(transport, &tgt->transports, link, transport_tmp) {
		TAILQ_REMOVE(&tgt->transports, transport, link);
		spdk_nvmf_transport_destroy(transport);
	}

	destroy_cb_fn = tgt->destroy_cb_fn;
	destroy_cb_arg = tgt->destroy_cb_arg;

	free(tgt);

	if (destroy_cb_fn) {
		destroy_cb_fn(destroy_cb_arg, 0);
	}
}

void
spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt,
		      spdk_nvmf_tgt_destroy_done_fn cb_fn,
		      void *cb_arg)
{
	tgt->destroy_cb_fn = cb_fn;
	tgt->destroy_cb_arg = cb_arg;

	spdk_io_device_unregister(tgt, spdk_nvmf_tgt_destroy_cb);
}

struct spdk_nvmf_tgt_listen_ctx {
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvme_transport_id trid;

	spdk_nvmf_tgt_listen_done_fn cb_fn;
	void *cb_arg;
};

static void
spdk_nvmf_write_subsystem_config_json(struct spdk_json_write_ctx *w,
				      struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host *host;
	struct spdk_nvmf_listener *listener;
	const struct spdk_nvme_transport_id *trid;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t max_namespaces;
	char uuid_str[SPDK_UUID_STRING_LEN];
	const char *trtype;
	const char *adrfam;

	if (spdk_nvmf_subsystem_get_type(subsystem) != SPDK_NVMF_SUBTYPE_NVME) {
		return;
	}

	/* { */
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "construct_nvmf_subsystem");

	/*     "params" : { */
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
	spdk_json_write_named_bool(w, "allow_any_host", spdk_nvmf_subsystem_get_allow_any_host(subsystem));

	/*         "listen_addresses" : [ */
	spdk_json_write_named_array_begin(w, "listen_addresses");
	for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem); listener != NULL;
	     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {
		trid = spdk_nvmf_listener_get_trid(listener);

		trtype = spdk_nvme_transport_id_trtype_str(trid->trtype);
		adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);

		/*        { */
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "trtype", trtype);
		if (adrfam) {
			spdk_json_write_named_string(w, "adrfam", adrfam);
		}

		spdk_json_write_named_string(w, "traddr", trid->traddr);
		spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
		spdk_json_write_object_end(w);
		/*        } */
	}
	spdk_json_write_array_end(w);
	/*         ] "listen_addresses" */

	/*         "hosts" : [ */
	spdk_json_write_named_array_begin(w, "hosts");
	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {
		spdk_json_write_string(w, spdk_nvmf_host_get_nqn(host));
	}
	spdk_json_write_array_end(w);
	/*         ] "hosts" */

	spdk_json_write_named_string(w, "serial_number", spdk_nvmf_subsystem_get_sn(subsystem));

	max_namespaces = spdk_nvmf_subsystem_get_max_namespaces(subsystem);
	if (max_namespaces != 0) {
		spdk_json_write_named_uint32(w, "max_namespaces", max_namespaces);
	}

	/*         "namespaces" : [ */
	spdk_json_write_named_array_begin(w, "namespaces");
	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts));

		/*         { */
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "nsid", spdk_nvmf_ns_get_id(ns));
		spdk_json_write_named_string(w, "bdev_name", spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));

		if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) {
			SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(uint64_t) * 2, "size mismatch");
			spdk_json_write_named_string_fmt(w, "nguid", "%016"PRIX64"%016"PRIX64, from_be64(&ns_opts.nguid[0]),
							 from_be64(&ns_opts.nguid[8]));
		}

		if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) {
			SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(uint64_t), "size mismatch");
			spdk_json_write_named_string_fmt(w, "eui64", "%016"PRIX64, from_be64(&ns_opts.eui64));
		}

		if (!spdk_mem_all_zero(&ns_opts.uuid, sizeof(ns_opts.uuid))) {
			spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &ns_opts.uuid);
			spdk_json_write_named_string(w, "uuid",  uuid_str);
		}
		/*         } */
		spdk_json_write_object_end(w);
	}

	/*         ] "namespaces" */
	spdk_json_write_array_end(w);

	/*     } "params" */
	spdk_json_write_object_end(w);

	/* } */
	spdk_json_write_object_end(w);
}

void
spdk_nvmf_tgt_write_config_json(struct spdk_json_write_ctx *w, struct spdk_nvmf_tgt *tgt)
{
	struct spdk_nvmf_subsystem *subsystem;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "set_nvmf_target_options");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "max_queue_depth", tgt->opts.max_queue_depth);
	spdk_json_write_named_uint32(w, "max_qpairs_per_ctrlr", tgt->opts.max_qpairs_per_ctrlr);
	spdk_json_write_named_uint32(w, "in_capsule_data_size", tgt->opts.in_capsule_data_size);
	spdk_json_write_named_uint32(w, "max_io_size", tgt->opts.max_io_size);
	spdk_json_write_named_uint32(w, "max_subsystems", tgt->opts.max_subsystems);
	spdk_json_write_named_uint32(w, "io_unit_size", tgt->opts.io_unit_size);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	subsystem = spdk_nvmf_subsystem_get_first(tgt);
	while (subsystem) {
		spdk_nvmf_write_subsystem_config_json(w, subsystem);
		subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	}
}

static void
spdk_nvmf_tgt_listen_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_tgt_listen_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	ctx->cb_fn(ctx->cb_arg, status);

	free(ctx);
}

static void
spdk_nvmf_tgt_listen_add_transport(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_tgt_listen_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_nvmf_poll_group_add_transport(group, ctx->transport);
	spdk_for_each_channel_continue(i, rc);
}

void
spdk_nvmf_tgt_listen(struct spdk_nvmf_tgt *tgt,
		     struct spdk_nvme_transport_id *trid,
		     spdk_nvmf_tgt_listen_done_fn cb_fn,
		     void *cb_arg)
{
	struct spdk_nvmf_transport *transport;
	int rc;
	bool propagate = false;

	transport = spdk_nvmf_tgt_get_transport(tgt, trid->trtype);
	if (!transport) {
		transport = spdk_nvmf_transport_create(tgt, trid->trtype);
		if (!transport) {
			SPDK_ERRLOG("Transport initialization failed\n");
			cb_fn(cb_arg, -EINVAL);
			return;
		}
		TAILQ_INSERT_TAIL(&tgt->transports, transport, link);

		propagate = true;
	}

	rc = spdk_nvmf_transport_listen(transport, trid);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to listen on address '%s'\n", trid->traddr);
		cb_fn(cb_arg, rc);
		return;
	}

	tgt->discovery_genctr++;

	if (propagate) {
		struct spdk_nvmf_tgt_listen_ctx *ctx;

		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			cb_fn(cb_arg, -ENOMEM);
			return;
		}

		ctx->tgt = tgt;
		ctx->transport = transport;
		ctx->trid = *trid;
		ctx->cb_fn = cb_fn;
		ctx->cb_arg = cb_arg;

		spdk_for_each_channel(tgt,
				      spdk_nvmf_tgt_listen_add_transport,
				      ctx,
				      spdk_nvmf_tgt_listen_done);
	} else {
		cb_fn(cb_arg, 0);
	}
}

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;
	uint32_t sid;

	if (!subnqn) {
		return NULL;
	}

	for (sid = 0; sid < tgt->opts.max_subsystems; sid++) {
		subsystem = tgt->subsystems[sid];
		if (subsystem == NULL) {
			continue;
		}

		if (strcmp(subnqn, subsystem->subnqn) == 0) {
			return subsystem;
		}
	}

	return NULL;
}

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, enum spdk_nvme_transport_type type)
{
	struct spdk_nvmf_transport *transport;

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		if (transport->ops->type == type) {
			return transport;
		}
	}

	return NULL;
}

void
spdk_nvmf_tgt_accept(struct spdk_nvmf_tgt *tgt, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_transport *transport, *tmp;

	TAILQ_FOREACH_SAFE(transport, &tgt->transports, link, tmp) {
		spdk_nvmf_transport_accept(transport, cb_fn);
	}
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
spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group)
{
	struct spdk_io_channel *ch;

	ch = spdk_io_channel_from_ctx(group);
	spdk_put_io_channel(ch);
}

int
spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	int rc = -1;
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_INIT(&qpair->outstanding);
	qpair->group = group;
	qpair->state = SPDK_NVMF_QPAIR_ACTIVATING;

	TAILQ_INSERT_TAIL(&group->qpairs, qpair, link);

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			rc = spdk_nvmf_transport_poll_group_add(tgroup, qpair);
			break;
		}
	}

	if (rc == 0) {
		qpair->state = SPDK_NVMF_QPAIR_ACTIVE;
	} else {
		qpair->state = SPDK_NVMF_QPAIR_INACTIVE;
	}

	return rc;
}

int
spdk_nvmf_poll_group_remove(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	int rc = -1;
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_REMOVE(&group->qpairs, qpair, link);

	qpair->group = NULL;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			rc = spdk_nvmf_transport_poll_group_remove(tgroup, qpair);
			break;
		}
	}

	return rc;
}

static void
_spdk_nvmf_ctrlr_free(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;

	spdk_nvmf_ctrlr_destruct(ctrlr);
}

static void
_spdk_nvmf_qpair_destroy(void *ctx, int status)
{
	struct spdk_nvmf_qpair *qpair = ctx;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	uint16_t qid = qpair->qid;
	uint32_t count;

	spdk_nvmf_poll_group_remove(qpair->group, qpair);

	assert(qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING);
	qpair->state = SPDK_NVMF_QPAIR_INACTIVE;

	spdk_nvmf_transport_qpair_fini(qpair);

	if (!ctrlr) {
		return;
	}

	pthread_mutex_lock(&ctrlr->mtx);
	spdk_bit_array_clear(ctrlr->qpair_mask, qid);
	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	pthread_mutex_unlock(&ctrlr->mtx);

	if (count == 0) {
		/* If this was the last queue pair on the controller, also send a message
		 * to the subsystem to remove the controller. */
		spdk_thread_send_msg(ctrlr->subsys->thread, _spdk_nvmf_ctrlr_free, ctrlr);
	}
}

static void
_spdk_nvmf_qpair_deactivate(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;

	if (qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING ||
	    qpair->state == SPDK_NVMF_QPAIR_INACTIVE) {
		/* This can occur if the connection is killed by the target,
		 * which results in a notification that the connection
		 * died. */
		return;
	}

	assert(qpair->state == SPDK_NVMF_QPAIR_ACTIVE);
	qpair->state = SPDK_NVMF_QPAIR_DEACTIVATING;

	/* Check for outstanding I/O */
	if (!TAILQ_EMPTY(&qpair->outstanding)) {
		qpair->state_cb = _spdk_nvmf_qpair_destroy;
		qpair->state_cb_arg = qpair;
		return;
	}

	_spdk_nvmf_qpair_destroy(qpair, 0);
}

void
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair)
{
	if (qpair->group->thread == spdk_get_thread()) {
		_spdk_nvmf_qpair_deactivate(qpair);
	} else {
		/* Send a message to the thread that owns this qpair */
		spdk_thread_send_msg(qpair->group->thread, _spdk_nvmf_qpair_deactivate, qpair);
	}
}

int
spdk_nvmf_poll_group_add_transport(struct spdk_nvmf_poll_group *group,
				   struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == transport) {
			/* Transport already in the poll group */
			return 0;
		}
	}

	tgroup = spdk_nvmf_transport_poll_group_create(transport);
	if (!tgroup) {
		SPDK_ERRLOG("Unable to create poll group for transport\n");
		return -1;
	}

	TAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);

	return 0;
}

static int
poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t new_num_channels, old_num_channels;
	uint32_t i;
	struct spdk_nvmf_ns *ns;

	/* Make sure our poll group has memory for this subsystem allocated */
	if (subsystem->id >= group->num_sgroups) {
		return -ENOMEM;
	}

	sgroup = &group->sgroups[subsystem->id];

	/* Make sure the array of channels is the correct size */
	new_num_channels = subsystem->max_nsid;
	old_num_channels = sgroup->num_channels;

	if (old_num_channels == 0) {
		if (new_num_channels > 0) {
			/* First allocation */
			sgroup->channels = calloc(new_num_channels, sizeof(sgroup->channels[0]));
			if (!sgroup->channels) {
				return -ENOMEM;
			}
		}
	} else if (new_num_channels > old_num_channels) {
		void *buf;

		/* Make the array larger */
		buf = realloc(sgroup->channels, new_num_channels * sizeof(sgroup->channels[0]));
		if (!buf) {
			return -ENOMEM;
		}

		sgroup->channels = buf;

		/* Null out the new channels slots */
		for (i = old_num_channels; i < new_num_channels; i++) {
			sgroup->channels[i] = NULL;
		}
	} else if (new_num_channels < old_num_channels) {
		void *buf;

		/* Free the extra I/O channels */
		for (i = new_num_channels; i < old_num_channels; i++) {
			if (sgroup->channels[i]) {
				spdk_put_io_channel(sgroup->channels[i]);
				sgroup->channels[i] = NULL;
			}
		}

		/* Make the array smaller */
		if (new_num_channels > 0) {
			buf = realloc(sgroup->channels, new_num_channels * sizeof(sgroup->channels[0]));
			if (!buf) {
				return -ENOMEM;
			}
			sgroup->channels = buf;
		} else {
			free(sgroup->channels);
			sgroup->channels = NULL;
		}
	}

	sgroup->num_channels = new_num_channels;

	/* Detect bdevs that were added or removed */
	for (i = 0; i < sgroup->num_channels; i++) {
		ns = subsystem->ns[i];
		if (ns == NULL && sgroup->channels[i] == NULL) {
			/* Both NULL. Leave empty */
		} else if (ns == NULL && sgroup->channels[i] != NULL) {
			/* There was a channel here, but the namespace is gone. */
			spdk_put_io_channel(sgroup->channels[i]);
			sgroup->channels[i] = NULL;
		} else if (ns != NULL && sgroup->channels[i] == NULL) {
			/* A namespace appeared but there is no channel yet */
			sgroup->channels[i] = spdk_bdev_get_io_channel(ns->desc);
		} else {
			/* A namespace was present before and didn't change. */
		}
	}

	return 0;
}

int
spdk_nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem)
{
	return poll_group_update_subsystem(group, subsystem);
}

int
spdk_nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
				   struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	int rc;

	rc = poll_group_update_subsystem(group, subsystem);
	if (rc) {
		return rc;
	}

	sgroup = &group->sgroups[subsystem->id];
	sgroup->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	TAILQ_INIT(&sgroup->queued);

	return 0;
}

int
spdk_nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_qpair *qpair, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t nsid;

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp) {
		if (qpair->ctrlr->subsys == subsystem) {
			spdk_nvmf_qpair_disconnect(qpair);
		}
	}

	sgroup = &group->sgroups[subsystem->id];
	sgroup->state = SPDK_NVMF_SUBSYSTEM_INACTIVE;

	for (nsid = 0; nsid < sgroup->num_channels; nsid++) {
		if (sgroup->channels[nsid]) {
			spdk_put_io_channel(sgroup->channels[nsid]);
			sgroup->channels[nsid] = NULL;
		}
	}

	sgroup->num_channels = 0;
	free(sgroup->channels);
	sgroup->channels = NULL;

	return 0;
}

int
spdk_nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				     struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;

	if (subsystem->id >= group->num_sgroups) {
		return -1;
	}

	sgroup = &group->sgroups[subsystem->id];
	if (sgroup == NULL) {
		return -1;
	}

	assert(sgroup->state == SPDK_NVMF_SUBSYSTEM_ACTIVE);
	/* TODO: This currently does not quiesce I/O */
	sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSED;

	return 0;
}

int
spdk_nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				      struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	int rc;

	if (subsystem->id >= group->num_sgroups) {
		return -1;
	}

	sgroup = &group->sgroups[subsystem->id];

	assert(sgroup->state == SPDK_NVMF_SUBSYSTEM_PAUSED);

	rc = poll_group_update_subsystem(group, subsystem);
	if (rc) {
		return rc;
	}

	sgroup->state = SPDK_NVMF_SUBSYSTEM_ACTIVE;

	/* Release all queued requests */
	TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) {
		TAILQ_REMOVE(&sgroup->queued, req, link);
		spdk_nvmf_request_exec(req);
	}

	return 0;
}
