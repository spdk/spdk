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

	/* Max number of lbks */
	uint64_t		num_lbks;

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
ftl_tail_md_hdr_num_lbks(void)
{
	return spdk_divide_round_up(sizeof(struct ftl_tail_md), FTL_BLOCK_SIZE);
}

size_t
ftl_vld_map_num_lbks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_vld_map_size(dev), FTL_BLOCK_SIZE);
}

size_t
ftl_lba_map_num_lbks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_num_band_lbks(dev) * sizeof(uint64_t), FTL_BLOCK_SIZE);
}

size_t
ftl_head_md_num_lbks(const struct spdk_ftl_dev *dev)
{
	return dev->xfer_size;
}

size_t
ftl_tail_md_num_lbks(const struct spdk_ftl_dev *dev)
{
	return spdk_divide_round_up(ftl_tail_md_hdr_num_lbks() +
				    ftl_vld_map_num_lbks(dev) +
				    ftl_lba_map_num_lbks(dev),
				    dev->xfer_size) * dev->xfer_size;
}

static uint64_t
ftl_band_tail_md_offset(const struct ftl_band *band)
{
	return ftl_band_num_usable_lbks(band) -
	       ftl_tail_md_num_lbks(band->dev);
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

	if (!dev->df_band) {
		dev->df_band = band;
	}

	ftl_reloc_add(dev->reloc, band, 0, ftl_num_band_lbks(dev), 1);
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
	if (band->num_chunks) {
		assert(ftl_band_validate_md(band) == true);
	}

	memset(lba_map->map, 0, ftl_lba_map_pool_elem_size(band->dev));
	spdk_mempool_put(dev->lba_pool, lba_map->map);
	spdk_dma_free(lba_map->dma_buf);
	lba_map->map = NULL;
	lba_map->dma_buf = NULL;
}

