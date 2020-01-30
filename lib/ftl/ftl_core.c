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

#include "spdk/likely.h"
#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/io_channel.h"
#include "spdk/bdev_module.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"
#include "spdk/ftl.h"
#include "spdk/crc32.h"

#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_io.h"
#include "ftl_rwb.h"
#include "ftl_debug.h"
#include "ftl_reloc.h"

struct ftl_band_flush {
	struct spdk_ftl_dev		*dev;
	/* Number of bands left to be flushed */
	size_t				num_bands;
	/* User callback */
	spdk_ftl_fn			cb_fn;
	/* Callback's argument */
	void				*cb_arg;
	/* List link */
	LIST_ENTRY(ftl_band_flush)	list_entry;
};

struct ftl_wptr {
	/* Owner device */
	struct spdk_ftl_dev		*dev;

	/* Current address */
	struct ftl_addr			addr;

	/* Band currently being written to */
	struct ftl_band			*band;

	/* Current logical block's offset */
	uint64_t			offset;

	/* Current zone */
	struct ftl_zone			*zone;

	/* Pending IO queue */
	TAILQ_HEAD(, ftl_io)		pending_queue;

	/* List link */
	LIST_ENTRY(ftl_wptr)		list_entry;

	/*
	 * If setup in direct mode, there will be no offset or band state update after IO.
	 * The zoned bdev address is not assigned by wptr, and is instead taken directly
	 * from the request.
	 */
	bool				direct_mode;

	/* Number of outstanding write requests */
	uint32_t			num_outstanding;

	/* Marks that the band related to this wptr needs to be closed as soon as possible */
	bool				flush;
};

struct ftl_flush {
	/* Owner device */
	struct spdk_ftl_dev		*dev;

	/* Number of batches to wait for */
	size_t				num_req;

	/* Callback */
	struct {
		spdk_ftl_fn		fn;
		void			*ctx;
	} cb;

	/* Batch bitmap */
	struct spdk_bit_array		*bmap;

	/* List link */
	LIST_ENTRY(ftl_flush)		list_entry;
};

static int
ftl_rwb_flags_from_io(const struct ftl_io *io)
{
	int valid_flags = FTL_IO_INTERNAL | FTL_IO_WEAK | FTL_IO_PAD;
	return io->flags & valid_flags;
}

static int
ftl_rwb_entry_weak(const struct ftl_rwb_entry *entry)
{
	return entry->flags & FTL_IO_WEAK;
}

static void
ftl_wptr_free(struct ftl_wptr *wptr)
{
	if (!wptr) {
		return;
	}

	free(wptr);
}

static void
ftl_remove_wptr(struct ftl_wptr *wptr)
{
	struct spdk_ftl_dev *dev = wptr->dev;
	struct ftl_band_flush *flush, *tmp;

	if (spdk_unlikely(wptr->flush)) {
		LIST_FOREACH_SAFE(flush, &dev->band_flush_list, list_entry, tmp) {
			assert(flush->num_bands > 0);
			if (--flush->num_bands == 0) {
				flush->cb_fn(flush->cb_arg, 0);
				LIST_REMOVE(flush, list_entry);
				free(flush);
			}
		}
	}

	LIST_REMOVE(wptr, list_entry);
	ftl_wptr_free(wptr);
}

static void
ftl_io_cmpl_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct spdk_ftl_dev *dev = io->dev;

	if (spdk_unlikely(!success)) {
		io->status = -EIO;
	}

	ftl_trace_completion(dev, io, FTL_TRACE_COMPLETION_DISK);

	if (io->type == FTL_IO_WRITE && ftl_is_append_supported(dev)) {
		assert(io->parent);
		io->parent->addr.offset = spdk_bdev_io_get_append_location(bdev_io);
	}

	ftl_io_dec_req(io);
	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}

	spdk_bdev_free_io(bdev_io);
}

static void
ftl_halt_writes(struct spdk_ftl_dev *dev, struct ftl_band *band)
{
	struct ftl_wptr *wptr = NULL;

	LIST_FOREACH(wptr, &dev->wptr_list, list_entry) {
		if (wptr->band == band) {
			break;
		}
	}

	/* If the band already has the high_prio flag set, other writes must */
	/* have failed earlier, so it's already taken care of. */
	if (band->high_prio) {
		assert(wptr == NULL);
		return;
	}

	ftl_band_write_failed(band);
	ftl_remove_wptr(wptr);
}

static struct ftl_wptr *
ftl_wptr_from_band(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_wptr *wptr = NULL;

	LIST_FOREACH(wptr, &dev->wptr_list, list_entry) {
		if (wptr->band == band) {
			return wptr;
		}
	}

	return NULL;
}

static void
ftl_md_write_fail(struct ftl_io *io, int status)
{
	struct ftl_band *band = io->band;
	struct ftl_wptr *wptr;
	char buf[128];

	wptr = ftl_wptr_from_band(band);
	assert(wptr);

	SPDK_ERRLOG("Metadata write failed @addr: %s, status: %d\n",
		    ftl_addr2str(wptr->addr, buf, sizeof(buf)), status);

	ftl_halt_writes(io->dev, band);
}

static void
ftl_md_write_cb(struct ftl_io *io, void *arg, int status)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_band *band = io->band;
	struct ftl_wptr *wptr;
	size_t id;

	wptr = ftl_wptr_from_band(band);
	assert(wptr);

	if (status) {
		ftl_md_write_fail(io, status);
		return;
	}

	ftl_band_set_next_state(band);
	if (band->state == FTL_BAND_STATE_CLOSED) {
		if (ftl_dev_has_nv_cache(dev)) {
			pthread_spin_lock(&nv_cache->lock);
			nv_cache->num_available += ftl_band_user_blocks(band);

			if (spdk_unlikely(nv_cache->num_available > nv_cache->num_data_blocks)) {
				nv_cache->num_available = nv_cache->num_data_blocks;
			}
			pthread_spin_unlock(&nv_cache->lock);
		}

		/*
		 * Go through the reloc_bitmap, checking for all the bands that had its data moved
		 * onto current band and update their counters to allow them to be used for writing
		 * (once they're closed and empty).
		 */
		for (id = 0; id < ftl_get_num_bands(dev); ++id) {
			if (spdk_bit_array_get(band->reloc_bitmap, id)) {
				assert(dev->bands[id].num_reloc_bands > 0);
				dev->bands[id].num_reloc_bands--;

				spdk_bit_array_clear(band->reloc_bitmap, id);
			}
		}

		ftl_remove_wptr(wptr);
	}
}

static int
ftl_read_next_physical_addr(struct ftl_io *io, struct ftl_addr *addr)
{
	struct spdk_ftl_dev *dev = io->dev;
	size_t num_blocks, max_blocks;

	assert(ftl_io_mode_physical(io));
	assert(io->iov_pos < io->iov_cnt);

	if (io->pos == 0) {
		*addr = io->addr;
	} else {
		*addr = ftl_band_next_xfer_addr(io->band, io->addr, io->pos);
	}

	assert(!ftl_addr_invalid(*addr));

	/* Metadata has to be read in the way it's written (jumping across */
	/* the zones in xfer_size increments) */
	if (io->flags & FTL_IO_MD) {
		max_blocks = dev->xfer_size - (addr->offset % dev->xfer_size);
		num_blocks = spdk_min(ftl_io_iovec_len_left(io), max_blocks);
		assert(addr->offset / dev->xfer_size ==
		       (addr->offset + num_blocks - 1) / dev->xfer_size);
	} else {
		num_blocks = ftl_io_iovec_len_left(io);
	}

	return num_blocks;
}

static int
ftl_wptr_close_band(struct ftl_wptr *wptr)
{
	struct ftl_band *band = wptr->band;

	ftl_band_set_state(band, FTL_BAND_STATE_CLOSING);

	return ftl_band_write_tail_md(band, ftl_md_write_cb);
}

