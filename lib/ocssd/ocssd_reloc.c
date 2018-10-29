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

#include <spdk/likely.h>
#include <spdk_internal/log.h>
#include <spdk/ocssd.h>
#include "ocssd_reloc.h"
#include "ocssd_core.h"
#include "ocssd_io.h"
#include "ocssd_rwb.h"
#include "ocssd_band.h"
#include "ocssd_debug.h"

struct ocssd_reloc;
struct ocssd_band_reloc;

typedef int (*ocssd_reloc_fn)(struct ocssd_band_reloc *, struct ocssd_io *);

struct ocssd_band_reloc {
	struct ocssd_reloc			*parent;

	/* Band being relocated */
	struct ocssd_band			*band;

	/* Number of logical blocks to be relocated */
	size_t					num_lbks;

	/* Bitmap of logical blocks to be relocated */
	void					*reloc_map;

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

	TAILQ_ENTRY(ocssd_band_reloc)		entry;

	/* TODO: get rid of md_buf */
	void					*md_buf;
};

struct ocssd_reloc {
	/* Device associated with relocate */
	struct ocssd_dev			*dev;

	/* Indicates relocate is about to halt */
	int					halt;

	/* Maximum number of IOs per band */
	size_t					max_qdepth;

	/* Maximum number of active band relocates */
	size_t					max_active;

	/* Maximum transfer size per single IO */
	size_t					xfer_size;

	/* IO memory pool */
	struct spdk_mempool			*io_pool;

	/* Array of band relocates */
	struct ocssd_band_reloc			*brelocs;

	/* Number of active/priority band relocates */
	size_t					num_active;

	/* Priority band relocates queue */
	TAILQ_HEAD(, ocssd_band_reloc)		prio_queue;

	/* Active band relocates queue */
	TAILQ_HEAD(, ocssd_band_reloc)		active_queue;

	/* Pending band relocates  queue */
	TAILQ_HEAD(, ocssd_band_reloc)		pending_queue;
};

static int
ocssd_reloc_retry(int status)
{
	return status == -EAGAIN;
}

static size_t
ocssd_reloc_max_ios(struct ocssd_reloc *reloc)
{
	return reloc->max_qdepth;
}

static struct ocssd_band_reloc *
ocssd_io_get_band_reloc(struct ocssd_io *io)
{
	return &io->dev->reloc->brelocs[io->band->id];
}

static void
ocssd_reloc_clr_lbk(struct ocssd_band_reloc *breloc, size_t lbkoff)
{
	ocssd_clr_bit(lbkoff, breloc->reloc_map);
	breloc->num_lbks--;
}

static void
_ocssd_reloc_prep(struct ocssd_band_reloc *breloc)
{
	struct ocssd_io *io;
	struct ocssd_reloc *reloc = breloc->parent;

	for (size_t i = 0; i < ocssd_reloc_max_ios(reloc); ++i) {
		io = spdk_mempool_get(breloc->parent->io_pool);
		spdk_ring_enqueue(breloc->free_queue, (void **)&io, 1);
	}
}

static void
ocssd_reloc_read_lba_map_cb(void *arg, int status)
{
	struct ocssd_io *io = arg;
	struct ocssd_band_reloc *breloc = ocssd_io_get_band_reloc(io);
	struct ocssd_reloc *reloc = breloc->parent;

	assert(status == 0);
	spdk_dma_free(breloc->md_buf);
	spdk_mempool_put(reloc->io_pool, io);
	_ocssd_reloc_prep(breloc);
}

static int
ocssd_reloc_read_lba_map(struct ocssd_band_reloc *breloc)
{
	struct ocssd_band *band = breloc->band;
	struct ocssd_dev *dev = band->dev;
	struct ocssd_io *io = spdk_mempool_get(breloc->parent->io_pool);

	io->dev = dev;
	io->band = band;
	io->cb.ctx = io;
	io->cb.fn = ocssd_reloc_read_lba_map_cb;

	breloc->md_buf = spdk_dma_zmalloc(ocssd_lba_map_num_lbks(dev) * OCSSD_BLOCK_SIZE,
					  OCSSD_BLOCK_SIZE, NULL);
	if (!breloc->md_buf) {
		return -1;
	}

	int __attribute__((unused)) rc = ocssd_band_alloc_md(band);
	assert(rc == 0);

	return ocssd_band_read_lba_map(band, &band->md, breloc->md_buf, &io->cb);
}

