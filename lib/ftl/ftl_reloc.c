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

#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/ftl.h"

#include "ftl_reloc.h"
#include "ftl_core.h"
#include "ftl_io.h"
#include "ftl_band.h"
#include "ftl_debug.h"

/* Maximum active reloc moves */
#define FTL_RELOC_MAX_MOVES 256

struct ftl_reloc;
struct ftl_band_reloc;

enum ftl_reloc_move_state {
	FTL_RELOC_STATE_READ_LBA_MAP,
	FTL_RELOC_STATE_READ,
	FTL_RELOC_STATE_WRITE,
};

enum ftl_band_reloc_state {
	FTL_BAND_RELOC_STATE_INACTIVE,
	FTL_BAND_RELOC_STATE_PENDING,
	FTL_BAND_RELOC_STATE_ACTIVE,
	FTL_BAND_RELOC_STATE_HIGH_PRIO
};

struct ftl_reloc_move {
	struct ftl_band_reloc			*breloc;

	/* Start addr */
	struct ftl_addr				addr;

	/* Number of logical blocks */
	size_t					num_blocks;

	/* Data buffer */
	void					*data;

	/* Move state (read lba_map, read, write) */
	enum ftl_reloc_move_state		state;

	/* IO associated with move */
	struct ftl_io				*io;

	STAILQ_ENTRY(ftl_reloc_move)		entry;
};

struct ftl_band_reloc {
	struct ftl_reloc			*parent;

	/* Band being relocated */
	struct ftl_band				*band;

	/* Number of logical blocks to be relocated */
	size_t					num_blocks;

	/* Bitmap of logical blocks to be relocated */
	struct spdk_bit_array			*reloc_map;

	/*  State of the band reloc */
	enum ftl_band_reloc_state		state;

	/* The band is being defragged */
	bool					defrag;

	/* Reloc map iterator */
	struct {
		/* Array of zone offsets */
		size_t				*zone_offset;

		/* Current zone */
		size_t				zone_current;
	} iter;

	/* Number of outstanding moves */
	size_t					num_outstanding;

	/* Pool of move objects */
	struct ftl_reloc_move			*moves;

	/* Move queue */
	STAILQ_HEAD(, ftl_reloc_move)		move_queue;

	TAILQ_ENTRY(ftl_band_reloc)		entry;
};

struct ftl_reloc {
	/* Device associated with relocate */
	struct spdk_ftl_dev			*dev;

	/* Indicates relocate is about to halt */
	bool					halt;

	/* Maximum number of IOs per band */
	size_t					max_qdepth;

	/* Maximum number of active band relocates */
	size_t					max_active;

	/* Maximum transfer size (in logical blocks) per single IO */
	size_t					xfer_size;
	/* Number of bands being defragged */
	size_t					num_defrag_bands;

	/* Array of band relocates */
	struct ftl_band_reloc			*brelocs;

	/* Number of active/priority band relocates */
	size_t					num_active;

	/* Priority band relocates queue */
	TAILQ_HEAD(, ftl_band_reloc)		prio_queue;

	/* Active band relocates queue */
	TAILQ_HEAD(, ftl_band_reloc)		active_queue;

	/* Pending band relocates queue */
	TAILQ_HEAD(, ftl_band_reloc)		pending_queue;
};

bool
ftl_reloc_is_defrag_active(const struct ftl_reloc *reloc)
{
	return reloc->num_defrag_bands > 0;
}

static size_t
ftl_reloc_iter_zone_offset(struct ftl_band_reloc *breloc)
{
	size_t zone = breloc->iter.zone_current;

	return breloc->iter.zone_offset[zone];
}

static size_t
ftl_reloc_iter_zone_done(struct ftl_band_reloc *breloc)
{
	size_t num_blocks = ftl_get_num_blocks_in_zone(breloc->parent->dev);

	return ftl_reloc_iter_zone_offset(breloc) == num_blocks;
}

static void
ftl_reloc_clr_block(struct ftl_band_reloc *breloc, size_t block_off)
{
	if (!spdk_bit_array_get(breloc->reloc_map, block_off)) {
		return;
	}

	spdk_bit_array_clear(breloc->reloc_map, block_off);
	assert(breloc->num_blocks);
	breloc->num_blocks--;
}

