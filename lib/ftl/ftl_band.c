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

#include "spdk/crc32.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/ftl.h"

#include "ftl_band.h"
#include "ftl_io.h"
#include "ftl_core.h"
#include "ftl_reloc.h"
#include "ftl_debug.h"

/* TODO: define some signature for meta version */
#define FTL_MD_VER 1

struct __attribute__((packed)) ftl_md_hdr {
	/* Device instance */
	struct spdk_uuid	uuid;

	/* Meta version */
	uint8_t			ver;

	/* Sequence number */
	uint64_t		seq;

	/* CRC32 checksum */
	uint32_t		checksum;
};

/* End metadata layout stored on media (with all three being aligned to block size): */
/* - header */
/* - valid bitmap */
/* - LBA map */
struct __attribute__((packed)) ftl_tail_md {
	struct ftl_md_hdr	hdr;

	/* Max number of blocks */
	uint64_t		num_blocks;

	uint8_t			reserved[4059];
};
SPDK_STATIC_ASSERT(sizeof(struct ftl_tail_md) == FTL_BLOCK_SIZE, "Incorrect metadata size");

struct __attribute__((packed)) ftl_head_md {
	struct ftl_md_hdr	hdr;

	/* Number of defrag cycles */
	uint64_t		wr_cnt;

	/* Number of surfaced LBAs */
	uint64_t		lba_cnt;

	/* Transfer size */
	uint32_t		xfer_size;
};

size_t
ftl_tail_md_hdr_num_blocks(void)
{
	return spdk_divide_round_up(sizeof(struct ftl_tail_md), FTL_BLOCK_SIZE);
}

size_t
ftl_vld_map_num_blocks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_vld_map_size(dev), FTL_BLOCK_SIZE);
}

size_t
ftl_lba_map_num_blocks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_get_num_blocks_in_band(dev) * sizeof(uint64_t), FTL_BLOCK_SIZE);
}

size_t
ftl_head_md_num_blocks(const struct spdk_ftl_dev *dev)
{
	return dev->xfer_size;
}

size_t
ftl_tail_md_num_blocks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_tail_md_hdr_num_blocks() +
				    ftl_vld_map_num_blocks(dev) +
				    ftl_lba_map_num_blocks(dev),
				    dev->xfer_size) * dev->xfer_size;
}

static uint64_t
ftl_band_tail_md_offset(const struct ftl_band *band)
{
	return ftl_band_num_usable_blocks(band) -
	       ftl_tail_md_num_blocks(band->dev);
}

int
ftl_band_full(struct ftl_band *band, size_t offset)
{
	return offset == ftl_band_tail_md_offset(band);
}

void
ftl_band_write_failed(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	band->high_prio = 1;

	ftl_reloc_add(dev->reloc, band, 0, ftl_get_num_blocks_in_band(dev), 1, true);
	ftl_band_set_state(band, FTL_BAND_STATE_CLOSED);
}

static void
ftl_band_free_lba_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;

	assert(band->state == FTL_BAND_STATE_CLOSED ||
	       band->state == FTL_BAND_STATE_FREE);
	assert(lba_map->ref_cnt == 0);
	assert(lba_map->map != NULL);
	assert(!band->high_prio);

	/* Verify that band's metadata is consistent with l2p */
	if (band->num_zones) {
		assert(ftl_band_validate_md(band) == true);
	}

	spdk_mempool_put(dev->lba_pool, lba_map->dma_buf);
	lba_map->map = NULL;
	lba_map->dma_buf = NULL;
}

static void
_ftl_band_set_free(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_band *lband, *prev;

	/* Remove the band from the closed band list */
	LIST_REMOVE(band, list_entry);

	/* Keep the list sorted by band's write count */
	LIST_FOREACH(lband, &dev->free_bands, list_entry) {
		if (lband->wr_cnt > band->wr_cnt) {
			LIST_INSERT_BEFORE(lband, band, list_entry);
			break;
		}
		prev = lband;
	}

	if (!lband) {
		if (LIST_EMPTY(&dev->free_bands)) {
			LIST_INSERT_HEAD(&dev->free_bands, band, list_entry);
		} else {
			LIST_INSERT_AFTER(prev, band, list_entry);
		}
	}

#if defined(DEBUG)
	prev = NULL;
	LIST_FOREACH(lband, &dev->free_bands, list_entry) {
		if (!prev) {
			continue;
		}
		assert(prev->wr_cnt <= lband->wr_cnt);
	}
#endif
	dev->num_free++;
	ftl_apply_limits(dev);
}

