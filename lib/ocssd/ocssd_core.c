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

#include <spdk/likely.h>
#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/io_channel.h>
#include <spdk/bdev_module.h>
#include <spdk_internal/log.h>
#include <spdk/ocssd.h>
#include "ocssd_utils.h"
#include "ocssd_core.h"
#include "ocssd_band.h"
#include "ocssd_io.h"
#include "ocssd_rwb.h"
#include "ocssd_nvme.h"
#include "ocssd_debug.h"

/* Max number of iovecs */
#define OCSSD_MAX_IOV 1024

struct ocssd_wptr {
	/* Owner device */
	struct ocssd_dev			*dev;

	/* Current PPA */
	struct ocssd_ppa			ppa;

	/* Band currently being written to */
	struct ocssd_band			*band;

	/* Current logical block's offset */
	uint64_t				offset;

	/* Current erase block */
	struct ocssd_chunk			*chunk;

	/* Halt flag - once set no writes are sent to the SSD */
	int					halt;

	/* Metadata DMA buffer */
	void					*md_buf;

	/* List link */
	LIST_ENTRY(ocssd_wptr)			list_entry;
};

struct ocssd_flush {
	/* Owner device */
	struct ocssd_dev			*dev;

	/* Number of batches to wait for */
	size_t					num_req;

	/* Callback */
	struct ocssd_cb				cb;

	/* Batch bitmap */
	void					*bmap;

	/* List link */
	LIST_ENTRY(ocssd_flush)			list_entry;
};

typedef int (*ocssd_next_ppa_fn)(struct ocssd_io *, struct ocssd_ppa *, size_t, void *);
static void _ocssd_read(void *);

static int
ocssd_rwb_flags_from_io(const struct ocssd_io *io)
{
	int valid_flags = OCSSD_IO_INTERNAL | OCSSD_IO_WEAK | OCSSD_IO_PAD;
	return io->flags & valid_flags;
}

static int
ocssd_rwb_entry_weak(const struct ocssd_rwb_entry *entry)
{
	return entry->flags & OCSSD_IO_WEAK;
}

static int
ocssd_check_thread(struct ocssd_dev *dev, enum ocssd_thread_id id)
{
	pthread_t tid = pthread_self();

	assert(id < OCSSD_THREAD_ID_MAX);

	return dev->thread[id].thread->tid == tid;
}

static void
ocssd_wptr_free(struct ocssd_wptr *wptr)
{
	if (!wptr) {
		return;
	}

	spdk_dma_free(wptr->md_buf);
	free(wptr);
}

static void
ocssd_remove_wptr(struct ocssd_wptr *wptr)
{
	LIST_REMOVE(wptr, list_entry);
	ocssd_wptr_free(wptr);
}

static void
ocssd_io_cmpl_cb(void *arg, const struct spdk_nvme_cpl *status)
{
	struct ocssd_io *io = arg;

	if (spdk_nvme_cpl_is_error(status)) {
		ocssd_io_process_error(io, status);
	}

	ocssd_trace(completion, ocssd_dev_trace(io->dev), io, OCSSD_TRACE_COMPLETION_DISK);

	if (!ocssd_io_dec_req(io)) {
		ocssd_io_complete(io);
	}
}

static void
ocssd_halt_writes(struct ocssd_dev *dev, struct ocssd_band *band)
{
	struct ocssd_wptr *wptr = NULL;

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

	ocssd_band_write_failed(band);
	ocssd_remove_wptr(wptr);
}

static struct ocssd_wptr *
ocssd_wptr_from_band(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_wptr *wptr = NULL;

	LIST_FOREACH(wptr, &dev->wptr_list, list_entry) {
		if (wptr->band == band) {
			return wptr;
		}
	}

	return NULL;
}

static void
ocssd_md_write_fail(struct ocssd_io *io, int status)
{
	struct ocssd_band *band = io->band;
	struct ocssd_wptr *wptr;
	char buf[128];

	wptr = ocssd_wptr_from_band(band);

	SPDK_ERRLOG("Metadata write failed @ppa: %s, status: %d\n",
		    ocssd_ppa2str(wptr->ppa, buf, sizeof(buf)), status);

	ocssd_halt_writes(io->dev, band);
}

static void
ocssd_md_write_cb(void *arg, int status)
{
	struct ocssd_io *io = arg;
	struct ocssd_wptr *wptr;

	wptr = ocssd_wptr_from_band(io->band);

	if (status) {
		ocssd_md_write_fail(io, status);
		return;
	}

	ocssd_band_set_next_state(io->band);
	if (ocssd_band_check_state(io->band, OCSSD_BAND_STATE_CLOSED)) {
		ocssd_remove_wptr(wptr);
	}
}

static int
ocssd_ppa_read_next_ppa(struct ocssd_io *io, struct ocssd_ppa *ppa,
			size_t lbk, void *ctx)
{
	struct ocssd_dev *dev = io->dev;
	size_t lbk_cnt, max_lbks;

	assert(ocssd_io_mode_ppa(io));
	assert(io->iov_pos < io->iov_cnt);

	if (lbk == 0) {
		*ppa = io->ppa;
	} else {
		*ppa = ocssd_band_next_xfer_ppa(io->band, io->ppa, lbk);
	}

	assert(!ocssd_ppa_invalid(*ppa));

	/* Metadata has to be read in the way it's written (jumping across */
	/* the chunks in xfer_size increments) */
	if (ocssd_io_md(io)) {
		max_lbks = dev->xfer_size - (ppa->lbk % dev->xfer_size);
		lbk_cnt = spdk_min(ocssd_io_iovec_len_left(io), max_lbks);
		assert(ppa->lbk / dev->xfer_size == (ppa->lbk + lbk_cnt - 1) / dev->xfer_size);
	} else {
		lbk_cnt = ocssd_io_iovec_len_left(io);
	}

	return lbk_cnt;
}

