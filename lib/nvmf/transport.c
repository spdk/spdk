/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/config.h"
#include "spdk/log.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_transport.h"
#include "spdk/queue.h"
#include "spdk/util.h"

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

struct spdk_nvmf_transport *
spdk_nvmf_transport_create(const char *transport_name, struct spdk_nvmf_transport_opts *opts)
{
	const struct spdk_nvmf_transport_ops *ops = NULL;
	struct spdk_nvmf_transport *transport;
	char spdk_mempool_name[MAX_MEMPOOL_NAME_LENGTH];
	int chars_written;

	ops = nvmf_get_transport_ops(transport_name);
	if (!ops) {
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		return NULL;
	}

	if (opts->max_aq_depth < SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE) {
		SPDK_ERRLOG("max_aq_depth %u is less than minimum defined by NVMf spec, use min value\n",
			    opts->max_aq_depth);
		opts->max_aq_depth = SPDK_NVMF_MIN_ADMIN_MAX_SQ_SIZE;
	}

	transport = ops->create(opts);
	if (!transport) {
		SPDK_ERRLOG("Unable to create new transport of type %s\n", transport_name);
		return NULL;
	}

	TAILQ_INIT(&transport->listeners);

	transport->ops = ops;
	transport->opts = *opts;
	chars_written = snprintf(spdk_mempool_name, MAX_MEMPOOL_NAME_LENGTH, "%s_%s_%s", "spdk_nvmf",
				 transport_name, "data");
	if (chars_written < 0) {
		SPDK_ERRLOG("Unable to generate transport data buffer pool name.\n");
		ops->destroy(transport, NULL, NULL);
		return NULL;
	}

	transport->data_buf_pool = spdk_mempool_create(spdk_mempool_name,
				   opts->num_shared_buffers,
				   opts->io_unit_size + NVMF_DATA_BUFFER_ALIGNMENT,
				   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				   SPDK_ENV_SOCKET_ID_ANY);

	if (!transport->data_buf_pool) {
		SPDK_ERRLOG("Unable to allocate buffer pool for poll group\n");
		ops->destroy(transport, NULL, NULL);
		return NULL;
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
	if (transport->data_buf_pool != NULL) {
		if (spdk_mempool_count(transport->data_buf_pool) !=
		    transport->opts.num_shared_buffers) {
			SPDK_ERRLOG("transport buffer pool count is %zu but should be %u\n",
				    spdk_mempool_count(transport->data_buf_pool),
				    transport->opts.num_shared_buffers);
		}
	}

	spdk_mempool_free(transport->data_buf_pool);

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
			   const struct spdk_nvme_transport_id *trid)
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

		rc = transport->ops->listen(transport, &listener->trid);
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
		transport->ops->stop_listen(transport, trid);
		free(listener);
	}

	return 0;
}

struct nvmf_stop_listen_ctx {
	struct spdk_nvmf_transport *transport;
	struct spdk_nvme_transport_id trid;
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
			spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
		}
	}
	spdk_for_each_channel_continue(i, 0);
}

int
spdk_nvmf_transport_stop_listen_async(struct spdk_nvmf_transport *transport,
				      const struct spdk_nvme_transport_id *trid,
				      spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
				      void *cb_arg)
{
	struct nvmf_stop_listen_ctx *ctx;

	ctx = calloc(1, sizeof(struct nvmf_stop_listen_ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->trid = *trid;
	ctx->transport = transport;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(transport->tgt, nvmf_stop_listen_disconnect_qpairs, ctx,
			      nvmf_stop_listen_fini);

	return 0;
}

uint32_t
nvmf_transport_accept(struct spdk_nvmf_transport *transport)
{
	return transport->ops->accept(transport);
}

void
nvmf_transport_listener_discover(struct spdk_nvmf_transport *transport,
				 struct spdk_nvme_transport_id *trid,
				 struct spdk_nvmf_discovery_log_page_entry *entry)
{
	transport->ops->listener_discover(transport, trid, entry);
}

struct spdk_nvmf_transport_poll_group *
nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_transport_poll_group *group;
	struct spdk_nvmf_transport_pg_cache_buf **bufs;
	uint32_t i;

	group = transport->ops->poll_group_create(transport);
	if (!group) {
		return NULL;
	}
	group->transport = transport;

	STAILQ_INIT(&group->pending_buf_queue);
	STAILQ_INIT(&group->buf_cache);

	if (transport->opts.buf_cache_size) {
		group->buf_cache_size = transport->opts.buf_cache_size;
		bufs = calloc(group->buf_cache_size, sizeof(struct spdk_nvmf_transport_pg_cache_buf *));

		if (!bufs) {
			SPDK_ERRLOG("Memory allocation failed, can't reserve buffers for the pg buffer cache\n");
			return group;
		}

		if (spdk_mempool_get_bulk(transport->data_buf_pool, (void **)bufs, group->buf_cache_size)) {
			group->buf_cache_size = (uint32_t)spdk_mempool_count(transport->data_buf_pool);
			SPDK_NOTICELOG("Unable to reserve the full number of buffers for the pg buffer cache. "
				       "Decrease the number of cached buffers from %u to %u\n",
				       transport->opts.buf_cache_size, group->buf_cache_size);
			/* Sanity check */
			assert(group->buf_cache_size <= transport->opts.buf_cache_size);
			/* Try again with less number of buffers */
			if (spdk_mempool_get_bulk(transport->data_buf_pool, (void **)bufs, group->buf_cache_size)) {
				SPDK_NOTICELOG("Failed to reserve %u buffers\n", group->buf_cache_size);
				group->buf_cache_size = 0;
			}
		}

		for (i = 0; i < group->buf_cache_size; i++) {
			STAILQ_INSERT_HEAD(&group->buf_cache, bufs[i], link);
		}
		group->buf_cache_count = group->buf_cache_size;

		free(bufs);
	}
	return group;
}

struct spdk_nvmf_transport_poll_group *
nvmf_transport_get_optimal_poll_group(struct spdk_nvmf_transport *transport,
				      struct spdk_nvmf_qpair *qpair)
{
	if (transport->ops->get_optimal_poll_group) {
		return transport->ops->get_optimal_poll_group(qpair);
	} else {
		return NULL;
	}
}

void
nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_transport_pg_cache_buf *buf, *tmp;

	if (!STAILQ_EMPTY(&group->pending_buf_queue)) {
		SPDK_ERRLOG("Pending I/O list wasn't empty on poll group destruction\n");
	}

	STAILQ_FOREACH_SAFE(buf, &group->buf_cache, link, tmp) {
		STAILQ_REMOVE(&group->buf_cache, buf, spdk_nvmf_transport_pg_cache_buf, link);
		spdk_mempool_put(group->transport->data_buf_pool, buf);
	}
	group->transport->ops->poll_group_destroy(group);
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

	return group->transport->ops->poll_group_add(group, qpair);
}