static void
_ftl_band_set_preparing(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Remove band from free list */
	LIST_REMOVE(band, list_entry);

	band->wr_cnt++;

	assert(dev->num_free > 0);
	dev->num_free--;

	ftl_apply_limits(dev);
}

static void
_ftl_band_set_closed(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;

	/* Set the state as free_md() checks for that */
	band->state = FTL_BAND_STATE_CLOSED;

	/* Free the lba map if there are no outstanding IOs */
	ftl_band_release_lba_map(band);

	if (spdk_likely(band->num_zones)) {
		LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
	} else {
		LIST_REMOVE(band, list_entry);
	}
}

static uint32_t
ftl_md_calc_crc(const struct ftl_md_hdr *hdr, size_t size)
{
	size_t checkoff = offsetof(struct ftl_md_hdr, checksum);
	size_t mdoff = checkoff + sizeof(hdr->checksum);
	uint32_t crc;

	crc = spdk_crc32c_update(hdr, checkoff, 0);
	return spdk_crc32c_update((const char *)hdr + mdoff, size - mdoff, crc);
}

static void
ftl_set_md_hdr(struct ftl_band *band, struct ftl_md_hdr *hdr, size_t size)
{
	hdr->seq = band->seq;
	hdr->ver = FTL_MD_VER;
	hdr->uuid = band->dev->uuid;
	hdr->checksum = ftl_md_calc_crc(hdr, size);
}

static int
ftl_pack_head_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_head_md *head = band->lba_map.dma_buf;

	head->wr_cnt = band->wr_cnt;
	head->lba_cnt = dev->num_lbas;
	head->xfer_size = dev->xfer_size;
	ftl_set_md_hdr(band, &head->hdr, sizeof(struct ftl_head_md));

	return FTL_MD_SUCCESS;
}

static int
ftl_pack_tail_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_tail_md *tail = lba_map->dma_buf;
	void *vld_offset;

	vld_offset = (char *)tail + ftl_tail_md_hdr_num_blocks() * FTL_BLOCK_SIZE;

	/* Clear out the buffer */
	memset(tail, 0, ftl_tail_md_hdr_num_blocks() * FTL_BLOCK_SIZE);
	tail->num_blocks = ftl_get_num_blocks_in_band(dev);

	pthread_spin_lock(&lba_map->lock);
	spdk_bit_array_store_mask(lba_map->vld, vld_offset);
	pthread_spin_unlock(&lba_map->lock);

	ftl_set_md_hdr(band, &tail->hdr, ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE);

	return FTL_MD_SUCCESS;
}

static int
ftl_md_hdr_vld(struct spdk_ftl_dev *dev, const struct ftl_md_hdr *hdr, size_t size)
{
	if (spdk_uuid_compare(&dev->uuid, &hdr->uuid) != 0) {
		return FTL_MD_NO_MD;
	}

	if (hdr->ver != FTL_MD_VER) {
		return FTL_MD_INVALID_VER;
	}

	if (ftl_md_calc_crc(hdr, size) != hdr->checksum) {
		return FTL_MD_INVALID_CRC;
	}

	return FTL_MD_SUCCESS;
}

static int
ftl_unpack_tail_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	void *vld_offset;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_tail_md *tail = lba_map->dma_buf;
	int rc;

	vld_offset = (char *)tail + ftl_tail_md_hdr_num_blocks() * FTL_BLOCK_SIZE;

	rc = ftl_md_hdr_vld(dev, &tail->hdr, ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE);
	if (rc) {
		return rc;
	}

	/*
	 * When restoring from a dirty shutdown it's possible old tail meta wasn't yet cleared -
	 * band had saved head meta, but didn't manage to send erase to all zones.
	 * The already found tail md header is valid, but inconsistent with the head meta. Treat
	 * such a band as open/without valid tail md.
	 */
	if (band->seq != tail->hdr.seq) {
		return FTL_MD_NO_MD;
	}

	if (tail->num_blocks != ftl_get_num_blocks_in_band(dev)) {
		return FTL_MD_INVALID_SIZE;
	}

	spdk_bit_array_load_mask(lba_map->vld, vld_offset);

	return FTL_MD_SUCCESS;
}