static void
ocssd_reloc_prep(struct ocssd_band_reloc *breloc)
{
	struct ocssd_band *band = breloc->band;
	struct ocssd_reloc *reloc = breloc->parent;

	breloc->active = 1;
	reloc->num_active++;

	if (!band->high_prio) {
		assert(band->md.lba_map == NULL);
		ocssd_reloc_read_lba_map(breloc);
		return;
	}

	_ocssd_reloc_prep(breloc);
}

static void
ocssd_reloc_free_io(struct ocssd_band_reloc *breloc, struct ocssd_io *io)
{
	spdk_dma_free(io->iov.iov_base);
	free(io->lbas);
	spdk_ring_enqueue(breloc->free_queue, (void **)&io, 1);
}

static void
ocssd_reloc_write_cb(void *arg, int status)
{
	struct ocssd_io *io = arg;
	struct ocssd_ppa ppa = io->ppa;
	struct ocssd_band_reloc *breloc = ocssd_io_get_band_reloc(io);

	assert(status == 0);

	for (size_t i = 0; i < io->lbk_cnt; ++i) {
		ppa.lbk = io->ppa.lbk + i;
		size_t lbkoff = ocssd_band_lbkoff_from_ppa(breloc->band, ppa);
		ocssd_reloc_clr_lbk(breloc, lbkoff);
	}

	ocssd_reloc_free_io(breloc, io);
}

static void
ocssd_reloc_read_cb(void *arg, int status)
{
	struct ocssd_io *io = arg;
	struct ocssd_band_reloc *breloc = ocssd_io_get_band_reloc(io);

	/* TODO: We should handle fail on relocation read. We need to inform */
	/* user that this group of blocks is bad (update l2p with bad block address and */
	/* put it to lba_map/sector_lba). Maybe we could also retry read with smaller granularity? */
	assert(status == 0);

	ocssd_io_clear_flags(io, OCSSD_IO_INITIALIZED);
	spdk_ring_enqueue(breloc->write_queue, (void **)&io, 1);
}

static void
ocssd_reloc_iter_reset(struct ocssd_band_reloc *breloc)
{
	memset(breloc->iter.chk_offset, 0, ocssd_dev_num_punits(breloc->band->dev) *
	       sizeof(*breloc->iter.chk_offset));
	breloc->iter.chk_current = 0;
}

static size_t
ocssd_reloc_iter_lbkoff(struct ocssd_band_reloc *breloc)
{
	size_t chk_offset = breloc->iter.chk_current * ocssd_dev_lbks_in_chunk(breloc->parent->dev);
	return breloc->iter.chk_offset[breloc->iter.chk_current] + chk_offset;
}

static void
ocssd_reloc_iter_next_chk(struct ocssd_band_reloc *breloc)
{
	size_t num_chk = ocssd_dev_num_punits(breloc->band->dev);
	breloc->iter.chk_current = (breloc->iter.chk_current + 1) % num_chk;
}

static int
ocssd_reloc_lbk_valid(struct ocssd_band_reloc *breloc, size_t lbkoff)
{
	return ocssd_get_bit(lbkoff, breloc->reloc_map) &&
	       ocssd_band_lbkoff_valid(breloc->band, lbkoff);
}

static size_t
ocssd_reloc_iter_chk_offset(struct ocssd_band_reloc *breloc)
{
	size_t chunk = breloc->iter.chk_current;
	return breloc->iter.chk_offset[chunk];
}

static int
ocssd_reloc_iter_next(struct ocssd_band_reloc *breloc, size_t *lbkoff)
{
	size_t chunk = breloc->iter.chk_current;

	*lbkoff = ocssd_reloc_iter_lbkoff(breloc);
	breloc->iter.chk_offset[chunk]++;

	if (ocssd_reloc_lbk_valid(breloc, *lbkoff)) {
		return 1;
	}

	return 0;
}

static int
ocssd_reloc_first_valid_lbk(struct ocssd_band_reloc *breloc, size_t *lbkoff)
{
	size_t num_lbks = ocssd_dev_lbks_in_chunk(breloc->parent->dev);

	for (size_t i = ocssd_reloc_iter_chk_offset(breloc); i < num_lbks; ++i) {
		if (ocssd_reloc_iter_next(breloc, lbkoff)) {
			return 1;
		}
		ocssd_reloc_clr_lbk(breloc, *lbkoff);
	}

	return 0;
}

