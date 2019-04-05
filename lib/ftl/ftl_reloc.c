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
#include "spdk_internal/log.h"
#include "spdk/ftl.h"
#include "ftl_reloc.h"
#include "ftl_core.h"
#include "ftl_io.h"
#include "ftl_rwb.h"
#include "ftl_band.h"
#include "ftl_debug.h"

struct ftl_reloc;
struct ftl_band_reloc;

typedef int (*ftl_reloc_fn)(struct ftl_band_reloc *, struct ftl_io *);

struct ftl_band_reloc {
	struct ftl_reloc			*parent;

	/* Band being relocated */
	struct ftl_band				*band;

	/* Number of logical blocks to be relocated */
	size_t					num_lbks;

	/* Bitmap of logical blocks to be relocated */
	struct spdk_bit_array			*reloc_map;

	/* Indicates band being acitvely processed */
	int					active;

	/* Reloc map iterator */
	struct {
		/* Array of chunk offsets */
		size_t				*chk_offset;

		/* Currently chunk */
		size_t				chk_current;
	} iter;

	/* Free IO queue */
	struct spdk_ring			*free_queue;

	/* Queue of IO ready to be written */
	struct spdk_ring			*write_queue;

	TAILQ_ENTRY(ftl_band_reloc)		entry;
};

struct ftl_reloc {
	/* Device associated with relocate */
	struct spdk_ftl_dev			*dev;

	/* Indicates relocate is about to halt */
	bool					halt;

	/* Maximum number of IOs per band */
	size_t					max_qdepth;

	/* IO buffer */
	struct ftl_io				**io;

	/* Maximum number of active band relocates */
	size_t					max_active;