static void
_ftl_band_set_free(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_band *lband, *prev;

	if (band == dev->df_band) {
		dev->df_band = NULL;
	}

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
	struct ftl_chunk *chunk;

	/* Set the state as free_md() checks for that */
	band->state = FTL_BAND_STATE_CLOSED;

	/* Free the lba map if there are no outstanding IOs */
	ftl_band_release_lba_map(band);

	if (spdk_likely(band->num_chunks)) {
		LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
		CIRCLEQ_FOREACH(chunk, &band->chunks, circleq) {
			chunk->state = FTL_CHUNK_STATE_CLOSED;
		}
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
	size_t map_size;
	void *vld_offset, *map_offset;

	map_size = ftl_num_band_lbks(dev) * sizeof(uint64_t);
	vld_offset = (char *)tail + ftl_tail_md_hdr_num_lbks() * FTL_BLOCK_SIZE;
	map_offset = (char *)vld_offset + ftl_vld_map_num_lbks(dev) * FTL_BLOCK_SIZE;

	/* Clear out the buffer */
	memset(tail, 0, ftl_tail_md_num_lbks(dev) * FTL_BLOCK_SIZE);
	tail->num_lbks = ftl_num_band_lbks(dev);

	pthread_spin_lock(&lba_map->lock);
	spdk_bit_array_store_mask(lba_map->vld, vld_offset);
	pthread_spin_unlock(&lba_map->lock);

	memcpy(map_offset, lba_map->map, map_size);
	ftl_set_md_hdr(band, &tail->hdr, ftl_tail_md_num_lbks(dev) * FTL_BLOCK_SIZE);

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
	size_t map_size;
	void *vld_offset, *map_offset;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_tail_md *tail = lba_map->dma_buf;
	int rc;

	map_size = ftl_num_band_lbks(dev) * sizeof(uint64_t);
	vld_offset = (char *)tail + ftl_tail_md_hdr_num_lbks() * FTL_BLOCK_SIZE;
	map_offset = (char *)vld_offset + ftl_vld_map_num_lbks(dev) * FTL_BLOCK_SIZE;

	rc = ftl_md_hdr_vld(dev, &tail->hdr, ftl_tail_md_num_lbks(dev) * FTL_BLOCK_SIZE);
	if (rc) {
		return rc;
	}

	if (tail->num_lbks != ftl_num_band_lbks(dev)) {
		return FTL_MD_INVALID_SIZE;
	}

	if (lba_map->vld) {
		spdk_bit_array_load_mask(lba_map->vld, vld_offset);
	}

	if (lba_map->map) {
		memcpy(lba_map->map, map_offset, map_size);
	}

	band->seq = tail->hdr.seq;
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

struct ftl_ppa
ftl_band_tail_md_ppa(struct ftl_band *band)
{
	struct ftl_ppa ppa = {};
	struct ftl_chunk *chunk;
	struct spdk_ftl_dev *dev = band->dev;
	size_t xfer_size = dev->xfer_size;
	size_t num_req = ftl_band_tail_md_offset(band) / xfer_size;
	size_t i;

	if (spdk_unlikely(!band->num_chunks)) {
		return ftl_to_ppa(FTL_PPA_INVALID);
	}

	/* Metadata should be aligned to xfer size */
	assert(ftl_band_tail_md_offset(band) % xfer_size == 0);

	chunk = CIRCLEQ_FIRST(&band->chunks);
	for (i = 0; i < num_req % band->num_chunks; ++i) {
		chunk = ftl_band_next_chunk(band, chunk);
	}

	ppa.lbk = (num_req / band->num_chunks) * xfer_size;
	ppa.chk = band->id;
	ppa.pu = chunk->punit->start_ppa.pu;
	ppa.grp = chunk->punit->start_ppa.grp;

	return ppa;
}

struct ftl_ppa
ftl_band_head_md_ppa(struct ftl_band *band)
{
	struct ftl_ppa ppa;

	if (spdk_unlikely(!band->num_chunks)) {
		return ftl_to_ppa(FTL_PPA_INVALID);
	}

	ppa = CIRCLEQ_FIRST(&band->chunks)->punit->start_ppa;
	ppa.chk = band->id;

	return ppa;
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
			assert(band->state == FTL_BAND_STATE_CLOSING);
			_ftl_band_set_closed(band);
		}
		break;

	default:
		break;
	}

	band->state = state;
}