static int
ftl_unpack_head_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_head_md *head = band->lba_map.dma_buf;
	int rc;

	rc = ftl_md_hdr_vld(dev, &head->hdr, sizeof(struct ftl_head_md));
	if (rc) {
		return rc;
	}

	band->seq = head->hdr.seq;
	band->wr_cnt = head->wr_cnt;

	if (dev->global_md.num_lbas == 0) {
		dev->global_md.num_lbas = head->lba_cnt;
	}

	if (dev->global_md.num_lbas != head->lba_cnt) {
		return FTL_MD_INVALID_SIZE;
	}

	if (dev->xfer_size != head->xfer_size) {
		return FTL_MD_INVALID_SIZE;
	}

	return FTL_MD_SUCCESS;
}

struct ftl_addr
ftl_band_tail_md_addr(struct ftl_band *band)
{
	struct ftl_addr addr = {};
	struct ftl_zone *zone;
	struct spdk_ftl_dev *dev = band->dev;
	size_t xfer_size = dev->xfer_size;
	size_t num_req = ftl_band_tail_md_offset(band) / xfer_size;
	size_t i;

	if (spdk_unlikely(!band->num_zones)) {
		return ftl_to_addr(FTL_ADDR_INVALID);
	}

	/* Metadata should be aligned to xfer size */
	assert(ftl_band_tail_md_offset(band) % xfer_size == 0);

	zone = CIRCLEQ_FIRST(&band->zones);
	for (i = 0; i < num_req % band->num_zones; ++i) {
		zone = ftl_band_next_zone(band, zone);
	}

	addr.offset = (num_req / band->num_zones) * xfer_size;
	addr.offset += zone->info.zone_id;

	return addr;
}

struct ftl_addr
ftl_band_head_md_addr(struct ftl_band *band)
{
	if (spdk_unlikely(!band->num_zones)) {
		return ftl_to_addr(FTL_ADDR_INVALID);
	}

	return ftl_to_addr(CIRCLEQ_FIRST(&band->zones)->info.zone_id);
}

void
ftl_band_set_state(struct ftl_band *band, enum ftl_band_state state)
{
	switch (state) {
	case FTL_BAND_STATE_FREE:
		assert(band->state == FTL_BAND_STATE_CLOSED);
		_ftl_band_set_free(band);
		break;

	case FTL_BAND_STATE_PREP:
		assert(band->state == FTL_BAND_STATE_FREE);
		_ftl_band_set_preparing(band);
		break;

	case FTL_BAND_STATE_CLOSED:
		if (band->state != FTL_BAND_STATE_CLOSED) {
			assert(band->state == FTL_BAND_STATE_CLOSING || band->high_prio);
			_ftl_band_set_closed(band);
		}
		break;

	default:
		break;
	}

	band->state = state;
}

void
ftl_band_set_addr(struct ftl_band *band, uint64_t lba, struct ftl_addr addr)
{
	struct ftl_lba_map *lba_map = &band->lba_map;
	uint64_t offset;

	assert(lba != FTL_LBA_INVALID);

	offset = ftl_band_block_offset_from_addr(band, addr);
	pthread_spin_lock(&lba_map->lock);

	lba_map->num_vld++;
	lba_map->map[offset] = lba;
	spdk_bit_array_set(lba_map->vld, offset);

	pthread_spin_unlock(&lba_map->lock);
}

size_t
ftl_band_age(const struct ftl_band *band)
{
	return (size_t)(band->dev->seq - band->seq);
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

	if (spdk_unlikely(offset <= ftl_head_md_num_blocks(band->dev))) {
		return ftl_band_user_blocks(band);
	}

	if (spdk_unlikely(offset > tail_md_offset)) {
		return 0;
	}

	return tail_md_offset - offset;
}

size_t
ftl_band_user_blocks(const struct ftl_band *band)
{
	return ftl_band_num_usable_blocks(band) -
	       ftl_head_md_num_blocks(band->dev) -
	       ftl_tail_md_num_blocks(band->dev);
}

