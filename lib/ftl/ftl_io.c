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
#include "spdk/ftl.h"
#include "spdk/likely.h"

#include "ftl_io.h"
#include "ftl_core.h"
#include "ftl_rwb.h"
#include "ftl_band.h"

void
ftl_io_inc_req(struct ftl_io *io)
{
	struct ftl_band *band = io->band;

	if (io->type != FTL_IO_READ && io->type != FTL_IO_ERASE) {
		ftl_band_acquire_md(band);
	}

	__atomic_fetch_add(&io->dev->num_inflight, 1, __ATOMIC_SEQ_CST);

	++io->req_cnt;
}

void
ftl_io_dec_req(struct ftl_io *io)
{
	struct ftl_band *band = io->band;
	unsigned long num_inflight __attribute__((unused));

	if (io->type != FTL_IO_READ && io->type != FTL_IO_ERASE) {
		ftl_band_release_md(band);
	}

	num_inflight = __atomic_fetch_sub(&io->dev->num_inflight, 1, __ATOMIC_SEQ_CST);

	assert(num_inflight > 0);
	assert(io->req_cnt > 0);

	--io->req_cnt;
}

struct iovec *
ftl_io_iovec(struct ftl_io *io)
{
	if (io->iov_cnt > 1) {
		return io->iov.vector;
	} else {
		return &io->iov.single;
	}
}

uint64_t
ftl_io_get_lba(const struct ftl_io *io, size_t offset)
{
	assert(offset < io->lbk_cnt);

	if (io->flags & FTL_IO_VECTOR_LBA) {
		return io->lba.vector[offset];
	} else {
		return io->lba.single + offset;
	}
}

uint64_t
ftl_io_current_lba(const struct ftl_io *io)
{
	return ftl_io_get_lba(io, io->pos);
}

void
ftl_io_update_iovec(struct ftl_io *io, size_t lbk_cnt)
{
	struct iovec *iov = ftl_io_iovec(io);
	size_t iov_lbks;

	io->pos += lbk_cnt;

	while (lbk_cnt > 0) {
		assert(io->iov_pos < io->iov_cnt);
		iov_lbks = iov[io->iov_pos].iov_len / PAGE_SIZE;

		if (io->iov_off + lbk_cnt < iov_lbks) {
			io->iov_off += lbk_cnt;
			break;
		}

		assert(iov_lbks > io->iov_off);
		lbk_cnt -= (iov_lbks - io->iov_off);
		io->iov_off = 0;
		io->iov_pos++;
	}
}

size_t
ftl_iovec_num_lbks(struct iovec *iov, size_t iov_cnt)
{
	size_t lbks = 0, i = 0;

	for (; i < iov_cnt; ++i) {
		lbks += iov[i].iov_len / PAGE_SIZE;
	}

	return lbks;
}

void *
ftl_io_iovec_addr(struct ftl_io *io)
{
	assert(io->iov_pos < io->iov_cnt);
	assert(io->iov_off * PAGE_SIZE < ftl_io_iovec(io)[io->iov_pos].iov_len);

	return (char *)ftl_io_iovec(io)[io->iov_pos].iov_base +
	       io->iov_off * PAGE_SIZE;
}

size_t
ftl_io_iovec_len_left(struct ftl_io *io)
{
	struct iovec *iov = ftl_io_iovec(io);
	return iov[io->iov_pos].iov_len / PAGE_SIZE - io->iov_off;
}

static void
_ftl_io_init_iovec(struct ftl_io *io, void *buf, size_t iov_cnt, size_t req_size)
{
	struct iovec *iov;
	size_t i;

	io->iov_pos = 0;
	io->iov_cnt = iov_cnt;
	io->lbk_cnt = iov_cnt * req_size;

	iov = ftl_io_iovec(io);
	for (i = 0; i < iov_cnt; ++i) {
		iov[i].iov_base = (char *)buf + i * req_size * PAGE_SIZE;
		iov[i].iov_len = req_size * PAGE_SIZE;
	}
}

