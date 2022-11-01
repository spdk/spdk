/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_BAND_H
#define FTL_BAND_H

#include "spdk/stdinc.h"
#include "spdk/bit_array.h"
#include "spdk/queue.h"
#include "spdk/crc32.h"

#include "ftl_io.h"
#include "ftl_internal.h"
#include "ftl_core.h"

#include "utils/ftl_df.h"

#define FTL_MAX_OPEN_BANDS FTL_LAYOUT_REGION_TYPE_P2L_COUNT

#define FTL_BAND_VERSION_0	0
#define FTL_BAND_VERSION_1	1

#define FTL_BAND_VERSION_CURRENT FTL_BAND_VERSION_1

struct spdk_ftl_dev;
struct ftl_band;
struct ftl_rq;
struct ftl_basic_rq;

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
typedef void (*ftl_band_validate_md_cb)(struct ftl_band *band, bool valid);

struct ftl_band_md {
	/* Band iterator for writing */
	struct {
		/* Current physical address of the write pointer */
		ftl_addr		addr;

		/* Offset from the band's start of the write pointer */
		uint64_t		offset;
	} iter;

	/* Band's state */
	enum ftl_band_state		state;

	/* Band type set during opening */
	enum ftl_band_type		type;

	/* nv_cache p2l md region associated with band */
	enum ftl_layout_region_type	p2l_md_region;

	/* Reserved */
	uint8_t				reserved[4];

	/* Sequence ID when band was opened */
	uint64_t			seq;

	/* Sequence ID when band was closed */
	uint64_t			close_seq_id;

	/* Number of times band was fully written (ie. number of free -> closed state cycles) */
	uint64_t			wr_cnt;

	/* Durable format object id for P2L map, allocated on shared memory */
	ftl_df_obj_id			df_p2l_map;

	/* CRC32 checksum of the associated P2L map when band is in closed state */
	uint32_t			p2l_map_checksum;

	/* Reserved */
	uint8_t				reserved2[4028];
} __attribute__((packed));

SPDK_STATIC_ASSERT(sizeof(struct ftl_band_md) == FTL_BLOCK_SIZE, "Incorrect metadata size");

struct ftl_band {
	/* Device this band belongs to */
	struct spdk_ftl_dev		*dev;

	struct ftl_band_md		*md;

	/* IO queue depth (outstanding IOs) */
	uint64_t			queue_depth;

	/* Fields for owner of the band - compaction, or gc */
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

	/* P2L map */
	struct ftl_p2l_map		p2l_map;

	/* Band relocation is in progress */
	bool				reloc;

	/* Band's index */
	uint32_t			id;

	/* Band's NAND id - a group multiple bands may be part of the same physical band on base device
	 * This way the write access pattern will match the actual physical layout more closely, leading
	 * to lower overall write amplification factor
	 */
	uint32_t			phys_id;

	/* Band start addr */
	ftl_addr			start_addr;

	/* End metadata start addr */
	ftl_addr			tail_md_addr;

	/* Metadata request */
	struct ftl_basic_rq		metadata_rq;

	/* Free/shut bands' lists
	 * Open bands are kept and managed directly by the writer (either GC or compaction). Each writer only
	 * needs to keep two bands (one currently written to, and a pre-assigned reserve band to make sure flow
	 * of data is always ongoing as the current one is closing).
	 */
	TAILQ_ENTRY(ftl_band)		queue_entry;

	/* For writing metadata */
	struct ftl_md_io_entry_ctx	md_persist_entry_ctx;

	/* Callback function for validate md */
	ftl_band_validate_md_cb		validate_cb;
};


uint64_t ftl_band_block_offset_from_addr(struct ftl_band *band, ftl_addr addr);
ftl_addr ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off);
void ftl_band_set_type(struct ftl_band *band, enum ftl_band_type type);
void ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state);
void ftl_band_acquire_p2l_map(struct ftl_band *band);
int ftl_band_alloc_p2l_map(struct ftl_band *band);
int ftl_band_open_p2l_map(struct ftl_band *band);
void ftl_band_release_p2l_map(struct ftl_band *band);
ftl_addr ftl_band_next_xfer_addr(struct ftl_band *band, ftl_addr addr, size_t num_blocks);
ftl_addr ftl_band_next_addr(struct ftl_band *band, ftl_addr addr, size_t offset);
size_t ftl_band_user_blocks_left(const struct ftl_band *band, size_t offset);
size_t ftl_band_user_blocks(const struct ftl_band *band);
void ftl_band_set_p2l(struct ftl_band *band, uint64_t lba, ftl_addr addr, uint64_t seq_id);
void ftl_band_set_addr(struct ftl_band *band, uint64_t lba, ftl_addr addr);
struct ftl_band *ftl_band_from_addr(struct spdk_ftl_dev *dev, ftl_addr addr);
ftl_addr ftl_band_tail_md_addr(struct ftl_band *band);
int ftl_band_filled(struct ftl_band *band, size_t offset);
int ftl_band_write_prep(struct ftl_band *band);
size_t ftl_p2l_map_pool_elem_size(struct spdk_ftl_dev *dev);
struct ftl_band *ftl_band_search_next_to_reloc(struct spdk_ftl_dev *dev);
void ftl_band_init_gc_iter(struct spdk_ftl_dev *dev);
ftl_addr ftl_band_p2l_map_addr(struct ftl_band *band);
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
void ftl_band_initialize_free_state(struct ftl_band *band);

static inline void
ftl_band_set_owner(struct ftl_band *band,
		   ftl_band_state_change_fn fn,
		   void *priv)
{
	assert(NULL == band->owner.priv);
	assert(NULL == band->owner.state_change_fn);

	band->owner.state_change_fn = fn;
	band->owner.priv = priv;
}

static inline void
ftl_band_clear_owner(struct ftl_band *band,
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
	return band->p2l_map.num_valid == 0;
}

static inline uint64_t
ftl_band_qd(const struct ftl_band *band)
{
	return band->queue_depth;
}

static inline bool
ftl_band_block_offset_valid(struct ftl_band *band, size_t block_off)
{
	struct ftl_p2l_map *p2l_map = &band->p2l_map;

	return ftl_bitmap_get(p2l_map->valid, block_off);
}

static inline void
ftl_band_iter_init(struct ftl_band *band)
{
	/* Initialize band iterator to begin state */
	band->md->iter.addr = band->start_addr;
	band->md->iter.offset = 0;
}

static inline void
ftl_band_iter_advance(struct ftl_band *band, uint64_t num_blocks)
{
	band->md->iter.offset += num_blocks;
	band->md->iter.addr = ftl_band_next_xfer_addr(band, band->md->iter.addr, num_blocks);
	assert(band->md->iter.addr != FTL_ADDR_INVALID);
}

static inline void
ftl_band_iter_set(struct ftl_band *band, uint64_t num_blocks)
{
	band->md->iter.offset = num_blocks;
	band->md->iter.addr = ftl_band_next_xfer_addr(band, band->md->iter.addr, num_blocks);
	assert(band->md->iter.addr != FTL_ADDR_INVALID);
}

#endif /* FTL_BAND_H */
