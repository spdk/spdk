/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
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

	ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_USER, bdev_io);

	if (spdk_unlikely(!success)) {
		io->status = -EIO;
	}

	ftl_trace_completion(dev, io, FTL_TRACE_COMPLETION_DISK);

	ftl_io_dec_req(io);
	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}

	spdk_bdev_free_io(bdev_io);
}

static void
ftl_band_erase(struct ftl_band *band)
{
	assert(band->md->state == FTL_BAND_STATE_CLOSED ||
	       band->md->state == FTL_BAND_STATE_FREE);

	ftl_band_set_state(band, FTL_BAND_STATE_PREP);
}

static size_t
ftl_get_limit(const struct spdk_ftl_dev *dev, int type)
{
	assert(type < SPDK_FTL_LIMIT_MAX);
	return dev->conf.limits[type];
}

static bool
ftl_shutdown_complete(struct spdk_ftl_dev *dev)
{
	uint64_t i;

	if (dev->num_inflight) {
		return false;
	}

	if (!ftl_nv_cache_is_halted(&dev->nv_cache)) {
		ftl_nv_cache_halt(&dev->nv_cache);
		return false;
	}

	if (!ftl_writer_is_halted(&dev->writer_user)) {
		ftl_writer_halt(&dev->writer_user);
		return false;
	}

	if (!ftl_reloc_is_halted(dev->reloc)) {
		ftl_reloc_halt(dev->reloc);
		return false;
	}

	if (!ftl_writer_is_halted(&dev->writer_gc)) {
		ftl_writer_halt(&dev->writer_gc);
		return false;
	}

	if (!ftl_nv_cache_chunks_busy(&dev->nv_cache)) {
		return false;
	}

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		if (dev->bands[i].queue_depth ||
		    dev->bands[i].md->state == FTL_BAND_STATE_CLOSING) {
			return false;
		}
	}

	if (!ftl_l2p_is_halted(dev)) {
		ftl_l2p_halt(dev);
		return false;
	}

	return true;
}

void
ftl_apply_limits(struct spdk_ftl_dev *dev)
{
	size_t limit;
	struct ftl_stats *stats = &dev->stats;
	int i;

	/*  Clear existing limit */
	dev->limit = SPDK_FTL_LIMIT_MAX;

	for (i = SPDK_FTL_LIMIT_CRIT; i < SPDK_FTL_LIMIT_MAX; ++i) {
		limit = ftl_get_limit(dev, i);

		if (dev->num_free <= limit) {
			stats->limits[i]++;
			dev->limit = i;
			break;
		}
	}

	ftl_trace_limits(dev, dev->limit, dev->num_free);
}

void
ftl_invalidate_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	struct ftl_band *band;
	struct ftl_p2l_map *p2l_map;

	if (ftl_addr_in_nvc(dev, addr)) {
		ftl_bitmap_clear(dev->valid_map, addr);
		return;
	}

	band = ftl_band_from_addr(dev, addr);
	p2l_map = &band->p2l_map;

	/* The bit might be already cleared if two writes are scheduled to the */
	/* same LBA at the same time */
	if (ftl_bitmap_get(dev->valid_map, addr)) {
		assert(p2l_map->num_valid > 0);
		ftl_bitmap_clear(dev->valid_map, addr);
		p2l_map->num_valid--;
	}

	/* Invalidate open/full band p2l_map entry to keep p2l and l2p
	 * consistency when band is going to close state */
	if (FTL_BAND_STATE_OPEN == band->md->state || FTL_BAND_STATE_FULL == band->md->state) {
		p2l_map->band_map[ftl_band_block_offset_from_addr(band, addr)].lba = FTL_LBA_INVALID;
		p2l_map->band_map[ftl_band_block_offset_from_addr(band, addr)].seq_id = 0;
	}
}

static int
ftl_read_canceled(int rc)
{
	return rc == -EFAULT;
}

