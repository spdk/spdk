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
#include "spdk/crc32.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/ftl.h"

#include "ftl_band.h"
#include "ftl_io.h"
#include "ftl_core.h"
#include "ftl_internal.h"
#include "utils/ftl_md.h"
#include "utils/ftl_defs.h"

static uint64_t
ftl_band_tail_md_offset(const struct ftl_band *band)
{
	return ftl_band_num_usable_blocks(band) -
	       ftl_tail_md_num_blocks(band->dev);
}

int
ftl_band_filled(struct ftl_band *band, size_t offset)
{
	return offset == ftl_band_tail_md_offset(band);
}

void
ftl_band_force_full(struct ftl_band *band)
{
	ftl_band_iter_init(band);
	ftl_band_iter_advance(band, ftl_band_tail_md_offset(band));
}

static void
ftl_band_free_lba_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;

	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);
	assert(lba_map->ref_cnt == 0);
	assert(lba_map->band_map != NULL);

	ftl_mempool_put(dev->lba_pool, lba_map->dma_buf);
	lba_map->band_map = NULL;
	lba_map->dma_buf = NULL;
}


static void
ftl_band_free_md_entry(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;

	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);

	ftl_mempool_put(dev->band_md_pool, lba_map->band_dma_md);
	lba_map->band_dma_md = NULL;
}

static void
_ftl_band_set_free(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Remove the band from the closed band list */
	TAILQ_INSERT_TAIL(&dev->free_bands, band, queue_entry);

	dev->num_free++;
	ftl_apply_limits(dev);
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
_ftl_band_set_closed(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Set the state as free_md() checks for that */
	band->md->state = FTL_BAND_STATE_CLOSED;
	if (band->owner.state_change_fn) {
		band->owner.state_change_fn(band);
	}

	/* Free the lba map if there are no outstanding IOs */
	ftl_band_release_lba_map(band);
	assert(band->lba_map.ref_cnt == 0);

	if (spdk_likely(band->num_zones)) {
		TAILQ_INSERT_TAIL(&dev->shut_bands, band, queue_entry);
	} else {
		TAILQ_REMOVE(&dev->shut_bands, band, queue_entry);
	}
}

ftl_addr
ftl_band_tail_md_addr(struct ftl_band *band)
{
	ftl_addr addr;
	struct ftl_zone *zone;
	struct spdk_ftl_dev *dev = band->dev;
	size_t xfer_size = dev->xfer_size;
	size_t num_req = ftl_band_tail_md_offset(band) / xfer_size;
	size_t i;

	if (spdk_unlikely(!band->num_zones)) {
		return FTL_ADDR_INVALID;
	}

	/* Metadata should be aligned to xfer size */
	assert(ftl_band_tail_md_offset(band) % xfer_size == 0);

	zone = CIRCLEQ_FIRST(&band->zones);
	for (i = 0; i < num_req % band->num_zones; ++i) {
		zone = ftl_band_next_zone(band, zone);
	}

	addr = (num_req / band->num_zones) * xfer_size;
	addr += zone->info.zone_id;

	return addr;
}

void
ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state)
{
	switch (state) {
	case FTL_BAND_STATE_FREE:
		assert(band->md->state == FTL_BAND_STATE_CLOSED);
		_ftl_band_set_free(band);

		band->md->lba_map_checksum = 0;
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
		band->md->lba_map_checksum = 0;
		break;
	default:
		break;
	}

	band->md->state = state;
}

void
ftl_band_set_type(struct ftl_band *band, enum ftl_band_type type)
{
	assert(type == FTL_BAND_TYPE_COMPACTION || type == FTL_BAND_TYPE_GC);

	switch (type) {
	case FTL_BAND_TYPE_COMPACTION:
	case FTL_BAND_TYPE_GC:
		band->md->type = type;
		break;
	default:
		break;
	}
}

