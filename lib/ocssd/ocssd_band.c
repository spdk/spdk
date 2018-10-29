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

#include <spdk/crc32.h>
#include <spdk/likely.h>
#include <spdk/ocssd.h>
#include "ocssd_band.h"
#include "ocssd_io.h"
#include "ocssd_core.h"
#include "ocssd_reloc.h"
#include "ocssd_debug.h"

/* TODO: define some signature for meta version */
#define OCSSD_MD_VER 1

typedef int (*ocssd_md_pack_fn)(struct ocssd_dev *, struct ocssd_md *, void *);

/* Metadata IO */
struct ocssd_md_io {
	/* Parent IO structure */
	struct ocssd_io		io;

	/* Destination metadata pointer */
	struct ocssd_md		*md;

	/* Metadata's buffer */
	void			*buf;

	/* Serialization/deserialization callback */
	ocssd_md_pack_fn	pack_fn;

	/* User's callback */
	struct ocssd_cb		cb;
};

struct __attribute__((packed)) ocssd_md_hdr {
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
struct __attribute__((packed, aligned(OCSSD_BLOCK_SIZE))) ocssd_tail_md {
	struct ocssd_md_hdr	hdr;

	/* Max number of lbks */
	uint64_t		num_lbks;
};
SPDK_STATIC_ASSERT(sizeof(struct ocssd_tail_md) == OCSSD_BLOCK_SIZE, "Incorrect metadata size");

struct __attribute__((packed)) ocssd_head_md {
	struct ocssd_md_hdr	hdr;

	/* Number of defrag cycles */
	uint64_t		wr_cnt;

	/* Number of surfaced LBAs */
	uint64_t		lba_cnt;

