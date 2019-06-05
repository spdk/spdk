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
#include "spdk/util.h"

#include "ftl_io.h"
#include "ftl_core.h"
#include "ftl_rwb.h"
#include "ftl_band.h"

void
ftl_io_inc_req(struct ftl_io *io)
{
	struct ftl_band *band = io->band;

	if (!(io->flags & FTL_IO_CACHE) && io->type != FTL_IO_READ && io->type != FTL_IO_ERASE) {
		ftl_band_acquire_lba_map(band);
	}

	__atomic_fetch_add(&io->dev->num_inflight, 1, __ATOMIC_SEQ_CST);

	++io->req_cnt;
}

void
ftl_io_dec_req(struct ftl_io *io)
{
	struct ftl_band *band = io->band;
	unsigned long num_inflight __attribute__((unused));

	if (!(io->flags & FTL_IO_CACHE) && io->type != FTL_IO_READ && io->type != FTL_IO_ERASE) {
		ftl_band_release_lba_map(band);
	}

	num_inflight = __atomic_fetch_sub(&io->dev->num_inflight, 1, __ATOMIC_SEQ_CST);

	assert(num_inflight > 0);
	assert(io->req_cnt > 0);

	--io->req_cnt;
}

struct iovec *
ftl_io_iovec(struct ftl_io *io)
{
	return &io->iov[0];
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
ftl_io_advance(struct ftl_io *io, size_t lbk_cnt)
{
	struct iovec *iov = ftl_io_iovec(io);
	size_t iov_lbks, lbk_left = lbk_cnt;

	io->pos += lbk_cnt;

	if (io->iov_cnt != 0) {
		while (lbk_left > 0) {
			assert(io->iov_pos < io->iov_cnt);
			iov_lbks = iov[io->iov_pos].iov_len / FTL_BLOCK_SIZE;

			if (io->iov_off + lbk_left < iov_lbks) {
				io->iov_off += lbk_left;
				break;
			}

			assert(iov_lbks > io->iov_off);
			lbk_left -= (iov_lbks - io->iov_off);
			io->iov_off = 0;
			io->iov_pos++;
		}
	}

	if (io->parent) {
		ftl_io_advance(io->parent, lbk_cnt);
	}
}

size_t
ftl_iovec_num_lbks(struct iovec *iov, size_t iov_cnt)
{
	size_t lbks = 0, i = 0;

	for (; i < iov_cnt; ++i) {
		lbks += iov[i].iov_len / FTL_BLOCK_SIZE;
	}

	return lbks;
}

void *
ftl_io_iovec_addr(struct ftl_io *io)
{
	assert(io->iov_pos < io->iov_cnt);
	assert(io->iov_off * FTL_BLOCK_SIZE < ftl_io_iovec(io)[io->iov_pos].iov_len);

	return (char *)ftl_io_iovec(io)[io->iov_pos].iov_base +
	       io->iov_off * FTL_BLOCK_SIZE;
}

size_t
ftl_io_iovec_len_left(struct ftl_io *io)
{
	struct iovec *iov = ftl_io_iovec(io);
	return iov[io->iov_pos].iov_len / FTL_BLOCK_SIZE - io->iov_off;
}

static void
_ftl_io_init_iovec(struct ftl_io *io, const struct iovec *iov, size_t iov_cnt, size_t lbk_cnt)
{
	size_t iov_off;

	io->iov_pos = 0;
	io->iov_cnt = iov_cnt;
	io->lbk_cnt = lbk_cnt;

	memcpy(io->iov, iov, iov_cnt * sizeof(*iov));

	if (lbk_cnt == 0) {
		for (iov_off = 0; iov_off < iov_cnt; ++iov_off) {
			io->lbk_cnt += iov[iov_off].iov_len / FTL_BLOCK_SIZE;
		}
	}
}

static void _ftl_io_free(struct ftl_io *io);

static int
ftl_io_add_child(struct ftl_io *io, const struct iovec *iov, size_t iov_cnt)
{
	struct ftl_io *child;

	child = ftl_io_alloc_child(io);
	if (spdk_unlikely(!child)) {
		return -ENOMEM;
	}

	_ftl_io_init_iovec(child, iov, iov_cnt, 0);

	if (io->flags & FTL_IO_VECTOR_LBA) {
		child->lba.vector = io->lba.vector + io->lbk_cnt;
	} else {
		child->lba.single = io->lba.single + io->lbk_cnt;
	}

	io->lbk_cnt += child->lbk_cnt;
	return 0;
}

static int
ftl_io_init_iovec(struct ftl_io *io, const struct iovec *iov, size_t iov_cnt, size_t lbk_cnt)
{
	struct ftl_io *child;
	size_t iov_off = 0, iov_left;
	int rc;

	if (spdk_likely(iov_cnt <= FTL_IO_MAX_IOVEC)) {
		_ftl_io_init_iovec(io, iov, iov_cnt, lbk_cnt);
		return 0;
	}

	while (iov_off < iov_cnt) {
		iov_left = spdk_min(iov_cnt - iov_off, FTL_IO_MAX_IOVEC);

		rc = ftl_io_add_child(io, &iov[iov_off], iov_left);
		if (spdk_unlikely(rc != 0)) {
			while ((child = LIST_FIRST(&io->children))) {
				assert(LIST_EMPTY(&child->children));
				LIST_REMOVE(child, child_entry);
				_ftl_io_free(child);
			}

			return -ENOMEM;
		}

		iov_off += iov_left;
	}

	assert(io->lbk_cnt == lbk_cnt);
	return 0;
}

void
ftl_io_shrink_iovec(struct ftl_io *io, size_t lbk_cnt)
{
	size_t iov_off = 0, lbk_off = 0;

	assert(io->lbk_cnt >= lbk_cnt);
	assert(io->pos == 0 && io->iov_pos == 0 && io->iov_off == 0);

	for (; iov_off < io->iov_cnt; ++iov_off) {
		size_t num_iov = io->iov[iov_off].iov_len / FTL_BLOCK_SIZE;
		size_t num_left = lbk_cnt - lbk_off;

		if (num_iov >= num_left) {
			io->iov[iov_off].iov_len = num_left * FTL_BLOCK_SIZE;
			io->iov_cnt = iov_off + 1;
			io->lbk_cnt = lbk_cnt;
			break;
		}

		lbk_off += num_iov;
	}
}

static void
ftl_io_init(struct ftl_io *io, struct spdk_ftl_dev *dev,
	    ftl_io_fn fn, void *ctx, int flags, int type)
{
	io->flags |= flags | FTL_IO_INITIALIZED;
	io->type = type;
	io->dev = dev;
	io->lba.single = FTL_LBA_INVALID;
	io->ppa.ppa = FTL_PPA_INVALID;
	io->cb_fn = fn;
	io->cb_ctx = ctx;
	io->trace = ftl_trace_alloc_id(dev);
}

struct ftl_io *
ftl_io_init_internal(const struct ftl_io_init_opts *opts)
{
	struct ftl_io *io = opts->io;
	struct ftl_io *parent = opts->parent;
	struct spdk_ftl_dev *dev = opts->dev;
	struct iovec iov = {
		.iov_base = opts->data,
		.iov_len  = opts->lbk_cnt * FTL_BLOCK_SIZE
	};

	if (!io) {
		if (parent) {
			io = ftl_io_alloc_child(parent);
		} else {
			io = ftl_io_alloc(dev->ioch);
		}

		if (!io) {
			return NULL;
		}
	}

	ftl_io_clear(io);
	ftl_io_init(io, dev, opts->cb_fn, opts->cb_ctx, opts->flags | FTL_IO_INTERNAL, opts->type);

	io->rwb_batch = opts->rwb_batch;
	io->band = opts->band;
	io->md = opts->md;

	if (parent) {
		if (parent->flags & FTL_IO_VECTOR_LBA) {
			io->lba.vector = parent->lba.vector + parent->pos;
		} else {
			io->lba.single = parent->lba.single + parent->pos;
		}
	}

	if (ftl_io_init_iovec(io, &iov, 1, opts->lbk_cnt)) {
		if (!opts->io) {
			ftl_io_free(io);
		}
		return NULL;
	}

	if (opts->flags & FTL_IO_VECTOR_LBA) {
		io->lba.vector = calloc(io->lbk_cnt, sizeof(uint64_t));
		if (!io->lba.vector) {
			ftl_io_free(io);
			return NULL;
		}
	}

	return io;
}

struct ftl_io *
ftl_io_rwb_init(struct spdk_ftl_dev *dev, struct ftl_band *band,
		struct ftl_rwb_batch *batch, ftl_io_fn cb)
{
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= batch,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= 0,
		.type		= FTL_IO_WRITE,
		.lbk_cnt	= dev->xfer_size,
		.cb_fn		= cb,
		.data		= ftl_rwb_batch_get_data(batch),
		.md		= ftl_rwb_batch_get_md(batch),
	};

	return ftl_io_init_internal(&opts);
}

