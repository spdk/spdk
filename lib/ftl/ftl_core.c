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
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/string.h"
#include "spdk/ftl.h"
#include "spdk/crc32.h"

#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_io.h"
#include "ftl_debug.h"
#include "ftl_internal.h"
#include "mngt/ftl_mngt.h"

struct ftl_wptr {
	/* Owner device */
	struct spdk_ftl_dev		*dev;

	/* Current address */
	ftl_addr			addr;

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
	 * If setup in direct mode, there will be no offset or band state u;pdate after IO.
	 * The zoned bdev address is not assigned by wptr, and is instead taken directly
	 * from the request.
	 */
	bool				direct_mode;

	/* Number of outstanding write requests */
	uint32_t			num_outstanding;

	/* Marks that the band related to this wptr needs to be closed as soon as possible */
	bool				flush;
};

size_t
spdk_ftl_io_size(void)
{
	return sizeof(struct ftl_io);
}

static void
ftl_io_cmpl_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct spdk_ftl_dev *dev = io->dev;

	if (spdk_unlikely(!success)) {
		io->status = -EIO;
	}

	if (io->type == FTL_IO_WRITE && ftl_is_append_supported(dev)) {
		assert(io->parent);
		io->parent->addr = spdk_bdev_io_get_append_location(bdev_io);
	}

	ftl_io_dec_req(io);
	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}

	spdk_bdev_free_io(bdev_io);
}

static void
ftl_submit_erase(struct ftl_io *io);

static void
_ftl_submit_erase(void *_io)
{
	struct ftl_io *io = _io;

	ftl_submit_erase(io);
}

static void
ftl_submit_erase(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_band *band = io->band;
	ftl_addr addr = io->addr;
	struct ftl_zone *zone;
	int rc = 0;
	size_t i;

	for (i = 0; i < io->num_blocks; ++i) {
		if (i != 0) {
			zone = ftl_band_next_zone(band, ftl_band_zone_from_addr(band, addr));
			assert(zone->info.state == SPDK_BDEV_ZONE_STATE_FULL);
			addr = zone->info.zone_id;
		}

		assert(ftl_addr_get_zone_offset(dev, addr) == 0);

		if (i < io->pos) {
			continue;
		}

		rc = spdk_bdev_zone_management(dev->base_bdev_desc, dev->base_ioch, addr,
					       SPDK_BDEV_ZONE_RESET, ftl_io_cmpl_cb, io);
		if (spdk_unlikely(rc)) {
			if (rc == -ENOMEM) {
				struct spdk_bdev *bdev;
				bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
				io->bdev_io_wait.bdev = bdev;
				io->bdev_io_wait.cb_fn = _ftl_submit_erase;
				io->bdev_io_wait.cb_arg = io;
				spdk_bdev_queue_io_wait(bdev, dev->base_ioch, &io->bdev_io_wait);
				return;
			} else {
				ftl_abort();
			}
		}

		ftl_io_inc_req(io);
		ftl_io_advance(io, 1);
	}

	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}
}

struct spdk_io_channel *
ftl_get_io_channel(const struct spdk_ftl_dev *dev)
{
	if (ftl_check_core_thread(dev)) {
		return dev->ioch;
	}

	return NULL;
}