static int
ftl_wptr_open_band(struct ftl_wptr *wptr)
{
	struct ftl_band *band = wptr->band;

	assert(ftl_band_zone_is_first(band, wptr->zone));
	assert(band->lba_map.num_vld == 0);

	ftl_band_clear_lba_map(band);

	assert(band->state == FTL_BAND_STATE_PREP);
	ftl_band_set_state(band, FTL_BAND_STATE_OPENING);

	return ftl_band_write_head_md(band, ftl_md_write_cb);
}

static int
ftl_submit_erase(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_band *band = io->band;
	struct ftl_addr addr = io->addr;
	struct ftl_io_channel *ioch;
	struct ftl_zone *zone;
	int rc = 0;
	size_t i;

	ioch = spdk_io_channel_get_ctx(ftl_get_io_channel(dev));

	for (i = 0; i < io->num_blocks; ++i) {
		if (i != 0) {
			zone = ftl_band_next_zone(band, ftl_band_zone_from_addr(band, addr));
			assert(zone->info.state == SPDK_BDEV_ZONE_STATE_CLOSED);
			addr.offset = zone->info.zone_id;
		}

		assert(ftl_addr_get_zone_offset(dev, addr) == 0);

		ftl_trace_submission(dev, io, addr, 1);
		rc = spdk_bdev_zone_management(dev->base_bdev_desc, ioch->base_ioch, addr.offset,
					       SPDK_BDEV_ZONE_RESET, ftl_io_cmpl_cb, io);
		if (spdk_unlikely(rc)) {
			ftl_io_fail(io, rc);
			SPDK_ERRLOG("Vector reset failed with status: %d\n", rc);
			break;
		}

		ftl_io_inc_req(io);
		ftl_io_advance(io, 1);
	}

	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}

	return rc;
}

static bool
ftl_check_core_thread(const struct spdk_ftl_dev *dev)
{
	return dev->core_thread.thread == spdk_get_thread();
}

static bool
ftl_check_read_thread(const struct spdk_ftl_dev *dev)
{
	return dev->read_thread.thread == spdk_get_thread();
}

struct spdk_io_channel *
ftl_get_io_channel(const struct spdk_ftl_dev *dev)
{
	if (ftl_check_core_thread(dev)) {
		return dev->core_thread.ioch;
	}
	if (ftl_check_read_thread(dev)) {
		return dev->read_thread.ioch;
	}

	assert(0);
	return NULL;
}

static int ftl_io_erase(struct ftl_io *io);

static void
_ftl_io_erase(void *ctx)
{
	ftl_io_erase((struct ftl_io *)ctx);
}

static int
ftl_io_erase(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;

	if (ftl_check_core_thread(dev)) {
		return ftl_submit_erase(io);
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_io_erase, io);
	return 0;
}

static void
ftl_erase_fail(struct ftl_io *io, int status)
{
	struct ftl_zone *zone;
	struct ftl_band *band = io->band;
	char buf[128];

	SPDK_ERRLOG("Erase failed at address: %s, status: %d\n",
		    ftl_addr2str(io->addr, buf, sizeof(buf)), status);

	zone = ftl_band_zone_from_addr(band, io->addr);
	zone->info.state = SPDK_BDEV_ZONE_STATE_OFFLINE;
	ftl_band_remove_zone(band, zone);
	band->tail_md_addr = ftl_band_tail_md_addr(band);
}

static void
ftl_zone_erase_cb(struct ftl_io *io, void *ctx, int status)
{
	struct ftl_zone *zone;

	zone = ftl_band_zone_from_addr(io->band, io->addr);
	zone->busy = false;

	if (spdk_unlikely(status)) {
		ftl_erase_fail(io, status);
		return;
	}

	zone->info.state = SPDK_BDEV_ZONE_STATE_EMPTY;
	zone->info.write_pointer = zone->info.zone_id;
}

static int
ftl_band_erase(struct ftl_band *band)
{
	struct ftl_zone *zone;
	struct ftl_io *io;
	int rc = 0;

	assert(band->state == FTL_BAND_STATE_CLOSED ||
	       band->state == FTL_BAND_STATE_FREE);

	ftl_band_set_state(band, FTL_BAND_STATE_PREP);

	CIRCLEQ_FOREACH(zone, &band->zones, circleq) {
		if (zone->info.state == SPDK_BDEV_ZONE_STATE_EMPTY) {
			continue;
		}

		io = ftl_io_erase_init(band, 1, ftl_zone_erase_cb);
		if (!io) {
			rc = -ENOMEM;
			break;
		}

		zone->busy = true;
		io->addr.offset = zone->info.zone_id;
		rc = ftl_io_erase(io);
		if (rc) {
			zone->busy = false;
			assert(0);
			/* TODO: change band's state back to close? */
			break;
		}
	}

	return rc;
}

static struct ftl_band *
ftl_next_write_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;

	/* Find a free band that has all of its data moved onto other closed bands */
	LIST_FOREACH(band, &dev->free_bands, list_entry) {
		assert(band->state == FTL_BAND_STATE_FREE);
		if (band->num_reloc_bands == 0 && band->num_reloc_blocks == 0) {
			break;
		}
	}

	if (spdk_unlikely(!band)) {
		return NULL;
	}

	if (ftl_band_erase(band)) {
		/* TODO: handle erase failure */
		return NULL;
	}

	return band;
}

static struct ftl_band *
ftl_next_wptr_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;

	if (!dev->next_band) {
		band = ftl_next_write_band(dev);
	} else {
		assert(dev->next_band->state == FTL_BAND_STATE_PREP);
		band = dev->next_band;
		dev->next_band = NULL;
	}

	return band;
}

static struct ftl_wptr *
ftl_wptr_init(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_wptr *wptr;

	wptr = calloc(1, sizeof(*wptr));
	if (!wptr) {
		return NULL;
	}

	wptr->dev = dev;
	wptr->band = band;
	wptr->zone = CIRCLEQ_FIRST(&band->zones);
	wptr->addr.offset = wptr->zone->info.zone_id;
	TAILQ_INIT(&wptr->pending_queue);

	return wptr;
}

static int
ftl_add_direct_wptr(struct ftl_band *band)
{
	struct spdk_ftl_dev *dev = band->dev;
	struct ftl_wptr *wptr;

	assert(band->state == FTL_BAND_STATE_OPEN);

	wptr = ftl_wptr_init(band);
	if (!wptr) {
		return -1;
	}

	wptr->direct_mode = true;

	if (ftl_band_alloc_lba_map(band)) {
		ftl_wptr_free(wptr);
		return -1;
	}

	LIST_INSERT_HEAD(&dev->wptr_list, wptr, list_entry);

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "wptr: direct band %u\n", band->id);
	ftl_trace_write_band(dev, band);
	return 0;
}

static void
ftl_close_direct_wptr(struct ftl_band *band)
{
	struct ftl_wptr *wptr = ftl_wptr_from_band(band);

	assert(wptr);
	assert(wptr->direct_mode);
	assert(band->state == FTL_BAND_STATE_CLOSED);

	ftl_band_release_lba_map(band);

	ftl_remove_wptr(wptr);
}

int
ftl_band_set_direct_access(struct ftl_band *band, bool access)
{
	if (access) {
		return ftl_add_direct_wptr(band);
	} else {
		ftl_close_direct_wptr(band);
		return 0;
	}
}

static int
ftl_add_wptr(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	struct ftl_wptr *wptr;

	band = ftl_next_wptr_band(dev);
	if (!band) {
		return -1;
	}

	wptr = ftl_wptr_init(band);
	if (!wptr) {
		return -1;
	}

	if (ftl_band_write_prep(band)) {
		ftl_wptr_free(wptr);
		return -1;
	}

	LIST_INSERT_HEAD(&dev->wptr_list, wptr, list_entry);

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "wptr: band %u\n", band->id);
	ftl_trace_write_band(dev, band);
	return 0;
}

