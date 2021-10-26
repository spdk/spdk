/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019, 2021 Mellanox Technologies LTD. All rights reserved.
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
static void nvmf_tgt_destroy_poll_group(void *io_device, void *ctx_buf);

/* supplied to a single call to nvmf_qpair_disconnect */
struct nvmf_qpair_disconnect_ctx {
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_ctrlr *ctrlr;
	nvmf_qpair_disconnect_cb cb_fn;
	struct spdk_thread *thread;
	void *ctx;
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
	uint32_t count;
};

static void
nvmf_qpair_set_state(struct spdk_nvmf_qpair *qpair,
		     enum spdk_nvmf_qpair_state state)
{
	assert(qpair != NULL);
	assert(qpair->group->thread == spdk_get_thread());

	qpair->state = state;
}

static int
nvmf_poll_group_poll(void *ctx)
{
	struct spdk_nvmf_poll_group *group = ctx;
	int rc;
	int count = 0;
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		rc = nvmf_transport_poll_group_poll(tgroup);
		if (rc < 0) {
			return SPDK_POLLER_BUSY;
		}
		count += rc;
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
nvmf_tgt_create_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_poll_group *group = ctx_buf;
	struct spdk_nvmf_transport *transport;
	struct spdk_thread *thread = spdk_get_thread();
	uint32_t sid;
	int rc;

	TAILQ_INIT(&group->tgroups);
	TAILQ_INIT(&group->qpairs);
	group->thread = thread;

	TAILQ_FOREACH(transport, &tgt->transports, link) {
		rc = nvmf_poll_group_add_transport(group, transport);
		if (rc != 0) {
			return rc;
		}
	}

	group->num_sgroups = tgt->max_subsystems;
	group->sgroups = calloc(tgt->max_subsystems, sizeof(struct spdk_nvmf_subsystem_poll_group));
	if (!group->sgroups) {
		return -ENOMEM;
	}

	for (sid = 0; sid < tgt->max_subsystems; sid++) {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = tgt->subsystems[sid];
		if (!subsystem) {
			continue;
		}

		if (nvmf_poll_group_add_subsystem(group, subsystem, NULL, NULL) != 0) {
			nvmf_tgt_destroy_poll_group(io_device, ctx_buf);
			return -1;
		}
	}

	pthread_mutex_lock(&tgt->mutex);
	TAILQ_INSERT_TAIL(&tgt->poll_groups, group, link);
	pthread_mutex_unlock(&tgt->mutex);

	group->poller = SPDK_POLLER_REGISTER(nvmf_poll_group_poll, group, 0);

	SPDK_DTRACE_PROBE1(nvmf_create_poll_group, spdk_thread_get_id(thread));

	return 0;
}

static void
nvmf_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
	struct spdk_nvmf_tgt *tgt = io_device;
	struct spdk_nvmf_poll_group *group = ctx_buf;
	struct spdk_nvmf_transport_poll_group *tgroup, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t sid, nsid;

	SPDK_DTRACE_PROBE1(nvmf_destroy_poll_group, spdk_thread_get_id(group->thread));

	pthread_mutex_lock(&tgt->mutex);
	TAILQ_REMOVE(&tgt->poll_groups, group, link);
	pthread_mutex_unlock(&tgt->mutex);

	TAILQ_FOREACH_SAFE(tgroup, &group->tgroups, link, tmp) {
		TAILQ_REMOVE(&group->tgroups, tgroup, link);
		nvmf_transport_poll_group_destroy(tgroup);
	}

	for (sid = 0; sid < group->num_sgroups; sid++) {
		sgroup = &group->sgroups[sid];

		for (nsid = 0; nsid < sgroup->num_ns; nsid++) {
			if (sgroup->ns_info[nsid].channel) {
				spdk_put_io_channel(sgroup->ns_info[nsid].channel);
				sgroup->ns_info[nsid].channel = NULL;
			}
		}

		free(sgroup->ns_info);
	}

	free(group->sgroups);

	spdk_poller_unregister(&group->poller);

	if (group->destroy_cb_fn) {
		group->destroy_cb_fn(group->destroy_cb_arg, 0);
	}
}