static void
ftl_erase_fail(struct ftl_io *io, int status)
{
	struct ftl_zone *zone;
	struct ftl_band *band = io->band;
	char buf[128];

	FTL_ERRLOG(band->dev, "Erase failed at address: %s, status: %d\n",
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

static void
_ftl_band_erase(void *_band)
{
	struct ftl_band *band = _band;
	struct ftl_zone *zone;
	struct ftl_io *io;

	CIRCLEQ_FOREACH(zone, &band->zones, circleq) {
		if (zone->info.state == SPDK_BDEV_ZONE_STATE_EMPTY) {
			continue;
		}

		io = ftl_io_erase_init(band, 1, ftl_zone_erase_cb);
		if (!io) {
			spdk_thread_send_msg(spdk_get_thread(), _ftl_band_erase, band);
			break;
		}

		zone->busy = true;
		io->addr = zone->info.zone_id;
		ftl_submit_erase(io);
	}
}

static void
ftl_band_erase(struct ftl_band *band)
{
	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);

	ftl_band_set_state(band, FTL_BAND_STATE_PREP);

	/* TODO: move ftl_band_erase to band abstraction */
	if (spdk_bdev_is_zoned(spdk_bdev_desc_get_bdev(band->dev->base_bdev_desc))) {
		_ftl_band_erase(band);
	} else {
		struct ftl_zone *zone = &band->zone_buf[0];

		zone->info.state = SPDK_BDEV_ZONE_STATE_EMPTY;
		zone->info.write_pointer = zone->info.zone_id;
	}
}

static size_t
ftl_get_limit(const struct spdk_ftl_dev *dev, int type)
{
	assert(type < SPDK_FTL_LIMIT_MAX);
	return dev->conf.limits[type];
}

static int
ftl_shutdown_complete(struct spdk_ftl_dev *dev)
{
	size_t i;

	if (dev->num_inflight) {
		return 0;
	}

	if (__atomic_load_n(&dev->num_io_channels, __ATOMIC_SEQ_CST) != 1) {
		return 0;
	}

	if (!ftl_nv_cache_is_halted(&dev->nv_cache)) {
		ftl_nv_cache_halt(&dev->nv_cache);
		return 0;
	}

	if (!ftl_writer_is_halted(&dev->writer_user)) {
		ftl_writer_halt(&dev->writer_user);
		return 0;
	}

	if (!ftl_reloc_is_halted(dev->reloc)) {
		ftl_reloc_halt(dev->reloc);
		return 0;
	}

	if (!ftl_writer_is_halted(&dev->writer_gc)) {
		ftl_writer_halt(&dev->writer_gc);
		return 0;
	}

	if (!ftl_nv_cache_chunks_busy(&dev->nv_cache)) {
		return 0;
	}

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		if (dev->bands[i].queue_depth ||
		    dev->bands[i].md->state == FTL_BAND_STATE_CLOSING) {
			return 0;
		}
	}

	if (!ftl_l2p_is_halted(dev)) {
		ftl_l2p_halt(dev);
		return 0;
	}

	return 1;
}

void
ftl_apply_limits(struct spdk_ftl_dev *dev)
{
	size_t limit;
	int i;

	/*  Clear existing limit */
	dev->limit = SPDK_FTL_LIMIT_MAX;

	for (i = SPDK_FTL_LIMIT_CRIT; i < SPDK_FTL_LIMIT_MAX; ++i) {
		limit = ftl_get_limit(dev, i);

		if (dev->num_free <= limit) {
			dev->limit = i;
			break;
		}
	}
}

void
ftl_invalidate_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	struct ftl_band *band;
	struct ftl_lba_map *lba_map;

	if (ftl_addr_cached(dev, addr)) {
		ftl_bitmap_clear(dev->valid_map, addr);
		return;
	}

	band = ftl_band_from_addr(dev, addr);
	lba_map = &band->lba_map;

	/* The bit might be already cleared if two writes are scheduled to the */
	/* same LBA at the same time */
	if (ftl_bitmap_get(dev->valid_map, addr)) {
		assert(lba_map->num_vld > 0);
		ftl_bitmap_clear(dev->valid_map, addr);
		lba_map->num_vld--;
	}

	/* Invalidate open/full band lba_map entry to keep p2l and l2p
	 * consistency when band is going to close state */
	if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
		lba_map->band_map[ftl_band_block_offset_from_addr(band, addr)].lba = FTL_LBA_INVALID;
		lba_map->band_map[ftl_band_block_offset_from_addr(band, addr)].seq_id = 0;
	}
}

static int
ftl_read_canceled(int rc)
{
	return rc == -EFAULT;
}