	/* Maximum transfer size (in logical blocks) per single IO */
	size_t					xfer_size;

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

static struct ftl_band_reloc *
ftl_io_get_band_reloc(struct ftl_io *io)
{
	return &io->dev->reloc->brelocs[io->band->id];
}

static size_t
ftl_reloc_iter_chk_offset(struct ftl_band_reloc *breloc)
{
	size_t chunk = breloc->iter.chk_current;

	return breloc->iter.chk_offset[chunk];
}

static size_t
ftl_reloc_iter_chk_done(struct ftl_band_reloc *breloc)
{
	size_t num_lbks = ftl_dev_lbks_in_chunk(breloc->parent->dev);

	return ftl_reloc_iter_chk_offset(breloc) == num_lbks;
}

static void
ftl_reloc_clr_lbk(struct ftl_band_reloc *breloc, size_t lbkoff)
{
	if (!spdk_bit_array_get(breloc->reloc_map, lbkoff)) {
		return;
	}

	spdk_bit_array_clear(breloc->reloc_map, lbkoff);
	assert(breloc->num_lbks);
	breloc->num_lbks--;
}

static void
_ftl_reloc_prep(struct ftl_band_reloc *breloc)
{
	struct ftl_io *io;
	struct ftl_reloc *reloc = breloc->parent;
	struct spdk_ftl_dev *dev = reloc->dev;
	size_t i;

	for (i = 0; i < reloc->max_qdepth; ++i) {
		io = ftl_io_alloc(dev->ioch);
		spdk_ring_enqueue(breloc->free_queue, (void **)&io, 1);
	}
}

static void
ftl_reloc_read_lba_map_cb(void *arg, int status)
{
	struct ftl_io *io = arg;
	struct ftl_band_reloc *breloc = ftl_io_get_band_reloc(io);

	assert(status == 0);
	ftl_io_free(io);
	_ftl_reloc_prep(breloc);
}

static int
ftl_reloc_read_lba_map(struct ftl_band_reloc *breloc)
{
	struct ftl_band *band = breloc->band;
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_io *io = ftl_io_alloc(dev->ioch);

	io->dev = dev;
	io->band = band;
	io->cb.ctx = io;
	io->cb.fn = ftl_reloc_read_lba_map_cb;

	if (ftl_band_alloc_md(band)) {
		assert(false);
	}

	return ftl_band_read_lba_map(band, &band->md, band->md.dma_buf, &io->cb);
}

static void
ftl_reloc_prep(struct ftl_band_reloc *breloc)
{
	struct ftl_band *band = breloc->band;
	struct ftl_reloc *reloc = breloc->parent;

	breloc->active = 1;
	reloc->num_active++;

	if (!band->high_prio) {
		assert(band->md.lba_map == NULL);
		ftl_reloc_read_lba_map(breloc);
		return;
	}

	_ftl_reloc_prep(breloc);
}

static void
ftl_reloc_free_io(struct ftl_band_reloc *breloc, struct ftl_io *io)
{
	spdk_dma_free(io->iov.single.iov_base);
	free(io->lba.vector);
	spdk_ring_enqueue(breloc->free_queue, (void **)&io, 1);
}

static void
ftl_reloc_write_cb(void *arg, int status)
{
	struct ftl_io *io = arg;
	struct ftl_ppa ppa = io->ppa;
	struct ftl_band_reloc *breloc = ftl_io_get_band_reloc(io);
	size_t i;

	if (status) {
		SPDK_ERRLOG("Reloc write failed with status: %d\n", status);
		assert(false);
		return;
	}

	for (i = 0; i < io->lbk_cnt; ++i) {
		ppa.lbk = io->ppa.lbk + i;
		size_t lbkoff = ftl_band_lbkoff_from_ppa(breloc->band, ppa);
		ftl_reloc_clr_lbk(breloc, lbkoff);
	}

	ftl_reloc_free_io(breloc, io);
}

static void
ftl_reloc_read_cb(void *arg, int status)
{
	struct ftl_io *io = arg;
	struct ftl_band_reloc *breloc = ftl_io_get_band_reloc(io);

	/* TODO: We should handle fail on relocation read. We need to inform */
	/* user that this group of blocks is bad (update l2p with bad block address and */
	/* put it to lba_map/sector_lba). Maybe we could also retry read with smaller granularity? */
	if (status) {
		SPDK_ERRLOG("Reloc read failed with status: %d\n", status);
		assert(false);
		return;
	}

	io->flags &= ~FTL_IO_INITIALIZED;
	spdk_ring_enqueue(breloc->write_queue, (void **)&io, 1);
}

static void
ftl_reloc_iter_reset(struct ftl_band_reloc *breloc)
{
	memset(breloc->iter.chk_offset, 0, ftl_dev_num_punits(breloc->band->dev) *
	       sizeof(*breloc->iter.chk_offset));
	breloc->iter.chk_current = 0;
}

static size_t
ftl_reloc_iter_lbkoff(struct ftl_band_reloc *breloc)
{
	size_t chk_offset = breloc->iter.chk_current * ftl_dev_lbks_in_chunk(breloc->parent->dev);

	return breloc->iter.chk_offset[breloc->iter.chk_current] + chk_offset;
}

static void
ftl_reloc_iter_next_chk(struct ftl_band_reloc *breloc)
{
	size_t num_chk = ftl_dev_num_punits(breloc->band->dev);

	breloc->iter.chk_current = (breloc->iter.chk_current + 1) % num_chk;
}

static int
ftl_reloc_lbk_valid(struct ftl_band_reloc *breloc, size_t lbkoff)
{
	return spdk_bit_array_get(breloc->reloc_map, lbkoff) &&
	       ftl_band_lbkoff_valid(breloc->band, lbkoff);
}

static int
ftl_reloc_iter_next(struct ftl_band_reloc *breloc, size_t *lbkoff)
{
	size_t chunk = breloc->iter.chk_current;

	*lbkoff = ftl_reloc_iter_lbkoff(breloc);

	if (ftl_reloc_iter_chk_done(breloc)) {
		return 0;
	}

	breloc->iter.chk_offset[chunk]++;

	if (!ftl_reloc_lbk_valid(breloc, *lbkoff)) {
		ftl_reloc_clr_lbk(breloc, *lbkoff);
		return 0;
	}

	return 1;
}

static int
ftl_reloc_first_valid_lbk(struct ftl_band_reloc *breloc, size_t *lbkoff)
{
	size_t i, num_lbks = ftl_dev_lbks_in_chunk(breloc->parent->dev);

	for (i = ftl_reloc_iter_chk_offset(breloc); i < num_lbks; ++i) {
		if (ftl_reloc_iter_next(breloc, lbkoff)) {
			return 1;
		}
	}

	return 0;
}

static int
ftl_reloc_iter_done(struct ftl_band_reloc *breloc)
{
	size_t i;
	size_t num_chks = ftl_dev_num_punits(breloc->band->dev);
	size_t num_lbks = ftl_dev_lbks_in_chunk(breloc->parent->dev);

	for (i = 0; i < num_chks; ++i) {
		if (breloc->iter.chk_offset[i] != num_lbks) {
			return 0;
		}
	}

	return 1;
}

static size_t
ftl_reloc_find_valid_lbks(struct ftl_band_reloc *breloc,
			  size_t num_lbk, struct ftl_ppa *ppa)
{
	size_t lbkoff, lbk_cnt = 0;

	if (!ftl_reloc_first_valid_lbk(breloc, &lbkoff)) {
		return 0;
	}

	*ppa = ftl_band_ppa_from_lbkoff(breloc->band, lbkoff);

	for (lbk_cnt = 1; lbk_cnt < num_lbk; lbk_cnt++) {
		if (!ftl_reloc_iter_next(breloc, &lbkoff)) {
			break;
		}
	}

	return lbk_cnt;
}

static size_t
ftl_reloc_next_lbks(struct ftl_band_reloc *breloc, struct ftl_ppa *ppa)
{
	size_t i, lbk_cnt = 0;
	struct spdk_ftl_dev *dev = breloc->parent->dev;

	for (i = 0; i < ftl_dev_num_punits(dev); ++i) {
		lbk_cnt = ftl_reloc_find_valid_lbks(breloc,
						    breloc->parent->xfer_size, ppa);
		ftl_reloc_iter_next_chk(breloc);

		if (lbk_cnt || ftl_reloc_iter_done(breloc)) {
			break;
		}
	}

	return lbk_cnt;
}

static void
ftl_reloc_io_reinit(struct ftl_io *io, struct ftl_band_reloc *breloc,
		    spdk_ftl_fn fn, enum ftl_io_type io_type, int flags)
{
	size_t i;
	uint64_t lbkoff;
	struct ftl_ppa ppa = io->ppa;

	ftl_io_reinit(io, fn, io, flags | FTL_IO_INTERNAL, io_type);

	io->ppa = ppa;
	io->band = breloc->band;
	io->lba.vector = calloc(io->lbk_cnt, sizeof(uint64_t));

	for (i = 0; i < io->lbk_cnt; ++i) {
		ppa.lbk = io->ppa.lbk + i;
		lbkoff = ftl_band_lbkoff_from_ppa(breloc->band, ppa);

		if (!ftl_band_lbkoff_valid(breloc->band, lbkoff)) {
			io->lba.vector[i] = FTL_LBA_INVALID;
			continue;
		}

		io->lba.vector[i] = breloc->band->md.lba_map[lbkoff];
	}

	ftl_trace_lba_io_init(io->dev, io);
}

static int
ftl_reloc_write(struct ftl_band_reloc *breloc, struct ftl_io *io)
{
	int rc;

	if (!(io->flags & FTL_IO_INITIALIZED)) {
		ftl_reloc_io_reinit(io, breloc, ftl_reloc_write_cb,
				    FTL_IO_WRITE,
				    FTL_IO_KEEP_ALIVE | FTL_IO_WEAK | FTL_IO_VECTOR_LBA);
	}

	rc = ftl_io_write(io);
	if (rc == -EAGAIN) {
		spdk_ring_enqueue(breloc->write_queue, (void **)&io, 1);
		return 0;
	}

	return rc;
}

static int
ftl_reloc_io_init(struct ftl_band_reloc *breloc, struct ftl_io *io,
		  struct ftl_ppa ppa, size_t num_lbks)
{
	struct ftl_io_init_opts opts = {
		.dev		= breloc->parent->dev,
		.io		= io,
		.rwb_batch	= NULL,
		.band		= breloc->band,
		.size		= sizeof(*io),
		.flags		= FTL_IO_KEEP_ALIVE | FTL_IO_INTERNAL | FTL_IO_PPA_MODE,
		.type		= FTL_IO_READ,
		.iov_cnt	= 1,
		.req_size	= num_lbks,
		.fn		= ftl_reloc_read_cb,
	};