static void
_nvmf_tgt_disconnect_next_qpair(void *ctx)
{
	struct spdk_nvmf_qpair *qpair;
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx;
	struct spdk_nvmf_poll_group *group = qpair_ctx->group;
	struct spdk_io_channel *ch;
	int rc = 0;

	qpair = TAILQ_FIRST(&group->qpairs);

	if (qpair) {
		rc = spdk_nvmf_qpair_disconnect(qpair, _nvmf_tgt_disconnect_next_qpair, ctx);
	}

	if (!qpair || rc != 0) {
		/* When the refcount from the channels reaches 0, nvmf_tgt_destroy_poll_group will be called. */
		ch = spdk_io_channel_from_ctx(group);
		spdk_put_io_channel(ch);
		free(qpair_ctx);
	}
}

static void
nvmf_tgt_destroy_poll_group_qpairs(struct spdk_nvmf_poll_group *group)
{
	struct nvmf_qpair_disconnect_many_ctx *ctx;

	SPDK_DTRACE_PROBE1(nvmf_destroy_poll_group_qpairs, spdk_thread_get_id(group->thread));

	ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_many_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate memory for destroy poll group ctx\n");
		return;
	}

	ctx->group = group;
	_nvmf_tgt_disconnect_next_qpair(ctx);
}

struct spdk_nvmf_tgt *
spdk_nvmf_tgt_create(struct spdk_nvmf_target_opts *opts)
{
	struct spdk_nvmf_tgt *tgt, *tmp_tgt;

	if (strnlen(opts->name, NVMF_TGT_NAME_MAX_LENGTH) == NVMF_TGT_NAME_MAX_LENGTH) {
		SPDK_ERRLOG("Provided target name exceeds the max length of %u.\n", NVMF_TGT_NAME_MAX_LENGTH);
		return NULL;
	}

	TAILQ_FOREACH(tmp_tgt, &g_nvmf_tgts, link) {
		if (!strncmp(opts->name, tmp_tgt->name, NVMF_TGT_NAME_MAX_LENGTH)) {
			SPDK_ERRLOG("Provided target name must be unique.\n");
			return NULL;
		}
	}

	tgt = calloc(1, sizeof(*tgt));
	if (!tgt) {
		return NULL;
	}

	snprintf(tgt->name, NVMF_TGT_NAME_MAX_LENGTH, "%s", opts->name);

	if (!opts || !opts->max_subsystems) {
		tgt->max_subsystems = SPDK_NVMF_DEFAULT_MAX_SUBSYSTEMS;
	} else {
		tgt->max_subsystems = opts->max_subsystems;
	}

	if (!opts) {
		tgt->crdt[0] = 0;
		tgt->crdt[1] = 0;
		tgt->crdt[2] = 0;
	} else {
		tgt->crdt[0] = opts->crdt[0];
		tgt->crdt[1] = opts->crdt[1];
		tgt->crdt[2] = opts->crdt[2];
	}

	if (!opts) {
		tgt->discovery_filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
	} else {
		tgt->discovery_filter = opts->discovery_filter;
	}

	tgt->discovery_genctr = 0;
	TAILQ_INIT(&tgt->transports);
	TAILQ_INIT(&tgt->poll_groups);

	tgt->subsystems = calloc(tgt->max_subsystems, sizeof(struct spdk_nvmf_subsystem *));
	if (!tgt->subsystems) {
		free(tgt);
		return NULL;
	}

	pthread_mutex_init(&tgt->mutex, NULL);

	spdk_io_device_register(tgt,
				nvmf_tgt_create_poll_group,
				nvmf_tgt_destroy_poll_group,
				sizeof(struct spdk_nvmf_poll_group),
				tgt->name);

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
	uint32_t i;
	int rc;

	if (tgt->subsystems) {
		for (i = 0; i < tgt->max_subsystems; i++) {
			if (tgt->subsystems[i]) {
				nvmf_subsystem_remove_all_listeners(tgt->subsystems[i], true);

				rc = spdk_nvmf_subsystem_destroy(tgt->subsystems[i], nvmf_tgt_destroy_cb, tgt);
				if (rc) {
					if (rc == -EINPROGRESS) {
						/* If rc is -EINPROGRESS, nvmf_tgt_destroy_cb will be called again when subsystem #i
						 * is destroyed, nvmf_tgt_destroy_cb will continue to destroy other subsystems if any */
						return;
					} else {
						SPDK_ERRLOG("Failed to destroy subsystem, id %u, rc %d\n", tgt->subsystems[i]->id, rc);
						assert(0);
					}
				}
			}
		}
		free(tgt->subsystems);
	}

	_nvmf_tgt_destroy_next_transport(tgt);
}