struct ftl_band *
ftl_band_from_addr(struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	size_t band_id = ftl_addr_get_band(dev, addr);

	assert(band_id < ftl_get_num_bands(dev));
	return &dev->bands[band_id];
}

struct ftl_zone *
ftl_band_zone_from_addr(struct ftl_band *band, struct ftl_addr addr)
{
	size_t pu_id = ftl_addr_get_punit(band->dev, addr);

	assert(pu_id < ftl_get_num_punits(band->dev));
	return &band->zone_buf[pu_id];
}

uint64_t
ftl_band_block_offset_from_addr(struct ftl_band *band, struct ftl_addr addr)
{
	assert(ftl_addr_get_band(band->dev, addr) == band->id);
	assert(ftl_addr_get_punit(band->dev, addr) < ftl_get_num_punits(band->dev));
	return addr.offset % ftl_get_num_blocks_in_band(band->dev);
}

struct ftl_addr
ftl_band_next_xfer_addr(struct ftl_band *band, struct ftl_addr addr, size_t num_blocks)
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
		return ftl_to_addr(FTL_ADDR_INVALID);
	}

	num_xfers = num_blocks / dev->xfer_size;
	for (size_t i = 0; i < num_xfers; ++i) {
		/* When the last zone is reached the block part of the address */
		/* needs to be increased by xfer_size */
		if (ftl_band_zone_is_last(band, zone)) {
			offset += dev->xfer_size;
			if (offset > ftl_get_num_blocks_in_zone(dev)) {
				return ftl_to_addr(FTL_ADDR_INVALID);
			}
		}

		zone = ftl_band_next_operational_zone(band, zone);
		assert(zone);

		num_blocks -= dev->xfer_size;
	}

	if (num_blocks) {
		offset += num_blocks;
		if (offset > ftl_get_num_blocks_in_zone(dev)) {
			return ftl_to_addr(FTL_ADDR_INVALID);
		}
	}

	addr.offset = zone->info.zone_id + offset;
	return addr;
}

static size_t
ftl_xfer_offset_from_addr(struct ftl_band *band, struct ftl_addr addr)
{
	struct ftl_zone *zone, *current_zone;
	unsigned int punit_offset = 0;
	size_t num_stripes, xfer_size = band->dev->xfer_size;
	uint64_t offset;

	assert(ftl_addr_get_band(band->dev, addr) == band->id);

	offset = ftl_addr_get_zone_offset(band->dev, addr);
	num_stripes = (offset / xfer_size) * band->num_zones;

	current_zone = ftl_band_zone_from_addr(band, addr);
	CIRCLEQ_FOREACH(zone, &band->zones, circleq) {
		if (current_zone == zone) {
			break;
		}
		punit_offset++;
	}

	return xfer_size * (num_stripes + punit_offset) + offset % xfer_size;
}

struct ftl_addr
ftl_band_addr_from_block_offset(struct ftl_band *band, uint64_t block_off)
{
	struct ftl_addr addr = { .offset = 0 };

	addr.offset = block_off + band->id * ftl_get_num_blocks_in_band(band->dev);
	return addr;
}

struct ftl_addr
ftl_band_next_addr(struct ftl_band *band, struct ftl_addr addr, size_t offset)
{
	uint64_t block_off = ftl_band_block_offset_from_addr(band, addr);
	return ftl_band_addr_from_block_offset(band, block_off + offset);
}

void
ftl_band_acquire_lba_map(struct ftl_band *band)
{
	assert(band->lba_map.map != NULL);
	band->lba_map.ref_cnt++;
}

int
ftl_band_alloc_lba_map(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;

	assert(lba_map->ref_cnt == 0);
	assert(lba_map->map == NULL);

	lba_map->dma_buf = spdk_mempool_get(dev->lba_pool);

	if (!lba_map->dma_buf) {
		return -1;
	}

	memset(lba_map->dma_buf, 0, ftl_lba_map_pool_elem_size(band->dev));

	lba_map->map = (uint64_t *)((char *)lba_map->dma_buf + FTL_BLOCK_SIZE *
				    (ftl_tail_md_hdr_num_blocks() + ftl_vld_map_num_blocks(dev)));

	lba_map->segments = (char *)lba_map->dma_buf + ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE;

	ftl_band_acquire_lba_map(band);
	return 0;
}

