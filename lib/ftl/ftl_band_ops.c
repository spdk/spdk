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
#include "spdk/queue.h"
#include "spdk/bdev_module.h"

#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_internal.h"

static void
write_rq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_rq *rq = arg;
	struct ftl_zone *zone = rq->io.zone;
	struct ftl_band *band = rq->io.band;

	rq->success = success;
	if (spdk_likely(success)) {
		if (ftl_is_append_supported(rq->dev)) {
			rq->io.addr = spdk_bdev_io_get_append_location(bdev_io);
		}

		zone->info.write_pointer += rq->num_blocks;
		if (zone->info.write_pointer == zone->info.zone_id + zone->info.capacity) {
			zone->info.state = SPDK_BDEV_ZONE_STATE_FULL;
		}
	}

	zone->busy = false;

	assert(band->queue_depth > 0);
	band->queue_depth--;

	rq->owner.cb(rq);
	spdk_bdev_free_io(bdev_io);
}

static void
ftl_band_rq_bdev_write(void *_rq)
{
	struct ftl_rq *rq = _rq;
	struct ftl_band *band = rq->io.band;
	struct spdk_ftl_dev *dev = band->dev;
	int rc;

	if (ftl_is_append_supported(dev)) {
		rc = spdk_bdev_zone_appendv(dev->base_bdev_desc, dev->base_ioch,
					    rq->io_vec, rq->io_vec_size,
					    ftl_addr_get_zone_slba(dev, band->md->iter.addr),
					    rq->num_blocks, write_rq_end, rq);
	} else {
		rc = spdk_bdev_writev_blocks(dev->base_bdev_desc, dev->base_ioch,
					     rq->io_vec, rq->io_vec_size,
					     rq->io.addr, rq->num_blocks,
					     write_rq_end, rq);
	}

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			rq->io.bdev_io_wait.bdev = bdev;
			rq->io.bdev_io_wait.cb_fn = ftl_band_rq_bdev_write;
			rq->io.bdev_io_wait.cb_arg = rq;
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &rq->io.bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

void
ftl_band_rq_write(struct ftl_band *band, struct ftl_rq *rq)
{
	struct spdk_ftl_dev *dev = band->dev;
	rq->success = false;
	rq->io.band = band;
	rq->io.zone = band->zone;
	rq->io.addr = band->md->iter.addr;

	ftl_band_rq_bdev_write(rq);

	band->queue_depth++;
	dev->io_activity_total += rq->num_blocks;

	ftl_band_iter_advance(band, rq->num_blocks);
	if (ftl_band_filled(band, band->md->iter.offset)) {
		ftl_band_set_state(band, FTL_BAND_STATE_FULL);
		band->owner.state_change_fn(band);
	}
}

static void
ftl_band_rq_bdev_read(void *_entry);

static void
read_rq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_rq_entry *entry = arg;
	struct ftl_band *band = entry->io.band;
	struct ftl_rq *rq = ftl_rq_from_entry(entry);

	if (spdk_unlikely(!success)) {
		rq->success = false;
		ftl_band_rq_bdev_read(entry);
		spdk_bdev_free_io(bdev_io);
		return;
	}

	assert(band->queue_depth > 0);
	band->queue_depth--;

	rq->owner.cb(rq);
	spdk_bdev_free_io(bdev_io);
}

static void
ftl_band_rq_bdev_read(void *_entry)
{
	struct ftl_rq_entry *entry = _entry;
	struct ftl_rq *rq = ftl_rq_from_entry(entry);
	struct spdk_ftl_dev *dev = rq->dev;
	int rc;

	rc = spdk_bdev_read_blocks(dev->base_bdev_desc, dev->base_ioch, entry->io_payload,
				   entry->bdev_io.offset_blocks, entry->bdev_io.num_blocks,
				   read_rq_end, entry);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			entry->bdev_io.wait_entry.bdev = bdev;
			entry->bdev_io.wait_entry.cb_fn = ftl_band_rq_bdev_read;
			entry->bdev_io.wait_entry.cb_arg = entry;
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &entry->bdev_io.wait_entry);
		} else {
			ftl_abort();
		}
	}
}