	/* Transfer size */
	uint32_t		xfer_size;
};

size_t
ocssd_tail_md_hdr_num_lbks(const struct ocssd_dev *dev)
{
	return ocssd_div_up(sizeof(struct ocssd_tail_md), OCSSD_BLOCK_SIZE);
}

size_t
ocssd_vld_map_num_lbks(const struct ocssd_dev *dev)
{
	return ocssd_div_up(ocssd_vld_map_size(dev), OCSSD_BLOCK_SIZE);
}

size_t
ocssd_lba_map_num_lbks(const struct ocssd_dev *dev)
{
	return ocssd_div_up(ocssd_num_band_lbks(dev) * sizeof(uint64_t), OCSSD_BLOCK_SIZE);
}

size_t
ocssd_head_md_num_lbks(const struct ocssd_dev *dev)
{
	return dev->xfer_size;
}

size_t
ocssd_tail_md_num_lbks(const struct ocssd_dev *dev)
{
	return ocssd_div_up(ocssd_tail_md_hdr_num_lbks(dev) +
			    ocssd_vld_map_num_lbks(dev) +
			    ocssd_lba_map_num_lbks(dev),
			    dev->xfer_size) * dev->xfer_size;
}

static uint64_t
ocssd_band_tail_md_offset(struct ocssd_band *band)
{
	return ocssd_band_num_usable_lbks(band) -
	       ocssd_tail_md_num_lbks(band->dev);
}

int
ocssd_band_full(struct ocssd_band *band, size_t offset)
{
	return offset == ocssd_band_tail_md_offset(band);
}

void
ocssd_band_write_failed(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;

	band->high_prio = 1;
	band->tail_md_ppa = ocssd_to_ppa(OCSSD_PPA_INVALID);

	if (!dev->df_band) {
		dev->df_band = band;
	}

	ocssd_reloc_add(dev->reloc, band, 0, ocssd_num_band_lbks(dev), 1);
	ocssd_band_set_state(band, OCSSD_BAND_STATE_CLOSED);
}

void
ocssd_band_clear_md(struct ocssd_band *band)
{
	memset(band->md.vld_map, 0, ocssd_vld_map_size(band->dev));
	memset(band->md.lba_map, 0, ocssd_num_band_lbks(band->dev) * sizeof(uint64_t));
	band->md.num_vld = 0;
}

static void
ocssd_band_free_md(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_md *md = &band->md;

	/* Try to free the LBA buffer only if the band is in closed/free state, */
	/* otherwise the metadata is in use. */
	if (!ocssd_band_check_state(band, OCSSD_BAND_STATE_CLOSED) &&
	    !ocssd_band_check_state(band, OCSSD_BAND_STATE_FREE)) {
		return;
	}

	if (md->ref_cnt > 0) {
		return;
	}

	assert(md->lba_map != NULL);
	assert(!band->high_prio);

	/* Verify that band's metadata is consistent with l2p */
	if (ocssd_band_has_chunks(band)) {
		ocssd_band_validate_md(band, band->md.lba_map);
	}

	spdk_mempool_put(dev->lba_pool, md->lba_map);
	md->lba_map = NULL;
}

static void
_ocssd_band_set_free(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_band *lband, *prev;

	/* Verify band's previous state */
	assert(band->state == OCSSD_BAND_STATE_CLOSED);

	if (band == dev->df_band) {
		dev->df_band = NULL;
	}

	/* Remove the band from the closed band list */
	LIST_REMOVE(band, list_entry);

	/* Keep the list sorted by band's write count */
	LIST_FOREACH(lband, &dev->free_bands, list_entry) {
		if (lband->md.wr_cnt > band->md.wr_cnt) {
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
		assert(prev->md.wr_cnt <= lband->md.wr_cnt);
	}
#endif
	dev->num_free++;
	ocssd_apply_limits(dev);
}

static void
_ocssd_band_set_opening(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_md *md = &band->md;

	/* Verify band's previous state */
	assert(band->state == OCSSD_BAND_STATE_PREP);
	LIST_REMOVE(band, list_entry);

	md->wr_cnt++;

	assert(dev->num_free > 0);
	dev->num_free--;

	ocssd_apply_limits(dev);
}

static void
_ocssd_band_set_closed(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_chunk *chunk;

	/* TODO: add this kind of check in band_set_state() */
	if (ocssd_band_check_state(band, OCSSD_BAND_STATE_CLOSED)) {
		return;
	}

	/* Set the state as free_md() checks for that */
	band->state = OCSSD_BAND_STATE_CLOSED;

	/* Free the md if there are no outstanding IOs */
	ocssd_band_release_md(band);

	if (ocssd_band_has_chunks(band)) {
		LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
		CIRCLEQ_FOREACH(chunk, &band->chunks, circleq) {
			ocssd_chunk_set_state(chunk, OCSSD_CHUNK_STATE_CLOSED);
		}
	} else {
		LIST_REMOVE(band, list_entry);
	}
}

static size_t
ocssd_dev_head_md_size(void)
{
	return sizeof(struct ocssd_head_md);
}

static uint32_t
ocssd_md_calc_crc(const struct ocssd_md_hdr *hdr, size_t size)
{
	size_t checkoff = offsetof(struct ocssd_md_hdr, checksum);
	size_t mdoff = checkoff + sizeof(hdr->checksum);
	uint32_t crc;

	crc = spdk_crc32c_update(hdr, checkoff, 0);
	return spdk_crc32c_update((const char *)hdr + mdoff, size - mdoff, crc);
}

static void
ocssd_set_md_hdr(struct ocssd_dev *dev, struct ocssd_md_hdr *hdr,
		 struct ocssd_md *md, size_t size)
{
	hdr->seq = md->seq;
	hdr->ver = OCSSD_MD_VER;
	hdr->uuid = dev->uuid;
	hdr->checksum = ocssd_md_calc_crc(hdr, size);
}

static int
ocssd_pack_head_md(struct ocssd_dev *dev, struct ocssd_md *md, void *data)
{
	struct ocssd_head_md *head = data;

	head->wr_cnt = md->wr_cnt;
	head->lba_cnt = dev->l2p_len;
	head->xfer_size = dev->xfer_size;
	ocssd_set_md_hdr(dev, &head->hdr, md, ocssd_dev_head_md_size());

	return OCSSD_MD_SUCCESS;
}

static int
ocssd_pack_tail_md(struct ocssd_dev *dev, struct ocssd_md *md, void *data)
{
	struct ocssd_tail_md *tail = data;
	size_t vld_size, map_size;
	void *vld_offset, *map_offset;

	vld_size = ocssd_vld_map_size(dev);
	map_size = ocssd_num_band_lbks(dev) * sizeof(uint64_t);
	vld_offset = (char *)data + ocssd_tail_md_hdr_num_lbks(dev) * OCSSD_BLOCK_SIZE;
	map_offset = (char *)vld_offset + ocssd_vld_map_num_lbks(dev) * OCSSD_BLOCK_SIZE;

	/* Clear out the buffer */
	memset(data, 0, ocssd_tail_md_num_lbks(dev) * OCSSD_BLOCK_SIZE);
	tail->num_lbks = ocssd_num_band_lbks(dev);

	pthread_spin_lock(&md->lock);
	memcpy(vld_offset, md->vld_map, vld_size);
	pthread_spin_unlock(&md->lock);

	memcpy(map_offset, md->lba_map, map_size);
	ocssd_set_md_hdr(dev, &tail->hdr, md, ocssd_tail_md_num_lbks(dev) * OCSSD_BLOCK_SIZE);

	return OCSSD_MD_SUCCESS;
}

static int
ocssd_md_hdr_vld(struct ocssd_dev *dev, const struct ocssd_md_hdr *hdr, size_t size)
{
	if (spdk_uuid_compare(&dev->uuid, &hdr->uuid) != 0) {
		return OCSSD_MD_NO_MD;
	}

	if (hdr->ver != OCSSD_MD_VER) {
		return OCSSD_MD_INVALID_VER;
	}

	if (ocssd_md_calc_crc(hdr, size) != hdr->checksum) {
		return OCSSD_MD_INVALID_CRC;
	}

	return OCSSD_MD_SUCCESS;
}

static int
ocssd_unpack_tail_md(struct ocssd_dev *dev, struct ocssd_md *md, void *data)
{
	struct ocssd_tail_md *tail = data;
	size_t vld_size, map_size;
	void *vld_offset, *map_offset;
	int rc;

	vld_size = ocssd_vld_map_size(dev);
	map_size = ocssd_num_band_lbks(dev) * sizeof(uint64_t);
	vld_offset = (char *)data + ocssd_tail_md_hdr_num_lbks(dev) * OCSSD_BLOCK_SIZE;
	map_offset = (char *)vld_offset + ocssd_vld_map_num_lbks(dev) * OCSSD_BLOCK_SIZE;

	rc = ocssd_md_hdr_vld(dev, &tail->hdr, ocssd_tail_md_num_lbks(dev) * OCSSD_BLOCK_SIZE);
	if (rc) {
		return rc;
	}

	if (tail->num_lbks != ocssd_num_band_lbks(dev)) {
		return OCSSD_MD_INVALID_SIZE;
	}

	if (md->vld_map) {
		memcpy(md->vld_map, vld_offset, vld_size);
	}

	if (md->lba_map) {
		memcpy(md->lba_map, map_offset, map_size);
	}

	md->seq = tail->hdr.seq;
	return OCSSD_MD_SUCCESS;
}

static int
ocssd_unpack_lba_map(struct ocssd_dev *dev, struct ocssd_md *md, void *data)
{
	memcpy(md->lba_map, data, ocssd_num_band_lbks(dev) * sizeof(uint64_t));
	return OCSSD_MD_SUCCESS;
}

static int
ocssd_unpack_head_md(struct ocssd_dev *dev, struct ocssd_md *md, void *data)
{
	struct ocssd_head_md *head = data;
	int rc;

	rc = ocssd_md_hdr_vld(dev, &head->hdr, ocssd_dev_head_md_size());
	if (rc) {
		return rc;
	}

	md->seq = head->hdr.seq;
	md->wr_cnt = head->wr_cnt;

	if (dev->global_md.l2p_len == 0) {
		dev->global_md.l2p_len = head->lba_cnt;
	}

	if (dev->global_md.l2p_len != head->lba_cnt) {
		return OCSSD_MD_INVALID_SIZE;
	}

	if (dev->xfer_size != head->xfer_size) {
		return OCSSD_MD_INVALID_SIZE;
	}

	return OCSSD_MD_SUCCESS;
}

struct ocssd_ppa
ocssd_band_tail_md_ppa(struct ocssd_band *band)
{
	struct ocssd_ppa ppa;
	struct ocssd_chunk *chunk;
	struct ocssd_dev *dev = band->dev;
	size_t xfer_size = dev->xfer_size;
	size_t num_req = ocssd_band_tail_md_offset(band) / xfer_size;
	size_t i;

	/* Metadata should be aligned to xfer size */
	assert(ocssd_band_tail_md_offset(band) % xfer_size == 0);

	chunk = CIRCLEQ_FIRST(&band->chunks);
	for (i = 0; i < num_req % band->num_chunks; ++i) {
		chunk = ocssd_band_next_chunk(band, chunk);
	}

	ppa.lbk = (num_req / band->num_chunks) * xfer_size;
	ppa.chk = band->id;
	ppa.pu = chunk->punit->start_ppa.pu;
	ppa.grp = chunk->punit->start_ppa.grp;

	return ppa;
}

struct ocssd_ppa
ocssd_band_head_md_ppa(struct ocssd_band *band)
{
	struct ocssd_ppa ppa;

	ppa = CIRCLEQ_FIRST(&band->chunks)->punit->start_ppa;
	ppa.chk = band->id;

	return ppa;
}

void
ocssd_band_set_state(struct ocssd_band *band, enum ocssd_band_state state)
{
	switch (state) {
	case OCSSD_BAND_STATE_FREE:
		_ocssd_band_set_free(band);
		break;

	case OCSSD_BAND_STATE_OPENING:
		_ocssd_band_set_opening(band);
		break;

	case OCSSD_BAND_STATE_CLOSED:
		_ocssd_band_set_closed(band);
		break;

	default:
		break;
	}

	band->state = state;
}

void
ocssd_band_set_addr(struct ocssd_band *band, uint64_t lba, struct ocssd_ppa ppa)
{
	struct ocssd_md *md = &band->md;
	uint64_t offset;

	if (ocssd_lba_invalid(lba)) {
		return;
	}

	offset = ocssd_band_lbkoff_from_ppa(band, ppa);

	ocssd_band_lock(band);

	md->num_vld++;
	md->lba_map[offset] = lba;
	ocssd_set_bit(offset, md->vld_map);

	ocssd_band_unlock(band);
}

size_t
ocssd_band_age(const struct ocssd_band *band)
{
	return (size_t)(band->dev->seq - band->md.seq);
}

size_t
ocssd_band_num_usable_lbks(const struct ocssd_band *band)
{
	return band->num_chunks * ocssd_dev_lbks_in_chunk(band->dev);
}

size_t
ocssd_band_user_lbks(const struct ocssd_band *band)
{
	return ocssd_band_num_usable_lbks(band) -
	       ocssd_head_md_num_lbks(band->dev) -
	       ocssd_tail_md_num_lbks(band->dev);
}

struct ocssd_band *
ocssd_band_from_ppa(struct ocssd_dev *dev, struct ocssd_ppa ppa)
{
	assert(ppa.chk < ocssd_dev_num_bands(dev));
	return &dev->bands[ppa.chk];
}

struct ocssd_chunk *
ocssd_band_chunk_from_ppa(struct ocssd_band *band, struct ocssd_ppa ppa)
{
	struct ocssd_dev *dev = band->dev;
	unsigned int punit;

	punit = ocssd_ppa_flatten_punit(dev, ppa);
	assert(punit < ocssd_dev_num_punits(dev));

	return &band->chunk_buf[punit];
}

uint64_t
ocssd_band_lbkoff_from_ppa(struct ocssd_band *band, struct ocssd_ppa ppa)
{
	struct ocssd_dev *dev = band->dev;
	unsigned int punit;

	punit = ocssd_ppa_flatten_punit(dev, ppa);
	assert(ppa.chk == band->id);

	return punit * ocssd_dev_lbks_in_chunk(dev) + ppa.lbk;
}

struct ocssd_ppa
ocssd_band_next_xfer_ppa(struct ocssd_band *band, struct ocssd_ppa ppa, size_t num_lbks)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_chunk *chunk;
	unsigned int punit_num;
	size_t num_xfers, num_stripes;

	assert(ppa.chk == band->id);

	punit_num = ocssd_ppa_flatten_punit(dev, ppa);
	chunk = &band->chunk_buf[punit_num];

	num_lbks += (ppa.lbk % dev->xfer_size);
	ppa.lbk  -= (ppa.lbk % dev->xfer_size);

#if defined(DEBUG)
	/* Check that the number of chunks has not been changed */
	struct ocssd_chunk *_chunk;
	size_t _num_chunks = 0;
	CIRCLEQ_FOREACH(_chunk, &band->chunks, circleq) {
		if (spdk_likely(!ocssd_chunk_is_bad(_chunk))) {
			_num_chunks++;
		}
	}
	assert(band->num_chunks == _num_chunks);
#endif
	num_stripes = (num_lbks / dev->xfer_size) / band->num_chunks;
	ppa.lbk  += num_stripes * dev->xfer_size;
	num_lbks -= num_stripes * dev->xfer_size * band->num_chunks;

	if (ppa.lbk > ocssd_dev_lbks_in_chunk(dev)) {
		return ocssd_to_ppa(OCSSD_PPA_INVALID);
	}

	num_xfers = num_lbks / dev->xfer_size;
	for (size_t i = 0; i < num_xfers; ++i) {
		/* When the last chunk is reached the lbk part of the address */
		/* needs to be increased by xfer_size */
		if (ocssd_band_chunk_is_last(band, chunk)) {
			ppa.lbk += dev->xfer_size;
			if (ppa.lbk > ocssd_dev_lbks_in_chunk(dev)) {
				return ocssd_to_ppa(OCSSD_PPA_INVALID);
			}
		}

		chunk = ocssd_band_next_operational_chunk(band, chunk);
		ppa.grp = chunk->start_ppa.grp;
		ppa.pu = chunk->start_ppa.pu;

		num_lbks -= dev->xfer_size;
	}

	if (num_lbks) {
		ppa.lbk += num_lbks;
		if (ppa.lbk > ocssd_dev_lbks_in_chunk(dev)) {
			return ocssd_to_ppa(OCSSD_PPA_INVALID);
		}
	}

	return ppa;
}

struct ocssd_ppa
ocssd_band_ppa_from_lbkoff(struct ocssd_band *band, uint64_t lbkoff)
{
	struct ocssd_ppa ppa = { .ppa = 0 };
	struct ocssd_dev *dev = band->dev;
	uint64_t punit;

	punit = lbkoff / ocssd_dev_lbks_in_chunk(dev) + dev->range.begin;

	ppa.lbk = lbkoff % ocssd_dev_lbks_in_chunk(dev);
	ppa.chk = band->id;
	ppa.pu = punit / dev->geo.num_grp;
	ppa.grp = punit % dev->geo.num_grp;

	return ppa;
}

struct ocssd_ppa
ocssd_band_next_ppa(struct ocssd_band *band, struct ocssd_ppa ppa, size_t offset)
{
	uint64_t lbkoff = ocssd_band_lbkoff_from_ppa(band, ppa);
	return ocssd_band_ppa_from_lbkoff(band, lbkoff + offset);
}

void
ocssd_band_acquire_md(struct ocssd_band *band)
{
	assert(band->md.lba_map != NULL);
	band->md.ref_cnt++;
}

int
ocssd_band_alloc_md(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_md *md = &band->md;

	assert(md->ref_cnt == 0);
	assert(md->lba_map == NULL);

	md->lba_map = spdk_mempool_get(dev->lba_pool);
	if (!md->lba_map) {
		return -1;
	}

	ocssd_band_acquire_md(band);
	return 0;
}

void
ocssd_band_release_md(struct ocssd_band *band)
{
	struct ocssd_md *md = &band->md;

	assert(md->ref_cnt > 0);
	assert(band->md.lba_map != NULL);
	md->ref_cnt--;

	ocssd_band_free_md(band);
}

static void
ocssd_read_md_cb(void *arg, int status)
{
	struct ocssd_md_io *md_io = arg;

	if (!status) {
		status = md_io->pack_fn(md_io->io.dev,
					md_io->md,
					md_io->buf);
	} else {
		status = OCSSD_MD_IO_FAILURE;
	}

	md_io->cb.fn(md_io->cb.ctx, status);
}

static struct ocssd_md_io *
ocssd_io_init_md_read(struct ocssd_dev *dev, struct ocssd_md *md, void *data, struct ocssd_ppa ppa,
		      struct ocssd_band *band, size_t lbk_cnt, size_t req_size, ocssd_md_pack_fn fn,
		      const struct ocssd_cb *cb)
{
	struct ocssd_md_io *io;
	struct ocssd_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= NULL,
		.band		= band,
		.size		= sizeof(*io),
		.flags		= OCSSD_IO_MEMORY | OCSSD_IO_MD | OCSSD_IO_PPA_MODE,
		.type		= OCSSD_IO_READ,
		.iov_cnt	= ocssd_div_up(lbk_cnt, req_size),
		.req_size	= req_size,
		.fn		= ocssd_read_md_cb,
		.data		= data,
	};

