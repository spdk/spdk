/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
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
	struct ftl_md *band_info_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	struct ftl_band_md *band_md = ftl_md_get_buffer(band_info_md);

	band->md = &band_md[band->id];

	return 0;
}

static int
ftl_dev_init_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t i;

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
		band->start_addr = i * dev->num_blocks_in_band;
		band->tail_md_addr = ftl_band_tail_md_addr(band);

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
}

void
ftl_mngt_decorate_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	decorate_bands(dev);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_finalize_init_bands(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_band *band, *temp_band;
	uint64_t num_open = 0, num_shut = 0;

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
