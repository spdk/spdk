/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/ftl.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/config.h"

#include "ftl_core.h"
#include "ftl_io.h"
#include "ftl_band.h"
#include "ftl_debug.h"
#include "ftl_nv_cache.h"
#include "ftl_writer.h"
#include "ftl_utils.h"
#include "mngt/ftl_mngt.h"

struct ftl_dev_init_ctx {
	spdk_ftl_init_fn		cb_fn;
	/* Callback's argument */
	void				*cb_arg;
};

struct ftl_dev_free_ctx {
	spdk_ftl_fn			cb_fn;
	/* Callback's argument */
	void				*cb_arg;
};

static int
init_core_thread(struct spdk_ftl_dev *dev)
{
	struct spdk_cpuset cpumask = {};

	/*
	 * If core mask is provided create core thread on first cpu that match with the mask,
	 * otherwise use current user thread
	 */
	if (dev->conf.core_mask) {
		if (spdk_cpuset_parse(&cpumask, dev->conf.core_mask)) {
			return -EINVAL;
		}
		dev->core_thread = spdk_thread_create("ftl_core_thread", &cpumask);
	} else {
		dev->core_thread = spdk_get_thread();
	}

	if (dev->core_thread == NULL) {
		FTL_ERRLOG(dev, "Cannot create thread for mask %s\n", dev->conf.core_mask);
		return -ENOMEM;
	}

	return 0;
}

static void
exit_thread(void *ctx)
{
	struct spdk_thread *thread = ctx;

	spdk_thread_exit(thread);
}

static void
deinit_core_thread(struct spdk_ftl_dev *dev)
{
	if (dev->core_thread && dev->conf.core_mask) {
		spdk_thread_send_msg(dev->core_thread, exit_thread,
				     dev->core_thread);
		dev->core_thread = NULL;
	}
}

static void
free_dev(struct spdk_ftl_dev *dev)
{
	if (!dev) {
		return;
	}

	deinit_core_thread(dev);
	spdk_ftl_conf_deinit(&dev->conf);
	free(dev);
}

static struct spdk_ftl_dev *
allocate_dev(const struct spdk_ftl_conf *conf, int *error)
{
	int rc;
	struct spdk_ftl_dev *dev = calloc(1, sizeof(*dev));

	if (!dev) {
		FTL_ERRLOG(dev, "Cannot allocate FTL device\n");
		*error = -ENOMEM;
		return NULL;
	}

	rc = ftl_conf_init_dev(dev, conf);
	if (rc) {
		*error = rc;
		goto error;
	}

	rc = init_core_thread(dev);
	if (rc) {
		*error = rc;
		goto error;
	}

	TAILQ_INIT(&dev->rd_sq);
	TAILQ_INIT(&dev->wr_sq);
	TAILQ_INIT(&dev->unmap_sq);
	TAILQ_INIT(&dev->ioch_queue);

	ftl_writer_init(dev, &dev->writer_user, SPDK_FTL_LIMIT_HIGH, FTL_BAND_TYPE_COMPACTION);
	ftl_writer_init(dev, &dev->writer_gc, SPDK_FTL_LIMIT_CRIT, FTL_BAND_TYPE_GC);

	return dev;
error:
	free_dev(dev);
	return NULL;
}

static void
dev_init_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_dev_init_ctx *ctx = _ctx;
	int rc;

	if (status) {
		if (dev->init_retry) {
			FTL_NOTICELOG(dev, "Startup retry\n");
			rc = spdk_ftl_dev_init(&dev->conf, ctx->cb_fn, ctx->cb_arg);
			if (!rc) {
				free_dev(dev);
				free(ctx);
				return;
			}
			FTL_NOTICELOG(dev, "Startup retry failed: %d\n", rc);
		}

		free_dev(dev);
		dev = NULL;
	}
	ctx->cb_fn(dev, ctx->cb_arg, status);
	free(ctx);
}

int
spdk_ftl_dev_init(const struct spdk_ftl_conf *conf, spdk_ftl_init_fn cb_fn, void *cb_arg)
{
	int rc = -1;
	struct ftl_dev_init_ctx *ctx;
	struct spdk_ftl_dev *dev = NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto error;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	dev = allocate_dev(conf, &rc);
	if (!dev) {
		goto error;
	}

	rc = ftl_mngt_call_dev_startup(dev, dev_init_cb, ctx);
	if (rc) {
		goto error;
	}

	return 0;

error:
	free(ctx);
	free_dev(dev);
	return rc;
}

static void
dev_free_cb(struct spdk_ftl_dev *dev, void *_ctx, int status)
{
	struct ftl_dev_free_ctx *ctx = _ctx;

	if (!status) {
		free_dev(dev);
	}
	ctx->cb_fn(ctx->cb_arg, status);
	free(ctx);
}

int
spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc = -1;
	struct ftl_dev_free_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto error;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = ftl_mngt_call_dev_shutdown(dev, dev_free_cb, ctx);
	if (rc) {
		goto error;
	}

	return 0;

error:
	free(ctx);
	return rc;
}

SPDK_LOG_REGISTER_COMPONENT(ftl_init)
