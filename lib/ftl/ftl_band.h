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

#ifndef FTL_BAND_H
#define FTL_BAND_H

#include "spdk/stdinc.h"
#include "spdk/bit_array.h"
#include "spdk/queue.h"
#include "spdk/bdev_zone.h"

#include "ftl_io.h"
#include "ftl_addr.h"
#include "ftl_core.h"

/* Number of LBAs that could be stored in a single block */
#define FTL_NUM_LBA_IN_BLOCK	(FTL_BLOCK_SIZE / sizeof(uint64_t))

struct spdk_ftl_dev;
struct ftl_lba_map_request;

struct ftl_zone {
	struct spdk_bdev_zone_info		info;

	/* Indicates that there is inflight write */
	bool					busy;

	CIRCLEQ_ENTRY(ftl_zone)			circleq;
};

enum ftl_md_status {
	FTL_MD_SUCCESS,
	/* Metadata read failure */
	FTL_MD_IO_FAILURE,
	/* Invalid version */
	FTL_MD_INVALID_VER,
	/* UUID doesn't match */
	FTL_MD_NO_MD,
	/* UUID and version matches but CRC doesn't */
	FTL_MD_INVALID_CRC,
	/* Vld or lba map size doesn't match */
	FTL_MD_INVALID_SIZE
};

enum ftl_lba_map_seg_state {
	FTL_LBA_MAP_SEG_CLEAR,
	FTL_LBA_MAP_SEG_PENDING,
	FTL_LBA_MAP_SEG_CACHED
};

struct ftl_lba_map {
	/* LBA/vld map lock */
	pthread_spinlock_t			lock;

	/* Number of valid LBAs */
	size_t					num_vld;

	/* LBA map's reference count */
	size_t					ref_cnt;

	/* Bitmap of valid LBAs */
	struct spdk_bit_array			*vld;

	/* LBA map (only valid for open/relocating bands) */
	uint64_t				*map;

	/* LBA map segment state map (clear, pending, cached) */
	uint8_t					*segments;

	LIST_HEAD(, ftl_lba_map_request)	request_list;

	/* Metadata DMA buffer (only valid for open/relocating bands) */
	void					*dma_buf;
};

enum ftl_band_state {
	FTL_BAND_STATE_FREE,
	FTL_BAND_STATE_PREP,
	FTL_BAND_STATE_OPENING,
	FTL_BAND_STATE_OPEN,
	FTL_BAND_STATE_FULL,
	FTL_BAND_STATE_CLOSING,
	FTL_BAND_STATE_CLOSED,
	FTL_BAND_STATE_MAX
};

struct ftl_lba_map_request {
	/*  Completion callback */
	ftl_io_fn				cb;

	/*  Completion callback context */
	void					*cb_ctx;

	/* Bit array of requested segments */
	struct spdk_bit_array			*segments;

	/* Number of pending segments to read */
	size_t					num_pending;

	LIST_ENTRY(ftl_lba_map_request)		list_entry;
};

struct ftl_band {
	/* Device this band belongs to */
	struct spdk_ftl_dev			*dev;

	/* Number of operational zones */
	size_t					num_zones;

	/* Array of zones */
	struct ftl_zone				*zone_buf;

	/* List of operational zones */
	CIRCLEQ_HEAD(, ftl_zone)		zones;

	/* LBA map */
	struct ftl_lba_map			lba_map;

	/* Band's state */
	enum ftl_band_state			state;

	/* Band's index */
	unsigned int				id;

	/* Latest merit calculation */
	double					merit;

	/* High defrag priority - means that the metadata should be copied and */
	/* the band should be defragged immediately */
	int					high_prio;

	/* Sequence number */
	uint64_t				seq;

	/* Number of defrag cycles */
	uint64_t				wr_cnt;

	/* End metadata start addr */
	struct ftl_addr				tail_md_addr;

	/* Bitmap of all bands that have its data moved onto this band */
	struct spdk_bit_array			*reloc_bitmap;
	/* Number of open bands containing data moved from this band */
	size_t					num_reloc_bands;
	/* Number of blocks currently being moved from this band */
	size_t					num_reloc_blocks;

	/* Free/shut bands' lists */
	LIST_ENTRY(ftl_band)			list_entry;

	/* High priority queue link */
	STAILQ_ENTRY(ftl_band)			prio_stailq;
};