static void
ftl_reloc_read_lba_map_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_reloc_move *move = arg;
	struct ftl_band_reloc *breloc = move->breloc;

	breloc->num_outstanding--;
	assert(status == 0);
	move->state = FTL_RELOC_STATE_WRITE;
	STAILQ_INSERT_TAIL(&breloc->move_queue, move, entry);
}

static int
ftl_reloc_read_lba_map(struct ftl_band_reloc *breloc, struct ftl_reloc_move *move)
{
	struct ftl_band *band = breloc->band;

	breloc->num_outstanding++;
	return ftl_band_read_lba_map(band, ftl_band_block_offset_from_addr(band, move->addr),
				     move->num_blocks, ftl_reloc_read_lba_map_cb, move);
}

static void
ftl_reloc_prep(struct ftl_band_reloc *breloc)
{
	struct ftl_band *band = breloc->band;
	struct ftl_reloc *reloc = breloc->parent;
	struct ftl_reloc_move *move;
	size_t i;

	reloc->num_active++;

	if (!band->high_prio) {
		if (ftl_band_alloc_lba_map(band)) {
			SPDK_ERRLOG("Failed to allocate lba map\n");
			assert(false);
		}
	} else {
		ftl_band_acquire_lba_map(band);
	}

	for (i = 0; i < reloc->max_qdepth; ++i) {
		move = &breloc->moves[i];
		move->state = FTL_RELOC_STATE_READ;
		STAILQ_INSERT_TAIL(&breloc->move_queue, move, entry);
	}
}

static void
ftl_reloc_free_move(struct ftl_band_reloc *breloc, struct ftl_reloc_move *move)
{
	assert(move);
	spdk_dma_free(move->data);
	memset(move, 0, sizeof(*move));
	move->state = FTL_RELOC_STATE_READ;
}

static void
ftl_reloc_write_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_reloc_move *move = arg;
	struct ftl_addr addr = move->addr;
	struct ftl_band_reloc *breloc = move->breloc;
	size_t i;

	breloc->num_outstanding--;

	if (status) {
		SPDK_ERRLOG("Reloc write failed with status: %d\n", status);
		assert(false);
		return;
	}

	for (i = 0; i < move->num_blocks; ++i) {
		addr.offset = move->addr.offset + i;
		size_t block_off = ftl_band_block_offset_from_addr(breloc->band, addr);
		ftl_reloc_clr_block(breloc, block_off);
	}

	ftl_reloc_free_move(breloc, move);
	STAILQ_INSERT_TAIL(&breloc->move_queue, move, entry);
}

static void
ftl_reloc_read_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_reloc_move *move = arg;
	struct ftl_band_reloc *breloc = move->breloc;

	breloc->num_outstanding--;

	/* TODO: We should handle fail on relocation read. We need to inform */
	/* user that this group of blocks is bad (update l2p with bad block address and */
	/* put it to lba_map/sector_lba). Maybe we could also retry read with smaller granularity? */
	if (status) {
		SPDK_ERRLOG("Reloc read failed with status: %d\n", status);
		assert(false);
		return;
	}

	move->state = FTL_RELOC_STATE_READ_LBA_MAP;
	move->io = NULL;
	STAILQ_INSERT_TAIL(&breloc->move_queue, move, entry);
}

static void
ftl_reloc_iter_reset(struct ftl_band_reloc *breloc)
{
	memset(breloc->iter.zone_offset, 0, ftl_get_num_punits(breloc->band->dev) *
	       sizeof(*breloc->iter.zone_offset));
	breloc->iter.zone_current = 0;
}

static size_t
ftl_reloc_iter_block_offset(struct ftl_band_reloc *breloc)
{
	size_t zone_offset = breloc->iter.zone_current * ftl_get_num_blocks_in_zone(breloc->parent->dev);

	return breloc->iter.zone_offset[breloc->iter.zone_current] + zone_offset;
}

static void
ftl_reloc_iter_next_zone(struct ftl_band_reloc *breloc)
{
	size_t num_zones = ftl_get_num_punits(breloc->band->dev);

	breloc->iter.zone_current = (breloc->iter.zone_current + 1) % num_zones;
}