void
ftl_band_set_addr(struct ftl_band *band, uint64_t lba, ftl_addr addr)
{
	struct ftl_lba_map *lba_map = &band->lba_map;
	uint64_t offset;

	offset = ftl_band_block_offset_from_addr(band, addr);

	lba_map->band_map[offset] = lba;
	lba_map->num_vld++;
}

size_t
ftl_band_num_usable_blocks(const struct ftl_band *band)
{
	return band->num_zones * ftl_get_num_blocks_in_zone(band->dev);
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

struct ftl_band *
ftl_band_from_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	size_t band_id = ftl_addr_get_band(dev, addr);

	assert(band_id < ftl_get_num_bands(dev));
	return &dev->bands[band_id];
}

struct ftl_zone *
ftl_band_zone_from_addr(struct ftl_band *band, ftl_addr addr)
{
	size_t pu_id = ftl_addr_get_punit(band->dev, addr);

	assert(pu_id < ftl_get_num_punits(band->dev));
	return &band->zone_buf[pu_id];
}

uint64_t
ftl_band_block_offset_from_addr(struct ftl_band *band, ftl_addr addr)
{
	assert(ftl_addr_get_band(band->dev, addr) == band->id);
	assert(ftl_addr_get_punit(band->dev, addr) <
	       ftl_get_num_punits(band->dev));
	return addr % ftl_get_num_blocks_in_band(band->dev);
}

ftl_addr
ftl_band_next_xfer_addr(struct ftl_band *band, ftl_addr addr, size_t num_blocks)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_zone *zone;
	size_t num_xfers, num_stripes;
	uint64_t offset;

	assert(ftl_addr_get_band(dev, addr) == band->id);

	offset = ftl_addr_get_zone_offset(dev, addr);
	zone = ftl_band_zone_from_addr(band, addr);

	num_blocks += (offset % dev->xfer_size);
	offset  -= (offset % dev->xfer_size);

#if defined(DEBUG)
	/* Check that the number of zones has not been changed */
	struct ftl_zone *_zone;
	size_t _num_zones = 0;
	CIRCLEQ_FOREACH(_zone, &band->zones, circleq) {
		if (spdk_likely(_zone->info.state != SPDK_BDEV_ZONE_STATE_OFFLINE)) {
			_num_zones++;
		}
	}
	assert(band->num_zones == _num_zones);
#endif
	assert(band->num_zones != 0);
	num_stripes = (num_blocks / dev->xfer_size) / band->num_zones;
	offset += num_stripes * dev->xfer_size;
	num_blocks -= num_stripes * dev->xfer_size * band->num_zones;

	if (offset > ftl_get_num_blocks_in_zone(dev)) {
		return FTL_ADDR_INVALID;
	}

	num_xfers = num_blocks / dev->xfer_size;
	for (size_t i = 0; i < num_xfers; ++i) {
		/* When the last zone is reached the block part of the address */
		/* needs to be increased by xfer_size */
		if (ftl_band_zone_is_last(band, zone)) {
			offset += dev->xfer_size;
			if (offset > ftl_get_num_blocks_in_zone(dev)) {
				return FTL_ADDR_INVALID;
			}
		}

		zone = ftl_band_next_operational_zone(band, zone);
		assert(zone);

		num_blocks -= dev->xfer_size;
	}

	if (num_blocks) {
		offset += num_blocks;
		if (offset > ftl_get_num_blocks_in_zone(dev)) {
			return FTL_ADDR_INVALID;
		}
	}

	addr = zone->info.zone_id + offset;
	return addr;
}

ftl_addr
ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off)
{
	ftl_addr addr;

	addr = block_off + band->id * ftl_get_num_blocks_in_band(band->dev);
	return addr;
}

ftl_addr
ftl_band_next_addr(struct ftl_band *band, ftl_addr addr, size_t offset)
{
	uint64_t block_off = ftl_band_block_offset_from_addr(band, addr);
	return ftl_band_addr_from_block_offset(band, block_off + offset);
}

