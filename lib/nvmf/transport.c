/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_transport.h"
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk_internal/usdt.h"

#define NVMF_TRANSPORT_DEFAULT_ASSOCIATION_TIMEOUT_IN_MS 120000

struct nvmf_transport_ops_list_element {
	struct spdk_nvmf_transport_ops			ops;
	TAILQ_ENTRY(nvmf_transport_ops_list_element)	link;
};

TAILQ_HEAD(nvmf_transport_ops_list, nvmf_transport_ops_list_element)
g_spdk_nvmf_transport_ops = TAILQ_HEAD_INITIALIZER(g_spdk_nvmf_transport_ops);

static inline const struct spdk_nvmf_transport_ops *
nvmf_get_transport_ops(const char *transport_name)
{
	struct nvmf_transport_ops_list_element *ops;
	TAILQ_FOREACH(ops, &g_spdk_nvmf_transport_ops, link) {
		if (strcasecmp(transport_name, ops->ops.name) == 0) {
			return &ops->ops;
		}
	}
	return NULL;
}

void
spdk_nvmf_transport_register(const struct spdk_nvmf_transport_ops *ops)
{
	struct nvmf_transport_ops_list_element *new_ops;

	if (nvmf_get_transport_ops(ops->name) != NULL) {
		SPDK_ERRLOG("Double registering nvmf transport type %s.\n", ops->name);
		assert(false);
		return;
	}

	new_ops = calloc(1, sizeof(*new_ops));
	if (new_ops == NULL) {
		SPDK_ERRLOG("Unable to allocate memory to register new transport type %s.\n", ops->name);
		assert(false);
		return;
	}

	new_ops->ops = *ops;

	TAILQ_INSERT_TAIL(&g_spdk_nvmf_transport_ops, new_ops, link);
}

const struct spdk_nvmf_transport_opts *
spdk_nvmf_get_transport_opts(struct spdk_nvmf_transport *transport)
{
	return &transport->opts;
}

void
nvmf_transport_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w,
			 bool named)
{
	const struct spdk_nvmf_transport_opts *opts = spdk_nvmf_get_transport_opts(transport);

	named ? spdk_json_write_named_object_begin(w, "params") : spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "trtype", spdk_nvmf_get_transport_name(transport));
	spdk_json_write_named_uint32(w, "max_queue_depth", opts->max_queue_depth);
	spdk_json_write_named_uint32(w, "max_io_qpairs_per_ctrlr", opts->max_qpairs_per_ctrlr - 1);
	spdk_json_write_named_uint32(w, "in_capsule_data_size", opts->in_capsule_data_size);
	spdk_json_write_named_uint32(w, "max_io_size", opts->max_io_size);
	spdk_json_write_named_uint32(w, "io_unit_size", opts->io_unit_size);
	spdk_json_write_named_uint32(w, "max_aq_depth", opts->max_aq_depth);
	spdk_json_write_named_uint32(w, "num_shared_buffers", opts->num_shared_buffers);
	spdk_json_write_named_uint32(w, "buf_cache_size", opts->buf_cache_size);
	spdk_json_write_named_bool(w, "dif_insert_or_strip", opts->dif_insert_or_strip);
	spdk_json_write_named_bool(w, "zcopy", opts->zcopy);

	if (transport->ops->dump_opts) {
		transport->ops->dump_opts(transport, w);
	}

	spdk_json_write_named_uint32(w, "abort_timeout_sec", opts->abort_timeout_sec);
	spdk_json_write_named_uint32(w, "ack_timeout", opts->ack_timeout);
	spdk_json_write_named_uint32(w, "data_wr_pool_size", opts->data_wr_pool_size);
	spdk_json_write_object_end(w);
}

void
nvmf_transport_listen_dump_trid(const struct spdk_nvme_transport_id *trid,
				struct spdk_json_write_ctx *w)
{
	const char *adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);

	spdk_json_write_named_string(w, "trtype", trid->trstring);
	spdk_json_write_named_string(w, "adrfam", adrfam ? adrfam : "unknown");
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
}