static int
ftl_read_next_logical_addr(struct ftl_io *io, ftl_addr *addr)
{
	struct spdk_ftl_dev *dev = io->dev;
	ftl_addr next_addr;
	size_t i;
	bool addr_cached = false;

	*addr = ftl_l2p_get(dev, ftl_io_current_lba(io));
	io->map[io->pos] = *addr;

	/* If the address is invalid, skip it */
	if (*addr == FTL_ADDR_INVALID) {
		return -EFAULT;
	}

	addr_cached = ftl_addr_cached(dev, *addr);

	for (i = 1; i < ftl_io_iovec_len_left(io); ++i) {
		next_addr = ftl_l2p_get(dev, ftl_io_get_lba(io, io->pos + i));

		if (next_addr == FTL_ADDR_INVALID) {
			break;
		}

		if (addr_cached != ftl_addr_cached(dev, next_addr)) {
			break;
		}

		if (*addr + i != next_addr) {
			break;
		}

		io->map[io->pos + i] = next_addr;
	}

	return i;
}

static void
ftl_submit_read(struct ftl_io *io);

static void
_ftl_submit_read(void *_io)
{
	struct ftl_io *io = _io;

	ftl_submit_read(io);
}

static void
ftl_submit_read(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	ftl_addr addr;
	int rc = 0, num_blocks;

	assert(LIST_EMPTY(&io->children));

	while (io->pos < io->num_blocks) {
		num_blocks = rc = ftl_read_next_logical_addr(io, &addr);

		/* Address is invalid, skip this block */
		if (ftl_read_canceled(rc)) {
			memset(ftl_io_iovec_addr(io), 0, FTL_BLOCK_SIZE);
			ftl_io_advance(io, 1);
			continue;
		}

		assert(num_blocks > 0);

		if (ftl_addr_cached(dev, addr)) {
			rc = ftl_nv_cache_read(io, addr, num_blocks, ftl_io_cmpl_cb, io);
		} else {
			rc = spdk_bdev_read_blocks(dev->base_bdev_desc, dev->base_ioch,
						   ftl_io_iovec_addr(io),
						   addr, num_blocks, ftl_io_cmpl_cb, io);
		}

		if (spdk_unlikely(rc)) {
			if (rc == -ENOMEM) {
				struct spdk_bdev *bdev;
				struct spdk_io_channel *ch;

				if (ftl_addr_cached(dev, addr)) {
					bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
					ch = dev->nv_cache.cache_ioch;
				} else {
					bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
					ch = dev->base_ioch;
				}
				io->bdev_io_wait.bdev = bdev;
				io->bdev_io_wait.cb_fn = _ftl_submit_read;
				io->bdev_io_wait.cb_arg = io;
				spdk_bdev_queue_io_wait(bdev, ch, &io->bdev_io_wait);
				return;
			} else {
				ftl_abort();
			}
		}

		ftl_io_inc_req(io);
		ftl_io_advance(io, num_blocks);
	}

	/* If we didn't have to read anything from the device, */
	/* complete the request right away */
	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}
}

bool
ftl_needs_defrag(struct spdk_ftl_dev *dev)
{
	size_t limit = ftl_get_limit(dev, SPDK_FTL_LIMIT_START);

	if (dev->num_free <= limit) {
		return true;
	}

	return false;
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
	attrs->num_zones = ftl_get_num_zones(dev);
	attrs->zone_size = ftl_get_num_blocks_in_zone(dev);
	attrs->conf = dev->conf;
	attrs->base_bdev = spdk_bdev_get_name(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
	attrs->optimum_io_size = dev->xfer_size;

	attrs->cache_bdev = NULL;
	if (dev->nv_cache.bdev_desc) {
		attrs->cache_bdev = spdk_bdev_get_name(
					    spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc));
	}
}

static void
ftl_io_pin_cb(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_io *io = pin_ctx->cb_ctx;

	if (spdk_unlikely(status != 0)) {
		/* Retry on the internal L2P fault */
		io->status = -EAGAIN;
		ftl_io_complete(io);
		return;
	}

	io->flags |= FTL_IO_PINNED;
	ftl_submit_read(io);
}

