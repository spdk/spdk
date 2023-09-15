/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
  */

#ifndef FTL_NV_CACHE_DEVICE_H
#define FTL_NV_CACHE_DEVICE_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "ftl_layout.h"

struct spdk_ftl_dev;
struct ftl_mngt_process;

/**
 * @brief NV Cache device features and capabilities
 */
struct ftl_nv_cache_device_features {
	/*
	 * The placeholder for NV Cache device features. It will be filled in the future.
	 */
};

/**
 * @brief NV Cache device operations interface
 */
struct ftl_nv_cache_device_ops {
	/**
	 * @brief Check if block device is valid for NV Cache device
	 *
	 * @param dev ftl device
	 * @param bdev bdev to be checked
	 *
	 * @retval true if bdev is valid for NV Cache device
	 * @retval false if bdev is not valid for NV Cache device
	 */
	bool (*is_bdev_compatible)(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

	struct ftl_md_layout_ops md_layout_ops;
};

/**
 * @brief NV Cache device descriptor
 */
struct ftl_nv_cache_device_desc {
	/**
	 * The name of the NV cache device type
	 */
	const char *name;

	/**
	 * The features list of the NV cache device type
	 *
	 */
	const struct ftl_nv_cache_device_features features;

	/**
	 * The NV cache device operations
	 */
	const struct ftl_nv_cache_device_ops ops;

	/** Internal fields */
	struct {
		/* The queue entry to put this description to a queue */
		TAILQ_ENTRY(ftl_nv_cache_device_desc) entry;
	} internal;
};

/**
 * @brief Macro to register NV Cache device type when the module is loaded
 *
 * @param desc NV Cache device type descriptor
 */
#define FTL_NV_CACHE_DEVICE_TYPE_REGISTER(desc) \
static void __attribute__((constructor)) ftl_nv_cache_device_register_##desc(void) \
{ \
	ftl_nv_cache_device_register(&desc); \
}

/**
 * @brief Register NV Cache device type
 *
 * @param desc NV Cache device type descriptor
 */
void ftl_nv_cache_device_register(struct ftl_nv_cache_device_desc *desc);

/**
 * @brief Get NV Cache device type descriptor by bdev
 *
 * @param bdev bdev for which NV Cache device type descriptor is requested
 *
 * @return NV Cache device type descriptor
 */
const struct ftl_nv_cache_device_desc *ftl_nv_cache_device_get_desc_by_bdev(
	struct spdk_ftl_dev *dev, struct spdk_bdev *bdev);

#endif /* FTL_NV_CACHE_DEVICE_H */
