/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_FTL_H
#define SPDK_BDEV_FTL_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "spdk/ftl.h"

struct spdk_bdev;
struct spdk_uuid;

struct ftl_bdev_info {
	const char		*name;
	struct spdk_uuid	uuid;
};

struct ftl_bdev_init_opts {
	/* Bdev's name */
	const char				*name;
	/* Base bdev's name */
	const char				*base_bdev;
	/* Write buffer bdev's name */
	const char				*cache_bdev;
	/* Bdev's mode */
	uint32_t				mode;
	/* UUID if device is restored from SSD */
	struct spdk_uuid			uuid;
	/* FTL library configuration */
	struct spdk_ftl_conf			ftl_conf;
};

typedef void (*ftl_bdev_init_fn)(const struct ftl_bdev_info *, void *, int);

int	bdev_ftl_create_bdev(const struct ftl_bdev_init_opts *bdev_opts,
			     ftl_bdev_init_fn cb, void *cb_arg);
void	bdev_ftl_delete_bdev(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_FTL_H */