void
ftl_band_rq_read(struct ftl_band *band, struct ftl_rq *rq)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_rq_entry *entry = &rq->entries[rq->iter.idx];

	assert(rq->iter.idx + rq->iter.count <= rq->num_blocks);

	rq->io.band = band;
	rq->io.zone = band->zone;
	rq->io.addr = band->md->iter.addr;
	entry->io.band = band;
	entry->bdev_io.offset_blocks = rq->io.addr;
	entry->bdev_io.num_blocks = rq->iter.count;

	ftl_band_rq_bdev_read(entry);

	dev->io_activity_total += rq->num_blocks;
	band->queue_depth++;
}

static void
write_brq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_basic_rq *brq = arg;
	struct ftl_zone *zone = brq->io.zone;
	struct ftl_band *band = brq->io.band;

	brq->success = success;
	if (spdk_likely(success)) {
		if (ftl_is_append_supported(brq->dev)) {
			brq->io.addr = spdk_bdev_io_get_append_location(bdev_io);
		}

		zone->info.write_pointer += brq->num_blocks;
		if (zone->info.write_pointer == zone->info.zone_id + zone->info.capacity) {
			zone->info.state = SPDK_BDEV_ZONE_STATE_FULL;
		}
	}

	zone->busy = false;

	assert(band->queue_depth > 0);
	band->queue_depth--;

	brq->owner.cb(brq);
	spdk_bdev_free_io(bdev_io);
}

static void
ftl_band_brq_bdev_write(void *_brq)
{
	struct ftl_basic_rq *brq = _brq;
	struct spdk_ftl_dev *dev = brq->dev;
	int rc;

	if (ftl_is_append_supported(dev)) {
		rc = spdk_bdev_zone_append(dev->base_bdev_desc, dev->base_ioch,
					   brq->io_payload,
					   ftl_addr_get_zone_slba(dev, brq->io.addr),
					   brq->num_blocks, write_brq_end, brq);
	} else {
		rc = spdk_bdev_write_blocks(dev->base_bdev_desc, dev->base_ioch,
					    brq->io_payload, brq->io.addr,
					    brq->num_blocks, write_brq_end, brq);
	}

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			brq->io.bdev_io_wait.bdev = bdev;
			brq->io.bdev_io_wait.cb_fn = ftl_band_brq_bdev_write;
			brq->io.bdev_io_wait.cb_arg = brq;
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &brq->io.bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

void
ftl_band_basic_rq_write(struct ftl_band *band, struct ftl_basic_rq *brq)
{
	struct spdk_ftl_dev *dev = band->dev;

	brq->io.addr = band->md->iter.addr;
	brq->io.band = band;
	brq->io.zone = band->zone;
	brq->success = false;

	ftl_band_brq_bdev_write(brq);

	dev->io_activity_total += brq->num_blocks;
	band->queue_depth++;
	ftl_band_iter_advance(band, brq->num_blocks);
	if (ftl_band_filled(band, band->md->iter.offset)) {
		ftl_band_set_state(band, FTL_BAND_STATE_FULL);
		band->owner.state_change_fn(band);
	}
}

static void
read_brq_end(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_basic_rq *brq = arg;
	struct ftl_band *band = brq->io.band;

	brq->success = success;

	assert(band->queue_depth > 0);
	band->queue_depth--;

	brq->owner.cb(brq);
	spdk_bdev_free_io(bdev_io);
}

static void
ftl_band_brq_bdev_read(void *_brq)
{
	struct ftl_basic_rq *brq = _brq;
	struct spdk_ftl_dev *dev = brq->dev;
	int rc;

	rc = spdk_bdev_read_blocks(dev->base_bdev_desc, dev->base_ioch,
				   brq->io_payload, brq->io.addr,
				   brq->num_blocks, read_brq_end, brq);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
			brq->io.bdev_io_wait.bdev = bdev;
			brq->io.bdev_io_wait.cb_fn = ftl_band_brq_bdev_read;
			brq->io.bdev_io_wait.cb_arg = brq;
			spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &brq->io.bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

void
ftl_band_basic_rq_read(struct ftl_band *band, struct ftl_basic_rq *brq)
{
	struct spdk_ftl_dev *dev = brq->dev;
	brq->io.band = band;

	ftl_band_brq_bdev_read(brq);

	brq->io.band->queue_depth++;
	dev->io_activity_total += brq->num_blocks;
}

static void
band_open_cb(int status, void *cb_arg)
{
	struct ftl_band *band = cb_arg;

	if (spdk_unlikely(status)) {
		ftl_md_persist_entry_retry(&band->md_persist_entry_ctx);
		return;
	}

	ftl_band_set_state(band, FTL_BAND_STATE_OPEN);
}

void
ftl_band_open(struct ftl_band *band, enum ftl_band_type type)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_band_md];
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_band_md];
	struct ftl_lba_map *lba_map = &band->lba_map;

	ftl_band_set_type(band, type);
	ftl_band_set_state(band, FTL_BAND_STATE_OPENING);

	memcpy(lba_map->band_dma_md, band->md, region->entry_size * FTL_BLOCK_SIZE);
	lba_map->band_dma_md->state = FTL_BAND_STATE_OPEN;
	lba_map->band_dma_md->lba_map_checksum = 0;

	if (spdk_unlikely(0 != band->lba_map.num_vld)) {
		/*
		 * This is inconsistent state, a band with valid block,
		 * it could be moved on the free list
		 */
		assert(0 == band->lba_map.num_vld);
		ftl_abort();
	}

	ftl_md_persist_entry(md, band->id, lba_map->band_dma_md, ftl_md_get_vss_buffer(md), band_open_cb, band,
			     &band->md_persist_entry_ctx);
}

