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

struct ftl_restore {
	struct spdk_ftl_dev		*dev;
	/* Completion callback (called for each phase of the restoration) */
	ftl_restore_fn			cb;
	/* Number of inflight IOs */
	unsigned int			num_ios;
	/* Current band number (index in the below bands array) */
	unsigned int			current;
	/* Array of bands */
	struct ftl_restore_band		*bands;
	/* Queue of bands to be padded (due to unsafe shutdown) */
	STAILQ_HEAD(, ftl_restore_band) pad_bands;
	/* Number of yet to be padded bands */
	size_t				num_pad_bands;
	/* Status of the padding */
	int				pad_status;
	/* Metadata buffer */
	void				*md_buf;
	/* LBA map buffer */
	void				*lba_map;
	/* Indicates we're in the final phase of the restoration */
	bool				l2p_phase;
};

static int
ftl_restore_tail_md(struct ftl_restore_band *rband);
static void
ftl_pad_chunk_cb(struct ftl_io *io, void *arg, int status);
static void
ftl_restore_pad_band(struct ftl_restore_band *rband);

static void
ftl_restore_free(struct ftl_restore *restore)
{
	if (!restore) {
		return;
	}

	spdk_dma_free(restore->md_buf);
	free(restore->lba_map);
	free(restore->bands);
	free(restore);
}

