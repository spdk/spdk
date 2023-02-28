/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/ftl.h"

#include "ftl_band.h"
#include "ftl_io.h"
#include "ftl_core.h"
#include "ftl_debug.h"
#include "ftl_internal.h"
#include "utils/ftl_md.h"
#include "utils/ftl_defs.h"

static uint64_t
ftl_band_tail_md_offset(const struct ftl_band *band)
{
	return ftl_get_num_blocks_in_band(band->dev) -
	       ftl_tail_md_num_blocks(band->dev);
}

int
ftl_band_filled(struct ftl_band *band, size_t offset)
{
	return offset == ftl_band_tail_md_offset(band);
}

static void
ftl_band_free_p2l_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);
	assert(p2l_map->ref_cnt == 0);
	assert(p2l_map->band_map != NULL);

	band->md->df_p2l_map = FTL_DF_OBJ_ID_INVALID;
	ftl_mempool_put(dev->p2l_pool, p2l_map->band_map);
	p2l_map->band_map = NULL;
}


static void
ftl_band_free_md_entry(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);
	assert(p2l_map->band_dma_md != NULL);

	ftl_mempool_put(dev->band_md_pool, p2l_map->band_dma_md);
	p2l_map->band_dma_md = NULL;
}

static void
_ftl_band_set_free(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Add the band to the free band list */
	TAILQ_INSERT_TAIL(&dev->free_bands, band, queue_entry);
	band->md->close_seq_id = 0;
	band->reloc = false;

	dev->num_free++;
	ftl_apply_limits(dev);

	band->md->p2l_map_checksum = 0;
}

static void
_ftl_band_set_preparing(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Remove band from free list */
	TAILQ_REMOVE(&dev->free_bands, band, queue_entry);

	band->md->wr_cnt++;

	assert(dev->num_free > 0);
	dev->num_free--;

	ftl_apply_limits(dev);
}

static void
_ftl_band_set_closed_cb(struct ftl_band *band, bool valid)
{
	struct spdk_ftl_dev *dev = band->dev;

	assert(valid == true);

	/* Set the state as free_md() checks for that */
	band->md->state = FTL_BAND_STATE_CLOSED;
	if (band->owner.state_change_fn) {
		band->owner.state_change_fn(band);
	}

	ftl_p2l_validate_ckpt(band);

	/* Free the P2L map if there are no outstanding IOs */
	ftl_band_release_p2l_map(band);
	assert(band->p2l_map.ref_cnt == 0);

	TAILQ_INSERT_TAIL(&dev->shut_bands, band, queue_entry);
}

static void
_ftl_band_set_closed(struct ftl_band *band)
{
	/* Verify that band's metadata is consistent with l2p */
	ftl_band_validate_md(band, _ftl_band_set_closed_cb);
}

ftl_addr
ftl_band_tail_md_addr(struct ftl_band *band)
{
	ftl_addr addr;

	/* Metadata should be aligned to xfer size */
	assert(ftl_band_tail_md_offset(band) % band->dev->xfer_size == 0);

	addr = ftl_band_tail_md_offset(band) + band->start_addr;

	return addr;
}

void
ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state)
{
	switch (state) {
	case FTL_BAND_STATE_FREE:
		assert(band->md->state == FTL_BAND_STATE_CLOSED);
		_ftl_band_set_free(band);
		break;

	case FTL_BAND_STATE_PREP:
		assert(band->md->state == FTL_BAND_STATE_FREE);
		_ftl_band_set_preparing(band);
		break;

	case FTL_BAND_STATE_CLOSED:
		if (band->md->state != FTL_BAND_STATE_CLOSED) {
			assert(band->md->state == FTL_BAND_STATE_CLOSING);
			_ftl_band_set_closed(band);
			return; /* state can be changed asynchronously */
		}
		break;

	case FTL_BAND_STATE_OPEN:
		band->md->p2l_map_checksum = 0;
		break;
	case FTL_BAND_STATE_OPENING:
	case FTL_BAND_STATE_FULL:
	case FTL_BAND_STATE_CLOSING:
		break;
	default:
		FTL_ERRLOG(band->dev, "Unknown band state, %u", state);
		assert(false);
		break;
	}

	band->md->state = state;
}

