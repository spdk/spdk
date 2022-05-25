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
#include "ftl_band.h"
#include "ftl_debug.h"

void
ftl_io_inc_req(struct ftl_io *io)
{
	io->dev->num_inflight++;
	io->req_cnt++;
}

void
ftl_io_dec_req(struct ftl_io *io)
{
	assert(io->dev->num_inflight > 0);
	assert(io->req_cnt > 0);

	io->dev->num_inflight--;
	io->req_cnt--;
}

struct iovec *
ftl_io_iovec(struct ftl_io *io)
{
	return &io->iov[0];
}

uint64_t
ftl_io_get_lba(const struct ftl_io *io, size_t offset)
{
	assert(offset < io->num_blocks);
	return io->lba + offset;
}

uint64_t
ftl_io_current_lba(const struct ftl_io *io)
{
	return ftl_io_get_lba(io, io->pos);
}

void
ftl_io_advance(struct ftl_io *io, size_t num_blocks)
{
	struct iovec *iov = ftl_io_iovec(io);
	size_t iov_blocks, block_left = num_blocks;

	io->pos += num_blocks;

	if (io->iov_cnt != 0) {
		while (block_left > 0) {
			assert(io->iov_pos < io->iov_cnt);
			iov_blocks = iov[io->iov_pos].iov_len / FTL_BLOCK_SIZE;

			if (io->iov_off + block_left < iov_blocks) {
				io->iov_off += block_left;
				break;
			}

			assert(iov_blocks > io->iov_off);
			block_left -= (iov_blocks - io->iov_off);
			io->iov_off = 0;
			io->iov_pos++;
		}
	}

	if (io->parent) {
		ftl_io_advance(io->parent, num_blocks);
	}
}

size_t
ftl_iovec_num_blocks(struct iovec *iov, size_t iov_cnt)
{
	size_t num_blocks = 0, i = 0;

	for (; i < iov_cnt; ++i) {
		if (iov[i].iov_len & (FTL_BLOCK_SIZE - 1)) {
			return 0;
		}

		num_blocks += iov[i].iov_len / FTL_BLOCK_SIZE;
	}

	return num_blocks;
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
	if (io->iov_pos < io->iov_cnt) {
		struct iovec *iov = ftl_io_iovec(io);
		return iov[io->iov_pos].iov_len / FTL_BLOCK_SIZE - io->iov_off;
	} else {
		return 0;
	}
}

static void
ftl_io_init_iovec(struct ftl_io *io, const struct iovec *iov, size_t iov_cnt, size_t iov_off,
		  size_t num_blocks)
{
	size_t offset = 0, num_left;

	io->iov_pos = 0;
	io->iov_cnt = 0;
	io->num_blocks = num_blocks;

	while (offset < num_blocks) {
		assert(io->iov_cnt < FTL_IO_MAX_IOVEC && io->iov_cnt < iov_cnt);

		num_left = spdk_min(iov[io->iov_cnt].iov_len / FTL_BLOCK_SIZE - iov_off,
				    num_blocks);
		io->iov[io->iov_cnt].iov_base = (char *)iov[io->iov_cnt].iov_base +
						iov_off * FTL_BLOCK_SIZE;
		io->iov[io->iov_cnt].iov_len = num_left * FTL_BLOCK_SIZE;

		offset += num_left;
		io->iov_cnt++;
		iov_off = 0;
	}
}

static void
ftl_io_init(struct ftl_io *io, struct spdk_ftl_dev *dev,
	    ftl_io_fn fn, void *ctx, int flags, int type)
{
	io->flags |= flags | FTL_IO_INITIALIZED;
	io->type = type;
	io->dev = dev;
	io->lba = FTL_LBA_INVALID;
	io->addr = FTL_ADDR_INVALID;
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
	const struct iovec *iov;
	size_t iov_cnt, iov_off;

	if (!io) {
		if (parent) {
			io = ftl_io_alloc_child(parent);
		} else {
			io = ftl_io_alloc(ftl_get_io_channel(dev));
		}

		if (!io) {
			return NULL;
		}
	}

	ftl_io_clear(io);
	ftl_io_init(io, dev, opts->cb_fn, opts->cb_ctx, opts->flags | FTL_IO_INTERNAL, opts->type);

	io->band = opts->band;
	io->md = opts->md;

	if (parent) {
		io->lba = parent->lba + parent->pos;

		if (opts->iovs[0].iov_base) {
			iov = &opts->iovs[0];
		} else {
			iov = &parent->iov[parent->iov_pos];
		}

		iov_cnt = parent->iov_cnt - parent->iov_pos;
		iov_off = parent->iov_off;
	} else {
		iov = &opts->iovs[0];
		iov_cnt = opts->iovcnt;
		iov_off = 0;
	}

	/* Some requests (zone resets) do not use iovecs */
	if (iov_cnt > 0) {
		ftl_io_init_iovec(io, iov, iov_cnt, iov_off, opts->num_blocks);
	}

	return io;
}

struct ftl_io *
ftl_io_erase_init(struct ftl_band *band, size_t num_blocks, ftl_io_fn cb)
{
	struct ftl_io *io;
	struct ftl_io_init_opts opts = {
		.dev		= band->dev,
		.io		= NULL,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= 0,
		.type		= FTL_IO_ERASE,
		.num_blocks	= 1,
		.cb_fn		= cb,
		.iovcnt		= 0,
		.md		= NULL,
		.ioch		= ftl_get_io_channel(band->dev),
	};

	io = ftl_io_init_internal(&opts);
	if (!io) {
		return NULL;
	}

	io->num_blocks = num_blocks;

	return io;
}

