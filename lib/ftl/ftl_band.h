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
#include "spdk/crc32.h"

#include "ftl_io.h"
#include "ftl_internal.h"
#include "ftl_core.h"

#include "utils/ftl_df.h"

#define FTL_MAX_OPEN_BANDS 4

#define FTL_BAND_VERSION_0	0
#define FTL_BAND_VERSION_1	1

#define FTL_BAND_VERSION_CURRENT FTL_BAND_VERSION_1

struct spdk_ftl_dev;
struct ftl_lba_map_request;
struct ftl_band;
struct ftl_rq;
struct ftl_basic_rq;

struct ftl_zone {
	struct spdk_bdev_zone_info	info;

	/* Indicates that there is inflight write */
	bool						busy;

	CIRCLEQ_ENTRY(ftl_zone)		circleq;
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

typedef void (*ftl_band_state_change_fn)(struct ftl_band *band);
typedef void (*ftl_band_ops_cb)(struct ftl_band *band, void *ctx, bool status);
typedef void (*ftl_band_md_cb)(struct ftl_band *band, void *ctx, enum ftl_md_status status);

struct ftl_band_md {
	/* Band iterator for writing */
	struct {
		/* Current address */
		ftl_addr			addr;

		/* Current logical block's offset */
		uint64_t			offset;
	} iter;

	/* Band's state */
	enum ftl_band_state		state;

	/* Band type set during opening */
	enum ftl_band_type		type;

	/* Number of defrag cycles */
	uint64_t				wr_cnt;

	/* Durable format object id for LBA map, allocated on shared memory */
	ftl_df_obj_id			df_lba_map;

	/* CRC32 checksum of the associated LBA map when band is in closed state */
	uint32_t				lba_map_checksum;
} __attribute__((aligned(FTL_BLOCK_SIZE)));

SPDK_STATIC_ASSERT(sizeof(struct ftl_band_md) == FTL_BLOCK_SIZE, "Incorrect metadata size");

struct ftl_band {
	/* Device this band belongs to */
	struct spdk_ftl_dev			*dev;

	struct ftl_band_md			*md;

	/* Current zone */
	struct ftl_zone				*zone;

	/* IO queue depth (outstanding IOs) */
	uint64_t					queue_depth;

	/* Fields for owner of the band - writer, or gc */
	struct {
		/* Callback context for the owner */
		void *priv;

		/* State change callback */
		ftl_band_state_change_fn state_change_fn;

		/* Callback for the owner */
		union {
			ftl_band_ops_cb ops_fn;
			ftl_band_md_cb md_fn;
		};

		/* Reference counter */
		uint64_t cnt;
	} owner;

	/* Number of operational zones */
	size_t						num_zones;

	/* Array of zones */
	struct ftl_zone				*zone_buf;

	/* List of operational zones */
	CIRCLEQ_HEAD(, ftl_zone)	zones;

	/* LBA map */
	struct ftl_lba_map			lba_map;

	/* Band under reloc */
	bool						reloc;

	/* Band's index */
	uint32_t					id;

	/* Band's NAND id - a group multiple bands may be part of the same physical band on base device
	 * This way the write access pattern will match the actual physical layout more closely, leading
	 * to lower overall write amplification factor
	 */
	uint32_t					phys_id;

	/* End metadata start addr */
	ftl_addr					tail_md_addr;

	/* Metadata request */
	struct ftl_basic_rq			metadata_rq;

	/* Free/shut bands' lists */
	TAILQ_ENTRY(ftl_band)		queue_entry;

	/* For writing metadata */
	struct ftl_md_io_entry_ctx		md_persist_entry_ctx;
};


uint64_t ftl_band_block_offset_from_addr(struct ftl_band *band, ftl_addr addr);
ftl_addr ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off);
void ftl_band_set_type(struct ftl_band *band, enum ftl_band_type type);
void ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state);
void ftl_band_acquire_lba_map(struct ftl_band *band);
int ftl_band_alloc_lba_map(struct ftl_band *band);
int ftl_band_open_lba_map(struct ftl_band *band);
void ftl_band_clear_lba_map(struct ftl_band *band);
void ftl_band_release_lba_map(struct ftl_band *band);
ftl_addr ftl_band_next_xfer_addr(struct ftl_band *band, ftl_addr addr, size_t num_blocks);
ftl_addr ftl_band_next_addr(struct ftl_band *band, ftl_addr addr, size_t offset);
size_t ftl_band_num_usable_blocks(const struct ftl_band *band);
size_t ftl_band_user_blocks_left(const struct ftl_band *band, size_t offset);
size_t ftl_band_user_blocks(const struct ftl_band *band);
void ftl_band_set_addr(struct ftl_band *band, uint64_t lba, ftl_addr addr);
struct ftl_band *ftl_band_from_addr(struct spdk_ftl_dev *dev, ftl_addr addr);
struct ftl_zone *ftl_band_zone_from_addr(struct ftl_band *band, ftl_addr);
ftl_addr ftl_band_tail_md_addr(struct ftl_band *band);
int ftl_band_filled(struct ftl_band *band, size_t offset);
void ftl_band_force_full(struct ftl_band *band);
int ftl_band_write_prep(struct ftl_band *band);
struct ftl_zone *ftl_band_next_operational_zone(struct ftl_band *band,
		struct ftl_zone *zone);