	io = (struct ocssd_md_io *)ocssd_io_init_internal(&opts);
	if (!io) {
		return NULL;
	}

	io->io.ppa = ppa;
	io->md = md;
	io->buf = data;
	io->pack_fn = fn;
	io->cb = *cb;

	return io;
}

static struct ocssd_io *
ocssd_io_init_md_write(struct ocssd_dev *dev, struct ocssd_band *band,
		       void *data, size_t req_cnt, ocssd_fn cb)
{
	struct ocssd_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.rwb_batch	= NULL,
		.band		= band,
		.size		= sizeof(struct ocssd_io),
		.flags		= OCSSD_IO_MEMORY | OCSSD_IO_MD | OCSSD_IO_PPA_MODE,
		.type		= OCSSD_IO_WRITE,
		.iov_cnt	= req_cnt,
		.req_size	= dev->xfer_size,
		.fn		= cb,
		.data		= data,
		.md		= NULL,
	};

	return ocssd_io_init_internal(&opts);
}

static int
ocssd_band_write_md(struct ocssd_band *band, void *data, size_t lbk_cnt,
		    ocssd_md_pack_fn md_fn, ocssd_fn cb)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_io *io;

	io = ocssd_io_init_md_write(dev, band, data,
				    ocssd_div_up(lbk_cnt, dev->xfer_size), cb);
	if (!io) {
		return -ENOMEM;
	}

	md_fn(dev, &band->md, data);

	return ocssd_io_write(io);
}

