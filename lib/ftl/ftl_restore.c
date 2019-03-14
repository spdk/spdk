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

#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_io.h"

struct ftl_restore_band {
	struct ftl_restore		*parent;

	struct ftl_band			*band;

	enum ftl_md_status		md_status;
};

struct ftl_restore {
	struct spdk_ftl_dev		*dev;

	ftl_restore_fn			cb;

	unsigned int			num_ios;

	unsigned int			current;

	struct ftl_restore_band		*bands;

	void				*md_buf;

	void				*lba_map;

	bool				l2p_phase;
};

static int
ftl_restore_tail_md(struct ftl_restore_band *rband);

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
	uint64_t lseq = ((struct ftl_restore_band *)lband)->band->md.seq;
	uint64_t rseq = ((struct ftl_restore_band *)rband)->band->md.seq;

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
		if (next_band && rband->band->md.seq == next_band->md.seq) {
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
ftl_restore_head_cb(void *ctx, int status)
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
	struct ftl_cb cb;
	char *head_buf = restore->md_buf;
	unsigned int num_failed = 0, num_ios;
	size_t i;

	cb.fn = ftl_restore_head_cb;
	restore->num_ios = ftl_dev_num_bands(dev);

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		rband = &restore->bands[i];
		cb.ctx = rband;

		if (ftl_band_read_head_md(rband->band, &rband->band->md, head_buf, &cb)) {
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

		head_buf += ftl_head_md_num_lbks(dev) * FTL_BLOCK_SIZE;
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
		if (!spdk_bit_array_get(band->md.vld_map, i)) {
			continue;
		}

		lba = band->md.lba_map[i];
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

	band->md.lba_map = NULL;
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
ftl_restore_tail_md_cb(void *ctx, int status)
{
	struct ftl_restore_band *rband = ctx;
	struct ftl_restore *restore = rband->parent;

	if (status) {
		ftl_restore_complete(restore, status);
		return;
	}

	if (ftl_restore_l2p(rband->band)) {
		ftl_restore_complete(restore, -ENOTRECOVERABLE);
		return;
	}

	rband = ftl_restore_next_band(restore);
	if (!rband) {
		ftl_restore_complete(restore, 0);
		return;
	}

	ftl_restore_tail_md(rband);
}

static int
ftl_restore_tail_md(struct ftl_restore_band *rband)
{
	struct ftl_restore *restore = rband->parent;
	struct ftl_band *band = rband->band;
	struct ftl_cb cb = { .fn = ftl_restore_tail_md_cb,
		       .ctx = rband
	};

	band->tail_md_ppa = ftl_band_tail_md_ppa(band);
	band->md.lba_map = restore->lba_map;

	if (ftl_band_read_tail_md(band, &band->md, restore->md_buf, band->tail_md_ppa, &cb)) {
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
