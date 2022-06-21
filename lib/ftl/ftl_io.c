/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/ftl.h"
#include "spdk/likely.h"
#include "spdk/util.h"

#include "ftl_io.h"
#include "ftl_core.h"
#include "ftl_debug.h"
#include "utils/ftl_mempool.h"

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

	if (io->iov_cnt == 0) {
		return;
	}

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
ftl_io_cb(struct ftl_io *io, void *arg, int status)
{
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
			/* IO rescheduled, return from the function */
			return;
		}
	}

	/* User completion added in next patch */
}

int
ftl_io_init(struct spdk_io_channel *_ioch, struct ftl_io *io, uint64_t lba, size_t num_blocks,
	    struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_ctx, int type)
{
	/* dev initialized from io channel context in next patch */
	struct spdk_ftl_dev *dev = NULL;

	memset(io, 0, sizeof(struct ftl_io));
	io->ioch = _ioch;

	io->flags |= FTL_IO_INITIALIZED;
	io->type = type;
	io->dev = dev;
	io->lba = FTL_LBA_INVALID;
	io->addr = FTL_ADDR_INVALID;
	io->cb_ctx = cb_ctx;
	io->lba = lba;
	io->user_fn = cb_fn;
	io->iov = iov;
	io->iov_cnt = iov_cnt;
	io->num_blocks = num_blocks;

	return 0;
}

void
ftl_io_complete(struct ftl_io *io)
{
	io->flags &= ~FTL_IO_INITIALIZED;
	io->done = true;

	ftl_io_cb(io, io->cb_ctx, io->status);
}

void
ftl_io_fail(struct ftl_io *io, int status)
{
	io->status = status;
	ftl_io_advance(io, io->num_blocks - io->pos);
}

void
ftl_io_clear(struct ftl_io *io)
{
	io->req_cnt = io->pos = io->iov_pos = io->iov_off = 0;
	io->done = false;
	io->status = 0;
	io->flags = 0;
}