void
ftl_band_release_lba_map(struct ftl_band *band)
{
	struct ftl_lba_map *lba_map = &band->lba_map;

	assert(lba_map->map != NULL);
	assert(lba_map->ref_cnt > 0);
	lba_map->ref_cnt--;

	if (lba_map->ref_cnt == 0) {
		ftl_band_free_lba_map(band);
	}
}

static void
ftl_read_md_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_md_io *md_io = (struct ftl_md_io *)io;

	if (!status) {
		status = md_io->pack_fn(md_io->io.band);
	} else {
		status = FTL_MD_IO_FAILURE;
	}

	md_io->cb_fn(io, md_io->cb_ctx, status);
}

static struct ftl_md_io *
ftl_io_init_md_read(struct spdk_ftl_dev *dev, struct ftl_addr addr,
		    struct ftl_band *band, size_t num_blocks, void *buf,
		    ftl_io_fn fn, ftl_md_pack_fn pack_fn, ftl_io_fn cb_fn, void *cb_ctx)
{
	struct ftl_md_io *io;
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.band		= band,
		.size		= sizeof(*io),
		.flags		= FTL_IO_MD | FTL_IO_PHYSICAL_MODE,
		.type		= FTL_IO_READ,
		.num_blocks	= num_blocks,
		.cb_fn		= fn,
		.iovs		= {
			{
				.iov_base = buf,
				.iov_len = num_blocks * FTL_BLOCK_SIZE,
			}
		},
		.iovcnt		= 1,
	};

	io = (struct ftl_md_io *)ftl_io_init_internal(&opts);
	if (!io) {
		return NULL;
	}

	io->io.addr = addr;
	io->pack_fn = pack_fn;
	io->cb_fn = cb_fn;
	io->cb_ctx = cb_ctx;

	return io;
}

static struct ftl_io *
ftl_io_init_md_write(struct spdk_ftl_dev *dev, struct ftl_band *band,
		     void *data, size_t num_blocks, ftl_io_fn cb)
{
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= FTL_IO_MD | FTL_IO_PHYSICAL_MODE,
		.type		= FTL_IO_WRITE,
		.num_blocks	= num_blocks,
		.cb_fn		= cb,
		.iovs		= {
			{
				.iov_base = data,
				.iov_len = num_blocks * FTL_BLOCK_SIZE,
			}
		},
		.iovcnt		= 1,
		.md		= NULL,
	};

	return ftl_io_init_internal(&opts);
}

static int
ftl_band_write_md(struct ftl_band *band, size_t num_blocks,
		  ftl_md_pack_fn md_fn, ftl_io_fn cb)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_io *io;

	io = ftl_io_init_md_write(dev, band, band->lba_map.dma_buf, num_blocks, cb);
	if (!io) {
		return -ENOMEM;
	}

	md_fn(band);

	ftl_io_write(io);
	return 0;
}

void
ftl_band_md_clear(struct ftl_band *band)
{
	band->seq = 0;
	band->wr_cnt = 0;
	band->lba_map.num_vld = 0;
	band->lba_map.map = NULL;
}

int
ftl_band_write_head_md(struct ftl_band *band, ftl_io_fn cb)
{
	return ftl_band_write_md(band, ftl_head_md_num_blocks(band->dev),
				 ftl_pack_head_md, cb);
}

int
ftl_band_write_tail_md(struct ftl_band *band, ftl_io_fn cb)
{
	return ftl_band_write_md(band, ftl_tail_md_num_blocks(band->dev),
				 ftl_pack_tail_md, cb);
}

static struct ftl_addr
ftl_band_lba_map_addr(struct ftl_band *band, size_t offset)
{
	return ftl_band_next_xfer_addr(band, band->tail_md_addr,
				       ftl_tail_md_hdr_num_blocks() +
				       ftl_vld_map_num_blocks(band->dev) +
				       offset);
}

