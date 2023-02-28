/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_internal.h"

static int
ftl_band_init_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	struct ftl_md *band_info_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	struct ftl_md *valid_map_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_VALID_MAP];
	uint64_t band_num_blocks = ftl_get_num_blocks_in_band(band->dev);
	size_t band_valid_map_bytes;
	struct ftl_band_md *band_md = ftl_md_get_buffer(band_info_md);

	if (band_num_blocks % (ftl_bitmap_buffer_alignment * 8)) {
		FTL_ERRLOG(dev, "The number of blocks in band is not divisible by bitmap word bits\n");
		return -EINVAL;
	}
	band_valid_map_bytes = band_num_blocks / 8;

	p2l_map->valid = ftl_bitmap_create(ftl_md_get_buffer(valid_map_md) +
					   band->start_addr / 8, band_valid_map_bytes);
	if (!p2l_map->valid) {
		return -ENOMEM;
	}

	band->md = &band_md[band->id];
	if (!ftl_fast_startup(dev)) {
		band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	}

	return 0;
}

static int
ftl_dev_init_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t i, blocks, md_blocks, md_bands;

	/* Calculate initial number of bands */
	blocks = spdk_bdev_get_num_blocks(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
	dev->num_bands = blocks / ftl_get_num_blocks_in_band(dev);

	/* Calculate number of bands considering base device metadata size requirement */
	md_blocks = ftl_layout_base_md_blocks(dev);
	md_bands = spdk_divide_round_up(md_blocks, dev->num_blocks_in_band);

	if (dev->num_bands > md_bands) {
		/* Save a band worth of space for metadata */
		dev->num_bands -= md_bands;
	} else {
		FTL_ERRLOG(dev, "Base device too small to store metadata\n");
		return -1;
	}

	TAILQ_INIT(&dev->free_bands);
	TAILQ_INIT(&dev->shut_bands);

	dev->num_free = 0;
	dev->bands = calloc(ftl_get_num_bands(dev), sizeof(*dev->bands));
	if (!dev->bands) {
		return -ENOMEM;
	}

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->id = i;
		band->dev = dev;

		/* Adding to shut_bands is necessary - see ftl_restore_band_close_cb() */
		TAILQ_INSERT_TAIL(&dev->shut_bands, band, queue_entry);
	}

	return 0;
}

static int
ftl_dev_init_bands_md(struct spdk_ftl_dev *dev)
{
	uint64_t i;
	int rc = 0;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		rc = ftl_band_init_md(&dev->bands[i]);
		if (rc) {
			FTL_ERRLOG(dev, "Failed to initialize metadata structures for band [%lu]\n", i);
			break;
		}
	}

	return rc;
}

static void
ftl_dev_deinit_bands(struct spdk_ftl_dev *dev)
{
	free(dev->bands);
}

static void
ftl_dev_deinit_bands_md(struct spdk_ftl_dev *dev)
{
	if (dev->bands) {
		uint64_t i;
		for (i = 0; i < dev->num_bands; ++i) {
			struct ftl_band *band = &dev->bands[i];

			ftl_bitmap_destroy(band->p2l_map.valid);
			band->p2l_map.valid = NULL;

			band->md = NULL;
		}
	}
}