static int
ocssd_wptr_close_band(struct ocssd_wptr *wptr)
{
	struct ocssd_band *band = wptr->band;

	ocssd_band_set_state(band, OCSSD_BAND_STATE_CLOSING);
	band->tail_md_ppa = wptr->ppa;

	return ocssd_band_write_tail_md(band, wptr->md_buf, ocssd_md_write_cb);
}

static int
ocssd_wptr_open_band(struct ocssd_wptr *wptr)
{
	struct ocssd_band *band = wptr->band;

	assert(ocssd_band_chunk_is_first(band, wptr->chunk));
	assert(band->md.num_vld == 0);

	ocssd_band_clear_md(band);

	assert(band->state == OCSSD_BAND_STATE_PREP);
	ocssd_band_set_state(band, OCSSD_BAND_STATE_OPENING);

	return ocssd_band_write_head_md(band, wptr->md_buf, ocssd_md_write_cb);
}

static struct ocssd_ppa
ocssd_erase_next_ppa(struct ocssd_io *io, struct ocssd_ppa ppa, size_t lbk)
{
	struct ocssd_band *band = io->band;
	struct ocssd_chunk *chunk;

	if (lbk == 0) {
		return io->ppa;
	}

	assert(ppa.lbk == 0);

	chunk = ocssd_band_next_chunk(band, ocssd_band_chunk_from_ppa(band, ppa));

	assert(chunk->state == OCSSD_CHUNK_STATE_CLOSED ||
	       chunk->state == OCSSD_CHUNK_STATE_VACANT);

	return chunk->start_ppa;
}

static int
ocssd_submit_erase(struct ocssd_io *io)
{
	struct ocssd_dev *dev = io->dev;
	struct ocssd_ppa ppa = ocssd_to_ppa(OCSSD_PPA_INVALID);
	uint64_t ppa_packed;
	int rc = 0;

	for (size_t i = 0; i < io->lbk_cnt; ++i) {
		ppa = ocssd_erase_next_ppa(io, ppa, i);
		ppa_packed = ocssd_ppa_addr_pack(dev, ppa);

		ocssd_trace(submission, ocssd_dev_trace(dev), io, ppa, 1);
		rc = ocssd_nvme_vector_reset(dev->ctrlr, ocssd_get_write_qpair(dev),
					     &ppa_packed, 1, NULL, ocssd_io_cmpl_cb, io);
		if (rc) {
			SPDK_ERRLOG("Vector reset failed with status: %d\n", rc);
			break;
		}

		ocssd_io_inc_req(io);
	}

	if (ocssd_io_done(io)) {
		ocssd_io_complete(io);
	}

	return rc;
}

static void
_ocssd_io_erase(void *ctx)
{
	ocssd_io_erase((struct ocssd_io *)ctx);
}

int
ocssd_io_erase(struct ocssd_io *io)
{
	struct ocssd_dev *dev = io->dev;

	if (ocssd_check_thread(dev, OCSSD_THREAD_ID_CORE)) {
		return ocssd_submit_erase(io);
	}

	ocssd_thread_send_msg(ocssd_get_core_thread(dev), _ocssd_io_erase, io);

	return 0;
}

static struct ocssd_band *
ocssd_next_write_band(struct ocssd_dev *dev)
{
	struct ocssd_band *band;

	band = LIST_FIRST(&dev->free_bands);
	if (!band) {
		return NULL;
	}
	assert(ocssd_band_check_state(band, OCSSD_BAND_STATE_FREE));

	if (ocssd_band_erase(band)) {
		/* TODO: handle erase failure */
		return NULL;
	}

	return band;
}

static struct ocssd_band *
ocssd_next_wptr_band(struct ocssd_dev *dev)
{
	struct ocssd_band *band;

	if (!dev->next_band) {
		band = ocssd_next_write_band(dev);
	} else {
		assert(ocssd_band_check_state(dev->next_band, OCSSD_BAND_STATE_PREP));
		band = dev->next_band;
		dev->next_band = NULL;
	}

	return band;
}

static struct ocssd_wptr *
ocssd_wptr_init(struct ocssd_band *band)
{
	struct ocssd_dev *dev = band->dev;
	struct ocssd_wptr *wptr;

	wptr = calloc(1, sizeof(*wptr));
	if (!wptr) {
		return NULL;
	}

	wptr->md_buf = spdk_dma_zmalloc(ocssd_tail_md_num_lbks(dev) * OCSSD_BLOCK_SIZE,
					OCSSD_BLOCK_SIZE, NULL);
	if (!wptr->md_buf) {
		ocssd_wptr_free(wptr);
		return NULL;
	}

	wptr->dev = dev;
	wptr->band = band;
	wptr->chunk = CIRCLEQ_FIRST(&band->chunks);
	wptr->ppa = wptr->chunk->start_ppa;

	return wptr;
}

static int
ocssd_add_wptr(struct ocssd_dev *dev)
{
	struct ocssd_band *band;
	struct ocssd_wptr *wptr;

	band = ocssd_next_wptr_band(dev);
	if (!band) {
		return -1;
	}

	wptr = ocssd_wptr_init(band);
	if (!wptr) {
		return -1;
	}

	if (ocssd_band_write_prep(band)) {
		ocssd_wptr_free(wptr);
		return -1;
	}

	LIST_INSERT_HEAD(&dev->wptr_list, wptr, list_entry);

	SPDK_DEBUGLOG(SPDK_LOG_OCSSD_CORE, "wptr: band %u\n", band->id);
	ocssd_trace(write_band, ocssd_dev_trace(dev), band);
	return 0;
}

