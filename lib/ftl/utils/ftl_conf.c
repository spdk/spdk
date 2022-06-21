/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */
#include "spdk/ftl.h"

#include "ftl_conf.h"
#include "ftl_core.h"

int
ftl_conf_cpy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src)
{
	char *core_mask = NULL;
	char *l2p_path = NULL;
	char *base_bdev = NULL;
	char *cache_bdev = NULL;

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

	free(dst->core_mask);
	free(dst->base_bdev);
	free(dst->cache_bdev);

	*dst = *src;
	dst->core_mask = core_mask;
	dst->base_bdev = base_bdev;
	dst->cache_bdev = cache_bdev;
	return 0;
error:
	free(core_mask);
	free(l2p_path);
	free(base_bdev);
	free(cache_bdev);
	return -ENOMEM;
}

void
ftl_conf_deinit(struct spdk_ftl_conf *conf)
{
	free(conf->core_mask);
	free(conf->base_bdev);
	free(conf->cache_bdev);
}
