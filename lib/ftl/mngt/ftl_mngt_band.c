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
	if (!ftl_fast_startup(dev)) {
		band->md->df_lba_map = FTL_DF_OBJ_ID_INVALID;
	}

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

	dev->num_logical_bands_in_physical = num_logical_in_phys;

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

static struct ftl_band *next_high_prio_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *result = NULL, *band;
	uint64_t validity = UINT64_MAX;

	TAILQ_FOREACH(band, &dev->shut_bands, queue_entry) {
		if (band->lba_map.num_vld < validity) {
			result = band;
			validity = result->lba_map.num_vld;
		}
	}

	return result;
}

static int finalize_init_gc(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;

	ftl_band_init_gc_iter(dev);
	dev->sb_shm->gc_info.band_id_high_prio = FTL_BAND_ID_INVALID;

	if (0 == dev->num_free) {
		/* Get number of available blocks in writer */
		uint64_t free_blocks = ftl_writer_free_block(&dev->writer_gc);

		/*
		 * First, check a band candidate to GC
		 */
		band = ftl_band_search_next_to_defrag(dev);
		ftl_bug(NULL == band);
		uint64_t blocks_to_move = band->lba_map.num_vld;
		if (blocks_to_move <= free_blocks) {
			/* This GC band can be moved */
			return 0;
		}

		/*
		 * The GC candidate cannot be moved because no enough space. We need to find
		 * another band.
		 */
		band = next_high_prio_band(dev);
		ftl_bug(NULL == band);

		if (band->lba_map.num_vld > free_blocks) {
			FTL_ERRLOG(dev, "CRITICAL ERROR, no more free bands and cannot start\n");
			return -1;
		} else {
			/* GC needs to start using this band */
			dev->sb_shm->gc_info.band_id_high_prio = band->id;
		}
	}

	return 0;
}

void
ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_band *band, *temp_band, *open_bands[FTL_MAX_OPEN_BANDS];
	size_t i, num_open = 0, num_shut = 0;
	bool fast_startup = ftl_fast_startup(dev);

	TAILQ_FOREACH_SAFE(band, &dev->free_bands, queue_entry, temp_band) {
		band->md->df_lba_map = FTL_DF_OBJ_ID_INVALID;
	}

	TAILQ_FOREACH_SAFE(band, &dev->shut_bands, queue_entry, temp_band) {
		if (band->md->state == FTL_BAND_STATE_OPEN ||
		    band->md->state == FTL_BAND_STATE_FULL) {
			TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
			open_bands[num_open++] = band;
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

		band->md->df_lba_map = FTL_DF_OBJ_ID_INVALID;
	}

	/* Assign open bands to writers and alloc necessary resources */
	for (i = 0; i < num_open; ++i) {
		band = open_bands[i];

		if (band->md->type == FTL_BAND_TYPE_COMPACTION) {
			if (band->md->state == FTL_BAND_STATE_FULL) {
				TAILQ_INSERT_TAIL(&dev->writer_user.full_bands, band, queue_entry);
			} else {
				if (dev->writer_user.band == NULL) {
					dev->writer_user.band = band;
				} else {
					dev->writer_user.next_band = band;
				}
			}

			dev->writer_user.band_num++;
			ftl_band_set_owner(band, ftl_writer_band_state_change, &dev->writer_user);
		}

		if (band->md->type == FTL_BAND_TYPE_GC) {
			if (band->md->state == FTL_BAND_STATE_FULL) {
				TAILQ_INSERT_TAIL(&dev->writer_gc.full_bands, band, queue_entry);
			} else {
				if (dev->writer_gc.band == NULL) {
					dev->writer_gc.band = band;
				} else {
					dev->writer_gc.next_band = band;
				}
			}

			dev->writer_gc.band_num++;
			ftl_band_set_owner(band, ftl_writer_band_state_change, &dev->writer_gc);
		}

		if (fast_startup) {
			FTL_NOTICELOG(dev, "SHM: band open lba map df_id 0x%"PRIx64"\n", band->md->df_lba_map);
			if (ftl_band_open_lba_map(band)) {
				ftl_mngt_fail_step(mngt);
				return;
			}

			uint64_t offset = band->md->iter.offset;
			ftl_band_iter_init(band);
			ftl_band_iter_set(band, offset);
		} else if (dev->sb->clean) {
			band->md->df_lba_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_lba_map(band)) {
				ftl_mngt_fail_step(mngt);
				return;
			}

			uint64_t offset = band->md->iter.offset;
			ftl_band_iter_init(band);
			ftl_band_iter_set(band, offset);
		}
	}

	if (fast_startup) {
		ftl_mempool_initialize_ext(dev->lba_pool);
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

	if (finalize_init_gc(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}
