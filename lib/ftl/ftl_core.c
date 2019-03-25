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
#include "spdk_internal/log.h"
#include "spdk/ftl.h"

#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_io.h"
#include "ftl_anm.h"
#include "ftl_rwb.h"
#include "ftl_debug.h"
#include "ftl_reloc.h"

/* Max number of iovecs */
#define FTL_MAX_IOV 1024

struct ftl_wptr {
	/* Owner device */
	struct spdk_ftl_dev		*dev;

	/* Current PPA */
	struct ftl_ppa			ppa;

	/* Band currently being written to */
	struct ftl_band			*band;

	/* Current logical block's offset */
	uint64_t			offset;

	/* Current erase block */
	struct ftl_chunk		*chunk;

	/* IO that is currently processed */
	struct ftl_io			*current_io;

	/* List link */
	LIST_ENTRY(ftl_wptr)		list_entry;
};

struct ftl_flush {
	/* Owner device */
	struct spdk_ftl_dev		*dev;

	/* Number of batches to wait for */
	size_t				num_req;

	/* Callback */
	struct ftl_cb			cb;

	/* Batch bitmap */
	struct spdk_bit_array		*bmap;

	/* List link */
	LIST_ENTRY(ftl_flush)		list_entry;
};

typedef int (*ftl_next_ppa_fn)(struct ftl_io *, struct ftl_ppa *, size_t, void *);
static void _ftl_read(void *);
static void _ftl_write(void *);

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
	LIST_REMOVE(wptr, list_entry);
	ftl_wptr_free(wptr);
}

static void
ftl_io_cmpl_cb(void *arg, const struct spdk_nvme_cpl *status)
{
	struct ftl_io *io = arg;

	if (spdk_nvme_cpl_is_error(status)) {
		ftl_io_process_error(io, status);
	}

	ftl_trace_completion(io->dev, io, FTL_TRACE_COMPLETION_DISK);

	ftl_io_dec_req(io);

	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}
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

	SPDK_ERRLOG("Metadata write failed @ppa: %s, status: %d\n",
		    ftl_ppa2str(wptr->ppa, buf, sizeof(buf)), status);

	ftl_halt_writes(io->dev, band);
}

static void
ftl_md_write_cb(void *arg, int status)
{
	struct ftl_io *io = arg;
	struct ftl_wptr *wptr;

	wptr = ftl_wptr_from_band(io->band);

	if (status) {
		ftl_md_write_fail(io, status);
		return;
	}

	ftl_band_set_next_state(io->band);
	if (io->band->state == FTL_BAND_STATE_CLOSED) {
		ftl_remove_wptr(wptr);
	}
}

static int
ftl_ppa_read_next_ppa(struct ftl_io *io, struct ftl_ppa *ppa,
		      size_t lbk, void *ctx)
{
	struct spdk_ftl_dev *dev = io->dev;
	size_t lbk_cnt, max_lbks;

	assert(ftl_io_mode_ppa(io));
	assert(io->iov_pos < io->iov_cnt);

	if (lbk == 0) {
		*ppa = io->ppa;
	} else {
		*ppa = ftl_band_next_xfer_ppa(io->band, io->ppa, lbk);
	}

	assert(!ftl_ppa_invalid(*ppa));

	/* Metadata has to be read in the way it's written (jumping across */
	/* the chunks in xfer_size increments) */
	if (io->flags & FTL_IO_MD) {
		max_lbks = dev->xfer_size - (ppa->lbk % dev->xfer_size);
		lbk_cnt = spdk_min(ftl_io_iovec_len_left(io), max_lbks);
		assert(ppa->lbk / dev->xfer_size == (ppa->lbk + lbk_cnt - 1) / dev->xfer_size);
	} else {
		lbk_cnt = ftl_io_iovec_len_left(io);
	}

	return lbk_cnt;
}