static void
ftl_io_user_enqueue_completion(void *_io)
{
	struct ftl_io *io = _io;
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(io->ioch);
	size_t result;

	result = spdk_ring_enqueue(ioch->cq, (void **)&io, 1, NULL);
	if (spdk_unlikely(result == 0)) {
		spdk_thread_send_msg(spdk_get_thread(), ftl_io_user_enqueue_completion, io);
	}
}

static void
ftl_io_user_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(io->ioch);

	if (spdk_unlikely(status)) {
		io->status = status;

		if (-EAGAIN == status) {
			/* IO has to be rescheduled again */
			switch (io->type) {
			case FTL_IO_READ:
				ftl_io_clear(io);
				TAILQ_INSERT_HEAD(&io->dev->rd_sq, io, queue_entry);
				break;
			case FTL_IO_WRITE:
				ftl_io_clear(io);
				TAILQ_INSERT_HEAD(&io->dev->wr_sq, io, queue_entry);
				break;
			default:
				/* Unknown IO type, complete to the user */
				assert(0);
				break;
			}

		}

		if (!io->status) {
			/* IO rescheduled, return form the function */
			return;
		}
	}

	if (io->map) {
		ftl_mempool_put(ioch->map_pool, io->map);
	}

	ftl_io_user_enqueue_completion(io);
}

int
ftl_io_user_init(struct spdk_io_channel *_ioch, struct ftl_io *io, uint64_t lba, size_t num_blocks,
		 struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_ctx, int type)
{
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(_ioch);
	struct spdk_ftl_dev *dev = ioch->dev;

	memset(io, 0, sizeof(struct ftl_io));
	io->ioch = _ioch;

	ftl_io_init(io, dev, ftl_io_user_cb, cb_ctx, 0, type);
	io->lba = lba;
	io->user_fn = cb_fn;
	io->iov = iov;
	io->iov_cnt = iov_cnt;
	io->num_blocks = num_blocks;

	ftl_trace_lba_io_init(io->dev, io);
	return 0;
}

static void
_ftl_io_free(struct ftl_io *io)
{
	struct ftl_io_channel *ioch;

	assert(LIST_EMPTY(&io->children));

	if (io->flags & FTL_IO_INTERNAL) {
		ioch = ftl_io_channel_get_ctx(io->ioch);
		spdk_mempool_put(ioch->io_pool, io);
	}
}

static bool
ftl_io_remove_child(struct ftl_io *io)
{
	struct ftl_io *parent = io->parent;
	bool parent_done;

	LIST_REMOVE(io, child_entry);
	parent_done = parent->done && LIST_EMPTY(&parent->children);

	if (io->status) {
		parent->status = io->status;
	}

	return parent_done;
}

static void
ftl_io_complete_verify(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	uint64_t i;
	uint64_t lba = io->lba;

	assert(io->num_blocks <= dev->xfer_size);

	if (FTL_IO_WRITE == io->type) {
		return;
	}

	if (spdk_unlikely(io->status)) {
		return;
	}

	for (i = 0; i < io->num_blocks; i++, lba++) {
		ftl_addr current_addr = ftl_l2p_get(dev, lba);

		if (spdk_unlikely(current_addr != io->map[i])) {
			io->status = -EAGAIN;
			break;
		}
	}
}

void
ftl_io_complete(struct ftl_io *io)
{
	struct ftl_io *parent = io->parent;
	bool complete;

	io->flags &= ~FTL_IO_INITIALIZED;

	complete = LIST_EMPTY(&io->children);
	io->done = true;

	if (complete) {
		if (io->flags & FTL_IO_PINNED) {
			ftl_io_complete_verify(io);
			ftl_l2p_unpin(io->dev, io->lba, io->num_blocks);
		}

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

	io = ftl_io_alloc(parent->dev->ioch);
	if (spdk_unlikely(!io)) {
		return NULL;
	}

	ftl_io_init(io, parent->dev, NULL, NULL, parent->flags, parent->type);
	io->parent = parent;
	LIST_INSERT_HEAD(&parent->children, io, child_entry);

	return io;
}

void ftl_io_fail(struct ftl_io *io, int status)
{
	io->status = status;
	ftl_io_advance(io, io->num_blocks - io->pos);
}

struct ftl_io *
ftl_io_alloc(struct spdk_io_channel *ch)
{
	struct ftl_io *io;
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(ch);

	io = spdk_mempool_get(ioch->io_pool);
	if (!io) {
		return NULL;
	}

	memset(io, 0, ioch->io_pool_elem_size);
	io->ioch = ch;

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
	io->band = NULL;
}

void
ftl_io_reset(struct ftl_io *io)
{
	io->req_cnt = io->pos = io->iov_pos = io->iov_off = 0;
	io->done = false;
	io->status = 0;
}

void
ftl_io_free(struct ftl_io *io)
{
	struct ftl_io *parent;

	if (!io) {
		return;
	}

	parent = io->parent;
	if (parent && ftl_io_remove_child(io)) {
		ftl_io_complete(parent);
	}

	_ftl_io_free(io);
}