spdk_nvme_transport_type_t
spdk_nvmf_get_transport_type(struct spdk_nvmf_transport *transport)
{
	return transport->ops->type;
}

const char *
spdk_nvmf_get_transport_name(struct spdk_nvmf_transport *transport)
{
	return transport->ops->name;
}

static void
nvmf_transport_opts_copy(struct spdk_nvmf_transport_opts *opts,
			 struct spdk_nvmf_transport_opts *opts_src,
			 size_t opts_size)
{
	assert(opts);
	assert(opts_src);

	opts->opts_size = opts_size;

#define SET_FIELD(field) \
	if (offsetof(struct spdk_nvmf_transport_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = opts_src->field; \
	} \

	SET_FIELD(max_queue_depth);
	SET_FIELD(max_qpairs_per_ctrlr);
	SET_FIELD(in_capsule_data_size);
	SET_FIELD(max_io_size);
	SET_FIELD(io_unit_size);
	SET_FIELD(max_aq_depth);
	SET_FIELD(buf_cache_size);
	SET_FIELD(num_shared_buffers);
	SET_FIELD(dif_insert_or_strip);
	SET_FIELD(abort_timeout_sec);
	SET_FIELD(association_timeout);
	SET_FIELD(transport_specific);
	SET_FIELD(acceptor_poll_rate);
	SET_FIELD(zcopy);
	SET_FIELD(ack_timeout);
	SET_FIELD(data_wr_pool_size);

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_transport_opts) == 72, "Incorrect size");

#undef SET_FIELD
#undef FILED_CHECK
}

struct nvmf_transport_create_ctx {
	const struct spdk_nvmf_transport_ops *ops;
	struct spdk_nvmf_transport_opts opts;
	void *cb_arg;
	spdk_nvmf_transport_create_done_cb cb_fn;
};

static bool
nvmf_transport_use_iobuf(struct spdk_nvmf_transport *transport)
{
	return transport->opts.num_shared_buffers || transport->opts.buf_cache_size;
}

static void
nvmf_transport_create_async_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct nvmf_transport_create_ctx *ctx = cb_arg;
	int chars_written;

	if (!transport) {
		SPDK_ERRLOG("Failed to create transport.\n");
		goto err;
	}

	pthread_mutex_init(&transport->mutex, NULL);
	TAILQ_INIT(&transport->listeners);
	transport->ops = ctx->ops;
	transport->opts = ctx->opts;
	chars_written = snprintf(transport->iobuf_name, MAX_MEMPOOL_NAME_LENGTH, "%s_%s", "nvmf",
				 transport->ops->name);
	if (chars_written < 0) {
		SPDK_ERRLOG("Unable to generate transport data buffer pool name.\n");
		goto err;
	}

	if (nvmf_transport_use_iobuf(transport)) {
		spdk_iobuf_register_module(transport->iobuf_name);
	}

	ctx->cb_fn(ctx->cb_arg, transport);
	free(ctx);
	return;

err:
	if (transport) {
		transport->ops->destroy(transport, NULL, NULL);
	}

	ctx->cb_fn(ctx->cb_arg, NULL);
	free(ctx);
}

static void
_nvmf_transport_create_done(void *ctx)
{
	struct nvmf_transport_create_ctx *_ctx = (struct nvmf_transport_create_ctx *)ctx;

	nvmf_transport_create_async_done(_ctx, _ctx->ops->create(&_ctx->opts));
}

