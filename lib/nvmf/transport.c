/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019, 2021 Mellanox Technologies LTD. All rights reserved.
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

#define MAX_MEMPOOL_NAME_LENGTH 40
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
	spdk_json_write_object_end(w);
}

void
nvmf_transport_listen_dump_opts(struct spdk_nvmf_transport *transport,
				const struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w)
{
	const char *adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);

	spdk_json_write_named_object_begin(w, "listen_address");

	spdk_json_write_named_string(w, "trtype", trid->trstring);
	spdk_json_write_named_string(w, "adrfam", adrfam ? adrfam : "unknown");
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);

	if (transport->ops->listen_dump_opts) {
		transport->ops->listen_dump_opts(transport, trid, w);
	}

	spdk_json_write_object_end(w);
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

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_transport_opts) == 64, "Incorrect size");

#undef SET_FIELD
#undef FILED_CHECK
}

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts)
{
	const struct spdk_nvmf_transport_ops *ops = NULL;
	struct spdk_nvmf_transport *transport;
	char spdk_mempool_name[MAX_MEMPOOL_NAME_LENGTH];
	int chars_written;
	struct spdk_nvmf_transport_opts opts_local = {};

	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return NULL;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("The opts_size in opts structure should not be zero\n");
		return NULL;
	}

	ops = nvmf_get_transport_ops(transport_name);
	if (!ops) {
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		return NULL;
	}
	nvmf_transport_opts_copy(&opts_local, opts, opts->opts_size);

	if (opts_local.max_io_size != 0 && (!spdk_u32_is_pow2(opts_local.max_io_size) ||
					    opts_local.max_io_size < 8192)) {
		SPDK_ERRLOG("max_io_size %u must be a power of 2 and be greater than or equal 8KB\n",
			    opts_local.max_io_size);
		return NULL;
	}

	if (opts_local.max_aq_depth < SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE) {
		SPDK_ERRLOG("max_aq_depth %u is less than minimum defined by NVMf spec, use min value\n",
			    opts_local.max_aq_depth);
		opts_local.max_aq_depth = SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE;
	}

	transport = ops->create(&opts_local);
	if (!transport) {
		SPDK_ERRLOG("Unable to create new transport of type %s\n", transport_name);
		return NULL;
	}

	pthread_mutex_init(&transport->mutex, NULL);
	TAILQ_INIT(&transport->listeners);

	transport->ops = ops;
	transport->opts = opts_local;

	chars_written = snprintf(spdk_mempool_name, MAX_MEMPOOL_NAME_LENGTH, "%s_%s_%s", "spdk_nvmf",
				 transport_name, "data");
	if (chars_written < 0) {
		SPDK_ERRLOG("Unable to generate transport data buffer pool name.\n");
		ops->destroy(transport, NULL, NULL);
		return NULL;
	}

	if (opts_local.num_shared_buffers) {
		transport->data_buf_pool = spdk_mempool_create(spdk_mempool_name,
					   opts_local.num_shared_buffers,
					   opts_local.io_unit_size + NVMF_DATA_BUFFER_ALIGNMENT,
					   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					   SPDK_ENV_SOCKET_ID_ANY);

		if (!transport->data_buf_pool) {
			if (spdk_mempool_lookup(spdk_mempool_name) != NULL) {
				SPDK_ERRLOG("Unable to allocate poll group buffer pull: already exists\n");
				SPDK_ERRLOG("Probably running in multiprocess environment, which is "
					    "unsupported by the nvmf library\n");
			} else {
				SPDK_ERRLOG("Unable to allocate buffer pool for poll group\n");
			}
			ops->destroy(transport, NULL, NULL);
			return NULL;
		}
	}

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

	if (transport->data_buf_pool != NULL) {
		if (spdk_mempool_count(transport->data_buf_pool) !=
		    transport->opts.num_shared_buffers) {
			SPDK_ERRLOG("transport buffer pool count is %zu but should be %u\n",
				    spdk_mempool_count(transport->data_buf_pool),
				    transport->opts.num_shared_buffers);
		}
		spdk_mempool_free(transport->data_buf_pool);
	}

	TAILQ_FOREACH_SAFE(listener, &transport->listeners, link, listener_tmp) {
		TAILQ_REMOVE(&transport->listeners, listener, link);
		transport->ops->stop_listen(transport, &listener->trid);
		free(listener);
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

static void
nvmf_stop_listen_disconnect_qpairs(struct spdk_io_channel_iter *i)
{
	struct nvmf_stop_listen_ctx *ctx;
	struct spdk_nvmf_poll_group *group;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_qpair *qpair, *tmp_qpair;
	struct spdk_nvme_transport_id tmp_trid;

	ctx = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, tmp_qpair) {
		/* skip qpairs that don't match the TRID. */
		if (spdk_nvmf_qpair_get_listen_trid(qpair, &tmp_trid)) {
			continue;
		}

		if (!spdk_nvme_transport_id_compare(&ctx->trid, &tmp_trid)) {
			if (ctx->subsystem == NULL || qpair->ctrlr == NULL ||
			    ctx->subsystem == qpair->ctrlr->subsys) {
				spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
			}
		}
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

struct spdk_nvmf_transport_poll_group *
nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup;
	struct spdk_nvmf_transport_pg_cache_buf **bufs;
	uint32_t i;

	pthread_mutex_lock(&transport->mutex);
	tgroup = transport->ops->poll_group_create(transport, group);
	pthread_mutex_unlock(&transport->mutex);
	if (!tgroup) {
		return NULL;
	}
	tgroup->transport = transport;

	STAILQ_INIT(&tgroup->pending_buf_queue);
	STAILQ_INIT(&tgroup->buf_cache);

	if (transport->opts.buf_cache_size) {
		tgroup->buf_cache_size = transport->opts.buf_cache_size;
		bufs = calloc(tgroup->buf_cache_size, sizeof(struct spdk_nvmf_transport_pg_cache_buf *));

		if (!bufs) {
			SPDK_ERRLOG("Memory allocation failed, can't reserve buffers for the pg buffer cache\n");
			return tgroup;
		}

		if (spdk_mempool_get_bulk(transport->data_buf_pool, (void **)bufs, tgroup->buf_cache_size)) {
			tgroup->buf_cache_size = (uint32_t)spdk_mempool_count(transport->data_buf_pool);
			SPDK_NOTICELOG("Unable to reserve the full number of buffers for the pg buffer cache. "
				       "Decrease the number of cached buffers from %u to %u\n",
				       transport->opts.buf_cache_size, tgroup->buf_cache_size);
			/* Sanity check */
			assert(tgroup->buf_cache_size <= transport->opts.buf_cache_size);
			/* Try again with less number of buffers */
			if (spdk_mempool_get_bulk(transport->data_buf_pool, (void **)bufs, tgroup->buf_cache_size)) {
				SPDK_NOTICELOG("Failed to reserve %u buffers\n", tgroup->buf_cache_size);
				tgroup->buf_cache_size = 0;
			}
		}

		for (i = 0; i < tgroup->buf_cache_size; i++) {
			STAILQ_INSERT_HEAD(&tgroup->buf_cache, bufs[i], link);
		}
		tgroup->buf_cache_count = tgroup->buf_cache_size;

		free(bufs);
	}

	return tgroup;
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
	struct spdk_nvmf_transport_pg_cache_buf *buf, *tmp;
	struct spdk_nvmf_transport *transport;

	transport = group->transport;

	if (!STAILQ_EMPTY(&group->pending_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on poll group destruction\n");
	}

	STAILQ_FOREACH_SAFE(buf, &group->buf_cache, link, tmp) {
		STAILQ_REMOVE(&group->buf_cache, buf, spdk_nvmf_transport_pg_cache_buf, link);
		spdk_mempool_put(transport->data_buf_pool, buf);
	}

	pthread_mutex_lock(&transport->mutex);
	transport->ops->poll_group_destroy(group);
	pthread_mutex_unlock(&transport->mutex);
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
		if (group->buf_cache_count < group->buf_cache_size) {
			STAILQ_INSERT_HEAD(&group->buf_cache,
					   (struct spdk_nvmf_transport_pg_cache_buf *)req->buffers[i],
					   link);
			group->buf_cache_count++;
		} else {
			spdk_mempool_put(transport->data_buf_pool, req->buffers[i]);
		}
		req->iov[i].iov_base = NULL;
		req->buffers[i] = NULL;
		req->iov[i].iov_len = 0;
	}
	req->iovcnt = 0;
	req->data_from_pool = false;
}

typedef int (*set_buffer_callback)(struct spdk_nvmf_request *req, void *buf,
				   uint32_t length,	uint32_t io_unit_size);
static int
nvmf_request_set_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
			uint32_t io_unit_size)
{
	req->buffers[req->iovcnt] = buf;
	req->iov[req->iovcnt].iov_base = (void *)((uintptr_t)(buf + NVMF_DATA_BUFFER_MASK) &
					 ~NVMF_DATA_BUFFER_MASK);
	req->iov[req->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	length -= req->iov[req->iovcnt].iov_len;
	req->iovcnt++;

	return length;
}

static int
nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			 struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_transport *transport,
			 uint32_t length, uint32_t io_unit_size,
			 set_buffer_callback cb_func)
{
	uint32_t num_buffers;
	uint32_t i = 0, j;
	void *buffer, *buffers[NVMF_REQ_MAX_BUFFERS];

	/* If the number of buffers is too large, then we know the I/O is larger than allowed.
	 *  Fail it.
	 */
	num_buffers = SPDK_CEIL_DIV(length, io_unit_size);
	if (num_buffers > NVMF_REQ_MAX_BUFFERS) {
		return -EINVAL;
	}

	while (i < num_buffers) {
		if (!(STAILQ_EMPTY(&group->buf_cache))) {
			group->buf_cache_count--;
			buffer = STAILQ_FIRST(&group->buf_cache);
			STAILQ_REMOVE_HEAD(&group->buf_cache, link);
			assert(buffer != NULL);

			length = cb_func(req, buffer, length, io_unit_size);
			i++;
		} else {
			if (spdk_mempool_get_bulk(transport->data_buf_pool, buffers,
						  num_buffers - i)) {
				return -ENOMEM;
			}
			for (j = 0; j < num_buffers - i; j++) {
				length = cb_func(req, buffers[j], length, io_unit_size);
			}
			i += num_buffers - i;
		}
	}

	assert(length == 0);

	return 0;
}

