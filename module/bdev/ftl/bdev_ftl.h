/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
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

typedef void (*ftl_bdev_thread_fn)(void *);

struct rpc_ftl_stats_ctx {
	struct spdk_bdev_desc		*ftl_bdev_desc;
	ftl_bdev_thread_fn		cb;
	struct spdk_jsonrpc_request	*request;
	struct ftl_stats		*ftl_stats;
};

typedef void (*ftl_bdev_init_fn)(const struct ftl_bdev_info *, void *, int);

int bdev_ftl_create_bdev(const struct spdk_ftl_conf *conf, ftl_bdev_init_fn cb, void *cb_arg);
void bdev_ftl_delete_bdev(const char *name, bool fast_shutdown, spdk_bdev_unregister_cb cb_fn,
			  void *cb_arg);
int bdev_ftl_defer_init(const struct spdk_ftl_conf *conf);
void bdev_ftl_unmap(const char *name, uint64_t lba, uint64_t num_blocks, spdk_ftl_fn cb_fn,
		    void *cb_arg);
int bdev_ftl_get_stats(const char *name, ftl_bdev_thread_fn cb,
		       struct spdk_jsonrpc_request *request, struct ftl_stats *stats);

#endif /* SPDK_BDEV_FTL_H */
