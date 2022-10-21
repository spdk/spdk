/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_band.h"
#include "ftl_core.h"
#include "ftl_debug.h"
#include "ftl_io.h"
#include "ftl_internal.h"
#include "spdk/ftl.h"
#include "spdk/likely.h"

struct ftl_reloc;
struct ftl_band_reloc;

/* TODO: Should probably change the move naming nomenclature to something more descriptive */
enum ftl_reloc_move_state {
	FTL_RELOC_STATE_READ = 0,
	FTL_RELOC_STATE_PIN,
	FTL_RELOC_STATE_WRITE,
	FTL_RELOC_STATE_WAIT,
	FTL_RELOC_STATE_HALT,

	FTL_RELOC_STATE_MAX
};

struct ftl_reloc_move {
	/* FTL device */
	struct spdk_ftl_dev *dev;

	struct ftl_reloc *reloc;

	/* Request for doing IO */
	struct ftl_rq *rq;

	/* Move state (read, write) */
	enum ftl_reloc_move_state state;

	/* Entry of circular list */
	TAILQ_ENTRY(ftl_reloc_move) qentry;
};

struct ftl_reloc {
	/* Device associated with relocate */
	struct spdk_ftl_dev *dev;

	/* Indicates relocate is about to halt */
	bool halt;

	/* Band which are read to relocate */
	struct ftl_band *band;

	/* Bands already read, but waiting for finishing GC */
	TAILQ_HEAD(, ftl_band) band_done;
	size_t band_done_count;

	/* Flags indicating reloc is waiting for a new band */
	bool band_waiting;

	/* Maximum number of IOs per band */
	size_t max_qdepth;

	/* Queue of free move objects */
	struct ftl_reloc_move *move_buffer;

	/* Array of movers queue for each state */
	TAILQ_HEAD(, ftl_reloc_move) move_queue[FTL_RELOC_STATE_MAX];

};

static void move_read_cb(struct ftl_rq *rq);
static void move_write_cb(struct ftl_rq *rq);
static void move_set_state(struct ftl_reloc_move *mv, enum ftl_reloc_move_state state);
static void move_write(struct ftl_reloc *reloc, struct ftl_reloc_move *mv);
static void move_read_error_cb(struct ftl_rq *rq, struct ftl_band *band, uint64_t idx,
			       uint64_t count);

static void
move_deinit(struct ftl_reloc_move *mv)
{
	assert(mv);
	ftl_rq_del(mv->rq);
}

static int
move_init(struct ftl_reloc *reloc, struct ftl_reloc_move *mv)
{
	mv->state = FTL_RELOC_STATE_HALT;
	TAILQ_INSERT_TAIL(&reloc->move_queue[FTL_RELOC_STATE_HALT], mv, qentry);

	mv->reloc = reloc;
	mv->dev = reloc->dev;
	mv->rq = ftl_rq_new(mv->dev, mv->dev->md_size);

	if (!mv->rq) {
		return -ENOMEM;
	}
	mv->rq->owner.priv = mv;

	return 0;
}

struct ftl_reloc *
ftl_reloc_init(struct spdk_ftl_dev *dev)
{
	struct ftl_reloc *reloc;
	struct ftl_reloc_move *move;
	size_t i, count;

	reloc = calloc(1, sizeof(*reloc));
	if (!reloc) {
		return NULL;
	}

	reloc->dev = dev;
	reloc->halt = true;
	reloc->max_qdepth = dev->sb->max_reloc_qdepth;

	reloc->move_buffer = calloc(reloc->max_qdepth, sizeof(*reloc->move_buffer));
	if (!reloc->move_buffer) {
		FTL_ERRLOG(dev, "Failed to initialize reloc moves pool");
		goto error;
	}

	/* Initialize movers queues */
	count = SPDK_COUNTOF(reloc->move_queue);
	for (i = 0; i < count; ++i) {
		TAILQ_INIT(&reloc->move_queue[i]);
	}

	for (i = 0; i < reloc->max_qdepth; ++i) {
		move = &reloc->move_buffer[i];

		if (move_init(reloc, move)) {
			goto error;
		}
	}

	TAILQ_INIT(&reloc->band_done);

	return reloc;
error:
	ftl_reloc_free(reloc);
	return NULL;
}

struct ftl_reloc_task_fini {
	struct ftl_reloc_task *task;
	spdk_msg_fn cb;
	void *cb_arg;
};

void
ftl_reloc_free(struct ftl_reloc *reloc)
{
	size_t i;

	if (!reloc) {
		return;
	}

	if (reloc->move_buffer) {
		for (i = 0; i < reloc->max_qdepth; ++i) {
			move_deinit(&reloc->move_buffer[i]);
		}
	}

	free(reloc->move_buffer);
	free(reloc);
}