void
ocssd_band_md_clear(struct ocssd_md *md)
{
	md->seq = 0;
	md->num_vld = 0;
	md->wr_cnt = 0;
	md->lba_map = NULL;
}

int
ocssd_band_write_head_md(struct ocssd_band *band, void *data, ocssd_fn cb)
{
	return ocssd_band_write_md(band, data, ocssd_head_md_num_lbks(band->dev),
				   ocssd_pack_head_md, cb);
}

int
ocssd_band_write_tail_md(struct ocssd_band *band, void *data, ocssd_fn cb)
{
	return ocssd_band_write_md(band, data, ocssd_tail_md_num_lbks(band->dev),
				   ocssd_pack_tail_md, cb);
}

static struct ocssd_ppa
ocssd_band_lba_map_ppa(struct ocssd_band *band)
{
	return ocssd_band_next_xfer_ppa(band, band->tail_md_ppa,
					ocssd_tail_md_hdr_num_lbks(band->dev) +
					ocssd_vld_map_num_lbks(band->dev));
}

static int
ocssd_band_read_md(struct ocssd_band *band, struct ocssd_md *md, void *data, size_t lbk_cnt,
		   size_t req_size, struct ocssd_ppa start_ppa, ocssd_md_pack_fn unpack_fn,
		   const struct ocssd_cb *cb)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_md_io *io;

	io = ocssd_io_init_md_read(dev, md, data, start_ppa, band, lbk_cnt,
				   req_size, unpack_fn, cb);
	if (!io) {
		return -ENOMEM;
	}

	return ocssd_io_read((struct ocssd_io *)io);
}

