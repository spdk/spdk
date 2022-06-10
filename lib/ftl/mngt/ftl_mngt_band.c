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

#include "ftl_core.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_internal.h"

static int ftl_band_init_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_md *band_info_md = dev->layout.md[ftl_layout_region_type_band_md];
	struct ftl_band_md *band_md = ftl_md_get_buffer(band_info_md);

	band->md = &band_md[band->id];

	return 0;
}

static int ftl_dev_init_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band, *pband;
	unsigned int i;

	TAILQ_INIT(&dev->free_bands);
	TAILQ_INIT(&dev->shut_bands);

	dev->num_free = 0;
	dev->bands = calloc(ftl_get_num_bands(dev), sizeof(*dev->bands));
	if (!dev->bands) {
		return -1;
	}

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->id = i;
		band->dev = dev;

		/* Adding to shut_bands is necessary - see ftl_restore_band_close_cb() */
		if (TAILQ_EMPTY(&dev->shut_bands)) {
			TAILQ_INSERT_HEAD(&dev->shut_bands, band, queue_entry);
		} else {
			TAILQ_INSERT_AFTER(&dev->shut_bands, pband, band, queue_entry);
		}
		pband = band;

		CIRCLEQ_INIT(&band->zones);
		band->zone_buf = calloc(ftl_get_num_punits(dev), sizeof(*band->zone_buf));
		if (!band->zone_buf) {
			FTL_ERRLOG(dev, "Failed to allocate block state table for band: [%u]\n", i);
			return -1;
		}
	}

	return 0;
}

static int ftl_dev_init_bands_md(struct spdk_ftl_dev *dev)
{
	unsigned int i;
	int rc = 0;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		rc = ftl_band_init_md(&dev->bands[i]);
		if (rc) {
			FTL_ERRLOG(dev, "Failed to initialize metadata structures for band [%u]\n", i);
			break;
		}
	}

	return rc;
}

static void free_band_items(struct ftl_band *band)
{
	free(band->zone_buf);
	band->zone_buf = NULL;
}

static void ftl_dev_deinit_bands(struct spdk_ftl_dev *dev)
{
	if (dev->bands) {
		uint64_t i;
		for (i = 0; i < dev->num_bands; ++i) {
			free_band_items(&dev->bands[i]);
		}
	}

	free(dev->bands);
}

void ftl_mngt_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_dev_init_bands(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_init_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_dev_init_bands_md(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_deinit_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_dev_deinit_bands(dev);
	ftl_mngt_next_step(mngt);
}

static void
decorate_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band, *temp_band;
	size_t i, num_to_drop, phys_id = 0;
	uint64_t num_blocks;
	uint64_t num_blocks_in_band = ftl_get_num_blocks_in_band(dev);
	uint64_t reclaim_unit_num_blocks = dev->conf.base_bdev_reclaim_unit_size / FTL_BLOCK_SIZE;
	uint32_t num_logical_in_phys = 2;

	assert(reclaim_unit_num_blocks % num_blocks_in_band == 0);
	assert(reclaim_unit_num_blocks >= num_blocks_in_band);

	num_blocks = spdk_bdev_get_num_blocks(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	/* For base bdev bigger than 1TB take reclaim uint size for grouping GC bands */
	if (num_blocks > ((1ULL << 40) / FTL_BLOCK_SIZE)) {
		assert(reclaim_unit_num_blocks < spdk_bdev_get_num_blocks(spdk_bdev_desc_get_bdev(
					dev->base_bdev_desc)));
		num_logical_in_phys = reclaim_unit_num_blocks / num_blocks_in_band;
	}

	num_to_drop = ftl_get_num_bands(dev) % num_logical_in_phys;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->tail_md_addr = ftl_band_tail_md_addr(band);

		band->phys_id = phys_id;
		if ((i + 1) % num_logical_in_phys == 0) {
			phys_id++;
		}

		/* Mark not aligned logical bands as broken */
		if (i + 1 > ftl_get_num_bands(dev) - num_to_drop) {
			band->num_zones = 0;
		}
	}

	/* Remove band from shut_bands list to prevent further processing */
	/* if all blocks on this band are bad */
	TAILQ_FOREACH_SAFE(band, &dev->shut_bands, queue_entry, temp_band) {
		if (!band->num_zones) {
			dev->num_bands--;
			TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
			free_band_items(band);
		}
	}
}

void ftl_mngt_decorate_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	decorate_bands(dev);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_band *band, *temp_band;
	size_t num_open = 0, num_shut = 0;

	TAILQ_FOREACH_SAFE(band, &dev->shut_bands, queue_entry, temp_band) {
		if (band->md->state == FTL_BAND_STATE_OPEN ||
		    band->md->state == FTL_BAND_STATE_FULL) {
			TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
			num_open++;
			assert(num_open <= FTL_MAX_OPEN_BANDS);
			continue;
		}

		if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
			TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
			assert(band->md->state == FTL_BAND_STATE_FREE);
			band->md->state = FTL_BAND_STATE_CLOSED;
			ftl_band_set_state(band, FTL_BAND_STATE_FREE);
		} else {
			num_shut++;
		}
	}

	/* Recalculate number of free bands */
	dev->num_free = 0;
	TAILQ_FOREACH(band, &dev->free_bands, queue_entry) {
		assert(band->md->state == FTL_BAND_STATE_FREE);
		dev->num_free++;
	}
	ftl_apply_limits(dev);

	if ((num_shut + num_open + dev->num_free) != ftl_get_num_bands(dev)) {
		FTL_ERRLOG(dev, "ERROR, band list inconsistent state\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}