static void
band_close_cb(int status, void *cb_arg)
{
	struct ftl_band *band = cb_arg;

	if (spdk_unlikely(status)) {
		ftl_md_persist_entry_retry(&band->md_persist_entry_ctx);
		return;
	}

	band->md->lba_map_checksum = band->lba_map.band_dma_md->lba_map_checksum;
	ftl_band_set_state(band, FTL_BAND_STATE_CLOSED);
}

static void
band_map_write_cb(struct ftl_basic_rq *brq)
{
	struct ftl_band *band = brq->io.band;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_band_md];
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_band_md];
	uint32_t band_map_crc;

	if (spdk_likely(brq->success)) {

		band_map_crc = spdk_crc32c_update(lba_map->dma_buf,
						  ftl_tail_md_num_blocks(dev) * FTL_BLOCK_SIZE, 0);
		memcpy(lba_map->band_dma_md, band->md, region->entry_size * FTL_BLOCK_SIZE);
		lba_map->band_dma_md->state = FTL_BAND_STATE_CLOSED;
		lba_map->band_dma_md->lba_map_checksum = band_map_crc;

		ftl_md_persist_entry(md, band->id, lba_map->band_dma_md, ftl_md_get_vss_buffer(md), band_close_cb, band,
				     &band->md_persist_entry_ctx);
	} else {
		/* Try to retry in case of failure */
		ftl_band_brq_bdev_write(brq);
		band->queue_depth++;
	}
}

void
ftl_band_close(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	void *metadata = band->lba_map.dma_buf;
	uint64_t num_blocks = ftl_tail_md_num_blocks(dev);

	/* Write LBA map first, after completion, set the state to close on nvcache, then internally */
	ftl_band_set_state(band, FTL_BAND_STATE_CLOSING);
	ftl_basic_rq_init(dev, &band->metadata_rq, metadata, num_blocks);
	ftl_basic_rq_set_owner(&band->metadata_rq, band_map_write_cb, band);

	ftl_band_basic_rq_write(band, &band->metadata_rq);
}

static void band_free_cb(int status, void *ctx)
{
	struct ftl_band *band = (struct ftl_band *)ctx;

	if (spdk_unlikely(status)) {
		ftl_md_persist_entry_retry(&band->md_persist_entry_ctx);
		return;
	}

	ftl_band_release_lba_map(band);
	FTL_DEBUGLOG(band->dev, "Band is going to free state. Band id: %u\n", band->id);
	ftl_band_set_state(band, FTL_BAND_STATE_FREE);
	assert(0 == band->lba_map.ref_cnt);
}

void
ftl_band_free(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_lba_map *lba_map = &band->lba_map;
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_band_md];
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_band_md];


	/*
	 * For zone_block vbdev there's no way to recover free band after shutdown since zone state
	 * and write pointer aren't persisted. For real ZNS drives the recovery flow will need to be adapted
	 * anyway to take into account these fields persisting and this will probably turn into a zone reset
	 */
	if (!ftl_is_zoned(band->dev)) {
		memcpy(lba_map->band_dma_md, band->md, region->entry_size * FTL_BLOCK_SIZE);
		lba_map->band_dma_md->state = FTL_BAND_STATE_FREE;
		lba_map->band_dma_md->lba_map_checksum = 0;

		ftl_md_persist_entry(md, band->id, lba_map->band_dma_md, ftl_md_get_vss_buffer(md), band_free_cb, band,
				     &band->md_persist_entry_ctx);

	} else {
		/* TODO: This is for jenkins tests only, any recovery from dirty shutdown will not work on zone ns */

		FTL_DEBUGLOG(dev, "Band is going to free state. Band id: %u\n", band->id);
		ftl_band_set_state(band, FTL_BAND_STATE_FREE);
		assert(0 == band->lba_map.ref_cnt);
	}
	/* TODO: The whole band erase code should probably be done here instead */
}