static int
ocssd_reloc_iter_done(struct ocssd_band_reloc *breloc)
{
	size_t num_chk = ocssd_dev_num_punits(breloc->band->dev);
	size_t last_lbk = ocssd_dev_lbks_in_chunk(breloc->parent->dev) - 1;

	for (size_t i = 0; i < num_chk; ++i) {
		if (breloc->iter.chk_offset[i] != last_lbk) {
			return 0;
		}
	}

	return 1;
}

static size_t
ocssd_reloc_find_valid_lbks(struct ocssd_band_reloc *breloc,
			    size_t num_lbk, struct ocssd_ppa *ppa)
{
	size_t lbkoff, lbk_cnt = 0;

	if (!ocssd_reloc_first_valid_lbk(breloc, &lbkoff)) {
		return 0;
	}

	*ppa = ocssd_band_ppa_from_lbkoff(breloc->band, lbkoff);

	for (lbk_cnt = 1; lbk_cnt < num_lbk; lbk_cnt++) {
		if (!ocssd_reloc_iter_next(breloc, &lbkoff)) {
			ocssd_reloc_clr_lbk(breloc, lbkoff);
			break;
		}
	}

	return lbk_cnt;
}

static size_t
ocssd_reloc_next_lbks(struct ocssd_band_reloc *breloc, struct ocssd_ppa *ppa)
{
	size_t lbk_cnt = 0;
	struct ocssd_dev *dev = breloc->parent->dev;

	for (size_t i = 0; i < ocssd_dev_num_punits(dev); ++i) {
		lbk_cnt = ocssd_reloc_find_valid_lbks(breloc,
						      breloc->parent->xfer_size, ppa);
		ocssd_reloc_iter_next_chk(breloc);

		if (lbk_cnt || ocssd_reloc_iter_done(breloc)) {
			break;
		}
	}

	return lbk_cnt;
}

static void
ocssd_reloc_io_reinit(struct ocssd_io *io, struct ocssd_band_reloc *breloc,
		      ocssd_fn fn, enum ocssd_io_type io_type, int flags)
{
	uint64_t lbkoff;
	struct ocssd_ppa ppa = io->ppa;

	ocssd_io_reinit(io, fn, io, flags | OCSSD_IO_INTERNAL, io_type);

	io->ppa = ppa;
	io->band = breloc->band;
	io->lbas = calloc(io->lbk_cnt, sizeof(uint64_t));

	for (size_t i = 0; i < io->lbk_cnt; ++i) {
		ppa.lbk = io->ppa.lbk + i;
		lbkoff = ocssd_band_lbkoff_from_ppa(breloc->band, ppa);

		if (!ocssd_band_lbkoff_valid(breloc->band, lbkoff)) {
			io->lbas[i] = OCSSD_LBA_INVALID;
			continue;
		}

		io->lbas[i] = breloc->band->md.lba_map[lbkoff];
	}

	ocssd_trace(lba_io_init, ocssd_dev_trace(io->dev), io);
}

static int
ocssd_reloc_write(struct ocssd_band_reloc *breloc, struct ocssd_io *io)
{
	if (!ocssd_io_initialized(io)) {
		ocssd_reloc_io_reinit(io, breloc, ocssd_reloc_write_cb,
				      OCSSD_IO_WRITE, OCSSD_IO_WEAK | OCSSD_IO_VECTOR_LBA);
	}

	int rc = ocssd_io_write(io);
	if (ocssd_reloc_retry(rc)) {
		spdk_ring_enqueue(breloc->write_queue, (void **)&io, 1);
		return 0;
	}

	return rc;
}

static int
ocssd_reloc_io_init(struct ocssd_band_reloc *breloc, struct ocssd_io *io,
		    struct ocssd_ppa ppa, size_t num_lbks)
{
	struct ocssd_io_init_opts opts = {
		.dev		= breloc->parent->dev,
		.io		= io,
		.rwb_batch	= NULL,
		.band		= breloc->band,
		.size		= sizeof(*io),
		.flags		= OCSSD_IO_INTERNAL | OCSSD_IO_PPA_MODE,
		.type		= OCSSD_IO_READ,
		.iov_cnt	= 1,
		.req_size	= num_lbks,
		.fn		= ocssd_reloc_read_cb,
	};

	opts.data = spdk_dma_malloc(PAGE_SIZE * num_lbks, PAGE_SIZE, NULL);
	if (!opts.data) {
		return -1;
	}

	io = ocssd_io_init_internal(&opts);
	io->ppa = ppa;
	return 0;
}