static int
nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts,
		      spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg, bool sync)
{
	struct nvmf_transport_create_ctx *ctx;
	struct spdk_iobuf_opts opts_iobuf = {};
	int rc;
	uint64_t count;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		goto err;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("The opts_size in opts structure should not be zero\n");
		goto err;
	}

	ctx->ops = nvmf_get_transport_ops(transport_name);
	if (!ctx->ops) {
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		goto err;
	}

	nvmf_transport_opts_copy(&ctx->opts, opts, opts->opts_size);
	if (ctx->opts.max_io_size != 0 && (!spdk_u32_is_pow2(ctx->opts.max_io_size) ||
					   ctx->opts.max_io_size < 8192)) {
		SPDK_ERRLOG("max_io_size %u must be a power of 2 and be greater than or equal 8KB\n",
			    ctx->opts.max_io_size);
		goto err;
	}

	if (ctx->opts.max_aq_depth < SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE) {
		SPDK_ERRLOG("max_aq_depth %u is less than minimum defined by NVMf spec, use min value\n",
			    ctx->opts.max_aq_depth);
		ctx->opts.max_aq_depth = SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE;
	}

	spdk_iobuf_get_opts(&opts_iobuf, sizeof(opts_iobuf));
	if (ctx->opts.io_unit_size == 0) {
		SPDK_ERRLOG("io_unit_size cannot be 0\n");
		goto err;
	}
	if (ctx->opts.io_unit_size > opts_iobuf.large_bufsize) {
		SPDK_ERRLOG("io_unit_size %u is larger than iobuf pool large buffer size %d\n",
			    ctx->opts.io_unit_size, opts_iobuf.large_bufsize);
		goto err;
	}

	if (ctx->opts.io_unit_size <= opts_iobuf.small_bufsize) {
		/* We'll be using the small buffer pool only */
		count = opts_iobuf.small_pool_count;
	} else {
		count = spdk_min(opts_iobuf.small_pool_count, opts_iobuf.large_pool_count);
	}

	if (ctx->opts.num_shared_buffers > count) {
		SPDK_WARNLOG("The num_shared_buffers value (%u) is larger than the available iobuf"
			     " pool size (%lu). Please increase the iobuf pool sizes.\n",
			     ctx->opts.num_shared_buffers, count);
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/* Prioritize sync create operation. */
	if (ctx->ops->create) {
		if (sync) {
			_nvmf_transport_create_done(ctx);
			return 0;
		}

		rc = spdk_thread_send_msg(spdk_get_thread(), _nvmf_transport_create_done, ctx);
		if (rc) {
			goto err;
		}

		return 0;
	}

	assert(ctx->ops->create_async);
	rc = ctx->ops->create_async(&ctx->opts, nvmf_transport_create_async_done, ctx);
	if (rc) {
		SPDK_ERRLOG("Unable to create new transport of type %s\n", transport_name);
		goto err;
	}

	return 0;
err:
	free(ctx);
	return -1;
}

int
spdk_nvmf_transport_create_async(const char *transport_name, struct spdk_nvmf_transport_opts *opts,
				 spdk_nvmf_transport_create_done_cb cb_fn, void *cb_arg)
{
	return nvmf_transport_create(transport_name, opts, cb_fn, cb_arg, false);
}

static void
nvmf_transport_create_sync_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport **_transport = cb_arg;

	*_transport = transport;
}

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_nvmf_transport *transport = NULL;

	/* Current implementation supports synchronous version of create operation only. */
	assert(nvmf_get_transport_ops(transport_name) && nvmf_get_transport_ops(transport_name)->create);

	nvmf_transport_create(transport_name, opts, nvmf_transport_create_sync_done, &transport, true);
	return transport;
}

struct spdk_nvmf_transport *
spdk_nvmf_transport_get_first(struct spdk_nvmf_tgt *tgt)
{
	return TAILQ_FIRST(&tgt->transports);
}

struct spdk_nvmf_transport *
spdk_nvmf_transport_get_next(struct spdk_nvmf_transport *transport)
{
	return TAILQ_NEXT(transport, link);
}

int
spdk_nvmf_transport_destroy(struct spdk_nvmf_transport *transport,
			    spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_listener *listener, *listener_tmp;

	TAILQ_FOREACH_SAFE(listener, &transport->listeners, link, listener_tmp) {
		TAILQ_REMOVE(&transport->listeners, listener, link);
		transport->ops->stop_listen(transport, &listener->trid);
		free(listener);
	}

	if (nvmf_transport_use_iobuf(transport)) {
		spdk_iobuf_unregister_module(transport->iobuf_name);
	}

	pthread_mutex_destroy(&transport->mutex);
	return transport->ops->destroy(transport, cb_fn, cb_arg);
}