static void
ocssd_wptr_advance(struct ocssd_wptr *wptr, size_t xfer_size)
{
	struct ocssd_band *band = wptr->band;
	struct ocssd_dev *dev = wptr->dev;
	struct ocssd_conf *conf = &dev->conf;
	size_t next_thld;

	wptr->offset += xfer_size;
	next_thld = (ocssd_band_num_usable_lbks(band) * conf->band_thld) / 100;

	if (ocssd_band_full(band, wptr->offset)) {
		ocssd_band_set_state(band, OCSSD_BAND_STATE_FULL);
	}

	wptr->ppa = ocssd_band_next_xfer_ppa(band, wptr->ppa, xfer_size);
	wptr->chunk = ocssd_band_next_operational_chunk(band, wptr->chunk);

	assert(!ocssd_ppa_invalid(wptr->ppa));

	SPDK_DEBUGLOG(SPDK_LOG_OCSSD_CORE, "wptr: grp:%d, pu:%d chunk:%d, lbk:%u\n",
		      wptr->ppa.grp, wptr->ppa.pu, wptr->ppa.chk, wptr->ppa.lbk);

	if (wptr->offset >= next_thld && !dev->next_band) {
		dev->next_band = ocssd_next_write_band(dev);
	}
}

static int
ocssd_wptr_ready(struct ocssd_wptr *wptr)
{
	struct ocssd_band *band = wptr->band;

	// TODO: add handling of empty bands

	if (spdk_unlikely(!ocssd_chunk_is_writable(wptr->chunk))) {
		/* Erasing band may fail after it was assigned to wptr. */
		if (spdk_unlikely(ocssd_chunk_is_bad(wptr->chunk))) {
			ocssd_wptr_advance(wptr, wptr->dev->xfer_size);
		}
		return 0;
	}

	/* If we're in the process of writing metadata, wait till it is */
	/* completed. */
	/* TODO: we should probably change bands once we're writing tail md */
	if (ocssd_band_state_changing(band)) {
		return 0;
	}

	if (ocssd_band_check_state(band, OCSSD_BAND_STATE_FULL)) {
		if (ocssd_wptr_close_band(wptr)) {
			/* TODO: need recovery here */
			assert(false);
		}
		return 0;
	}

	if (!ocssd_band_check_state(band, OCSSD_BAND_STATE_OPEN)) {
		if (ocssd_wptr_open_band(wptr)) {
			/* TODO: need recovery here */
			assert(false);
		}
		return 0;
	}

	return 1;
}

static const struct ocssd_limit *
ocssd_get_limit(const struct ocssd_dev *dev, int type)
{
	assert(type < OCSSD_LIMIT_MAX);
	return &dev->conf.defrag.limits[type];
}

static int
ocssd_update_md_entry(struct ocssd_dev *dev, struct ocssd_rwb_entry *entry)
{
	struct ocssd_ppa ppa;

	/* If the LBA is invalid don't bother checking the md and l2p */
	if (spdk_unlikely(ocssd_lba_invalid(entry->lba))) {
		return 1;
	}

	ppa = ocssd_l2p_get(dev, entry->lba);
	if (!(ocssd_ppa_cached(ppa) && ppa.offset == entry->pos)) {
		ocssd_invalidate_addr(dev, entry->ppa);
		return 1;
	}

	return 0;
}

static void
ocssd_evict_cache_entry(struct ocssd_dev *dev, struct ocssd_rwb_entry *entry)
{
	pthread_spin_lock(&entry->lock);

	if (!ocssd_rwb_entry_valid(entry)) {
		goto unlock;
	}

	/* Make sure the metadata is in sync with l2p. If the l2p still contains */
	/* the entry, fill it with the on-disk PPA and clear the cache status */
	/* bit. Otherwise, skip the l2p update and just clear the cache status. */
	/* This can happen, when a write comes during the time that l2p contains */
	/* the entry, but the entry doesn't  have a PPA assigned (and therefore */
	/* does not have the cache bit set). */
	if (ocssd_update_md_entry(dev, entry)) {
		goto clear;
	}

	ocssd_l2p_set(dev, entry->lba, entry->ppa);
clear:
	ocssd_rwb_entry_invalidate(entry);
unlock:
	pthread_spin_unlock(&entry->lock);
}

static struct ocssd_rwb_entry *
ocssd_acquire_entry(struct ocssd_dev *dev, int flags)
{
	struct ocssd_rwb_entry *entry;

	entry = ocssd_rwb_acquire(dev->rwb, ocssd_rwb_type_from_flags(flags));
	if (!entry) {
		return NULL;
	}

	ocssd_evict_cache_entry(dev, entry);

	entry->flags = flags;
	return entry;
}

static void
ocssd_rwb_pad(struct ocssd_dev *dev, size_t size)
{
	struct ocssd_rwb_entry *entry;
	int flags = OCSSD_IO_PAD | OCSSD_IO_INTERNAL;

	for (size_t i = 0; i < size; ++i) {
		entry = ocssd_acquire_entry(dev, flags);
		if (!entry) {
			break;
		}

		entry->lba = OCSSD_LBA_INVALID;
		entry->ppa = ocssd_to_ppa(OCSSD_PPA_INVALID);
		memset(entry->data, 0, OCSSD_BLOCK_SIZE);
		ocssd_rwb_push(entry);
	}
}

static void
ocssd_remove_free_bands(struct ocssd_dev *dev)
{
	while (!LIST_EMPTY(&dev->free_bands)) {
		LIST_REMOVE(LIST_FIRST(&dev->free_bands), list_entry);
	}

	dev->next_band = NULL;
}