size_t ftl_lba_map_pool_elem_size(struct spdk_ftl_dev *dev);
void ftl_band_remove_zone(struct ftl_band *band, struct ftl_zone *zone);
struct ftl_band *ftl_band_search_next_to_defrag(struct spdk_ftl_dev *dev);
void ftl_band_init_gc_iter(struct spdk_ftl_dev *dev);
ftl_addr ftl_band_lba_map_addr(struct ftl_band *band);
void ftl_valid_map_load_state(struct spdk_ftl_dev *dev);
void ftl_bands_load_state(struct spdk_ftl_dev *dev);
void ftl_band_open(struct ftl_band *band, enum ftl_band_type type);
void ftl_band_close(struct ftl_band *band);
void ftl_band_free(struct ftl_band *band);
void ftl_band_rq_write(struct ftl_band *band, struct ftl_rq *rq);
void ftl_band_rq_read(struct ftl_band *band, struct ftl_rq *rq);
void ftl_band_basic_rq_write(struct ftl_band *band, struct ftl_basic_rq *brq);
void ftl_band_basic_rq_read(struct ftl_band *band, struct ftl_basic_rq *brq);
void ftl_band_get_next_gc(struct spdk_ftl_dev *dev, ftl_band_ops_cb cb, void *cntx);
void ftl_band_read_tail_brq_md(struct ftl_band *band, ftl_band_md_cb cb, void *cntx);

static inline void ftl_band_set_owner(struct ftl_band *band,
				      ftl_band_state_change_fn fn,
				      void *priv)
{
	assert(NULL == band->owner.priv);
	assert(NULL == band->owner.state_change_fn);

	band->owner.state_change_fn = fn;
	band->owner.priv = priv;
}

static inline void ftl_band_clear_owner(struct ftl_band *band,
					ftl_band_state_change_fn fn,
					void *priv)
{
	assert(priv == band->owner.priv);
	assert(fn == band->owner.state_change_fn);

	band->owner.state_change_fn = NULL;
	band->owner.priv = NULL;
}

static inline int
ftl_band_empty(const struct ftl_band *band)
{
	return band->lba_map.num_vld == 0;
}

static inline uint64_t
ftl_band_qd(const struct ftl_band *band)
{
	return band->queue_depth;
}

static inline struct ftl_zone *
ftl_band_next_zone(struct ftl_band *band, struct ftl_zone *zone)
{
	assert(zone->info.state != SPDK_BDEV_ZONE_STATE_OFFLINE);
	return CIRCLEQ_LOOP_NEXT(&band->zones, zone, circleq);
}

static inline int
ftl_band_block_offset_valid(struct ftl_band *band, size_t block_off)
{
	struct ftl_lba_map *lba_map = &band->lba_map;

	if (ftl_bitmap_get(lba_map->vld, block_off)) {
		return 1;
	}
	return 0;
}

static inline int
ftl_band_zone_is_last(struct ftl_band *band, struct ftl_zone *zone)
{
	return zone == CIRCLEQ_LAST(&band->zones);
}

static inline int
ftl_zone_is_writable(const struct spdk_ftl_dev *dev, const struct ftl_zone *zone)
{
	bool busy = ftl_is_append_supported(dev) ? false : zone->busy;

	return (zone->info.state == SPDK_BDEV_ZONE_STATE_OPEN ||
		zone->info.state == SPDK_BDEV_ZONE_STATE_EMPTY) &&
	       !busy;
}

static inline void
ftl_band_iter_init(struct ftl_band *band)
{
	/* Initialize band iterator to begin state */
	band->zone = CIRCLEQ_FIRST(&band->zones);
	band->md->iter.addr = band->zone->info.zone_id;
	band->md->iter.offset = 0;
}

static inline void
ftl_band_iter_advance(struct ftl_band *band, uint64_t num_blocks)
{
	band->md->iter.offset += num_blocks;
	band->zone->busy = true;
	band->md->iter.addr = ftl_band_next_xfer_addr(band, band->md->iter.addr, num_blocks);
	band->zone = ftl_band_next_operational_zone(band, band->zone);
	assert(band->md->iter.addr != FTL_ADDR_INVALID);
}

static inline void
ftl_band_iter_set(struct ftl_band *band, uint64_t num_blocks)
{
	band->md->iter.offset = num_blocks;
	band->md->iter.addr = ftl_band_next_xfer_addr(band, band->md->iter.addr, num_blocks);
	band->zone = ftl_band_next_operational_zone(band, band->zone);
	assert(band->md->iter.addr != FTL_ADDR_INVALID);
}

#endif /* FTL_BAND_H */