struct ftl_io *
ftl_io_erase_init(struct ftl_band *band, size_t lbk_cnt, ftl_io_fn cb)
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
		.lbk_cnt	= 1,
		.cb_fn		= cb,
		.data		= NULL,
		.md		= NULL,
	};

	io = ftl_io_init_internal(&opts);
	if (!io) {
		return NULL;
	}

	io->lbk_cnt = lbk_cnt;

	return io;
}

static void
_ftl_user_cb(struct ftl_io *io, void *arg, int status)
{
	io->user_fn(arg, status);
}

struct ftl_io *
ftl_io_user_init(struct spdk_io_channel *_ioch, uint64_t lba, size_t lbk_cnt, struct iovec *iov,
		 size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_ctx, int type)
{
	struct ftl_io_channel *ioch = spdk_io_channel_get_ctx(_ioch);
	struct spdk_ftl_dev *dev = ioch->dev;
	struct ftl_io *io;

	io = ftl_io_alloc(_ioch);
	if (spdk_unlikely(!io)) {
		return NULL;
	}

	ftl_io_init(io, dev, _ftl_user_cb, cb_ctx, 0, type);
	io->lba.single = lba;
	io->user_fn = cb_fn;

	if (ftl_io_init_iovec(io, iov, iov_cnt, lbk_cnt)) {
		ftl_io_free(io);
		return NULL;
	}

	ftl_trace_lba_io_init(io->dev, io);
	return io;
}

