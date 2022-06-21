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

#include "spdk/ftl.h"

#include "ftl_conf.h"
#include "ftl_core.h"

static const struct spdk_ftl_conf	g_default_conf = {
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
	.base_bdev_reclaim_unit_size = (1ULL << 30) * 72
};

static void
conf_init_defaults(struct spdk_ftl_conf *conf)
{
	*conf = g_default_conf;
}

int ftl_conf_cpy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src)
{
	char *core_mask = NULL;
	char *l2p_path = NULL;
	char *base_bdev = NULL;
	char *cache_bdev = NULL;

	if (src->core_mask) {
		core_mask = strdup(src->core_mask);
		if (!core_mask) {
			goto ERROR;
		}
	}
	if (src->l2p_path) {
		l2p_path = strdup(src->l2p_path);
		if (!l2p_path) {
			goto ERROR;
		}
	}
	if (src->base_bdev) {
		base_bdev = strdup(src->base_bdev);
		if (!base_bdev) {
			goto ERROR;
		}
	}
	if (src->cache_bdev) {
		cache_bdev = strdup(src->cache_bdev);
		if (!cache_bdev) {
			goto ERROR;
		}
	}

	free(dst->core_mask);
	free(dst->l2p_path);
	free(dst->base_bdev);
	free(dst->cache_bdev);

	*dst = *src;
	dst->core_mask = core_mask;
	dst->l2p_path = l2p_path;
	dst->base_bdev = base_bdev;
	dst->cache_bdev = cache_bdev;
	return 0;
ERROR:
	free(core_mask);
	free(l2p_path);
	free(base_bdev);
	free(cache_bdev);
	return -ENOMEM;
}

void ftl_conf_free(struct spdk_ftl_conf *conf)
{
	free(conf->core_mask);
	free(conf->l2p_path);
	free(conf->base_bdev);
	free(conf->cache_bdev);
}

int ftl_conf_init_dev(struct spdk_ftl_dev *dev,
		      const struct spdk_ftl_dev_init_opts *opts)
{
	int rc;

	if (!opts->name) {
		FTL_ERRLOG(dev, "No FTL name in configuration\n");
		return -EINVAL;
	}
	if (!opts->base_bdev) {
		FTL_ERRLOG(dev, "No base device in configuration\n");
		return -EINVAL;
	}
	if (!opts->cache_bdev) {
		FTL_ERRLOG(dev, "No NV cache device in configuration\n");
		return -EINVAL;
	}

	if (opts->conf) {
		rc = ftl_conf_cpy(&dev->conf, opts->conf);
	} else {
		conf_init_defaults(&dev->conf);
		rc = 0;
	}
	if (rc) {
		return rc;
	}

	dev->name = strdup(opts->name);
	dev->uuid = opts->uuid;
	dev->conf.mode = opts->mode;

	free(dev->conf.base_bdev);
	dev->conf.base_bdev = strdup(opts->base_bdev);

	free(dev->conf.cache_bdev);
	dev->conf.cache_bdev = strdup(opts->cache_bdev);

	if (!dev->name || !dev->conf.base_bdev || !dev->conf.cache_bdev) {
		goto error;
	}

	return 0;
error:
	ftl_conf_deinit_dev(dev);
	return -ENOMEM;
}

void ftl_conf_deinit_dev(struct spdk_ftl_dev *dev)
{
	ftl_conf_free(&dev->conf);
	free(dev->name);
}
