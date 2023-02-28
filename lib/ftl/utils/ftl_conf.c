/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/ftl.h"

#include "ftl_conf.h"
#include "ftl_core.h"

static const struct spdk_ftl_conf g_default_conf = {
	/* 2 free bands - compaction is blocked, gc only */
	.limits[SPDK_FTL_LIMIT_CRIT]	= 2,
	/* 3 free bands */
	.limits[SPDK_FTL_LIMIT_HIGH]	= 3,
	/* 4 free bands */
	.limits[SPDK_FTL_LIMIT_LOW]	= 4,
	/* 5 free bands - gc starts running */
	.limits[SPDK_FTL_LIMIT_START]	= 5,
	/* 20% spare blocks */
	.overprovisioning = 20,
	/* 2GiB of DRAM for l2p cache */
	.l2p_dram_limit = 2048,
	/* IO pool size per user thread (this should be adjusted to thread IO qdepth) */
	.user_io_pool_size = 2048,
	.nv_cache = {
		.chunk_compaction_threshold = 80,
		.chunk_free_target = 5,
	},
	.fast_shutdown = true,
};

void
spdk_ftl_get_default_conf(struct spdk_ftl_conf *conf, size_t conf_size)
{
	assert(conf_size > 0);
	assert(conf_size <= sizeof(struct spdk_ftl_conf));

	memcpy(conf, &g_default_conf, conf_size);
	conf->conf_size = conf_size;
}

void
spdk_ftl_dev_get_conf(const struct spdk_ftl_dev *dev, struct spdk_ftl_conf *conf, size_t conf_size)
{
	assert(conf_size > 0);
	assert(conf_size <= sizeof(struct spdk_ftl_conf));

	memcpy(conf, &dev->conf, conf_size);
	conf->conf_size = conf_size;
}

int
spdk_ftl_conf_copy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src)
{
	char *name = NULL;
	char *core_mask = NULL;
	char *base_bdev = NULL;
	char *cache_bdev = NULL;

	if (!src->conf_size || src->conf_size > sizeof(struct spdk_ftl_conf)) {
		return -EINVAL;
	}

	if (src->name) {
		name = strdup(src->name);
		if (!name) {
			goto error;
		}
	}
	if (src->core_mask) {
		core_mask = strdup(src->core_mask);
		if (!core_mask) {
			goto error;
		}
	}
	if (src->base_bdev) {
		base_bdev = strdup(src->base_bdev);
		if (!base_bdev) {
			goto error;
		}
	}
	if (src->cache_bdev) {
		cache_bdev = strdup(src->cache_bdev);
		if (!cache_bdev) {
			goto error;
		}
	}

	memcpy(dst, src, src->conf_size);

	dst->name = name;
	dst->core_mask = core_mask;
	dst->base_bdev = base_bdev;
	dst->cache_bdev = cache_bdev;
	return 0;
error:
	free(name);
	free(core_mask);
	free(base_bdev);
	free(cache_bdev);
	return -ENOMEM;
}

void
spdk_ftl_conf_deinit(struct spdk_ftl_conf *conf)
{
	free(conf->name);
	free(conf->core_mask);
	free(conf->base_bdev);
	free(conf->cache_bdev);
}

int
ftl_conf_init_dev(struct spdk_ftl_dev *dev, const struct spdk_ftl_conf *conf)
{
	int rc;

	if (!conf->conf_size) {
		FTL_ERRLOG(dev, "FTL configuration is uninitialized\n");
		return -EINVAL;
	}

	if (!conf->name) {
		FTL_ERRLOG(dev, "No FTL name in configuration\n");
		return -EINVAL;
	}
	if (!conf->base_bdev) {
		FTL_ERRLOG(dev, "No base device in configuration\n");
		return -EINVAL;
	}
	if (!conf->cache_bdev) {
		FTL_ERRLOG(dev, "No NV cache device in configuration\n");
		return -EINVAL;
	}

	rc = spdk_ftl_conf_copy(&dev->conf, conf);
	if (rc) {
		return rc;
	}

	dev->limit = SPDK_FTL_LIMIT_MAX;
	return 0;
}

bool
ftl_conf_is_valid(const struct spdk_ftl_conf *conf)
{
	if (conf->overprovisioning >= 100) {
		return false;
	}
	if (conf->overprovisioning == 0) {
		return false;
	}

	if (conf->nv_cache.chunk_compaction_threshold == 0 ||
	    conf->nv_cache.chunk_compaction_threshold > 100) {
		return false;
	}

	if (conf->nv_cache.chunk_free_target == 0 || conf->nv_cache.chunk_free_target > 100) {
		return false;
	}

	if (conf->l2p_dram_limit == 0) {
		return false;
	}

	return true;
}