void
spdk_nvmf_tgt_destroy(struct spdk_nvmf_tgt *tgt,
		      spdk_nvmf_tgt_destroy_done_fn cb_fn,
		      void *cb_arg)
{
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
nvmf_write_subsystem_config_json(struct spdk_json_write_ctx *w,
				 struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host *host;
	struct spdk_nvmf_subsystem_listener *listener;
	const struct spdk_nvme_transport_id *trid;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t max_namespaces;
	char uuid_str[SPDK_UUID_STRING_LEN];

	if (spdk_nvmf_subsystem_get_type(subsystem) != SPDK_NVMF_SUBTYPE_NVME) {
		return;
	}

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
	spdk_json_write_named_bool(w, "ana_reporting", nvmf_subsystem_get_ana_reporting(subsystem));

	/*     } "params" */
	spdk_json_write_object_end(w);

	/* } */
	spdk_json_write_object_end(w);

	for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem); listener != NULL;
	     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {
		trid = spdk_nvmf_subsystem_listener_get_trid(listener);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_listener");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
		nvmf_transport_listen_dump_opts(listener->transport, trid, w);

		/*     } "params" */
		spdk_json_write_object_end(w);

		/* } */
		spdk_json_write_object_end(w);
	}

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "nvmf_subsystem_add_host");

		/*     "params" : { */
		spdk_json_write_named_object_begin(w, "params");

		spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
		spdk_json_write_named_string(w, "host", spdk_nvmf_host_get_nqn(host));

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

		if (nvmf_subsystem_get_ana_reporting(subsystem)) {
			spdk_json_write_named_uint32(w, "anagrpid", ns_opts.anagrpid);
		}

		/*     "namespace" */
		spdk_json_write_object_end(w);

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
#undef SET_FIELD

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_listen_opts) == 16, "Incorrect size");
}

void
spdk_nvmf_listen_opts_init(struct spdk_nvmf_listen_opts *opts, size_t opts_size)
{
	struct spdk_nvmf_listen_opts opts_local = {};

	/* local version of opts should have defaults set here */

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
			  struct spdk_nvme_transport_id *trid)
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

void spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
				 struct spdk_nvmf_transport *transport,
				 spdk_nvmf_tgt_add_transport_done_fn cb_fn,
				 void *cb_arg)
{
	struct spdk_nvmf_tgt_add_transport_ctx *ctx;

	SPDK_DTRACE_PROBE2(nvmf_tgt_add_transport, transport, tgt->name);

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

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	struct spdk_nvmf_subsystem	*subsystem;
	uint32_t sid;

	if (!subnqn) {
		return NULL;
	}

	/* Ensure that subnqn is null terminated */
	if (!memchr(subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1)) {
		SPDK_ERRLOG("Connect SUBNQN is not null terminated\n");
		return NULL;
	}

	for (sid = 0; sid < tgt->max_subsystems; sid++) {
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
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
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
				spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
				return;
			}
		}
		group = tgt->next_poll_group;
		tgt->next_poll_group = TAILQ_NEXT(group, link);
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Unable to send message to poll group.\n");
		spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		return;
	}

	ctx->qpair = qpair;
	ctx->group = group;

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
	int rc = -1;
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_INIT(&qpair->outstanding);
	qpair->group = group;
	qpair->ctrlr = NULL;
	qpair->disconnect_started = false;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			rc = nvmf_transport_poll_group_add(tgroup, qpair);
			break;
		}
	}

	/* We add the qpair to the group only it is successfully added into the tgroup */
	if (rc == 0) {
		SPDK_DTRACE_PROBE2(nvmf_poll_group_add_qpair, qpair, spdk_thread_get_id(group->thread));
		TAILQ_INSERT_TAIL(&group->qpairs, qpair, link);
		nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ACTIVE);
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
	/* Store cb args since cb_ctx can be freed in _nvmf_ctrlr_free_from_qpair */
	nvmf_qpair_disconnect_cb cb_fn = qpair_ctx->cb_fn;
	void *cb_arg = qpair_ctx->ctx;
	struct spdk_thread *cb_thread = qpair_ctx->thread;

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

	if (cb_fn) {
		spdk_thread_send_msg(cb_thread, cb_fn, cb_arg);
	}
}