static void
ftl_io_pin(struct ftl_io *io)
{
	if (spdk_unlikely(io->flags & FTL_IO_PINNED)) {
		/*
		 * The IO is in a retry path and it had been pinned already.
		 * Continue with further processing.
		 */
		ftl_l2p_pin_skip(io->dev, ftl_io_pin_cb, io, &io->l2p_pin_ctx);
	} else {
		/* First time when pinning the IO */
		ftl_l2p_pin(io->dev, io->lba, io->num_blocks,
			    ftl_io_pin_cb, io, &io->l2p_pin_ctx);
	}
}

static void
start_io(struct ftl_io *io)
{
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(io->ioch);
	struct spdk_ftl_dev *dev = io->dev;

	io->map = ftl_mempool_get(ioch->map_pool);
	if (spdk_unlikely(!io->map)) {
		io->status = -ENOMEM;
		ftl_io_complete(io);
		return;
	}

	switch (io->type) {
	case FTL_IO_READ:
		TAILQ_INSERT_TAIL(&dev->rd_sq, io, queue_entry);
		break;
	case FTL_IO_WRITE:
		TAILQ_INSERT_TAIL(&dev->wr_sq, io, queue_entry);
		break;
	case FTL_IO_UNMAP:
		TAILQ_INSERT_TAIL(&dev->unmap_sq, io, queue_entry);
		break;
	default:
		io->status = -EOPNOTSUPP;
		ftl_io_complete(io);
	}
}

static int
queue_io(struct spdk_ftl_dev *dev, struct ftl_io *io)
{
	size_t result;
	struct ftl_io_channel *ioch = ftl_io_channel_get_ctx(io->ioch);

	result = spdk_ring_enqueue(ioch->sq, (void **)&io, 1, NULL);
	if (spdk_unlikely(0 == result)) {
		return -EAGAIN;
	}

	return 0;
}