	opts.data = spdk_dma_malloc(PAGE_SIZE * num_lbks, PAGE_SIZE, NULL);
	if (!opts.data) {
		return -1;
	}

	io = ftl_io_init_internal(&opts);
	io->ppa = ppa;
	return 0;
}

static int
ftl_reloc_read(struct ftl_band_reloc *breloc, struct ftl_io *io)
{
	struct ftl_ppa ppa;
	size_t num_lbks;
	int rc;

	num_lbks = ftl_reloc_next_lbks(breloc, &ppa);

	if (!num_lbks) {
		spdk_ring_enqueue(breloc->free_queue, (void **)&io, 1);
		return 0;
	}

	if (ftl_reloc_io_init(breloc, io, ppa, num_lbks)) {
		SPDK_ERRLOG("Failed to initialize io for relocation.");
		return -1;
	}

	rc = ftl_io_read(io);
	if (rc == -ENOMEM) {
		rc = 0;
	}

	return rc;
}

static void
ftl_reloc_process_queue(struct ftl_band_reloc *breloc, struct spdk_ring *queue,
			ftl_reloc_fn fn)
{
	size_t i, num_ios;
	struct ftl_reloc *reloc = breloc->parent;

	num_ios = spdk_ring_dequeue(queue, (void **)reloc->io, reloc->max_qdepth);

	for (i = 0; i < num_ios; ++i) {
		if (fn(breloc, reloc->io[i])) {
			SPDK_ERRLOG("Reloc queue processing failed\n");
			assert(false);
		}
	}
}

static void
ftl_reloc_process_write_queue(struct ftl_band_reloc *breloc)
{
	ftl_reloc_process_queue(breloc, breloc->write_queue, ftl_reloc_write);
}

static void
ftl_reloc_process_free_queue(struct ftl_band_reloc *breloc)
{
	ftl_reloc_process_queue(breloc, breloc->free_queue, ftl_reloc_read);
}

static int
ftl_reloc_done(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc *reloc = breloc->parent;

	return spdk_ring_count(breloc->free_queue) == reloc->max_qdepth;
}

static void
ftl_reloc_release_io(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc *reloc = breloc->parent;
	size_t i, num_ios;

	num_ios = spdk_ring_dequeue(breloc->free_queue, (void **)reloc->io, reloc->max_qdepth);

	for (i = 0; i < num_ios; ++i) {
		ftl_io_free(reloc->io[i]);
	}
}

static void
ftl_reloc_release(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc *reloc = breloc->parent;
	struct ftl_band *band = breloc->band;

	if (band->high_prio) {
		band->high_prio = 0;
		TAILQ_REMOVE(&reloc->prio_queue, breloc, entry);
	} else {
		TAILQ_REMOVE(&reloc->active_queue, breloc, entry);
	}

	ftl_reloc_release_io(breloc);
	ftl_reloc_iter_reset(breloc);
	ftl_band_release_md(band);

	breloc->active = 0;
	reloc->num_active--;

	if (breloc->num_lbks) {
		TAILQ_INSERT_TAIL(&reloc->pending_queue, breloc, entry);
		return;
	}

	if (ftl_band_empty(band)) {
		ftl_band_set_state(breloc->band, FTL_BAND_STATE_FREE);
	}
}

static void
ftl_process_reloc(struct ftl_band_reloc *breloc)
{
	ftl_reloc_process_free_queue(breloc);

	ftl_reloc_process_write_queue(breloc);

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

	breloc->reloc_map = spdk_bit_array_create(ftl_num_band_lbks(reloc->dev));
	if (!breloc->reloc_map) {
		SPDK_ERRLOG("Failed to initialize reloc map");
		return -1;
	}

	breloc->iter.chk_offset = calloc(ftl_dev_num_punits(band->dev),
					 sizeof(*breloc->iter.chk_offset));
	if (!breloc->iter.chk_offset) {
		SPDK_ERRLOG("Failed to initialize reloc iterator");
		return -1;
	}

	breloc->free_queue = spdk_ring_create(SPDK_RING_TYPE_MP_SC,
					      reloc->max_qdepth * 2,
					      SPDK_ENV_SOCKET_ID_ANY);
	if (!breloc->free_queue) {
		SPDK_ERRLOG("Failed to initialize reloc free queue");
		return -1;
	}

	breloc->write_queue = spdk_ring_create(SPDK_RING_TYPE_MP_SC,
					       reloc->max_qdepth * 2,
					       SPDK_ENV_SOCKET_ID_ANY);
	if (!breloc->write_queue) {
		SPDK_ERRLOG("Failed to initialize reloc write queue");
		return -1;
	}

	return 0;
}

static void
ftl_band_reloc_free(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc *reloc = breloc->parent;
	struct ftl_io *io;
	size_t i, num_ios;

	if (!breloc) {
		return;
	}

	if (breloc->active) {
		num_ios = spdk_ring_dequeue(breloc->write_queue, (void **)reloc->io, reloc->max_qdepth);
		for (i = 0; i < num_ios; ++i) {
			io = reloc->io[i];
			if (io->flags & FTL_IO_INITIALIZED) {
				ftl_reloc_free_io(breloc, io);
			}
		}

		ftl_reloc_release_io(breloc);
	}

	spdk_ring_free(breloc->free_queue);
	spdk_ring_free(breloc->write_queue);
	spdk_bit_array_free(&breloc->reloc_map);
	free(breloc->iter.chk_offset);
}

static void
ftl_reloc_add_active_queue(struct ftl_band_reloc *breloc)
{
	struct ftl_reloc *reloc = breloc->parent;

	TAILQ_REMOVE(&reloc->pending_queue, breloc, entry);
	TAILQ_INSERT_HEAD(&reloc->active_queue, breloc, entry);
	ftl_reloc_prep(breloc);
}

struct ftl_reloc *
ftl_reloc_init(struct spdk_ftl_dev *dev)
{
#define POOL_NAME_LEN 128
	struct ftl_reloc *reloc;
	char pool_name[POOL_NAME_LEN];
	int rc;
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

	reloc->brelocs =  calloc(ftl_dev_num_bands(dev), sizeof(*reloc->brelocs));
	if (!reloc->brelocs) {
		goto error;
	}

	reloc->io = calloc(reloc->max_qdepth, sizeof(*reloc->io));
	if (!reloc->io) {
		goto error;
	}

	for (i = 0; i < ftl_dev_num_bands(reloc->dev); ++i) {
		if (ftl_band_reloc_init(reloc, &reloc->brelocs[i], &dev->bands[i])) {
			goto error;
		}
	}

	rc = snprintf(pool_name, sizeof(pool_name), "%s-%s", dev->name, "reloc-io-pool");
	if (rc < 0 || rc >= POOL_NAME_LEN) {
		return NULL;
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

	for (i = 0; i < ftl_dev_num_bands(reloc->dev); ++i) {
		ftl_band_reloc_free(&reloc->brelocs[i]);
	}

	free(reloc->brelocs);
	free(reloc->io);
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

void
ftl_reloc(struct ftl_reloc *reloc)
{
	struct ftl_band_reloc *breloc, *tbreloc;

	if (ftl_reloc_is_halted(reloc)) {
		return;
	}

	/* Process first band from priority queue and return */
	breloc = TAILQ_FIRST(&reloc->prio_queue);
	if (breloc) {
		if (!breloc->active) {
			ftl_reloc_prep(breloc);
		}
		ftl_process_reloc(breloc);
		return;
	}

	TAILQ_FOREACH_SAFE(breloc, &reloc->pending_queue, entry, tbreloc) {
		if (reloc->num_active == reloc->max_active) {
			break;
		}
		ftl_reloc_add_active_queue(breloc);
	}

	TAILQ_FOREACH_SAFE(breloc, &reloc->active_queue, entry, tbreloc) {
		ftl_process_reloc(breloc);
	}
}

void
ftl_reloc_add(struct ftl_reloc *reloc, struct ftl_band *band, size_t offset,
	      size_t num_lbks, int prio)
{
	struct ftl_band_reloc *breloc = &reloc->brelocs[band->id];
	size_t i, prev_lbks = breloc->num_lbks;

	for (i = offset; i < offset + num_lbks; ++i) {
		if (spdk_bit_array_get(breloc->reloc_map, i)) {
			continue;
		}
		spdk_bit_array_set(breloc->reloc_map, i);
		breloc->num_lbks++;
	}

	if (!prev_lbks && !prio) {
		TAILQ_INSERT_HEAD(&reloc->pending_queue, breloc, entry);
	}

	if (prio) {
		TAILQ_INSERT_TAIL(&reloc->prio_queue, breloc, entry);
		ftl_band_acquire_md(breloc->band);
	}
}
