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
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/crc32.h"

#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_io.h"

struct ftl_restore_band {
	struct ftl_restore		*parent;
	/* Associated band */
	struct ftl_band			*band;
	/* Status of retrieving this band's metadata */
	enum ftl_md_status		md_status;
	/* Padded queue link  */
	STAILQ_ENTRY(ftl_restore_band)	stailq;
};

struct ftl_nv_cache_restore;

/* Describes single phase to be restored from non-volatile cache */
struct ftl_nv_cache_range {
	struct ftl_nv_cache_restore	*parent;
	/* Start offset */
	uint64_t			start_addr;
	/* Last block's address */
	uint64_t			last_addr;
	/*
	 * Number of blocks (can be smaller than the difference between the last
	 * and the starting block due to range overlap)
	 */
	uint64_t			num_blocks;
	/* Number of blocks already recovered */
	uint64_t			num_recovered;
	/* Current address during recovery */
	uint64_t			current_addr;
	/* Phase of the range */
	unsigned int			phase;
	/* Indicates whether the data from this range needs to be recovered */
	bool				recovery;
};

struct ftl_nv_cache_block {
	struct ftl_nv_cache_restore	*parent;
	/* Data buffer */
	void				*buf;
	/* Metadata buffer */
	void				*md_buf;
	/* Block offset within the cache */
	uint64_t			offset;
};

struct ftl_nv_cache_restore {
	struct ftl_nv_cache		*nv_cache;
	/* IO channel to use */
	struct spdk_io_channel		*ioch;
	/*
	 * Non-volatile cache ranges. The ranges can overlap, as we have no
	 * control over the order of completions. The phase of the range is the
	 * index within the table. The range with index 0 marks blocks that were
	 * never written.
	 */
	struct ftl_nv_cache_range	range[FTL_NV_CACHE_PHASE_COUNT];
#define FTL_NV_CACHE_RESTORE_DEPTH 128
	/* Non-volatile cache buffers */
	struct ftl_nv_cache_block	block[FTL_NV_CACHE_RESTORE_DEPTH];
	/* Current address */
	uint64_t			current_addr;
	/* Number of outstanding requests */
	size_t				num_outstanding;
	/* Recovery/scan status */
	int				status;
	/* Current phase of the recovery */
	unsigned int			phase;
};

struct ftl_restore {
	struct spdk_ftl_dev		*dev;
	/* Completion callback (called for each phase of the restoration) */
	ftl_restore_fn			cb;
	/* Completion callback context */
	void				*cb_arg;
	/* Number of inflight IOs */
	unsigned int			num_ios;
	/* Current band number (index in the below bands array) */
	unsigned int			current;
	/* Array of bands */
	struct ftl_restore_band		*bands;
	/* Queue of bands to be padded (due to unsafe shutdown) */
	STAILQ_HEAD(, ftl_restore_band) pad_bands;
	/* Status of the padding */
	int				pad_status;
	/* Metadata buffer */
	void				*md_buf;
	/* LBA map buffer */
	void				*lba_map;
	/* Indicates we're in the final phase of the restoration */
	bool				final_phase;
	/* Non-volatile cache recovery */
	struct ftl_nv_cache_restore	nv_cache;
};

static int
ftl_restore_tail_md(struct ftl_restore_band *rband);
static void
ftl_pad_zone_cb(struct ftl_io *io, void *arg, int status);
static void
ftl_restore_pad_band(struct ftl_restore_band *rband);

static void
ftl_restore_free(struct ftl_restore *restore)
{
	unsigned int i;

	if (!restore) {
		return;
	}

	for (i = 0; i < FTL_NV_CACHE_RESTORE_DEPTH; ++i) {
		spdk_dma_free(restore->nv_cache.block[i].buf);
	}

	spdk_dma_free(restore->md_buf);
	free(restore->bands);
	free(restore);
}

static struct ftl_restore *
ftl_restore_init(struct spdk_ftl_dev *dev, ftl_restore_fn cb, void *cb_arg)
{
	struct ftl_restore *restore;
	struct ftl_restore_band *rband;
	size_t i;

	restore = calloc(1, sizeof(*restore));
	if (!restore) {
		goto error;
	}

	restore->dev = dev;
	restore->cb = cb;
	restore->cb_arg = cb_arg;
	restore->final_phase = false;

	restore->bands = calloc(ftl_get_num_bands(dev), sizeof(*restore->bands));
	if (!restore->bands) {
		goto error;
	}

	STAILQ_INIT(&restore->pad_bands);

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		rband = &restore->bands[i];
		rband->band = &dev->bands[i];
		rband->parent = restore;
		rband->md_status = FTL_MD_NO_MD;
	}

	/* Allocate buffer capable of holding head mds of all bands */
	restore->md_buf = spdk_dma_zmalloc(ftl_get_num_bands(dev) * ftl_head_md_num_blocks(dev) *
					   FTL_BLOCK_SIZE, 0, NULL);
	if (!restore->md_buf) {
		goto error;
	}

	return restore;
error:
	ftl_restore_free(restore);
	return NULL;
}

static void
ftl_restore_complete(struct ftl_restore *restore, int status)
{
	struct ftl_restore *ctx = status ? NULL : restore;
	bool final_phase = restore->final_phase;

	restore->cb(ctx, status, restore->cb_arg);
	if (status || final_phase) {
		ftl_restore_free(restore);
	}
}