int
ocssd_band_read_tail_md(struct ocssd_band *band, struct ocssd_md *md,
			void *data, struct ocssd_ppa ppa, const struct ocssd_cb *cb)
{
	return ocssd_band_read_md(band, md, data,
				  ocssd_tail_md_num_lbks(band->dev),
				  band->dev->xfer_size,
				  ppa,
				  ocssd_unpack_tail_md,
				  cb);
}

int
ocssd_band_read_lba_map(struct ocssd_band *band, struct ocssd_md *md,
			void *data, const struct ocssd_cb *cb)
{
	/* TODO: change this interface to allow reading parts of the LBA map instead of */
	/* reading whole metadata */
	return ocssd_band_read_md(band, md, data,
				  ocssd_lba_map_num_lbks(band->dev),
				  band->dev->xfer_size,
				  ocssd_band_lba_map_ppa(band),
				  ocssd_unpack_lba_map,
				  cb);
}

int
ocssd_band_read_head_md(struct ocssd_band *band, struct ocssd_md *md,
			void *data, const struct ocssd_cb *cb)
{
	return ocssd_band_read_md(band, md, data,
				  ocssd_head_md_num_lbks(band->dev),
				  band->dev->xfer_size,
				  ocssd_band_head_md_ppa(band),
				  ocssd_unpack_head_md,
				  cb);
}