static void
ftl_wptr_advance(struct ftl_wptr *wptr, size_t xfer_size)
{
	struct ftl_band *band = wptr->band;
	struct spdk_ftl_dev *dev = wptr->dev;
	struct spdk_ftl_conf *conf = &dev->conf;
	size_t next_thld;

	if (spdk_unlikely(wptr->direct_mode)) {
		return;
	}

	wptr->offset += xfer_size;
	next_thld = (ftl_band_num_usable_blocks(band) * conf->band_thld) / 100;

	if (ftl_band_full(band, wptr->offset)) {
		ftl_band_set_state(band, FTL_BAND_STATE_FULL);
	}

	wptr->zone->busy = true;
	wptr->addr = ftl_band_next_xfer_addr(band, wptr->addr, xfer_size);
	wptr->zone = ftl_band_next_operational_zone(band, wptr->zone);

	assert(!ftl_addr_invalid(wptr->addr));

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "wptr: pu:%lu band:%lu, offset:%lu\n",
		      ftl_addr_get_punit(dev, wptr->addr),
		      ftl_addr_get_band(dev, wptr->addr),
		      wptr->addr.offset);

	if (wptr->offset >= next_thld && !dev->next_band) {
		dev->next_band = ftl_next_write_band(dev);
	}
}

static size_t
ftl_wptr_user_blocks_left(const struct ftl_wptr *wptr)
{
	return ftl_band_user_blocks_left(wptr->band, wptr->offset);
}

static int
ftl_wptr_ready(struct ftl_wptr *wptr)
{
	struct ftl_band *band = wptr->band;

	/* TODO: add handling of empty bands */

	if (spdk_unlikely(!ftl_zone_is_writable(wptr->zone))) {
		/* Erasing band may fail after it was assigned to wptr. */
		if (spdk_unlikely(wptr->zone->info.state == SPDK_BDEV_ZONE_STATE_OFFLINE)) {
			ftl_wptr_advance(wptr, wptr->dev->xfer_size);
		}
		return 0;
	}

	/* If we're in the process of writing metadata, wait till it is */
	/* completed. */
	/* TODO: we should probably change bands once we're writing tail md */
	if (ftl_band_state_changing(band)) {
		return 0;
	}

	if (band->state == FTL_BAND_STATE_FULL) {
		if (wptr->num_outstanding == 0) {
			if (ftl_wptr_close_band(wptr)) {
				/* TODO: need recovery here */
				assert(false);
			}
		}

		return 0;
	}

	if (band->state != FTL_BAND_STATE_OPEN) {
		if (ftl_wptr_open_band(wptr)) {
			/* TODO: need recovery here */
			assert(false);
		}

		return 0;
	}

	return 1;
}

int
ftl_flush_active_bands(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_wptr *wptr;
	struct ftl_band_flush *flush;

	assert(ftl_get_core_thread(dev) == spdk_get_thread());

	flush = calloc(1, sizeof(*flush));
	if (spdk_unlikely(!flush)) {
		return -ENOMEM;
	}

	LIST_INSERT_HEAD(&dev->band_flush_list, flush, list_entry);

	flush->cb_fn = cb_fn;
	flush->cb_arg = cb_arg;
	flush->dev = dev;

	LIST_FOREACH(wptr, &dev->wptr_list, list_entry) {
		wptr->flush = true;
		flush->num_bands++;
	}

	return 0;
}

static const struct spdk_ftl_limit *
ftl_get_limit(const struct spdk_ftl_dev *dev, int type)
{
	assert(type < SPDK_FTL_LIMIT_MAX);
	return &dev->conf.limits[type];
}

static bool
ftl_cache_lba_valid(struct spdk_ftl_dev *dev, struct ftl_rwb_entry *entry)
{
	struct ftl_addr addr;

	/* If the LBA is invalid don't bother checking the md and l2p */
	if (spdk_unlikely(entry->lba == FTL_LBA_INVALID)) {
		return false;
	}

	addr = ftl_l2p_get(dev, entry->lba);
	if (!(ftl_addr_cached(addr) && addr.cache_offset == entry->pos)) {
		return false;
	}

	return true;
}

static void
ftl_evict_cache_entry(struct spdk_ftl_dev *dev, struct ftl_rwb_entry *entry)
{
	pthread_spin_lock(&entry->lock);

	if (!ftl_rwb_entry_valid(entry)) {
		goto unlock;
	}

	/* If the l2p wasn't updated and still points at the entry, fill it with the */
	/* on-disk address and clear the cache status bit. Otherwise, skip the l2p update */
	/* and just clear the cache status. */
	if (!ftl_cache_lba_valid(dev, entry)) {
		goto clear;
	}

	ftl_l2p_set(dev, entry->lba, entry->addr);
clear:
	ftl_rwb_entry_invalidate(entry);
unlock:
	pthread_spin_unlock(&entry->lock);
}

static struct ftl_rwb_entry *
ftl_acquire_entry(struct spdk_ftl_dev *dev, int flags)
{
	struct ftl_rwb_entry *entry;

	entry = ftl_rwb_acquire(dev->rwb, ftl_rwb_type_from_flags(flags));
	if (!entry) {
		return NULL;
	}

	ftl_evict_cache_entry(dev, entry);

	entry->flags = flags;
	return entry;
}

static void
ftl_rwb_pad(struct spdk_ftl_dev *dev, size_t size)
{
	struct ftl_rwb_entry *entry;
	int flags = FTL_IO_PAD | FTL_IO_INTERNAL;

	for (size_t i = 0; i < size; ++i) {
		entry = ftl_acquire_entry(dev, flags);
		if (!entry) {
			break;
		}

		entry->lba = FTL_LBA_INVALID;
		entry->addr = ftl_to_addr(FTL_ADDR_INVALID);
		memset(entry->data, 0, FTL_BLOCK_SIZE);
		ftl_rwb_push(entry);
	}
}

static void
ftl_remove_free_bands(struct spdk_ftl_dev *dev)
{
	while (!LIST_EMPTY(&dev->free_bands)) {
		LIST_REMOVE(LIST_FIRST(&dev->free_bands), list_entry);
	}

	dev->next_band = NULL;
}

static void
ftl_wptr_pad_band(struct ftl_wptr *wptr)
{
	struct spdk_ftl_dev *dev = wptr->dev;
	size_t size = ftl_rwb_num_pending(dev->rwb);
	size_t blocks_left, rwb_size, pad_size;

	blocks_left = ftl_wptr_user_blocks_left(wptr);
	assert(size <= blocks_left);
	assert(blocks_left % dev->xfer_size == 0);
	rwb_size = ftl_rwb_size(dev->rwb) - size;
	pad_size = spdk_min(blocks_left - size, rwb_size);

	/* Pad write buffer until band is full */
	ftl_rwb_pad(dev, pad_size);
}

static void
ftl_wptr_process_shutdown(struct ftl_wptr *wptr)
{
	struct spdk_ftl_dev *dev = wptr->dev;
	size_t size = ftl_rwb_num_pending(dev->rwb);
	size_t num_active = dev->xfer_size * ftl_rwb_get_active_batches(dev->rwb);

	num_active = num_active ? num_active : dev->xfer_size;
	if (size >= num_active) {
		return;
	}

	/* If we reach this point we need to remove free bands */
	/* and pad current wptr band to the end */
	if (ftl_rwb_get_active_batches(dev->rwb) <= 1) {
		ftl_remove_free_bands(dev);
	}

	ftl_wptr_pad_band(wptr);
}

static int
ftl_shutdown_complete(struct spdk_ftl_dev *dev)
{
	return !__atomic_load_n(&dev->num_inflight, __ATOMIC_SEQ_CST) &&
	       LIST_EMPTY(&dev->wptr_list) && TAILQ_EMPTY(&dev->retry_queue);
}

void
ftl_apply_limits(struct spdk_ftl_dev *dev)
{
	const struct spdk_ftl_limit *limit;
	struct ftl_stats *stats = &dev->stats;
	size_t rwb_limit[FTL_RWB_TYPE_MAX];
	int i;

	ftl_rwb_get_limits(dev->rwb, rwb_limit);

	/* Clear existing limit */
	dev->limit = SPDK_FTL_LIMIT_MAX;

	for (i = SPDK_FTL_LIMIT_CRIT; i < SPDK_FTL_LIMIT_MAX; ++i) {
		limit = ftl_get_limit(dev, i);

		if (dev->num_free <= limit->thld) {
			rwb_limit[FTL_RWB_TYPE_USER] =
				(limit->limit * ftl_rwb_entry_cnt(dev->rwb)) / 100;
			stats->limits[i]++;
			dev->limit = i;
			goto apply;
		}
	}

	/* Clear the limits, since we don't need to apply them anymore */
	rwb_limit[FTL_RWB_TYPE_USER] = ftl_rwb_entry_cnt(dev->rwb);
apply:
	ftl_trace_limits(dev, rwb_limit, dev->num_free);
	ftl_rwb_set_limits(dev->rwb, rwb_limit);
}