struct spdk_nvmf_listener *
nvmf_transport_find_listener(struct spdk_nvmf_transport *transport,
			     const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	TAILQ_FOREACH(listener, &transport->listeners, link) {
		if (spdk_nvme_transport_id_compare(&listener->trid, trid) == 0) {
			return listener;
		}
	}

	return NULL;
}

int
spdk_nvmf_transport_listen(struct spdk_nvmf_transport *transport,
			   const struct spdk_nvme_transport_id *trid, struct spdk_nvmf_listen_opts *opts)
{
	struct spdk_nvmf_listener *listener;
	int rc;

	listener = nvmf_transport_find_listener(transport, trid);
	if (!listener) {
		listener = calloc(1, sizeof(*listener));
		if (!listener) {
			return -ENOMEM;
		}

		listener->ref = 1;
		listener->trid = *trid;
		listener->sock_impl = opts->sock_impl;
		TAILQ_INSERT_TAIL(&transport->listeners, listener, link);
		pthread_mutex_lock(&transport->mutex);
		rc = transport->ops->listen(transport, &listener->trid, opts);
		pthread_mutex_unlock(&transport->mutex);
		if (rc != 0) {
			TAILQ_REMOVE(&transport->listeners, listener, link);
			free(listener);
		}
		return rc;
	}

	if (opts->sock_impl && strncmp(opts->sock_impl, listener->sock_impl, strlen(listener->sock_impl))) {
		SPDK_ERRLOG("opts->sock_impl: '%s' doesn't match listener->sock_impl: '%s'\n", opts->sock_impl,
			    listener->sock_impl);
		return -EINVAL;
	}

	++listener->ref;

	return 0;
}

int
spdk_nvmf_transport_stop_listen(struct spdk_nvmf_transport *transport,
				const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_listener *listener;

	listener = nvmf_transport_find_listener(transport, trid);
	if (!listener) {
		return -ENOENT;
	}

	if (--listener->ref == 0) {
		TAILQ_REMOVE(&transport->listeners, listener, link);
		pthread_mutex_lock(&transport->mutex);
		transport->ops->stop_listen(transport, trid);
		pthread_mutex_unlock(&transport->mutex);
		free(listener);
	}

	return 0;
}

struct nvmf_stop_listen_ctx {
	struct spdk_nvmf_transport *transport;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvmf_subsystem *subsystem;
	spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn;
	void *cb_arg;
};

static void
nvmf_stop_listen_fini(struct spdk_io_channel_iter *i, int status)
{
	struct nvmf_stop_listen_ctx *ctx;
	struct spdk_nvmf_transport *transport;
	int rc = status;

	ctx = spdk_io_channel_iter_get_ctx(i);
	transport = ctx->transport;
	assert(transport != NULL);

	rc = spdk_nvmf_transport_stop_listen(transport, &ctx->trid);
	if (rc) {
		SPDK_ERRLOG("Failed to stop listening on address '%s'\n", ctx->trid.traddr);
	}

	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, rc);
	}
	free(ctx);
}

static void nvmf_stop_listen_disconnect_qpairs(struct spdk_io_channel_iter *i);

static void
nvmf_stop_listen_disconnect_qpairs_msg(void *ctx)
{
	nvmf_stop_listen_disconnect_qpairs((struct spdk_io_channel_iter *)ctx);
}