static void
read_md_cb(struct ftl_basic_rq *brq)
{
	struct ftl_band *band = brq->owner.priv;
	struct spdk_ftl_dev *dev = band->dev;
	ftl_band_ops_cb cb;
	uint32_t band_map_crc;
	bool success = true;
	void *priv;

	cb = band->owner.ops_fn;
	priv = band->owner.priv;

	if (!brq->success) {
		ftl_band_basic_rq_read(band, &band->metadata_rq);
		return;
	}

	band_map_crc = spdk_crc32c_update(band->lba_map.dma_buf,
					  ftl_tail_md_num_blocks(band->dev) * FTL_BLOCK_SIZE, 0);
	if (band->md->lba_map_checksum && band->md->lba_map_checksum != band_map_crc) {
		FTL_ERRLOG(dev, "GC error, inconsistent LBA map CRC\n");
		success = false;
	}
	band->owner.ops_fn = NULL;
	band->owner.priv = NULL;
	cb(band, priv, success);
}

static int
_read_md(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_basic_rq *rq = &band->metadata_rq;

	if (ftl_band_alloc_lba_map(band)) {
		return -ENOMEM;
	}

	/* Read LBA map */
	ftl_basic_rq_init(dev, rq, band->lba_map.band_map, ftl_lba_map_num_blocks(dev));
	ftl_basic_rq_set_owner(rq, read_md_cb, band);

	rq->io.band = band;
	rq->io.addr = ftl_band_lba_map_addr(band);
	rq->io.zone = ftl_band_zone_from_addr(band, rq->io.addr);

	ftl_band_basic_rq_read(band, &band->metadata_rq);

	return 0;
}

static void
read_md(void *band)
{
	int rc;

	rc = _read_md(band);
	if (spdk_unlikely(rc)) {
		spdk_thread_send_msg(spdk_get_thread(), read_md, band);
	}
}

static void
read_tail_md_cb(struct ftl_basic_rq *brq)
{
	struct ftl_band *band = brq->owner.priv;
	enum ftl_md_status status = FTL_MD_IO_FAILURE;
	ftl_band_md_cb cb;
	void *priv;

	if (spdk_unlikely(!brq->success)) {
		ftl_band_basic_rq_read(band, &band->metadata_rq);
		return;
	}

	cb = band->owner.md_fn;
	band->owner.md_fn = NULL;

	priv = band->owner.priv;
	band->owner.priv = NULL;

	status = FTL_MD_SUCCESS;

	cb(band, priv, status);
}

void
ftl_band_read_tail_brq_md(struct ftl_band *band, ftl_band_md_cb cb, void *cntx)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_basic_rq *rq = &band->metadata_rq;

	ftl_basic_rq_init(dev, rq, band->lba_map.dma_buf, ftl_tail_md_num_blocks(dev));
	ftl_basic_rq_set_owner(rq, read_tail_md_cb, band);

	assert(!band->owner.md_fn);
	assert(!band->owner.priv);
	band->owner.md_fn = cb;
	band->owner.priv = cntx;

	rq->io.band = band;
	rq->io.addr = band->tail_md_addr;
	rq->io.zone = ftl_band_zone_from_addr(band, rq->io.addr);

	ftl_band_basic_rq_read(band, &band->metadata_rq);
}

void
ftl_band_get_next_gc(struct spdk_ftl_dev *dev, ftl_band_ops_cb cb, void *cntx)
{
	struct ftl_band *band = ftl_band_search_next_to_defrag(dev);

	/* if disk is very small, GC start very early that no band is ready for it */
	if (spdk_unlikely(!band)) {
		cb(NULL, cntx, false);
		return;
	}

	/* Only one owner is allowed */
	assert(!band->queue_depth);
	assert(!band->owner.ops_fn);
	assert(!band->owner.priv);
	band->owner.ops_fn = cb;
	band->owner.priv = cntx;

	read_md(band);
}