static int
ftl_band_read_md(struct ftl_band *band, size_t num_blocks, struct ftl_addr start_addr,
		 void *buf, ftl_io_fn fn, ftl_md_pack_fn pack_fn, ftl_io_fn cb_fn, void *cb_ctx)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_md_io *io;

	if (spdk_unlikely(!band->num_zones)) {
		return -ENOENT;
	}

	io = ftl_io_init_md_read(dev, start_addr, band, num_blocks, buf, fn, pack_fn, cb_fn, cb_ctx);
	if (!io) {
		return -ENOMEM;
	}

	ftl_io_read((struct ftl_io *)io);
	return 0;
}

int
ftl_band_read_tail_md(struct ftl_band *band, struct ftl_addr addr, ftl_io_fn cb_fn, void *cb_ctx)
{
	return ftl_band_read_md(band, ftl_tail_md_num_blocks(band->dev), addr, band->lba_map.dma_buf,
				ftl_read_md_cb, ftl_unpack_tail_md, cb_fn, cb_ctx);
}

static size_t
ftl_lba_map_request_segment_done(struct ftl_lba_map_request *request, size_t offset,
				 size_t num_segments)
{
	size_t i, num_done = 0;

	for (i = offset; i < offset + num_segments; ++i) {
		if (spdk_bit_array_get(request->segments, i)) {
			spdk_bit_array_clear(request->segments, offset);
			num_done++;
		}
	}

	assert(request->num_pending >= num_done);
	request->num_pending -= num_done;

	return num_done;
}

static void
ftl_lba_map_set_segment_state(struct ftl_lba_map *lba_map, size_t offset, size_t num_segments,
			      enum ftl_lba_map_seg_state state)
{
	size_t i;

	for (i = offset; i < offset + num_segments; ++i) {
		lba_map->segments[i] = state;
	}
}

static void
ftl_lba_map_request_free(struct spdk_ftl_dev *dev, struct ftl_lba_map_request *request)
{
	spdk_bit_array_clear_mask(request->segments);
	spdk_mempool_put(dev->lba_request_pool, request);
}

static void
ftl_process_lba_map_requests(struct spdk_ftl_dev *dev, struct ftl_lba_map *lba_map, size_t offset,
			     size_t num_segments, int status)
{
	struct ftl_lba_map_request *request, *trequest;
	size_t num_done;

	LIST_FOREACH_SAFE(request, &lba_map->request_list, list_entry, trequest) {
		num_done = ftl_lba_map_request_segment_done(request, offset, num_segments);
		if (request->num_pending == 0 || (status && num_done)) {
			request->cb(NULL, request->cb_ctx, status);
			LIST_REMOVE(request, list_entry);
			ftl_lba_map_request_free(dev, request);
		}
	}
}

static size_t
ftl_lba_map_offset_from_addr(struct ftl_band *band, struct ftl_addr addr)
{
	size_t offset;
	struct ftl_addr start_addr = ftl_band_lba_map_addr(band, 0);

	offset =  ftl_xfer_offset_from_addr(band, addr) - ftl_xfer_offset_from_addr(band, start_addr);
	assert(offset < ftl_lba_map_num_blocks(band->dev));

	return offset;
}

static void
ftl_read_lba_map_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_lba_map *lba_map = &io->band->lba_map;
	uint64_t block_off;

	block_off = ftl_lba_map_offset_from_addr(io->band, io->addr);
	assert(block_off + io->num_blocks <= ftl_lba_map_num_blocks(io->dev));

	if (!status) {
		ftl_lba_map_set_segment_state(lba_map, block_off, io->num_blocks,
					      FTL_LBA_MAP_SEG_CACHED);
	}

	ftl_process_lba_map_requests(io->dev, lba_map, block_off, io->num_blocks, status);
}

static struct ftl_lba_map_request *
ftl_lba_map_alloc_request(struct ftl_band *band, size_t offset, size_t num_segments,
			  ftl_io_fn cb, void *cb_ctx)
{
	struct ftl_lba_map_request *request;
	struct spdk_ftl_dev *dev = band->dev;
	size_t i;

	request = spdk_mempool_get(dev->lba_request_pool);
	if (!request) {
		return NULL;
	}

	request->cb = cb;
	request->cb_ctx = cb_ctx;
	request->num_pending = num_segments;

	for (i = offset; i < offset + num_segments; ++i) {
		spdk_bit_array_set(request->segments, i);
	}

	return request;
}

