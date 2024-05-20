/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

#include "spdk/bdev_module.h"
#include "spdk/crc32.h"

#include "ftl_internal.h"
#include "ftl_band.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "ftl_nv_cache_io.h"
#include "ftl_writer.h"
#include "mngt/ftl_mngt.h"

struct ftl_p2l_ckpt {
	TAILQ_ENTRY(ftl_p2l_ckpt)	link;
	union ftl_md_vss		*vss_md_page;
	struct ftl_md			*md;
	struct ftl_layout_region	*layout_region;
	uint64_t			num_pages;
	uint64_t			pages_per_xfer;

#if defined(DEBUG)
	uint64_t			dbg_bmp_sz;
	void				*dbg_bmp;
	struct ftl_bitmap		*bmp;
#endif
};

static struct ftl_p2l_ckpt *
ftl_p2l_ckpt_new(struct spdk_ftl_dev *dev, int region_type)
{
	struct ftl_p2l_ckpt *ckpt;
	struct ftl_layout_region *region = ftl_layout_region_get(dev, region_type);

	ckpt = calloc(1, sizeof(struct ftl_p2l_ckpt));
	if (!ckpt) {
		return NULL;
	}

	ckpt->layout_region = region;
	ckpt->md = dev->layout.md[region_type];
	ckpt->pages_per_xfer = dev->layout.p2l.pages_per_xfer;
	ckpt->num_pages = dev->layout.p2l.ckpt_pages;
	if (dev->nv_cache.md_size) {
		ckpt->vss_md_page = ftl_md_vss_buf_alloc(region, region->num_entries);
		if (!ckpt->vss_md_page) {
			free(ckpt);
			return NULL;
		}
	}

#if defined(DEBUG)
	/* The bitmap size must be a multiple of word size (8b) - round up */
	ckpt->dbg_bmp_sz = spdk_divide_round_up(ckpt->num_pages, 8);

	ckpt->dbg_bmp = calloc(1, ckpt->dbg_bmp_sz);
	assert(ckpt->dbg_bmp);
	ckpt->bmp = ftl_bitmap_create(ckpt->dbg_bmp, ckpt->dbg_bmp_sz);
	assert(ckpt->bmp);
#endif

	return ckpt;
}

static void
ftl_p2l_ckpt_destroy(struct ftl_p2l_ckpt *ckpt)
{
#if defined(DEBUG)
	ftl_bitmap_destroy(ckpt->bmp);
	free(ckpt->dbg_bmp);
#endif
	spdk_dma_free(ckpt->vss_md_page);
	free(ckpt);
}

int
ftl_p2l_ckpt_init(struct spdk_ftl_dev *dev)
{
	int region_type;
	struct ftl_p2l_ckpt *ckpt;

	TAILQ_INIT(&dev->p2l_ckpt.free);
	TAILQ_INIT(&dev->p2l_ckpt.inuse);
	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX;
	     region_type++) {
		ckpt = ftl_p2l_ckpt_new(dev, region_type);
		if (!ckpt) {
			return -1;
		}
		TAILQ_INSERT_TAIL(&dev->p2l_ckpt.free, ckpt, link);
	}
	return 0;
}

void
ftl_p2l_ckpt_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_p2l_ckpt *ckpt, *ckpt_next;

	TAILQ_FOREACH_SAFE(ckpt, &dev->p2l_ckpt.free, link, ckpt_next) {
		TAILQ_REMOVE(&dev->p2l_ckpt.free, ckpt, link);
		ftl_p2l_ckpt_destroy(ckpt);
	}

	TAILQ_FOREACH_SAFE(ckpt, &dev->p2l_ckpt.inuse, link, ckpt_next) {
		TAILQ_REMOVE(&dev->p2l_ckpt.inuse, ckpt, link);
		ftl_p2l_ckpt_destroy(ckpt);
	}
}

struct ftl_p2l_ckpt *
ftl_p2l_ckpt_acquire(struct spdk_ftl_dev *dev)
{
	struct ftl_p2l_ckpt *ckpt;

	ckpt = TAILQ_FIRST(&dev->p2l_ckpt.free);
	assert(ckpt);
	TAILQ_REMOVE(&dev->p2l_ckpt.free, ckpt, link);
	TAILQ_INSERT_TAIL(&dev->p2l_ckpt.inuse, ckpt, link);
	return ckpt;
}