static int
ftl_reloc_block_valid(struct ftl_band_reloc *breloc, size_t block_off)
{
	struct ftl_addr addr = ftl_band_addr_from_block_offset(breloc->band, block_off);

	return ftl_addr_is_written(breloc->band, addr) &&
	       spdk_bit_array_get(breloc->reloc_map, block_off) &&
	       ftl_band_block_offset_valid(breloc->band, block_off);
}

static int
ftl_reloc_iter_next(struct ftl_band_reloc *breloc, size_t *block_off)
{
	size_t zone = breloc->iter.zone_current;

	*block_off = ftl_reloc_iter_block_offset(breloc);

	if (ftl_reloc_iter_zone_done(breloc)) {
		return 0;
	}

	breloc->iter.zone_offset[zone]++;

	if (!ftl_reloc_block_valid(breloc, *block_off)) {
		ftl_reloc_clr_block(breloc, *block_off);
		return 0;
	}

	return 1;
}

static int
ftl_reloc_first_valid_block(struct ftl_band_reloc *breloc, size_t *block_off)
{
	size_t i, num_blocks = ftl_get_num_blocks_in_zone(breloc->parent->dev);

	for (i = ftl_reloc_iter_zone_offset(breloc); i < num_blocks; ++i) {
		if (ftl_reloc_iter_next(breloc, block_off)) {
			return 1;
		}
	}

	return 0;
}

static int
ftl_reloc_iter_done(struct ftl_band_reloc *breloc)
{
	size_t i;
	size_t num_zones = ftl_get_num_punits(breloc->band->dev);
	size_t num_blocks = ftl_get_num_blocks_in_zone(breloc->parent->dev);

	for (i = 0; i < num_zones; ++i) {
		if (breloc->iter.zone_offset[i] != num_blocks) {
			return 0;
		}
	}

	return 1;
}

static size_t
ftl_reloc_find_valid_blocks(struct ftl_band_reloc *breloc,
			    size_t _num_blocks, struct ftl_addr *addr)
{
	size_t block_off, num_blocks = 0;

	if (!ftl_reloc_first_valid_block(breloc, &block_off)) {
		return 0;
	}

	*addr = ftl_band_addr_from_block_offset(breloc->band, block_off);

	for (num_blocks = 1; num_blocks < _num_blocks; num_blocks++) {
		if (!ftl_reloc_iter_next(breloc, &block_off)) {
			break;
		}
	}

	return num_blocks;
}

static size_t
ftl_reloc_next_blocks(struct ftl_band_reloc *breloc, struct ftl_addr *addr)
{
	size_t i, num_blocks = 0;
	struct spdk_ftl_dev *dev = breloc->parent->dev;

	for (i = 0; i < ftl_get_num_punits(dev); ++i) {
		num_blocks = ftl_reloc_find_valid_blocks(breloc, breloc->parent->xfer_size, addr);
		ftl_reloc_iter_next_zone(breloc);

		if (num_blocks || ftl_reloc_iter_done(breloc)) {
			break;
		}
	}

	return num_blocks;
}

static struct ftl_io *
ftl_reloc_io_init(struct ftl_band_reloc *breloc, struct ftl_reloc_move *move,
		  ftl_io_fn fn, enum ftl_io_type io_type, int flags)
{
	size_t block_off, i;
	struct ftl_addr addr = move->addr;
	struct ftl_io *io = NULL;
	struct ftl_io_init_opts opts = {
		.dev		= breloc->parent->dev,
		.band		= breloc->band,
		.size		= sizeof(*io),
		.flags		= flags | FTL_IO_INTERNAL | FTL_IO_PHYSICAL_MODE,
		.type		= io_type,
		.num_blocks	= move->num_blocks,
		.iovs		= {
			{
				.iov_base = move->data,
				.iov_len = move->num_blocks * FTL_BLOCK_SIZE,
			}
		},
		.iovcnt		= 1,
		.cb_fn		= fn,
	};

	io = ftl_io_init_internal(&opts);
	if (!io) {
		return NULL;
	}

	io->cb_ctx = move;
	io->addr = move->addr;

	if (flags & FTL_IO_VECTOR_LBA) {
		for (i = 0; i < io->num_blocks; ++i, ++addr.offset) {
			block_off = ftl_band_block_offset_from_addr(breloc->band, addr);

			if (!ftl_band_block_offset_valid(breloc->band, block_off)) {
				io->lba.vector[i] = FTL_LBA_INVALID;
				continue;
			}

			io->lba.vector[i] = breloc->band->lba_map.map[block_off];
		}
	}

	ftl_trace_lba_io_init(io->dev, io);

	return io;
}