static struct ftl_restore *
ftl_restore_init(struct spdk_ftl_dev *dev, ftl_restore_fn cb)
{
	struct ftl_restore *restore;
	struct ftl_restore_band *rband;
	size_t i, md_size;

	restore = calloc(1, sizeof(*restore));
	if (!restore) {
		goto error;
	}

	restore->dev = dev;
	restore->cb = cb;
	restore->l2p_phase = false;

	restore->bands = calloc(ftl_dev_num_bands(dev), sizeof(*restore->bands));
	if (!restore->bands) {
		goto error;
	}

	STAILQ_INIT(&restore->pad_bands);

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		rband = &restore->bands[i];
		rband->band = &dev->bands[i];
		rband->parent = restore;
		rband->md_status = FTL_MD_NO_MD;
	}

	/* Allocate buffer capable of holding either tail md or head mds of all bands */
	md_size = spdk_max(ftl_dev_num_bands(dev) * ftl_head_md_num_lbks(dev) * FTL_BLOCK_SIZE,
			   ftl_tail_md_num_lbks(dev) * FTL_BLOCK_SIZE);

	restore->md_buf = spdk_dma_zmalloc(md_size, FTL_BLOCK_SIZE, NULL);
	if (!restore->md_buf) {
		goto error;
	}

	restore->lba_map = calloc(ftl_num_band_lbks(dev), sizeof(uint64_t));
	if (!restore->lba_map) {
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
	bool l2p_phase = restore->l2p_phase;

	restore->cb(restore->dev, ctx, status);
	if (status || l2p_phase) {
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

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
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

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
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
	qsort(restore->bands, ftl_dev_num_bands(dev), sizeof(struct ftl_restore_band),
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

static int
ftl_restore_head_md(struct ftl_restore *restore)
{
	struct spdk_ftl_dev *dev = restore->dev;
	struct ftl_restore_band *rband;
	struct ftl_lba_map *lba_map;
	unsigned int num_failed = 0, num_ios;
	size_t i;

	restore->num_ios = ftl_dev_num_bands(dev);

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		rband = &restore->bands[i];
		lba_map = &rband->band->lba_map;

		lba_map->dma_buf = restore->md_buf + i * ftl_head_md_num_lbks(dev) * FTL_BLOCK_SIZE;

		if (ftl_band_read_head_md(rband->band, ftl_restore_head_cb, rband)) {
			if (spdk_likely(rband->band->num_chunks)) {
				SPDK_ERRLOG("Failed to read metadata on band %zu\n", i);

				rband->md_status = FTL_MD_INVALID_CRC;

				/* If the first IO fails, don't bother sending anything else */
				if (i == 0) {
					ftl_restore_free(restore);
					return -EIO;
				}
			}

			num_failed++;
		}
	}

	if (spdk_unlikely(num_failed > 0)) {
		num_ios = __atomic_fetch_sub(&restore->num_ios, num_failed, __ATOMIC_SEQ_CST);
		if (num_ios == num_failed) {
			ftl_restore_free(restore);
			return -EIO;
		}
	}

	return 0;
}

int
ftl_restore_md(struct spdk_ftl_dev *dev, ftl_restore_fn cb)
{
	struct ftl_restore *restore;

	restore = ftl_restore_init(dev, cb);
	if (!restore) {
		return -ENOMEM;
	}

	return ftl_restore_head_md(restore);
}

static int
ftl_restore_l2p(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_ppa ppa;
	uint64_t lba;
	size_t i;

	for (i = 0; i < ftl_num_band_lbks(band->dev); ++i) {
		if (!spdk_bit_array_get(band->lba_map.vld, i)) {
			continue;
		}

		lba = band->lba_map.map[i];
		if (lba >= dev->num_lbas) {
			return -1;
		}

		ppa = ftl_l2p_get(dev, lba);
		if (!ftl_ppa_invalid(ppa)) {
			ftl_invalidate_addr(dev, ppa);
		}

		ppa = ftl_band_ppa_from_lbkoff(band, i);

		ftl_band_set_addr(band, lba, ppa);
		ftl_l2p_set(dev, lba, ppa);
	}

	band->lba_map.map = NULL;
	return 0;
}

static struct ftl_restore_band *
ftl_restore_next_band(struct ftl_restore *restore)
{
	struct ftl_restore_band *rband;

	for (; restore->current < ftl_dev_num_bands(restore->dev); ++restore->current) {
		rband = &restore->bands[restore->current];

		if (spdk_likely(rband->band->num_chunks) &&
		    rband->md_status == FTL_MD_SUCCESS) {
			restore->current++;
			return rband;
		}
	}

	return NULL;
}

static void
ftl_nv_cache_header_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_restore *restore = cb_arg;
	struct spdk_ftl_dev *dev = restore->dev;
	struct spdk_bdev *bdev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_nv_cache_header *hdr;
	struct iovec *iov = NULL;
	int rc = 0, iov_cnt = 0;

	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
	spdk_bdev_io_get_iovec(bdev_io, &iov, &iov_cnt);
	hdr = iov[0].iov_base;

	if (!success) {
		SPDK_ERRLOG("Unable to read non-volatile cache metadata header\n");
		rc = -ENOTRECOVERABLE;
		goto out;
	}

	if (hdr->size != spdk_bdev_get_num_blocks(bdev)) {
		SPDK_ERRLOG("Unexpected size of the non-volatile cache bdev (%"PRIu64", expected: %"
			    PRIu64")\n", hdr->size, spdk_bdev_get_num_blocks(bdev));
		rc = -ENOTRECOVERABLE;
		goto out;
	}

	if (spdk_uuid_compare(&hdr->uuid, &dev->uuid)) {
		SPDK_ERRLOG("Invalid device UUID\n");
		rc = -ENOTRECOVERABLE;
		goto out;
	}

	if (!ftl_nv_cache_phase_is_valid(hdr->phase)) {
		SPDK_ERRLOG("Invalid phase of the non-volatile cache (%u)\n", hdr->phase);
		rc = -ENOTRECOVERABLE;
		goto out;
	}

	pthread_spin_lock(&nv_cache->lock);
	nv_cache->ready = true;
	pthread_spin_unlock(&nv_cache->lock);
out:
	ftl_restore_complete(restore, rc);
	spdk_bdev_free_io(bdev_io);
	spdk_dma_free(hdr);
}

static void
ftl_restore_nv_cache(struct ftl_restore *restore)
{
	struct spdk_ftl_dev *dev = restore->dev;
	struct spdk_bdev *bdev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_io_channel *ioch;
	void *buf;
	int rc;

	ioch = spdk_io_channel_get_ctx(dev->ioch);
	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);

	buf = spdk_dma_zmalloc(spdk_bdev_get_block_size(bdev), spdk_bdev_get_buf_align(bdev), NULL);
	if (spdk_unlikely(!buf)) {
		SPDK_ERRLOG("Memory allocation failure\n");
		ftl_restore_complete(restore, -ENOMEM);
		return;
	}

	rc = spdk_bdev_read_blocks(nv_cache->bdev_desc, ioch->cache_ioch, buf, 0, 1,
				   ftl_nv_cache_header_cb, restore);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to read non-volatile cache metadata header: %s\n",
			    spdk_strerror(-rc));
		ftl_restore_complete(restore, rc);
		spdk_dma_free(buf);
	}
}