void
ftl_p2l_ckpt_release(struct spdk_ftl_dev *dev, struct ftl_p2l_ckpt *ckpt)
{
	assert(ckpt);
#if defined(DEBUG)
	memset(ckpt->dbg_bmp, 0, ckpt->dbg_bmp_sz);
#endif
	TAILQ_REMOVE(&dev->p2l_ckpt.inuse, ckpt, link);
	TAILQ_INSERT_TAIL(&dev->p2l_ckpt.free, ckpt, link);
}

static void
ftl_p2l_ckpt_issue_end(int status, void *arg)
{
	struct ftl_rq *rq = arg;
	assert(rq);

	if (status) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		/* retry */
		ftl_md_persist_entry_retry(&rq->md_persist_entry_ctx);
		return;
#else
		ftl_abort();
#endif
	}

	assert(rq->io.band->queue_depth > 0);
	rq->io.band->queue_depth--;

	rq->owner.cb(rq);
}

void
ftl_p2l_ckpt_issue(struct ftl_rq *rq)
{
	struct ftl_rq_entry *iter = rq->entries;
	struct spdk_ftl_dev *dev = rq->dev;
	ftl_addr addr = rq->io.addr;
	struct ftl_p2l_ckpt *ckpt = NULL;
	struct ftl_p2l_ckpt_page_no_vss *map_page;
	struct ftl_band *band;
	uint64_t band_offs, p2l_map_page_no, cur_page, i, j;

	assert(rq);
	band = rq->io.band;
	ckpt = band->p2l_map.p2l_ckpt;
	assert(ckpt);
	assert(rq->num_blocks == dev->xfer_size);

	/* Derive the P2L map page no */
	band_offs = ftl_band_block_offset_from_addr(band, rq->io.addr);
	p2l_map_page_no = band_offs / dev->xfer_size * ckpt->pages_per_xfer;
	assert(p2l_map_page_no < ckpt->num_pages);

	/* Get the corresponding P2L map page - the underlying stored data has the same entries as in the end metadata of band P2L (ftl_p2l_map_entry),
	 * however we're interested in a whole page (4KiB) worth of content and submit it in two requests with additional metadata
	 */
	map_page = ftl_md_get_buffer(ckpt->md);
	assert(map_page);
	map_page += p2l_map_page_no;
	i = 0;
	for (cur_page = 0; cur_page < ckpt->pages_per_xfer; cur_page++) {
		struct ftl_p2l_ckpt_page_no_vss *page = map_page + cur_page;
		/* Update the band P2L map */
		for (j = 0; i < rq->num_blocks && j < FTL_NUM_P2L_ENTRIES_NO_VSS; i++, iter++, j++) {
			if (iter->lba != FTL_LBA_INVALID) {
				/* This is compaction or reloc */
				assert(!ftl_addr_in_nvc(rq->dev, addr));
				ftl_band_set_p2l(band, iter->lba, addr, iter->seq_id);
			}
			page->map[j].lba = iter->lba;
			page->map[j].seq_id = iter->seq_id;
			addr = ftl_band_next_addr(band, addr, 1);
		}

		/* Set up the md */
		page->metadata.p2l_ckpt.seq_id = band->md->seq;
		page->metadata.p2l_ckpt.count = j;

#if defined(DEBUG)
		ftl_bitmap_set(ckpt->bmp, p2l_map_page_no + cur_page);
#endif
		page->metadata.p2l_ckpt.p2l_checksum = spdk_crc32c_update(page->map,
						       FTL_NUM_P2L_ENTRIES_NO_VSS * sizeof(struct ftl_p2l_map_entry), 0);
	}
	/* Save the P2L map entry */
	ftl_md_persist_entries(ckpt->md, p2l_map_page_no, ckpt->pages_per_xfer, map_page, NULL,
			       ftl_p2l_ckpt_issue_end, rq, &rq->md_persist_entry_ctx);
}

#if defined(DEBUG)
static void
ftl_p2l_validate_pages(struct ftl_band *band, struct ftl_p2l_ckpt *ckpt,
		       uint64_t page_begin, uint64_t page_end, bool val)
{
	uint64_t page_no;

	for (page_no = page_begin; page_no < page_end; page_no++) {
		assert(ftl_bitmap_get(ckpt->bmp, page_no) == val);
	}
}