int
nvmf_transport_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
				 struct spdk_nvmf_qpair *qpair)
{
	int rc = ENOTSUP;

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
	qpair->transport->ops->qpair_abort_request(qpair, req);
}

bool
spdk_nvmf_transport_opts_init(const char *transport_name,
			      struct spdk_nvmf_transport_opts *opts)
{
	const struct spdk_nvmf_transport_ops *ops;

	ops = nvmf_get_transport_ops(transport_name);
	if (!ops) {
		SPDK_ERRLOG("Transport type %s unavailable.\n", transport_name);
		return false;
	}

	opts->association_timeout = NVMF_TRANSPORT_DEFAULT_ASSOCIATION_TIMEOUT_IN_MS;
	ops->opts_init(opts);
	return true;
}

int
spdk_nvmf_transport_poll_group_get_stat(struct spdk_nvmf_tgt *tgt,
					struct spdk_nvmf_transport *transport,
					struct spdk_nvmf_transport_poll_group_stat **stat)
{
	if (transport->ops->poll_group_get_stat) {
		return transport->ops->poll_group_get_stat(tgt, stat);
	} else {
		return -ENOTSUP;
	}
}

void
spdk_nvmf_transport_poll_group_free_stat(struct spdk_nvmf_transport *transport,
		struct spdk_nvmf_transport_poll_group_stat *stat)
{
	if (transport->ops->poll_group_free_stat) {
		transport->ops->poll_group_free_stat(stat);
	}
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
	req->data_from_pool = false;
}

static inline int
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
			 uint32_t length)
{
	uint32_t io_unit_size = transport->opts.io_unit_size;
	uint32_t num_buffers;
	uint32_t i = 0, j;
	void *buffer, *buffers[NVMF_REQ_MAX_BUFFERS];

	/* If the number of buffers is too large, then we know the I/O is larger than allowed.
	 *  Fail it.
	 */
	num_buffers = SPDK_CEIL_DIV(length, io_unit_size);
	if (num_buffers + req->iovcnt > NVMF_REQ_MAX_BUFFERS) {
		return -EINVAL;
	}

	while (i < num_buffers) {
		if (!(STAILQ_EMPTY(&group->buf_cache))) {
			group->buf_cache_count--;
			buffer = STAILQ_FIRST(&group->buf_cache);
			STAILQ_REMOVE_HEAD(&group->buf_cache, link);
			assert(buffer != NULL);

			length = nvmf_request_set_buffer(req, buffer, length, io_unit_size);
			i++;
		} else {
			if (spdk_mempool_get_bulk(transport->data_buf_pool, buffers,
						  num_buffers - i)) {
				return -ENOMEM;
			}
			for (j = 0; j < num_buffers - i; j++) {
				length = nvmf_request_set_buffer(req, buffers[j], length, io_unit_size);
			}
			i += num_buffers - i;
		}
	}

	assert(length == 0);

	req->data_from_pool = true;
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

	rc = nvmf_request_get_buffers(req, group, transport, length);
	if (rc == -ENOMEM) {
		spdk_nvmf_request_free_buffers(req, group, transport);
	}

	return rc;
}

int
spdk_nvmf_request_get_buffers_multi(struct spdk_nvmf_request *req,
				    struct spdk_nvmf_transport_poll_group *group,
				    struct spdk_nvmf_transport *transport,
				    uint32_t *lengths, uint32_t num_lengths)
{
	int rc = 0;
	uint32_t i;

	req->iovcnt = 0;

	for (i = 0; i < num_lengths; i++) {
		rc = nvmf_request_get_buffers(req, group, transport, lengths[i]);
		if (rc != 0) {
			goto err_exit;
		}
	}

	return 0;

err_exit:
	spdk_nvmf_request_free_buffers(req, group, transport);
	return rc;
}