void
ftl_band_set_addr(struct ftl_band *band, uint64_t lba, struct ftl_ppa ppa)
{
	struct ftl_lba_map *lba_map = &band->lba_map;
	uint64_t offset;

	assert(lba != FTL_LBA_INVALID);

	offset = ftl_band_lbkoff_from_ppa(band, ppa);
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
ftl_band_num_usable_lbks(const struct ftl_band *band)
{
	return band->num_chunks * ftl_dev_lbks_in_chunk(band->dev);
}

size_t
ftl_band_user_lbks_left(const struct ftl_band *band, size_t offset)
{
	size_t tail_md_offset = ftl_band_tail_md_offset(band);

	if (spdk_unlikely(offset <= ftl_head_md_num_lbks(band->dev))) {
		return ftl_band_user_lbks(band);
	}

	if (spdk_unlikely(offset > tail_md_offset)) {
		return 0;
	}

	return tail_md_offset - offset;
}

size_t
ftl_band_user_lbks(const struct ftl_band *band)
{
	return ftl_band_num_usable_lbks(band) -
	       ftl_head_md_num_lbks(band->dev) -
	       ftl_tail_md_num_lbks(band->dev);
}

struct ftl_band *
ftl_band_from_ppa(struct spdk_ftl_dev *dev, struct ftl_ppa ppa)
{
	assert(ppa.chk < ftl_dev_num_bands(dev));
	return &dev->bands[ppa.chk];
}

struct ftl_chunk *
ftl_band_chunk_from_ppa(struct ftl_band *band, struct ftl_ppa ppa)
{
	struct spdk_ftl_dev *dev = band->dev;
	unsigned int punit;

	punit = ftl_ppa_flatten_punit(dev, ppa);
	assert(punit < ftl_dev_num_punits(dev));

	return &band->chunk_buf[punit];
}

uint64_t
ftl_band_lbkoff_from_ppa(struct ftl_band *band, struct ftl_ppa ppa)
{
	struct spdk_ftl_dev *dev = band->dev;
	unsigned int punit;

	punit = ftl_ppa_flatten_punit(dev, ppa);
	assert(ppa.chk == band->id);

	return punit * ftl_dev_lbks_in_chunk(dev) + ppa.lbk;
}

struct ftl_ppa
ftl_band_next_xfer_ppa(struct ftl_band *band, struct ftl_ppa ppa, size_t num_lbks)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_chunk *chunk;
	unsigned int punit_num;
	size_t num_xfers, num_stripes;

	assert(ppa.chk == band->id);

	punit_num = ftl_ppa_flatten_punit(dev, ppa);
	chunk = &band->chunk_buf[punit_num];

	num_lbks += (ppa.lbk % dev->xfer_size);
	ppa.lbk  -= (ppa.lbk % dev->xfer_size);

#if defined(DEBUG)
	/* Check that the number of chunks has not been changed */
	struct ftl_chunk *_chunk;
	size_t _num_chunks = 0;
	CIRCLEQ_FOREACH(_chunk, &band->chunks, circleq) {
		if (spdk_likely(_chunk->state != FTL_CHUNK_STATE_BAD)) {
			_num_chunks++;
		}
	}
	assert(band->num_chunks == _num_chunks);
#endif
	assert(band->num_chunks != 0);
	num_stripes = (num_lbks / dev->xfer_size) / band->num_chunks;
	ppa.lbk  += num_stripes * dev->xfer_size;
	num_lbks -= num_stripes * dev->xfer_size * band->num_chunks;

	if (ppa.lbk > ftl_dev_lbks_in_chunk(dev)) {
		return ftl_to_ppa(FTL_PPA_INVALID);
	}

	num_xfers = num_lbks / dev->xfer_size;
	for (size_t i = 0; i < num_xfers; ++i) {
		/* When the last chunk is reached the lbk part of the address */
		/* needs to be increased by xfer_size */
		if (ftl_band_chunk_is_last(band, chunk)) {
			ppa.lbk += dev->xfer_size;
			if (ppa.lbk > ftl_dev_lbks_in_chunk(dev)) {
				return ftl_to_ppa(FTL_PPA_INVALID);
			}
		}

		chunk = ftl_band_next_operational_chunk(band, chunk);
		ppa.grp = chunk->start_ppa.grp;
		ppa.pu = chunk->start_ppa.pu;

		num_lbks -= dev->xfer_size;
	}

	if (num_lbks) {
		ppa.lbk += num_lbks;
		if (ppa.lbk > ftl_dev_lbks_in_chunk(dev)) {
			return ftl_to_ppa(FTL_PPA_INVALID);
		}
	}

	return ppa;
}

static size_t
ftl_xfer_offset_from_ppa(struct ftl_band *band, struct ftl_ppa ppa)
{
	struct ftl_chunk *chunk, *current_chunk;
	unsigned int punit_offset = 0;
	size_t off, num_stripes, xfer_size = band->dev->xfer_size;

	assert(ppa.chk == band->id);

	num_stripes = (ppa.lbk / xfer_size) * band->num_chunks;
	off = ppa.lbk % xfer_size;

	current_chunk = ftl_band_chunk_from_ppa(band, ppa);
	CIRCLEQ_FOREACH(chunk, &band->chunks, circleq) {
		if (current_chunk == chunk) {
			break;
		}
		punit_offset++;
	}

	return xfer_size * (num_stripes + punit_offset) + off;
}

struct ftl_ppa
ftl_band_ppa_from_lbkoff(struct ftl_band *band, uint64_t lbkoff)
{
	struct ftl_ppa ppa = { .ppa = 0 };
	struct spdk_ftl_dev *dev = band->dev;
	uint64_t punit;

	punit = lbkoff / ftl_dev_lbks_in_chunk(dev) + dev->range.begin;

	ppa.lbk = lbkoff % ftl_dev_lbks_in_chunk(dev);
	ppa.chk = band->id;
	ppa.pu = punit / dev->geo.num_grp;
	ppa.grp = punit % dev->geo.num_grp;

	return ppa;
}