void
ftl_band_set_type(struct ftl_band *band, enum ftl_band_type type)
{
	switch (type) {
	case FTL_BAND_TYPE_COMPACTION:
	case FTL_BAND_TYPE_GC:
		band->md->type = type;
		break;
	default:
		assert(false);
		break;
	}
}

void
ftl_band_set_p2l(struct ftl_band *band, uint64_t lba, ftl_addr addr, uint64_t seq_id)
{
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	uint64_t offset;

	offset = ftl_band_block_offset_from_addr(band, addr);

	p2l_map->band_map[offset].lba = lba;
	p2l_map->band_map[offset].seq_id = seq_id;
}

void
ftl_band_set_addr(struct ftl_band *band, uint64_t lba, ftl_addr addr)
{
	band->p2l_map.num_valid++;
	ftl_bitmap_set(band->dev->valid_map, addr);
}

size_t
ftl_band_user_blocks_left(const struct ftl_band *band, size_t offset)
{
	size_t tail_md_offset = ftl_band_tail_md_offset(band);

	if (spdk_unlikely(offset > tail_md_offset)) {
		return 0;
	}

	return tail_md_offset - offset;
}

size_t
ftl_band_user_blocks(const struct ftl_band *band)
{
	return ftl_get_num_blocks_in_band(band->dev) -
	       ftl_tail_md_num_blocks(band->dev);
}

static inline uint64_t
ftl_addr_get_band(const struct spdk_ftl_dev *dev, ftl_addr addr)
{
	return (addr - dev->bands->start_addr) / ftl_get_num_blocks_in_band(dev);
}

struct ftl_band *
ftl_band_from_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	uint64_t band_id = ftl_addr_get_band(dev, addr);

	assert(band_id < ftl_get_num_bands(dev));
	return &dev->bands[band_id];
}

uint64_t
ftl_band_block_offset_from_addr(struct ftl_band *band, ftl_addr addr)
{
	assert(ftl_addr_get_band(band->dev, addr) == band->id);
	return addr - band->start_addr;
}

ftl_addr
ftl_band_next_xfer_addr(struct ftl_band *band, ftl_addr addr, size_t num_blocks)
{
	struct spdk_ftl_dev *dev = band->dev;
	size_t num_xfers;
	uint64_t offset;

	assert(ftl_addr_get_band(dev, addr) == band->id);

	offset = addr - band->start_addr;

	/* In case starting address wasn't aligned to xfer_size, we'll align for consistent calculation
	 * purposes - the unaligned value will be preserved at the end however.
	 */
	num_blocks += (offset % dev->xfer_size);
	offset -= (offset % dev->xfer_size);

	/* Calculate offset based on xfer_size aligned writes */
	num_xfers = (num_blocks / dev->xfer_size);
	offset += num_xfers * dev->xfer_size;
	num_blocks -= num_xfers * dev->xfer_size;

	if (offset > ftl_get_num_blocks_in_band(dev)) {
		return FTL_ADDR_INVALID;
	}

	/* If there's any unalignment (either starting addr value or num_blocks), reintroduce it to the final address
	 */
	if (num_blocks) {
		offset += num_blocks;
		if (offset > ftl_get_num_blocks_in_band(dev)) {
			return FTL_ADDR_INVALID;
		}
	}

	addr = band->start_addr + offset;
	return addr;
}

ftl_addr
ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off)
{
	ftl_addr addr;

	addr = block_off + band->start_addr;
	return addr;
}

ftl_addr
ftl_band_next_addr(struct ftl_band *band, ftl_addr addr, size_t offset)
{
	uint64_t block_off = ftl_band_block_offset_from_addr(band, addr);

	return ftl_band_addr_from_block_offset(band, block_off + offset);
}

