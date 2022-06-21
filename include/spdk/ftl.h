/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_FTL_H
#define SPDK_FTL_H

#include "spdk/stdinc.h"
#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_ftl_dev;

struct spdk_ftl_conf {
	/* Device's name */
	char					*name;

	/* Device UUID (valid when restoring device from disk) */
	struct spdk_uuid			uuid;

	/* Percentage of base device blocks not exposed to the user */
	uint64_t				overprovisioning;

	/* Core mask - core thread plus additional relocation threads */
	char					*core_mask;

	/* IO pool size per user thread */
	size_t					user_io_pool_size;

	/* FTL startup mode mask, see spdk_ftl_mode enum for possible values */
	uint32_t				mode;

	/* Name of base block device (zoned or non-zoned) */
	char					*base_bdev;

	/* Name of cache block device (must support extended metadata) */
	char					*cache_bdev;
};

enum spdk_ftl_mode {
	/* Create new device */
	SPDK_FTL_MODE_CREATE = (1 << 0),
};

typedef void (*spdk_ftl_fn)(void *cb_arg, int status);
typedef void (*spdk_ftl_init_fn)(struct spdk_ftl_dev *dev, void *cb_arg, int status);

/**
 * Initialize the FTL on the given pair of bdevs - base and cache bdev.
 * Upon receiving a successful completion callback user is free to use I/O calls.
 *
 * \param conf configuration for new device
 * \param cb callback function to call when the device is created
 * \param cb_arg callback's argument
 *
 * \return 0 if initialization was started successfully, negative errno otherwise.
 */
int spdk_ftl_dev_init(const struct spdk_ftl_conf *conf, spdk_ftl_init_fn cb, void *cb_arg);

/**
 * Deinitialize and free given device.
 *
 * \param dev device
 * \param cb callback function to call when the device is freed
 * \param cb_arg callback's argument
 *
 * \return 0 if deinitialization was started successfully, negative errno otherwise.
 */
int spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *cb_arg);

/**
 * Initialize FTL configuration structure with default values.
 *
 * \param conf FTL configuration to initialize
 */
void spdk_ftl_get_default_conf(struct spdk_ftl_conf *conf);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FTL_H */
