/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/thread.h"

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"

struct ftl_io_channel_ctx {
	struct ftl_io_channel *ioch;
};

struct ftl_io_channel *
ftl_io_channel_get_ctx(struct spdk_io_channel *ioch)
{
	struct ftl_io_channel_ctx *ctx = spdk_io_channel_get_ctx(ioch);
	return ctx->ioch;
}

static void
ftl_dev_register_channel(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;

	/* This only runs on the core thread, so it's safe to do this lockless */
	TAILQ_INSERT_TAIL(&dev->ioch_queue, ioch, entry);
}

static void
io_channel_unregister(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;

	TAILQ_REMOVE(&dev->ioch_queue, ioch, entry);

	spdk_ring_free(ioch->cq);
	spdk_ring_free(ioch->sq);
	ftl_mempool_destroy(ioch->map_pool);
	free(ioch);
}

static int
io_channel_create_cb(void *io_device, void *ctx)
{
	struct spdk_ftl_dev *dev = io_device;
	struct ftl_io_channel_ctx *_ioch = ctx;
	struct ftl_io_channel *ioch;
	char mempool_name[32];
	int rc;

	FTL_NOTICELOG(dev, "FTL IO channel created on %s\n",
		      spdk_thread_get_name(spdk_get_thread()));

	/* This gets unregistered asynchronously with the device -
	 * we can't just use the ctx buffer passed by the thread library
	 */
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

	ioch->map_pool = ftl_mempool_create(
				 dev->conf.user_io_pool_size,
				 sizeof(ftl_addr) * dev->xfer_size,
				 64,
				 SPDK_ENV_SOCKET_ID_ANY);
	if (!ioch->map_pool) {
		FTL_ERRLOG(dev, "Failed to create IO channel's  map IO pool\n");
		goto fail_io_pool;
	}

	ioch->cq = spdk_ring_create(SPDK_RING_TYPE_SP_SC, spdk_align64pow2(dev->conf.user_io_pool_size + 1),
				    SPDK_ENV_SOCKET_ID_ANY);
	if (!ioch->cq) {
		FTL_ERRLOG(dev, "Failed to create IO channel completion queue\n");
		goto fail_io_pool;
	}

	ioch->sq = spdk_ring_create(SPDK_RING_TYPE_SP_SC, spdk_align64pow2(dev->conf.user_io_pool_size + 1),
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

	if (spdk_thread_send_msg(dev->core_thread, ftl_dev_register_channel, ioch)) {
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
	free(ioch);

	return -1;
}

static void
io_channel_destroy_cb(void *io_device, void *ctx)
{
	struct ftl_io_channel_ctx *_ioch = ctx;
	struct ftl_io_channel *ioch = _ioch->ioch;
	struct spdk_ftl_dev *dev = ioch->dev;

	FTL_NOTICELOG(dev, "FTL IO channel destroy on %s\n",
		      spdk_thread_get_name(spdk_get_thread()));

	spdk_poller_unregister(&ioch->poller);
	spdk_thread_send_msg(ftl_get_core_thread(dev),
			     io_channel_unregister, ioch);
}

void
ftl_mngt_register_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->io_device_registered = true;

	spdk_io_device_register(dev, io_channel_create_cb,
				io_channel_destroy_cb,
				sizeof(struct ftl_io_channel_ctx),
				NULL);

	ftl_mngt_next_step(mngt);
}

static void
unregister_cb(void *io_device)
{
	struct spdk_ftl_dev *dev = io_device;
	struct ftl_mngt_process *mngt = dev->unregister_process;

	dev->io_device_registered = false;
	dev->unregister_process = NULL;

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_unregister_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->io_device_registered) {
		dev->unregister_process = mngt;
		spdk_io_device_unregister(dev, unregister_cb);
	} else {
		ftl_mngt_skip_step(mngt);
	}
}

void
ftl_mngt_init_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->ioch = spdk_get_io_channel(dev);
	if (!dev->ioch) {
		FTL_ERRLOG(dev, "Unable to get IO channel for core thread");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->ioch) {
		spdk_put_io_channel(dev->ioch);
		dev->ioch = NULL;
	}

	ftl_mngt_next_step(mngt);
}