static void
nvmf_stop_listen_disconnect_qpairs(struct spdk_io_channel_iter *i)
{
	struct nvmf_stop_listen_ctx *ctx;
	struct spdk_nvmf_poll_group *group;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_qpair *qpair, *tmp_qpair;
	struct spdk_nvme_transport_id tmp_trid;
	bool qpair_found = false;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp_qpair) {
		if (spdk_nvmf_qpair_get_listen_trid(qpair, &tmp_trid)) {
			continue;
		}

		/* Skip qpairs that don't match the listen trid and subsystem pointer.  If
		 * the ctx->subsystem is NULL, it means disconnect all qpairs that match
		 * the listen trid. */
		if (!spdk_nvme_transport_id_compare(&ctx->trid, &tmp_trid)) {
			if (ctx->subsystem == NULL ||
			    (qpair->ctrlr != NULL && ctx->subsystem == qpair->ctrlr->subsys)) {
				spdk_nvmf_qpair_disconnect(qpair);
				qpair_found = true;
			}
		}
	}
	if (qpair_found) {
		spdk_thread_send_msg(spdk_get_thread(), nvmf_stop_listen_disconnect_qpairs_msg, i);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

int
spdk_nvmf_transport_stop_listen_async(struct spdk_nvmf_transport *transport,
				      const struct spdk_nvme_transport_id *trid,
				      struct spdk_nvmf_subsystem *subsystem,
				      spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				      void *cb_arg)
{
	struct nvmf_stop_listen_ctx *ctx;

	if (trid->subnqn[0] != '\0') {
		SPDK_ERRLOG("subnqn should be empty, use subsystem pointer instead\n");
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(struct nvmf_stop_listen_ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->trid = *trid;
	ctx->subsystem = subsystem;
	ctx->transport = transport;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(transport->tgt, nvmf_stop_listen_disconnect_qpairs, ctx,
			      nvmf_stop_listen_fini);

	return 0;
}

void
nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				 struct spdk_nvme_transport_id *trid,
				 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	transport->ops->listener_discover(transport, trid, entry);
}

static int
nvmf_tgroup_poll(void *arg)
{
	struct spdk_nvmf_transport_poll_group *tgroup = arg;
	int rc;

	rc = nvmf_transport_poll_group_poll(tgroup);
	return rc == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

static void
nvmf_transport_poll_group_create_poller(struct spdk_nvmf_transport_poll_group *tgroup)
{
	char poller_name[SPDK_NVMF_TRSTRING_MAX_LEN + 32];

	snprintf(poller_name, sizeof(poller_name), "nvmf_%s", tgroup->transport->ops->name);
	tgroup->poller = spdk_poller_register_named(nvmf_tgroup_poll, tgroup, 0, poller_name);
	spdk_poller_register_interrupt(tgroup->poller, NULL, NULL);
}

struct spdk_nvmf_transport_poll_group *
nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup;
	struct spdk_iobuf_opts opts_iobuf = {};
	uint32_t buf_cache_size, small_cache_size, large_cache_size;
	int rc;

	pthread_mutex_lock(&transport->mutex);
	tgroup = transport->ops->poll_group_create(transport, group);
	pthread_mutex_unlock(&transport->mutex);
	if (!tgroup) {
		return NULL;
	}
	tgroup->transport = transport;
	nvmf_transport_poll_group_create_poller(tgroup);

	STAILQ_INIT(&tgroup->pending_buf_queue);

	if (!nvmf_transport_use_iobuf(transport)) {
		/* We aren't going to allocate any shared buffers or cache, so just return now. */
		return tgroup;
	}

	buf_cache_size = transport->opts.buf_cache_size;

	/* buf_cache_size of UINT32_MAX means the value should be calculated dynamically
	 * based on the number of buffers in the shared pool and the number of poll groups
	 * that are sharing them.  We allocate 75% of the pool for the cache, and then
	 * divide that by number of poll groups to determine the buf_cache_size for this
	 * poll group.
	 */
	if (buf_cache_size == UINT32_MAX) {
		uint32_t num_shared_buffers = transport->opts.num_shared_buffers;

		/* Theoretically the nvmf library can dynamically add poll groups to
		 * the target, after transports have already been created.  We aren't
		 * going to try to really handle this case efficiently, just do enough
		 * here to ensure we don't divide-by-zero.
		 */
		uint16_t num_poll_groups = group->tgt->num_poll_groups ? : spdk_env_get_core_count();

		buf_cache_size = (num_shared_buffers * 3 / 4) / num_poll_groups;
	}

	spdk_iobuf_get_opts(&opts_iobuf, sizeof(opts_iobuf));
	small_cache_size = buf_cache_size;
	if (transport->opts.io_unit_size <= opts_iobuf.small_bufsize) {
		large_cache_size = 0;
	} else {
		large_cache_size = buf_cache_size;
	}

	tgroup->buf_cache = calloc(1, sizeof(*tgroup->buf_cache));
	if (!tgroup->buf_cache) {
		SPDK_ERRLOG("Unable to allocate an iobuf channel in the poll group.\n");
		goto err;
	}

	rc = spdk_iobuf_channel_init(tgroup->buf_cache, transport->iobuf_name, small_cache_size,
				     large_cache_size);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to reserve the full number of buffers for the pg buffer cache.\n");
		rc = spdk_iobuf_channel_init(tgroup->buf_cache, transport->iobuf_name, 0, 0);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to create an iobuf channel in the poll group.\n");
			goto err;
		}
	}

	return tgroup;
err:
	transport->ops->poll_group_destroy(tgroup);
	return NULL;
}