void
ftl_p2l_validate_ckpt(struct ftl_band *band)
{
	struct ftl_p2l_ckpt *ckpt = band->p2l_map.p2l_ckpt;
	uint64_t num_blks_tail_md = ftl_tail_md_num_blocks(band->dev);
	uint64_t num_pages_tail_md;

	if (!ckpt) {
		return;
	}

	num_pages_tail_md = num_blks_tail_md / band->dev->xfer_size * ckpt->pages_per_xfer;

	assert(num_blks_tail_md % band->dev->xfer_size == 0);

	/* all data pages written */
	ftl_p2l_validate_pages(band, ckpt,
			       0, ckpt->num_pages - num_pages_tail_md, true);

	/* tail md pages not written */
	ftl_p2l_validate_pages(band, ckpt, ckpt->num_pages - num_pages_tail_md,
			       ckpt->num_pages, false);
}
#endif

static struct ftl_band *
ftl_get_band_from_region(struct spdk_ftl_dev *dev, enum ftl_layout_region_type type)
{
	struct ftl_band *band = NULL;
	uint64_t i;

	assert(type >= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN);
	assert(type <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX);

	for (i = 0; i < ftl_get_num_bands(dev); i++) {
		band = &dev->bands[i];
		if ((band->md->state == FTL_BAND_STATE_OPEN ||
		     band->md->state == FTL_BAND_STATE_FULL) &&
		    band->md->p2l_md_region == type) {
			return band;
		}
	}

	return NULL;
}

static void ftl_mngt_persist_band_p2l(struct ftl_mngt_process *mngt, struct ftl_p2l_sync_ctx *ctx);

static void
ftl_p2l_ckpt_persist_end(int status, void *arg)
{
	struct ftl_mngt_process *mngt = arg;
	struct ftl_p2l_sync_ctx *ctx;

	assert(mngt);

	if (status) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	ctx = ftl_mngt_get_step_ctx(mngt);
	ctx->xfer_start++;

	if (ctx->xfer_start == ctx->xfer_end) {
		ctx->md_region++;
		ftl_mngt_continue_step(mngt);
	} else {
		ftl_mngt_persist_band_p2l(mngt, ctx);
	}
}

static void
ftl_mngt_persist_band_p2l(struct ftl_mngt_process *mngt, struct ftl_p2l_sync_ctx *ctx)
{
	struct ftl_band *band = ctx->band;
	struct ftl_p2l_ckpt_page_no_vss *map_page;
	struct ftl_p2l_map_entry *band_entries;
	struct ftl_p2l_ckpt *ckpt;
	struct spdk_ftl_dev *dev = band->dev;
	uint64_t cur_page;
	uint64_t lbas_synced = 0;

	ckpt = band->p2l_map.p2l_ckpt;

	map_page = ftl_md_get_buffer(ckpt->md);
	assert(map_page);

	map_page += ctx->xfer_start * ckpt->pages_per_xfer;

	for (cur_page = 0; cur_page < ckpt->pages_per_xfer; cur_page++) {
		struct ftl_p2l_ckpt_page_no_vss *page = map_page + cur_page;
		uint64_t lbas_to_copy = spdk_min(FTL_NUM_P2L_ENTRIES_NO_VSS, dev->xfer_size - lbas_synced);

		band_entries = band->p2l_map.band_map + ctx->xfer_start * dev->xfer_size + lbas_synced;
		memcpy(page->map, band_entries, lbas_to_copy * sizeof(struct ftl_p2l_map_entry));

		page->metadata.p2l_ckpt.seq_id = band->md->seq;
		page->metadata.p2l_ckpt.p2l_checksum = spdk_crc32c_update(page->map,
						       FTL_NUM_P2L_ENTRIES_NO_VSS * sizeof(struct ftl_p2l_map_entry), 0);
		page->metadata.p2l_ckpt.count = lbas_to_copy;
		lbas_synced += lbas_to_copy;
	}

	assert(lbas_synced == dev->xfer_size);
	/* Save the P2L map entry */
	ftl_md_persist_entries(ckpt->md, ctx->xfer_start * ckpt->pages_per_xfer, ckpt->pages_per_xfer,
			       map_page, NULL,
			       ftl_p2l_ckpt_persist_end, mngt, &band->md_persist_entry_ctx);
}