static int
ftl_band_cmp(const void *lband, const void *rband)
{
	uint64_t lseq = ((struct ftl_restore_band *)lband)->band->seq;
	uint64_t rseq = ((struct ftl_restore_band *)rband)->band->seq;

	if (lseq < rseq) {
		return -1;
	} else {
		return 1;
	}
}

static int
ftl_restore_check_seq(const struct ftl_restore *restore)
{
	const struct spdk_ftl_dev *dev = restore->dev;
	const struct ftl_restore_band *rband;
	const struct ftl_band *next_band;
	size_t i;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		rband = &restore->bands[i];
		if (rband->md_status != FTL_MD_SUCCESS) {
			continue;
		}

		next_band = LIST_NEXT(rband->band, list_entry);
		if (next_band && rband->band->seq == next_band->seq) {
			return -1;
		}
	}

	return 0;
}

static bool
ftl_restore_head_valid(struct spdk_ftl_dev *dev, struct ftl_restore *restore, size_t *num_valid)
{
	struct ftl_restore_band *rband;
	size_t i;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		rband = &restore->bands[i];

		if (rband->md_status != FTL_MD_SUCCESS &&
		    rband->md_status != FTL_MD_NO_MD &&
		    rband->md_status != FTL_MD_IO_FAILURE) {
			SPDK_ERRLOG("Inconsistent head metadata found on band %u\n",
				    rband->band->id);
			return false;
		}

		if (rband->md_status == FTL_MD_SUCCESS) {
			(*num_valid)++;
		}
	}

	return true;
}

static void
ftl_restore_head_complete(struct ftl_restore *restore)
{
	struct spdk_ftl_dev *dev = restore->dev;
	size_t num_valid = 0;
	int status = -EIO;

	if (!ftl_restore_head_valid(dev, restore, &num_valid)) {
		goto out;
	}

	if (num_valid == 0) {
		SPDK_ERRLOG("Couldn't find any valid bands\n");
		goto out;
	}

	/* Sort bands in sequence number ascending order */
	qsort(restore->bands, ftl_get_num_bands(dev), sizeof(struct ftl_restore_band),
	      ftl_band_cmp);

	if (ftl_restore_check_seq(restore)) {
		SPDK_ERRLOG("Band sequence consistency failed\n");
		goto out;
	}

	dev->num_lbas = dev->global_md.num_lbas;
	status = 0;
out:
	ftl_restore_complete(restore, status);
}

static void
ftl_restore_head_cb(struct ftl_io *io, void *ctx, int status)
{
	struct ftl_restore_band *rband = ctx;
	struct ftl_restore *restore = rband->parent;
	unsigned int num_ios;

	rband->md_status = status;
	num_ios = __atomic_fetch_sub(&restore->num_ios, 1, __ATOMIC_SEQ_CST);
	assert(num_ios > 0);

	if (num_ios == 1) {
		ftl_restore_head_complete(restore);
	}
}

static void
ftl_restore_head_md(void *ctx)
{
	struct ftl_restore *restore = ctx;
	struct spdk_ftl_dev *dev = restore->dev;
	struct ftl_restore_band *rband;
	struct ftl_lba_map *lba_map;
	unsigned int num_failed = 0, num_ios;
	size_t i;

	restore->num_ios = ftl_get_num_bands(dev);

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		rband = &restore->bands[i];
		lba_map = &rband->band->lba_map;

		lba_map->dma_buf = restore->md_buf + i * ftl_head_md_num_blocks(dev) * FTL_BLOCK_SIZE;

		if (ftl_band_read_head_md(rband->band, ftl_restore_head_cb, rband)) {
			if (spdk_likely(rband->band->num_zones)) {
				SPDK_ERRLOG("Failed to read metadata on band %zu\n", i);

				rband->md_status = FTL_MD_INVALID_CRC;

				/* If the first IO fails, don't bother sending anything else */
				if (i == 0) {
					ftl_restore_complete(restore, -EIO);
				}
			}

			num_failed++;
		}
	}

	if (spdk_unlikely(num_failed > 0)) {
		num_ios = __atomic_fetch_sub(&restore->num_ios, num_failed, __ATOMIC_SEQ_CST);
		if (num_ios == num_failed) {
			ftl_restore_complete(restore, -EIO);
		}
	}
}

int
ftl_restore_md(struct spdk_ftl_dev *dev, ftl_restore_fn cb, void *cb_arg)
{
	struct ftl_restore *restore;

	restore = ftl_restore_init(dev, cb, cb_arg);
	if (!restore) {
		return -ENOMEM;
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_restore_head_md, restore);

	return 0;
}

static int
ftl_restore_l2p(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_addr addr;
	uint64_t lba;
	size_t i;

	for (i = 0; i < ftl_get_num_blocks_in_band(band->dev); ++i) {
		if (!spdk_bit_array_get(band->lba_map.vld, i)) {
			continue;
		}

		lba = band->lba_map.map[i];
		if (lba >= dev->num_lbas) {
			return -1;
		}

		addr = ftl_l2p_get(dev, lba);
		if (!ftl_addr_invalid(addr)) {
			ftl_invalidate_addr(dev, addr);
		}

		addr = ftl_band_addr_from_block_offset(band, i);

		ftl_band_set_addr(band, lba, addr);
		ftl_l2p_set(dev, lba, addr);
	}

	return 0;
}