struct spdk_nvmf_transport_poll_group *
nvmf_transport_get_optimal_poll_group(struct spdk_nvmf_transport *transport,
				      struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	if (transport->ops->get_optimal_poll_group) {
		pthread_mutex_lock(&transport->mutex);
		tgroup = transport->ops->get_optimal_poll_group(qpair);
		pthread_mutex_unlock(&transport->mutex);

		return tgroup;
	} else {
		return NULL;
	}
}

void
nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_transport *transport;
	struct spdk_iobuf_channel *ch = NULL;

	transport = group->transport;

	spdk_poller_unregister(&group->poller);

	if (!STAILQ_EMPTY(&group->pending_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on poll group destruction\n");
	}

	if (nvmf_transport_use_iobuf(transport)) {
		/* The call to poll_group_destroy both frees the group memory, but also
		 * releases any remaining buffers. Cache channel pointer so we can still
		 * release the resources after the group has been freed. */
		ch = group->buf_cache;
	}

	pthread_mutex_lock(&transport->mutex);
	transport->ops->poll_group_destroy(group);
	pthread_mutex_unlock(&transport->mutex);

	if (nvmf_transport_use_iobuf(transport)) {
		spdk_iobuf_channel_fini(ch);
		free(ch);
	}
}

void
nvmf_transport_poll_group_pause(struct spdk_nvmf_transport_poll_group *tgroup)
{
	spdk_poller_unregister(&tgroup->poller);
}

void
nvmf_transport_poll_group_resume(struct spdk_nvmf_transport_poll_group *tgroup)
{
	nvmf_transport_poll_group_create_poller(tgroup);
}

int
nvmf_transport_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_qpair *qpair)
{
	if (qpair->transport) {
		assert(qpair->transport == group->transport);
		if (qpair->transport != group->transport) {
			return -1;
		}
	} else {
		qpair->transport = group->transport;
	}

	SPDK_DTRACE_PROBE3(nvmf_transport_poll_group_add, qpair, qpair->qid,
			   spdk_thread_get_id(group->group->thread));

	return group->transport->ops->poll_group_add(group, qpair);
}

int
nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	int rc = ENOTSUP;

	SPDK_DTRACE_PROBE3(nvmf_transport_poll_group_remove, qpair, qpair->qid,
			   spdk_thread_get_id(group->group->thread));

	assert(qpair->transport == group->transport);
	if (group->transport->ops->poll_group_remove) {
		rc = group->transport->ops->poll_group_remove(group, qpair);
	}

	return rc;
}

int
nvmf_transport_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	return group->transport->ops->poll_group_poll(group);
}

int
nvmf_transport_req_free(struct spdk_nvmf_request *req)
{
	return req->qpair->transport->ops->req_free(req);
}

int
nvmf_transport_req_complete(struct spdk_nvmf_request *req)
{
	return req->qpair->transport->ops->req_complete(req);
}