static void
ocssd_band_remove_chunk(struct ocssd_band *band, struct ocssd_chunk *chunk)
{
	CIRCLEQ_REMOVE(&band->chunks, chunk, circleq);
	band->num_chunks--;
}

static void
ocssd_erase_fail(struct ocssd_io *io, int status)
{
	struct ocssd_chunk *chunk;
	char buf[128];

	SPDK_ERRLOG("Erase failed @ppa: %s, status: %d\n",
		    ocssd_ppa2str(io->ppa, buf, sizeof(buf)), status);

	chunk = ocssd_band_chunk_from_ppa(io->band, io->ppa);
	ocssd_chunk_set_state(chunk, OCSSD_CHUNK_STATE_BAD);
	ocssd_band_remove_chunk(io->band, chunk);
}

static void
ocssd_band_erase_cb(void *ctx, int status)
{
	struct ocssd_io *io = ctx;
	struct ocssd_chunk *chunk;

	if (spdk_unlikely(status)) {
		ocssd_erase_fail(io, status);
		return;
	}
	chunk = ocssd_band_chunk_from_ppa(io->band, io->ppa);
	ocssd_chunk_set_state(chunk, OCSSD_CHUNK_STATE_FREE);
}

int
ocssd_band_erase(struct ocssd_band *band)
{
	struct ocssd_chunk *chunk;
	struct ocssd_io *io;
	int rc = 0;

	assert(ocssd_band_check_state(band, OCSSD_BAND_STATE_CLOSED) ||
	       ocssd_band_check_state(band, OCSSD_BAND_STATE_FREE));

	ocssd_band_set_state(band, OCSSD_BAND_STATE_PREP);

	CIRCLEQ_FOREACH(chunk, &band->chunks, circleq) {
		if (chunk->state == OCSSD_CHUNK_STATE_FREE) {
			continue;
		}

		io = ocssd_io_erase_init(band, 1, ocssd_band_erase_cb);
		if (!io) {
			rc = -ENOMEM;
			break;
		}

		io->ppa = chunk->start_ppa;
		rc = ocssd_io_erase(io);
		if (rc) {
			assert(0);
			/* TODO: change band's state back to close? */
			break;
		}
	}

	return rc;
}

int
ocssd_band_write_prep(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;

	if (ocssd_band_alloc_md(band)) {
		return -1;
	}

	band->md.seq = ++dev->seq;
	return 0;
}

struct ocssd_chunk *
ocssd_band_next_operational_chunk(struct ocssd_band *band, struct ocssd_chunk *chunk)
{
	struct ocssd_chunk *result = NULL;
	struct ocssd_chunk *entry;

	if (spdk_unlikely(!ocssd_band_has_chunks(band))) {
		return NULL;
	}

	/* Erasing band may fail after it was assigned to wptr. */
	/* In such a case chunk is no longer in band->chunks queue. */
	if (spdk_likely(!ocssd_chunk_is_bad(chunk))) {
		result = ocssd_band_next_chunk(band, chunk);
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