static struct ftl_restore_band *
ftl_restore_next_band(struct ftl_restore *restore)
{
	struct ftl_restore_band *rband;

	for (; restore->current < ftl_get_num_bands(restore->dev); ++restore->current) {
		rband = &restore->bands[restore->current];

		if (spdk_likely(rband->band->num_zones) &&
		    rband->md_status == FTL_MD_SUCCESS) {
			restore->current++;
			return rband;
		}
	}

	return NULL;
}

static void
ftl_nv_cache_restore_complete(struct ftl_nv_cache_restore *restore, int status)
{
	struct ftl_restore *ftl_restore = SPDK_CONTAINEROF(restore, struct ftl_restore, nv_cache);

	restore->status = restore->status ? : status;
	if (restore->num_outstanding == 0) {
		ftl_restore_complete(ftl_restore, restore->status);
	}
}

static void ftl_nv_cache_block_read_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void
ftl_nv_cache_restore_done(struct ftl_nv_cache_restore *restore, uint64_t current_addr)
{
	struct ftl_nv_cache *nv_cache = restore->nv_cache;

	pthread_spin_lock(&nv_cache->lock);
	nv_cache->current_addr = current_addr;
	nv_cache->ready = true;
	pthread_spin_unlock(&nv_cache->lock);

	SPDK_DEBUGLOG(ftl_init, "Enabling non-volatile cache (phase: %u, addr: %"
		      PRIu64")\n", nv_cache->phase, current_addr);

	ftl_nv_cache_restore_complete(restore, 0);
}

static void
ftl_nv_cache_write_header_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_nv_cache_restore *restore = cb_arg;

	spdk_bdev_free_io(bdev_io);
	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Unable to write the non-volatile cache metadata header\n");
		ftl_nv_cache_restore_complete(restore, -EIO);
		return;
	}

	ftl_nv_cache_restore_done(restore, FTL_NV_CACHE_DATA_OFFSET);
}

static void
ftl_nv_cache_scrub_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_nv_cache_restore *restore = cb_arg;
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
	int rc;

	spdk_bdev_free_io(bdev_io);
	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Scrubbing non-volatile cache failed\n");
		ftl_nv_cache_restore_complete(restore, -EIO);
		return;
	}

	nv_cache->phase = 1;
	rc = ftl_nv_cache_write_header(nv_cache, false, ftl_nv_cache_write_header_cb, restore);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Unable to write the non-volatile cache metadata header: %s\n",
			    spdk_strerror(-rc));
		ftl_nv_cache_restore_complete(restore, -EIO);
	}
}

static void
ftl_nv_cache_scrub_header_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_nv_cache_restore *restore = cb_arg;
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
	int rc;

	spdk_bdev_free_io(bdev_io);
	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Unable to write non-volatile cache metadata header\n");
		ftl_nv_cache_restore_complete(restore, -EIO);
		return;
	}

	rc = ftl_nv_cache_scrub(nv_cache, ftl_nv_cache_scrub_cb, restore);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Unable to scrub the non-volatile cache: %s\n", spdk_strerror(-rc));
		ftl_nv_cache_restore_complete(restore, rc);
	}
}

static void
ftl_nv_cache_band_flush_cb(void *ctx, int status)
{
	struct ftl_nv_cache_restore *restore = ctx;
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
	int rc;

	if (spdk_unlikely(status != 0)) {
		SPDK_ERRLOG("Flushing active bands failed: %s\n", spdk_strerror(-status));
		ftl_nv_cache_restore_complete(restore, status);
		return;
	}

	/*
	 * Use phase 0 to indicate that the cache is being scrubbed. If the power is lost during
	 * this process, we'll know it needs to be resumed.
	 */
	nv_cache->phase = 0;
	rc = ftl_nv_cache_write_header(nv_cache, false, ftl_nv_cache_scrub_header_cb, restore);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Unable to write non-volatile cache metadata header: %s\n",
			    spdk_strerror(-rc));
		ftl_nv_cache_restore_complete(restore, rc);
	}
}

static void
ftl_nv_cache_wbuf_flush_cb(void *ctx, int status)
{
	struct ftl_nv_cache_restore *restore = ctx;
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	int rc;

	if (spdk_unlikely(status != 0)) {
		SPDK_ERRLOG("Flushing the write buffer failed: %s\n", spdk_strerror(-status));
		ftl_nv_cache_restore_complete(restore, status);
		return;
	}

	rc = ftl_flush_active_bands(dev, ftl_nv_cache_band_flush_cb, restore);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Unable to flush active bands: %s\n", spdk_strerror(-rc));
		ftl_nv_cache_restore_complete(restore, rc);
	}
}

