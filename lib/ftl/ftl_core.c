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
#include "ftl_debug.h"
#include "ftl_internal.h"
#include "mngt/ftl_mngt.h"

struct ftl_wptr {
	/* Owner device */
	struct spdk_ftl_dev		*dev;

	/* Current address */
	ftl_addr			addr;

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

static int
ftl_shutdown_complete(struct spdk_ftl_dev *dev)
{
	if (dev->num_inflight) {
		return 0;
	}

	return 1;
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

	if ((io_activity_total_old != dev->io_activity_total)) {
		return SPDK_POLLER_BUSY;
	}

	return SPDK_POLLER_IDLE;
}

struct spdk_io_channel *
spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev)
{
	return spdk_get_io_channel(dev);
}

SPDK_LOG_REGISTER_COMPONENT(ftl_core)