struct ftl_ppa
ftl_band_next_ppa(struct ftl_band *band, struct ftl_ppa ppa, size_t offset)
{
	uint64_t lbkoff = ftl_band_lbkoff_from_ppa(band, ppa);
	return ftl_band_ppa_from_lbkoff(band, lbkoff + offset);
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

	lba_map->map = spdk_mempool_get(dev->lba_pool);
	if (!lba_map->map) {
		return -1;
	}

	lba_map->segments = (char *)lba_map->map + ftl_lba_map_num_lbks(dev) * FTL_BLOCK_SIZE;

	lba_map->dma_buf = spdk_dma_zmalloc(ftl_tail_md_num_lbks(dev) * FTL_BLOCK_SIZE,
					    FTL_BLOCK_SIZE, NULL);
	if (!lba_map->dma_buf) {
		spdk_mempool_put(dev->lba_pool, lba_map->map);
		return -1;
	}

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
ftl_io_init_md_read(struct spdk_ftl_dev *dev, struct ftl_ppa ppa,
		    struct ftl_band *band, size_t lbk_cnt, void *buf,
		    ftl_io_fn fn, ftl_md_pack_fn pack_fn, ftl_io_fn cb_fn, void *cb_ctx)
{
	struct ftl_md_io *io;
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= NULL,
		.band		= band,
		.size		= sizeof(*io),
		.flags		= FTL_IO_MD | FTL_IO_PPA_MODE,
		.type		= FTL_IO_READ,
		.lbk_cnt	= lbk_cnt,
		.cb_fn		= fn,
		.data		= buf,
	};

	io = (struct ftl_md_io *)ftl_io_init_internal(&opts);
	if (!io) {
		return NULL;
	}

	io->io.ppa = ppa;
	io->pack_fn = pack_fn;
	io->cb_fn = cb_fn;
	io->cb_ctx = cb_ctx;

	return io;
}

static struct ftl_io *
ftl_io_init_md_write(struct spdk_ftl_dev *dev, struct ftl_band *band,
		     void *data, size_t lbk_cnt, ftl_io_fn cb)
{
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= NULL,
		.band		= band,
		.size		= sizeof(struct ftl_io),
		.flags		= FTL_IO_MD | FTL_IO_PPA_MODE,
		.type		= FTL_IO_WRITE,
		.lbk_cnt	= lbk_cnt,
		.cb_fn		= cb,
		.data		= data,
		.md		= NULL,
	};

	return ftl_io_init_internal(&opts);
}

static int
ftl_band_write_md(struct ftl_band *band, size_t lbk_cnt,
		  ftl_md_pack_fn md_fn, ftl_io_fn cb)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_io *io;

	io = ftl_io_init_md_write(dev, band, band->lba_map.dma_buf, lbk_cnt, cb);
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
	return ftl_band_write_md(band, ftl_head_md_num_lbks(band->dev),
				 ftl_pack_head_md, cb);
}

int
ftl_band_write_tail_md(struct ftl_band *band, ftl_io_fn cb)
{
	return ftl_band_write_md(band, ftl_tail_md_num_lbks(band->dev),
				 ftl_pack_tail_md, cb);
}

static struct ftl_ppa
ftl_band_lba_map_ppa(struct ftl_band *band, size_t offset)
{
	return ftl_band_next_xfer_ppa(band, band->tail_md_ppa,
				      ftl_tail_md_hdr_num_lbks() +
				      ftl_vld_map_num_lbks(band->dev) +
				      offset);
}