static void
ftl_nv_cache_recovery_done(struct ftl_nv_cache_restore *restore)
{
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
	struct ftl_nv_cache_range *range_prev, *range_current;
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct spdk_bdev *bdev;
	uint64_t current_addr;
	int rc;

	range_prev = &restore->range[ftl_nv_cache_prev_phase(nv_cache->phase)];
	range_current = &restore->range[nv_cache->phase];
	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);

	/*
	 * If there are more than two ranges or the ranges overlap, scrub the non-volatile cache to
	 * make sure that any subsequent power loss will find the cache in usable state
	 */
	if ((range_prev->num_blocks + range_current->num_blocks < nv_cache->num_data_blocks) ||
	    (range_prev->start_addr < range_current->last_addr &&
	     range_current->start_addr < range_prev->last_addr)) {
		SPDK_DEBUGLOG(ftl_init, "Non-volatile cache inconsistency detected\n");

		rc = ftl_flush_wbuf(dev, ftl_nv_cache_wbuf_flush_cb, restore);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Unable to flush the write buffer: %s\n", spdk_strerror(-rc));
			ftl_nv_cache_restore_complete(restore, rc);
		}

		return;
	}

	/* The latest phase is the one written in the header (set in nvc_cache->phase) */
	current_addr = range_current->last_addr + 1;

	/*
	 * The first range might be empty (only the header was written) or the range might
	 * end at the last available address, in which case set current address to the
	 * beginning of the device.
	 */
	if (range_current->num_blocks == 0 || current_addr >= spdk_bdev_get_num_blocks(bdev)) {
		current_addr = FTL_NV_CACHE_DATA_OFFSET;
	}

	ftl_nv_cache_restore_done(restore, current_addr);
}

static void
ftl_nv_cache_recover_block(struct ftl_nv_cache_block *block)
{
	struct ftl_nv_cache_restore *restore = block->parent;
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
	struct ftl_nv_cache_range *range = &restore->range[restore->phase];
	int rc;

	assert(range->current_addr <= range->last_addr);

	restore->num_outstanding++;
	block->offset = range->current_addr++;
	rc = spdk_bdev_read_blocks_with_md(nv_cache->bdev_desc, restore->ioch,
					   block->buf, block->md_buf,
					   block->offset, 1, ftl_nv_cache_block_read_cb,
					   block);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Non-volatile cache restoration failed on block %"PRIu64" (%s)\n",
			    block->offset, spdk_strerror(-rc));
		restore->num_outstanding--;
		ftl_nv_cache_restore_complete(restore, rc);
	}
}

static void
ftl_nv_cache_recover_range(struct ftl_nv_cache_restore *restore)
{
	struct ftl_nv_cache_range *range;
	unsigned int phase = restore->phase;

	do {
		/* Find first range with non-zero number of blocks that is marked for recovery */
		range = &restore->range[phase];
		if (range->recovery && range->num_recovered < range->num_blocks) {
			break;
		}

		phase = ftl_nv_cache_next_phase(phase);
	} while (phase != restore->phase);

	/* There are no ranges to be recovered, we're done */
	if (range->num_recovered == range->num_blocks || !range->recovery) {
		SPDK_DEBUGLOG(ftl_init, "Non-volatile cache recovery done\n");
		ftl_nv_cache_recovery_done(restore);
		return;
	}

	range->current_addr = range->start_addr;
	restore->phase = phase;

	SPDK_DEBUGLOG(ftl_init, "Recovering range %u %"PRIu64"-%"PRIu64" (%"PRIu64")\n",
		      phase, range->start_addr, range->last_addr, range->num_blocks);

	ftl_nv_cache_recover_block(&restore->block[0]);
}

static void
ftl_nv_cache_write_cb(struct ftl_io *io, void *cb_arg, int status)
{
	struct ftl_nv_cache_block *block = cb_arg;
	struct ftl_nv_cache_restore *restore = block->parent;
	struct ftl_nv_cache_range *range = &restore->range[restore->phase];

	restore->num_outstanding--;
	if (status != 0) {
		SPDK_ERRLOG("Non-volatile cache restoration failed on block %"PRIu64" (%s)\n",
			    block->offset, spdk_strerror(-status));
		ftl_nv_cache_restore_complete(restore, -ENOMEM);
		return;
	}

	range->num_recovered++;
	if (range->current_addr <= range->last_addr) {
		ftl_nv_cache_recover_block(block);
	} else if (restore->num_outstanding == 0) {
		assert(range->num_recovered == range->num_blocks);
		ftl_nv_cache_recover_range(restore);
	}
}

static struct ftl_io *
ftl_nv_cache_alloc_io(struct ftl_nv_cache_block *block, uint64_t lba)
{
	struct ftl_restore *restore = SPDK_CONTAINEROF(block->parent, struct ftl_restore, nv_cache);
	struct ftl_io_init_opts opts = {
		.dev		= restore->dev,
		.io		= NULL,
		.flags		= FTL_IO_BYPASS_CACHE,
		.type		= FTL_IO_WRITE,
		.num_blocks	= 1,
		.cb_fn		= ftl_nv_cache_write_cb,
		.cb_ctx		= block,
		.iovs		= {
			{
				.iov_base = block->buf,
				.iov_len = FTL_BLOCK_SIZE,
			}
		},
		.iovcnt		= 1,
	};
	struct ftl_io *io;

	io = ftl_io_init_internal(&opts);
	if (spdk_unlikely(!io)) {
		return NULL;
	}

	io->lba.single = lba;
	return io;
}