static size_t
ftl_lba_map_num_clear_segments(struct ftl_lba_map *lba_map,
			       size_t offset, size_t num_segments)
{
	size_t i, cnt = 0;

	for (i = offset; i < offset + num_segments; ++i) {
		if (lba_map->segments[i] != FTL_LBA_MAP_SEG_CLEAR) {
			break;
		}
		cnt++;
	}

	return cnt;
}

int
ftl_band_read_lba_map(struct ftl_band *band, size_t offset, size_t lba_cnt,
		      ftl_io_fn cb_fn, void *cb_ctx)
{
	size_t num_blocks, block_off, num_read, num_segments;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_lba_map_request *request;
	int rc = 0;

	block_off = offset / FTL_NUM_LBA_IN_BLOCK;
	num_segments = spdk_divide_round_up(offset + lba_cnt, FTL_NUM_LBA_IN_BLOCK);
	num_blocks = num_segments - block_off;
	assert(block_off + num_blocks <= ftl_lba_map_num_blocks(band->dev));

	request = ftl_lba_map_alloc_request(band, block_off, num_blocks, cb_fn, cb_ctx);
	if (!request) {
		return -ENOMEM;
	}

	while (num_blocks) {
		if (lba_map->segments[block_off] != FTL_LBA_MAP_SEG_CLEAR) {
			if (lba_map->segments[block_off] == FTL_LBA_MAP_SEG_CACHED) {
				ftl_lba_map_request_segment_done(request, block_off, 1);
			}
			num_blocks--;
			block_off++;
			continue;
		}

		num_read = ftl_lba_map_num_clear_segments(lba_map, block_off, num_blocks);
		ftl_lba_map_set_segment_state(lba_map, block_off, num_read,
					      FTL_LBA_MAP_SEG_PENDING);

		rc = ftl_band_read_md(band, num_read, ftl_band_lba_map_addr(band, block_off),
				      (char *)band->lba_map.map + block_off * FTL_BLOCK_SIZE,
				      ftl_read_lba_map_cb, NULL, cb_fn, cb_ctx);
		if (rc) {
			ftl_lba_map_request_free(band->dev, request);
			return rc;
		}

		assert(num_blocks >= num_read);
		num_blocks -= num_read;
		block_off += num_read;
	}

	if (request->num_pending) {
		LIST_INSERT_HEAD(&lba_map->request_list, request, list_entry);
	} else {
		cb_fn(NULL, cb_ctx, 0);
		ftl_lba_map_request_free(band->dev, request);
	}

	return rc;
}

int
ftl_band_read_head_md(struct ftl_band *band, ftl_io_fn cb_fn, void *cb_ctx)
{
	return ftl_band_read_md(band,
				ftl_head_md_num_blocks(band->dev),
				ftl_band_head_md_addr(band),
				band->lba_map.dma_buf,
				ftl_read_md_cb,
				ftl_unpack_head_md,
				cb_fn,
				cb_ctx);
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
	struct spdk_ftl_dev *dev = band->dev;

	if (ftl_band_alloc_lba_map(band)) {
		return -1;
	}

	band->seq = ++dev->seq;
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
	size_t num_segments;

	spdk_bit_array_clear_mask(lba_map->vld);
	memset(lba_map->map, 0, ftl_lba_map_num_blocks(band->dev) * FTL_BLOCK_SIZE);

	/* For open band all lba map segments are already cached */
	assert(band->state == FTL_BAND_STATE_PREP);
	num_segments = spdk_divide_round_up(ftl_get_num_blocks_in_band(band->dev), FTL_NUM_LBA_IN_BLOCK);
	ftl_lba_map_set_segment_state(&band->lba_map, 0, num_segments, FTL_LBA_MAP_SEG_CACHED);

	lba_map->num_vld = 0;
}

size_t
ftl_lba_map_pool_elem_size(struct spdk_ftl_dev *dev)
{
	/* Map pool element holds the whole tail md + segments map */
	return ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE +
	       spdk_divide_round_up(ftl_get_num_blocks_in_band(dev), FTL_NUM_LBA_IN_BLOCK);
}