void
ftl_band_acquire_p2l_map(struct ftl_band *band)
{
	assert(band->p2l_map.band_map != NULL);
	band->p2l_map.ref_cnt++;
}

static int
ftl_band_alloc_md_entry(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;
	struct ftl_layout_region *region = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_BAND_MD];

	p2l_map->band_dma_md = ftl_mempool_get(dev->band_md_pool);

	if (!p2l_map->band_dma_md) {
		return -1;
	}

	memset(p2l_map->band_dma_md, 0, region->entry_size * FTL_BLOCK_SIZE);
	return 0;
}

int
ftl_band_alloc_p2l_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	assert(p2l_map->ref_cnt == 0);
	assert(p2l_map->band_map == NULL);

	assert(band->md->df_p2l_map == FTL_DF_OBJ_ID_INVALID);
	p2l_map->band_map = ftl_mempool_get(dev->p2l_pool);
	if (!p2l_map->band_map) {
		return -1;
	}

	if (ftl_band_alloc_md_entry(band)) {
		ftl_band_free_p2l_map(band);
		return -1;
	}

	band->md->df_p2l_map = ftl_mempool_get_df_obj_id(dev->p2l_pool, p2l_map->band_map);

	/* Set the P2L to FTL_LBA_INVALID */
	memset(p2l_map->band_map, -1, FTL_BLOCK_SIZE * ftl_p2l_map_num_blocks(band->dev));

	ftl_band_acquire_p2l_map(band);
	return 0;
}

int
ftl_band_open_p2l_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	assert(p2l_map->ref_cnt == 0);
	assert(p2l_map->band_map == NULL);

	assert(band->md->df_p2l_map != FTL_DF_OBJ_ID_INVALID);

	if (ftl_band_alloc_md_entry(band)) {
		p2l_map->band_map = NULL;
		return -1;
	}

	p2l_map->band_map = ftl_mempool_claim_df(dev->p2l_pool, band->md->df_p2l_map);

	ftl_band_acquire_p2l_map(band);
	return 0;
}

void
ftl_band_release_p2l_map(struct ftl_band *band)
{
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	assert(p2l_map->band_map != NULL);
	assert(p2l_map->ref_cnt > 0);
	p2l_map->ref_cnt--;

	if (p2l_map->ref_cnt == 0) {
		if (p2l_map->p2l_ckpt) {
			ftl_p2l_ckpt_release(band->dev, p2l_map->p2l_ckpt);
			p2l_map->p2l_ckpt = NULL;
		}
		ftl_band_free_p2l_map(band);
		ftl_band_free_md_entry(band);
	}
}

ftl_addr
ftl_band_p2l_map_addr(struct ftl_band *band)
{
	return band->tail_md_addr;
}

int
ftl_band_write_prep(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	if (ftl_band_alloc_p2l_map(band)) {
		return -1;
	}

	band->p2l_map.p2l_ckpt = ftl_p2l_ckpt_acquire(dev);
	band->md->p2l_md_region = ftl_p2l_ckpt_region_type(band->p2l_map.p2l_ckpt);
	ftl_band_iter_init(band);

	band->md->seq = ftl_get_next_seq_id(dev);

	FTL_DEBUGLOG(dev, "Band to write, id %u seq %"PRIu64"\n", band->id, band->md->seq);
	return 0;
}

size_t
ftl_p2l_map_pool_elem_size(struct spdk_ftl_dev *dev)
{
	/* Map pool element holds the whole tail md */
	return ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE;
}

static double
_band_invalidity(struct ftl_band *band)
{
	double valid = band->p2l_map.num_valid;
	double count = ftl_band_user_blocks(band);

	return 1.0 - (valid / count);
}