static void
ocssd_process_shutdown(struct ocssd_dev *dev)
{
	size_t size = ocssd_rwb_num_acquired(dev->rwb, OCSSD_RWB_TYPE_INTERNAL) +
		      ocssd_rwb_num_acquired(dev->rwb, OCSSD_RWB_TYPE_USER);

	if (size >= dev->xfer_size) {
		return;
	}

	/* If we reach this point we need to remove free bands */
	/* and pad current wptr band to the end */
	ocssd_remove_free_bands(dev);

	/* Pad write buffer until band is full */
	ocssd_rwb_pad(dev, dev->xfer_size - size);
}

static int
ocssd_shutdown_complete(struct ocssd_dev *dev)
{
	return !atomic_load(&dev->num_inflight) && LIST_EMPTY(&dev->wptr_list);
}

void
ocssd_apply_limits(struct ocssd_dev *dev)
{
	const struct ocssd_limit *limit;
	struct ocssd_stats *stats = &dev->stats;
	size_t rwb_limit[OCSSD_RWB_TYPE_MAX];
	int i;

	ocssd_rwb_get_limits(dev->rwb, rwb_limit);

	/* Clear existing limit */
	dev->limit = OCSSD_LIMIT_MAX;

	for (i = OCSSD_LIMIT_CRIT; i < OCSSD_LIMIT_MAX; ++i) {
		limit = ocssd_get_limit(dev, i);

		if (dev->num_free <= limit->thld) {
			rwb_limit[OCSSD_RWB_TYPE_USER] =
				(limit->limit * ocssd_rwb_entry_cnt(dev->rwb)) / 100;
			stats->limits[i]++;
			dev->limit = i;
			goto apply;
		}
	}

	/* Clear the limits, since we don't need to apply them anymore */
	rwb_limit[OCSSD_RWB_TYPE_USER] = ocssd_rwb_entry_cnt(dev->rwb);
apply:
	ocssd_trace(limits, ocssd_dev_trace(dev), rwb_limit, dev->num_free);
	ocssd_rwb_set_limits(dev->rwb, rwb_limit);
}

static int
ocssd_invalidate_addr_unlocked(struct ocssd_dev *dev, struct ocssd_ppa ppa)
{
	struct ocssd_band *band = ocssd_band_from_ppa(dev, ppa);
	struct ocssd_md *md = &band->md;
	uint64_t offset;

	offset = ocssd_band_lbkoff_from_ppa(band, ppa);

	/* The bit might be already cleared if two writes are scheduled to the */
	/* same LBA at the same time */
	if (ocssd_get_bit(offset, md->vld_map)) {
		assert(md->num_vld > 0);
		ocssd_clr_bit(offset, md->vld_map);
		md->num_vld--;
		return 1;
	}

	return 0;
}

int
ocssd_invalidate_addr(struct ocssd_dev *dev, struct ocssd_ppa ppa)
{
	struct ocssd_band *band;
	int rc;

	assert(!ocssd_ppa_cached(ppa));
	band = ocssd_band_from_ppa(dev, ppa);

	ocssd_band_lock(band);
	rc = ocssd_invalidate_addr_unlocked(dev, ppa);
	ocssd_band_unlock(band);

	return rc;
}

static int
ocssd_read_retry(int rc)
{
	return rc == -EAGAIN;
}

static int
ocssd_read_canceled(int rc)
{
	return rc == 0;
}

static int
ocssd_submit_read(struct ocssd_io *io, ocssd_next_ppa_fn next_ppa,
		  void *ctx)
{
	struct ocssd_dev	*dev = io->dev;
	struct ocssd_ppa	ppa;
	size_t			lbk = 0;
	int			rc = 0, lbk_cnt;

	while (lbk < io->lbk_cnt) {
		/* We might hit the cache here, if so, skip the read */
		lbk_cnt = rc = next_ppa(io, &ppa, lbk, ctx);

		/* We might need to retry the read from scratch (e.g. */
		/* because write was under way and completed before */
		/* we could read it from rwb */
		if (ocssd_read_retry(rc)) {
			continue;
		}

		/* We don't have to schedule the read, as it was read from cache */
		if (ocssd_read_canceled(rc)) {
			ocssd_io_update_iovec(io, 1);
			lbk++;
			continue;
		}

		assert(lbk_cnt > 0);

		ocssd_trace(submission, ocssd_dev_trace(dev), io, ppa, lbk_cnt);
		rc = ocssd_nvme_read(dev->ctrlr, ocssd_get_read_qpair(dev),
				     ocssd_io_iovec_addr(io),
				     ocssd_ppa_addr_pack(io->dev, ppa), lbk_cnt,
				     ocssd_io_cmpl_cb, io, 0);
		if (rc) {
			SPDK_ERRLOG("spdk_nvme_ns_cmd_read failed with status: %d\n", rc);
			io->status = -EIO;
			break;
		}

		ocssd_io_update_iovec(io, lbk_cnt);
		ocssd_io_inc_req(io);
		lbk += lbk_cnt;
	}

	/* If we didn't have to read anything from the device, */
	/* complete the request right away */
	if (ocssd_io_done(io)) {
		ocssd_io_complete(io);
	}

	return rc;
}

static int
ocssd_ppa_cache_read(struct ocssd_io *io, uint64_t lba,
		     struct ocssd_ppa ppa, void *buf)
{
	struct ocssd_rwb *rwb = io->dev->rwb;
	struct ocssd_rwb_entry *entry;
	struct ocssd_ppa nppa;
	int rc = 0;