static void
ftl_nv_cache_block_read_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_nv_cache_block *block = cb_arg;
	struct ftl_nv_cache_restore *restore = block->parent;
	struct ftl_nv_cache_range *range = &restore->range[restore->phase];
	struct ftl_io *io;
	unsigned int phase;
	uint64_t lba;

	spdk_bdev_free_io(bdev_io);
	restore->num_outstanding--;

	if (!success) {
		SPDK_ERRLOG("Non-volatile cache restoration failed on block %"PRIu64"\n",
			    block->offset);
		ftl_nv_cache_restore_complete(restore, -EIO);
		return;
	}

	ftl_nv_cache_unpack_lba(*(uint64_t *)block->md_buf, &lba, &phase);
	if (spdk_unlikely(phase != restore->phase)) {
		if (range->current_addr < range->last_addr) {
			ftl_nv_cache_recover_block(block);
		} else if (restore->num_outstanding == 0) {
			ftl_nv_cache_recover_range(restore);
		}

		return;
	}

	io = ftl_nv_cache_alloc_io(block, lba);
	if (spdk_unlikely(!io)) {
		SPDK_ERRLOG("Failed to allocate ftl_io during non-volatile cache recovery\n");
		ftl_nv_cache_restore_complete(restore, -ENOMEM);
		return;
	}

	restore->num_outstanding++;
	ftl_io_write(io);
}

/*
 * Since we have no control over the order in which the requests complete in regards to their
 * submission, the cache can be in either of the following states:
 *  - [1 1 1 1 1 1 1 1 1 1]: simplest case, whole cache contains single phase (although it should be
 *			     very rare),
 *  - [1 1 1 1 3 3 3 3 3 3]: two phases, changing somewhere in the middle with no overlap. This is
 *			     the state left by clean shutdown,
 *  - [1 1 1 1 3 1 3 3 3 3]: similar to the above, but this time the two ranges overlap. This
 *			     happens when completions are reordered during unsafe shutdown,
 *  - [2 1 2 1 1 1 1 3 1 3]: three different phases, each one of which can overlap with
 *			     previous/next one. The data from the oldest phase doesn't need to be
 *			     recovered, as it was already being written to, which means it's
 *			     already on the main storage.
 */
static void
ftl_nv_cache_scan_done(struct ftl_nv_cache_restore *restore)
{
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
#if defined(DEBUG)
	struct ftl_nv_cache_range *range;
	uint64_t i, num_blocks = 0;

	for (i = 0; i < FTL_NV_CACHE_PHASE_COUNT; ++i) {
		range = &restore->range[i];
		SPDK_DEBUGLOG(ftl_init, "Range %"PRIu64": %"PRIu64"-%"PRIu64" (%" PRIu64
			      ")\n", i, range->start_addr, range->last_addr, range->num_blocks);
		num_blocks += range->num_blocks;
	}
	assert(num_blocks == nv_cache->num_data_blocks);
#endif
	restore->phase = ftl_nv_cache_prev_phase(nv_cache->phase);

	/*
	 * Only the latest two phases need to be recovered. The third one, even if present,
	 * already has to be stored on the main storage, as it's already started to be
	 * overwritten (only present here because of reordering of requests' completions).
	 */
	restore->range[nv_cache->phase].recovery = true;
	restore->range[restore->phase].recovery = true;

	ftl_nv_cache_recover_range(restore);
}

static int ftl_nv_cache_scan_block(struct ftl_nv_cache_block *block);

static void
ftl_nv_cache_scan_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_nv_cache_block *block = cb_arg;
	struct ftl_nv_cache_restore *restore = block->parent;
	struct ftl_nv_cache_range *range;
	struct spdk_bdev *bdev;
	unsigned int phase;
	uint64_t lba;

	restore->num_outstanding--;
	bdev = spdk_bdev_desc_get_bdev(restore->nv_cache->bdev_desc);
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		SPDK_ERRLOG("Non-volatile cache scan failed on block %"PRIu64"\n",
			    block->offset);
		ftl_nv_cache_restore_complete(restore, -EIO);
		return;
	}

	/* If we've already hit an error, don't bother with scanning anything else */
	if (spdk_unlikely(restore->status != 0)) {
		ftl_nv_cache_restore_complete(restore, restore->status);
		return;
	}

	ftl_nv_cache_unpack_lba(*(uint64_t *)block->md_buf, &lba, &phase);
	range = &restore->range[phase];
	range->num_blocks++;

	if (range->start_addr == FTL_LBA_INVALID || range->start_addr > block->offset) {
		range->start_addr = block->offset;
	}

	if (range->last_addr == FTL_LBA_INVALID || range->last_addr < block->offset) {
		range->last_addr = block->offset;
	}

	/* All the blocks were read, once they're all completed and we're finished */
	if (restore->current_addr == spdk_bdev_get_num_blocks(bdev)) {
		if (restore->num_outstanding == 0) {
			ftl_nv_cache_scan_done(restore);
		}

		return;
	}

	ftl_nv_cache_scan_block(block);
}

static int
ftl_nv_cache_scan_block(struct ftl_nv_cache_block *block)
{
	struct ftl_nv_cache_restore *restore = block->parent;
	struct ftl_nv_cache *nv_cache = restore->nv_cache;
	int rc;

	restore->num_outstanding++;
	block->offset = restore->current_addr++;
	rc = spdk_bdev_read_blocks_with_md(nv_cache->bdev_desc, restore->ioch,
					   block->buf, block->md_buf,
					   block->offset, 1, ftl_nv_cache_scan_cb,
					   block);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Non-volatile cache scan failed on block %"PRIu64" (%s)\n",
			    block->offset, spdk_strerror(-rc));
		restore->num_outstanding--;
		ftl_nv_cache_restore_complete(restore, rc);
		return rc;
	}

	return 0;
}