static int
ftl_invalidate_addr_unlocked(struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	struct ftl_band *band = ftl_band_from_addr(dev, addr);
	struct ftl_lba_map *lba_map = &band->lba_map;
	uint64_t offset;

	offset = ftl_band_block_offset_from_addr(band, addr);

	/* The bit might be already cleared if two writes are scheduled to the */
	/* same LBA at the same time */
	if (spdk_bit_array_get(lba_map->vld, offset)) {
		assert(lba_map->num_vld > 0);
		spdk_bit_array_clear(lba_map->vld, offset);
		lba_map->num_vld--;
		return 1;
	}

	return 0;
}

int
ftl_invalidate_addr(struct spdk_ftl_dev *dev, struct ftl_addr addr)
{
	struct ftl_band *band;
	int rc;

	assert(!ftl_addr_cached(addr));
	band = ftl_band_from_addr(dev, addr);

	pthread_spin_lock(&band->lba_map.lock);
	rc = ftl_invalidate_addr_unlocked(dev, addr);
	pthread_spin_unlock(&band->lba_map.lock);

	return rc;
}

static int
ftl_read_retry(int rc)
{
	return rc == -EAGAIN;
}

static int
ftl_read_canceled(int rc)
{
	return rc == -EFAULT || rc == 0;
}

static void
ftl_add_to_retry_queue(struct ftl_io *io)
{
	if (!(io->flags & FTL_IO_RETRY)) {
		io->flags |= FTL_IO_RETRY;
		TAILQ_INSERT_TAIL(&io->dev->retry_queue, io, retry_entry);
	}
}

static int
ftl_cache_read(struct ftl_io *io, uint64_t lba,
	       struct ftl_addr addr, void *buf)
{
	struct ftl_rwb *rwb = io->dev->rwb;
	struct ftl_rwb_entry *entry;
	struct ftl_addr naddr;
	int rc = 0;

	entry = ftl_rwb_entry_from_offset(rwb, addr.cache_offset);
	pthread_spin_lock(&entry->lock);

	naddr = ftl_l2p_get(io->dev, lba);
	if (addr.offset != naddr.offset) {
		rc = -1;
		goto out;
	}

	memcpy(buf, entry->data, FTL_BLOCK_SIZE);
out:
	pthread_spin_unlock(&entry->lock);
	return rc;
}

static int
ftl_read_next_logical_addr(struct ftl_io *io, struct ftl_addr *addr)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_addr next_addr;
	size_t i;

	*addr = ftl_l2p_get(dev, ftl_io_current_lba(io));

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "Read addr:%lx, lba:%lu\n",
		      addr->offset, ftl_io_current_lba(io));

	/* If the address is invalid, skip it (the buffer should already be zero'ed) */
	if (ftl_addr_invalid(*addr)) {
		return -EFAULT;
	}

	if (ftl_addr_cached(*addr)) {
		if (!ftl_cache_read(io, ftl_io_current_lba(io), *addr, ftl_io_iovec_addr(io))) {
			return 0;
		}

		/* If the state changed, we have to re-read the l2p */
		return -EAGAIN;
	}

	for (i = 1; i < ftl_io_iovec_len_left(io); ++i) {
		next_addr = ftl_l2p_get(dev, ftl_io_get_lba(io, io->pos + i));

		if (ftl_addr_invalid(next_addr) || ftl_addr_cached(next_addr)) {
			break;
		}

		if (addr->offset + i != next_addr.offset) {
			break;
		}
	}

	return i;
}

static int
ftl_submit_read(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_io_channel *ioch;
	struct ftl_addr addr;
	int rc = 0, num_blocks;

	ioch = spdk_io_channel_get_ctx(io->ioch);

	assert(LIST_EMPTY(&io->children));

	while (io->pos < io->num_blocks) {
		if (ftl_io_mode_physical(io)) {
			num_blocks = rc = ftl_read_next_physical_addr(io, &addr);
		} else {
			num_blocks = rc = ftl_read_next_logical_addr(io, &addr);
		}

		/* We might need to retry the read from scratch (e.g. */
		/* because write was under way and completed before */
		/* we could read it from rwb */
		if (ftl_read_retry(rc)) {
			continue;
		}

		/* We don't have to schedule the read, as it was read from cache */
		if (ftl_read_canceled(rc)) {
			ftl_io_advance(io, 1);
			ftl_trace_completion(io->dev, io, rc ? FTL_TRACE_COMPLETION_INVALID :
					     FTL_TRACE_COMPLETION_CACHE);
			rc = 0;
			continue;
		}

		assert(num_blocks > 0);

		ftl_trace_submission(dev, io, addr, num_blocks);
		rc = spdk_bdev_read_blocks(dev->base_bdev_desc, ioch->base_ioch,
					   ftl_io_iovec_addr(io),
					   addr.offset,
					   num_blocks, ftl_io_cmpl_cb, io);
		if (spdk_unlikely(rc)) {
			if (rc == -ENOMEM) {
				ftl_add_to_retry_queue(io);
			} else {
				ftl_io_fail(io, rc);
			}
			break;
		}

		ftl_io_inc_req(io);
		ftl_io_advance(io, num_blocks);
	}

	/* If we didn't have to read anything from the device, */
	/* complete the request right away */
	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}

	return rc;
}

static void
ftl_complete_flush(struct ftl_flush *flush)
{
	assert(flush->num_req == 0);
	LIST_REMOVE(flush, list_entry);

	flush->cb.fn(flush->cb.ctx, 0);

	spdk_bit_array_free(&flush->bmap);
	free(flush);
}

static void
ftl_process_flush(struct spdk_ftl_dev *dev, struct ftl_rwb_batch *batch)
{
	struct ftl_flush *flush, *tflush;
	size_t offset;

	LIST_FOREACH_SAFE(flush, &dev->flush_list, list_entry, tflush) {
		offset = ftl_rwb_batch_get_offset(batch);

		if (spdk_bit_array_get(flush->bmap, offset)) {
			spdk_bit_array_clear(flush->bmap, offset);
			if (!(--flush->num_req)) {
				ftl_complete_flush(flush);
			}
		}
	}
}

static void
ftl_nv_cache_wrap_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_nv_cache *nv_cache = cb_arg;

	if (!success) {
		SPDK_ERRLOG("Unable to write non-volatile cache metadata header\n");
		/* TODO: go into read-only mode */
		assert(0);
	}

	pthread_spin_lock(&nv_cache->lock);
	nv_cache->ready = true;
	pthread_spin_unlock(&nv_cache->lock);

	spdk_bdev_free_io(bdev_io);
}

static void
ftl_nv_cache_wrap(void *ctx)
{
	struct ftl_nv_cache *nv_cache = ctx;
	int rc;

	rc = ftl_nv_cache_write_header(nv_cache, false, ftl_nv_cache_wrap_cb, nv_cache);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Unable to write non-volatile cache metadata header: %s\n",
			    spdk_strerror(-rc));
		/* TODO: go into read-only mode */
		assert(0);
	}
}