static void
dump_bands_under_relocation(struct spdk_ftl_dev *dev)
{
	uint64_t i = dev->sb_shm->gc_info.current_band_id;
	uint64_t end = dev->sb_shm->gc_info.current_band_id + dev->num_logical_bands_in_physical;

	for (; i < end; i++) {
		struct ftl_band *band = &dev->bands[i];

		FTL_DEBUGLOG(dev, "Band, id %u, phys_is %u, wr cnt = %u, invalidity = %u%%\n",
			     band->id, band->phys_id, (uint32_t)band->md->wr_cnt,
			     (uint32_t)(_band_invalidity(band) * 100));
	}
}

static bool
is_band_relocateable(struct ftl_band *band)
{
	/* Can only move data from closed bands */
	if (FTL_BAND_STATE_CLOSED != band->md->state) {
		return false;
	}

	/* Band is already under relocation, skip it */
	if (band->reloc) {
		return false;
	}

	return true;
}

static void
get_band_phys_info(struct spdk_ftl_dev *dev, uint64_t phys_id,
		   double *invalidity, double *wr_cnt)
{
	struct ftl_band *band;
	uint64_t band_id = phys_id * dev->num_logical_bands_in_physical;

	*wr_cnt = *invalidity = 0.0L;
	for (; band_id < ftl_get_num_bands(dev); band_id++) {
		band = &dev->bands[band_id];

		if (phys_id != band->phys_id) {
			break;
		}

		*wr_cnt += band->md->wr_cnt;

		if (!is_band_relocateable(band)) {
			continue;
		}

		*invalidity += _band_invalidity(band);
	}

	*invalidity /= dev->num_logical_bands_in_physical;
	*wr_cnt /= dev->num_logical_bands_in_physical;
}

static bool
band_cmp(double a_invalidity, double a_wr_cnt,
	 double b_invalidity, double b_wr_cnt,
	 uint64_t a_id, uint64_t b_id)
{
	assert(a_id != FTL_BAND_PHYS_ID_INVALID);
	assert(b_id != FTL_BAND_PHYS_ID_INVALID);
	double diff = a_invalidity - b_invalidity;
	if (diff < 0.0L) {
		diff *= -1.0L;
	}

	/* Use the following metrics for picking bands for GC (in order):
	 * - relative invalidity
	 * - if invalidity is similar (within 10% points), then their write counts (how many times band was written to)
	 * - if write count is equal, then pick based on their placement on base device (lower LBAs win)
	 */
	if (diff > 0.1L) {
		return a_invalidity > b_invalidity;
	}

	if (a_wr_cnt != b_wr_cnt) {
		return a_wr_cnt < b_wr_cnt;
	}

	return a_id < b_id;
}

static void
band_start_gc(struct spdk_ftl_dev *dev, struct ftl_band *band)
{
	ftl_bug(false == is_band_relocateable(band));

	TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
	band->reloc = true;

	FTL_DEBUGLOG(dev, "Band to GC, id %u\n", band->id);
}

static struct ftl_band *
gc_high_priority_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	uint64_t high_prio_id = dev->sb_shm->gc_info.band_id_high_prio;

	if (FTL_BAND_ID_INVALID != high_prio_id) {
		ftl_bug(high_prio_id >= dev->num_bands);

		band = &dev->bands[high_prio_id];
		dev->sb_shm->gc_info.band_id_high_prio = FTL_BAND_ID_INVALID;

		band_start_gc(dev, band);
		FTL_NOTICELOG(dev, "GC takes high priority band, id %u\n", band->id);
		return band;
	}

	return 0;
}

static void
ftl_band_reset_gc_iter(struct spdk_ftl_dev *dev)
{
	dev->sb->gc_info.is_valid = 0;
	dev->sb->gc_info.current_band_id = FTL_BAND_ID_INVALID;
	dev->sb->gc_info.band_id_high_prio = FTL_BAND_ID_INVALID;
	dev->sb->gc_info.band_phys_id = FTL_BAND_PHYS_ID_INVALID;

	dev->sb_shm->gc_info = dev->sb->gc_info;
}