uint64_t	ftl_band_block_offset_from_addr(struct ftl_band *band, struct ftl_addr addr);
struct ftl_addr ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off);
void		ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state);
size_t		ftl_band_age(const struct ftl_band *band);
void		ftl_band_acquire_lba_map(struct ftl_band *band);
int		ftl_band_alloc_lba_map(struct ftl_band *band);
void		ftl_band_clear_lba_map(struct ftl_band *band);
void		ftl_band_release_lba_map(struct ftl_band *band);
int		ftl_band_read_lba_map(struct ftl_band *band,
				      size_t offset, size_t lba_cnt,
				      ftl_io_fn cb_fn, void *cb_ctx);
struct ftl_addr ftl_band_next_xfer_addr(struct ftl_band *band, struct ftl_addr addr,
					size_t num_blocks);
struct ftl_addr ftl_band_next_addr(struct ftl_band *band, struct ftl_addr addr,
				   size_t offset);
size_t		ftl_band_num_usable_blocks(const struct ftl_band *band);
size_t		ftl_band_user_blocks_left(const struct ftl_band *band, size_t offset);
size_t		ftl_band_user_blocks(const struct ftl_band *band);
void		ftl_band_set_addr(struct ftl_band *band, uint64_t lba,
				  struct ftl_addr addr);
struct ftl_band *ftl_band_from_addr(struct spdk_ftl_dev *dev, struct ftl_addr addr);
struct ftl_zone *ftl_band_zone_from_addr(struct ftl_band *band, struct ftl_addr);
void		ftl_band_md_clear(struct ftl_band *band);
int		ftl_band_read_tail_md(struct ftl_band *band, struct ftl_addr,
				      ftl_io_fn cb_fn, void *cb_ctx);
int		ftl_band_read_head_md(struct ftl_band *band, ftl_io_fn cb_fn, void *cb_ctx);
int		ftl_band_write_tail_md(struct ftl_band *band, ftl_io_fn cb);
int		ftl_band_write_head_md(struct ftl_band *band, ftl_io_fn cb);
struct ftl_addr ftl_band_tail_md_addr(struct ftl_band *band);
struct ftl_addr ftl_band_head_md_addr(struct ftl_band *band);
void		ftl_band_write_failed(struct ftl_band *band);
int		ftl_band_full(struct ftl_band *band, size_t offset);
int		ftl_band_write_prep(struct ftl_band *band);
struct ftl_zone *ftl_band_next_operational_zone(struct ftl_band *band,
		struct ftl_zone *zone);
size_t		ftl_lba_map_pool_elem_size(struct spdk_ftl_dev *dev);
void		ftl_band_remove_zone(struct ftl_band *band, struct ftl_zone *zone);


static inline int
ftl_band_empty(const struct ftl_band *band)
{
	return band->lba_map.num_vld == 0;
}

static inline struct ftl_zone *
ftl_band_next_zone(struct ftl_band *band, struct ftl_zone *zone)
{
	assert(zone->info.state != SPDK_BDEV_ZONE_STATE_OFFLINE);
	return CIRCLEQ_LOOP_NEXT(&band->zones, zone, circleq);
}

static inline void
ftl_band_set_next_state(struct ftl_band *band)
{
	ftl_band_set_state(band, (band->state + 1) % FTL_BAND_STATE_MAX);
}

static inline int
ftl_band_state_changing(struct ftl_band *band)
{
	return band->state == FTL_BAND_STATE_OPENING ||
	       band->state == FTL_BAND_STATE_CLOSING;
}

static inline int
ftl_band_block_offset_valid(struct ftl_band *band, size_t block_off)
{
	struct ftl_lba_map *lba_map = &band->lba_map;

	pthread_spin_lock(&lba_map->lock);
	if (spdk_bit_array_get(lba_map->vld, block_off)) {
		pthread_spin_unlock(&lba_map->lock);
		return 1;
	}

	pthread_spin_unlock(&lba_map->lock);
	return 0;
}

static inline int
ftl_band_zone_is_last(struct ftl_band *band, struct ftl_zone *zone)
{
	return zone == CIRCLEQ_LAST(&band->zones);
}

static inline int
ftl_band_zone_is_first(struct ftl_band *band, struct ftl_zone *zone)
{
	return zone == CIRCLEQ_FIRST(&band->zones);
}

static inline int
ftl_zone_is_writable(const struct spdk_ftl_dev *dev, const struct ftl_zone *zone)
{
	bool busy = ftl_is_append_supported(dev) ? false : zone->busy;

	return (zone->info.state == SPDK_BDEV_ZONE_STATE_OPEN ||
		zone->info.state == SPDK_BDEV_ZONE_STATE_EMPTY) &&
	       !busy;
}

#endif /* FTL_BAND_H */