void
nvmf_transport_qpair_fini(struct spdk_nvmf_qpair *qpair,
			  spdk_nvmf_transport_qpair_fini_cb cb_fn,
			  void *cb_arg)
{
	SPDK_DTRACE_PROBE1(nvmf_transport_qpair_fini, qpair);

	qpair->transport->ops->qpair_fini(qpair, cb_fn, cb_arg);
}

int
nvmf_transport_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_peer_trid(qpair, trid);
}

int
nvmf_transport_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				    struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_local_trid(qpair, trid);
}

int
nvmf_transport_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				     struct spdk_nvme_transport_id *trid)
{
	return qpair->transport->ops->qpair_get_listen_trid(qpair, trid);
}

void
nvmf_transport_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvmf_request *req)
{
	if (qpair->transport->ops->qpair_abort_request) {
		qpair->transport->ops->qpair_abort_request(qpair, req);
	}
}

bool
spdk_nvmf_transport_opts_init(const char *transport_name,
			      struct spdk_nvmf_transport_opts *opts, size_t opts_size)
{
	const struct spdk_nvmf_transport_ops *ops;
	struct spdk_nvmf_transport_opts opts_local = {};

	ops = nvmf_get_transport_ops(transport_name);
	if (!ops) {
		SPDK_ERRLOG("Transport type %s unavailable.\n", transport_name);
		return false;
	}

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return false;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size inside opts should not be zero value\n");
		return false;
	}

	opts_local.association_timeout = NVMF_TRANSPORT_DEFAULT_ASSOCIATION_TIMEOUT_IN_MS;
	opts_local.acceptor_poll_rate = SPDK_NVMF_DEFAULT_ACCEPT_POLL_RATE_US;
	opts_local.disable_command_passthru = false;
	ops->opts_init(&opts_local);

	nvmf_transport_opts_copy(opts, &opts_local, opts_size);

	return true;
}

void
spdk_nvmf_request_free_buffers(struct spdk_nvmf_request *req,
			       struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_nvmf_transport *transport)
{
	uint32_t i;

	for (i = 0; i < req->iovcnt; i++) {
		spdk_iobuf_put(group->buf_cache, req->iov[i].iov_base, req->iov[i].iov_len);
		req->iov[i].iov_base = NULL;
		req->iov[i].iov_len = 0;
	}
	req->iovcnt = 0;
	req->data_from_pool = false;
}

static int
nvmf_request_set_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
			uint32_t io_unit_size)
{
	req->iov[req->iovcnt].iov_base = buf;
	req->iov[req->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	length -= req->iov[req->iovcnt].iov_len;
	req->iovcnt++;
	req->data_from_pool = true;

	return length;
}

static int
nvmf_request_set_stripped_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
				 uint32_t io_unit_size)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;

	data->iov[data->iovcnt].iov_base = buf;
	data->iov[data->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	length -= data->iov[data->iovcnt].iov_len;
	data->iovcnt++;
	req->data_from_pool = true;

	return length;
}

static void nvmf_request_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf);

static int
nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			 struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_transport *transport,
			 uint32_t length, uint32_t io_unit_size,
			 bool stripped_buffers)
{
	struct spdk_iobuf_entry *entry = NULL;
	uint32_t num_buffers;
	uint32_t i = 0;
	void *buffer;

	/* If the number of buffers is too large, then we know the I/O is larger than allowed.
	 *  Fail it.
	 */
	num_buffers = SPDK_CEIL_DIV(length, io_unit_size);
	if (spdk_unlikely(num_buffers > NVMF_REQ_MAX_BUFFERS)) {
		return -EINVAL;
	}

	/* Use iobuf queuing only if transport supports it */
	if (transport->ops->req_get_buffers_done != NULL) {
		entry = &req->iobuf.entry;
	}

	while (i < num_buffers) {
		buffer = spdk_iobuf_get(group->buf_cache, spdk_min(io_unit_size, length), entry,
					nvmf_request_iobuf_get_cb);
		if (spdk_unlikely(buffer == NULL)) {
			req->iobuf.remaining_length = length;
			return -ENOMEM;
		}
		if (stripped_buffers) {
			length = nvmf_request_set_stripped_buffer(req, buffer, length, io_unit_size);
		} else {
			length = nvmf_request_set_buffer(req, buffer, length, io_unit_size);
		}
		i++;
	}

	assert(length == 0);

	return 0;
}