static uint64_t
ftl_reserve_nv_cache(struct ftl_nv_cache *nv_cache, size_t *num_blocks, unsigned int *phase)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	uint64_t num_available, cache_size, cache_addr = FTL_LBA_INVALID;

	cache_size = spdk_bdev_get_num_blocks(bdev);

	pthread_spin_lock(&nv_cache->lock);
	if (spdk_unlikely(nv_cache->num_available == 0 || !nv_cache->ready)) {
		goto out;
	}

	num_available = spdk_min(nv_cache->num_available, *num_blocks);
	num_available = spdk_min(num_available, dev->conf.nv_cache.max_request_cnt);

	if (spdk_unlikely(nv_cache->current_addr + num_available > cache_size)) {
		*num_blocks = cache_size - nv_cache->current_addr;
	} else {
		*num_blocks = num_available;
	}

	cache_addr = nv_cache->current_addr;
	nv_cache->current_addr += *num_blocks;
	nv_cache->num_available -= *num_blocks;
	*phase = nv_cache->phase;

	if (nv_cache->current_addr == spdk_bdev_get_num_blocks(bdev)) {
		nv_cache->current_addr = FTL_NV_CACHE_DATA_OFFSET;
		nv_cache->phase = ftl_nv_cache_next_phase(nv_cache->phase);
		nv_cache->ready = false;
		spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_nv_cache_wrap, nv_cache);
	}
out:
	pthread_spin_unlock(&nv_cache->lock);
	return cache_addr;
}

static struct ftl_io *
ftl_alloc_io_nv_cache(struct ftl_io *parent, size_t num_blocks)
{
	struct ftl_io_init_opts opts = {
		.dev		= parent->dev,
		.parent		= parent,
		.data		= ftl_io_iovec_addr(parent),
		.num_blocks	= num_blocks,
		.flags		= parent->flags | FTL_IO_CACHE,
	};

	return ftl_io_init_internal(&opts);
}

static void
ftl_nv_cache_submit_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;

	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Non-volatile cache write failed at %"PRIx64"\n", io->addr.offset);
		io->status = -EIO;
	}

	ftl_io_dec_req(io);
	if (ftl_io_done(io)) {
		spdk_mempool_put(nv_cache->md_pool, io->md);
		ftl_io_complete(io);
	}

	spdk_bdev_free_io(bdev_io);
}

static void
ftl_submit_nv_cache(void *ctx)
{
	struct ftl_io *io = ctx;
	struct spdk_ftl_dev *dev = io->dev;
	struct spdk_thread *thread;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	struct ftl_io_channel *ioch;
	int rc;

	ioch = spdk_io_channel_get_ctx(io->ioch);
	thread = spdk_io_channel_get_thread(io->ioch);

	rc = spdk_bdev_write_blocks_with_md(nv_cache->bdev_desc, ioch->cache_ioch,
					    ftl_io_iovec_addr(io), io->md, io->addr.offset,
					    io->num_blocks, ftl_nv_cache_submit_cb, io);
	if (rc == -ENOMEM) {
		spdk_thread_send_msg(thread, ftl_submit_nv_cache, io);
		return;
	} else if (rc) {
		SPDK_ERRLOG("Write to persistent cache failed: %s (%"PRIu64", %"PRIu64")\n",
			    spdk_strerror(-rc), io->addr.offset, io->num_blocks);
		spdk_mempool_put(nv_cache->md_pool, io->md);
		io->status = -EIO;
		ftl_io_complete(io);
		return;
	}

	ftl_io_advance(io, io->num_blocks);
	ftl_io_inc_req(io);
}

static void
ftl_nv_cache_fill_md(struct ftl_io *io, unsigned int phase)
{
	struct spdk_bdev *bdev;
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;
	uint64_t block_off, lba;
	void *md_buf = io->md;

	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);

	for (block_off = 0; block_off < io->num_blocks; ++block_off) {
		lba = ftl_nv_cache_pack_lba(ftl_io_get_lba(io, block_off), phase);
		memcpy(md_buf, &lba, sizeof(lba));
		md_buf += spdk_bdev_get_md_size(bdev);
	}
}

static void
_ftl_write_nv_cache(void *ctx)
{
	struct ftl_io *child, *io = ctx;
	struct spdk_ftl_dev *dev = io->dev;
	struct spdk_thread *thread;
	unsigned int phase;
	uint64_t num_blocks;

	thread = spdk_io_channel_get_thread(io->ioch);

	while (io->pos < io->num_blocks) {
		num_blocks = ftl_io_iovec_len_left(io);

		child = ftl_alloc_io_nv_cache(io, num_blocks);
		if (spdk_unlikely(!child)) {
			spdk_thread_send_msg(thread, _ftl_write_nv_cache, io);
			return;
		}

		child->md = spdk_mempool_get(dev->nv_cache.md_pool);
		if (spdk_unlikely(!child->md)) {
			ftl_io_free(child);
			spdk_thread_send_msg(thread, _ftl_write_nv_cache, io);
			break;
		}

		/* Reserve area on the write buffer cache */
		child->addr.offset = ftl_reserve_nv_cache(&dev->nv_cache, &num_blocks, &phase);
		if (child->addr.offset == FTL_LBA_INVALID) {
			spdk_mempool_put(dev->nv_cache.md_pool, child->md);
			ftl_io_free(child);
			spdk_thread_send_msg(thread, _ftl_write_nv_cache, io);
			break;
		}

		/* Shrink the IO if there isn't enough room in the cache to fill the whole iovec */
		if (spdk_unlikely(num_blocks != ftl_io_iovec_len_left(io))) {
			ftl_io_shrink_iovec(child, num_blocks);
		}

		ftl_nv_cache_fill_md(child, phase);
		ftl_submit_nv_cache(child);
	}

	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}
}

static void
ftl_write_nv_cache(struct ftl_io *parent)
{
	ftl_io_reset(parent);
	parent->flags |= FTL_IO_CACHE;
	_ftl_write_nv_cache(parent);
}

int
ftl_nv_cache_write_header(struct ftl_nv_cache *nv_cache, bool shutdown,
			  spdk_bdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_nv_cache_header *hdr = nv_cache->dma_buf;
	struct spdk_bdev *bdev;
	struct ftl_io_channel *ioch;

	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
	ioch = spdk_io_channel_get_ctx(ftl_get_io_channel(dev));

	memset(hdr, 0, spdk_bdev_get_block_size(bdev));

	hdr->phase = (uint8_t)nv_cache->phase;
	hdr->size = spdk_bdev_get_num_blocks(bdev);
	hdr->uuid = dev->uuid;
	hdr->version = FTL_NV_CACHE_HEADER_VERSION;
	hdr->current_addr = shutdown ? nv_cache->current_addr : FTL_LBA_INVALID;
	hdr->checksum = spdk_crc32c_update(hdr, offsetof(struct ftl_nv_cache_header, checksum), 0);

	return spdk_bdev_write_blocks(nv_cache->bdev_desc, ioch->cache_ioch, hdr, 0, 1,
				      cb_fn, cb_arg);
}

int
ftl_nv_cache_scrub(struct ftl_nv_cache *nv_cache, spdk_bdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_ftl_dev *dev = SPDK_CONTAINEROF(nv_cache, struct spdk_ftl_dev, nv_cache);
	struct ftl_io_channel *ioch;
	struct spdk_bdev *bdev;

	ioch = spdk_io_channel_get_ctx(ftl_get_io_channel(dev));
	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);

	return spdk_bdev_write_zeroes_blocks(nv_cache->bdev_desc, ioch->cache_ioch, 1,
					     spdk_bdev_get_num_blocks(bdev) - 1,
					     cb_fn, cb_arg);
}

static void
ftl_write_fail(struct ftl_io *io, int status)
{
	struct ftl_rwb_batch *batch = io->rwb_batch;
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_rwb_entry *entry;
	struct ftl_band *band;
	char buf[128];

	entry = ftl_rwb_batch_first_entry(batch);

	band = ftl_band_from_addr(io->dev, entry->addr);
	SPDK_ERRLOG("Write failed @addr: %s, status: %d\n",
		    ftl_addr2str(entry->addr, buf, sizeof(buf)), status);

	/* Close the band and, halt wptr and defrag */
	ftl_halt_writes(dev, band);

	ftl_rwb_foreach(entry, batch) {
		/* Invalidate meta set by process_writes() */
		ftl_invalidate_addr(dev, entry->addr);
	}

	/* Reset the batch back to the the RWB to resend it later */
	ftl_rwb_batch_revert(batch);
}