int
spdk_nvmf_request_get_buffers(struct spdk_nvmf_request *req,
			      struct spdk_nvmf_transport_poll_group *group,
			      struct spdk_nvmf_transport *transport,
			      uint32_t length)
{
	int rc;

	req->iovcnt = 0;
	rc = nvmf_request_get_buffers(req, group, transport, length,
				      transport->opts.io_unit_size,
				      nvmf_request_set_buffer);
	if (!rc) {
		req->data_from_pool = true;
	} else if (rc == -ENOMEM) {
		spdk_nvmf_request_free_buffers(req, group, transport);
		return rc;
	}

	return rc;
}

static int
nvmf_request_set_stripped_buffer(struct spdk_nvmf_request *req, void *buf, uint32_t length,
				 uint32_t io_unit_size)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;

	data->buffers[data->iovcnt] = buf;
	data->iov[data->iovcnt].iov_base = (void *)((uintptr_t)(buf + NVMF_DATA_BUFFER_MASK) &
					   ~NVMF_DATA_BUFFER_MASK);
	data->iov[data->iovcnt].iov_len  = spdk_min(length, io_unit_size);
	length -= data->iov[data->iovcnt].iov_len;
	data->iovcnt++;

	return length;
}

void
nvmf_request_free_stripped_buffers(struct spdk_nvmf_request *req,
				   struct spdk_nvmf_transport_poll_group *group,
				   struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_stripped_data *data = req->stripped_data;
	uint32_t i;

	for (i = 0; i < data->iovcnt; i++) {
		if (group->buf_cache_count < group->buf_cache_size) {
			STAILQ_INSERT_HEAD(&group->buf_cache,
					   (struct spdk_nvmf_transport_pg_cache_buf *)data->buffers[i],
					   link);
			group->buf_cache_count++;
		} else {
			spdk_mempool_put(transport->data_buf_pool, data->buffers[i]);
		}
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

	rc = nvmf_request_get_buffers(req, group, transport, length, io_unit_size,
				      nvmf_request_set_stripped_buffer);
	if (rc == -ENOMEM) {
		nvmf_request_free_stripped_buffers(req, group, transport);
		return rc;
	}
	return rc;
}
