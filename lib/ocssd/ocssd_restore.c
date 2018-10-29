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
#include "ocssd_core.h"
#include "ocssd_band.h"

struct ocssd_restore_band {
	struct ocssd_restore		*parent;

	enum ocssd_md_status		md_status;
};

struct ocssd_restore {
	atomic_uint			num_ios;

	struct ocssd_band		**bands;

	struct ocssd_restore_band	*io;

	void				*md_buf;

	void				*lba_map;
};

static void
ocssd_restore_cb(void *ctx, int status)
{
	unsigned int cnt __attribute__((unused));
	struct ocssd_restore_band *io = ctx;

	io->md_status = status;
	cnt = atomic_fetch_sub(&io->parent->num_ios, 1);
	assert(cnt);
}

static void
ocssd_restore_wait_io_cmpl(struct ocssd_restore *restore)
{
	while (atomic_load(&restore->num_ios));
}

static int
ocssd_band_cmp(const void *lband, const void *rband)
{
	uint64_t lseq = ((*(struct ocssd_band **)lband))->md.seq;
	uint64_t rseq = ((*(struct ocssd_band **)rband))->md.seq;

	if (lseq < rseq) {
		return -1;
	} else {
		return 1;
	}
}

static int
ocssd_restore_l2p(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_ppa ppa;
	uint64_t lba;

	for (size_t i = 0; i < ocssd_num_band_lbks(band->dev); ++i) {
		if (!ocssd_get_bit(i, band->md.vld_map)) {
			continue;
		}

		lba = band->md.lba_map[i];
		if (lba >= dev->l2p_len) {
			return -1;
		}

		ppa = ocssd_l2p_get(dev, lba);

		if (!ocssd_ppa_invalid(ppa)) {
			ocssd_invalidate_addr(dev, ppa);
		}

		ppa = ocssd_band_ppa_from_lbkoff(band, i);

		ocssd_band_set_addr(band, lba, ppa);
		ocssd_l2p_set(dev, lba, ppa);
	}

	band->md.lba_map = NULL;
	return 0;
}

static int
ocssd_restore_head_md(struct ocssd_dev *dev, struct ocssd_restore *restore)
{
	struct ocssd_band	*band;
	void			*head_buf;
	int			rc;
	int			head_found = 0;
	struct ocssd_cb		cb = {
		.fn = ocssd_restore_cb,
	};

	head_buf = spdk_dma_zmalloc(ocssd_dev_num_bands(dev) * ocssd_head_md_num_lbks(dev) *
				    OCSSD_BLOCK_SIZE, OCSSD_BLOCK_SIZE, NULL);
	if (!head_buf) {
		return -ENOMEM;
	}

	restore->num_ios = dev->num_bands;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		cb.ctx = &restore->io[band->id];
		rc = ocssd_band_read_head_md(band, &band->md, head_buf + band->id *
					     ocssd_head_md_num_lbks(dev) * OCSSD_BLOCK_SIZE, &cb);
		if (rc) {
			SPDK_ERRLOG("Unable to read head metadata\n");
			break;
		}
	}

	ocssd_restore_wait_io_cmpl(restore);

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
ocssd_restore_head_md_valid(struct ocssd_dev *dev, struct ocssd_restore *restore)
{
	struct ocssd_band	*band;
	enum ocssd_md_status	status;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		status = restore->io[band->id].md_status;
		if (status != OCSSD_MD_SUCCESS &&
		    status != OCSSD_MD_NO_MD &&
		    status != OCSSD_MD_IO_FAILURE) {
			SPDK_ERRLOG("Inconsistent head metadata found on band %u\n", band->id);
			return 0;
		}
	}

	return 1;
}

static int
ocssd_restore_read_tail_md(struct ocssd_band *band, struct ocssd_restore_band *io)
{
	void *buf = io->parent->md_buf;
	struct ocssd_cb cb = {
		.fn = ocssd_restore_cb,
		.ctx = io
	};

	io->parent->num_ios = 1;
	if (ocssd_band_read_tail_md(band, &band->md, buf, band->tail_md_ppa, &cb)) {
		SPDK_ERRLOG("Unable to read tail metadata\n");
		return -1;
	}

	/* TODO: try to use this wait only in one place */
	ocssd_restore_wait_io_cmpl(io->parent);

	return io->md_status;
}

static int
ocssd_restore_tail_md(struct ocssd_band *band, struct ocssd_restore_band *io)
{
	band->tail_md_ppa = ocssd_band_tail_md_ppa(band);
	if (!ocssd_restore_read_tail_md(band, io)) {
		return 0;
	}

	return -1;
}

void
ocssd_restore_free(struct ocssd_restore *restore)
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

struct ocssd_restore *
ocssd_restore_init(struct ocssd_dev *dev)
{
	struct ocssd_restore *restore;
	size_t i;

	restore = calloc(1, sizeof(*restore));
	if (!restore) {
		goto error;
	}

	restore->io = calloc(ocssd_dev_num_bands(dev), sizeof(*restore->io));
	if (!restore->io) {
		goto error;
	}

	restore->bands = calloc(ocssd_dev_num_bands(dev), sizeof(*restore->bands));
	if (!restore->bands) {
		goto error;
	}

	for (i = 0; i < ocssd_dev_num_bands(dev); ++i) {
		restore->bands[i] = &dev->bands[i];
		restore->io[i].parent = restore;
		restore->io[i].md_status = OCSSD_MD_NO_MD;
	}

	restore->md_buf = spdk_dma_zmalloc(ocssd_tail_md_num_lbks(dev) * OCSSD_BLOCK_SIZE,
					   OCSSD_BLOCK_SIZE, NULL);
	if (!restore->md_buf) {
		goto error;
	}

	restore->lba_map = calloc(ocssd_num_band_lbks(dev), sizeof(uint64_t));
	if (!restore->lba_map) {
		goto error;
	}

	return restore;
error:
	ocssd_restore_free(restore);
	return NULL;
}

static int
ocssd_restore_check_md_seq(const struct ocssd_dev *dev, const struct ocssd_restore *restore)
{
	struct ocssd_band *band, *nband;

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
ocssd_restore_check_device(struct ocssd_dev *dev, struct ocssd_restore *restore)
{
	int rc = 0;

	rc = ocssd_restore_head_md(dev, restore);
	if (rc) {
		goto out;
	}

	if (!ocssd_restore_head_md_valid(dev, restore)) {
		rc = -1;
		goto out;
	}

	/* Sort bands in sequence number ascending order */
	qsort(restore->bands, ocssd_dev_num_bands(dev), sizeof(struct ocssd_band *),
	      ocssd_band_cmp);

	if (ocssd_restore_check_md_seq(dev, restore)) {
		rc = -1;
		goto out;
	}

	dev->l2p_len = dev->global_md.l2p_len;

out:
	return rc;
}

int
ocssd_restore_state(struct ocssd_dev *dev, struct ocssd_restore *restore)
{
	int				rc = 0;
	struct ocssd_band		*band;
	struct ocssd_restore_band	*io;

	/* Read end metadata sequentially for bands where valid start metadata was found */
	for (size_t i = 0; i < ocssd_dev_num_bands(dev); ++i) {
		band = restore->bands[i];
		io = &restore->io[band->id];
		if (!band->num_chunks || io->md_status) {
			ocssd_band_md_clear(&band->md);
			continue;
		}

		band->md.lba_map = restore->lba_map;
		rc = ocssd_restore_tail_md(band, io);
		if (rc) {
			break;
		}

		rc = ocssd_restore_l2p(band);
		if (rc) {
			break;
		}
	}

	return rc;
}
