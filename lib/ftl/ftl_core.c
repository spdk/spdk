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

struct spdk_io_channel *
ftl_get_io_channel(const struct spdk_ftl_dev *dev)
{
	if (ftl_check_core_thread(dev)) {
		return dev->ioch;
	}

	return NULL;
}

static int
ftl_shutdown_complete(struct spdk_ftl_dev *dev)
{
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

	if (!ftl_nv_cache_chunks_busy(&dev->nv_cache)) {
		return 0;
	}

	if (!ftl_l2p_is_halted(dev)) {
		ftl_l2p_halt(dev);
		return 0;
	}

	return 1;
}

void
ftl_invalidate_addr(struct spdk_ftl_dev *dev, ftl_addr addr)
{
	if (ftl_addr_cached(dev, addr)) {
		return;
	}
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
		io->status = -EOPNOTSUPP;
		ftl_io_complete(io);
		break;
	case FTL_IO_WRITE:
		TAILQ_INSERT_TAIL(&dev->wr_sq, io, queue_entry);
		break;
	case FTL_IO_UNMAP:
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

static void
ftl_process_io_queue(struct spdk_ftl_dev *dev)
{
	struct ftl_io_channel *ioch;
	struct ftl_io *io;

	if (!ftl_nv_cache_full(&dev->nv_cache) && !TAILQ_EMPTY(&dev->wr_sq)) {
		io = TAILQ_FIRST(&dev->wr_sq);
		TAILQ_REMOVE(&dev->wr_sq, io, queue_entry);
		assert(io->type == FTL_IO_WRITE);
		if (!ftl_nv_cache_write(io)) {
			TAILQ_INSERT_HEAD(&dev->wr_sq, io, queue_entry);
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
	ftl_nv_cache_process(dev);
	ftl_l2p_process(dev);

	if ((io_activity_total_old != dev->io_activity_total)) {
		return SPDK_POLLER_BUSY;
	}

	return SPDK_POLLER_IDLE;
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

struct spdk_io_channel *
spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev)
{
	return spdk_get_io_channel(dev);
}

SPDK_LOG_REGISTER_COMPONENT(ftl_core)