int
spdk_ftl_writev(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
		uint64_t lba, size_t lba_cnt, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
		void *cb_arg)
{
	int rc;

	if (iov_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_blocks(iov, iov_cnt)) {
		FTL_ERRLOG(dev, "Invalid IO vector to handle, device %s, LBA %"PRIu64"\n",
			   dev->name, lba);
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	rc = ftl_io_user_init(ch, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_WRITE);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

int
spdk_ftl_readv(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	       uint64_t lba,
	       size_t lba_cnt, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;

	if (iov_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_blocks(iov, iov_cnt)) {
		FTL_ERRLOG(dev, "Invalid IO vector to handle, device %s, LBA %"PRIu64"\n",
			   dev->name, lba);
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	rc = ftl_io_user_init(ch, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_READ);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

int
ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	  uint64_t lba, size_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;

	rc = ftl_io_user_init(ch, io, lba, lba_cnt, NULL, 0, cb_fn, cb_arg, FTL_IO_UNMAP);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

int
spdk_ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	       uint64_t lba, size_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;
	uint32_t aligment = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba + lba_cnt < lba_cnt) {
		return -EINVAL;
	}

	if (lba + lba_cnt > dev->num_lbas) {
		return -EINVAL;
	}

	if (lba % aligment || lba_cnt % aligment) {
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	if (io) {
		rc = ftl_unmap(dev, io, ch, lba, lba_cnt, cb_fn, cb_arg);
	} else {
		rc = ftl_mngt_unmap(dev, lba, lba_cnt, cb_fn, cb_arg);
	}

	return rc;
}

static void ftl_process_media_event(struct spdk_ftl_dev *dev, struct spdk_bdev_media_event event);

static void
_ftl_process_media_event(void *ctx)
{
	struct ftl_media_event *event = ctx;
	struct spdk_ftl_dev *dev = event->dev;

	ftl_process_media_event(dev, event->event);
	spdk_mempool_put(dev->media_events_pool, event);
}

static void
ftl_process_media_event(struct spdk_ftl_dev *dev, struct spdk_bdev_media_event event)
{
	if (!ftl_check_core_thread(dev)) {
		struct ftl_media_event *media_event;

		media_event = spdk_mempool_get(dev->media_events_pool);
		if (!media_event) {
			FTL_ERRLOG(dev, "Media event lost due to lack of memory");
			return;
		}

		media_event->dev = dev;
		media_event->event = event;
		spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_process_media_event,
				     media_event);
		return;
	}
}

void
ftl_get_media_events(struct spdk_ftl_dev *dev)
{
#define FTL_MAX_MEDIA_EVENTS 128
	struct spdk_bdev_media_event events[FTL_MAX_MEDIA_EVENTS];
	size_t num_events, i;

	if (!dev->initialized) {
		return;
	}

	do {
		num_events = spdk_bdev_get_media_events(dev->base_bdev_desc,
							events, FTL_MAX_MEDIA_EVENTS);

		for (i = 0; i < num_events; ++i) {
			ftl_process_media_event(dev, events[i]);
		}

	} while (num_events);
}

#define FTL_IO_QUEUE_BATCH 16
int
ftl_io_channel_poll(void *arg)
{
	struct ftl_io_channel *ch = arg;
	void *ios[FTL_IO_QUEUE_BATCH];
	uint64_t i, count;

	count = spdk_ring_dequeue(ch->cq, ios, FTL_IO_QUEUE_BATCH);
	if (count == 0) {
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < count; i++) {
		struct ftl_io *io = ios[i];
		io->user_fn(io->cb_ctx, io->status);
	}

	return SPDK_POLLER_BUSY;
}

static void
ftl_process_io_channel(struct spdk_ftl_dev *dev, struct ftl_io_channel *ioch)
{
	void *ios[FTL_IO_QUEUE_BATCH];
	size_t count, i;

	count = spdk_ring_dequeue(ioch->sq, ios, FTL_IO_QUEUE_BATCH);
	if (count == 0) {
		return;
	}

	for (i = 0; i < count; i++) {
		struct ftl_io *io = ios[i];
		start_io(io);
	}
}

static void ftl_process_unmap_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_io *io = md->owner.cb_ctx;

	io->dev->unmap_qd--;

	if (spdk_unlikely(status)) {
		TAILQ_INSERT_HEAD(&io->dev->unmap_sq, io, queue_entry);
		return;
	}

	ftl_io_complete(io);
}

void
ftl_set_unmap_map(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks, uint64_t seq_id)
{
	uint64_t first_page, num_pages;
	uint64_t first_md_block, num_md_blocks, num_pages_in_block;
	uint32_t lbas_in_page = FTL_BLOCK_SIZE / dev->layout.l2p.addr_size;
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_trim_md];
	uint64_t *page = ftl_md_get_buffer(md);
	union ftl_md_vss *page_vss;
	size_t i;

	first_page = lba / lbas_in_page;
	num_pages = num_blocks / lbas_in_page;

	for (i = first_page; i < first_page + num_pages; ++i) {
		ftl_bitmap_set(dev->unmap_map, i);
		page[i] = seq_id;
	}

	num_pages_in_block = FTL_BLOCK_SIZE / sizeof(*page);
	first_md_block = first_page / num_pages_in_block;
	num_md_blocks = spdk_divide_round_up(num_pages, num_pages_in_block);
	page_vss = ftl_md_get_vss_buffer(md) + first_md_block;
	for (i = first_md_block; i < num_md_blocks; ++i, page_vss++) {
		page_vss->unmap.start_lba = lba;
		page_vss->unmap.num_blocks = num_blocks;
		page_vss->unmap.seq_id = seq_id;
	}
}

static bool
ftl_process_unmap(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_trim_md];
	uint64_t seq_id;

	seq_id = ftl_nv_cache_acquire_trim_seq_id(&dev->nv_cache);
	if (seq_id == 0) {
		return false;
	}

	dev->unmap_in_progress = true;
	dev->unmap_qd++;

	dev->sb_shm->trim.start_lba = io->lba;
	dev->sb_shm->trim.num_blocks = io->num_blocks;
	dev->sb_shm->trim.seq_id = seq_id;
	dev->sb_shm->trim.in_progress = true;
	ftl_set_unmap_map(dev, io->lba, io->num_blocks, seq_id);
	ftl_debug_inject_unmap_error();
	dev->sb_shm->trim.in_progress = false;

	md->owner.cb_ctx = io;
	md->cb = ftl_process_unmap_cb;

	ftl_md_persist(md);

	return true;
}

