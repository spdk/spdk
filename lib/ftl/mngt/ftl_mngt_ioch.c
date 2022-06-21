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

#include "spdk/thread.h"

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "utils/ftl_mempool.h"

struct ftl_io_channel_cntx {
	struct ftl_io_channel *ioch;
};

struct ftl_io_channel *
ftl_io_channel_get_ctx(struct spdk_io_channel *ioch)
{
	struct ftl_io_channel_cntx *cntx = spdk_io_channel_get_ctx(ioch);
	return cntx->ioch;
}

static void io_channel_register(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;

	TAILQ_INSERT_TAIL(&dev->ioch_queue, ioch, entry);
}

static void io_channel_unregister(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;
	uint32_t num_io_channels __attribute__((unused));

	num_io_channels = __atomic_fetch_sub(&dev->num_io_channels, 1,
					     __ATOMIC_SEQ_CST);
	assert(num_io_channels > 0);

	TAILQ_REMOVE(&dev->ioch_queue, ioch, entry);

	spdk_ring_free(ioch->cq);
	spdk_ring_free(ioch->sq);
	ftl_mempool_destroy(ioch->map_pool);
	spdk_mempool_free(ioch->io_pool);
	free(ioch);
}

static int io_channel_create_cb(void *io_device, void *ctx)
{
	struct spdk_ftl_dev *dev = io_device;
	struct ftl_io_channel_cntx *_ioch = ctx;
	struct ftl_io_channel *ioch;
	uint32_t num_io_channels;
	char mempool_name[32];
	int rc;

	FTL_NOTICELOG(dev, "FTL IO channel created on %s\n",
		      spdk_thread_get_name(spdk_get_thread()));

	num_io_channels = __atomic_fetch_add(&dev->num_io_channels, 1, __ATOMIC_SEQ_CST);
	if (num_io_channels >= dev->sb->max_io_channels) {
		FTL_ERRLOG(dev, "Reached maximum number of IO channels\n");
		__atomic_fetch_sub(&dev->num_io_channels, 1, __ATOMIC_SEQ_CST);
		return -1;
	}

	ioch = calloc(1, sizeof(*ioch));
	if (ioch == NULL) {
		FTL_ERRLOG(dev, "Failed to allocate IO channel\n");
		return -1;
	}

	rc = snprintf(mempool_name, sizeof(mempool_name), "ftl_io_%p", ioch);
	if (rc < 0 || rc >= (int)sizeof(mempool_name)) {
		FTL_ERRLOG(dev, "Failed to create IO channel pool name\n");
		free(ioch);
		return -1;
	}

	ioch->dev = dev;


	ioch->io_pool_elem_size = sizeof(struct ftl_io);

	ioch->io_pool = spdk_mempool_create(mempool_name,
					    dev->conf.user_io_pool_size,
					    ioch->io_pool_elem_size,
					    dev->conf.user_io_pool_size,
					    SPDK_ENV_SOCKET_ID_ANY);
	if (!ioch->io_pool) {
		FTL_ERRLOG(dev, "Failed to create IO channel's IO pool\n");
		free(ioch);
		return -1;
	}

	ioch->map_pool = ftl_mempool_create(
				 dev->conf.user_io_pool_size,
				 sizeof(ftl_addr) * dev->xfer_size,
				 64,
				 SPDK_ENV_SOCKET_ID_ANY);
	if (!ioch->map_pool) {
		FTL_ERRLOG(dev, "Failed to create IO channel's  map IO pool\n");
		goto fail_io_pool;
	}

	ioch->cq = spdk_ring_create(SPDK_RING_TYPE_SP_SC, 4096,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!ioch->cq) {
		FTL_ERRLOG(dev, "Failed to create IO channel completion queue\n");
		goto fail_io_pool;
	}

	ioch->sq = spdk_ring_create(SPDK_RING_TYPE_SP_SC, 4096,
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!ioch->sq) {
		FTL_ERRLOG(dev, "Failed to create IO channel submission queue\n");
		goto fail_cq;
	}

	ioch->poller = SPDK_POLLER_REGISTER(ftl_io_channel_poll, ioch, 0);
	if (!ioch->poller) {
		FTL_ERRLOG(dev, "Failed to register IO channel poller\n");
		goto fail_sq;
	}

	if (spdk_thread_send_msg(dev->core_thread, io_channel_register,
				 ioch)) {
		FTL_ERRLOG(dev, "Failed to register IO channel\n");
		goto fail_poller;
	}

	_ioch->ioch = ioch;
	return 0;

fail_poller:
	spdk_poller_unregister(&ioch->poller);
fail_cq:
	spdk_ring_free(ioch->cq);
fail_sq:
	spdk_ring_free(ioch->sq);
fail_io_pool:
	ftl_mempool_destroy(ioch->map_pool);
	spdk_mempool_free(ioch->io_pool);
	free(ioch);

	return -1;
}

static void io_channel_destroy_cb(void *io_device, void *ctx)
{
	struct ftl_io_channel_cntx *_ioch = ctx;
	struct ftl_io_channel *ioch = _ioch->ioch;
	struct spdk_ftl_dev *dev = ioch->dev;

	FTL_NOTICELOG(dev, "FTL IO channel destroy on %s\n",
		      spdk_thread_get_name(spdk_get_thread()));

	spdk_poller_unregister(&ioch->poller);
	spdk_thread_send_msg(ftl_get_core_thread(dev),
			     io_channel_unregister, ioch);
}

static void init_io_channel(struct spdk_ftl_dev *dev)
{
	/* Align the IO channels to nearest power of 2 to allow for easy addr bit shift */
	dev->sb->max_io_channels = spdk_align32pow2(dev->sb->max_io_channels);
	dev->num_io_channels = 0;

	spdk_io_device_register(dev, io_channel_create_cb,
				io_channel_destroy_cb,
				sizeof(struct ftl_io_channel_cntx),
				NULL);
}

void ftl_mngt_init_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	init_io_channel(dev);

	dev->ioch = spdk_get_io_channel(dev);
	if (!dev->ioch) {
		FTL_ERRLOG(dev, "Unable to get IO channel for core thread");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_deinit_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (dev->ioch) {
		spdk_put_io_channel(dev->ioch);
		dev->ioch = NULL;
	}

	spdk_io_device_unregister(dev, NULL);
	ftl_mngt_next_step(mngt);
}