	entry = ocssd_rwb_entry_from_offset(rwb, ppa.offset);
	pthread_spin_lock(&entry->lock);

	nppa = ocssd_l2p_get(io->dev, lba);
	if (ppa.ppa != nppa.ppa) {
		rc = -1;
		goto out;
	}

	memcpy(buf, entry->data, OCSSD_BLOCK_SIZE);
out:
	pthread_spin_unlock(&entry->lock);
	return rc;
}

static int
ocssd_lba_read_next_ppa(struct ocssd_io *io, struct ocssd_ppa *ppa,
			size_t lbk, void *ctx)
{
	struct ocssd_dev *dev = io->dev;
	*ppa = ocssd_l2p_get(dev, io->lba + lbk);

	(void) ctx;

	SPDK_DEBUGLOG(SPDK_LOG_OCSSD_CORE, "Read ppa:%lx, lba:%lu\n", ppa->ppa, io->lba);

	/* If the PPA is invalid, skip it (the buffer should already be zero'ed) */
	if (ocssd_ppa_invalid(*ppa)) {
		ocssd_trace(completion, ocssd_dev_trace(io->dev), io,
			    OCSSD_TRACE_COMPLETION_INVALID);
		return 0;
	}

	if (ocssd_ppa_cached(*ppa)) {
		if (!ocssd_ppa_cache_read(io, io->lba + lbk,
					  *ppa, ocssd_io_iovec_addr(io))) {
			ocssd_trace(completion, ocssd_dev_trace(io->dev), io,
				    OCSSD_TRACE_COMPLETION_CACHE);
			return 0;
		}

		/* If the state changed, we have to re-read the l2p */
		return -EAGAIN;
	}

	/* We want to read one lbk at a time */
	return 1;
}

static void
ocssd_complete_flush(struct ocssd_flush *flush)
{
	assert(flush->num_req == 0);
	LIST_REMOVE(flush, list_entry);

	flush->cb.fn(flush->cb.ctx, 0);

	free(flush->bmap);
	free(flush);
}

static void
ocssd_process_flush(struct ocssd_dev *dev, struct ocssd_rwb_batch *batch)
{
	struct ocssd_flush *flush, *tflush;
	size_t offset;

	LIST_FOREACH_SAFE(flush, &dev->flush_list, list_entry, tflush) {
		offset = ocssd_rwb_batch_get_offset(batch);

		if (ocssd_get_bit(offset, flush->bmap)) {
			ocssd_clr_bit(offset, flush->bmap);
			if (!(--flush->num_req)) {
				ocssd_complete_flush(flush);
			}
		}
	}
}

static void
ocssd_write_fail(struct ocssd_io *io, int status)
{
	struct ocssd_rwb_batch *batch = io->rwb_batch;
	struct ocssd_dev *dev = io->dev;
	struct ocssd_rwb_entry *entry;
	struct ocssd_band *band;
	char buf[128];

	entry = ocssd_rwb_batch_first_entry(batch);

	band = ocssd_band_from_ppa(io->dev, entry->ppa);
	SPDK_ERRLOG("Write failed @ppa: %s, status: %d\n",
		    ocssd_ppa2str(entry->ppa, buf, sizeof(buf)), status);

	/* Close the band and, halt wptr and defrag */
	ocssd_halt_writes(dev, band);

	ocssd_rwb_foreach(entry, batch) {
		/* Invalidate meta set by process_writes() */
		ocssd_invalidate_addr(dev, entry->ppa);
	}

	/* Reset the batch back to the the RWB to resend it later */
	ocssd_rwb_batch_revert(batch);
}

static void
ocssd_write_cb(void *arg, int status)
{
	struct ocssd_io *io = arg;
	struct ocssd_dev *dev = io->dev;
	struct ocssd_rwb_batch *batch = io->rwb_batch;
	struct ocssd_rwb_entry *entry;

	if (status) {
		ocssd_write_fail(io, status);
		return;
	}

	assert(io->lbk_cnt == dev->xfer_size);
	ocssd_rwb_foreach(entry, batch) {
		if (!ocssd_io_md(io) && !(entry->flags & OCSSD_IO_PAD)) {
			/* Verify that the LBA is set for user lbks */
			assert(entry->lba != OCSSD_LBA_INVALID);
		}

		SPDK_DEBUGLOG(SPDK_LOG_OCSSD_CORE, "Write ppa:%lu, lba:%lu\n",
			      entry->ppa.ppa, entry->lba);

		if (ocssd_update_md_entry(dev, entry)) {
			ocssd_rwb_entry_invalidate(entry);
		}
	}

	ocssd_process_flush(dev, batch);
	ocssd_rwb_batch_release(batch);
}

static void
ocssd_update_rwb_stats(struct ocssd_dev *dev, const struct ocssd_rwb_entry *entry)
{
	if (!ocssd_rwb_entry_internal(entry)) {
		dev->stats.write_user++;
	}
	dev->stats.write_total++;
}

static void
ocssd_update_l2p(struct ocssd_dev *dev, const struct ocssd_rwb_entry *entry,
		 struct ocssd_ppa ppa)
{
	struct ocssd_ppa prev_ppa;
	struct ocssd_rwb_entry *prev;
	struct ocssd_band *band;
	int valid;

	prev_ppa = ocssd_l2p_get(dev, entry->lba);
	if (ocssd_ppa_invalid(prev_ppa)) {
		ocssd_l2p_set(dev, entry->lba, ppa);
		return;
	}

	/* If the L2P's PPA is different than what we expected we don't need to */
	/* do anything (someone's already overwritten our data). */
	if (ocssd_rwb_entry_weak(entry) && !ocssd_ppa_cmp(prev_ppa, entry->ppa)) {
		return;
	}