static void
_ftl_io_free(struct ftl_io *io)
{
	struct ftl_io_channel *ioch;

	assert(LIST_EMPTY(&io->children));

	if (io->flags & FTL_IO_VECTOR_LBA) {
		free(io->lba.vector);
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
	bool complete;

	io->flags &= ~FTL_IO_INITIALIZED;

	pthread_spin_lock(&io->lock);
	complete = LIST_EMPTY(&io->children);
	io->done = true;
	pthread_spin_unlock(&io->lock);

	if (complete) {
		if (io->cb_fn) {
			io->cb_fn(io, io->cb_ctx, io->status);
		}

		if (parent && ftl_io_remove_child(io)) {
			ftl_io_complete(parent);
		}

		_ftl_io_free(io);
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

	ftl_io_init(io, parent->dev, NULL, NULL, parent->flags, parent->type);
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

void ftl_io_fail(struct ftl_io *io, int status)
{
	io->status = status;
	ftl_io_advance(io, io->lbk_cnt - io->pos);
}

void *
ftl_io_get_md(const struct ftl_io *io)
{
	if (!io->md) {
		return NULL;
	}

	return (char *)io->md + io->pos * io->dev->md_size;
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
ftl_io_reinit(struct ftl_io *io, ftl_io_fn cb, void *ctx, int flags, int type)
{
	ftl_io_clear(io);
	ftl_io_init(io, io->dev, cb, ctx, flags, type);
}

void
ftl_io_clear(struct ftl_io *io)
{
	ftl_io_reset(io);

	io->flags = 0;
	io->rwb_batch = NULL;
	io->band = NULL;
}

void
ftl_io_reset(struct ftl_io *io)
{
	io->req_cnt = io->pos = io->iov_pos = io->iov_off = 0;
	io->done = false;
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

void
ftl_io_call_foreach_child(struct ftl_io *io, int (*callback)(struct ftl_io *))
{
	struct ftl_io *child, *tmp;

	assert(!io->done);

	/*
	 * If the IO doesn't have any children, it means that it directly describes a request (i.e.
	 * all of the buffers, LBAs, etc. are filled). Otherwise the IO only groups together several
	 * requests and may be partially filled, so the callback needs to be called on all of its
	 * children instead.
	 */
	if (LIST_EMPTY(&io->children)) {
		callback(io);
		return;
	}

	LIST_FOREACH_SAFE(child, &io->children, child_entry, tmp) {
		int rc = callback(child);
		if (rc) {
			assert(rc != -EAGAIN);
			ftl_io_fail(io, rc);
			break;
		}
	}

	/*
	 * If all the callbacks were processed or an error occurred, treat this IO as completed.
	 * Multiple calls to ftl_io_call_foreach_child are not supported, resubmissions are supposed
	 * to be handled in the callback.
	 */
	ftl_io_complete(io);
}
