/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_FTL_H
#define SPDK_BDEV_FTL_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "spdk/ftl.h"

#include "ftl_core.h"

struct ftl_bdev_info {
	const char		*name;
	struct spdk_uuid	uuid;
};

typedef void (*ftl_bdev_init_fn)(const struct ftl_bdev_info *, void *, int);
typedef void (*ftl_bdev_thread_fn)(void *);

int bdev_ftl_create_bdev(const struct spdk_ftl_conf *conf, ftl_bdev_init_fn cb, void *cb_arg);
void bdev_ftl_delete_bdev(const char *name, bool fast_shutdown, spdk_bdev_unregister_cb cb_fn,
			  void *cb_arg);
int bdev_ftl_defer_init(const struct spdk_ftl_conf *conf);

#endif /* SPDK_BDEV_FTL_H */
