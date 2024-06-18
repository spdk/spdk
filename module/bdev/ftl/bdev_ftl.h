/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 *   Copyright 2023 Solidigm All Rights Reserved
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

struct rpc_ftl_stats_ctx {
	struct spdk_bdev_desc		*ftl_bdev_desc;
	struct spdk_jsonrpc_request	*request;
	struct ftl_stats		ftl_stats;
};

typedef void (*ftl_bdev_init_fn)(const struct ftl_bdev_info *, void *, int);

int bdev_ftl_create_bdev(const struct spdk_ftl_conf *conf, ftl_bdev_init_fn cb, void *cb_arg);
void bdev_ftl_delete_bdev(const char *name, bool fast_shutdown, spdk_bdev_unregister_cb cb_fn,
			  void *cb_arg);
int bdev_ftl_defer_init(const struct spdk_ftl_conf *conf);
void bdev_ftl_unmap(const char *name, uint64_t lba, uint64_t num_blocks, spdk_ftl_fn cb_fn,
		    void *cb_arg);

/**
 * @brief Get FTL bdev device statistics
 *
 * @param name The name of the FTL bdev device
 * @param cb Callback function when the stats are ready
 * @param ftl_stats_ctx The context for getting the statistics
 *
 * @note In callback function will return the context of rpc_ftl_stats_ctx
 * and it contains struct ftl_stats
 */
void bdev_ftl_get_stats(const char *name, spdk_ftl_fn cb, struct rpc_ftl_stats_ctx *ftl_stats_ctx);

/**
 * @brief Get FTL bdev device properties
 *
 * @param name The name of FTL bdev device
 * @param cb_fn Callback function called when the stats are ready
 * @param request The JSON request will be filled with the FTL properties
 *
 * @note The JSON request will be returned as the context in the callback function
 */
void bdev_ftl_get_properties(const char *name, spdk_ftl_fn cb_fn,
			     struct spdk_jsonrpc_request *request);

/**
 * @brief Set FTL bdev device property
 *
 * @param name The name of FTL bdev device
 * @param property The property name to be set
 * @param value New value of the property
 * @param cb_fn Collback function invoked when the operation finished
 * @param cb_arg Collback function argument
 */
void bdev_ftl_set_property(const char *name, const char *property, const char *value,
			   spdk_ftl_fn cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_FTL_H */