static void
ftl_nv_cache_clean_header_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_nv_cache_restore *restore = cb_arg;

	spdk_bdev_free_io(bdev_io);
	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Unable to write the non-volatile cache metadata header\n");
		ftl_nv_cache_restore_complete(restore, -EIO);
		return;
	}

	ftl_nv_cache_restore_done(restore, restore->current_addr);
}

static bool
ftl_nv_cache_header_valid(struct spdk_ftl_dev *dev, const struct ftl_nv_cache_header *hdr)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	uint32_t checksum;

	checksum = spdk_crc32c_update(hdr, offsetof(struct ftl_nv_cache_header, checksum), 0);
	if (checksum != hdr->checksum) {
		SPDK_ERRLOG("Invalid header checksum (found: %"PRIu32", expected: %"PRIu32")\n",
			    checksum, hdr->checksum);
		return false;
	}

	if (hdr->version != FTL_NV_CACHE_HEADER_VERSION) {
		SPDK_ERRLOG("Invalid header version (found: %"PRIu32", expected: %"PRIu32")\n",
			    hdr->version, FTL_NV_CACHE_HEADER_VERSION);
		return false;
	}

	if (hdr->size != spdk_bdev_get_num_blocks(bdev)) {
		SPDK_ERRLOG("Unexpected size of the non-volatile cache bdev (%"PRIu64", expected: %"
			    PRIu64")\n", hdr->size, spdk_bdev_get_num_blocks(bdev));
		return false;
	}

	if (spdk_uuid_compare(&hdr->uuid, &dev->uuid)) {
		SPDK_ERRLOG("Invalid device UUID\n");
		return false;
	}

	if (!ftl_nv_cache_phase_is_valid(hdr->phase) && hdr->phase != 0) {
		return false;
	}

	if ((hdr->current_addr >= spdk_bdev_get_num_blocks(bdev) ||
	     hdr->current_addr  < FTL_NV_CACHE_DATA_OFFSET) &&
	    (hdr->current_addr != FTL_LBA_INVALID)) {
		SPDK_ERRLOG("Unexpected value of non-volatile cache's current address: %"PRIu64"\n",
			    hdr->current_addr);
		return false;
	}

	return true;
}

static void
ftl_nv_cache_read_header_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_restore *restore = cb_arg;
	struct spdk_ftl_dev *dev = restore->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_nv_cache_header *hdr;
	struct iovec *iov = NULL;
	int iov_cnt = 0, i, rc;

	if (!success) {
		SPDK_ERRLOG("Unable to read non-volatile cache metadata header\n");
		ftl_restore_complete(restore, -ENOTRECOVERABLE);
		goto out;
	}

	spdk_bdev_io_get_iovec(bdev_io, &iov, &iov_cnt);
	assert(iov != NULL);
	hdr = iov[0].iov_base;

	if (!ftl_nv_cache_header_valid(dev, hdr)) {
		ftl_restore_complete(restore, -ENOTRECOVERABLE);
		goto out;
	}

	/* Remember the latest phase */
	nv_cache->phase = hdr->phase;

	/* If the phase equals zero, we lost power during recovery. We need to finish it up
	 * by scrubbing the device once again.
	 */
	if (hdr->phase == 0) {
		SPDK_DEBUGLOG(ftl_init, "Detected phase 0, restarting scrub\n");
		rc = ftl_nv_cache_scrub(nv_cache, ftl_nv_cache_scrub_cb, restore);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Unable to scrub the non-volatile cache: %s\n",
				    spdk_strerror(-rc));
			ftl_restore_complete(restore, -ENOTRECOVERABLE);
		}

		goto out;
	}

	/* Valid current_addr means that the shutdown was clean, so we just need to overwrite the
	 * header to make sure that any power loss occurring before the cache is wrapped won't be
	 * mistaken for a clean shutdown.
	 */
	if (hdr->current_addr != FTL_LBA_INVALID) {
		restore->nv_cache.current_addr = hdr->current_addr;

		rc = ftl_nv_cache_write_header(nv_cache, false, ftl_nv_cache_clean_header_cb,
					       &restore->nv_cache);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to overwrite the non-volatile cache header: %s\n",
				    spdk_strerror(-rc));
			ftl_restore_complete(restore, -ENOTRECOVERABLE);
		}

		goto out;
	}

	/* Otherwise the shutdown was unexpected, so we need to recover the data from the cache */
	restore->nv_cache.current_addr = FTL_NV_CACHE_DATA_OFFSET;

	for (i = 0; i < FTL_NV_CACHE_RESTORE_DEPTH; ++i) {
		if (ftl_nv_cache_scan_block(&restore->nv_cache.block[i])) {
			break;
		}
	}
out:
	spdk_bdev_free_io(bdev_io);
}