void
spdk_nvmf_poll_group_remove(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;
	int rc;

	SPDK_DTRACE_PROBE2(nvmf_poll_group_remove_qpair, qpair,
			   spdk_thread_get_id(qpair->group->thread));
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ERROR);

	/* Find the tgroup and remove the qpair from the tgroup */
	TAILQ_FOREACH(tgroup, &qpair->group->tgroups, link) {
		if (tgroup->transport == qpair->transport) {
			rc = nvmf_transport_poll_group_remove(tgroup, qpair);
			if (rc && (rc != ENOTSUP)) {
				SPDK_ERRLOG("Cannot remove qpair=%p from transport group=%p\n",
					    qpair, tgroup);
			}
			break;
		}
	}

	TAILQ_REMOVE(&qpair->group->qpairs, qpair, link);
	qpair->group = NULL;
}

static void
_nvmf_qpair_destroy(void *ctx, int status)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx;
	struct spdk_nvmf_qpair *qpair = qpair_ctx->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_request *req, *tmp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;

	assert(qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING);
	qpair_ctx->qid = qpair->qid;

	if (ctrlr) {
		if (0 == qpair->qid) {
			assert(qpair->group->stat.current_admin_qpairs > 0);
			qpair->group->stat.current_admin_qpairs--;
		} else {
			assert(qpair->group->stat.current_io_qpairs > 0);
			qpair->group->stat.current_io_qpairs--;
		}

		sgroup = &qpair->group->sgroups[ctrlr->subsys->id];
		TAILQ_FOREACH_SAFE(req, &sgroup->queued, link, tmp) {
			if (req->qpair == qpair) {
				TAILQ_REMOVE(&sgroup->queued, req, link);
				if (nvmf_transport_req_free(req)) {
					SPDK_ERRLOG("Transport request free error!/n");
				}
			}
		}
	}

	qpair_ctx->ctrlr = ctrlr;
	spdk_nvmf_poll_group_remove(qpair);
	nvmf_transport_qpair_fini(qpair, _nvmf_transport_qpair_fini_complete, qpair_ctx);
}

static void
_nvmf_qpair_disconnect_msg(void *ctx)
{
	struct nvmf_qpair_disconnect_ctx *qpair_ctx = ctx;

	spdk_nvmf_qpair_disconnect(qpair_ctx->qpair, qpair_ctx->cb_fn, qpair_ctx->ctx);
	free(ctx);
}

int
spdk_nvmf_qpair_disconnect(struct spdk_nvmf_qpair *qpair, nvmf_qpair_disconnect_cb cb_fn, void *ctx)
{
	struct spdk_nvmf_poll_group *group = qpair->group;
	struct nvmf_qpair_disconnect_ctx *qpair_ctx;

	if (__atomic_test_and_set(&qpair->disconnect_started, __ATOMIC_RELAXED)) {
		if (cb_fn) {
			cb_fn(ctx);
		}
		return 0;
	}

	/* If we get a qpair in the uninitialized state, we can just destroy it immediately */
	if (qpair->state == SPDK_NVMF_QPAIR_UNINITIALIZED) {
		nvmf_transport_qpair_fini(qpair, NULL, NULL);
		if (cb_fn) {
			cb_fn(ctx);
		}
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
		qpair_ctx->cb_fn = cb_fn;
		qpair_ctx->thread = group->thread;
		qpair_ctx->ctx = ctx;
		spdk_thread_send_msg(group->thread, _nvmf_qpair_disconnect_msg, qpair_ctx);
		return 0;
	}

	SPDK_DTRACE_PROBE2(nvmf_qpair_disconnect, qpair, spdk_thread_get_id(group->thread));
	assert(qpair->state == SPDK_NVMF_QPAIR_ACTIVE);
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_DEACTIVATING);

	qpair_ctx = calloc(1, sizeof(struct nvmf_qpair_disconnect_ctx));
	if (!qpair_ctx) {
		SPDK_ERRLOG("Unable to allocate context for nvmf_qpair_disconnect\n");
		return -ENOMEM;
	}

	qpair_ctx->qpair = qpair;
	qpair_ctx->cb_fn = cb_fn;
	qpair_ctx->thread = group->thread;
	qpair_ctx->ctx = ctx;

	/* Check for outstanding I/O */
	if (!TAILQ_EMPTY(&qpair->outstanding)) {
		SPDK_DTRACE_PROBE2(nvmf_poll_group_drain_qpair, qpair, spdk_thread_get_id(group->thread));
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
	return nvmf_transport_qpair_get_peer_trid(qpair, trid);
}

