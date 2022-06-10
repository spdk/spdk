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

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/ftl.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/bdev_zone.h"
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
	/* Thread to call the callback on */
};

static const struct spdk_ftl_conf	g_default_conf = {
        /* 1 free bands  / 0 % host writes */
        .limits[SPDK_FTL_LIMIT_CRIT]  = 2,
        /* 3 free bands / 12 % host writes */
        .limits[SPDK_FTL_LIMIT_HIGH]  = 3,
        /* 7 free bands / 45 % host writes */
        .limits[SPDK_FTL_LIMIT_LOW]   = 4,
        /* 10 free bands / 75 % host writes - defrag starts running */
        .limits[SPDK_FTL_LIMIT_START] = 5,
	/* 20% spare blocks */
	.lba_rsvd = 20,
	/* IO pool size per user thread (this should be adjusted to thread IO qdepth) */
	.user_io_pool_size = 2048,
	.nv_cache = {
		/* Maximum number of blocks per request */
		.max_request_size = 16,
		.chunk_compaction_threshold = 80,
		.chunk_free_target = 5,
	},
	.fast_shdn = true,
	.base_bdev_reclaim_unit_size = (1ULL << 30) * 72
};

static int init_core_thread(struct spdk_ftl_dev *dev)
{
	uint32_t i;
	struct spdk_cpuset cpumask = {}, tmp_cpumask = {};

	/*
	 * If core mask is provided create core thread on first cpu that match with the mask,
	 * otherwise use current user thread
	 */
	if (dev->conf.core_mask) {
		if (spdk_cpuset_parse(&cpumask, dev->conf.core_mask)) {
			return -1;
		}

		SPDK_ENV_FOREACH_CORE(i) {
			if (spdk_cpuset_get_cpu(&cpumask, i)) {
				spdk_cpuset_set_cpu(&tmp_cpumask, i, true);
				dev->core_thread = spdk_thread_create("ftl_core_thread", &tmp_cpumask);
				break;
			}
		}
	} else {
		dev->core_thread = spdk_get_thread();
	}

	if (dev->core_thread == NULL) {
		FTL_ERRLOG(dev, "Cannot create thread for mask %s\n", dev->conf.core_mask);
		return -1;
	}

	return 0;
}

static void exit_thread(void *cntx)
{
	struct spdk_thread *thread = cntx;
	spdk_thread_exit(thread);
}

static void deinit_core_thread(struct spdk_ftl_dev *dev)
{
	if (dev->core_thread && dev->conf.core_mask) {
		spdk_thread_send_msg(dev->core_thread, exit_thread,
				     dev->core_thread);
		dev->core_thread = NULL;
	}
}

static void free_dev(struct spdk_ftl_dev *dev)
{
	if (!dev) {
		return;
	}

	deinit_core_thread(dev);
	ftl_conf_deinit_dev(dev);
	free(dev);
}

static struct spdk_ftl_dev *allocate_dev(
	const struct spdk_ftl_dev_init_opts *opts, int *error)
{
	int rc;
	struct spdk_ftl_dev *dev = calloc(1, sizeof(*dev));

	if (!dev) {
		FTL_ERRLOG(dev, "Cannot allocate FTL device\n");
		*error = -ENOMEM;
		return NULL;
	}

	rc = ftl_conf_init_dev(dev, opts);
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
	TAILQ_INIT(&dev->ioch_queue);

	ftl_writer_init(dev, &dev->writer_user, SPDK_FTL_LIMIT_HIGH, FTL_BAND_TYPE_COMPACTION);
	ftl_writer_init(dev, &dev->writer_gc, SPDK_FTL_LIMIT_CRIT, FTL_BAND_TYPE_GC);

	return dev;
error:
	free_dev(dev);
	return NULL;
}

static void spdk_ftl_dev_init_cb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_dev_init_ctx *ictx = ftl_mngt_get_caller_context(mngt);
	int status = ftl_mngt_get_status(mngt);
	struct spdk_ftl_dev_init_opts init_opts;
	int rc;

	if (status) {
		if (dev->init_retry) {
			init_opts.name = dev->name;
			init_opts.uuid = dev->uuid;
			init_opts.mode = dev->conf.mode;
			init_opts.base_bdev = dev->conf.base_bdev;
			init_opts.cache_bdev = dev->conf.cache_bdev;
			init_opts.conf = &dev->conf;

			FTL_NOTICELOG(dev, "Startup retry\n");
			rc = spdk_ftl_dev_init(&init_opts, ictx->cb_fn, ictx->cb_arg);
			if (!rc) {
				free_dev(dev);
				ftl_mngt_clear_dev(mngt);
				free(ictx);
				return;
			}
			FTL_NOTICELOG(dev, "Startup retry failed: %d\n", rc);
		}

		free_dev(dev);
		dev = NULL;
		ftl_mngt_clear_dev(mngt);
	}
	ictx->cb_fn(dev, ictx->cb_arg, status);
	free(ictx);
}

int
spdk_ftl_dev_init(const struct spdk_ftl_dev_init_opts *opts,
		  spdk_ftl_init_fn cb_fn, void *cb_arg)
{
	int rc = -1;
	struct ftl_dev_init_ctx *ictx;
	struct spdk_ftl_dev *dev = NULL;

	ictx = calloc(1, sizeof(*ictx));
	if (!ictx) {
		rc = -ENOMEM;
		goto error;
	}
	ictx->cb_fn = cb_fn;
	ictx->cb_arg = cb_arg;

	dev = allocate_dev(opts, &rc);
	if (!dev) {
		goto error;
	}

	rc = ftl_mngt_startup(dev, spdk_ftl_dev_init_cb, ictx);
	if (rc) {
		goto error;
	}

	return 0;

error:
	free(ictx);
	free_dev(dev);
	return rc;
}

static void spdk_ftl_dev_free_cb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_dev_init_ctx *ictx = ftl_mngt_get_caller_context(mngt);
	int status = ftl_mngt_get_status(mngt);

	if (!status) {
		free_dev(dev);
		dev = NULL;
		ftl_mngt_clear_dev(mngt);
	}
	ictx->cb_fn(dev, ictx->cb_arg, status);
	free(ictx);
}

int spdk_ftl_dev_free(struct spdk_ftl_dev *dev,
		      spdk_ftl_init_fn cb_fn, void *cb_arg)
{
	int rc = -1;
	struct ftl_dev_init_ctx *ictx;

	ictx = calloc(1, sizeof(*ictx));
	if (!ictx) {
		rc = -ENOMEM;
		goto error;
	}
	ictx->cb_fn = cb_fn;
	ictx->cb_arg = cb_arg;

	rc = ftl_mngt_shutdown(dev, spdk_ftl_dev_free_cb, ictx);
	if (rc) {
		goto error;
	}

	return 0;

error:
	free(ictx);
	return rc;
}

void
spdk_ftl_conf_init_defaults(struct spdk_ftl_conf *conf)
{
	*conf = g_default_conf;
}

SPDK_LOG_REGISTER_COMPONENT(ftl_init)