static int
ftl_band_read_md(struct ftl_band *band, size_t lbk_cnt, struct ftl_ppa start_ppa,
		 void *buf, ftl_io_fn fn, ftl_md_pack_fn pack_fn, ftl_io_fn cb_fn, void *cb_ctx)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_md_io *io;

	if (spdk_unlikely(!band->num_chunks)) {
		return -ENOENT;
	}

	io = ftl_io_init_md_read(dev, start_ppa, band, lbk_cnt, buf, fn, pack_fn, cb_fn, cb_ctx);
	if (!io) {
		return -ENOMEM;
	}

	ftl_io_read((struct ftl_io *)io);
	return 0;
}

int
ftl_band_read_tail_md(struct ftl_band *band, struct ftl_ppa ppa, ftl_io_fn cb_fn, void *cb_ctx)
{
	return ftl_band_read_md(band, ftl_tail_md_num_lbks(band->dev), ppa, band->lba_map.dma_buf,
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
ftl_lba_map_offset_from_ppa(struct ftl_band *band, struct ftl_ppa ppa)
{
	size_t offset;
	struct ftl_ppa start_ppa = ftl_band_lba_map_ppa(band, 0);

	offset =  ftl_xfer_offset_from_ppa(band, ppa) - ftl_xfer_offset_from_ppa(band, start_ppa);
	assert(offset < ftl_lba_map_num_lbks(band->dev));

	return offset;
}

static void
ftl_read_lba_map_cb(struct ftl_io *io, void *arg, int status)
{
	struct ftl_lba_map *lba_map = &io->band->lba_map;
	uint64_t lbk_off;

	lbk_off = ftl_lba_map_offset_from_ppa(io->band, io->ppa);
	assert(lbk_off + io->lbk_cnt <= ftl_lba_map_num_lbks(io->dev));

	if (!status) {
		memcpy((char *)lba_map->map + lbk_off * FTL_BLOCK_SIZE,
		       io->iov[0].iov_base,
		       io->lbk_cnt * FTL_BLOCK_SIZE);

		ftl_lba_map_set_segment_state(lba_map, lbk_off, io->lbk_cnt,
					      FTL_LBA_MAP_SEG_CACHED);
	}

	ftl_process_lba_map_requests(io->dev, lba_map, lbk_off, io->lbk_cnt, status);
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
	size_t lbk_cnt, lbk_off, num_read, num_segments;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_lba_map_request *request;
	int rc = 0;

	lbk_off = offset / FTL_NUM_LBA_IN_BLOCK;
	num_segments = spdk_divide_round_up(offset + lba_cnt, FTL_NUM_LBA_IN_BLOCK);
	lbk_cnt = num_segments - lbk_off;
	assert(lbk_off + lbk_cnt <= ftl_lba_map_num_lbks(band->dev));

	request = ftl_lba_map_alloc_request(band, lbk_off, lbk_cnt, cb_fn, cb_ctx);
	if (!request) {
		return -ENOMEM;
	}

	while (lbk_cnt) {
		if (lba_map->segments[lbk_off] != FTL_LBA_MAP_SEG_CLEAR) {
			if (lba_map->segments[lbk_off] == FTL_LBA_MAP_SEG_CACHED) {
				ftl_lba_map_request_segment_done(request, lbk_off, 1);
			}
			lbk_cnt--;
			lbk_off++;
			continue;
		}

		num_read = ftl_lba_map_num_clear_segments(lba_map, lbk_off, lbk_cnt);
		ftl_lba_map_set_segment_state(lba_map, lbk_off, num_read,
					      FTL_LBA_MAP_SEG_PENDING);

		rc = ftl_band_read_md(band, num_read, ftl_band_lba_map_ppa(band, lbk_off),
				      (char *)band->lba_map.dma_buf + lbk_off * FTL_BLOCK_SIZE,
				      ftl_read_lba_map_cb, NULL, cb_fn, cb_ctx);
		if (rc) {
			ftl_lba_map_request_free(band->dev, request);
			return rc;
		}

		assert(lbk_cnt >= num_read);
		lbk_cnt -= num_read;
		lbk_off += num_read;
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
				ftl_head_md_num_lbks(band->dev),
				ftl_band_head_md_ppa(band),
				band->lba_map.dma_buf,
				ftl_read_md_cb,
				ftl_unpack_head_md,
				cb_fn,
				cb_ctx);
}

static void
ftl_band_remove_chunk(struct ftl_band *band, struct ftl_chunk *chunk)
{
	CIRCLEQ_REMOVE(&band->chunks, chunk, circleq);
	band->num_chunks--;
}

static void
ftl_erase_fail(struct ftl_io *io, int status)
{
	struct ftl_chunk *chunk;
	struct ftl_band *band = io->band;
	char buf[128];

	SPDK_ERRLOG("Erase failed @ppa: %s, status: %d\n",
		    ftl_ppa2str(io->ppa, buf, sizeof(buf)), status);

	chunk = ftl_band_chunk_from_ppa(band, io->ppa);
	chunk->state = FTL_CHUNK_STATE_BAD;
	ftl_band_remove_chunk(band, chunk);
	band->tail_md_ppa = ftl_band_tail_md_ppa(band);
}

static void
ftl_band_erase_cb(struct ftl_io *io, void *ctx, int status)
{
	struct ftl_chunk *chunk;

	if (spdk_unlikely(status)) {
		ftl_erase_fail(io, status);
		return;
	}
	chunk = ftl_band_chunk_from_ppa(io->band, io->ppa);
	chunk->state = FTL_CHUNK_STATE_FREE;
}

int
ftl_band_erase(struct ftl_band *band)
{
	struct ftl_chunk *chunk;
	struct ftl_io *io;
	int rc = 0;

	assert(band->state == FTL_BAND_STATE_CLOSED ||
	       band->state == FTL_BAND_STATE_FREE);

	ftl_band_set_state(band, FTL_BAND_STATE_PREP);

	CIRCLEQ_FOREACH(chunk, &band->chunks, circleq) {
		if (chunk->state == FTL_CHUNK_STATE_FREE) {
			continue;
		}

		io = ftl_io_erase_init(band, 1, ftl_band_erase_cb);
		if (!io) {
			rc = -ENOMEM;
			break;
		}

		io->ppa = chunk->start_ppa;
		rc = ftl_io_erase(io);
		if (rc) {
			assert(0);
			/* TODO: change band's state back to close? */
			break;
		}
	}

	return rc;
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

struct ftl_chunk *
ftl_band_next_operational_chunk(struct ftl_band *band, struct ftl_chunk *chunk)
{
	struct ftl_chunk *result = NULL;
	struct ftl_chunk *entry;

	if (spdk_unlikely(!band->num_chunks)) {
		return NULL;
	}

	/* Erasing band may fail after it was assigned to wptr. */
	/* In such a case chunk is no longer in band->chunks queue. */
	if (spdk_likely(chunk->state != FTL_CHUNK_STATE_BAD)) {
		result = ftl_band_next_chunk(band, chunk);
	} else {
		CIRCLEQ_FOREACH_REVERSE(entry, &band->chunks, circleq) {
			if (entry->pos > chunk->pos) {
				result = entry;
			} else {
				if (!result) {
					result = CIRCLEQ_FIRST(&band->chunks);
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
	memset(lba_map->map, 0, ftl_lba_map_pool_elem_size(band->dev));

	/* For open band all lba map segments are already cached */
	assert(band->state == FTL_BAND_STATE_PREP);
	num_segments = spdk_divide_round_up(ftl_num_band_lbks(band->dev), FTL_NUM_LBA_IN_BLOCK),
	ftl_lba_map_set_segment_state(&band->lba_map, 0, num_segments, FTL_LBA_MAP_SEG_CACHED);

	lba_map->num_vld = 0;
}

size_t
ftl_lba_map_pool_elem_size(struct spdk_ftl_dev *dev)
{
	/* lba map pool element has size capable to store lba map + segments map */
	return ftl_lba_map_num_lbks(dev) * FTL_BLOCK_SIZE +
	       spdk_divide_round_up(ftl_num_band_lbks(dev), FTL_NUM_LBA_IN_BLOCK);
}