static void
ftl_write_cb(struct ftl_io *io, void *arg, int status)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_rwb_batch *batch = io->rwb_batch;
	struct ftl_rwb_entry *entry;
	struct ftl_band *band;
	struct ftl_addr prev_addr, addr = io->addr;

	if (status) {
		ftl_write_fail(io, status);
		return;
	}

	assert(io->num_blocks == dev->xfer_size);
	assert(!(io->flags & FTL_IO_MD));

	ftl_rwb_foreach(entry, batch) {
		band = entry->band;
		if (!(entry->flags & FTL_IO_PAD)) {
			/* Verify that the LBA is set for user blocks */
			assert(entry->lba != FTL_LBA_INVALID);
		}

		if (band != NULL) {
			assert(band->num_reloc_blocks > 0);
			band->num_reloc_blocks--;
		}

		entry->addr = addr;
		if (entry->lba != FTL_LBA_INVALID) {
			pthread_spin_lock(&entry->lock);
			prev_addr = ftl_l2p_get(dev, entry->lba);

			/* If the l2p was updated in the meantime, don't update band's metadata */
			if (ftl_addr_cached(prev_addr) && prev_addr.cache_offset == entry->pos) {
				/* Setting entry's cache bit needs to be done after metadata */
				/* within the band is updated to make sure that writes */
				/* invalidating the entry clear the metadata as well */
				ftl_band_set_addr(io->band, entry->lba, entry->addr);
				ftl_rwb_entry_set_valid(entry);
			}
			pthread_spin_unlock(&entry->lock);
		}

		SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "Write addr:%lu, lba:%lu\n",
			      entry->addr.offset, entry->lba);

		addr = ftl_band_next_addr(io->band, addr, 1);
	}

	ftl_process_flush(dev, batch);
	ftl_rwb_batch_release(batch);
}

static void
ftl_update_rwb_stats(struct spdk_ftl_dev *dev, const struct ftl_rwb_entry *entry)
{
	if (!ftl_rwb_entry_internal(entry)) {
		dev->stats.write_user++;
	}
	dev->stats.write_total++;
}

static void
ftl_update_l2p(struct spdk_ftl_dev *dev, const struct ftl_rwb_entry *entry,
	       struct ftl_addr addr)
{
	struct ftl_addr prev_addr;
	struct ftl_rwb_entry *prev;
	struct ftl_band *band;
	int valid;

	prev_addr = ftl_l2p_get(dev, entry->lba);
	if (ftl_addr_invalid(prev_addr)) {
		ftl_l2p_set(dev, entry->lba, addr);
		return;
	}

	/* If the L2P's physical address is different than what we expected we don't need to */
	/* do anything (someone's already overwritten our data). */
	if (ftl_rwb_entry_weak(entry) && !ftl_addr_cmp(prev_addr, entry->addr)) {
		return;
	}

	if (ftl_addr_cached(prev_addr)) {
		assert(!ftl_rwb_entry_weak(entry));
		prev = ftl_rwb_entry_from_offset(dev->rwb, prev_addr.cache_offset);
		pthread_spin_lock(&prev->lock);

		/* Re-read the L2P under the lock to protect against updates */
		/* to this LBA from other threads */
		prev_addr = ftl_l2p_get(dev, entry->lba);

		/* If the entry is no longer in cache, another write has been */
		/* scheduled in the meantime, so we have to invalidate its LBA */
		if (!ftl_addr_cached(prev_addr)) {
			ftl_invalidate_addr(dev, prev_addr);
		}

		/* If previous entry is part of cache, remove and invalidate it */
		if (ftl_rwb_entry_valid(prev)) {
			ftl_invalidate_addr(dev, prev->addr);
			ftl_rwb_entry_invalidate(prev);
		}

		ftl_l2p_set(dev, entry->lba, addr);
		pthread_spin_unlock(&prev->lock);
		return;
	}

	/* Lock the band containing previous physical address. This assures atomic changes to */
	/* the L2P as wall as metadata. The valid bits in metadata are used to */
	/* check weak writes validity. */
	band = ftl_band_from_addr(dev, prev_addr);
	pthread_spin_lock(&band->lba_map.lock);

	valid = ftl_invalidate_addr_unlocked(dev, prev_addr);

	/* If the address has been invalidated already, we don't want to update */
	/* the L2P for weak writes, as it means the write is no longer valid. */
	if (!ftl_rwb_entry_weak(entry) || valid) {
		ftl_l2p_set(dev, entry->lba, addr);
	}

	pthread_spin_unlock(&band->lba_map.lock);
}

static struct ftl_io *
ftl_io_init_child_write(struct ftl_io *parent, struct ftl_addr addr,
			void *data, void *md, ftl_io_fn cb)
{
	struct ftl_io *io;
	struct spdk_ftl_dev *dev = parent->dev;
	struct ftl_io_init_opts opts = {
		.dev		= dev,
		.io		= NULL,
		.parent		= parent,
		.rwb_batch	= NULL,
		.band		= parent->band,
		.size		= sizeof(struct ftl_io),
		.flags		= 0,
		.type		= parent->type,
		.num_blocks	= dev->xfer_size,
		.cb_fn		= cb,
		.data		= data,
		.md		= md,
	};

	io = ftl_io_init_internal(&opts);
	if (!io) {
		return NULL;
	}

	io->addr = addr;

	return io;
}

static void
ftl_io_child_write_cb(struct ftl_io *io, void *ctx, int status)
{
	struct ftl_zone *zone;
	struct ftl_wptr *wptr;

	zone = ftl_band_zone_from_addr(io->band, io->addr);
	wptr = ftl_wptr_from_band(io->band);

	zone->busy = false;
	zone->info.write_pointer += io->num_blocks;

	/* If some other write on the same band failed the write pointer would already be freed */
	if (spdk_likely(wptr)) {
		wptr->num_outstanding--;
	}
}

static int
ftl_submit_child_write(struct ftl_wptr *wptr, struct ftl_io *io, int num_blocks)
{
	struct spdk_ftl_dev	*dev = io->dev;
	struct ftl_io_channel	*ioch;
	struct ftl_io		*child;
	struct ftl_addr		addr;
	int			rc;

	ioch = spdk_io_channel_get_ctx(io->ioch);

	if (spdk_likely(!wptr->direct_mode)) {
		addr = wptr->addr;
	} else {
		assert(io->flags & FTL_IO_DIRECT_ACCESS);
		assert(ftl_addr_get_band(dev, io->addr) == wptr->band->id);
		addr = io->addr;
	}

	/* Split IO to child requests and release zone immediately after child is completed */
	child = ftl_io_init_child_write(io, addr, ftl_io_iovec_addr(io),
					ftl_io_get_md(io), ftl_io_child_write_cb);
	if (!child) {
		return -EAGAIN;
	}

	wptr->num_outstanding++;

	if (ftl_is_append_supported(dev)) {
		rc = spdk_bdev_zone_append(dev->base_bdev_desc, ioch->base_ioch,
					   ftl_io_iovec_addr(child),
					   ftl_addr_get_zone_slba(dev, addr),
					   num_blocks, ftl_io_cmpl_cb, child);
	} else {
		rc = spdk_bdev_write_blocks(dev->base_bdev_desc, ioch->base_ioch,
					    ftl_io_iovec_addr(child),
					    addr.offset,
					    num_blocks, ftl_io_cmpl_cb, child);
	}

	if (rc) {
		wptr->num_outstanding--;
		ftl_io_fail(child, rc);
		ftl_io_complete(child);
		SPDK_ERRLOG("spdk_bdev_write_blocks_with_md failed with status:%d, addr:%lu\n",
			    rc, addr.offset);
		return -EIO;
	}

	ftl_io_inc_req(child);
	ftl_io_advance(child, num_blocks);

	return 0;
}

