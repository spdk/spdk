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

#include <spdk/stdinc.h>
#include <spdk/ocssd.h>
#include "ocssd_utils.h"
#include "ocssd_io.h"
#include "ocssd_core.h"
#include "ocssd_band.h"
#include "ocssd_rwb.h"

size_t
ocssd_io_inc_req(struct ocssd_io *io)
{
	struct ocssd_band *band = io->band;

	if (ocssd_io_get_type(io) != OCSSD_IO_READ &&
	    ocssd_io_get_type(io) != OCSSD_IO_ERASE) {
		ocssd_band_acquire_md(band);
	}

	atomic_fetch_add(&io->dev->num_inflight, 1);
	io->req_cnt++;

	return io->req_cnt;
}

size_t
ocssd_io_dec_req(struct ocssd_io *io)
{
	struct ocssd_band *band = io->band;

	assert(io->req_cnt);
	assert(atomic_load(&io->dev->num_inflight));

	if (ocssd_io_get_type(io) != OCSSD_IO_READ &&
	    ocssd_io_get_type(io) != OCSSD_IO_ERASE) {
		ocssd_band_release_md(band);
	}

	atomic_fetch_sub(&io->dev->num_inflight, 1);
	io->req_cnt--;

	return io->req_cnt;
}

struct iovec *
ocssd_io_iovec(struct ocssd_io *io)
{
	if (io->iov_cnt > 1) {
		return io->iovs;
	} else {
		return &io->iov;
	}
}

uint64_t
ocssd_io_current_lba(struct ocssd_io *io)
{
	if (ocssd_io_vector_lba(io)) {
		return io->lbas[io->pos];
	} else {
		return io->lba + io->pos;
	}
}

void
ocssd_io_update_iovec(struct ocssd_io *io, size_t lbk_cnt)
{
	struct iovec *iov = ocssd_io_iovec(io);
	size_t iov_lbks;

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
ocssd_iovec_num_lbks(struct iovec *iov, size_t iov_cnt)
{
	size_t lbks = 0, i = 0;

	for (; i < iov_cnt; ++i) {
		lbks += iov[i].iov_len / PAGE_SIZE;
	}

	return lbks;
}

void *
ocssd_io_iovec_addr(struct ocssd_io *io)
{
	assert(io->iov_pos < io->iov_cnt);
	assert(io->iov_off * PAGE_SIZE < ocssd_io_iovec(io)[io->iov_pos].iov_len);

	return (char *)ocssd_io_iovec(io)[io->iov_pos].iov_base +
	       io->iov_off * PAGE_SIZE;
}

size_t
ocssd_io_iovec_len_left(struct ocssd_io *io)
{
	struct iovec *iov = ocssd_io_iovec(io);
	return iov[io->iov_pos].iov_len / PAGE_SIZE - io->iov_off;
}

int
ocssd_io_init_iovec(struct ocssd_io *io, void *buf,
		    size_t iov_cnt, size_t req_size)
{
	struct iovec *iov;
	size_t i;

	if (iov_cnt > 1) {
		iov = io->iovs = calloc(iov_cnt, sizeof(*iov));
		if (!iov) {
			return -ENOMEM;
		}
	} else {
		iov = &io->iov;
	}

	io->iov_pos = 0;
	io->iov_cnt = iov_cnt;
	for (i = 0; i < iov_cnt; ++i) {
		iov[i].iov_base = (char *)buf + i * req_size * PAGE_SIZE;
		iov[i].iov_len = req_size * PAGE_SIZE;
	}

	return 0;
}

void
ocssd_io_init(struct ocssd_io *io, struct ocssd_dev *dev,
	      ocssd_fn fn, void *ctx, int flags, int type)
{
	ocssd_io_set_flags(io, flags | OCSSD_IO_INITIALIZED);
	ocssd_io_set_type(io, type);
	io->dev = dev;
	io->lba = OCSSD_LBA_INVALID;
	io->cb.fn = fn;
	io->cb.ctx = ctx;
	io->trace = ocssd_trace_alloc_group(dev->stats.trace);
}

struct ocssd_io *
ocssd_io_init_internal(const struct ocssd_io_init_opts *opts)
{
	struct ocssd_io *io = opts->io;

	if (!io) {
		assert(opts->size);
		io = calloc(1, opts->size);
		if (!io) {
			return NULL;
		}
	}

	ocssd_io_clear(io);
	ocssd_io_init(io, opts->dev, opts->fn, io, opts->flags | OCSSD_IO_INTERNAL, opts->type);

	io->lbk_cnt = opts->iov_cnt * opts->req_size;
	io->rwb_batch = opts->rwb_batch;
	io->band = opts->band;
	io->md = io->md;

	if (ocssd_io_init_iovec(io, opts->data, opts->iov_cnt, opts->req_size)) {
		if (!opts->io) {
			free(io);
		}
		return NULL;
	}

	return io;
}

struct ocssd_io *
ocssd_io_rwb_init(struct ocssd_dev *dev, struct ocssd_band *band,
		  struct ocssd_rwb_batch *batch, ocssd_fn cb)
{
	struct ocssd_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= batch,
		.band		= band,
		.size		= sizeof(struct ocssd_io),
		.flags		= OCSSD_IO_MEMORY,
		.type		= OCSSD_IO_WRITE,
		.iov_cnt	= 1,
		.req_size	= dev->xfer_size,
		.fn		= cb,
		.data		= ocssd_rwb_batch_data(batch),
		.md		= ocssd_rwb_batch_md(batch),
	};

	return ocssd_io_init_internal(&opts);
}