static int
ftl_get_next_read_addr(struct ftl_io *io, ftl_addr *addr)
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

	addr_cached = ftl_addr_in_nvc(dev, *addr);

	for (i = 1; i < ftl_io_iovec_len_left(io); ++i) {
		next_addr = ftl_l2p_get(dev, ftl_io_get_lba(io, io->pos + i));

		if (next_addr == FTL_ADDR_INVALID) {
			break;
		}

		/* It's not enough to check for contiguity, if user data is on the last block
		 * of base device and first nvc, then they're 'contiguous', but can't be handled
		 * with one read request.
		 */
		if (addr_cached != ftl_addr_in_nvc(dev, next_addr)) {
			break;
		}

		if (*addr + i != next_addr) {
			break;
		}

		io->map[io->pos + i] = next_addr;
	}

	return i;
}

static void ftl_submit_read(struct ftl_io *io);

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

	while (io->pos < io->num_blocks) {
		num_blocks = ftl_get_next_read_addr(io, &addr);
		rc = num_blocks;

		/* User LBA doesn't hold valid data (trimmed or never written to), fill with 0 and skip this block */
		if (ftl_read_canceled(rc)) {
			memset(ftl_io_iovec_addr(io), 0, FTL_BLOCK_SIZE);
			ftl_io_advance(io, 1);
			continue;
		}

		assert(num_blocks > 0);

		ftl_trace_submission(dev, io, addr, num_blocks);

		if (ftl_addr_in_nvc(dev, addr)) {
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

				if (ftl_addr_in_nvc(dev, addr)) {
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
ftl_needs_reloc(struct spdk_ftl_dev *dev)
{
	size_t limit = ftl_get_limit(dev, SPDK_FTL_LIMIT_START);

	if (dev->num_free <= limit) {
		return true;
	}

	return false;
}

void
spdk_ftl_dev_get_attrs(const struct spdk_ftl_dev *dev, struct spdk_ftl_attrs *attrs,
		       size_t attrs_size)
{
	attrs->num_blocks = dev->num_lbas;
	attrs->block_size = FTL_BLOCK_SIZE;
	attrs->optimum_io_size = dev->xfer_size;
	/* NOTE: check any new fields in attrs against attrs_size */
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
		uint64_t lba, uint64_t lba_cnt, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn,
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
			   dev->conf.name, lba);
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	rc = ftl_io_init(ch, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_WRITE);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

int
spdk_ftl_readv(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	       uint64_t lba, uint64_t lba_cnt, struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
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
			   dev->conf.name, lba);
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	rc = ftl_io_init(ch, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_READ);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

int
ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	  uint64_t lba, uint64_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;

	rc = ftl_io_init(ch, io, lba, lba_cnt, NULL, 0, cb_fn, cb_arg, FTL_IO_UNMAP);
	if (rc) {
		return rc;
	}

	return queue_io(dev, io);
}

int
spdk_ftl_unmap(struct spdk_ftl_dev *dev, struct ftl_io *io, struct spdk_io_channel *ch,
	       uint64_t lba, uint64_t lba_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;
	uint64_t alignment = dev->layout.l2p.lbas_in_page;

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba + lba_cnt < lba_cnt) {
		return -EINVAL;
	}

	if (lba + lba_cnt > dev->num_lbas) {
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	if (lba % alignment || lba_cnt % alignment) {
		if (!io) {
			/* This is management/RPC path, its parameters must be aligned to 1MiB. */
			return -EINVAL;
		}

		/* Otherwise unaligned IO requests are NOPs */
		rc = ftl_io_init(ch, io, lba, lba_cnt, NULL, 0, cb_fn, cb_arg, FTL_IO_UNMAP);
		if (rc) {
			return rc;
		}

		io->status = 0;
		ftl_io_complete(io);
		return 0;
	}

	if (io) {
		rc = ftl_unmap(dev, io, ch, lba, lba_cnt, cb_fn, cb_arg);
	} else {
		rc = ftl_mngt_unmap(dev, lba, lba_cnt, cb_fn, cb_arg);
	}

	return rc;
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

static void
ftl_process_unmap_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_io *io = md->owner.cb_ctx;

	io->dev->unmap_qd--;

	if (spdk_unlikely(status)) {
#ifdef SPDK_FTL_RETRY_ON_ERROR
		TAILQ_INSERT_HEAD(&io->dev->unmap_sq, io, queue_entry);
		return;
#else
		io->status = status;
#endif
	}

	ftl_io_complete(io);
}

void
ftl_set_unmap_map(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t num_blocks, uint64_t seq_id)
{
	uint64_t first_page, num_pages;
	uint64_t first_md_block, num_md_blocks, num_pages_in_block;
	uint32_t lbas_in_page = dev->layout.l2p.lbas_in_page;
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
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
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];
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

	/* TODO: Try to figure out a mechanism to batch more requests at the same time,
	 * with keeping enough resources (pinned pages), between reads, writes and gc/compaction
	 */
	if (!TAILQ_EMPTY(&dev->rd_sq)) {
		io = TAILQ_FIRST(&dev->rd_sq);
		TAILQ_REMOVE(&dev->rd_sq, io, queue_entry);
		assert(io->type == FTL_IO_READ);
		ftl_io_pin(io);
		ftl_add_io_activity(dev);
	}

	while (!TAILQ_EMPTY(&dev->wr_sq) && !ftl_nv_cache_throttle(dev)) {
		io = TAILQ_FIRST(&dev->wr_sq);
		TAILQ_REMOVE(&dev->wr_sq, io, queue_entry);
		assert(io->type == FTL_IO_WRITE);
		if (!ftl_nv_cache_write(io)) {
			TAILQ_INSERT_HEAD(&dev->wr_sq, io, queue_entry);
			break;
		}
		ftl_add_io_activity(dev);
	}

	if (!TAILQ_EMPTY(&dev->unmap_sq) && dev->unmap_qd == 0) {
		io = TAILQ_FIRST(&dev->unmap_sq);
		TAILQ_REMOVE(&dev->unmap_sq, io, queue_entry);
		assert(io->type == FTL_IO_UNMAP);

		/*
		 * Unmap operation requires generating a sequence id for itself, which it gets based on the open chunk
		 * in nv cache. If there are no open chunks (because we're in the middle of state transition or compaction
		 * lagged behind), then we need to wait for the nv cache to resolve the situation - it's fine to just put the
		 * unmap and try again later.
		 */
		if (!ftl_process_unmap(io)) {
			TAILQ_INSERT_HEAD(&dev->unmap_sq, io, queue_entry);
		} else {
			ftl_add_io_activity(dev);
		}
	}

	TAILQ_FOREACH(ioch, &dev->ioch_queue, entry) {
		ftl_process_io_channel(dev, ioch);
	}
}

int
ftl_core_poller(void *ctx)
{
	struct spdk_ftl_dev *dev = ctx;
	uint64_t io_activity_total_old = dev->stats.io_activity_total;

	if (dev->halt && ftl_shutdown_complete(dev)) {
		spdk_poller_unregister(&dev->core_poller);
		return SPDK_POLLER_IDLE;
	}

	ftl_process_io_queue(dev);
	ftl_writer_run(&dev->writer_user);
	ftl_writer_run(&dev->writer_gc);
	ftl_reloc(dev->reloc);
	ftl_nv_cache_process(dev);
	ftl_l2p_process(dev);

	if (io_activity_total_old != dev->stats.io_activity_total) {
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

void *g_ftl_write_buf;
void *g_ftl_read_buf;

int
spdk_ftl_init(void)
{
	g_ftl_write_buf = spdk_zmalloc(FTL_ZERO_BUFFER_SIZE, FTL_ZERO_BUFFER_SIZE, NULL,
				       SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_ftl_write_buf) {
		return -ENOMEM;
	}

	g_ftl_read_buf = spdk_zmalloc(FTL_ZERO_BUFFER_SIZE, FTL_ZERO_BUFFER_SIZE, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!g_ftl_read_buf) {
		spdk_free(g_ftl_write_buf);
		g_ftl_write_buf = NULL;
		return -ENOMEM;
	}
	return 0;
}

void
spdk_ftl_fini(void)
{
	spdk_free(g_ftl_write_buf);
	spdk_free(g_ftl_read_buf);
}

void
spdk_ftl_dev_set_fast_shutdown(struct spdk_ftl_dev *dev, bool fast_shutdown)
{
	assert(dev);
	dev->conf.fast_shutdown = fast_shutdown;
}

void
ftl_stats_bdev_io_completed(struct spdk_ftl_dev *dev, enum ftl_stats_type type,
			    struct spdk_bdev_io *bdev_io)
{
	struct ftl_stats_entry *stats_entry = &dev->stats.entries[type];
	struct ftl_stats_group *stats_group;
	uint32_t cdw0;
	int sct;
	int sc;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		stats_group = &stats_entry->read;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		stats_group = &stats_entry->write;
		break;
	default:
		return;
	}

	spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

	if (sct == SPDK_NVME_SCT_GENERIC && sc == SPDK_NVME_SC_SUCCESS) {
		stats_group->ios++;
		stats_group->blocks += bdev_io->u.bdev.num_blocks;
	} else if (sct == SPDK_NVME_SCT_MEDIA_ERROR) {
		stats_group->errors.media++;
	} else {
		stats_group->errors.other++;
	}
}

struct spdk_io_channel *
spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev)
{
	return spdk_get_io_channel(dev);
}

void
ftl_stats_crc_error(struct spdk_ftl_dev *dev, enum ftl_stats_type type)
{

	struct ftl_stats_entry *stats_entry = &dev->stats.entries[type];
	struct ftl_stats_group *stats_group = &stats_entry->read;

	stats_group->errors.crc++;
}

struct ftl_get_stats_ctx {
	struct spdk_ftl_dev *dev;
	struct ftl_stats *stats;
	struct spdk_thread *thread;
	spdk_ftl_stats_fn cb_fn;
	void *cb_arg;
};

static void
_ftl_get_stats_cb(void *_ctx)
{
	struct ftl_get_stats_ctx *stats_ctx = _ctx;

	stats_ctx->cb_fn(stats_ctx->stats, stats_ctx->cb_arg);
	free(stats_ctx);
}

static void
_ftl_get_stats(void *_ctx)
{
	struct ftl_get_stats_ctx *stats_ctx = _ctx;

	*stats_ctx->stats = stats_ctx->dev->stats;

	if (spdk_thread_send_msg(stats_ctx->thread, _ftl_get_stats_cb, stats_ctx)) {
		ftl_abort();
	}
}

int
spdk_ftl_get_stats(struct spdk_ftl_dev *dev, struct ftl_stats *stats, spdk_ftl_stats_fn cb_fn,
		   void *cb_arg)
{
	struct ftl_get_stats_ctx *stats_ctx;
	int rc;

	stats_ctx = calloc(1, sizeof(struct ftl_get_stats_ctx));
	if (!stats_ctx) {
		return -ENOMEM;
	}

	stats_ctx->dev = dev;
	stats_ctx->stats = stats;
	stats_ctx->cb_fn = cb_fn;
	stats_ctx->cb_arg = cb_arg;
	stats_ctx->thread = spdk_get_thread();

	rc = spdk_thread_send_msg(dev->core_thread, _ftl_get_stats, stats_ctx);
	if (rc) {
		goto stats_allocated;
	}

	return 0;

stats_allocated:
	free(stats_ctx);
	return rc;
}

SPDK_LOG_REGISTER_COMPONENT(ftl_core)