static int
ftl_reloc_write(struct ftl_band_reloc *breloc, struct ftl_reloc_move *move)
{
	int io_flags =  FTL_IO_WEAK | FTL_IO_VECTOR_LBA | FTL_IO_BYPASS_CACHE;

	if (spdk_likely(!move->io)) {
		move->io = ftl_reloc_io_init(breloc, move, ftl_reloc_write_cb,
					     FTL_IO_WRITE, io_flags);
		if (!move->io) {
			ftl_reloc_free_move(breloc, move);
			STAILQ_INSERT_TAIL(&breloc->move_queue, move, entry);
			return -ENOMEM;
		}
	}

	breloc->num_outstanding++;
	ftl_io_write(move->io);
	return 0;
}

static int
ftl_reloc_read(struct ftl_band_reloc *breloc, struct ftl_reloc_move *move)
{
	struct ftl_addr addr = {};

	move->num_blocks = ftl_reloc_next_blocks(breloc, &addr);
	move->breloc = breloc;
	move->addr = addr;

	if (!move->num_blocks) {
		return 0;
	}

	move->data = spdk_dma_malloc(FTL_BLOCK_SIZE * move->num_blocks, 4096, NULL);
	if (!move->data) {
		return -1;
	}

	move->io = ftl_reloc_io_init(breloc, move, ftl_reloc_read_cb, FTL_IO_READ, 0);
	if (!move->io) {
		ftl_reloc_free_move(breloc, move);
		STAILQ_INSERT_TAIL(&breloc->move_queue, move, entry);
		SPDK_ERRLOG("Failed to initialize io for relocation.");
		return -1;
	}

	breloc->num_outstanding++;
	ftl_io_read(move->io);
	return 0;
}

static void
ftl_reloc_process_moves(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc_move *move;
	STAILQ_HEAD(, ftl_reloc_move) move_queue;
	int rc = 0;

	/*
	 * When IO allocation fails, we do not want to retry immediately so keep moves on
	 * temporary queue
	 */
	STAILQ_INIT(&move_queue);
	STAILQ_SWAP(&breloc->move_queue, &move_queue, ftl_reloc_move);

	while (!STAILQ_EMPTY(&move_queue)) {
		move = STAILQ_FIRST(&move_queue);
		STAILQ_REMOVE_HEAD(&move_queue, entry);

		switch (move->state) {
		case FTL_RELOC_STATE_READ_LBA_MAP:
			rc = ftl_reloc_read_lba_map(breloc, move);
			break;
		case FTL_RELOC_STATE_READ:
			rc = ftl_reloc_read(breloc, move);
			break;
		case FTL_RELOC_STATE_WRITE:
			rc = ftl_reloc_write(breloc, move);
			break;
		default:
			assert(false);
			break;
		}

		if (rc) {
			SPDK_ERRLOG("Move queue processing failed\n");
			assert(false);
		}
	}
}

static bool
ftl_reloc_done(struct ftl_band_reloc *breloc)
{
	return !breloc->num_outstanding && STAILQ_EMPTY(&breloc->move_queue);
}

static void
ftl_reloc_release(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc *reloc = breloc->parent;
	struct ftl_band *band = breloc->band;

	ftl_reloc_iter_reset(breloc);
	ftl_band_release_lba_map(band);
	reloc->num_active--;

	if (breloc->state == FTL_BAND_RELOC_STATE_HIGH_PRIO) {
		/* High prio band must be relocated as a whole and ANM events will be ignored */
		assert(breloc->num_blocks == 0 && ftl_band_empty(band));
		TAILQ_REMOVE(&reloc->prio_queue, breloc, entry);
		band->high_prio = 0;
		breloc->state = FTL_BAND_RELOC_STATE_INACTIVE;
	} else {
		assert(breloc->state == FTL_BAND_RELOC_STATE_ACTIVE);
		TAILQ_REMOVE(&reloc->active_queue, breloc, entry);
		breloc->state = FTL_BAND_RELOC_STATE_INACTIVE;

		/* If we got ANM event during relocation put such band back to pending queue */
		if (breloc->num_blocks != 0) {
			breloc->state = FTL_BAND_RELOC_STATE_PENDING;
			TAILQ_INSERT_TAIL(&reloc->pending_queue, breloc, entry);
			return;
		}
	}

	if (ftl_band_empty(band) && band->state == FTL_BAND_STATE_CLOSED) {
		ftl_band_set_state(breloc->band, FTL_BAND_STATE_FREE);

		if (breloc->defrag) {
			breloc->defrag = false;
			assert(reloc->num_defrag_bands > 0);
			reloc->num_defrag_bands--;
		}
	}
}