void
ftl_mngt_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_dev_init_bands(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_init_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_dev_init_bands_md(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_deinit_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_dev_deinit_bands(dev);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_bands_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_dev_deinit_bands_md(dev);
	ftl_mngt_next_step(mngt);
}

/*
 * For grouping multiple logical bands (1GiB) to make any IOs more sequential from the drive's
 * perspective. Improves WAF.
 */
#define BASE_BDEV_RECLAIM_UNIT_SIZE (72 * GiB)

static void
decorate_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t i, num_to_drop, phys_id = 0;
	uint64_t num_blocks, num_bands;
	uint64_t num_blocks_in_band = ftl_get_num_blocks_in_band(dev);
	uint64_t reclaim_unit_num_blocks = BASE_BDEV_RECLAIM_UNIT_SIZE / FTL_BLOCK_SIZE;
	uint32_t num_logical_in_phys = 2;

	assert(reclaim_unit_num_blocks % num_blocks_in_band == 0);

	num_blocks = spdk_bdev_get_num_blocks(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	/* For base bdev bigger than 1TB take reclaim uint size for grouping GC bands */
	if (num_blocks > (TiB / FTL_BLOCK_SIZE)) {
		assert(reclaim_unit_num_blocks < num_blocks);
		num_logical_in_phys = reclaim_unit_num_blocks / num_blocks_in_band;
	}

	num_to_drop = ftl_get_num_bands(dev) % num_logical_in_phys;

	i = 0;
	while (i < ftl_get_num_bands(dev) - num_to_drop) {
		band = &dev->bands[i];

		band->phys_id = phys_id;
		i++;
		if (i % num_logical_in_phys == 0) {
			phys_id++;
		}
	}

	/* Mark not aligned logical bands as broken */
	num_bands = ftl_get_num_bands(dev);
	while (i < num_bands) {
		band = &dev->bands[i];
		dev->num_bands--;
		TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
		i++;
	}

	dev->num_logical_bands_in_physical = num_logical_in_phys;
}

void
ftl_mngt_decorate_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	decorate_bands(dev);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_initialize_band_address(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_band *band;
	struct ftl_md *data_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_DATA_BASE];
	uint64_t i;

	for (i = 0; i < ftl_get_num_bands(dev); i++) {
		band = &dev->bands[i];
		band->start_addr = data_md->region->current.offset + i * dev->num_blocks_in_band;
		band->tail_md_addr = ftl_band_tail_md_addr(band);
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_recover_max_seq(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	size_t band_close_seq_id = 0, band_open_seq_id = 0;
	size_t chunk_close_seq_id = 0, chunk_open_seq_id = 0;
	size_t max = 0;

	TAILQ_FOREACH(band, &dev->shut_bands, queue_entry) {
		band_open_seq_id = spdk_max(band_open_seq_id, band->md->seq);
		band_close_seq_id = spdk_max(band_close_seq_id, band->md->close_seq_id);
	}
	ftl_nv_cache_get_max_seq_id(&dev->nv_cache, &chunk_open_seq_id, &chunk_close_seq_id);


	dev->nv_cache.last_seq_id = chunk_close_seq_id;
	dev->writer_gc.last_seq_id = band_close_seq_id;
	dev->writer_user.last_seq_id = band_close_seq_id;

	max = spdk_max(max, band_open_seq_id);
	max = spdk_max(max, band_close_seq_id);
	max = spdk_max(max, chunk_open_seq_id);
	max = spdk_max(max, chunk_close_seq_id);

	dev->sb->seq_id = max;
}

static int
_band_cmp(const void *_a, const void *_b)
{
	struct ftl_band *a, *b;

	a = *((struct ftl_band **)_a);
	b = *((struct ftl_band **)_b);

	return a->md->seq - b->md->seq;
}

static struct ftl_band *
next_high_prio_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *result = NULL, *band;
	uint64_t validity = UINT64_MAX;

	TAILQ_FOREACH(band, &dev->shut_bands, queue_entry) {
		if (band->p2l_map.num_valid < validity) {
			result = band;
			validity = result->p2l_map.num_valid;
		}
	}

	return result;
}

static int
finalize_init_gc(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t free_blocks, blocks_to_move;

	ftl_band_init_gc_iter(dev);
	dev->sb_shm->gc_info.band_id_high_prio = FTL_BAND_ID_INVALID;

	if (0 == dev->num_free) {
		/* Get number of available blocks in writer */
		free_blocks = ftl_writer_get_free_blocks(&dev->writer_gc);

		/*
		 * First, check a band candidate to GC
		 */
		band = ftl_band_search_next_to_reloc(dev);
		ftl_bug(NULL == band);
		blocks_to_move = band->p2l_map.num_valid;
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

		if (band->p2l_map.num_valid > free_blocks) {
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
ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_band *band, *temp_band, *open_bands[FTL_MAX_OPEN_BANDS];
	struct ftl_writer *writer = NULL;
	uint64_t i, num_open = 0, num_shut = 0;
	uint64_t offset;
	bool fast_startup = ftl_fast_startup(dev);

	ftl_recover_max_seq(dev);

	TAILQ_FOREACH_SAFE(band, &dev->free_bands, queue_entry, temp_band) {
		band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
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

		band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	}

	/* Assign open bands to writers and alloc necessary resources */
	qsort(open_bands, num_open, sizeof(open_bands[0]), _band_cmp);

	for (i = 0; i < num_open; ++i) {
		band = open_bands[i];

		if (band->md->type == FTL_BAND_TYPE_COMPACTION) {
			writer = &dev->writer_user;
		} else if (band->md->type == FTL_BAND_TYPE_GC) {
			writer = &dev->writer_gc;
		} else {
			assert(false);
		}

		if (band->md->state == FTL_BAND_STATE_FULL) {
			TAILQ_INSERT_TAIL(&writer->full_bands, band, queue_entry);
		} else {
			if (writer->band == NULL) {
				writer->band = band;
			} else {
				writer->next_band = band;
			}
		}

		writer->num_bands++;
		ftl_band_set_owner(band, ftl_writer_band_state_change, writer);

		if (fast_startup) {
			FTL_NOTICELOG(dev, "SHM: band open P2L map df_id 0x%"PRIx64"\n", band->md->df_p2l_map);
			if (ftl_band_open_p2l_map(band)) {
				ftl_mngt_fail_step(mngt);
				return;
			}

			offset = band->md->iter.offset;
			ftl_band_iter_init(band);
			ftl_band_iter_set(band, offset);
			ftl_mngt_p2l_ckpt_restore_shm_clean(band);
		} else if (dev->sb->clean) {
			band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
			if (ftl_band_alloc_p2l_map(band)) {
				ftl_mngt_fail_step(mngt);
				return;
			}

			offset = band->md->iter.offset;
			ftl_band_iter_init(band);
			ftl_band_iter_set(band, offset);

			if (ftl_mngt_p2l_ckpt_restore_clean(band)) {
				ftl_mngt_fail_step(mngt);
				return;
			}
		}
	}

	if (fast_startup) {
		ftl_mempool_initialize_ext(dev->p2l_pool);
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