static int
ocssd_reloc_read(struct ocssd_band_reloc *breloc, struct ocssd_io *io)
{
	struct ocssd_ppa ppa;
	size_t num_lbks;

	num_lbks = ocssd_reloc_next_lbks(breloc, &ppa);

	if (!num_lbks) {
		spdk_ring_enqueue(breloc->free_queue, (void **)&io, 1);
		return 0;
	}

	if (ocssd_reloc_io_init(breloc, io, ppa, num_lbks)) {
		SPDK_ERRLOG("Failed to initialize io for relocation.");
		return -1;
	}

	return ocssd_io_read(io);
}

static void
ocssd_reloc_process_queue(struct ocssd_band_reloc *breloc, struct spdk_ring *queue,
			  ocssd_reloc_fn fn)
{
	int rc __attribute__((unused));
	size_t num_ios;
	struct ocssd_io *io[ocssd_reloc_max_ios(breloc->parent)];

	num_ios = spdk_ring_dequeue(queue, (void **)io,
				    ocssd_reloc_max_ios(breloc->parent));

	for (size_t i = 0; i < num_ios; ++i) {
		rc = fn(breloc, io[i]);
		assert(rc == 0);
	}
}

static void
ocssd_reloc_process_write_queue(struct ocssd_band_reloc *breloc)
{
	ocssd_reloc_process_queue(breloc, breloc->write_queue, ocssd_reloc_write);
}

static void
ocssd_reloc_process_free_queue(struct ocssd_band_reloc *breloc)
{
	ocssd_reloc_process_queue(breloc, breloc->free_queue, ocssd_reloc_read);
}

static int
ocssd_reloc_done(struct ocssd_band_reloc *breloc)
{
	return spdk_ring_count(breloc->free_queue) == ocssd_reloc_max_ios(breloc->parent);
}

static void
ocssd_reloc_release_io(struct ocssd_band_reloc *breloc)
{
	struct ocssd_reloc *reloc = breloc->parent;
	struct ocssd_io *io[ocssd_reloc_max_ios(reloc)];
	size_t num_ios;

	num_ios = spdk_ring_dequeue(breloc->free_queue, (void **)io,
				    ocssd_reloc_max_ios(reloc));

	for (size_t i = 0; i < num_ios; ++i) {
		spdk_mempool_put(reloc->io_pool, io[i]);
	}
}

static void
ocssd_reloc_release(struct ocssd_band_reloc *breloc)
{
	struct ocssd_reloc *reloc = breloc->parent;
	struct ocssd_band *band = breloc->band;

	if (band->high_prio) {
		band->high_prio = 0;
		TAILQ_REMOVE(&reloc->prio_queue, breloc, entry);
	} else {
		TAILQ_REMOVE(&reloc->active_queue, breloc, entry);
	}

	ocssd_reloc_release_io(breloc);
	ocssd_reloc_iter_reset(breloc);
	ocssd_band_release_md(band);

	breloc->active = 0;
	reloc->num_active--;

	if (breloc->num_lbks) {
		TAILQ_INSERT_TAIL(&reloc->pending_queue, breloc, entry);
		return;
	}

	if (ocssd_band_empty(band)) {
		ocssd_band_set_state(breloc->band, OCSSD_BAND_STATE_FREE);
	}
}

static void
ocssd_process_reloc(struct ocssd_band_reloc *breloc)
{
	ocssd_reloc_process_write_queue(breloc);

	ocssd_reloc_process_free_queue(breloc);

	if (ocssd_reloc_done(breloc)) {
		ocssd_reloc_release(breloc);
	}
}

static int
ocssd_band_reloc_init(struct ocssd_reloc *reloc, struct ocssd_band_reloc *breloc,
		      struct ocssd_band *band)
{
	breloc->band = band;
	breloc->parent = reloc;

	breloc->reloc_map = calloc(ocssd_vld_map_size(band->dev), 1);
	if (!breloc->reloc_map) {
		SPDK_ERRLOG("Failed to initialize reloc map");
		return -1;
	}