static int
ftl_wptr_close_band(struct ftl_wptr *wptr)
{
	struct ftl_band *band = wptr->band;

	ftl_band_set_state(band, FTL_BAND_STATE_CLOSING);
	band->tail_md_ppa = wptr->ppa;

	return ftl_band_write_tail_md(band, band->md.dma_buf, ftl_md_write_cb);
}

static int
ftl_wptr_open_band(struct ftl_wptr *wptr)
{
	struct ftl_band *band = wptr->band;

	assert(ftl_band_chunk_is_first(band, wptr->chunk));
	assert(band->md.num_vld == 0);

	ftl_band_clear_md(band);

	assert(band->state == FTL_BAND_STATE_PREP);
	ftl_band_set_state(band, FTL_BAND_STATE_OPENING);

	return ftl_band_write_head_md(band, band->md.dma_buf, ftl_md_write_cb);
}

static int
ftl_submit_erase(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_band *band = io->band;
	struct ftl_ppa ppa = io->ppa;
	struct ftl_chunk *chunk;
	uint64_t ppa_packed;
	int rc = 0;
	size_t i;

	for (i = 0; i < io->lbk_cnt; ++i) {
		if (i != 0) {
			chunk = ftl_band_next_chunk(band, ftl_band_chunk_from_ppa(band, ppa));
			assert(chunk->state == FTL_CHUNK_STATE_CLOSED ||
			       chunk->state == FTL_CHUNK_STATE_VACANT);
			ppa = chunk->start_ppa;
		}

		assert(ppa.lbk == 0);
		ppa_packed = ftl_ppa_addr_pack(dev, ppa);

		ftl_trace_submission(dev, io, ppa, 1);
		rc = spdk_nvme_ocssd_ns_cmd_vector_reset(dev->ns, ftl_get_write_qpair(dev),
				&ppa_packed, 1, NULL, ftl_io_cmpl_cb, io);
		if (rc) {
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

static void
_ftl_io_erase(void *ctx)
{
	ftl_io_erase((struct ftl_io *)ctx);
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

int
ftl_io_erase(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;

	if (ftl_check_core_thread(dev)) {
		return ftl_submit_erase(io);
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_io_erase, io);
	return 0;
}

static struct ftl_band *
ftl_next_write_band(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;

	band = LIST_FIRST(&dev->free_bands);
	if (!band) {
		return NULL;
	}
	assert(band->state == FTL_BAND_STATE_FREE);

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
	wptr->chunk = CIRCLEQ_FIRST(&band->chunks);
	wptr->ppa = wptr->chunk->start_ppa;

	return wptr;
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

	wptr->offset += xfer_size;
	next_thld = (ftl_band_num_usable_lbks(band) * conf->band_thld) / 100;

	if (ftl_band_full(band, wptr->offset)) {
		ftl_band_set_state(band, FTL_BAND_STATE_FULL);
	}

	wptr->chunk->busy = true;
	wptr->ppa = ftl_band_next_xfer_ppa(band, wptr->ppa, xfer_size);
	wptr->chunk = ftl_band_next_operational_chunk(band, wptr->chunk);

	assert(!ftl_ppa_invalid(wptr->ppa));

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "wptr: grp:%d, pu:%d chunk:%d, lbk:%u\n",
		      wptr->ppa.grp, wptr->ppa.pu, wptr->ppa.chk, wptr->ppa.lbk);

	if (wptr->offset >= next_thld && !dev->next_band) {
		dev->next_band = ftl_next_write_band(dev);
	}
}

static int
ftl_wptr_ready(struct ftl_wptr *wptr)
{
	struct ftl_band *band = wptr->band;

	/* TODO: add handling of empty bands */

	if (spdk_unlikely(!ftl_chunk_is_writable(wptr->chunk))) {
		/* Erasing band may fail after it was assigned to wptr. */
		if (spdk_unlikely(wptr->chunk->state == FTL_CHUNK_STATE_BAD)) {
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
		if (ftl_wptr_close_band(wptr)) {
			/* TODO: need recovery here */
			assert(false);
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

static const struct spdk_ftl_limit *
ftl_get_limit(const struct spdk_ftl_dev *dev, int type)
{
	assert(type < SPDK_FTL_LIMIT_MAX);
	return &dev->conf.defrag.limits[type];
}

static bool
ftl_cache_lba_valid(struct spdk_ftl_dev *dev, struct ftl_rwb_entry *entry)
{
	struct ftl_ppa ppa;

	/* If the LBA is invalid don't bother checking the md and l2p */
	if (spdk_unlikely(entry->lba == FTL_LBA_INVALID)) {
		return false;
	}

	ppa = ftl_l2p_get(dev, entry->lba);
	if (!(ftl_ppa_cached(ppa) && ppa.offset == entry->pos)) {
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
	/* on-disk PPA and clear the cache status bit. Otherwise, skip the l2p update */
	/* and just clear the cache status. */
	if (!ftl_cache_lba_valid(dev, entry)) {
		goto clear;
	}

	ftl_l2p_set(dev, entry->lba, entry->ppa);
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
		entry->ppa = ftl_to_ppa(FTL_PPA_INVALID);
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
ftl_process_shutdown(struct spdk_ftl_dev *dev)
{
	size_t size = ftl_rwb_num_acquired(dev->rwb, FTL_RWB_TYPE_INTERNAL) +
		      ftl_rwb_num_acquired(dev->rwb, FTL_RWB_TYPE_USER);

	if (size >= dev->xfer_size) {
		return;
	}

	/* If we reach this point we need to remove free bands */
	/* and pad current wptr band to the end */
	ftl_remove_free_bands(dev);

	/* Pad write buffer until band is full */
	ftl_rwb_pad(dev, dev->xfer_size - size);
}

static int
ftl_shutdown_complete(struct spdk_ftl_dev *dev)
{
	return !__atomic_load_n(&dev->num_inflight, __ATOMIC_SEQ_CST) &&
	       LIST_EMPTY(&dev->wptr_list);
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
ftl_invalidate_addr_unlocked(struct spdk_ftl_dev *dev, struct ftl_ppa ppa)
{
	struct ftl_band *band = ftl_band_from_ppa(dev, ppa);
	struct ftl_md *md = &band->md;
	uint64_t offset;

	offset = ftl_band_lbkoff_from_ppa(band, ppa);

	/* The bit might be already cleared if two writes are scheduled to the */
	/* same LBA at the same time */
	if (spdk_bit_array_get(md->vld_map, offset)) {
		assert(md->num_vld > 0);
		spdk_bit_array_clear(md->vld_map, offset);
		md->num_vld--;
		return 1;
	}

	return 0;
}

int
ftl_invalidate_addr(struct spdk_ftl_dev *dev, struct ftl_ppa ppa)
{
	struct ftl_band *band;
	int rc;

	assert(!ftl_ppa_cached(ppa));
	band = ftl_band_from_ppa(dev, ppa);

	pthread_spin_lock(&band->md.lock);
	rc = ftl_invalidate_addr_unlocked(dev, ppa);
	pthread_spin_unlock(&band->md.lock);

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
	return rc == 0;
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
ftl_submit_read(struct ftl_io *io, ftl_next_ppa_fn next_ppa,
		void *ctx)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_ppa ppa;
	int rc = 0, lbk_cnt;

	while (io->pos < io->lbk_cnt) {
		/* We might hit the cache here, if so, skip the read */
		lbk_cnt = rc = next_ppa(io, &ppa, io->pos, ctx);

		/* We might need to retry the read from scratch (e.g. */
		/* because write was under way and completed before */
		/* we could read it from rwb */
		if (ftl_read_retry(rc)) {
			continue;
		}

		/* We don't have to schedule the read, as it was read from cache */
		if (ftl_read_canceled(rc)) {
			ftl_io_advance(io, 1);
			continue;
		}

		assert(lbk_cnt > 0);

		ftl_trace_submission(dev, io, ppa, lbk_cnt);
		rc = spdk_nvme_ns_cmd_read(dev->ns, ftl_get_read_qpair(dev),
					   ftl_io_iovec_addr(io),
					   ftl_ppa_addr_pack(io->dev, ppa), lbk_cnt,
					   ftl_io_cmpl_cb, io, 0);
		if (rc == -ENOMEM) {
			ftl_add_to_retry_queue(io);
			break;
		} else if (rc) {
			ftl_io_fail(io, rc);
			break;
		}

		ftl_io_inc_req(io);
		ftl_io_advance(io, lbk_cnt);
	}

	/* If we didn't have to read anything from the device, */
	/* complete the request right away */
	if (ftl_io_done(io)) {
		ftl_io_complete(io);
	}

	return rc;
}

static int
ftl_ppa_cache_read(struct ftl_io *io, uint64_t lba,
		   struct ftl_ppa ppa, void *buf)
{
	struct ftl_rwb *rwb = io->dev->rwb;
	struct ftl_rwb_entry *entry;
	struct ftl_ppa nppa;
	int rc = 0;

	entry = ftl_rwb_entry_from_offset(rwb, ppa.offset);
	pthread_spin_lock(&entry->lock);

	nppa = ftl_l2p_get(io->dev, lba);
	if (ppa.ppa != nppa.ppa) {
		rc = -1;
		goto out;
	}

	memcpy(buf, entry->data, FTL_BLOCK_SIZE);
out:
	pthread_spin_unlock(&entry->lock);
	return rc;
}

static int
ftl_lba_read_next_ppa(struct ftl_io *io, struct ftl_ppa *ppa,
		      size_t lbk, void *ctx)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_ppa next_ppa;
	size_t i;

	*ppa = ftl_l2p_get(dev, io->lba + lbk);

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "Read ppa:%lx, lba:%lu\n", ppa->ppa, io->lba);

	/* If the PPA is invalid, skip it (the buffer should already be zero'ed) */
	if (ftl_ppa_invalid(*ppa)) {
		ftl_trace_completion(io->dev, io, FTL_TRACE_COMPLETION_INVALID);
		return 0;
	}

	if (ftl_ppa_cached(*ppa)) {
		if (!ftl_ppa_cache_read(io, io->lba + lbk, *ppa, ftl_io_iovec_addr(io))) {
			ftl_trace_completion(io->dev, io, FTL_TRACE_COMPLETION_CACHE);
			return 0;
		}

		/* If the state changed, we have to re-read the l2p */
		return -EAGAIN;
	}

	for (i = 1; i < ftl_io_iovec_len_left(io); ++i) {
		next_ppa = ftl_l2p_get(dev, io->lba + lbk + i);

		if (ftl_ppa_invalid(next_ppa) || ftl_ppa_cached(next_ppa)) {
			break;
		}

		if (ftl_ppa_addr_pack(dev, *ppa) + i != ftl_ppa_addr_pack(dev, next_ppa)) {
			break;
		}
	}

	return i;
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
			spdk_bit_array_set(flush->bmap, offset);
			if (!(--flush->num_req)) {
				ftl_complete_flush(flush);
			}
		}
	}
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

	band = ftl_band_from_ppa(io->dev, entry->ppa);
	SPDK_ERRLOG("Write failed @ppa: %s, status: %d\n",
		    ftl_ppa2str(entry->ppa, buf, sizeof(buf)), status);

	/* Close the band and, halt wptr and defrag */
	ftl_halt_writes(dev, band);

	ftl_rwb_foreach(entry, batch) {
		/* Invalidate meta set by process_writes() */
		ftl_invalidate_addr(dev, entry->ppa);
	}

	/* Reset the batch back to the the RWB to resend it later */
	ftl_rwb_batch_revert(batch);
}

static void
ftl_write_cb(void *arg, int status)
{
	struct ftl_io *io = arg;
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_rwb_batch *batch = io->rwb_batch;
	struct ftl_rwb_entry *entry;

	if (status) {
		ftl_write_fail(io, status);
		return;
	}

	assert(io->lbk_cnt == dev->xfer_size);
	ftl_rwb_foreach(entry, batch) {
		if (!(io->flags & FTL_IO_MD) && !(entry->flags & FTL_IO_PAD)) {
			/* Verify that the LBA is set for user lbks */
			assert(entry->lba != FTL_LBA_INVALID);
		}

		SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "Write ppa:%lu, lba:%lu\n",
			      entry->ppa.ppa, entry->lba);
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
	       struct ftl_ppa ppa)
{
	struct ftl_ppa prev_ppa;
	struct ftl_rwb_entry *prev;
	struct ftl_band *band;
	int valid;

	prev_ppa = ftl_l2p_get(dev, entry->lba);
	if (ftl_ppa_invalid(prev_ppa)) {
		ftl_l2p_set(dev, entry->lba, ppa);
		return;
	}

	/* If the L2P's PPA is different than what we expected we don't need to */
	/* do anything (someone's already overwritten our data). */
	if (ftl_rwb_entry_weak(entry) && !ftl_ppa_cmp(prev_ppa, entry->ppa)) {
		return;
	}

	if (ftl_ppa_cached(prev_ppa)) {
		assert(!ftl_rwb_entry_weak(entry));
		prev = ftl_rwb_entry_from_offset(dev->rwb, prev_ppa.offset);
		pthread_spin_lock(&prev->lock);

		/* Re-read the L2P under the lock to protect against updates */
		/* to this LBA from other threads */
		prev_ppa = ftl_l2p_get(dev, entry->lba);

		/* If the entry is no longer in cache, another write has been */
		/* scheduled in the meantime, so we have to invalidate its LBA */
		if (!ftl_ppa_cached(prev_ppa)) {
			ftl_invalidate_addr(dev, prev_ppa);
		}

		/* If previous entry is part of cache, remove and invalidate it */
		if (ftl_rwb_entry_valid(prev)) {
			ftl_invalidate_addr(dev, prev->ppa);
			ftl_rwb_entry_invalidate(prev);
		}

		ftl_l2p_set(dev, entry->lba, ppa);
		pthread_spin_unlock(&prev->lock);
		return;
	}

	/* Lock the band containing previous PPA. This assures atomic changes to */
	/* the L2P as wall as metadata. The valid bits in metadata are used to */
	/* check weak writes validity. */
	band = ftl_band_from_ppa(dev, prev_ppa);
	pthread_spin_lock(&band->md.lock);

	valid = ftl_invalidate_addr_unlocked(dev, prev_ppa);

	/* If the address has been invalidated already, we don't want to update */
	/* the L2P for weak writes, as it means the write is no longer valid. */
	if (!ftl_rwb_entry_weak(entry) || valid) {
		ftl_l2p_set(dev, entry->lba, ppa);
	}

	pthread_spin_unlock(&band->md.lock);
}

static void
ftl_io_child_write_cb(void *ctx, int status)
{
	struct ftl_chunk *chunk;
	struct ftl_io *io = ctx;

	chunk = ftl_band_chunk_from_ppa(io->band, io->ppa);
	chunk->busy = false;
}

static int
ftl_submit_child_write(struct ftl_wptr *wptr, struct ftl_io *io, int lbk_cnt)
{
	struct spdk_ftl_dev	*dev = io->dev;
	struct ftl_io		*child;
	struct iovec		*iov = ftl_io_iovec(io);
	int rc;

	/* Split IO to child requests and release chunk immediately after child is completed */
	child = ftl_io_init_child_write(io, wptr->ppa, iov[io->iov_pos].iov_base,
					ftl_io_get_md(io), ftl_io_child_write_cb);
	if (!child) {
		return -EAGAIN;
	}

	rc = spdk_nvme_ns_cmd_write_with_md(dev->ns, ftl_get_write_qpair(dev),
					    child->iov.iov_base, child->md,
					    ftl_ppa_addr_pack(dev, wptr->ppa),
					    lbk_cnt, ftl_io_cmpl_cb, child, 0, 0, 0);
	if (rc) {
		ftl_io_fail(child, rc);
		ftl_io_complete(child);
		SPDK_ERRLOG("spdk_nvme_ns_cmd_write failed with status:%d, ppa:%lu\n",
			    rc, wptr->ppa.ppa);

		return rc;
	}

	ftl_io_inc_req(child);
	ftl_io_advance(child, lbk_cnt);

	return 0;
}

static int
ftl_submit_write(struct ftl_wptr *wptr, struct ftl_io *io)
{
	struct spdk_ftl_dev	*dev = io->dev;
	struct iovec		*iov = ftl_io_iovec(io);
	int			rc = 0;
	size_t			lbk_cnt;

	while (io->iov_pos < io->iov_cnt) {
		lbk_cnt = iov[io->iov_pos].iov_len / PAGE_SIZE;
		assert(iov[io->iov_pos].iov_len > 0);
		assert(lbk_cnt == dev->xfer_size);

		/* There are no guarantees of the order of completion of NVMe IO submission queue */
		/* so wait until chunk is not busy before submitting another write */
		if (wptr->chunk->busy) {
			wptr->current_io = io;
			rc = -EAGAIN;
			break;
		}

		rc = ftl_submit_child_write(wptr, io, lbk_cnt);

		if (rc == -EAGAIN) {
			wptr->current_io = io;
			break;
		} else if (rc) {
			ftl_io_fail(io, rc);
			break;
		}

		/* Update parent iovec */
		ftl_io_advance(io, lbk_cnt);

		ftl_trace_submission(dev, io, wptr->ppa, lbk_cnt);

		ftl_wptr_advance(wptr, lbk_cnt);
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
	size_t size;

	size = ftl_rwb_num_acquired(rwb, FTL_RWB_TYPE_INTERNAL) +
	       ftl_rwb_num_acquired(rwb, FTL_RWB_TYPE_USER);

	/* There must be something in the RWB, otherwise the flush */
	/* wouldn't be waiting for anything */
	assert(size > 0);

	/* Only add padding when there's less than xfer size */
	/* entries in the buffer. Otherwise we just have to wait */
	/* for the entries to become ready. */
	if (size < dev->xfer_size) {
		ftl_rwb_pad(dev, dev->xfer_size - (size % dev->xfer_size));
	}
}

static int
ftl_wptr_process_writes(struct ftl_wptr *wptr)
{
	struct spdk_ftl_dev	*dev = wptr->dev;
	struct ftl_rwb_batch	*batch;
	struct ftl_rwb_entry	*entry;
	struct ftl_io		*io;
	struct ftl_ppa		ppa, prev_ppa;

	if (wptr->current_io) {
		if (ftl_submit_write(wptr, wptr->current_io) == -EAGAIN) {
			return 0;
		}
		wptr->current_io = NULL;
	}

	/* Make sure the band is prepared for writing */
	if (!ftl_wptr_ready(wptr)) {
		return 0;
	}

	if (dev->halt) {
		ftl_process_shutdown(dev);
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

	io = ftl_io_rwb_init(dev, wptr->band, batch, ftl_write_cb);
	if (!io) {
		goto error;
	}

	ppa = wptr->ppa;
	ftl_rwb_foreach(entry, batch) {
		entry->ppa = ppa;

		if (entry->lba != FTL_LBA_INVALID) {
			pthread_spin_lock(&entry->lock);
			prev_ppa = ftl_l2p_get(dev, entry->lba);

			/* If the l2p was updated in the meantime, don't update band's metadata */
			if (ftl_ppa_cached(prev_ppa) && prev_ppa.offset == entry->pos) {
				/* Setting entry's cache bit needs to be done after metadata */
				/* within the band is updated to make sure that writes */
				/* invalidating the entry clear the metadata as well */
				ftl_band_set_addr(wptr->band, entry->lba, entry->ppa);
				ftl_rwb_entry_set_valid(entry);
			}
			pthread_spin_unlock(&entry->lock);
		}

		ftl_trace_rwb_pop(dev, entry);
		ftl_update_rwb_stats(dev, entry);

		ppa = ftl_band_next_ppa(wptr->band, ppa, 1);
	}

	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "Write ppa:%lx, %lx\n", wptr->ppa.ppa,
		      ftl_ppa_addr_pack(dev, wptr->ppa));

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
	struct ftl_band *band;

	memcpy(entry->data, ftl_io_iovec_addr(io), FTL_BLOCK_SIZE);

	if (ftl_rwb_entry_weak(entry)) {
		band = ftl_band_from_ppa(io->dev, io->ppa);
		entry->ppa = ftl_band_next_ppa(band, io->ppa, io->pos);
	}

	entry->trace = io->trace;

	if (entry->md) {
		memcpy(entry->md, &entry->lba, sizeof(io->lba));
	}
}

static int
ftl_rwb_fill(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_rwb_entry *entry;
	struct ftl_ppa ppa = { .cached = 1 };
	int flags = ftl_rwb_flags_from_io(io);
	uint64_t lba;

	while (io->pos < io->lbk_cnt) {
		lba = ftl_io_current_lba(io);
		if (lba == FTL_LBA_INVALID) {
			ftl_io_advance(io, 1);
			continue;
		}

		entry = ftl_acquire_entry(dev, flags);
		if (!entry) {
			return -EAGAIN;
		}

		entry->lba = lba;
		ftl_rwb_entry_fill(entry, io);

		ppa.offset = entry->pos;

		ftl_io_advance(io, 1);
		ftl_update_l2p(dev, entry, ppa);

		/* Needs to be done after L2P is updated to avoid race with */
		/* write completion callback when it's processed faster than */
		/* L2P is set in update_l2p(). */
		ftl_rwb_push(entry);
		ftl_trace_rwb_fill(dev, io);
	}

	ftl_io_complete(io);
	return 0;
}

static bool
ftl_dev_needs_defrag(struct spdk_ftl_dev *dev)
{
	const struct spdk_ftl_limit *limit = ftl_get_limit(dev, SPDK_FTL_LIMIT_START);

	if (ftl_reloc_is_halted(dev->reloc)) {
		return false;
	}

	if (dev->df_band) {
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

	/* If the band doesn't have any usable lbks it's of no use */
	usable = ftl_band_num_usable_lbks(band);
	if (usable == 0) {
		return 0.0;
	}

	valid =  threshold_valid ? (usable - *threshold_valid) : band->md.num_vld;
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

	thld_vld = (ftl_band_num_usable_lbks(band) * conf->defrag.invalid_thld) / 100;

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
		band = dev->df_band = ftl_select_defrag_band(dev);

		if (band) {
			ftl_reloc_add(dev->reloc, band, 0, ftl_num_band_lbks(dev), 0);
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
	attrs->lbk_cnt = dev->num_lbas;
	attrs->lbk_size = FTL_BLOCK_SIZE;
	attrs->range = dev->range;
	attrs->cache_bdev_desc = dev->cache_bdev_desc;
}

static void
_ftl_io_write(void *ctx)
{
	ftl_io_write((struct ftl_io *)ctx);
}

int
ftl_io_write(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;

	/* For normal IOs we just need to copy the data onto the rwb */
	if (!(io->flags & FTL_IO_MD)) {
		return ftl_rwb_fill(io);
	}

	/* Metadata has its own buffer, so it doesn't have to be copied, so just */
	/* send it the the core thread and schedule the write immediately */
	if (ftl_check_core_thread(dev)) {
		return ftl_submit_write(ftl_wptr_from_band(io->band), io);
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_io_write, io);

	return 0;
}

static int
_spdk_ftl_write(struct ftl_io *io)
{
	int rc;

	rc = ftl_io_write(io);
	if (rc == -EAGAIN) {
		spdk_thread_send_msg(spdk_io_channel_get_thread(io->ioch),
				     _ftl_write, io);
		return 0;
	}

	if (rc) {
		ftl_io_free(io);
	}

	return rc;
}

static void
_ftl_write(void *ctx)
{
	_spdk_ftl_write(ctx);
}

int
spdk_ftl_write(struct spdk_ftl_dev *dev, struct spdk_io_channel *ch, uint64_t lba, size_t lba_cnt,
	       struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_io *io;

	if (iov_cnt == 0 || iov_cnt > FTL_MAX_IOV) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_lbks(iov, iov_cnt)) {
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	io = ftl_io_alloc(ch);
	if (!io) {
		return -ENOMEM;
	}

	ftl_io_user_init(dev, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_WRITE);
	return _spdk_ftl_write(io);
}

int
ftl_io_read(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	ftl_next_ppa_fn	next_ppa;

	if (ftl_check_read_thread(dev)) {
		if (ftl_io_mode_ppa(io)) {
			next_ppa = ftl_ppa_read_next_ppa;
		} else {
			next_ppa = ftl_lba_read_next_ppa;
		}

		return ftl_submit_read(io, next_ppa, NULL);
	}

	spdk_thread_send_msg(ftl_get_read_thread(dev), _ftl_read, io);
	return 0;
}

static void
_ftl_read(void *arg)
{
	ftl_io_read((struct ftl_io *)arg);
}

int
spdk_ftl_read(struct spdk_ftl_dev *dev, struct spdk_io_channel *ch, uint64_t lba, size_t lba_cnt,
	      struct iovec *iov, size_t iov_cnt, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_io *io;

	if (iov_cnt == 0 || iov_cnt > FTL_MAX_IOV) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ftl_iovec_num_lbks(iov, iov_cnt)) {
		return -EINVAL;
	}

	if (!dev->initialized) {
		return -EBUSY;
	}

	io = ftl_io_alloc(ch);
	if (!io) {
		return -ENOMEM;
	}

	ftl_io_user_init(dev, io, lba, lba_cnt, iov, iov_cnt, cb_fn, cb_arg, FTL_IO_READ);
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
spdk_ftl_flush(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg)
{
	struct ftl_flush *flush;

	if (!dev->initialized) {
		return -EBUSY;
	}

	flush = ftl_flush_init(dev, cb_fn, cb_arg);
	if (!flush) {
		return -ENOMEM;
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_flush, flush);
	return 0;
}

void
ftl_process_anm_event(struct ftl_anm_event *event)
{
	SPDK_DEBUGLOG(SPDK_LOG_FTL_CORE, "Unconsumed ANM received for dev: %p...\n", event->dev);
	ftl_anm_event_complete(event);
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
			rc = ftl_io_read(io);
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
	struct spdk_nvme_qpair *qpair = ftl_get_read_qpair(dev);
	size_t num_completed;

	if (dev->halt) {
		if (ftl_shutdown_complete(dev)) {
			spdk_poller_unregister(&thread->poller);
			return 0;
		}
	}

	num_completed = spdk_nvme_qpair_process_completions(qpair, 0);

	if (num_completed && !TAILQ_EMPTY(&dev->retry_queue)) {
		ftl_process_retry_queue(dev);
	}

	return num_completed;
}

int
ftl_task_core(void *ctx)
{
	struct ftl_thread *thread = ctx;
	struct spdk_ftl_dev *dev = thread->dev;
	struct spdk_nvme_qpair *qpair = ftl_get_write_qpair(dev);

	if (dev->halt) {
		if (ftl_shutdown_complete(dev)) {
			spdk_poller_unregister(&thread->poller);
			return 0;
		}
	}

	ftl_process_writes(dev);
	spdk_nvme_qpair_process_completions(qpair, 0);
	ftl_process_relocs(dev);

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("ftl_core", SPDK_LOG_FTL_CORE)
