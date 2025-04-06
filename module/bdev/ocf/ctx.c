/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#include <ocf/ocf.h>
#ifdef SPDK_HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "spdk/env.h"
#include "spdk/log.h"

#include "ctx.h"
#include "data.h"
#include "vbdev_ocf_cache.h"

ocf_ctx_t vbdev_ocf_ctx;

static ctx_data_t *
vbdev_ocf_ctx_data_alloc(uint32_t pages)
{
	struct vbdev_ocf_data *data;
	void *buf;
	uint32_t sz;

	data = vbdev_ocf_data_alloc(1);
	if (data == NULL) {
		return NULL;
	}

	sz = pages * PAGE_SIZE;
	buf = spdk_malloc(sz, PAGE_SIZE, NULL,
			  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		vbdev_ocf_data_free(data);
		return NULL;
	}

	vbdev_ocf_iovs_add(data, buf, sz);

	data->size = sz;

	return data;
}

static void
vbdev_ocf_ctx_data_free(ctx_data_t *ctx_data)
{
	struct vbdev_ocf_data *data = ctx_data;
	int i;

	if (!data) {
		return;
	}

	for (i = 0; i < data->iovcnt; i++) {
		spdk_free(data->iovs[i].iov_base);
	}

	vbdev_ocf_data_free(data);
}

static int
vbdev_ocf_ctx_data_mlock(ctx_data_t *ctx_data)
{
	/* TODO [mlock]: add mlock option */
	return 0;
}

static void
vbdev_ocf_ctx_data_munlock(ctx_data_t *ctx_data)
{
	/* TODO [mlock]: add mlock option */
}

static size_t
iovec_flatten(struct iovec *iov, size_t iovcnt, void *buf, size_t size, size_t offset)
{
	size_t i, len, done = 0;

	for (i = 0; i < iovcnt; i++) {
		if (offset >= iov[i].iov_len) {
			offset -= iov[i].iov_len;
			continue;
		}

		if (iov[i].iov_base == NULL) {
			continue;
		}

		if (done >= size) {
			break;
		}

		len = MIN(size - done, iov[i].iov_len - offset);
		memcpy(buf, iov[i].iov_base + offset, len);
		buf += len;
		done += len;
		offset = 0;
	}

	return done;
}

static uint32_t
vbdev_ocf_ctx_data_rd(void *dst, ctx_data_t *src, uint32_t size)
{
	struct vbdev_ocf_data *s = src;
	uint32_t size_local;

	size_local = iovec_flatten(s->iovs, s->iovcnt, dst, size, s->seek);
	s->seek += size_local;

	return size_local;
}

static size_t
buf_to_iovec(const void *buf, size_t size, struct iovec *iov, size_t iovcnt, size_t offset)
{
	size_t i, len, done = 0;

	for (i = 0; i < iovcnt; i++) {
		if (offset >= iov[i].iov_len) {
			offset -= iov[i].iov_len;
			continue;
		}

		if (iov[i].iov_base == NULL) {
			continue;
		}

		if (done >= size) {
			break;
		}

		len = MIN(size - done, iov[i].iov_len - offset);
		memcpy(iov[i].iov_base + offset, buf, len);
		buf += len;
		done += len;
		offset = 0;
	}

	return done;
}

static uint32_t
vbdev_ocf_ctx_data_wr(ctx_data_t *dst, const void *src, uint32_t size)
{
	struct vbdev_ocf_data *d = dst;
	uint32_t size_local;

	size_local = buf_to_iovec(src, size, d->iovs, d->iovcnt, d->seek);
	d->seek += size_local;

	return size_local;
}

static size_t
iovset(struct iovec *iov, size_t iovcnt, int byte, size_t size, size_t offset)
{
	size_t i, len, done = 0;

	for (i = 0; i < iovcnt; i++) {
		if (offset >= iov[i].iov_len) {
			offset -= iov[i].iov_len;
			continue;
		}

		if (iov[i].iov_base == NULL) {
			continue;
		}

		if (done >= size) {
			break;
		}

		len = MIN(size - done, iov[i].iov_len - offset);
		memset(iov[i].iov_base + offset, byte, len);
		done += len;
		offset = 0;
	}

	return done;
}