void
ftl_reloc_halt(struct ftl_reloc *reloc)
{
	reloc->halt = true;
}

void
ftl_reloc_resume(struct ftl_reloc *reloc)
{
	struct ftl_reloc_move *mv, *next;
	reloc->halt = false;

	TAILQ_FOREACH_SAFE(mv, &reloc->move_queue[FTL_RELOC_STATE_HALT], qentry,
			   next) {
		move_set_state(mv, FTL_RELOC_STATE_READ);
	}
}

static void
move_set_state(struct ftl_reloc_move *mv, enum ftl_reloc_move_state state)
{
	struct ftl_reloc *reloc = mv->reloc;

	switch (state) {
	case FTL_RELOC_STATE_READ:
		mv->rq->owner.cb = move_read_cb;
		mv->rq->owner.error = move_read_error_cb;
		mv->rq->iter.idx = 0;
		mv->rq->iter.count = 0;
		mv->rq->success = true;
		break;

	case FTL_RELOC_STATE_WRITE:
		mv->rq->owner.cb = move_write_cb;
		mv->rq->owner.error = NULL;
		break;

	case FTL_RELOC_STATE_PIN:
	case FTL_RELOC_STATE_WAIT:
	case FTL_RELOC_STATE_HALT:
		break;

	default:
		ftl_abort();
		break;
	}

	if (mv->state != state) {
		/* Remove the mover from previous queue */
		TAILQ_REMOVE(&reloc->move_queue[mv->state], mv, qentry);
		/* Insert the mover to the new queue */
		TAILQ_INSERT_TAIL(&reloc->move_queue[state], mv, qentry);
		/* Update state */
		mv->state = state;
	}
}

static void
move_get_band_cb(struct ftl_band *band, void *cntx, bool status)
{
	struct ftl_reloc *reloc = cntx;

	if (spdk_likely(status)) {
		reloc->band = band;
		ftl_band_iter_init(band);
	}
	reloc->band_waiting = false;
}

static void
move_grab_new_band(struct ftl_reloc *reloc)
{
	if (!reloc->band_waiting) {
		if (!ftl_needs_reloc(reloc->dev)) {
			return;
		}

		/* Limit number of simultaneously relocated bands */
		if (reloc->band_done_count > 2) {
			return;
		}

		reloc->band_waiting = true;
		ftl_band_get_next_gc(reloc->dev, move_get_band_cb, reloc);
	}
}

static struct ftl_band *
move_get_band(struct ftl_reloc *reloc)
{
	struct ftl_band *band = reloc->band;

	if (!band) {
		move_grab_new_band(reloc);
		return NULL;
	}

	if (!ftl_band_filled(band, band->md->iter.offset)) {
		/* Band still not read, we can continue reading */
		return band;
	}

	TAILQ_INSERT_TAIL(&reloc->band_done, band, queue_entry);
	reloc->band_done_count++;
	reloc->band = NULL;

	return NULL;
}

static void
move_advance_rq(struct ftl_rq *rq)
{
	struct ftl_band *band = rq->io.band;
	uint64_t offset, i;
	struct ftl_rq_entry *entry = &rq->entries[rq->iter.idx];

	assert(rq->iter.idx + rq->iter.count <= rq->num_blocks);

	for (i = 0; i < rq->iter.count; i++) {
		offset = ftl_band_block_offset_from_addr(band, rq->io.addr);

		assert(offset < ftl_get_num_blocks_in_band(band->dev));
		assert(ftl_band_block_offset_valid(band, offset));

		entry->lba = band->p2l_map.band_map[offset].lba;
		entry->addr = rq->io.addr;
		entry->owner.priv = band;
		entry->seq_id = band->p2l_map.band_map[offset].seq_id;

		entry++;
		rq->io.addr = ftl_band_next_addr(band, rq->io.addr, 1);
		band->owner.cnt++;
	}

	/* Increase QD for the request */
	rq->iter.qd++;

	/* Advanced request iterator */
	rq->iter.idx += rq->iter.count;
}

static void
move_init_entries(struct ftl_rq *rq, uint64_t idx, uint64_t count)
{
	uint64_t i = 0;
	struct ftl_rq_entry *iter = &rq->entries[idx];

	assert(idx + count <= rq->num_blocks);

	i = 0;
	while (i < count) {
		iter->addr = FTL_ADDR_INVALID;
		iter->owner.priv = NULL;
		iter->lba = FTL_LBA_INVALID;
		iter->seq_id = 0;
		iter++;
		i++;
	}
}

static void
move_read_error_cb(struct ftl_rq *rq, struct ftl_band *band, uint64_t idx, uint64_t count)
{
	move_init_entries(rq, idx, count);
	band->owner.cnt -= count;
}