static void
ftl_process_io_queue(struct spdk_ftl_dev *dev)
{
	struct ftl_io_channel *ioch;
	struct ftl_io *io;

	if (!TAILQ_EMPTY(&dev->rd_sq)) {
		io = TAILQ_FIRST(&dev->rd_sq);
		TAILQ_REMOVE(&dev->rd_sq, io, queue_entry);
		assert(io->type == FTL_IO_READ);
		ftl_io_pin(io);
	}

	if (!ftl_nv_cache_full(&dev->nv_cache) && !TAILQ_EMPTY(&dev->wr_sq)) {
		io = TAILQ_FIRST(&dev->wr_sq);
		TAILQ_REMOVE(&dev->wr_sq, io, queue_entry);
		assert(io->type == FTL_IO_WRITE);
		if (!ftl_nv_cache_write(io)) {
			TAILQ_INSERT_HEAD(&dev->wr_sq, io, queue_entry);
		}
	}

	if (!TAILQ_EMPTY(&dev->unmap_sq) && dev->unmap_qd == 0) {
		io = TAILQ_FIRST(&dev->unmap_sq);
		TAILQ_REMOVE(&dev->unmap_sq, io, queue_entry);
		assert(io->type == FTL_IO_UNMAP);

		if (!ftl_process_unmap(io)) {
			TAILQ_INSERT_HEAD(&dev->unmap_sq, io, queue_entry);
		}
	}

	TAILQ_FOREACH(ioch, &dev->ioch_queue, entry) {
		ftl_process_io_channel(dev, ioch);
	}
}

int
ftl_task_core(void *ctx)
{
	struct spdk_ftl_dev *dev = ctx;
	uint64_t io_activity_total_old = dev->io_activity_total;

	if (dev->halt) {
		if (ftl_shutdown_complete(dev)) {
			spdk_poller_unregister(&dev->core_poller);
			return SPDK_POLLER_IDLE;
		}
	}

	ftl_process_io_queue(dev);
	ftl_writer_run(&dev->writer_user);
	ftl_writer_run(&dev->writer_gc);
	ftl_reloc(dev->reloc);
	ftl_nv_cache_process(dev);
	ftl_l2p_process(dev);

	if ((io_activity_total_old != dev->io_activity_total)) {
		return SPDK_POLLER_BUSY;
	}

	return SPDK_POLLER_IDLE;
}

struct ftl_band *
ftl_band_get_next_free(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band = NULL;

	if (!TAILQ_EMPTY(&dev->free_bands)) {
		band = TAILQ_FIRST(&dev->free_bands);
		TAILQ_REMOVE(&dev->free_bands, band, queue_entry);
		ftl_band_erase(band);
	}

	return band;
}

void *g_ftl_zero_buf;
void *g_ftl_tmp_buf;

int spdk_ftl_init(void)
{
	g_ftl_zero_buf = spdk_zmalloc(FTL_ZERO_BUFFER_SIZE, FTL_ZERO_BUFFER_SIZE, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_ftl_zero_buf) {
		return -ENOMEM;
	}

	g_ftl_tmp_buf = spdk_zmalloc(FTL_ZERO_BUFFER_SIZE, FTL_ZERO_BUFFER_SIZE, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_ftl_tmp_buf) {
		spdk_free(g_ftl_zero_buf);
		g_ftl_zero_buf = NULL;
		return -ENOMEM;
	}
	return 0;
}

void spdk_ftl_fini(void)
{
	spdk_free(g_ftl_zero_buf);
	spdk_free(g_ftl_tmp_buf);
}

void
spdk_ftl_dev_set_fast_shdn(struct spdk_ftl_dev *dev, bool fast_shdn)
{
	assert(dev);
	dev->conf.fast_shdn = fast_shdn;
}

struct spdk_io_channel *
spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev)
{
	return spdk_get_io_channel(dev);
}

SPDK_LOG_REGISTER_COMPONENT(ftl_core)