static int
ftl_submit_write(struct ftl_wptr *wptr, struct ftl_io *io)
{
	struct spdk_ftl_dev	*dev = io->dev;
	int			rc = 0;

	assert(io->num_blocks % dev->xfer_size == 0);
	/* Only one child write make sense in case of user write */
	assert((io->flags & FTL_IO_MD) || io->iov_cnt == 1);

	while (io->iov_pos < io->iov_cnt) {
		/* There are no guarantees of the order of completion of NVMe IO submission queue */
		/* so wait until zone is not busy before submitting another write */
		if (!ftl_is_append_supported(dev) && wptr->zone->busy) {
			TAILQ_INSERT_TAIL(&wptr->pending_queue, io, retry_entry);
			rc = -EAGAIN;
			break;
		}

		rc = ftl_submit_child_write(wptr, io, dev->xfer_size);
		if (spdk_unlikely(rc)) {
			if (rc == -EAGAIN) {
				TAILQ_INSERT_TAIL(&wptr->pending_queue, io, retry_entry);
			} else {
				ftl_io_fail(io, rc);
			}
			break;
		}

		ftl_trace_submission(dev, io, wptr->addr, dev->xfer_size);
		ftl_wptr_advance(wptr, dev->xfer_size);
	}

	if (ftl_io_done(io)) {
		/* Parent IO will complete after all children are completed */
		ftl_io_complete(io);
	}

	return rc;
}

static void
ftl_flush_pad_batch(struct spdk_ftl_dev *dev)
{
	struct ftl_rwb *rwb = dev->rwb;
	size_t size, num_entries;

	size = ftl_rwb_num_acquired(rwb, FTL_RWB_TYPE_INTERNAL) +
	       ftl_rwb_num_acquired(rwb, FTL_RWB_TYPE_USER);

	/* There must be something in the RWB, otherwise the flush */
	/* wouldn't be waiting for anything */
	assert(size > 0);

	/* Only add padding when there's less than xfer size */
	/* entries in the buffer. Otherwise we just have to wait */
	/* for the entries to become ready. */
	num_entries = ftl_rwb_get_active_batches(dev->rwb) * dev->xfer_size;
	if (size < num_entries) {
		ftl_rwb_pad(dev, num_entries - (size % num_entries));
	}
}

static int
ftl_wptr_process_writes(struct ftl_wptr *wptr)
{
	struct spdk_ftl_dev	*dev = wptr->dev;
	struct ftl_rwb_batch	*batch;
	struct ftl_rwb_entry	*entry;
	struct ftl_io		*io;

	if (spdk_unlikely(!TAILQ_EMPTY(&wptr->pending_queue))) {
		io = TAILQ_FIRST(&wptr->pending_queue);
		TAILQ_REMOVE(&wptr->pending_queue, io, retry_entry);

		if (ftl_submit_write(wptr, io) == -EAGAIN) {
			return 0;
		}
	}

	/* Make sure the band is prepared for writing */
	if (!ftl_wptr_ready(wptr)) {
		return 0;
	}

	if (dev->halt) {
		ftl_wptr_process_shutdown(wptr);
	}

	if (spdk_unlikely(wptr->flush)) {
		ftl_wptr_pad_band(wptr);
	}

	batch = ftl_rwb_pop(dev->rwb);
	if (!batch) {
		/* If there are queued flush requests we need to pad the RWB to */
		/* force out remaining entries */
		if (!LIST_EMPTY(&dev->flush_list)) {
			ftl_flush_pad_batch(dev);
		}

		return 0;
	}

	io = ftl_io_rwb_init(dev, wptr->addr, wptr->band, batch, ftl_write_cb);
	if (!io) {
		goto error;
	}

	ftl_rwb_foreach(entry, batch) {
		/* Update band's relocation stats if the IO comes from reloc */
		if (entry->flags & FTL_IO_WEAK) {
			if (!spdk_bit_array_get(wptr->band->reloc_bitmap, entry->band->id)) {
				spdk_bit_array_set(wptr->band->reloc_bitmap, entry->band->id);
				entry->band->num_reloc_bands++;
			}
		}

		ftl_trace_rwb_pop(dev, entry);
		ftl_update_rwb_stats(dev, entry);
	}

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "Write addr:%lx\n", wptr->addr.offset);

	if (ftl_submit_write(wptr, io)) {
		/* TODO: we need some recovery here */
		assert(0 && "Write submit failed");
		if (ftl_io_done(io)) {
			ftl_io_free(io);
		}
	}

	return dev->xfer_size;
error:
	ftl_rwb_batch_revert(batch);
	return 0;
}

static int
ftl_process_writes(struct spdk_ftl_dev *dev)
{
	struct ftl_wptr *wptr, *twptr;
	size_t num_active = 0;
	enum ftl_band_state state;

	LIST_FOREACH_SAFE(wptr, &dev->wptr_list, list_entry, twptr) {
		ftl_wptr_process_writes(wptr);
		state = wptr->band->state;

		if (state != FTL_BAND_STATE_FULL &&
		    state != FTL_BAND_STATE_CLOSING &&
		    state != FTL_BAND_STATE_CLOSED) {
			num_active++;
		}
	}

	if (num_active < 1) {
		ftl_add_wptr(dev);
	}

	return 0;
}

static void
ftl_rwb_entry_fill(struct ftl_rwb_entry *entry, struct ftl_io *io)
{
	memcpy(entry->data, ftl_io_iovec_addr(io), FTL_BLOCK_SIZE);

	if (ftl_rwb_entry_weak(entry)) {
		entry->band = ftl_band_from_addr(io->dev, io->addr);
		entry->addr = ftl_band_next_addr(entry->band, io->addr, io->pos);
		entry->band->num_reloc_blocks++;
	}

	entry->trace = io->trace;
	entry->lba = ftl_io_current_lba(io);

	if (entry->md) {
		memcpy(entry->md, &entry->lba, sizeof(entry->lba));
	}
}

static int
ftl_rwb_fill(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_rwb_entry *entry;
	struct ftl_addr addr = { .cached = 1 };
	int flags = ftl_rwb_flags_from_io(io);

	while (io->pos < io->num_blocks) {
		if (ftl_io_current_lba(io) == FTL_LBA_INVALID) {
			ftl_io_advance(io, 1);
			continue;
		}

		entry = ftl_acquire_entry(dev, flags);
		if (!entry) {
			return -EAGAIN;
		}

		ftl_rwb_entry_fill(entry, io);

		addr.cache_offset = entry->pos;

		ftl_trace_rwb_fill(dev, io);
		ftl_update_l2p(dev, entry, addr);
		ftl_io_advance(io, 1);

		/* Needs to be done after L2P is updated to avoid race with */
		/* write completion callback when it's processed faster than */
		/* L2P is set in update_l2p(). */
		ftl_rwb_push(entry);
	}

	if (ftl_io_done(io)) {
		if (ftl_dev_has_nv_cache(dev) && !(io->flags & FTL_IO_BYPASS_CACHE)) {
			ftl_write_nv_cache(io);
		} else {
			ftl_io_complete(io);
		}
	}

	return 0;
}

static bool
ftl_dev_needs_defrag(struct spdk_ftl_dev *dev)
{
	const struct spdk_ftl_limit *limit = ftl_get_limit(dev, SPDK_FTL_LIMIT_START);

	if (ftl_reloc_is_halted(dev->reloc)) {
		return false;
	}

	if (ftl_reloc_is_defrag_active(dev->reloc)) {
		return false;
	}

	if (dev->num_free <= limit->thld) {
		return true;
	}

	return false;
}

static double
ftl_band_calc_merit(struct ftl_band *band, size_t *threshold_valid)
{
	size_t usable, valid, invalid;
	double vld_ratio;

	/* If the band doesn't have any usable blocks it's of no use */
	usable = ftl_band_num_usable_blocks(band);
	if (usable == 0) {
		return 0.0;
	}

	valid =  threshold_valid ? (usable - *threshold_valid) : band->lba_map.num_vld;
	invalid = usable - valid;

	/* Add one to avoid division by 0 */
	vld_ratio = (double)invalid / (double)(valid + 1);
	return vld_ratio * ftl_band_age(band);
}

static bool
ftl_band_needs_defrag(struct ftl_band *band, struct spdk_ftl_dev *dev)
{
	struct spdk_ftl_conf *conf = &dev->conf;
	size_t thld_vld;

	/* If we're in dire need of free bands, every band is worth defragging */
	if (ftl_current_limit(dev) == SPDK_FTL_LIMIT_CRIT) {
		return true;
	}

	thld_vld = (ftl_band_num_usable_blocks(band) * conf->invalid_thld) / 100;

	return band->merit > ftl_band_calc_merit(band, &thld_vld);
}