void
ftl_mngt_persist_bands_p2l(struct ftl_mngt_process *mngt)
{
	struct ftl_p2l_sync_ctx *ctx = ftl_mngt_get_step_ctx(mngt);
	struct ftl_band *band;
	uint64_t band_offs, num_xfers;

	if (ctx->md_region > FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX) {
		ftl_mngt_next_step(mngt);
		return;
	}

	band = ftl_get_band_from_region(ftl_mngt_get_dev(mngt), ctx->md_region);

	/* No band has the md region assigned (shutdown happened before next_band was assigned) */
	if (!band) {
		ctx->xfer_start = 0;
		ctx->xfer_end = 0;
		ctx->md_region++;
		ftl_mngt_continue_step(mngt);
		return;
	}

	band_offs = ftl_band_block_offset_from_addr(band, band->md->iter.addr);
	num_xfers = band_offs / band->dev->xfer_size;

	ctx->xfer_start = 0;
	ctx->xfer_end = num_xfers;
	ctx->band = band;

	/* Band wasn't written to - no need to sync its P2L */
	if (ctx->xfer_end == 0) {
		ctx->md_region++;
		ftl_mngt_continue_step(mngt);
		return;
	}

	ftl_mngt_persist_band_p2l(mngt, ctx);
}

uint64_t
ftl_mngt_p2l_ckpt_get_seq_id(struct spdk_ftl_dev *dev, int md_region)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_md *md = layout->md[md_region];
	struct ftl_p2l_ckpt_page_no_vss *page = ftl_md_get_buffer(md);
	uint64_t page_no, seq_id = 0;

	for (page_no = 0; page_no < layout->p2l.ckpt_pages; page_no++, page++) {
		if (seq_id < page->metadata.p2l_ckpt.seq_id) {
			seq_id = page->metadata.p2l_ckpt.seq_id;
		}
	}
	return seq_id;
}

int
ftl_mngt_p2l_ckpt_restore(struct ftl_band *band, uint32_t md_region, uint64_t seq_id)
{
	struct ftl_layout *layout = &band->dev->layout;
	struct ftl_md *md = layout->md[md_region];
	struct ftl_p2l_ckpt_page_no_vss *page = ftl_md_get_buffer(md);
	struct ftl_p2l_map_entry *band_entries;
	struct spdk_ftl_dev *dev = band->dev;
	uint64_t page_no, page_max = 0, xfer_count, lbas_synced;
	uint64_t pages_per_xfer = spdk_divide_round_up(dev->xfer_size, FTL_NUM_P2L_ENTRIES_NO_VSS);
	bool page_found = false;

	assert(band->md->p2l_md_region == md_region);
	if (band->md->p2l_md_region != md_region) {
		return -EINVAL;
	}

	assert(band->md->seq == seq_id);
	if (band->md->seq != seq_id) {
		return -EINVAL;
	}

	for (page_no = 0; page_no < layout->p2l.ckpt_pages; page_no++, page++) {
		if (page->metadata.p2l_ckpt.seq_id != seq_id) {
			continue;
		}

		page_max = page_no;
		page_found = true;

		if (page->metadata.p2l_ckpt.p2l_checksum &&
		    page->metadata.p2l_ckpt.p2l_checksum != spdk_crc32c_update(page->map,
				    FTL_NUM_P2L_ENTRIES_NO_VSS * sizeof(struct ftl_p2l_map_entry), 0)) {
			ftl_stats_crc_error(band->dev, FTL_STATS_TYPE_MD_NV_CACHE);
			return -EINVAL;
		}

		xfer_count = page_no / pages_per_xfer;
		lbas_synced = (page_no % pages_per_xfer) * FTL_NUM_P2L_ENTRIES_NO_VSS;

		/* Restore the page from P2L checkpoint */
		band_entries = band->p2l_map.band_map + xfer_count * dev->xfer_size + lbas_synced;

		memcpy(band_entries, page->map, page->metadata.p2l_ckpt.count * sizeof(struct ftl_p2l_map_entry));
	}

	assert(page_found);
	if (!page_found) {
		return -EINVAL;
	}

	/* Restore check point in band P2L map */
	band->p2l_map.p2l_ckpt = ftl_p2l_ckpt_acquire_region_type(
					 band->dev, md_region);

	/* Align page_max to xfer_size aligned pages */
	if ((page_max + 1) % pages_per_xfer != 0) {
		page_max += (pages_per_xfer - page_max % pages_per_xfer - 1);
	}
#ifdef DEBUG
	/* Set check point valid map for validation */
	struct ftl_p2l_ckpt *ckpt = band->p2l_map.p2l_ckpt;
	for (uint64_t i = 0; i <= page_max; i++) {
		ftl_bitmap_set(ckpt->bmp, i);
	}
#endif

	ftl_band_iter_init(band);
	/* Align page max to xfer size and set iter */
	ftl_band_iter_set(band, (page_max / band->p2l_map.p2l_ckpt->pages_per_xfer + 1) * dev->xfer_size);

	return 0;
}