void
ftl_band_acquire_lba_map(struct ftl_band *band)
{
	assert(band->lba_map.band_map != NULL);
	band->lba_map.ref_cnt++;
}

static int
ftl_band_alloc_md_entry(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_band_md];

	lba_map->band_dma_md = ftl_mempool_get(dev->band_md_pool);

	if (!lba_map->band_dma_md) {
		return -1;
	}

	memset(lba_map->band_dma_md, 0, region->entry_size * FTL_BLOCK_SIZE);
	return 0;
}

int
ftl_band_alloc_lba_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;

	assert(lba_map->ref_cnt == 0);
	assert(lba_map->band_map == NULL);

	lba_map->dma_buf = ftl_mempool_get(dev->lba_pool);
	if (!lba_map->dma_buf) {
		return -1;
	}

	if (ftl_band_alloc_md_entry(band)) {
		ftl_mempool_put(dev->lba_pool, lba_map->dma_buf);
		lba_map->dma_buf = NULL;
		return -1;
	}

	/* Set the P2L to FTL_LBA_INVALID */
	memset(lba_map->dma_buf, -1, FTL_BLOCK_SIZE * ftl_lba_map_num_blocks(band->dev));

	lba_map->band_map = lba_map->dma_buf;

	ftl_band_acquire_lba_map(band);
	return 0;
}

void
ftl_band_release_lba_map(struct ftl_band *band)
{
	struct ftl_lba_map *lba_map = &band->lba_map;

	assert(lba_map->band_map != NULL);
	assert(lba_map->ref_cnt > 0);
	lba_map->ref_cnt--;

	if (lba_map->ref_cnt == 0) {
		ftl_band_free_lba_map(band);
		ftl_band_free_md_entry(band);
	}
}

ftl_addr
ftl_band_lba_map_addr(struct ftl_band *band)
{
	return band->tail_md_addr;
}

void
ftl_band_remove_zone(struct ftl_band *band, struct ftl_zone *zone)
{
	CIRCLEQ_REMOVE(&band->zones, zone, circleq);
	band->num_zones--;
}

int
ftl_band_write_prep(struct ftl_band *band)
{
	if (ftl_band_alloc_lba_map(band)) {
		return -1;
	}

	ftl_band_iter_init(band);

	return 0;
}

struct ftl_zone *
ftl_band_next_operational_zone(struct ftl_band *band, struct ftl_zone *zone)
{
	struct ftl_zone *result = NULL;
	struct ftl_zone *entry;

	if (spdk_unlikely(!band->num_zones)) {
		return NULL;
	}

	/* Erasing band may fail after it was assigned to wptr. */
	/* In such a case zone is no longer in band->zones queue. */
	if (spdk_likely(zone->info.state != SPDK_BDEV_ZONE_STATE_OFFLINE)) {
		result = ftl_band_next_zone(band, zone);
	} else {
		CIRCLEQ_FOREACH_REVERSE(entry, &band->zones, circleq) {
			if (entry->info.zone_id > zone->info.zone_id) {
				result = entry;
			} else {
				if (!result) {
					result = CIRCLEQ_FIRST(&band->zones);
				}
				break;
			}
		}
	}

	return result;
}

void
ftl_band_clear_lba_map(struct ftl_band *band)
{
	struct ftl_lba_map *lba_map = &band->lba_map;

	memset(lba_map->band_map, -1, ftl_lba_map_num_blocks(band->dev) * FTL_BLOCK_SIZE);
	/* For open band all lba map segments are already cached */
	assert(band->md->state == FTL_BAND_STATE_PREP);
	lba_map->num_vld = 0;
}

size_t
ftl_lba_map_pool_elem_size(struct spdk_ftl_dev *dev)
{
	/* Map pool element holds the whole tail md */
	return ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE;
}