struct ocssd_io *
ocssd_io_erase_init(struct ocssd_band *band, size_t lbk_cnt, ocssd_fn cb)
{
	struct ocssd_io *io;
	struct ocssd_io_init_opts opts = {
		.dev		= band->dev,
		.io		= NULL,
		.rwb_batch	= NULL,
		.band		= band,
		.size		= sizeof(struct ocssd_io),
		.flags		= OCSSD_IO_MEMORY | OCSSD_IO_PPA_MODE,
		.type		= OCSSD_IO_ERASE,
		.iov_cnt	= 0,
		.req_size	= 1,
		.fn		= cb,
		.data		= NULL,
		.md		= NULL,
	};

	io = ocssd_io_init_internal(&opts);
	io->lbk_cnt = lbk_cnt;

	return io;
}

void
ocssd_io_user_init(struct ocssd_io *io, uint64_t lba, size_t lbk_cnt, struct iovec *iov,
		   size_t iov_cnt, const struct ocssd_cb *cb, int type)
{
	if (ocssd_io_initialized(io)) {
		return;
	}

	ocssd_io_init(io, io->dev, cb->fn, cb->ctx, 0, type);

	io->lba = lba;
	io->lbk_cnt = lbk_cnt;
	io->iov_cnt = iov_cnt;

	if (iov_cnt > 1) {
		io->iovs = iov;
	} else {
		io->iov = *iov;
	}

	ocssd_trace(lba_io_init, ocssd_dev_trace(io->dev), io);
}

void
ocssd_io_complete(struct ocssd_io *io)
{
	int mem_free = ocssd_io_mem_free(io);

	ocssd_io_clear_flags(io, OCSSD_IO_INITIALIZED);
	io->cb.fn(io->cb.ctx, io->status);

	if (mem_free) {
		spdk_ocssd_io_free(io);
	}
}

void
ocssd_io_process_error(struct ocssd_io *io, const struct spdk_nvme_cpl *status)
{
	io->status = -EIO;

	/* TODO: add error handling for specifc cases */
	if (status->status.sct == SPDK_NVME_SCT_MEDIA_ERROR &&
	    status->status.sc == SPDK_OCSSD_SC_READ_HIGH_ECC) {
		io->status = 0;
	}
}

void *
ocssd_io_get_md(const struct ocssd_io *io)
{
	if (!io->md) {
		return NULL;
	}

	return (char *)io->md + io->pos * OCSSD_BLOCK_SIZE;
}

struct ocssd_io *
spdk_ocssd_io_alloc(struct ocssd_dev *dev)
{
	struct ocssd_io *io;

	io = calloc(1, sizeof(*io));
	if (!io) {
		return NULL;
	}

	io->dev = dev;
	return io;
}

void
ocssd_io_reinit(struct ocssd_io *io, ocssd_fn fn, void *ctx, int flags, int type)
{
	ocssd_io_clear(io);
	ocssd_io_init(io, io->dev, fn, ctx, flags, type);
}

void
spdk_ocssd_io_free(struct ocssd_io *io)
{
	if (!io) {
		return;
	}

	if (ocssd_io_internal(io) && io->iov_cnt > 1) {
		free(io->iovs);
	}

	free(io);
}
