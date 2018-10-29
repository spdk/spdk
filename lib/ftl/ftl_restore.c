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
#include <spdk/ftl.h>
#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_io.h"

struct ftl_restore_band {
	struct ftl_restore		*parent;

	enum ftl_md_status		md_status;
};

struct ftl_restore {
	atomic_uint			num_ios;

	struct ftl_band			**bands;

	struct ftl_restore_band		*io;

	void				*md_buf;

	void				*lba_map;
};

static void
ftl_restore_cb(void *ctx, int status)
{
	unsigned int cnt __attribute__((unused));
	struct ftl_restore_band *io = ctx;

	io->md_status = status;
	cnt = atomic_fetch_sub(&io->parent->num_ios, 1);
	assert(cnt);
}

static void
ftl_restore_wait_io_cmpl(struct ftl_restore *restore)
{
	while (atomic_load(&restore->num_ios));
}

static int
ftl_band_cmp(const void *lband, const void *rband)
{
	uint64_t lseq = ((*(struct ftl_band **)lband))->md.seq;
	uint64_t rseq = ((*(struct ftl_band **)rband))->md.seq;

	if (lseq < rseq) {
		return -1;
	} else {
		return 1;
	}
}

static int
ftl_restore_l2p(struct ftl_band *band)
{
	struct ftl_dev *dev = band->dev;
	struct ftl_ppa ppa;
	uint64_t lba;

	for (size_t i = 0; i < ftl_num_band_lbks(band->dev); ++i) {
		if (!ftl_get_bit(i, band->md.vld_map)) {
			continue;
		}

		lba = band->md.lba_map[i];
		if (lba >= dev->l2p_len) {
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

static int
ftl_restore_head_md(struct ftl_dev *dev, struct ftl_restore *restore)
{
	struct ftl_band	*band;
	void			*head_buf;
	int			rc;
	int			head_found = 0;
	struct ftl_cb		cb = {
		.fn = ftl_restore_cb,
	};

	head_buf = spdk_dma_zmalloc(ftl_dev_num_bands(dev) * ftl_head_md_num_lbks(dev) *
				    FTL_BLOCK_SIZE, FTL_BLOCK_SIZE, NULL);
	if (!head_buf) {
		return -ENOMEM;
	}

	restore->num_ios = dev->num_bands;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		cb.ctx = &restore->io[band->id];
		rc = ftl_band_read_head_md(band, &band->md, head_buf + band->id *
					   ftl_head_md_num_lbks(dev) * FTL_BLOCK_SIZE, &cb);
		if (rc) {
			SPDK_ERRLOG("Unable to read head metadata\n");
			break;
		}
	}

	ftl_restore_wait_io_cmpl(restore);

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (!restore->io[band->id].md_status) {
			head_found = 1;
			break;
		}
	}

	if (!head_found) {
		rc = -1;
		SPDK_ERRLOG("Unable to find head metadata header\n");
		goto out;
	}
out:
	spdk_dma_free(head_buf);
	return rc;
}

static int
ftl_restore_head_md_valid(struct ftl_dev *dev, struct ftl_restore *restore)
{
	struct ftl_band	*band;
	enum ftl_md_status	status;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		status = restore->io[band->id].md_status;
		if (status != FTL_MD_SUCCESS &&
		    status != FTL_MD_NO_MD &&
		    status != FTL_MD_IO_FAILURE) {
			SPDK_ERRLOG("Inconsistent head metadata found on band %u\n", band->id);
			return 0;
		}
	}

	return 1;
}

static int
ftl_restore_read_tail_md(struct ftl_band *band, struct ftl_restore_band *io)
{
	void *buf = io->parent->md_buf;
	struct ftl_cb cb = {
		.fn = ftl_restore_cb,
		.ctx = io
	};

	io->parent->num_ios = 1;
	if (ftl_band_read_tail_md(band, &band->md, buf, band->tail_md_ppa, &cb)) {
		SPDK_ERRLOG("Unable to read tail metadata\n");
		return -1;
	}

	/* TODO: try to use this wait only in one place */
	ftl_restore_wait_io_cmpl(io->parent);

	return io->md_status;
}

static int
ftl_restore_tail_md(struct ftl_band *band, struct ftl_restore_band *io)
{
	band->tail_md_ppa = ftl_band_tail_md_ppa(band);
	if (!ftl_restore_read_tail_md(band, io)) {
		return 0;
	}

	return -1;
}

void
ftl_restore_free(struct ftl_restore *restore)
{
	if (!restore) {
		return;
	}

	spdk_dma_free(restore->md_buf);
	free(restore->lba_map);
	free(restore->bands);
	free(restore->io);
	free(restore);
}

struct ftl_restore *
ftl_restore_init(struct ftl_dev *dev)
{
	struct ftl_restore *restore;
	size_t i;

	restore = calloc(1, sizeof(*restore));
	if (!restore) {
		goto error;
	}

	restore->io = calloc(ftl_dev_num_bands(dev), sizeof(*restore->io));
	if (!restore->io) {
		goto error;
	}

	restore->bands = calloc(ftl_dev_num_bands(dev), sizeof(*restore->bands));
	if (!restore->bands) {
		goto error;
	}

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		restore->bands[i] = &dev->bands[i];
		restore->io[i].parent = restore;
		restore->io[i].md_status = FTL_MD_NO_MD;
	}

	restore->md_buf = spdk_dma_zmalloc(ftl_tail_md_num_lbks(dev) * FTL_BLOCK_SIZE,
					   FTL_BLOCK_SIZE, NULL);
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

static int
ftl_restore_check_md_seq(const struct ftl_dev *dev, const struct ftl_restore *restore)
{
	struct ftl_band *band, *nband;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (restore->io[band->id].md_status) {
			continue;
		}

		nband = LIST_NEXT(band, list_entry);
		if (nband && band->md.seq == nband->md.seq) {
			return -1;
		}
	}
	return 0;
}

int
ftl_restore_check_device(struct ftl_dev *dev, struct ftl_restore *restore)
{
	int rc = 0;

	rc = ftl_restore_head_md(dev, restore);
	if (rc) {
		goto out;
	}

	if (!ftl_restore_head_md_valid(dev, restore)) {
		rc = -1;
		goto out;
	}

	/* Sort bands in sequence number ascending order */
	qsort(restore->bands, ftl_dev_num_bands(dev), sizeof(struct ftl_band *),
	      ftl_band_cmp);

	if (ftl_restore_check_md_seq(dev, restore)) {
		rc = -1;
		goto out;
	}

	dev->l2p_len = dev->global_md.l2p_len;

out:
	return rc;
}

int
ftl_restore_state(struct ftl_dev *dev, struct ftl_restore *restore)
{
	int				rc = 0;
	struct ftl_band		*band;
	struct ftl_restore_band	*io;

	/* Read end metadata sequentially for bands where valid start metadata was found */
	for (size_t i = 0; i < ftl_dev_num_bands(dev); ++i) {
		band = restore->bands[i];
		io = &restore->io[band->id];
		if (!band->num_chunks || io->md_status) {
			ftl_band_md_clear(&band->md);
			continue;
		}

		band->md.lba_map = restore->lba_map;
		rc = ftl_restore_tail_md(band, io);
		if (rc) {
			break;
		}

		rc = ftl_restore_l2p(band);
		if (rc) {
			break;
		}
	}

	return rc;
}