	if (ocssd_ppa_cached(prev_ppa)) {
		assert(!ocssd_rwb_entry_weak(entry));
		prev = ocssd_rwb_entry_from_offset(dev->rwb, prev_ppa.offset);
		pthread_spin_lock(&prev->lock);

		/* Re-read the L2P under the lock to protect against updates */
		/* to this LBA from other threads */
		prev_ppa = ocssd_l2p_get(dev, entry->lba);

		/* If the entry is no longer in cache, another write has been */
		/* scheduled in the meantime, so we have to invalidate its LBA */
		if (!ocssd_ppa_cached(prev_ppa)) {
			ocssd_invalidate_addr(dev, prev_ppa);
		}

		/* If previous entry is part of cache, remove and invalidate it */
		if (ocssd_rwb_entry_valid(prev)) {
			ocssd_invalidate_addr(dev, prev->ppa);
			ocssd_rwb_entry_invalidate(prev);
		}

		ocssd_l2p_set(dev, entry->lba, ppa);
		pthread_spin_unlock(&prev->lock);
		return;
	}

	/* Lock the band containing previous PPA. This assures atomic changes to */
	/* the L2P as wall as metadata. The valid bits in metadata are used to */
	/* check weak writes validity. */
	band = ocssd_band_from_ppa(dev, prev_ppa);
	ocssd_band_lock(band);

	valid = ocssd_invalidate_addr_unlocked(dev, prev_ppa);

	/* If the address has been invalidated already, we don't want to update */
	/* the L2P for weak writes, as it means the write is no longer valid. */
	if (!ocssd_rwb_entry_weak(entry) || valid) {
		ocssd_l2p_set(dev, entry->lba, ppa);
	}

	ocssd_band_unlock(band);
}

static int
ocssd_submit_write(struct ocssd_wptr *wptr, struct ocssd_io *io)
{
	struct ocssd_dev	*dev = io->dev;
	struct iovec		*iov = ocssd_io_iovec(io);
	int			rc = 0;
	size_t			i;

	for (i = 0; i < io->iov_cnt; ++i) {
		assert(iov[i].iov_len > 0);
		assert(iov[i].iov_len / PAGE_SIZE == dev->xfer_size);

		ocssd_trace(submission, ocssd_dev_trace(dev), io, wptr->ppa,
			    iov[i].iov_len / PAGE_SIZE);
		rc = ocssd_nvme_write_with_md(dev->ctrlr, ocssd_get_write_qpair(dev),
					      iov[i].iov_base, ocssd_io_get_md(io),
					      ocssd_ppa_addr_pack(dev, wptr->ppa),
					      iov[i].iov_len / PAGE_SIZE,
					      ocssd_io_cmpl_cb, io, 0, 0, 0);
		if (rc) {
			SPDK_ERRLOG("spdk_nvme_ns_cmd_write failed with status:%d, ppa:%lu\n",
				    rc, wptr->ppa.ppa);
			io->status = -EIO;
			break;
		}

		io->pos = iov[i].iov_len / PAGE_SIZE;
		ocssd_io_inc_req(io);
		ocssd_wptr_advance(wptr, iov[i].iov_len / PAGE_SIZE);
	}

	if (ocssd_io_done(io)) {
		ocssd_io_complete(io);
	}

	return rc;
}

static void
ocssd_flush_pad_batch(struct ocssd_dev *dev)
{
	struct ocssd_rwb *rwb = dev->rwb;
	size_t size;

	size = ocssd_rwb_num_acquired(rwb, OCSSD_RWB_TYPE_INTERNAL) +
	       ocssd_rwb_num_acquired(rwb, OCSSD_RWB_TYPE_USER);

	/* There must be something in the RWB, otherwise the flush */
	/* wouldn't be waiting for anything */
	assert(size > 0);

	/* Only add padding when there's less than xfer size */
	/* entries in the buffer. Otherwise we just have to wait */
	/* for the entries to become ready. */
	if (size < dev->xfer_size) {
		ocssd_rwb_pad(dev, dev->xfer_size - (size % dev->xfer_size));
	}
}

static int
ocssd_wptr_process_writes(struct ocssd_wptr *wptr)
{
	struct ocssd_dev	*dev = wptr->dev;
	struct ocssd_rwb_batch	*batch;
	struct ocssd_rwb_entry	*entry;
	struct ocssd_io		*io;
	struct ocssd_ppa	ppa;
	int			rc;

	/* Make sure the band is prepared for writing */
	if (!ocssd_wptr_ready(wptr)) {
		return 0;
	}

	if (!ocssd_thread_running(ocssd_get_core_thread(dev))) {
		ocssd_process_shutdown(dev);
	}

	batch = ocssd_rwb_pop(dev->rwb);
	if (!batch) {
		/* If there are queued flush requests we need to pad the RWB to */
		/* force out remaining entries */
		if (!LIST_EMPTY(&dev->flush_list)) {
			ocssd_flush_pad_batch(dev);
		}

		return 0;
	}

	io = ocssd_io_rwb_init(dev, wptr->band, batch, ocssd_write_cb);
	if (!io) {
		goto error;
	}

	ppa = wptr->ppa;
	ocssd_rwb_foreach(entry, batch) {
		entry->ppa = ppa;
		/* Setting entry's cache bit needs to be done after metadata */
		/* within the band is updated to make sure that writes */
		/* invalidating the entry clear the metadata as well */
		ocssd_band_set_addr(wptr->band, entry->lba, entry->ppa);
		ocssd_rwb_entry_set_valid(entry);
		ocssd_trace(rwb_pop, ocssd_dev_trace(dev), entry);
		ocssd_update_rwb_stats(dev, entry);
		ppa = ocssd_band_next_ppa(wptr->band, ppa, 1);
	}

	SPDK_DEBUGLOG(SPDK_LOG_OCSSD_CORE, "Write ppa:%lx, %lx\n", wptr->ppa.ppa,
		      ocssd_ppa_addr_pack(dev, wptr->ppa));

	rc = ocssd_submit_write(wptr, io);
	if (rc) {
		/* TODO: we need some recovery here */
		assert(0 && "Write submit failed");
		if (ocssd_io_done(io)) {
			spdk_ocssd_io_free(io);
		}
	}

	return dev->xfer_size;
error:
	ocssd_rwb_batch_revert(batch);
	return 0;
}