void
ftl_restore_nv_cache(struct ftl_restore *restore, ftl_restore_fn cb, void *cb_arg)
{
	struct spdk_ftl_dev *dev = restore->dev;
	struct spdk_bdev *bdev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_io_channel *ioch;
	struct ftl_nv_cache_restore *nvc_restore = &restore->nv_cache;
	struct ftl_nv_cache_block *block;
	size_t alignment;
	int rc, i;

	ioch = ftl_io_channel_get_ctx(ftl_get_io_channel(dev));
	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
	alignment = spdk_max(spdk_bdev_get_buf_align(bdev), sizeof(uint64_t));

	nvc_restore->nv_cache = nv_cache;
	nvc_restore->ioch = ioch->cache_ioch;

	restore->final_phase = true;
	restore->cb = cb;
	restore->cb_arg = cb_arg;

	for (i = 0; i < FTL_NV_CACHE_RESTORE_DEPTH; ++i) {
		block = &nvc_restore->block[i];
		block->parent = nvc_restore;
		block->buf = spdk_dma_zmalloc(spdk_bdev_get_block_size(bdev) +
					      spdk_bdev_get_md_size(bdev),
					      alignment, NULL);
		if (!block->buf) {
			/* The memory will be freed in ftl_restore_free */
			SPDK_ERRLOG("Unable to allocate memory\n");
			ftl_restore_complete(restore, -ENOMEM);
			return;
		}

		block->md_buf = (char *)block->buf + spdk_bdev_get_block_size(bdev);
	}

	for (i = 0; i < FTL_NV_CACHE_PHASE_COUNT; ++i) {
		nvc_restore->range[i].parent = nvc_restore;
		nvc_restore->range[i].start_addr = FTL_LBA_INVALID;
		nvc_restore->range[i].last_addr = FTL_LBA_INVALID;
		nvc_restore->range[i].num_blocks = 0;
		nvc_restore->range[i].recovery = false;
		nvc_restore->range[i].phase = i;
	}

	rc = spdk_bdev_read_blocks(nv_cache->bdev_desc, ioch->cache_ioch, nv_cache->dma_buf,
				   0, FTL_NV_CACHE_DATA_OFFSET, ftl_nv_cache_read_header_cb, restore);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to read non-volatile cache metadata header: %s\n",
			    spdk_strerror(-rc));
		ftl_restore_complete(restore, rc);
	}
}

static bool
ftl_pad_zone_pad_finish(struct ftl_restore_band *rband, bool direct_access)
{
	struct ftl_restore *restore = rband->parent;
	struct ftl_restore_band *next_band;
	size_t i, num_pad_zones = 0;

	if (spdk_unlikely(restore->pad_status && !restore->num_ios)) {
		if (direct_access) {
			/* In case of any errors found we want to clear direct access. */
			/* Direct access bands have their own allocated md, which would be lost */
			/* on restore complete otherwise. */
			rband->band->state = FTL_BAND_STATE_CLOSED;
			ftl_band_set_direct_access(rband->band, false);
		}
		ftl_restore_complete(restore, restore->pad_status);
		return true;
	}

	for (i = 0; i < rband->band->num_zones; ++i) {
		if (rband->band->zone_buf[i].info.state != SPDK_BDEV_ZONE_STATE_FULL) {
			num_pad_zones++;
		}
	}

	/* Finished all zones in a band, check if all bands are done */
	if (num_pad_zones == 0) {
		if (direct_access) {
			rband->band->state = FTL_BAND_STATE_CLOSED;
			ftl_band_set_direct_access(rband->band, false);
		}

		next_band = STAILQ_NEXT(rband, stailq);
		if (!next_band) {
			ftl_restore_complete(restore, restore->pad_status);
			return true;
		} else {
			/* Start off padding in the next band */
			ftl_restore_pad_band(next_band);
			return true;
		}
	}

	return false;
}

static struct ftl_io *
ftl_restore_init_pad_io(struct ftl_restore_band *rband, void *buffer,
			struct ftl_addr addr)
{
	struct ftl_band *band = rband->band;
	struct spdk_ftl_dev *dev = band->dev;
	int flags = FTL_IO_PAD | FTL_IO_INTERNAL | FTL_IO_PHYSICAL_MODE | FTL_IO_MD |
		    FTL_IO_DIRECT_ACCESS;
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= flags,
		.type		= FTL_IO_WRITE,
		.num_blocks	= dev->xfer_size,
		.cb_fn		= ftl_pad_zone_cb,
		.cb_ctx		= rband,
		.iovs		= {
			{
				.iov_base = buffer,
				.iov_len = dev->xfer_size * FTL_BLOCK_SIZE,
			}
		},
		.iovcnt		= 1,
		.parent		= NULL,
	};
	struct ftl_io *io;

	io = ftl_io_init_internal(&opts);
	if (spdk_unlikely(!io)) {
		return NULL;
	}

	io->addr = addr;
	rband->parent->num_ios++;

	return io;
}