static void
move_read_cb(struct ftl_rq *rq)
{
	struct ftl_reloc_move *mv = rq->owner.priv;

	/* Decrease QD of the request */
	assert(rq->iter.qd > 0);
	rq->iter.qd--;

	if (rq->iter.idx != rq->num_blocks || rq->iter.qd) {
		return;
	}

	move_set_state(mv, FTL_RELOC_STATE_PIN);
}

static void
move_rq_pad(struct ftl_rq *rq, struct ftl_band *band)
{
	struct ftl_rq_entry *entry = &rq->entries[rq->iter.idx];

	for (; rq->iter.idx < rq->num_blocks; ++rq->iter.idx) {
		entry->addr = rq->io.addr;
		entry->owner.priv = band;
		entry->lba = FTL_LBA_INVALID;
		entry->seq_id = 0;
		entry++;
		rq->io.addr = ftl_band_next_addr(band, rq->io.addr, 1);
		band->owner.cnt++;
	}

	assert(rq->iter.idx == rq->num_blocks);
}

static void
move_read(struct ftl_reloc *reloc, struct ftl_reloc_move *mv, struct ftl_band *band)
{
	struct ftl_rq *rq = mv->rq;
	uint64_t blocks = ftl_get_num_blocks_in_band(band->dev);
	uint64_t pos = band->md->iter.offset;
	uint64_t begin = ftl_bitmap_find_first_set(band->p2l_map.valid, pos, UINT64_MAX);
	uint64_t end, band_left, rq_left;

	if (spdk_likely(begin < blocks)) {
		if (begin > pos) {
			ftl_band_iter_advance(band, begin - pos);
		} else if (begin == pos) {
			/* Valid block at the position of iterator */
		} else {
			/* Inconsistent state */
			ftl_abort();
		}
	} else if (UINT64_MAX == begin) {
		/* No more valid LBAs in the band */
		band_left = ftl_band_user_blocks_left(band, pos);
		ftl_band_iter_advance(band, band_left);

		assert(ftl_band_filled(band, band->md->iter.offset));

		if (rq->iter.idx) {
			move_rq_pad(rq, band);
			move_set_state(mv, FTL_RELOC_STATE_WAIT);
			rq->iter.qd++;
			rq->owner.cb(rq);
		}

		return;
	} else {
		/* Inconsistent state */
		ftl_abort();
	}

	rq_left = rq->num_blocks - rq->iter.idx;
	assert(rq_left > 0);

	/* Find next clear bit, but no further than max request count */
	end = ftl_bitmap_find_first_clear(band->p2l_map.valid, begin + 1, begin + rq_left);
	if (end != UINT64_MAX) {
		rq_left = end - begin;
	}

	band_left = ftl_band_user_blocks_left(band, band->md->iter.offset);
	rq->iter.count = spdk_min(rq_left, band_left);

	ftl_band_rq_read(band, rq);

	move_advance_rq(rq);

	/* Advance band iterator */
	ftl_band_iter_advance(band, rq->iter.count);

	/* If band is fully written pad rest of request */
	if (ftl_band_filled(band, band->md->iter.offset)) {
		move_rq_pad(rq, band);
	}

	if (rq->iter.idx == rq->num_blocks) {
		/*
		 * All request entries scheduled for reading,
		 * We can change state to waiting
		 */
		move_set_state(mv, FTL_RELOC_STATE_WAIT);
	}
}

static void
move_pin_cb(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_reloc_move *mv = pin_ctx->cb_ctx;
	struct ftl_rq *rq = mv->rq;

	if (status) {
		rq->iter.status = status;
		pin_ctx->lba = FTL_LBA_INVALID;
	}

	if (--rq->iter.remaining == 0) {
		if (rq->iter.status) {
			/* unpin and try again */
			ftl_rq_unpin(rq);
			move_set_state(mv, FTL_RELOC_STATE_PIN);
			return;
		}

		move_set_state(mv, FTL_RELOC_STATE_WRITE);
	}
}

static void
move_pin(struct ftl_reloc_move *mv)
{
	struct ftl_rq *rq = mv->rq;
	struct ftl_rq_entry *entry = rq->entries;
	uint64_t i;

	move_set_state(mv, FTL_RELOC_STATE_WAIT);

	rq->iter.remaining = rq->iter.count = rq->num_blocks;
	rq->iter.status = 0;

	for (i = 0; i < rq->num_blocks; i++) {
		if (entry->lba != FTL_LBA_INVALID) {
			ftl_l2p_pin(rq->dev, entry->lba, 1, move_pin_cb, mv, &entry->l2p_pin_ctx);
		} else {
			ftl_l2p_pin_skip(rq->dev, move_pin_cb, mv, &entry->l2p_pin_ctx);
		}
		entry++;
	}
}