static bool
ftl_pad_chunk_pad_finish(struct ftl_restore_band *rband, bool direct_access)
{
	struct ftl_restore *restore = rband->parent;
	size_t i, num_pad_chunks = 0;

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

	for (i = 0; i < rband->band->num_chunks; ++i) {
		if (rband->band->chunk_buf[i].state != FTL_CHUNK_STATE_CLOSED) {
			num_pad_chunks++;
		}
	}

	/* Finished all chunks in a band, check if all bands are done */
	if (num_pad_chunks == 0) {
		if (direct_access) {
			rband->band->state = FTL_BAND_STATE_CLOSED;
			ftl_band_set_direct_access(rband->band, false);
		}
		if (--restore->num_pad_bands == 0) {
			ftl_restore_complete(restore, restore->pad_status);
			return true;
		} else {
			/* Start off padding in the next band */
			ftl_restore_pad_band(STAILQ_NEXT(rband, stailq));
			return true;
		}
	}

	return false;
}

static struct ftl_io *
ftl_restore_init_pad_io(struct ftl_restore_band *rband, void *buffer,
			struct ftl_ppa ppa)
{
	struct ftl_band *band = rband->band;
	struct spdk_ftl_dev *dev = band->dev;
	int flags = FTL_IO_PAD | FTL_IO_INTERNAL | FTL_IO_PPA_MODE | FTL_IO_MD |
		    FTL_IO_DIRECT_ACCESS;
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= NULL,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= flags,
		.type		= FTL_IO_WRITE,
		.lbk_cnt	= dev->xfer_size,
		.cb_fn		= ftl_pad_chunk_cb,
		.cb_ctx		= rband,
		.data		= buffer,
		.parent		= NULL,
	};
	struct ftl_io *io;

	io = ftl_io_init_internal(&opts);
	if (spdk_unlikely(!io)) {
		return NULL;
	}

	io->ppa = ppa;
	rband->parent->num_ios++;

	return io;
}

static void
ftl_pad_chunk_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_restore_band *rband = arg;
	struct ftl_restore *restore = rband->parent;
	struct ftl_band *band = io->band;
	struct ftl_chunk *chunk;
	struct ftl_io *new_io;

	restore->num_ios--;
	/* TODO check for next unit error vs early close error */
	if (status) {
		restore->pad_status = status;
		goto end;
	}

	if (io->ppa.lbk + io->lbk_cnt == band->dev->geo.clba) {
		chunk = ftl_band_chunk_from_ppa(band, io->ppa);
		chunk->state = FTL_CHUNK_STATE_CLOSED;
	} else {
		struct ftl_ppa ppa = io->ppa;
		ppa.lbk += io->lbk_cnt;
		new_io = ftl_restore_init_pad_io(rband, io->iov[0].iov_base, ppa);
		if (spdk_unlikely(!new_io)) {
			restore->pad_status = -ENOMEM;
			goto end;
		}

		ftl_io_write(new_io);
		return;
	}