static int
ftl_io_init_iovec(struct ftl_io *io, void *buf, size_t iov_cnt, size_t req_size)
{
	if (iov_cnt > 1) {
		io->iov.vector = calloc(iov_cnt, sizeof(struct iovec));
		if (!io->iov.vector) {
			return -ENOMEM;
		}
	}

	_ftl_io_init_iovec(io, buf, iov_cnt, req_size);

	return 0;
}

void
ftl_io_shrink_iovec(struct ftl_io *io, char *buf, size_t iov_cnt, size_t req_size)
{
	assert(io->iov_cnt >= iov_cnt);
	assert(io->lbk_cnt >= iov_cnt * req_size);
	assert(io->pos == 0 && io->iov_pos == 0 && io->iov_off == 0);

	if (iov_cnt == 1 && io->iov_cnt > 1) {
		free(io->iov.vector);
	}

	_ftl_io_init_iovec(io, buf, iov_cnt, req_size);
}

static void
ftl_io_init(struct ftl_io *io, struct spdk_ftl_dev *dev,
	    spdk_ftl_fn fn, void *ctx, int flags, int type)
{
	io->flags |= flags | FTL_IO_INITIALIZED;
	io->type = type;
	io->dev = dev;
	io->lba.single = FTL_LBA_INVALID;
	io->ppa.ppa = FTL_PPA_INVALID;
	io->cb.fn = fn;
	io->cb.ctx = ctx;
	io->trace = ftl_trace_alloc_id(dev);
}

struct ftl_io *
ftl_io_init_internal(const struct ftl_io_init_opts *opts)
{
	struct ftl_io *io = opts->io;
	struct spdk_ftl_dev *dev = opts->dev;

	if (!io) {
		if (opts->parent) {
			io = ftl_io_alloc_child(opts->parent);
		} else {
			io = ftl_io_alloc(dev->ioch);
		}

		if (!io) {
			return NULL;
		}
	}

	ftl_io_clear(io);
	ftl_io_init(io, dev, opts->fn, io, opts->flags | FTL_IO_INTERNAL, opts->type);

	io->rwb_batch = opts->rwb_batch;
	io->band = opts->band;
	io->md = opts->md;

	if (ftl_io_init_iovec(io, opts->data, opts->iov_cnt, opts->req_size)) {
		if (!opts->io) {
			ftl_io_free(io);
		}
		return NULL;
	}

	return io;
}

struct ftl_io *
ftl_io_rwb_init(struct spdk_ftl_dev *dev, struct ftl_band *band,
		struct ftl_rwb_batch *batch, spdk_ftl_fn cb)
{
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= batch,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= 0,
		.type		= FTL_IO_WRITE,
		.iov_cnt	= 1,
		.req_size	= dev->xfer_size,
		.fn		= cb,
		.data		= ftl_rwb_batch_get_data(batch),
		.md		= ftl_rwb_batch_get_md(batch),
	};

	return ftl_io_init_internal(&opts);
}

struct ftl_io *
ftl_io_erase_init(struct ftl_band *band, size_t lbk_cnt, spdk_ftl_fn cb)
{
	struct ftl_io *io;
	struct ftl_io_init_opts opts = {
		.dev		= band->dev,
		.io		= NULL,
		.rwb_batch	= NULL,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= FTL_IO_PPA_MODE,
		.type		= FTL_IO_ERASE,
		.iov_cnt	= 0,
		.req_size	= 1,
		.fn		= cb,
		.data		= NULL,
		.md		= NULL,
	};

	io = ftl_io_init_internal(&opts);
	io->lbk_cnt = lbk_cnt;

	return io;
}

void
ftl_io_user_init(struct spdk_ftl_dev *dev, struct ftl_io *io, uint64_t lba, size_t lbk_cnt,
		 struct iovec *iov, size_t iov_cnt,
		 spdk_ftl_fn cb_fn, void *cb_arg, int type)
{
	if (io->flags & FTL_IO_INITIALIZED) {
		return;
	}

	ftl_io_init(io, dev, cb_fn, cb_arg, 0, type);

	io->lba.single = lba;
	io->lbk_cnt = lbk_cnt;
	io->iov_cnt = iov_cnt;

	if (iov_cnt > 1) {
		io->iov.vector = iov;
	} else {
		io->iov.single = *iov;
	}

	ftl_trace_lba_io_init(io->dev, io);
}