static int
ocssd_process_writes(void *arg)
{
	struct ocssd_dev *dev = arg;
	struct ocssd_wptr *wptr, *twptr;
	size_t num_active = 0;

	LIST_FOREACH_SAFE(wptr, &dev->wptr_list, list_entry, twptr) {
		ocssd_wptr_process_writes(wptr);

		if (!ocssd_band_check_state(wptr->band, OCSSD_BAND_STATE_FULL) &&
		    !ocssd_band_check_state(wptr->band, OCSSD_BAND_STATE_CLOSING) &&
		    !ocssd_band_check_state(wptr->band, OCSSD_BAND_STATE_CLOSED)) {
			num_active++;
		}
	}

	if (num_active < 1) {
		ocssd_add_wptr(dev);
	}

	return 0;
}

static void
ocssd_rwb_entry_fill(struct ocssd_rwb_entry *entry, struct ocssd_io *io)
{
	struct ocssd_band *band;

	memcpy(entry->data, ocssd_io_iovec_addr(io), OCSSD_BLOCK_SIZE);

	if (ocssd_rwb_entry_weak(entry)) {
		band = ocssd_band_from_ppa(io->dev, io->ppa);
		entry->ppa = ocssd_band_next_ppa(band, io->ppa, io->pos);
	}

	entry->trace = io->trace;

	if (entry->md) {
		memcpy(entry->md, &entry->lba, sizeof(io->lba));
	}
}

static int
ocssd_rwb_fill(struct ocssd_io *io)
{
	struct ocssd_dev *dev = io->dev;
	struct ocssd_rwb_entry *entry;
	struct ocssd_ppa ppa = { .cached = 1 };
	int flags = ocssd_rwb_flags_from_io(io);
	uint64_t lba;

	for (; io->pos < io->lbk_cnt; ++io->pos) {
		lba = ocssd_io_current_lba(io);
		if (ocssd_lba_invalid(lba)) {
			ocssd_io_update_iovec(io, 1);
			continue;
		}

		entry = ocssd_acquire_entry(dev, flags);
		if (!entry) {
			return -EAGAIN;
		}

		entry->lba = lba;
		ocssd_rwb_entry_fill(entry, io);

		ppa.offset = entry->pos;

		ocssd_io_update_iovec(io, 1);
		ocssd_update_l2p(dev, entry, ppa);

		/* Needs to be done after L2P is updated to avoid race with */
		/* write completion callback when it's processed faster than */
		/* L2P is set in update_l2p(). */
		ocssd_rwb_push(entry);
		ocssd_trace(rwb_fill, ocssd_dev_trace(dev), io);
	}

	ocssd_io_complete(io);
	return 0;
}

static int
ocssd_process_completions(void *arg)
{
	struct ocssd_io_thread *thread = arg;
	struct ocssd_dev *dev = thread->dev;

	return ocssd_nvme_process_completions(dev->ctrlr, thread->qpair, 1);
}

static int
ocssd_dev_running(void *ctx)
{
	struct ocssd_dev *dev = ctx;
	return ocssd_thread_running(ocssd_get_core_thread(dev)) ||
	       ocssd_thread_running(ocssd_get_read_thread(dev)) ||
	       !ocssd_shutdown_complete(dev);
}

int
ocssd_current_limit(const struct ocssd_dev *dev)
{
	return dev->limit;
}

int
spdk_ocssd_dev_get_attrs(const struct ocssd_dev *dev, struct ocssd_attrs *attrs)
{
	if (!dev || !attrs) {
		return -EINVAL;
	}

	attrs->uuid = dev->uuid;
	attrs->lbk_cnt = dev->l2p_len;
	attrs->lbk_size = OCSSD_BLOCK_SIZE;

	return 0;
}

static void
_ocssd_io_write(void *ctx)
{
	ocssd_io_write((struct ocssd_io *)ctx);
}

int
ocssd_io_write(struct ocssd_io *io)
{
	struct ocssd_dev *dev = io->dev;

	/* For normal IOs we just need to copy the data onto the rwb */
	if (!ocssd_io_md(io)) {
		return ocssd_rwb_fill(io);
	}

	/* Metadata has its own buffer, so it doesn't have to be copied, so just */
	/* send it the the core thread and schedule the write immediately */
	if (ocssd_check_thread(dev, OCSSD_THREAD_ID_CORE)) {
		return ocssd_submit_write(ocssd_wptr_from_band(io->band), io);
	}

	ocssd_thread_send_msg(ocssd_get_core_thread(dev), _ocssd_io_write, io);

	return 0;
}

int
spdk_ocssd_write(struct ocssd_io *io, uint64_t lba, size_t lba_cnt,
		 struct iovec *iov, size_t iov_cnt, const struct ocssd_cb *cb)
{
	if (!io || !iov || !cb) {
		return -EINVAL;
	}