enum ftl_layout_region_type
ftl_p2l_ckpt_region_type(const struct ftl_p2l_ckpt *ckpt) {
	return ckpt->layout_region->type;
}

struct ftl_p2l_ckpt *
ftl_p2l_ckpt_acquire_region_type(struct spdk_ftl_dev *dev, uint32_t region_type)
{
	struct ftl_p2l_ckpt *ckpt = NULL;

	TAILQ_FOREACH(ckpt, &dev->p2l_ckpt.free, link) {
		if (ckpt->layout_region->type == region_type) {
			break;
		}
	}

	assert(ckpt);

	TAILQ_REMOVE(&dev->p2l_ckpt.free, ckpt, link);
	TAILQ_INSERT_TAIL(&dev->p2l_ckpt.inuse, ckpt, link);

	return ckpt;
}

int
ftl_mngt_p2l_ckpt_restore_clean(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_p2l_ckpt_page_no_vss *page;
	enum ftl_layout_region_type md_region = band->md->p2l_md_region;
	struct ftl_p2l_ckpt *ckpt;
	uint64_t page_no;
	uint64_t num_written_pages, lbas_synced;

	if (md_region < FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN ||
	    md_region > FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MAX) {
		return -EINVAL;
	}

	assert(band->md->iter.offset % dev->xfer_size == 0);

	/* Associate band with md region before shutdown */
	if (!band->p2l_map.p2l_ckpt) {
		band->p2l_map.p2l_ckpt = ftl_p2l_ckpt_acquire_region_type(dev, md_region);
	}

	/* Band was opened but no data was written */
	if (band->md->iter.offset == 0) {
		return 0;
	}

	ckpt = band->p2l_map.p2l_ckpt;
	num_written_pages = band->md->iter.offset / dev->xfer_size * ckpt->pages_per_xfer;

	page_no = 0;
	lbas_synced = 0;

	/* Restore P2L map up to last written page */
	page = ftl_md_get_buffer(layout->md[md_region]);


	for (; page_no < num_written_pages; page_no++, page++) {
		assert(page->metadata.p2l_ckpt.seq_id == band->md->seq);
		/* Restore the page from P2L checkpoint */
		memcpy(band->p2l_map.band_map + lbas_synced, page->map,
		       page->metadata.p2l_ckpt.count * sizeof(struct ftl_p2l_map_entry));

		lbas_synced += page->metadata.p2l_ckpt.count;

#if defined(DEBUG)
		assert(ftl_bitmap_get(band->p2l_map.p2l_ckpt->bmp, page_no) == false);
		ftl_bitmap_set(band->p2l_map.p2l_ckpt->bmp, page_no);
#endif
	}

	assert(lbas_synced % dev->xfer_size == 0);

	assert(page->metadata.p2l_ckpt.seq_id < band->md->seq);

	return 0;
}

void
ftl_mngt_p2l_ckpt_restore_shm_clean(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	enum ftl_layout_region_type md_region = band->md->p2l_md_region;

	/* Associate band with md region before shutdown */
	if (!band->p2l_map.p2l_ckpt) {
		band->p2l_map.p2l_ckpt = ftl_p2l_ckpt_acquire_region_type(dev, md_region);
	}

#if defined(DEBUG)
	uint64_t page_no;
	uint64_t num_written_pages;

	assert(band->md->iter.offset % dev->xfer_size == 0);
	num_written_pages = band->md->iter.offset / dev->xfer_size * band->p2l_map.p2l_ckpt->pages_per_xfer;

	/* Band was opened but no data was written */
	if (band->md->iter.offset == 0) {
		return;
	}

	/* Set page number to first data page - skip head md */
	page_no = 0;

	for (; page_no < num_written_pages; page_no++) {
		assert(ftl_bitmap_get(band->p2l_map.p2l_ckpt->bmp, page_no) == false);
		ftl_bitmap_set(band->p2l_map.p2l_ckpt->bmp, page_no);
	}
#endif
}