static void
_ftl_io_free(struct ftl_io *io)
{
	struct ftl_io_channel *ioch;

	assert(LIST_EMPTY(&io->children));

	if ((io->flags & FTL_IO_INTERNAL) && io->iov_cnt > 1) {
		free(io->iov.vector);
	}

	if (pthread_spin_destroy(&io->lock)) {
		SPDK_ERRLOG("pthread_spin_destroy failed\n");
	}

	ioch = spdk_io_channel_get_ctx(io->ioch);
	spdk_mempool_put(ioch->io_pool, io);
}

static bool
ftl_io_remove_child(struct ftl_io *io)
{
	struct ftl_io *parent = io->parent;
	bool parent_done;

	pthread_spin_lock(&parent->lock);
	LIST_REMOVE(io, child_entry);
	parent_done = parent->done && LIST_EMPTY(&parent->children);
	parent->status = parent->status ? : io->status;
	pthread_spin_unlock(&parent->lock);

	return parent_done;
}

void
ftl_io_complete(struct ftl_io *io)
{
	struct ftl_io *parent = io->parent;
	bool complete, keep_alive = io->flags & FTL_IO_KEEP_ALIVE;

	io->flags &= ~FTL_IO_INITIALIZED;

	pthread_spin_lock(&io->lock);
	complete = LIST_EMPTY(&io->children);
	io->done = true;
	pthread_spin_unlock(&io->lock);

	if (complete) {
		if (io->cb.fn) {
			io->cb.fn(io->cb.ctx, io->status);
		}

		if (parent && ftl_io_remove_child(io)) {
			ftl_io_complete(parent);
		}

		if (!keep_alive) {
			_ftl_io_free(io);
		}
	}
}

struct ftl_io *
ftl_io_alloc_child(struct ftl_io *parent)
{
	struct ftl_io *io;

	io = ftl_io_alloc(parent->ioch);
	if (spdk_unlikely(!io)) {
		return NULL;
	}

	io->parent = parent;

	pthread_spin_lock(&parent->lock);
	LIST_INSERT_HEAD(&parent->children, io, child_entry);
	pthread_spin_unlock(&parent->lock);

	return io;
}

void
ftl_io_process_error(struct ftl_io *io, const struct spdk_nvme_cpl *status)
{
	/* TODO: add error handling for specifc cases */
	if (status->status.sct == SPDK_NVME_SCT_MEDIA_ERROR &&
	    status->status.sc == SPDK_OCSSD_SC_READ_HIGH_ECC) {
		return;
	}

	io->status = -EIO;
}

void *
ftl_io_get_md(const struct ftl_io *io)
{
	if (!io->md) {
		return NULL;
	}

	return (char *)io->md + io->pos * FTL_BLOCK_SIZE;
}

struct ftl_io *
ftl_io_alloc(struct spdk_io_channel *ch)
{
	struct ftl_io *io;
	struct ftl_io_channel *ioch = spdk_io_channel_get_ctx(ch);

	io = spdk_mempool_get(ioch->io_pool);
	if (!io) {
		return NULL;
	}

	memset(io, 0, ioch->elem_size);
	io->ioch = ch;

	if (pthread_spin_init(&io->lock, PTHREAD_PROCESS_PRIVATE)) {
		SPDK_ERRLOG("pthread_spin_init failed\n");
		spdk_mempool_put(ioch->io_pool, io);
		return NULL;
	}

	return io;
}

void
ftl_io_reinit(struct ftl_io *io, spdk_ftl_fn fn, void *ctx, int flags, int type)
{
	ftl_io_clear(io);
	ftl_io_init(io, io->dev, fn, ctx, flags, type);
}

void
ftl_io_clear(struct ftl_io *io)
{
	io->pos = 0;
	io->iov_pos = 0;
	io->iov_off = 0;
	io->done = false;
	io->req_cnt = 0;
	io->flags = 0;
	io->rwb_batch = NULL;
	io->band = NULL;
}

void
ftl_io_free(struct ftl_io *io)
{
	struct ftl_io *parent = io->parent;

	if (!io) {
		return;
	}

	if (parent && ftl_io_remove_child(io)) {
		ftl_io_complete(parent);
	}

	_ftl_io_free(io);
}
