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
#include "spdk_internal/log.h"

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
	buf = spdk_dma_malloc(sz, PAGE_SIZE, NULL);
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
		spdk_dma_free(data->iovs[i].iov_base);
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
		off = MIN(off, d->size);
		d->seek = off;
		break;
	case ctx_data_seek_current:
		off = MIN(off, d->size - d->seek);
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

static int
vbdev_ocf_ctx_cleaner_init(ocf_cleaner_t c)
{
	/* TODO [writeback]: implement with writeback mode support */
	return 0;
}

static void
vbdev_ocf_ctx_cleaner_stop(ocf_cleaner_t c)
{
	/* TODO [writeback]: implement with writeback mode support */
}

struct spdk_metadata_ctx {
	struct	spdk_poller *poller;
	bool	kick;
};

static int
metadata_poller(void *mu_ctx)
{
	ocf_metadata_updater_t mu = (ocf_metadata_updater_t)mu_ctx;
	struct spdk_metadata_ctx *ctx = (struct spdk_metadata_ctx *)ocf_metadata_updater_get_priv(mu);

	if (ctx->kick) {
		ocf_metadata_updater_run(mu);
		ctx->kick = false;
		return 1;
	}
	return 0;
}

static int
vbdev_ocf_volume_updater_init(ocf_metadata_updater_t mu)
{
	struct spdk_metadata_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->poller = spdk_poller_register(metadata_poller, mu, 0);
	if (ctx->poller == NULL) {
		return -ENOMEM;
	}

	ocf_metadata_updater_set_priv(mu, ctx);

	return 0;
}

static void
vbdev_ocf_volume_updater_stop(ocf_metadata_updater_t mu)
{
	struct spdk_metadata_ctx *ctx = (struct spdk_metadata_ctx *)ocf_metadata_updater_get_priv(mu);

	spdk_poller_unregister(&ctx->poller);
	free(ctx);
}

static void
vbdev_ocf_volume_updater_kick(ocf_metadata_updater_t mu)
{
	struct spdk_metadata_ctx *ctx = (struct spdk_metadata_ctx *)ocf_metadata_updater_get_priv(mu);

	ctx->kick = true;
}

/* This function is main way by which OCF communicates with user
 * We don't want to use SPDK_LOG here because debugging information that is
 * associated with every print message is not helpful in callback that only prints info
 * while the real source is somewhere in OCF code */
static int
vbdev_ocf_ctx_log_printf(ocf_logger_t logger, ocf_logger_lvl_t lvl,
			 const char *fmt, va_list args)
{
	FILE *lfile = stdout;

	if (lvl > log_info) {
		return 0;
	}

	if (lvl <= log_warn) {
		lfile = stderr;
	}

	return vfprintf(lfile, fmt, args);
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
		},

		.logger = {
			.printf = vbdev_ocf_ctx_log_printf,
			.dump_stack = NULL,
		},

	},
};

int
vbdev_ocf_ctx_init(void)
{
	int ret;

	ret = ocf_ctx_init(&vbdev_ocf_ctx, &vbdev_ocf_ctx_cfg);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

void
vbdev_ocf_ctx_cleanup(void)
{
	ocf_ctx_exit(vbdev_ocf_ctx);
	vbdev_ocf_ctx = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("ocf_ocfctx", SPDK_LOG_OCFCTX)