	if (iov_cnt == 0 || iov_cnt > OCSSD_MAX_IOV) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ocssd_iovec_num_lbks(iov, iov_cnt)) {
		return -EINVAL;
	}

	ocssd_io_user_init(io, lba, lba_cnt, iov, iov_cnt, cb, OCSSD_IO_WRITE);
	return ocssd_io_write(io);
}

int
ocssd_io_read(struct ocssd_io *io)
{
	struct ocssd_dev	*dev = io->dev;
	ocssd_next_ppa_fn	next_ppa;

	/* TODO: Do we need this check? Maybe we should just go ahead and send */
	/* message regardless of current thread. */
	if (ocssd_check_thread(dev, OCSSD_THREAD_ID_READ)) {
		if (ocssd_io_mode_ppa(io)) {
			next_ppa = ocssd_ppa_read_next_ppa;
		} else {
			next_ppa = ocssd_lba_read_next_ppa;
		}

		return ocssd_submit_read(io, next_ppa, NULL);
	}

	ocssd_thread_send_msg(ocssd_get_read_thread(dev), _ocssd_read, io);
	return 0;
}

static void
_ocssd_read(void *arg)
{
	ocssd_io_read((struct ocssd_io *)arg);
}

int
spdk_ocssd_read(struct ocssd_io *io, uint64_t lba, size_t lba_cnt,
		struct iovec *iov, size_t iov_cnt, const struct ocssd_cb *cb)
{
	if (!io || !iov || !cb) {
		return -EINVAL;
	}

	if (iov_cnt == 0 || iov_cnt > OCSSD_MAX_IOV) {
		return -EINVAL;
	}

	if (lba_cnt == 0) {
		return -EINVAL;
	}

	if (lba_cnt != ocssd_iovec_num_lbks(iov, iov_cnt)) {
		return -EINVAL;
	}

	ocssd_io_user_init(io, lba, lba_cnt, iov, iov_cnt, cb, OCSSD_IO_READ);

	ocssd_thread_send_msg(ocssd_get_read_thread(io->dev), _ocssd_read, io);
	return 0;
}

static struct ocssd_flush *
ocssd_flush_init(struct ocssd_dev *dev, const struct ocssd_cb *cb)
{
	struct ocssd_flush *flush;
	struct ocssd_rwb *rwb = dev->rwb;

	flush = calloc(1, sizeof(*flush));
	if (!flush) {
		return NULL;
	}

	flush->bmap = calloc(ocssd_div_up(ocssd_rwb_num_batches(rwb), CHAR_BIT), 1);
	if (!flush->bmap) {
		goto error;
	}

	flush->dev = dev;
	flush->cb = *cb;

	return flush;
error:
	free(flush);
	return NULL;
}

static void
_ocssd_flush(void *ctx)
{
	struct ocssd_flush *flush = ctx;
	struct ocssd_dev *dev = flush->dev;
	struct ocssd_rwb *rwb = dev->rwb;
	struct ocssd_rwb_batch *batch;

	/* Attach flush object to all non-empty batches */
	ocssd_rwb_foreach_batch(batch, rwb) {
		if (!ocssd_rwb_batch_empty(batch)) {
			ocssd_set_bit(ocssd_rwb_batch_get_offset(batch), flush->bmap);
			flush->num_req++;
		}
	}

	LIST_INSERT_HEAD(&dev->flush_list, flush, list_entry);

	/* If the RWB was already empty, the flush can be completed right away */
	if (!flush->num_req) {
		ocssd_complete_flush(flush);
	}
}

int
spdk_ocssd_flush(struct ocssd_dev *dev, const struct ocssd_cb *cb)
{
	struct ocssd_flush *flush;

	if (!dev || !cb) {
		return -EINVAL;
	}

	flush = ocssd_flush_init(dev, cb);
	if (!flush) {
		return -ENOMEM;
	}

	ocssd_thread_send_msg(ocssd_get_core_thread(dev), _ocssd_flush, flush);
	return 0;
}

void
ocssd_read_thread(void *ctx)
{
	struct ocssd_dev *dev = ctx;
	struct ocssd_io_thread *io_thread = &dev->thread[OCSSD_THREAD_ID_READ];
	struct spdk_poller *poller;

	poller = spdk_poller_register(ocssd_process_completions, io_thread, 0);
	if (!poller) {
		return;
	}

	ocssd_thread_set_initialized(io_thread->thread);

	while (ocssd_dev_running(dev)) {
		ocssd_thread_process(io_thread->thread);
	}

	spdk_poller_unregister(&poller);
	spdk_free_thread();
}

void
ocssd_core_thread(void *ctx)
{
	struct ocssd_dev *dev = ctx;
	struct ocssd_io_thread *io_thread = &dev->thread[OCSSD_THREAD_ID_CORE];
	struct {
		spdk_poller_fn fn;
		void *ctx;
	} pollers[] = {
		{ ocssd_process_writes, dev },
		{ ocssd_process_completions, io_thread }
	};
	struct spdk_poller *spdk_pollers[ocssd_array_size(pollers)];

	for (size_t i = 0; i < ocssd_array_size(pollers); ++i) {
		spdk_pollers[i] = spdk_poller_register(pollers[i].fn, pollers[i].ctx, 0);
	}

	ocssd_thread_set_initialized(io_thread->thread);

	while (ocssd_dev_running(dev)) {
		ocssd_thread_process(io_thread->thread);
	}

	for (size_t i = 0; i < ocssd_array_size(pollers); ++i) {
		spdk_poller_unregister(&spdk_pollers[i]);
	}

	spdk_free_thread();
}

SPDK_LOG_REGISTER_COMPONENT("ocssd_core", SPDK_LOG_OCSSD_CORE)