	breloc->iter.chk_offset = calloc(ocssd_dev_num_punits(band->dev),
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
ocssd_band_reloc_free(struct ocssd_band_reloc *breloc)
{
	if (!breloc) {
		return;
	}

	spdk_ring_free(breloc->free_queue);
	spdk_ring_free(breloc->write_queue);
	free(breloc->reloc_map);
	free(breloc->iter.chk_offset);
}

static void
ocssd_reloc_add_active_queue(struct ocssd_band_reloc *breloc)
{
	struct ocssd_reloc *reloc = breloc->parent;

	TAILQ_REMOVE(&reloc->pending_queue, breloc, entry);
	TAILQ_INSERT_HEAD(&reloc->active_queue, breloc, entry);
	ocssd_reloc_prep(breloc);
}

struct ocssd_reloc *
ocssd_reloc_init(struct ocssd_dev *dev)
{
#define POOL_NAME_LEN 128
	struct ocssd_reloc *reloc;
	char pool_name[POOL_NAME_LEN];
	int rc;

	reloc = calloc(1, sizeof(*reloc));
	if (!reloc) {
		return NULL;
	}

	reloc->dev = dev;
	reloc->halt = 1;
	reloc->max_qdepth = dev->conf.max_reloc_qdepth;
	reloc->max_active = dev->conf.max_active_relocs;
	reloc->xfer_size = dev->xfer_size;

	reloc->brelocs =  calloc(ocssd_dev_num_bands(dev), sizeof(*reloc->brelocs));
	if (!reloc->brelocs) {
		goto error;
	}

	for (size_t i = 0; i < ocssd_dev_num_bands(reloc->dev); ++i) {
		if (ocssd_band_reloc_init(reloc, &reloc->brelocs[i], &dev->bands[i])) {
			goto error;
		}
	}

	rc = snprintf(pool_name, sizeof(pool_name), "%s-%s", dev->name, "reloc-io-pool");
	if (rc < 0 || rc >= POOL_NAME_LEN) {
		return NULL;
	}

	/* Add one to max_active band to handle priority bands */
	reloc->io_pool = spdk_mempool_create(pool_name,
					     reloc->max_qdepth * (reloc->max_active + 1),
					     sizeof(struct ocssd_io),
					     SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					     SPDK_ENV_SOCKET_ID_ANY);
	if (!reloc->io_pool) {
		goto error;
	}

	TAILQ_INIT(&reloc->pending_queue);
	TAILQ_INIT(&reloc->active_queue);
	TAILQ_INIT(&reloc->prio_queue);

	return reloc;
error:
	ocssd_reloc_free(reloc);
	return NULL;
}

void
ocssd_reloc_free(struct ocssd_reloc *reloc)
{
	if (!reloc) {
		return;
	}

	for (size_t i = 0; i < ocssd_dev_num_bands(reloc->dev); ++i) {
		ocssd_band_reloc_free(&reloc->brelocs[i]);
	}

	spdk_mempool_free(reloc->io_pool);
	free(reloc->brelocs);
	free(reloc);
}

int
ocssd_reloc_halted(const struct ocssd_reloc *reloc)
{
	return reloc->halt;
}

void
ocssd_reloc_halt(struct ocssd_reloc *reloc)
{
	reloc->halt = 1;
}

void
ocssd_reloc_resume(struct ocssd_reloc *reloc)
{
	reloc->halt = 0;
}

void
ocssd_reloc(struct ocssd_reloc *reloc)
{
	struct ocssd_band_reloc *breloc, *tbreloc;

	if (ocssd_reloc_halted(reloc)) {
		return;
	}

	/* Process first band from priority queue and return */
	breloc = TAILQ_FIRST(&reloc->prio_queue);
	if (breloc) {
		if (!breloc->active) {
			ocssd_reloc_prep(breloc);
		}
		ocssd_process_reloc(breloc);
		return;
	}

	TAILQ_FOREACH_SAFE(breloc, &reloc->pending_queue, entry, tbreloc) {
		if (reloc->num_active == reloc->max_active) {
			break;
		}
		ocssd_reloc_add_active_queue(breloc);
	}

	TAILQ_FOREACH_SAFE(breloc, &reloc->active_queue, entry, tbreloc) {
		ocssd_process_reloc(breloc);
	}
}

void
ocssd_reloc_add(struct ocssd_reloc *reloc, struct ocssd_band *band, size_t offset,
		size_t num_lbks, int prio)
{
	struct ocssd_band_reloc *breloc = &reloc->brelocs[band->id];
	size_t prev_lbks = breloc->num_lbks;

	for (size_t i = offset; i < offset + num_lbks; ++i) {
		if (ocssd_get_bit(i, breloc->reloc_map)) {
			continue;
		}
		ocssd_set_bit(i, breloc->reloc_map);
		breloc->num_lbks++;
	}

	if (!prev_lbks && !prio) {
		TAILQ_INSERT_HEAD(&reloc->pending_queue, breloc, entry);
	}

	if (prio) {
		TAILQ_INSERT_TAIL(&reloc->prio_queue, breloc, entry);
		ocssd_band_acquire_md(breloc->band);
	}
}