static void
ftl_process_reloc(struct ftl_band_reloc *breloc)
{
	ftl_reloc_process_moves(breloc);

	if (ftl_reloc_done(breloc)) {
		ftl_reloc_release(breloc);
	}
}

static int
ftl_band_reloc_init(struct ftl_reloc *reloc, struct ftl_band_reloc *breloc,
		    struct ftl_band *band)
{
	breloc->band = band;
	breloc->parent = reloc;

	breloc->reloc_map = spdk_bit_array_create(ftl_get_num_blocks_in_band(reloc->dev));
	if (!breloc->reloc_map) {
		SPDK_ERRLOG("Failed to initialize reloc map");
		return -1;
	}

	breloc->iter.zone_offset = calloc(ftl_get_num_punits(band->dev),
					  sizeof(*breloc->iter.zone_offset));
	if (!breloc->iter.zone_offset) {
		SPDK_ERRLOG("Failed to initialize reloc iterator");
		return -1;
	}

	STAILQ_INIT(&breloc->move_queue);

	breloc->moves = calloc(reloc->max_qdepth, sizeof(*breloc->moves));
	if (!breloc->moves) {
		return -1;
	}

	return 0;
}

static void
ftl_band_reloc_free(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc_move *move;

	if (!breloc) {
		return;
	}

	assert(breloc->num_outstanding == 0);

	/* Drain write queue if there is active band relocation during shutdown */
	if (breloc->state == FTL_BAND_RELOC_STATE_ACTIVE ||
	    breloc->state == FTL_BAND_RELOC_STATE_HIGH_PRIO) {
		assert(breloc->parent->halt);
		STAILQ_FOREACH(move, &breloc->move_queue, entry) {
			ftl_reloc_free_move(breloc, move);
		}
	}

	spdk_bit_array_free(&breloc->reloc_map);
	free(breloc->iter.zone_offset);
	free(breloc->moves);
}

struct ftl_reloc *
ftl_reloc_init(struct spdk_ftl_dev *dev)
{
	struct ftl_reloc *reloc;
	size_t i;

	reloc = calloc(1, sizeof(*reloc));
	if (!reloc) {
		return NULL;
	}

	reloc->dev = dev;
	reloc->halt = true;
	reloc->max_qdepth = dev->conf.max_reloc_qdepth;
	reloc->max_active = dev->conf.max_active_relocs;
	reloc->xfer_size = dev->xfer_size;
	reloc->num_defrag_bands = 0;

	if (reloc->max_qdepth > FTL_RELOC_MAX_MOVES) {
		goto error;
	}

	reloc->brelocs = calloc(ftl_get_num_bands(dev), sizeof(*reloc->brelocs));
	if (!reloc->brelocs) {
		goto error;
	}

	for (i = 0; i < ftl_get_num_bands(reloc->dev); ++i) {
		if (ftl_band_reloc_init(reloc, &reloc->brelocs[i], &dev->bands[i])) {
			goto error;
		}
	}

	TAILQ_INIT(&reloc->pending_queue);
	TAILQ_INIT(&reloc->active_queue);
	TAILQ_INIT(&reloc->prio_queue);

	return reloc;
error:
	ftl_reloc_free(reloc);
	return NULL;
}

void
ftl_reloc_free(struct ftl_reloc *reloc)
{
	size_t i;

	if (!reloc) {
		return;
	}

	for (i = 0; i < ftl_get_num_bands(reloc->dev); ++i) {
		ftl_band_reloc_free(&reloc->brelocs[i]);
	}

	free(reloc->brelocs);
	free(reloc);
}