static uint32_t
vbdev_ocf_ctx_data_zero(ctx_data_t *dst, uint32_t size)
{
	struct vbdev_ocf_data *d = dst;
	uint32_t size_local;

	size_local = iovset(d->iovs, d->iovcnt, 0, size, d->seek);
	d->seek += size_local;

	return size_local;
}

static uint32_t
vbdev_ocf_ctx_data_seek(ctx_data_t *dst, ctx_data_seek_t seek, uint32_t offset)
{
	struct vbdev_ocf_data *d = dst;
	uint32_t off = 0;

	switch (seek) {
	case ctx_data_seek_begin:
		off = MIN(offset, d->size);
		d->seek = off;
		break;
	case ctx_data_seek_current:
		off = MIN(offset, d->size - d->seek);
		d->seek += off;
		break;
	}

	return off;
}

static uint64_t
vbdev_ocf_ctx_data_cpy(ctx_data_t *dst, ctx_data_t *src, uint64_t to,
		       uint64_t from, uint64_t bytes)
{
	struct vbdev_ocf_data *s = src;
	struct vbdev_ocf_data *d = dst;
	uint32_t it_iov = 0;
	uint32_t it_off = 0;
	uint32_t n, sz;

	bytes = MIN(bytes, s->size - from);
	bytes = MIN(bytes, d->size - to);
	sz = bytes;

	while (from || bytes) {
		if (s->iovs[it_iov].iov_len == it_off) {
			it_iov++;
			it_off = 0;
			continue;
		}

		if (from) {
			n = MIN(from, s->iovs[it_iov].iov_len);
			from -= n;
		} else {
			n = MIN(bytes, s->iovs[it_iov].iov_len);
			buf_to_iovec(s->iovs[it_iov].iov_base + it_off, n, d->iovs, d->iovcnt, to);
			bytes -= n;
			to += n;
		}

		it_off += n;
	}

	return sz;
}

static void
vbdev_ocf_ctx_data_secure_erase(ctx_data_t *ctx_data)
{
	struct vbdev_ocf_data *data = ctx_data;
	struct iovec *iovs = data->iovs;
	int i;

	for (i = 0; i < data->iovcnt; i++) {
		if (env_memset(iovs[i].iov_base, iovs[i].iov_len, 0)) {
			assert(false);
		}
	}
}

int
vbdev_ocf_queue_create(ocf_cache_t cache, ocf_queue_t *queue, const struct ocf_queue_ops *ops)
{

	return ocf_queue_create(cache, queue, ops);
}

int
vbdev_ocf_queue_create_mngt(ocf_cache_t cache, ocf_queue_t *queue, const struct ocf_queue_ops *ops)
{
	return ocf_queue_create_mngt(cache, queue, ops);
}

void
vbdev_ocf_queue_put(ocf_queue_t queue)
{
	ocf_queue_put(queue);
}