int
spdk_nvmf_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	return nvmf_transport_qpair_get_local_trid(qpair, trid);
}

int
spdk_nvmf_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				struct spdk_nvme_transport_id *trid)
{
	return nvmf_transport_qpair_get_listen_trid(qpair, trid);
}

int
nvmf_poll_group_add_transport(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	TAILQ_FOREACH(tgroup, &group->tgroups, link) {
		if (tgroup->transport == transport) {
			/* Transport already in the poll group */
			return 0;
		}
	}

	tgroup = nvmf_transport_poll_group_create(transport);
	if (!tgroup) {
		SPDK_ERRLOG("Unable to create poll group for transport\n");
		return -1;
	}
	SPDK_DTRACE_PROBE2(nvmf_transport_poll_group_create, transport, spdk_thread_get_id(group->thread));

	tgroup->group = group;
	TAILQ_INSERT_TAIL(&group->tgroups, tgroup, link);

	return 0;
}

static int
poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
			    struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	uint32_t new_num_ns, old_num_ns;
	uint32_t i, j;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_registrant *reg, *tmp;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	struct spdk_nvmf_ctrlr *ctrlr;
	bool ns_changed;

	/* Make sure our poll group has memory for this subsystem allocated */
	if (subsystem->id >= group->num_sgroups) {
		return -ENOMEM;
	}

	sgroup = &group->sgroups[subsystem->id];

	/* Make sure the array of namespace information is the correct size */
	new_num_ns = subsystem->max_nsid;
	old_num_ns = sgroup->num_ns;

	ns_changed = false;

	if (old_num_ns == 0) {
		if (new_num_ns > 0) {
			/* First allocation */
			sgroup->ns_info = calloc(new_num_ns, sizeof(struct spdk_nvmf_subsystem_pg_ns_info));
			if (!sgroup->ns_info) {
				return -ENOMEM;
			}
		}
	} else if (new_num_ns > old_num_ns) {
		void *buf;

		/* Make the array larger */
		buf = realloc(sgroup->ns_info, new_num_ns * sizeof(struct spdk_nvmf_subsystem_pg_ns_info));
		if (!buf) {
			return -ENOMEM;
		}

		sgroup->ns_info = buf;

		/* Null out the new namespace information slots */
		for (i = old_num_ns; i < new_num_ns; i++) {
			memset(&sgroup->ns_info[i], 0, sizeof(struct spdk_nvmf_subsystem_pg_ns_info));
		}
	} else if (new_num_ns < old_num_ns) {
		void *buf;

		/* Free the extra I/O channels */
		for (i = new_num_ns; i < old_num_ns; i++) {
			ns_info = &sgroup->ns_info[i];

			if (ns_info->channel) {
				spdk_put_io_channel(ns_info->channel);
				ns_info->channel = NULL;
			}
		}

		/* Make the array smaller */
		if (new_num_ns > 0) {
			buf = realloc(sgroup->ns_info, new_num_ns * sizeof(struct spdk_nvmf_subsystem_pg_ns_info));
			if (!buf) {
				return -ENOMEM;
			}
			sgroup->ns_info = buf;
		} else {
			free(sgroup->ns_info);
			sgroup->ns_info = NULL;
		}
	}

	sgroup->num_ns = new_num_ns;

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
		}

		if (ns == NULL) {
			memset(ns_info, 0, sizeof(*ns_info));
		} else {
			ns_info->uuid = *spdk_bdev_get_uuid(ns->bdev);
			ns_info->num_blocks = spdk_bdev_get_num_blocks(ns->bdev);
			ns_info->crkey = ns->crkey;
			ns_info->rtype = ns->rtype;
			if (ns->holder) {
				ns_info->holder_id = ns->holder->hostid;
			}

			memset(&ns_info->reg_hostid, 0, SPDK_NVMF_MAX_NUM_REGISTRANTS * sizeof(struct spdk_uuid));
			j = 0;
			TAILQ_FOREACH_SAFE(reg, &ns->registrants, link, tmp) {
				if (j >= SPDK_NVMF_MAX_NUM_REGISTRANTS) {
					SPDK_ERRLOG("Maximum %u registrants can support.\n", SPDK_NVMF_MAX_NUM_REGISTRANTS);
					return -EINVAL;
				}
				ns_info->reg_hostid[j++] = reg->hostid;
			}
		}
	}

	if (ns_changed) {
		TAILQ_FOREACH(ctrlr, &subsystem->ctrlrs, link) {
			/* It is possible that a ctrlr was added but the admin_qpair hasn't been
			 * assigned yet.
			 */
			if (!ctrlr->admin_qpair) {
				continue;
			}
			if (ctrlr->admin_qpair->group == group) {
				nvmf_ctrlr_async_event_ns_notice(ctrlr);
				nvmf_ctrlr_async_event_ana_change_notice(ctrlr);
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
	uint32_t i;

	TAILQ_INIT(&sgroup->queued);

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

	SPDK_DTRACE_PROBE2(nvmf_poll_group_add_subsystem, spdk_thread_get_id(group->thread),
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
remove_subsystem_qpair_cb(void *ctx)
{
	struct nvmf_qpair_disconnect_many_ctx *qpair_ctx = ctx;

	assert(qpair_ctx->count > 0);
	qpair_ctx->count--;
	if (qpair_ctx->count == 0) {
		/* All of the asynchronous callbacks for this context have been
		 * completed.  Call nvmf_poll_group_remove_subsystem_msg() again
		 * to check if all associated qpairs for this subsystem have
		 * been removed from the poll group.
		 */
		nvmf_poll_group_remove_subsystem_msg(ctx);
	}
}

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

	/* Initialize count to 1.  This acts like a ref count, to ensure that if spdk_nvmf_qpair_disconnect
	 * immediately invokes the callback (i.e. the qpairs is already in process of being disconnected)
	 * that we don't recursively call nvmf_poll_group_remove_subsystem_msg before we've iterated the
	 * full list of qpairs.
	 */
	qpair_ctx->count = 1;
	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, qpair_tmp) {
		if ((qpair->ctrlr != NULL) && (qpair->ctrlr->subsys == subsystem)) {
			qpairs_found = true;
			qpair_ctx->count++;
			rc = spdk_nvmf_qpair_disconnect(qpair, remove_subsystem_qpair_cb, ctx);
			if (rc) {
				break;
			}
		}
	}
	qpair_ctx->count--;

	if (!qpairs_found) {
		_nvmf_poll_group_remove_subsystem_cb(ctx, 0);
		return;
	}

	if (qpair_ctx->count == 0 || rc) {
		/* If count == 0, it means there were some qpairs in the poll group but they
		 * were already in process of being disconnected.  So we send a message to this
		 * same thread so that this function executes again later.  We won't actually
		 * invoke the remove_subsystem_cb until all of the qpairs are actually removed
		 * from the poll group.
		 */
		spdk_thread_send_msg(spdk_get_thread(), nvmf_poll_group_remove_subsystem_msg, ctx);
	}
}

void
nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct nvmf_qpair_disconnect_many_ctx *ctx;
	uint32_t i;

	SPDK_DTRACE_PROBE3(nvmf_poll_group_remove_subsystem, group, spdk_thread_get_id(group->thread),
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

	if (subsystem->id >= group->num_sgroups) {
		rc = -1;
		goto fini;
	}

	sgroup = &group->sgroups[subsystem->id];
	if (sgroup->state == SPDK_NVMF_SUBSYSTEM_PAUSED) {
		goto fini;
	}
	sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSING;

	/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
	if (nsid - 1 < sgroup->num_ns) {
		ns_info  = &sgroup->ns_info[nsid - 1];
		ns_info->state = SPDK_NVMF_SUBSYSTEM_PAUSING;
	}

	if (sgroup->mgmt_io_outstanding > 0) {
		assert(sgroup->cb_fn == NULL);
		sgroup->cb_fn = cb_fn;
		assert(sgroup->cb_arg == NULL);
		sgroup->cb_arg = cb_arg;
		return;
	}

	if (ns_info != NULL && ns_info->io_outstanding > 0) {
		assert(sgroup->cb_fn == NULL);
		sgroup->cb_fn = cb_fn;
		assert(sgroup->cb_arg == NULL);
		sgroup->cb_arg = cb_arg;
		return;
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