static void
nvmf_request_iobuf_get_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_nvmf_request *req = SPDK_CONTAINEROF(entry, struct spdk_nvmf_request, iobuf.entry);
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(group, transport);
	uint32_t length = req->iobuf.remaining_length;
	uint32_t io_unit_size = transport->opts.io_unit_size;
	int rc;

	assert(tgroup != NULL);

	length = nvmf_request_set_buffer(req, buf, length, io_unit_size);
	rc = nvmf_request_get_buffers(req, tgroup, transport, length, io_unit_size, false);
	if (rc == 0) {
		transport->ops->req_get_buffers_done(req);
	}
}

int
spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_transport *transport,
			      uint32_t length)
{
	int rc;

	assert(nvmf_transport_use_iobuf(transport));

	req->iovcnt = 0;
	rc = nvmf_request_get_buffers(req, group, transport, length, transport->opts.io_unit_size, false);
	if (spdk_unlikely(rc == -ENOMEM && transport->ops->req_get_buffers_done == NULL)) {
		spdk_nvmf_request_free_buffers(req, group, transport);
	}

	return rc;
}

static int
nvmf_request_get_buffers_abort_cb(struct spdk_iobuf_channel *ch, struct spdk_iobuf_entry *entry,
				  void *cb_ctx)
{
	struct spdk_nvmf_request *req, *req_to_abort = cb_ctx;

	req = SPDK_CONTAINEROF(entry, struct spdk_nvmf_request, iobuf.entry);
	if (req != req_to_abort) {
		return 0;
	}

	spdk_iobuf_entry_abort(ch, entry, spdk_min(req->iobuf.remaining_length,
			       req->qpair->transport->opts.io_unit_size));
	return 1;
}

bool
nvmf_request_get_buffers_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_transport_poll_group *tgroup = nvmf_get_transport_poll_group(req->qpair->group,
			req->qpair->transport);
	int rc;

	assert(tgroup != NULL);

	rc = spdk_iobuf_for_each_entry(tgroup->buf_cache, nvmf_request_get_buffers_abort_cb, req);
	return rc == 1;
}

void
nvmf_request_free_stripped_buffers(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_transport_poll_group *group,
				   struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;
	uint32_t i;

	for (i = 0; i < data->iovcnt; i++) {
		spdk_iobuf_put(group->buf_cache, data->iov[i].iov_base, data->iov[i].iov_len);
	}
	free(data);
	req->stripped_data = NULL;
}

int
nvmf_request_get_stripped_buffers(struct spdk_nvmf_request *req,
				  struct spdk_nvmf_transport_poll_group *group,
				  struct spdk_nvmf_transport *transport,
				  uint32_t length)
{
	uint32_t block_size = req->dif.dif_ctx.block_size;
	uint32_t data_block_size = block_size - req->dif.dif_ctx.md_size;
	uint32_t io_unit_size = transport->opts.io_unit_size / block_size * data_block_size;
	struct spdk_nvmf_stripped_data *data;
	uint32_t i;
	int rc;

	/* We don't support iobuf queueing with stripped buffers yet */
	assert(transport->ops->req_get_buffers_done == NULL);

	/* Data blocks must be block aligned */
	for (i = 0; i < req->iovcnt; i++) {
		if (req->iov[i].iov_len % block_size) {
			return -EINVAL;
		}
	}

	data = calloc(1, sizeof(*data));
	if (data == NULL) {
		SPDK_ERRLOG("Unable to allocate memory for stripped_data.\n");
		return -ENOMEM;
	}
	req->stripped_data = data;
	req->stripped_data->iovcnt = 0;

	rc = nvmf_request_get_buffers(req, group, transport, length, io_unit_size, true);
	if (rc == -ENOMEM) {
		nvmf_request_free_stripped_buffers(req, group, transport);
		return rc;
	}
	return rc;
}