static void
move_finish_write(struct ftl_rq *rq)
{
	uint64_t i;
	struct spdk_ftl_dev *dev = rq->dev;
	struct ftl_rq_entry *iter = rq->entries;
	ftl_addr addr = rq->io.addr;
	struct ftl_band *rq_band = rq->io.band;
	struct ftl_band *band;

	for (i = 0; i < rq->num_blocks; ++i, ++iter) {
		band = iter->owner.priv;

		if (band) {
			assert(band->owner.cnt > 0);
			band->owner.cnt--;
		}
		if (iter->lba != FTL_LBA_INVALID) {
			/* Update L2P table */
			ftl_l2p_update_base(dev, iter->lba, addr, iter->addr);
			ftl_l2p_unpin(dev, iter->lba, 1);
		}
		addr = ftl_band_next_addr(rq_band, addr, 1);
	}
}

static void
move_write_cb(struct ftl_rq *rq)
{
	struct ftl_reloc_move *mv = rq->owner.priv;

	assert(rq->iter.qd == 1);
	rq->iter.qd--;

	if (spdk_likely(rq->success)) {
		move_finish_write(rq);
		move_set_state(mv, FTL_RELOC_STATE_READ);
	} else {
		/* Write failed, repeat write */
		move_set_state(mv, FTL_RELOC_STATE_WRITE);
	}
}

static void
move_write(struct ftl_reloc *reloc, struct ftl_reloc_move *mv)
{
	struct spdk_ftl_dev *dev = mv->dev;
	struct ftl_rq *rq = mv->rq;

	assert(rq->iter.idx == rq->num_blocks);

	/* Request contains data to be placed on a new location, submit it */
	ftl_writer_queue_rq(&dev->writer_gc, rq);
	rq->iter.qd++;

	move_set_state(mv, FTL_RELOC_STATE_WAIT);
}

static void
move_run(struct ftl_reloc *reloc, struct ftl_reloc_move *mv)
{
	struct ftl_band *band;

	switch (mv->state) {
	case FTL_RELOC_STATE_READ: {
		if (spdk_unlikely(reloc->halt)) {
			move_set_state(mv, FTL_RELOC_STATE_HALT);
			break;
		}

		band = move_get_band(reloc);
		if (!band) {
			break;
		}

		move_read(reloc, mv, band);
	}
	break;

	case FTL_RELOC_STATE_PIN:
		move_pin(mv);
		ftl_add_io_activity(reloc->dev);
		break;

	case FTL_RELOC_STATE_WRITE:
		if (spdk_unlikely(reloc->halt)) {
			ftl_rq_unpin(mv->rq);
			move_set_state(mv, FTL_RELOC_STATE_HALT);
			break;
		}

		ftl_add_io_activity(reloc->dev);
		move_write(reloc, mv);
		break;

	case FTL_RELOC_STATE_HALT:
	case FTL_RELOC_STATE_WAIT:
		break;

	default:
		assert(0);
		ftl_abort();
		break;
	}
}

static void
move_handle_band_error(struct ftl_band *band)
{
	struct ftl_reloc *reloc = band->dev->reloc;
	/*
	 * Handle band error, it's because an error occurred during reading,
	 * Add band to the close band list, will try reloc it in a moment
	 */
	TAILQ_REMOVE(&reloc->band_done, band, queue_entry);
	reloc->band_done_count--;

	band->md->state = FTL_BAND_STATE_CLOSING;
	ftl_band_set_state(band, FTL_BAND_STATE_CLOSED);
}

static void
move_release_bands(struct ftl_reloc *reloc)
{
	struct ftl_band *band;

	if (TAILQ_EMPTY(&reloc->band_done)) {
		return;
	}

	band = TAILQ_FIRST(&reloc->band_done);

	if (band->owner.cnt || ftl_band_qd(band)) {
		/* Band still in use */
		return;
	}

	if (ftl_band_empty(band)) {
		assert(ftl_band_filled(band, band->md->iter.offset));
		TAILQ_REMOVE(&reloc->band_done, band, queue_entry);
		reloc->band_done_count--;
		ftl_band_free(band);
	} else {
		move_handle_band_error(band);
	}
}

bool
ftl_reloc_is_halted(const struct ftl_reloc *reloc)
{
	size_t i, count;

	count = SPDK_COUNTOF(reloc->move_queue);
	for (i = 0; i < count; ++i) {
		if (i == FTL_RELOC_STATE_HALT) {
			continue;
		}

		if (!TAILQ_EMPTY(&reloc->move_queue[i])) {
			return false;
		}
	}

	return true;
}

void
ftl_reloc(struct ftl_reloc *reloc)
{
	size_t i, count;

	count = SPDK_COUNTOF(reloc->move_queue);
	for (i = 0; i < count; ++i) {
		if (TAILQ_EMPTY(&reloc->move_queue[i])) {
			continue;
		}

		move_run(reloc, TAILQ_FIRST(&reloc->move_queue[i]));
	}

	move_release_bands(reloc);
}
