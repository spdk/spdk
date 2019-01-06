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

ocf_ctx_t opencas_ctx;

static ctx_data_t *
opencas_ctx_data_alloc(uint32_t pages)
{
	struct bdev_ocf_data *data;
	void *buf;
	uint32_t sz;

	data = opencas_data_alloc(1);

	sz = pages * PAGE_SIZE;
	buf = spdk_dma_malloc(sz, PAGE_SIZE, NULL);
	if (buf == NULL) {
		return NULL;
	}

	_opencas_iovs_add(data, buf, sz);

	data->size = sz;

	return data;
}

static void
opencas_ctx_data_free(ctx_data_t *ctx_data)
{
	struct bdev_ocf_data *data = ctx_data;
	int i;

	if (!data) {
		return;
	}

	for (i = 0; i < data->iovcnt; i++) {
		spdk_dma_free(data->iovs[i].iov_base);
	}

	opencas_data_free(data);
}

static int
opencas_ctx_data_mlock(ctx_data_t *ctx_data)
{
	/* TODO [mlock]: add mlock option */
	return 0;
}

static void
opencas_ctx_data_munlock(ctx_data_t *ctx_data)
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
opencas_ctx_data_rd(void *dst, ctx_data_t *src, uint32_t size)
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
opencas_ctx_data_wr(ctx_data_t *dst, const void *src, uint32_t size)
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
opencas_ctx_data_zero(ctx_data_t *dst, uint32_t size)
{
	struct bdev_ocf_data *d = dst;
	uint32_t size_local;

	size_local = iovset(d->iovs, d->iovcnt, 0, size, d->seek);
	d->seek += size_local;

	return size_local;
}

static uint32_t
opencas_ctx_data_seek(ctx_data_t *dst, ctx_data_seek_t seek, uint32_t offset)
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
opencas_ctx_data_cpy(ctx_data_t *dst, ctx_data_t *src, uint64_t to,
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
			/* TODO should we ignore bytes copied return param? */
			buf_to_iovec(s->iovs[it_iov].iov_base + it_off, n, d->iovs, d->iovcnt, to);
			bytes -= n;
			to += n;
		}

		it_off += n;
	}

	return sz;
}

static void
opencas_ctx_data_secure_erase(ctx_data_t *ctx_data)
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
opencas_ctx_cleaner_init(ocf_cleaner_t c)
{
	/* TODO [writeback]: implement with writeback mode support */
	return 0;
}

static void
opencas_ctx_cleaner_stop(ocf_cleaner_t c)
{
	/* TODO [writeback]: implement with writeback mode support */
}

static int opencas_dobj_updater_init(ocf_metadata_updater_t mu)
{
	/* TODO [metadata]: implement with persistent metadata support */
	return 0;
}
static void opencas_dobj_updater_stop(ocf_metadata_updater_t mu)
{
	/* TODO [metadata]: implement with persistent metadata support */
}
static void opencas_dobj_updater_kick(ocf_metadata_updater_t mu)
{
	/* TODO [metadata]: implement with persistent metadata support */
}

static const struct ocf_ctx_ops opencas_ctx_ops = {
	.name = "CAS SPDK",

	.data_alloc = opencas_ctx_data_alloc,
	.data_free = opencas_ctx_data_free,
	.data_mlock = opencas_ctx_data_mlock,
	.data_munlock = opencas_ctx_data_munlock,
	.data_rd = opencas_ctx_data_rd,
	.data_wr = opencas_ctx_data_wr,
	.data_zero = opencas_ctx_data_zero,
	.data_seek = opencas_ctx_data_seek,
	.data_cpy = opencas_ctx_data_cpy,
	.data_secure_erase = opencas_ctx_data_secure_erase,

	.queue_init = opencas_ctx_queue_init,
	.queue_kick = opencas_ctx_queue_kick,
	.queue_stop = opencas_ctx_queue_stop,

	.cleaner_init = opencas_ctx_cleaner_init,
	.cleaner_stop = opencas_ctx_cleaner_stop,

	.metadata_updater_init = opencas_dobj_updater_init,
	.metadata_updater_stop = opencas_dobj_updater_stop,
	.metadata_updater_kick = opencas_dobj_updater_kick,
};

static int
opencas_ctx_log_printf(const struct ocf_logger *logger,
		       ocf_logger_lvl_t lvl, const char *fmt, va_list args)
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

#define CTX_LOG_TRACE_DEPTH	16

static int
opencas_ctx_log_dump_stack(const struct ocf_logger *logger)
{
	void *trace[CTX_LOG_TRACE_DEPTH];
	char **messages = NULL;
	char *out;
	int i, size;
	int len = 0, offset = 0;

	size = backtrace(trace, CTX_LOG_TRACE_DEPTH);
	messages = backtrace_symbols(trace, size);
	for (i = 0; i < size; i++) {
		len += strlen(messages[i]) + 1;
	}

	out = malloc(sizeof(char) * len + 1);
	for (i = 0; i < size; i++) {
		offset += snprintf(out + offset, ENV_MAX_STR, "%s\n", messages[i]);
	}
	out[len] = 0;

	SPDK_ERRLOG("[stack trace]>>>\n");
	SPDK_ERRLOG("%s\n", out);
	SPDK_ERRLOG("<<<[stack trace]\n");

	free(messages);
	free(out);

	return 0;
}

static const struct ocf_logger logger = {
	.printf = opencas_ctx_log_printf,
	.dump_stack = opencas_ctx_log_dump_stack,
};

int
opencas_ctx_init(void)
{
	int ret;

	ret = ocf_ctx_init(&opencas_ctx, &opencas_ctx_ops);
	if (ret < 0) {
		return ret;
	}

	ocf_ctx_set_logger(opencas_ctx, &logger);

	return 0;
}

void
opencas_ctx_cleanup(void)
{
	ocf_ctx_exit(opencas_ctx);
	opencas_ctx = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("cache_ocfctx", SPDK_LOG_OCFCTX)