struct ftl_band *
ftl_band_search_next_to_reloc(struct spdk_ftl_dev *dev)
{
	double invalidity, max_invalidity = 0.0L;
	double wr_cnt, max_wr_cnt = 0.0L;
	uint64_t phys_id = FTL_BAND_PHYS_ID_INVALID;
	struct ftl_band *band;
	uint64_t i, band_count;
	uint64_t phys_count;

	band = gc_high_priority_band(dev);
	if (spdk_unlikely(NULL != band)) {
		return band;
	}

	phys_count = dev->num_logical_bands_in_physical;
	band_count = ftl_get_num_bands(dev);

	for (; dev->sb_shm->gc_info.current_band_id < band_count;) {
		band = &dev->bands[dev->sb_shm->gc_info.current_band_id];
		if (band->phys_id != dev->sb_shm->gc_info.band_phys_id) {
			break;
		}

		if (false == is_band_relocateable(band)) {
			dev->sb_shm->gc_info.current_band_id++;
			continue;
		}

		band_start_gc(dev, band);
		return band;
	}

	for (i = 0; i < band_count; i += phys_count) {
		band = &dev->bands[i];

		/* Calculate entire band physical group invalidity */
		get_band_phys_info(dev, band->phys_id, &invalidity, &wr_cnt);

		if (invalidity != 0.0L) {
			if (phys_id == FTL_BAND_PHYS_ID_INVALID ||
			    band_cmp(invalidity, wr_cnt, max_invalidity, max_wr_cnt,
				     band->phys_id, phys_id)) {
				max_wr_cnt = wr_cnt;
				phys_id = band->phys_id;

				if (invalidity > max_invalidity) {
					max_invalidity = invalidity;
				}
			}
		}
	}

	if (FTL_BAND_PHYS_ID_INVALID != phys_id) {
		FTL_DEBUGLOG(dev, "Band physical id %"PRIu64" to GC\n", phys_id);
		dev->sb_shm->gc_info.is_valid = 0;
		dev->sb_shm->gc_info.current_band_id = phys_id * phys_count;
		dev->sb_shm->gc_info.band_phys_id = phys_id;
		dev->sb_shm->gc_info.is_valid = 1;
		dump_bands_under_relocation(dev);
		return ftl_band_search_next_to_reloc(dev);
	} else {
		ftl_band_reset_gc_iter(dev);
	}

	return NULL;
}

void
ftl_band_init_gc_iter(struct spdk_ftl_dev *dev)
{
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_band_reset_gc_iter(dev);
		return;
	}

	if (dev->sb->clean) {
		dev->sb_shm->gc_info = dev->sb->gc_info;
		return;
	}

	if (ftl_fast_startup(dev) || ftl_fast_recovery(dev)) {
		return;
	}

	/* We lost GC state due to dirty shutdown, reset GC state to start over */
	ftl_band_reset_gc_iter(dev);
}

void
ftl_valid_map_load_state(struct spdk_ftl_dev *dev)
{
	uint64_t i;
	struct ftl_band *band;

	for (i = 0; i < dev->num_bands; i++) {
		band = &dev->bands[i];
		band->p2l_map.num_valid = ftl_bitmap_count_set(band->p2l_map.valid);
	}
}

void
ftl_band_initialize_free_state(struct ftl_band *band)
{
	/* All bands start on the shut list during startup, removing it manually here */
	TAILQ_REMOVE(&band->dev->shut_bands, band, queue_entry);
	_ftl_band_set_free(band);
}

void
ftl_bands_load_state(struct spdk_ftl_dev *dev)
{
	uint64_t i;
	struct ftl_band *band;

	for (i = 0; i < dev->num_bands; i++) {
		band = &dev->bands[i];

		if (band->md->state == FTL_BAND_STATE_FREE) {
			ftl_band_initialize_free_state(band);
		}
	}
}