end:
	spdk_dma_free(io->iov[0].iov_base);
	ftl_pad_chunk_pad_finish(rband, true);
}

static void
ftl_restore_pad_band(struct ftl_restore_band *rband)
{
	struct spdk_ocssd_chunk_information_entry info;
	struct ftl_restore *restore = rband->parent;
	struct ftl_band *band = rband->band;
	struct spdk_ftl_dev *dev = band->dev;
	void *buffer = NULL;
	struct ftl_io *io;
	struct ftl_ppa ppa;
	size_t i;
	int rc = 0;

	/* Check if some chunks are not closed */
	if (ftl_pad_chunk_pad_finish(rband, false)) {
		/* If we're here, end meta wasn't recognized, but the whole band is written */
		/* Assume the band was padded and ignore it */
		return;
	}

	/* The LBA map was assigned from restore pool */
	band->lba_map.map = NULL;
	band->state = FTL_BAND_STATE_OPEN;
	rc = ftl_band_set_direct_access(band, true);
	if (rc) {
		restore->pad_status = rc;
		if (--restore->num_pad_bands == 0) {
			if (dev->nv_cache.bdev_desc) {
				ftl_restore_nv_cache(restore);
			} else {
				ftl_restore_complete(restore, restore->pad_status);
			}
		}
		return;
	}

	for (i = 0; i < band->num_chunks; ++i) {
		if (band->chunk_buf[i].state == FTL_CHUNK_STATE_CLOSED) {
			continue;
		}

		rc = ftl_retrieve_chunk_info(dev, band->chunk_buf[i].start_ppa, &info, 1);
		if (spdk_unlikely(rc)) {
			goto error;
		}
		ppa = band->chunk_buf[i].start_ppa;
		ppa.lbk = info.wp;

		buffer = spdk_dma_zmalloc(FTL_BLOCK_SIZE * dev->xfer_size, sizeof(uint32_t), NULL);
		if (spdk_unlikely(!buffer)) {
			rc = -ENOMEM;
			goto error;
		}

		io = ftl_restore_init_pad_io(rband, buffer, ppa);
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
	ftl_pad_chunk_pad_finish(rband, true);
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
			ftl_restore_complete(restore, status);
			return;
		} else {
			SPDK_ERRLOG("%s while restoring tail md. Will attempt to pad band %u.\n",
				    spdk_strerror(-status), rband->band->id);
			STAILQ_INSERT_TAIL(&restore->pad_bands, rband, stailq);
			restore->num_pad_bands++;
		}
	}

	if (!status && ftl_restore_l2p(rband->band)) {
		ftl_restore_complete(restore, -ENOTRECOVERABLE);
		return;
	}

	rband = ftl_restore_next_band(restore);
	if (!rband) {
		if (!STAILQ_EMPTY(&restore->pad_bands)) {
			spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_restore_pad_open_bands,
					     restore);
		} else if (dev->nv_cache.bdev_desc) {
			ftl_restore_nv_cache(restore);
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

	band->tail_md_ppa = ftl_band_tail_md_ppa(band);
	band->lba_map.map = restore->lba_map;
	band->lba_map.dma_buf = restore->md_buf;

	if (ftl_band_read_tail_md(band, band->tail_md_ppa, ftl_restore_tail_md_cb, rband)) {
		SPDK_ERRLOG("Failed to send tail metadata read\n");
		ftl_restore_complete(restore, -EIO);
		return -EIO;
	}

	return 0;
}

int
ftl_restore_device(struct ftl_restore *restore, ftl_restore_fn cb)
{
	struct ftl_restore_band *rband;

	restore->l2p_phase = true;
	restore->current = 0;
	restore->cb = cb;

	/* If restore_device is called, there must be at least one valid band */
	rband = ftl_restore_next_band(restore);
	return ftl_restore_tail_md(rband);
}