static void
ftl_pad_zone_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_restore_band *rband = arg;
	struct ftl_restore *restore = rband->parent;
	struct ftl_band *band = io->band;
	struct ftl_zone *zone;
	struct ftl_io *new_io;
	uint64_t offset;

	restore->num_ios--;
	/* TODO check for next unit error vs early close error */
	if (status) {
		restore->pad_status = status;
		goto end;
	}

	offset = io->addr.offset % ftl_get_num_blocks_in_zone(restore->dev);
	if (offset + io->num_blocks == ftl_get_num_blocks_in_zone(restore->dev)) {
		zone = ftl_band_zone_from_addr(band, io->addr);
		zone->info.state = SPDK_BDEV_ZONE_STATE_FULL;
	} else {
		struct ftl_addr addr = io->addr;
		addr.offset += io->num_blocks;
		new_io = ftl_restore_init_pad_io(rband, io->iov[0].iov_base, addr);
		if (spdk_unlikely(!new_io)) {
			restore->pad_status = -ENOMEM;
			goto end;
		}

		ftl_io_write(new_io);
		return;
	}

end:
	spdk_dma_free(io->iov[0].iov_base);
	ftl_pad_zone_pad_finish(rband, true);
}

static void
ftl_restore_pad_band(struct ftl_restore_band *rband)
{
	struct ftl_restore *restore = rband->parent;
	struct ftl_band *band = rband->band;
	struct spdk_ftl_dev *dev = band->dev;
	void *buffer = NULL;
	struct ftl_io *io;
	struct ftl_addr addr;
	size_t i;
	int rc = 0;

	/* Check if some zones are not closed */
	if (ftl_pad_zone_pad_finish(rband, false)) {
		/*
		 * If we're here, end meta wasn't recognized, but the whole band is written
		 * Assume the band was padded and ignore it
		 */
		return;
	}

	band->state = FTL_BAND_STATE_OPEN;
	rc = ftl_band_set_direct_access(band, true);
	if (rc) {
		ftl_restore_complete(restore, rc);
		return;
	}

	for (i = 0; i < band->num_zones; ++i) {
		if (band->zone_buf[i].info.state == SPDK_BDEV_ZONE_STATE_FULL) {
			continue;
		}

		addr.offset = band->zone_buf[i].info.write_pointer;

		buffer = spdk_dma_zmalloc(FTL_BLOCK_SIZE * dev->xfer_size, 0, NULL);
		if (spdk_unlikely(!buffer)) {
			rc = -ENOMEM;
			goto error;
		}

		io = ftl_restore_init_pad_io(rband, buffer, addr);
		if (spdk_unlikely(!io)) {
			rc = -ENOMEM;
			spdk_dma_free(buffer);
			goto error;
		}

		ftl_io_write(io);
	}

	return;

error:
	restore->pad_status = rc;
	ftl_pad_zone_pad_finish(rband, true);
}

static void
ftl_restore_pad_open_bands(void *ctx)
{
	struct ftl_restore *restore = ctx;

	ftl_restore_pad_band(STAILQ_FIRST(&restore->pad_bands));
}

static void
ftl_restore_tail_md_cb(struct ftl_io *io, void *ctx, int status)
{
	struct ftl_restore_band *rband = ctx;
	struct ftl_restore *restore = rband->parent;
	struct spdk_ftl_dev *dev = restore->dev;

	if (status) {
		if (!dev->conf.allow_open_bands) {
			SPDK_ERRLOG("%s while restoring tail md in band %u.\n",
				    spdk_strerror(-status), rband->band->id);
			ftl_band_release_lba_map(rband->band);
			ftl_restore_complete(restore, status);
			return;
		} else {
			SPDK_ERRLOG("%s while restoring tail md. Will attempt to pad band %u.\n",
				    spdk_strerror(-status), rband->band->id);
			STAILQ_INSERT_TAIL(&restore->pad_bands, rband, stailq);
		}
	}

	if (!status && ftl_restore_l2p(rband->band)) {
		ftl_band_release_lba_map(rband->band);
		ftl_restore_complete(restore, -ENOTRECOVERABLE);
		return;
	}
	ftl_band_release_lba_map(rband->band);

	rband = ftl_restore_next_band(restore);
	if (!rband) {
		if (!STAILQ_EMPTY(&restore->pad_bands)) {
			spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_restore_pad_open_bands,
					     restore);
		} else {
			ftl_restore_complete(restore, 0);
		}

		return;
	}

	ftl_restore_tail_md(rband);
}

static int
ftl_restore_tail_md(struct ftl_restore_band *rband)
{
	struct ftl_restore *restore = rband->parent;
	struct ftl_band *band = rband->band;

	if (ftl_band_alloc_lba_map(band)) {
		SPDK_ERRLOG("Failed to allocate lba map\n");
		ftl_restore_complete(restore, -ENOMEM);
		return -ENOMEM;
	}

	if (ftl_band_read_tail_md(band, band->tail_md_addr, ftl_restore_tail_md_cb, rband)) {
		SPDK_ERRLOG("Failed to send tail metadata read\n");
		ftl_restore_complete(restore, -EIO);
		return -EIO;
	}

	return 0;
}

int
ftl_restore_device(struct ftl_restore *restore, ftl_restore_fn cb, void *cb_arg)
{
	struct spdk_ftl_dev *dev = restore->dev;
	struct ftl_restore_band *rband;

	restore->current = 0;
	restore->cb = cb;
	restore->cb_arg = cb_arg;
	restore->final_phase = dev->nv_cache.bdev_desc == NULL;

	/* If restore_device is called, there must be at least one valid band */
	rband = ftl_restore_next_band(restore);
	assert(rband);
	return ftl_restore_tail_md(rband);
}