int
vbdev_ocf_queue_poller(void *ctx)
{
	ocf_queue_t queue = ctx;
	int i, queue_runs;

	queue_runs = spdk_min(ocf_queue_pending_io(queue), VBDEV_OCF_QUEUE_RUN_MAX);

	for (i = 0; i < queue_runs; i++) {
		ocf_queue_run_single(queue);
	}

	return queue_runs ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

struct cleaner_priv {
	struct spdk_poller *poller;
	ocf_queue_t         mngt_queue;
	uint64_t            next_run;
};

static int
cleaner_poll(void *arg)
{
	ocf_cleaner_t cleaner = arg;
	struct cleaner_priv *priv = ocf_cleaner_get_priv(cleaner);

	if (spdk_get_ticks() >= priv->next_run) {
		ocf_cleaner_run(cleaner, priv->mngt_queue);
		return SPDK_POLLER_BUSY;
	}

	return SPDK_POLLER_IDLE;
}

static void
cleaner_cmpl(ocf_cleaner_t c, uint32_t interval)
{
	struct cleaner_priv *priv = ocf_cleaner_get_priv(c);

	priv->next_run = spdk_get_ticks() + ((interval * spdk_get_ticks_hz()) / 1000);
}

static int
vbdev_ocf_ctx_cleaner_init(ocf_cleaner_t c)
{
	struct cleaner_priv        *priv  = calloc(1, sizeof(*priv));
	ocf_cache_t                 cache = ocf_cleaner_get_cache(c);
	struct vbdev_ocf_cache *vbdev_ocf_cache = ocf_cache_get_priv(cache);

	if (priv == NULL) {
		return -ENOMEM;
	}

	priv->mngt_queue = vbdev_ocf_cache->ocf_cache_mngt_q;

	ocf_cleaner_set_cmpl(c, cleaner_cmpl);
	ocf_cleaner_set_priv(c, priv);

	return 0;
}

static void
vbdev_ocf_ctx_cleaner_stop(ocf_cleaner_t c)
{
	struct cleaner_priv *priv = ocf_cleaner_get_priv(c);

	if (priv) {
		spdk_poller_unregister(&priv->poller);
		vbdev_ocf_queue_put(priv->mngt_queue);
		free(priv);
	}
}

// this one should only set next_run to "now" and poller registration should be move to _init with saved thread
static void
vbdev_ocf_ctx_cleaner_kick(ocf_cleaner_t cleaner)
{
	struct cleaner_priv *priv  = ocf_cleaner_get_priv(cleaner);

	if (priv->poller) {
		return;
	}

	/* We start cleaner poller at the same thread where cache was created
	 * TODO: allow user to specify core at which cleaner should run */
	priv->poller = SPDK_POLLER_REGISTER(cleaner_poll, cleaner, 0);
}

/* This function is main way by which OCF communicates with user
 * We don't want to use SPDK_LOG here because debugging information that is
 * associated with every print message is not helpful in callback that only prints info
 * while the real source is somewhere in OCF code */
static int
vbdev_ocf_ctx_log_printf(ocf_logger_t logger, ocf_logger_lvl_t lvl,
			 const char *fmt, va_list args)
{
	int spdk_lvl;

	switch (lvl) {
	case log_emerg:
	case log_alert:
	case log_crit:
	case log_err:
		spdk_lvl = SPDK_LOG_ERROR;
		break;

	case log_warn:
		spdk_lvl = SPDK_LOG_WARN;
		break;

	case log_notice:
		spdk_lvl = SPDK_LOG_NOTICE;
		break;

	case log_info:
	case log_debug:
	default:
		spdk_lvl = SPDK_LOG_INFO;
	}

	spdk_vlog(spdk_lvl, NULL, -1, NULL, fmt, args);
	return 0;
}

static const struct ocf_ctx_config vbdev_ocf_ctx_cfg = {
	.name = "SPDK_OCF",

	.ops = {
		.data = {
			.alloc = vbdev_ocf_ctx_data_alloc,
			.free = vbdev_ocf_ctx_data_free,
			.mlock = vbdev_ocf_ctx_data_mlock,
			.munlock = vbdev_ocf_ctx_data_munlock,
			.read = vbdev_ocf_ctx_data_rd,
			.write = vbdev_ocf_ctx_data_wr,
			.zero = vbdev_ocf_ctx_data_zero,
			.seek = vbdev_ocf_ctx_data_seek,
			.copy = vbdev_ocf_ctx_data_cpy,
			.secure_erase = vbdev_ocf_ctx_data_secure_erase,
		},

		.cleaner = {
			.init = vbdev_ocf_ctx_cleaner_init,
			.stop = vbdev_ocf_ctx_cleaner_stop,
			.kick = vbdev_ocf_ctx_cleaner_kick,
		},

		.logger = {
			.print = vbdev_ocf_ctx_log_printf,
			.dump_stack = NULL,
		},

	},
};

int
vbdev_ocf_ctx_init(void)
{
	int ret;

	ret = ocf_ctx_create(&vbdev_ocf_ctx, &vbdev_ocf_ctx_cfg);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

void
vbdev_ocf_ctx_cleanup(void)
{
	ocf_ctx_put(vbdev_ocf_ctx);
	vbdev_ocf_ctx = NULL;
}