bool
ftl_reloc_is_halted(const struct ftl_reloc *reloc)
{
	return reloc->halt;
}

void
ftl_reloc_halt(struct ftl_reloc *reloc)
{
	reloc->halt = true;
}

void
ftl_reloc_resume(struct ftl_reloc *reloc)
{
	reloc->halt = false;
}

bool
ftl_reloc(struct ftl_reloc *reloc)
{
	struct ftl_band_reloc *breloc, *tbreloc;

	if (ftl_reloc_is_halted(reloc)) {
		return false;
	}

	/* Process first band from priority queue and return */
	breloc = TAILQ_FIRST(&reloc->prio_queue);
	if (breloc) {
		ftl_process_reloc(breloc);
		return true;
	}

	TAILQ_FOREACH_SAFE(breloc, &reloc->pending_queue, entry, tbreloc) {
		if (reloc->num_active == reloc->max_active) {
			break;
		}

		/* Wait for band to close before relocating */
		if (breloc->band->state != FTL_BAND_STATE_CLOSED) {
			continue;
		}

		ftl_reloc_prep(breloc);
		assert(breloc->state == FTL_BAND_RELOC_STATE_PENDING);
		TAILQ_REMOVE(&reloc->pending_queue, breloc, entry);
		breloc->state = FTL_BAND_RELOC_STATE_ACTIVE;
		TAILQ_INSERT_HEAD(&reloc->active_queue, breloc, entry);
	}

	TAILQ_FOREACH_SAFE(breloc, &reloc->active_queue, entry, tbreloc) {
		assert(breloc->state == FTL_BAND_RELOC_STATE_ACTIVE);
		ftl_process_reloc(breloc);
	}

	return reloc->num_active != 0;
}

void
ftl_reloc_add(struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
	      size_t num_blocks, int prio, bool is_defrag)
{
	struct ftl_band_reloc *breloc = &reloc->brelocs[band->id];
	size_t i;

	/* No need to add anything if already at high prio - whole band should be relocated */
	if (!prio && band->high_prio) {
		return;
	}

	pthread_spin_lock(&band->lba_map.lock);
	if (band->lba_map.num_vld == 0) {
		pthread_spin_unlock(&band->lba_map.lock);

		/* If the band is closed and has no valid blocks, free it */
		if (band->state == FTL_BAND_STATE_CLOSED) {
			ftl_band_set_state(band, FTL_BAND_STATE_FREE);
		}

		return;
	}
	pthread_spin_unlock(&band->lba_map.lock);

	for (i = offset; i < offset + num_blocks; ++i) {
		if (spdk_bit_array_get(breloc->reloc_map, i)) {
			continue;
		}
		spdk_bit_array_set(breloc->reloc_map, i);
		breloc->num_blocks++;
	}

	/* If the band is coming from the defrag process, mark it appropriately */
	if (is_defrag) {
		assert(offset == 0 && num_blocks == ftl_get_num_blocks_in_band(band->dev));
		reloc->num_defrag_bands++;
		breloc->defrag = true;
	}

	if (!prio) {
		if (breloc->state == FTL_BAND_RELOC_STATE_INACTIVE) {
			breloc->state = FTL_BAND_RELOC_STATE_PENDING;
			TAILQ_INSERT_HEAD(&reloc->pending_queue, breloc, entry);
		}
	} else {
		bool active = false;
		/* If priority band is already on pending or active queue, remove it from it */
		switch (breloc->state) {
		case FTL_BAND_RELOC_STATE_PENDING:
			TAILQ_REMOVE(&reloc->pending_queue, breloc, entry);
			break;
		case FTL_BAND_RELOC_STATE_ACTIVE:
			active = true;
			TAILQ_REMOVE(&reloc->active_queue, breloc, entry);
			break;
		default:
			break;
		}

		breloc->state = FTL_BAND_RELOC_STATE_HIGH_PRIO;
		TAILQ_INSERT_TAIL(&reloc->prio_queue, breloc, entry);

		/*
		 * If band has been already on active queue it doesn't need any additional
		 * resources
		 */
		if (!active) {
			ftl_reloc_prep(breloc);
		}
	}
}
