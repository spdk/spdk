/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */
#include "spdk/ftl.h"

#include "ftl_conf.h"
#include "ftl_core.h"

static const struct spdk_ftl_conf g_default_conf = {
	/* 20% spare blocks */
	.overprovisioning = 20,
	/* IO pool size per user thread (this should be adjusted to thread IO qdepth) */
	.user_io_pool_size = 2048,
};

void
spdk_ftl_get_default_conf(struct spdk_ftl_conf *conf)
{
	*conf = g_default_conf;
}

void
spdk_ftl_dev_get_conf(const struct spdk_ftl_dev *dev, struct spdk_ftl_conf *conf)
{
	*conf = dev->conf;
}

int
spdk_ftl_conf_copy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src)
{
	char *name = NULL;
	char *core_mask = NULL;
	char *base_bdev = NULL;
	char *cache_bdev = NULL;

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

	*dst = *src;
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

	return true;
}
