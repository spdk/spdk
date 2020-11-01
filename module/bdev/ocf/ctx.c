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
#include <ocf/ocf.h>
#include <execinfo.h>

#include "spdk/env.h"
#include "spdk/log.h"

#include "ctx.h"
#include "ocf_env.h"
#include "data.h"

ocf_ctx_t vbdev_ocf_ctx;

static ctx_data_t *
vbdev_ocf_ctx_data_alloc(uint32_t pages)
{
	struct bdev_ocf_data *data;
	void *buf;
	uint32_t sz;

	data = vbdev_ocf_data_alloc(1);

	sz = pages * PAGE_SIZE;
	buf = spdk_malloc(sz, PAGE_SIZE, NULL,
			  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		return NULL;
	}

	vbdev_ocf_iovs_add(data, buf, sz);

	data->size = sz;

	return data;
}

static void
vbdev_ocf_ctx_data_free(ctx_data_t *ctx_data)
{
	struct bdev_ocf_data *data = ctx_data;
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
	struct bdev_ocf_data *s = src;
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
	struct bdev_ocf_data *d = dst;
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
	struct bdev_ocf_data *d = dst;
	uint32_t size_local;

	size_local = iovset(d->iovs, d->iovcnt, 0, size, d->seek);
	d->seek += size_local;

	return size_local;
}

static uint32_t
vbdev_ocf_ctx_data_seek(ctx_data_t *dst, ctx_data_seek_t seek, uint32_t offset)
{
	struct bdev_ocf_data *d = dst;
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
	struct bdev_ocf_data *s = src;
	struct bdev_ocf_data *d = dst;
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
	struct bdev_ocf_data *data = ctx_data;
	struct iovec *iovs = data->iovs;
	int i;

	for (i = 0; i < data->iovcnt; i++) {
		if (env_memset(iovs[i].iov_base, iovs[i].iov_len, 0)) {
			assert(false);
		}
	}
}

int vbdev_ocf_queue_create(ocf_cache_t cache, ocf_queue_t *queue, const struct ocf_queue_ops *ops)
{
	int rc;
	struct vbdev_ocf_cache_ctx *ctx = ocf_cache_get_priv(cache);

	pthread_mutex_lock(&ctx->lock);
	rc = ocf_queue_create(cache, queue, ops);
	pthread_mutex_unlock(&ctx->lock);
	return rc;
}

void vbdev_ocf_queue_put(ocf_queue_t queue)
{
	ocf_cache_t cache = ocf_queue_get_cache(queue);
	struct vbdev_ocf_cache_ctx *ctx = ocf_cache_get_priv(cache);

	pthread_mutex_lock(&ctx->lock);
	ocf_queue_put(queue);
	pthread_mutex_unlock(&ctx->lock);
}

void vbdev_ocf_cache_ctx_put(struct vbdev_ocf_cache_ctx *ctx)
{
	if (env_atomic_dec_return(&ctx->refcnt) == 0) {
		pthread_mutex_destroy(&ctx->lock);
		free(ctx);
	}
}

void vbdev_ocf_cache_ctx_get(struct vbdev_ocf_cache_ctx *ctx)
{
	env_atomic_inc(&ctx->refcnt);
}

struct cleaner_priv {
	struct spdk_poller *poller;
	ocf_queue_t         queue;
	uint64_t            next_run;
};

static int
cleaner_poll(void *arg)
{
	ocf_cleaner_t cleaner = arg;
	struct cleaner_priv *priv = ocf_cleaner_get_priv(cleaner);
	uint32_t iono = ocf_queue_pending_io(priv->queue);
	int i, max = spdk_min(32, iono);

	for (i = 0; i < max; i++) {
		ocf_queue_run_single(priv->queue);
	}

	if (spdk_get_ticks() >= priv->next_run) {
		ocf_cleaner_run(cleaner, priv->queue);
		return SPDK_POLLER_BUSY;
	}

	if (iono > 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

static void
cleaner_cmpl(ocf_cleaner_t c, uint32_t interval)
{
	struct cleaner_priv *priv = ocf_cleaner_get_priv(c);

	priv->next_run = spdk_get_ticks() + ((interval * spdk_get_ticks_hz()) / 1000);
}

static void
cleaner_queue_kick(ocf_queue_t q)
{
}

static void
cleaner_queue_stop(ocf_queue_t q)
{
	struct cleaner_priv *cpriv = ocf_queue_get_priv(q);

	if (cpriv) {
		spdk_poller_unregister(&cpriv->poller);
		free(cpriv);
	}
}

const struct ocf_queue_ops cleaner_queue_ops = {
	.kick_sync = cleaner_queue_kick,
	.kick = cleaner_queue_kick,
	.stop = cleaner_queue_stop,
};

static int
vbdev_ocf_ctx_cleaner_init(ocf_cleaner_t c)
{
	int rc;
	struct cleaner_priv        *priv  = calloc(1, sizeof(*priv));
	ocf_cache_t                 cache = ocf_cleaner_get_cache(c);
	struct vbdev_ocf_cache_ctx *cctx  = ocf_cache_get_priv(cache);

	if (priv == NULL) {
		return -ENOMEM;
	}

	rc = vbdev_ocf_queue_create(cache, &priv->queue, &cleaner_queue_ops);
	if (rc) {
		free(priv);
		return rc;
	}

	ocf_queue_set_priv(priv->queue, priv);

	cctx->cleaner_queue  = priv->queue;

	ocf_cleaner_set_cmpl(c, cleaner_cmpl);
	ocf_cleaner_set_priv(c, priv);

	return 0;
}

static void
vbdev_ocf_ctx_cleaner_stop(ocf_cleaner_t c)
{
	struct cleaner_priv *priv = ocf_cleaner_get_priv(c);

	vbdev_ocf_queue_put(priv->queue);
}

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

static void
vbdev_ocf_md_kick(void *ctx)
{
	ocf_metadata_updater_t mu = ctx;
	ocf_cache_t cache = ocf_metadata_updater_get_cache(mu);

	ocf_metadata_updater_run(mu);

	/* Decrease cache ref count after metadata has been updated */
	ocf_mngt_cache_put(cache);
}

static int
vbdev_ocf_volume_updater_init(ocf_metadata_updater_t mu)
{
	struct spdk_thread *md_thread = spdk_get_thread();

	ocf_metadata_updater_set_priv(mu, md_thread);

	return 0;
}

static void
vbdev_ocf_volume_updater_stop(ocf_metadata_updater_t mu)
{

}

static void
vbdev_ocf_volume_updater_kick(ocf_metadata_updater_t mu)
{
	struct spdk_thread *md_thread = ocf_metadata_updater_get_priv(mu);
	ocf_cache_t cache = ocf_metadata_updater_get_cache(mu);

	/* Increase cache ref count prior sending a message to a thread
	 * for metadata update */
	ocf_mngt_cache_get(cache);

	/* We need to send message to updater thread because
	 * kick can happen from any thread */
	spdk_thread_send_msg(md_thread, vbdev_ocf_md_kick, mu);
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
	.name = "OCF SPDK",

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

		.metadata_updater = {
			.init = vbdev_ocf_volume_updater_init,
			.stop = vbdev_ocf_volume_updater_stop,
			.kick = vbdev_ocf_volume_updater_kick,
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