static struct ftl_band *
ftl_select_defrag_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band, *mband = NULL;
	double merit = 0;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		assert(band->state == FTL_BAND_STATE_CLOSED);
		band->merit = ftl_band_calc_merit(band, NULL);
		if (band->merit > merit) {
			merit = band->merit;
			mband = band;
		}
	}

	if (mband && !ftl_band_needs_defrag(mband, dev)) {
		mband = NULL;
	}

	return mband;
}

static void
ftl_process_relocs(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;

	if (ftl_dev_needs_defrag(dev)) {
		band = ftl_select_defrag_band(dev);
		if (band) {
			ftl_reloc_add(dev->reloc, band, 0, ftl_get_num_blocks_in_band(dev), 0, true);
			ftl_trace_defrag_band(dev, band);
		}
	}

	ftl_reloc(dev->reloc);
}

int
ftl_current_limit(const struct spdk_ftl_dev *dev)
{
	return dev->limit;
}

void
spdk_ftl_dev_get_attrs(const struct spdk_ftl_dev *dev, struct spdk_ftl_attrs *attrs)
{
	attrs->uuid = dev->uuid;
	attrs->num_blocks = dev->num_lbas;
	attrs->block_size = FTL_BLOCK_SIZE;
	attrs->cache_bdev_desc = dev->nv_cache.bdev_desc;
	attrs->num_zones = ftl_get_num_zones(dev);
	attrs->zone_size = ftl_get_num_blocks_in_zone(dev);
	attrs->conf = dev->conf;
}

static void
_ftl_io_write(void *ctx)
{
	ftl_io_write((struct ftl_io *)ctx);
}

static int
ftl_rwb_fill_leaf(struct ftl_io *io)
{
	int rc;

	rc = ftl_rwb_fill(io);
	if (rc == -EAGAIN) {
		spdk_thread_send_msg(spdk_io_channel_get_thread(io->ioch),
				     _ftl_io_write, io);
		return 0;
	}

	return rc;
}

static int
ftl_submit_write_leaf(struct ftl_io *io)
{
	int rc;

	rc = ftl_submit_write(ftl_wptr_from_band(io->band), io);
	if (rc == -EAGAIN) {
		/* EAGAIN means that the request was put on the pending queue */
		return 0;
	}

	return rc;
}

void
ftl_io_write(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;

	/* For normal IOs we just need to copy the data onto the rwb */
	if (!(io->flags & FTL_IO_MD)) {
		ftl_io_call_foreach_child(io, ftl_rwb_fill_leaf);
	} else {
		/* Metadata has its own buffer, so it doesn't have to be copied, so just */
		/* send it the the core thread and schedule the write immediately */
		if (ftl_check_core_thread(dev)) {
			ftl_io_call_foreach_child(io, ftl_submit_write_leaf);
		} else {
			spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_io_write, io);
		}
	}
}

int
spdk_ftl_write(struct spdk_ftl_dev *dev, struct spdk_io_channel *ch, uint64_t lba, size_t lba_cnt,
	       struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_io *io;

	if (iov_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_blocks(iov, iov_cnt)) {
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	io = ftl_io_user_init(ch, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_WRITE);
	if (!io) {
		return -ENOMEM;
	}

	ftl_io_write(io);

	return 0;
}

static int
ftl_io_read_leaf(struct ftl_io *io)
{
	int rc;

	rc = ftl_submit_read(io);
	if (rc == -ENOMEM) {
		/* ENOMEM means that the request was put on a pending queue */
		return 0;
	}

	return rc;
}

static void
_ftl_io_read(void *arg)
{
	ftl_io_read((struct ftl_io *)arg);
}

void
ftl_io_read(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;

	if (ftl_check_read_thread(dev)) {
		ftl_io_call_foreach_child(io, ftl_io_read_leaf);
	} else {
		spdk_thread_send_msg(ftl_get_read_thread(dev), _ftl_io_read, io);
	}
}

int
spdk_ftl_read(struct spdk_ftl_dev *dev, struct spdk_io_channel *ch, uint64_t lba, size_t lba_cnt,
	      struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_io *io;

	if (iov_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_blocks(iov, iov_cnt)) {
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	io = ftl_io_user_init(ch, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_READ);
	if (!io) {
		return -ENOMEM;
	}

	ftl_io_read(io);
	return 0;
}

static struct ftl_flush *
ftl_flush_init(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_flush *flush;
	struct ftl_rwb *rwb = dev->rwb;

	flush = calloc(1, sizeof(*flush));
	if (!flush) {
		return NULL;
	}

	flush->bmap = spdk_bit_array_create(ftl_rwb_num_batches(rwb));
	if (!flush->bmap) {
		goto error;
	}

	flush->dev = dev;
	flush->cb.fn = cb_fn;
	flush->cb.ctx = cb_arg;

	return flush;
error:
	free(flush);
	return NULL;
}

static void
_ftl_flush(void *ctx)
{
	struct ftl_flush *flush = ctx;
	struct spdk_ftl_dev *dev = flush->dev;
	struct ftl_rwb *rwb = dev->rwb;
	struct ftl_rwb_batch *batch;

	/* Attach flush object to all non-empty batches */
	ftl_rwb_foreach_batch(batch, rwb) {
		if (!ftl_rwb_batch_empty(batch)) {
			spdk_bit_array_set(flush->bmap, ftl_rwb_batch_get_offset(batch));
			flush->num_req++;
		}
	}

	LIST_INSERT_HEAD(&dev->flush_list, flush, list_entry);

	/* If the RWB was already empty, the flush can be completed right away */
	if (!flush->num_req) {
		ftl_complete_flush(flush);
	}
}

int
ftl_flush_rwb(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_flush *flush;

	flush = ftl_flush_init(dev, cb_fn, cb_arg);
	if (!flush) {
		return -ENOMEM;
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_flush, flush);
	return 0;
}

int
spdk_ftl_flush(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg)
{
	if (!dev->initialized) {
		return -EBUSY;
	}

	return ftl_flush_rwb(dev, cb_fn, cb_arg);
}

bool
ftl_addr_is_written(struct ftl_band *band, struct ftl_addr addr)
{
	struct ftl_zone *zone = ftl_band_zone_from_addr(band, addr);

	return addr.offset < zone->info.write_pointer;
}

static void
ftl_process_retry_queue(struct spdk_ftl_dev *dev)
{
	struct ftl_io *io;
	int rc;

	while (!TAILQ_EMPTY(&dev->retry_queue)) {
		io = TAILQ_FIRST(&dev->retry_queue);

		/* Retry only if IO is still healthy */
		if (spdk_likely(io->status == 0)) {
			rc = ftl_submit_read(io);
			if (rc == -ENOMEM) {
				break;
			}
		}

		io->flags &= ~FTL_IO_RETRY;
		TAILQ_REMOVE(&dev->retry_queue, io, retry_entry);

		if (ftl_io_done(io)) {
			ftl_io_complete(io);
		}
	}
}

int
ftl_task_read(void *ctx)
{
	struct ftl_thread *thread = ctx;
	struct spdk_ftl_dev *dev = thread->dev;

	if (dev->halt) {
		if (ftl_shutdown_complete(dev)) {
			spdk_poller_unregister(&thread->poller);
			return 0;
		}
	}

	if (!TAILQ_EMPTY(&dev->retry_queue)) {
		ftl_process_retry_queue(dev);
		return 1;
	}

	return 0;
}

int
ftl_task_core(void *ctx)
{
	struct ftl_thread *thread = ctx;
	struct spdk_ftl_dev *dev = thread->dev;

	if (dev->halt) {
		if (ftl_shutdown_complete(dev)) {
			spdk_poller_unregister(&thread->poller);
			return 0;
		}
	}

	ftl_process_writes(dev);
	ftl_process_relocs(dev);

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("ftl_core", SPDK_LOG_FTL_CORE)
